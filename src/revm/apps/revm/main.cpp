// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

/*
 * revm — unified REVM CLI (see newusage.txt / GUIDE.md)
 */

#include "core/Board.hpp"
#include "core/Config.hpp"
#include "core/RunSetup.hpp"
#include "goldens/CompareReport.hpp"
#include "goldens/PlayLog.hpp"
#include "goldens/PlayPlayer.hpp"
#include "goldens/PlayRecorder.hpp"
#include "input/GoldenInput.hpp"
#include "input/JoystickConfig.hpp"
#include "input/LiveInput.hpp"
#include "util/Hash.hpp"
#define REVM_LOG_MODULE "revm"
#include "util/Log.hpp"

#if defined(REVM_HAS_CPUMOCK)
#include "cpumock/CpuMockHost.hpp"
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<revm::Board *> g_board_for_signal{nullptr};

void on_signal_quit(int) {
	if (revm::Board * b = g_board_for_signal.load()) {
		b->RequestQuit(0);
	}
}

void install_quit_signals(revm::Board & board) {
	g_board_for_signal.store(&board);
	std::signal(SIGINT, on_signal_quit);
	std::signal(SIGTERM, on_signal_quit);
}

void clear_quit_signals() {
	g_board_for_signal.store(nullptr);
}

void print_usage(const char * argv0) {
	std::fprintf(stderr,
		"Usage:\n"
		"  %s [options]\n"
		"\n"
		"Machine image (mutually exclusive):\n"
		"  --load-prg FILE       Load PRG (DMALoad + RUN); BEGIN = boot / 0\n"
		"  --load-snapshot FILE  Restore FullSnapshot (BEGIN from snap)\n"
		"  --no-load             Boot to BASIC (no PRG / snapshot)\n"
		"\n"
		"Play role (mutually exclusive):\n"
		"  --record-play FILE    Record play JSON (inputs, rand_seed, snapshot hashes)\n"
		"  --use-play FILE       Replay play (applies play rand_seed; verify hashes)\n"
		"\n"
		"Snapshots (Stage 3 captures Twin):\n"
		"  --save-snapshot CYCLE FILE  Write FullSnapshot at CYCLE\n"
		"  --add-play-snapshot CYCLE   Requires --use-play; write bin + register in play\n"
		"\n"
		"Media (Main board):\n"
		"  --save-audio FILE     WAV from SID (headless requires --resid)\n"
		"  --save-video FILE     MP4 via ffmpeg: 2× nearest-neighbor RGB,\n"
		"                        H.264 + reSID AAC. Requires --resid.\n"
		"  --save-screen FRAME FILE  Pepto P6 PPM of Main at VBLANK FRAME\n"
		"                        (same frame counter as --max-frames)\n"
		"  --frame-observations FILE  Per-frame indexed pixels + public SID bytes\n"
		"\n"
		"Common:\n"
		"  --version             Print the C64 Rework framework version\n"
		"  --config FILE         Input config (default: ./revm.cfg, then beside binary;\n"
		"                        creates ./revm.cfg from built-in defaults if absent)\n"
		"  --rom-dir DIR         Directory of C64 ROM dumps (required)\n"
		"                        basic*.bin, kernal*.bin, chargen*.bin;\n"
		"                        optional dos1541ii*.bin\n"
		"  --rand-seed N         Seed Frodo color-RAM LCG (default: 42; stored in play)\n"
		"  --main-blank          Stage 4.5: do not restore BEGIN into CpuMock Main\n"
		"                        (fresh chips, zero RAM/color; Twin still restores)\n"
		"  --no-audio            Disable SID\n"
		"  --resid               Use reSID 6581 (Main board; --no-audio wins)\n"
		"  --max-frames N        Quit after N frames\n"
		"  --max-cycles N        Quit once cycle counter reaches N\n"
		"  --max-seconds N       Quit after N wall-clock seconds (then hard\n"
		"                        _Exit(124) if still stuck after 2s grace)\n"
		"  --watch-main-pc A[,B…]  Debug probe: log Main opcode fetch at hex\n"
		"                        ADDR(s) (module watch-main, PC=$…); prints\n"
		"                        regs, from-PC, $01 bank (L/H/C), stack,\n"
		"                        and a 32-deep PC history ring\n"
		"  --kb FILE             Load Stage-2 knowledge base (*.kb.json)\n"
		"  --watch / --watch-verbose / --watch-window / --watch-font SIZE\n"
		"                        Watch window: click a variable, configured cheat keys\n"
		"  --watch-timeline FILE JSONL of watch changes (implies watch sampling)\n"
		"  --kb-check-verbose    Stage 3: also check KB watch:\"verbose\" slots\n"
		"  --trace-calls         Log fetches at KB objects with trace:\"yes\"\n"
		"  --coverage FILE       Record REVMCOV2 coverage (u16 counts/addr)\n"
		"  --headless            Hidden window, uncapped; requires --max-frames N\n"
		"                        or --use-play FILE\n"
		"  --log-debug CYCLE     Emit Debug logs once Main cycle >= CYCLE\n"
		"  --log-verbose CYCLE   Emit Verbose logs once Main cycle >= CYCLE\n"
		"                        (also enables Debug from the same cycle)\n"
		"  --events FILE         Structured JSONL event stream (Stage 3 sync\n"
		"                        telemetry); \"-\" writes EVENT lines to stderr\n"
		"  --report FILE         End-of-run JSON summary aggregated from the\n"
		"                        event stream\n"
#if defined(REVM_HAS_CPUMOCK)
		"  --no-twin             Stage 3: disable Twin (implies --ignore-checks,\n"
		"                        --ignore-asserts, --ignore-play-hashes)\n"
#endif
		"  --ignore-checks       Run Main↔Twin / kb / golden compares but do not abort\n"
		"  --ignore-asserts      Log Assert*/QuitOnAssert failures but do not hard Quit\n"
		"  --ignore-play-hashes  Skip Twin ↔ play.json snapshot SHA checks\n"
		"  --dump-fail DIR       Screen PPM dump dir (default revm-fail/)\n"
		"\n"
		"Snapshot hash = sha256sum of FullSnapshot .bin (entire POD).\n"
		"F12 or Ctrl+C quits cleanly (finalizes play / media).\n",
		argv0);
}

std::string exe_dir(const char * argv0) {
#if defined(__linux__)
	{
		std::error_code ec;
		fs::path link = fs::read_symlink("/proc/self/exe", ec);
		if (!ec && !link.empty()) return link.parent_path().string();
	}
#endif
	if (!argv0 || !*argv0) return {};
	fs::path p(argv0);
	std::error_code ec;
	if (p.is_relative()) {
		p = fs::absolute(p, ec);
		if (ec) return {};
	}
	return p.parent_path().string();
}

std::string find_default_config(const char * argv0) {
	if (fs::exists("revm.cfg")) return "revm.cfg";
	const std::string dir = exe_dir(argv0);
	if (!dir.empty()) {
		fs::path beside = fs::path(dir) / "revm.cfg";
		if (fs::exists(beside)) return beside.lexically_normal().string();
	}
	return {};
}

bool load_joystick_config(const std::string & path_arg, revm::JoystickConfig & cfg,
                          std::string & error, const char * argv0) {
	std::string path = path_arg;
	if (path.empty()) path = find_default_config(argv0);
	if (path.empty()) {
		path = "revm.cfg";
		if (!revm::JoystickConfig::WriteDefaultFile(path, error)) return false;
		REVM_LOG(REVM_INFO, "Created default input config: %s", path.c_str());
	}
	if (!cfg.LoadFile(path, error)) return false;
	REVM_LOG(REVM_DEBUG, "Joystick config: %s", path.c_str());
	return true;
}

std::string key_label(SDL_Scancode scancode) {
	if (scancode == SDL_SCANCODE_UNKNOWN) return "Unbound";
	const char * name = SDL_GetScancodeName(scancode);
	return name && *name ? name : "Unknown";
}

void print_live_key_sheet(const revm::JoystickConfig & cfg) {
	auto joy_row = [](const char * action, SDL_Scancode joy1, SDL_Scancode joy2) {
		const std::string left = key_label(joy1);
		const std::string right = key_label(joy2);
		std::fprintf(stderr, "  %-12s  %-18s  %-18s\n", action, left.c_str(), right.c_str());
	};
	auto shortcut = [](const char * keys, const char * action) {
		std::fprintf(stderr, "  %-18s  %s\n", keys, action);
	};

	std::fprintf(stderr,
	             "\nLive controls\n"
	             "  %-12s  %-18s  %-18s\n"
	             "  %-12s  %-18s  %-18s\n",
	             "", "Joystick 1", "Joystick 2",
	             "", "----------", "----------");
	joy_row("Up", cfg.joy1.up, cfg.joy2.up);
	joy_row("Down", cfg.joy1.down, cfg.joy2.down);
	joy_row("Left", cfg.joy1.left, cfg.joy2.left);
	joy_row("Right", cfg.joy1.right, cfg.joy2.right);
	joy_row("Fire", cfg.joy1.fire, cfg.joy2.fire);

	std::fprintf(stderr, "\n  %-18s  %s\n", "Shortcut", "Action");
	std::fprintf(stderr, "  %-18s  %s\n", "--------", "------");
	shortcut("F1-F8", "C64 function keys");
	shortcut("F9", "Mark frame/cycle timestamp");
	shortcut("Ctrl+F9", "Save screenshot as frameN.ppm in the current directory");
	shortcut("F10", "Pause / resume");
	shortcut("Ctrl+Tab", "Swap joystick ports 1 / 2");
	shortcut("F11", "C64 Restore (NMI) (disabled)");
	shortcut("F12", "Quit cleanly (finalize recording / media)");
	shortcut("Keypad Enter", "Toggle fullscreen");
	const std::string increment = key_label(cfg.cheats.increment);
	const std::string decrement = key_label(cfg.cheats.decrement);
	shortcut(increment.c_str(), "Increment selected KB variable (click to select in watch window)");
	shortcut(decrement.c_str(), "Decrement selected KB variable");
	std::fprintf(stderr, "\n");
}

bool parse_watch_main_pc_list(const char * text, std::vector<uint16_t> & out,
                              std::string & error) {
	out.clear();
	if (!text || !*text) {
		error = "empty --watch-main-pc address list";
		return false;
	}
	const char * p = text;
	while (*p) {
		while (*p == ' ' || *p == '\t') ++p;
		if (!*p) break;
		const char * start = p;
		while (*p && *p != ',') ++p;
		std::string tok(start, p);
		// trim trailing space
		while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
		if (tok.empty()) {
			error = "empty address in --watch-main-pc list";
			return false;
		}
		const char * t = tok.c_str();
		if (*t == '$') ++t;
		else if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t += 2;
		char * end = nullptr;
		unsigned long v = std::strtoul(t, &end, 16);
		if (end == t || *end != '\0' || v > 0xffffu) {
			error = std::string("invalid --watch-main-pc address: ") + tok;
			return false;
		}
		out.push_back(static_cast<uint16_t>(v));
		if (*p == ',') ++p;
	}
	if (out.empty()) {
		error = "empty --watch-main-pc address list";
		return false;
	}
	return true;
}

struct Opts {
	revm::Config cfg;
	std::string config_path;
	std::string load_prg;
	std::string load_snapshot;
	bool no_load = false;
	std::string record_play;
	std::string use_play;
	uint32_t max_frames = 0;
	revm::PlaybackCompareOpts compare;
	revm::ActiveRunSetup setup;
	const char * argv0 = nullptr;
};

void apply_speed_policy(Opts & o) {
	if (o.cfg.headless) o.cfg.limit_speed = false;
}

// Speakers off under --headless. SID synth is only whatever the user asked
// for (--resid); media flags never turn a renderer on by themselves.
void apply_headless_audio(Opts & o) {
	if (!o.cfg.headless) return;
	o.cfg.audio_enabled = false;
	if (o.cfg.no_audio) o.cfg.sid_resid = false;
}

void log_pacing(const revm::Config & cfg) {
	if (cfg.headless) {
		REVM_LOG(REVM_DEBUG, "pacing: uncapped (headless)");
	} else {
		REVM_LOG(REVM_DEBUG, "pacing: real-time 50 Hz");
	}
}

bool apply_use_play_end(Opts & o, std::string & error) {
	if (o.use_play.empty()) return true;
	if (o.max_frames != 0) return true;
	uint32_t last = 0;
	if (!revm::PlayEndFrame(o.use_play, last, error)) return false;
	o.max_frames = last;
	REVM_LOG(REVM_DEBUG, "Auto --max-frames %u from play %s", last,
	             o.use_play.c_str());
	return true;
}

bool parse_args(int argc, char ** argv, Opts & o, std::string & error) {
	o.argv0 = argv[0];
	int i = 1;
	auto need_i = [&](const char * flag) -> const char * {
		if (i >= argc) {
			error = std::string(flag) + " requires an argument";
			return nullptr;
		}
		return argv[i++];
	};

	while (i < argc) {
		const char * a = argv[i++];
		if (std::strcmp(a, "--version") == 0) {
			std::printf("revm %s\n", C64REWORK_VERSION);
			std::exit(0);
		} else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
			print_usage(argv[0]);
			std::exit(0);
		} else if (std::strcmp(a, "--load-prg") == 0) {
			const char * v = need_i("--load-prg");
			if (!v) return false;
			o.load_prg = v;
		} else if (std::strcmp(a, "--load-snapshot") == 0) {
			const char * v = need_i("--load-snapshot");
			if (!v) return false;
			o.load_snapshot = v;
			o.cfg.begin_snap_path = v;
		} else if (std::strcmp(a, "--no-load") == 0) {
			o.no_load = true;
		} else if (std::strcmp(a, "--record-play") == 0) {
			const char * v = need_i("--record-play");
			if (!v) return false;
			o.record_play = v;
		} else if (std::strcmp(a, "--use-play") == 0) {
			const char * v = need_i("--use-play");
			if (!v) return false;
			o.use_play = v;
			o.cfg.play_path = v;
		} else if (std::strcmp(a, "--save-snapshot") == 0) {
			const char * cyc = need_i("--save-snapshot");
			if (!cyc) return false;
			const char * file = need_i("--save-snapshot");
			if (!file) {
				error = "--save-snapshot requires CYCLE FILE";
				return false;
			}
			o.cfg.save_snapshot_cycle = std::strtoull(cyc, nullptr, 10);
			o.cfg.save_snapshot_path = file;
		} else if (std::strcmp(a, "--add-play-snapshot") == 0) {
			const char * cyc = need_i("--add-play-snapshot");
			if (!cyc) return false;
			o.cfg.add_play_snapshot_cycle = std::strtoull(cyc, nullptr, 10);
		} else if (std::strcmp(a, "--save-audio") == 0) {
			const char * v = need_i("--save-audio");
			if (!v) return false;
			o.cfg.save_audio_path = v;
		} else if (std::strcmp(a, "--save-video") == 0) {
			const char * v = need_i("--save-video");
			if (!v) return false;
			o.cfg.save_video_path = v;
		} else if (std::strcmp(a, "--save-screen") == 0) {
			const char * fr = need_i("--save-screen");
			if (!fr) return false;
			const char * file = need_i("--save-screen");
			if (!file || !*file) {
				error = "--save-screen requires FRAME FILE";
				return false;
			}
			o.cfg.save_screen_frame = uint32_t(std::strtoul(fr, nullptr, 10));
			o.cfg.save_screen_path = file;
		} else if (std::strcmp(a, "--frame-observations") == 0) {
			const char * v = need_i("--frame-observations");
			if (!v) return false;
			if (!*v) {
				error = "--frame-observations requires a file path";
				return false;
			}
			o.cfg.frame_observations_path = v;
		} else if (std::strcmp(a, "--config") == 0) {
			const char * v = need_i("--config");
			if (!v) return false;
			o.config_path = v;
		} else if (std::strcmp(a, "--rom-dir") == 0) {
			const char * v = need_i("--rom-dir");
			if (!v) return false;
			o.cfg.rom_dir = v;
		} else if (std::strcmp(a, "--rand-seed") == 0) {
			const char * v = need_i("--rand-seed");
			if (!v) return false;
			o.cfg.rand_seed = unsigned(std::strtoul(v, nullptr, 10));
		} else if (std::strcmp(a, "--main-blank") == 0) {
			o.cfg.main_blank = true;
		} else if (std::strcmp(a, "--no-audio") == 0) {
			o.cfg.no_audio = true;
			o.cfg.audio_enabled = false;
			o.cfg.sid_resid = false;
		} else if (std::strcmp(a, "--resid") == 0) {
			o.cfg.sid_resid = true;
		} else if (std::strcmp(a, "--max-frames") == 0) {
			const char * v = need_i("--max-frames");
			if (!v) return false;
			o.max_frames = uint32_t(std::strtoul(v, nullptr, 10));
		} else if (std::strcmp(a, "--max-cycles") == 0) {
			const char * v = need_i("--max-cycles");
			if (!v) return false;
			o.cfg.max_cycles = std::strtoull(v, nullptr, 10);
		} else if (std::strcmp(a, "--max-seconds") == 0) {
			const char * v = need_i("--max-seconds");
			if (!v) return false;
			char * end = nullptr;
			const double secs = std::strtod(v, &end);
			if (end == v || *end != '\0' || !(secs > 0.0)) {
				error = "--max-seconds requires a positive number";
				return false;
			}
			o.cfg.max_seconds = secs;
		} else if (std::strcmp(a, "--watch-main-pc") == 0) {
			const char * v = need_i("--watch-main-pc");
			if (!v) return false;
			if (!parse_watch_main_pc_list(v, o.cfg.watch_main_pcs, error)) return false;
		} else if (std::strcmp(a, "--kb") == 0) {
			const char * v = need_i("--kb");
			if (!v) return false;
			o.cfg.kb_path = v;
		} else if (std::strcmp(a, "--watch") == 0) {
			o.cfg.kb_watch = true;
		} else if (std::strcmp(a, "--watch-verbose") == 0) {
			o.cfg.kb_watch = true;
			o.cfg.kb_watch_verbose = true;
		} else if (std::strcmp(a, "--watch-window") == 0) {
			o.cfg.kb_watch_window = true;
		} else if (std::strcmp(a, "--watch-font") == 0) {
			const char * v = need_i("--watch-font");
			if (!v) return false;
			o.cfg.kb_watch_font = v;
		} else if (std::strcmp(a, "--kb-check-verbose") == 0) {
			o.cfg.kb_check_verbose = true;
		} else if (std::strcmp(a, "--trace-calls") == 0) {
			o.cfg.kb_trace_calls = true;
		} else if (std::strcmp(a, "--watch-timeline") == 0) {
			const char * v = need_i("--watch-timeline");
			if (!v) return false;
			o.cfg.watch_timeline_path = v;
		} else if (std::strcmp(a, "--coverage") == 0) {
			const char * v = need_i("--coverage");
			if (!v) return false;
			o.cfg.coverage_path = v;
		} else if (std::strcmp(a, "--headless") == 0) {
			o.cfg.headless = true;
		} else if (std::strcmp(a, "--log-debug") == 0) {
			const char * v = need_i("--log-debug");
			if (!v) return false;
			char * end = nullptr;
			unsigned long cyc = std::strtoul(v, &end, 0);
			if (end == v || *end != '\0' || cyc > 0xfffffffful) {
				error = "--log-debug requires a cycle number";
				return false;
			}
			o.cfg.log_debug_from_cycle = uint32_t(cyc);
		} else if (std::strcmp(a, "--log-verbose") == 0) {
			const char * v = need_i("--log-verbose");
			if (!v) return false;
			char * end = nullptr;
			unsigned long cyc = std::strtoul(v, &end, 0);
			if (end == v || *end != '\0' || cyc > 0xfffffffful) {
				error = "--log-verbose requires a cycle number";
				return false;
			}
			o.cfg.log_verbose_from_cycle = uint32_t(cyc);
		} else if (std::strcmp(a, "--events") == 0) {
			const char * v = need_i("--events");
			if (!v) return false;
			if (!*v) {
				error = "--events requires a file path";
				return false;
			}
			o.cfg.events_path = v;
		} else if (std::strcmp(a, "--report") == 0) {
			const char * v = need_i("--report");
			if (!v) return false;
			if (!*v) {
				error = "--report requires a file path";
				return false;
			}
			o.cfg.report_path = v;
		} else if (std::strcmp(a, "--no-twin") == 0) {
			o.compare.no_twin = true;
			o.compare.ignore_checks = true;
			o.compare.ignore_asserts = true;
			o.compare.ignore_play_hashes = true;
		} else if (std::strcmp(a, "--ignore-checks") == 0) {
			o.compare.ignore_checks = true;
		} else if (std::strcmp(a, "--ignore-asserts") == 0) {
			o.compare.ignore_asserts = true;
		} else if (std::strcmp(a, "--ignore-play-hashes") == 0) {
			o.compare.ignore_play_hashes = true;
		} else if (std::strcmp(a, "--dump-fail") == 0) {
			const char * v = need_i("--dump-fail");
			if (!v) return false;
			o.compare.dump_fail_dir = v;
		} else {
			error = std::string("Unknown option: ") + a;
			return false;
		}
	}
	return true;
}

bool validate(Opts & o, std::string & error) {
	// --no-audio wins over --resid regardless of flag order.
	if (o.cfg.no_audio) {
		o.cfg.audio_enabled = false;
		o.cfg.sid_resid = false;
	}
	if (o.cfg.headless && o.max_frames == 0 && o.use_play.empty()) {
		error = "--headless requires a positive --max-frames or --use-play";
		return false;
	}
	const bool want_media =
		!o.cfg.save_audio_path.empty() || !o.cfg.save_video_path.empty();
	if (want_media && o.cfg.no_audio) {
		error = "--save-audio/--save-video cannot be used with --no-audio";
		return false;
	}
	if (!o.cfg.save_video_path.empty() && !o.cfg.sid_resid) {
		error = "--save-video requires --resid";
		return false;
	}
	if (!o.cfg.save_audio_path.empty() && o.cfg.headless && !o.cfg.sid_resid) {
		error = "--save-audio under --headless requires --resid";
		return false;
	}

	const int machine_n = int(!o.load_prg.empty()) + int(!o.load_snapshot.empty()) +
	                      int(o.no_load);
	if (machine_n > 1) {
		error = "--load-prg, --load-snapshot, and --no-load are mutually exclusive";
		return false;
	}
	if (!o.record_play.empty() && !o.use_play.empty()) {
		error = "--record-play and --use-play are mutually exclusive";
		return false;
	}
	if (!o.record_play.empty() && o.load_prg.empty() && o.load_snapshot.empty() &&
	    !o.no_load) {
		error = "--record-play requires --load-prg, --load-snapshot, or --no-load";
		return false;
	}
	if (o.cfg.add_play_snapshot_cycle && o.use_play.empty()) {
		error = "--add-play-snapshot requires --use-play";
		return false;
	}
	if (!o.cfg.save_snapshot_path.empty() && !o.cfg.save_snapshot_cycle) {
		error = "--save-snapshot requires CYCLE and FILE";
		return false;
	}
	if (!o.cfg.save_screen_path.empty() && !o.cfg.save_screen_frame) {
		error = "--save-screen requires FRAME and FILE";
		return false;
	}
	const bool intrinsic_cpumock_start =
		o.setup.primary == revm::BoardRole::CpuMock && o.cfg.main_blank &&
		o.compare.no_twin;
	if (o.load_prg.empty() && o.load_snapshot.empty() && o.use_play.empty() &&
	    o.record_play.empty() && !o.no_load && !intrinsic_cpumock_start) {
		error = "Need --load-prg, --load-snapshot, --no-load, --record-play, "
		        "and/or --use-play";
		return false;
	}
	if (!o.load_prg.empty() && !fs::exists(o.load_prg)) {
		error = "PRG not found: " + o.load_prg;
		return false;
	}
	if (!o.load_snapshot.empty() && !fs::exists(o.load_snapshot)) {
		error = "Snapshot not found: " + o.load_snapshot;
		return false;
	}
#if defined(REVM_HAS_CPUMOCK)
	if (o.setup.primary == revm::BoardRole::CpuMock) {
		if (o.load_snapshot.empty() && o.record_play.empty() && !o.no_load &&
		    !intrinsic_cpumock_start) {
			// Stage 3 always needs BEGIN snap except when recording from PRG / BASIC.
			if (!o.use_play.empty() || o.load_prg.empty()) {
				if (o.use_play.empty() && o.load_prg.empty()) {
					error = "Stage 3 requires --load-snapshot";
					return false;
				}
			}
		}
		if (o.no_load) {
			error = "Stage 3 does not support --no-load (needs --load-snapshot)";
			return false;
		}
	}
#endif
	for (const auto & [flag, path] :
	     {std::pair{"--events", o.cfg.events_path},
	      {"--report", o.cfg.report_path}}) {
		if (path.empty() || path == "-") continue;
		FILE * f = std::fopen(path.c_str(), "a");
		if (!f) {
			error = flag + std::string(" path not writable: ") + path;
			return false;
		}
		std::fclose(f);
	}
	return true;
}

bool setup_live_or_play_input(revm::Board & board, Opts & o, revm::LiveInput & live,
                              revm::GoldenInput & golden, revm::PlayPlayer * player,
                              std::string & error) {
	if (player) {
		board.SetInputSource(&player->Input());
		board.SetPlayPlayer(player);
		return true;
	}
	if (!o.use_play.empty()) {
		error = "internal: use-play without player";
		return false;
	}
	revm::JoystickConfig jcfg;
	if (!load_joystick_config(o.config_path, jcfg, error, o.argv0)) return false;
	if (!o.cfg.headless) print_live_key_sheet(jcfg);
	live.SetJoystickConfig(jcfg);
	board.SetInputSource(&live);
	(void)golden;
	return true;
}

int run_record_play(Opts & o) {
	// A no-twin Stage 3 recording must be produced by the same CpuMock Main
	// path that will consume it.  The ordinary recorder below intentionally
	// remains the full-6510/C64 path for base builds and other topologies.
#if defined(REVM_HAS_CPUMOCK)
	if (o.setup.primary == revm::BoardRole::CpuMock && o.compare.no_twin) {
		if (o.load_snapshot.empty()) {
			REVM_LOG(REVM_ERROR,
			         "Stage 3 --record-play --no-twin requires --load-snapshot");
			return 1;
		}

		o.cfg.run_mode = revm::RunMode::Record;
		o.cfg.stage = revm::Stage::Stage3;
		o.cfg.prg_path.clear();
		o.cfg.begin_snap_path = o.load_snapshot;
		apply_speed_policy(o);
		apply_headless_audio(o);

		std::string error;
		revm::JoystickConfig jcfg;
		if (!load_joystick_config(o.config_path, jcfg, error, o.argv0)) {
			REVM_LOG(REVM_ERROR, "%s", error.c_str());
			return 1;
		}
		if (!o.cfg.headless) print_live_key_sheet(jcfg);

		revm::PlaySource src;
		src.type = "snapshot";
		src.path = o.load_snapshot;
		src.sha256 = revm::Sha256File(src.path);
		revm::PlayRecorder play;
		play.Configure(o.record_play, src);

		revm::cpumock::CpuMockHost host;
		const int rc = host.RunPlay(o.cfg.prg_path, nullptr, &jcfg, o.max_frames,
		                           o.cfg.headless, o.cfg.limit_speed,
		                           o.cfg.audio_enabled, o.compare, &o.cfg, &play);
		if (play.Started() && !play.Finish(error)) {
			REVM_LOG(REVM_ERROR, "Failed to write play log: %s", error.c_str());
			return 1;
		}
		return rc;
	}
#endif

	o.cfg.run_mode = revm::RunMode::Record;
	o.cfg.stage = revm::Stage::Stage1;
	const bool from_snap = !o.load_snapshot.empty();
	if (from_snap) {
		o.cfg.prg_path.clear();
		o.cfg.begin_snap_path = o.load_snapshot;
	} else {
		o.cfg.prg_path = o.load_prg; // empty with --no-load → BASIC boot
	}
	apply_speed_policy(o);
	apply_headless_audio(o);

	std::string error;
	revm::Board board;
	board.Configure(o.cfg);
	if (!board.Init(error)) {
		REVM_LOG(REVM_ERROR, "Init failed: %s", error.c_str());
		return 1;
	}
	install_quit_signals(board);

	revm::LiveInput live(board.Machine());
	revm::GoldenInput golden;
	if (!setup_live_or_play_input(board, o, live, golden, nullptr, error)) {
		REVM_LOG(REVM_ERROR, "%s", error.c_str());
		clear_quit_signals();
		return 1;
	}

	if (from_snap) {
		if (!board.LoadBeginSnap(o.load_snapshot, nullptr, error)) {
			REVM_LOG(REVM_ERROR, "Failed to restore snapshot: %s", error.c_str());
			clear_quit_signals();
			return 1;
		}
	}

	revm::PlaySource src;
	if (from_snap) {
		src.type = "snapshot";
		src.path = o.load_snapshot;
		src.sha256 = revm::Sha256File(src.path);
	} else if (o.no_load || o.load_prg.empty()) {
		src.type = "none";
		src.path.clear();
		src.sha256.clear();
	} else {
		src.type = "prg";
		src.path = o.load_prg;
		src.sha256 = revm::Sha256File(src.path);
	}

	revm::PlayRecorder play;
	play.Configure(o.record_play, src);
	play.Start(board);
	board.SetPlayRecorder(&play);

	board.SetFrameCallback([&](uint32_t frame, uint32_t) {
		if (o.max_frames && frame >= o.max_frames) board.RequestQuit(0);
	});

	REVM_LOG(REVM_DEBUG, "REVM --record-play %s", o.record_play.c_str());
	log_pacing(o.cfg);
	if (o.max_frames) {
		REVM_LOG(REVM_DEBUG, "Will stop at frame %u", o.max_frames);
	} else if (o.cfg.max_cycles) {
		REVM_LOG(REVM_DEBUG, "Will stop at cycle %llu",
		             static_cast<unsigned long long>(*o.cfg.max_cycles));
	}
	if (o.cfg.max_seconds) {
		REVM_LOG(REVM_DEBUG, "Will stop after %.3fs wall-clock",
		             *o.cfg.max_seconds);
	}

	int rc = board.Run();
	clear_quit_signals();
	if (!play.Finish(error)) {
		REVM_LOG(REVM_ERROR, "Failed to write play log: %s", error.c_str());
		return 1;
	}
	return rc;
}

int run_use_play(Opts & o) {
	std::string error;
	if (!apply_use_play_end(o, error)) {
		REVM_LOG(REVM_ERROR, "%s", error.c_str());
		return 1;
	}

	revm::PlayPlayer player;
	if (!player.Load(o.use_play, error)) {
		REVM_LOG(REVM_ERROR, "Failed to load play: %s", error.c_str());
		return 1;
	}
	player.SetCompareOpts(o.compare);
	// Play file seed wins for hash fidelity (warn if CLI --rand-seed differed).
	if (o.cfg.rand_seed != player.Log().rand_seed) {
		REVM_LOG(REVM_DEBUG, "use-play: applying play rand_seed=%u (overriding CLI/default %u)",
			player.Log().rand_seed, o.cfg.rand_seed);
	}
	o.cfg.rand_seed = player.Log().rand_seed;

#if defined(REVM_HAS_CPUMOCK)
	if (o.setup.primary == revm::BoardRole::CpuMock) {
		apply_speed_policy(o);
		apply_headless_audio(o);
		o.cfg.play_path = o.use_play;
		revm::cpumock::CpuMockHost host;
		return host.RunPlayback(o.use_play, o.max_frames, o.cfg.headless,
		                       o.cfg.limit_speed, o.cfg.audio_enabled, o.compare,
		                       &o.cfg);
	}
#endif

	revm::WarnPlayUsage(player.Log(), /*system_no_twin=*/false);

	o.cfg.run_mode = revm::RunMode::Playback;
	o.cfg.stage = revm::Stage::Stage1;
	o.cfg.play_path = o.use_play;
	if (!o.load_snapshot.empty()) {
		o.cfg.prg_path.clear();
		o.cfg.begin_snap_path = o.load_snapshot;
	} else if (!o.load_prg.empty()) {
		o.cfg.prg_path = o.load_prg;
	} else if (player.Log().source.type == "prg") {
		o.cfg.prg_path = player.Log().source.path;
	} else if (player.Log().source.type == "snapshot") {
		o.cfg.begin_snap_path = player.Log().source.path;
		o.load_snapshot = player.Log().source.path;
	}
	apply_speed_policy(o);
	apply_headless_audio(o);

	revm::Board board;
	board.Configure(o.cfg);
	if (!board.Init(error)) {
		REVM_LOG(REVM_ERROR, "Init failed: %s", error.c_str());
		return 1;
	}
	install_quit_signals(board);

	board.SetInputSource(&player.Input());
	board.SetPlayPlayer(&player);

	if (!o.load_snapshot.empty() || !o.cfg.begin_snap_path.empty()) {
		const std::string snap =
			!o.load_snapshot.empty() ? o.load_snapshot : o.cfg.begin_snap_path;
		if (!board.LoadBeginSnap(snap, &player.Input(), error)) {
			REVM_LOG(REVM_ERROR, "Failed to restore snapshot: %s", error.c_str());
			clear_quit_signals();
			return 1;
		}
		const uint64_t cyc = board.CycleCounter();
		if (const revm::PlaySnapshot * ps = revm::FindPlaySnapshot(player.Log(), cyc)) {
			const std::string file_hash = revm::Sha256File(snap);
			if (file_hash != ps->sha256) {
				REVM_LOG(REVM_ERROR, "load-snapshot hash mismatch for cycle %llu\n"
					"  play    %s\n"
					"  file    %s",
					static_cast<unsigned long long>(cyc), ps->sha256.c_str(),
					file_hash.c_str());
				clear_quit_signals();
				return 1;
			}
		}
	}
	// PRG autostart already handled by Board::Init via cfg.prg_path.

	board.SetFrameCallback([&](uint32_t frame, uint32_t) {
		if (o.max_frames && frame >= o.max_frames) board.RequestQuit(0);
	});

	REVM_LOG(REVM_DEBUG, "REVM --use-play %s (verify snapshot hashes on Main)",
	             o.use_play.c_str());
	log_pacing(o.cfg);

	int rc = board.Run();
	clear_quit_signals();
	REVM_LOG(REVM_DEBUG, "Comparisons=%llu Failures=%llu",
	             static_cast<unsigned long long>(player.Comparisons()),
	             static_cast<unsigned long long>(player.Failures()));
	if (rc != 0) return rc;
	return player.Ok() ? 0 : 1;
}

int run_interactive(Opts & o) {
	o.cfg.run_mode = revm::RunMode::Play;
	o.cfg.stage = revm::Stage::Stage1;
	o.cfg.prg_path = o.load_prg;
	if (!o.load_snapshot.empty()) {
		o.cfg.prg_path.clear();
		o.cfg.begin_snap_path = o.load_snapshot;
	}
	apply_speed_policy(o);
	apply_headless_audio(o);

	std::string error;

#if defined(REVM_HAS_CPUMOCK)
	if (o.setup.primary == revm::BoardRole::CpuMock) {
		revm::JoystickConfig jcfg;
		if (!load_joystick_config(o.config_path, jcfg, error, o.argv0)) {
			REVM_LOG(REVM_ERROR, "%s", error.c_str());
			return 1;
		}
		if (!o.cfg.headless) print_live_key_sheet(jcfg);
		log_pacing(o.cfg);
		revm::cpumock::CpuMockHost host;
		return host.RunPlay(o.cfg.prg_path, nullptr, &jcfg, o.max_frames,
		                   o.cfg.headless, o.cfg.limit_speed, o.cfg.audio_enabled,
		                   o.compare, &o.cfg);
	}
#endif

	revm::Board board;
	board.Configure(o.cfg);
	if (!board.Init(error)) {
		REVM_LOG(REVM_ERROR, "Init failed: %s", error.c_str());
		return 1;
	}
	install_quit_signals(board);

	revm::LiveInput live(board.Machine());
	revm::GoldenInput golden;
	if (!setup_live_or_play_input(board, o, live, golden, nullptr, error)) {
		REVM_LOG(REVM_ERROR, "%s", error.c_str());
		clear_quit_signals();
		return 1;
	}

	if (!o.load_snapshot.empty()) {
		if (!board.LoadBeginSnap(o.load_snapshot, nullptr, error)) {
			REVM_LOG(REVM_ERROR, "Failed to restore snapshot: %s", error.c_str());
			clear_quit_signals();
			return 1;
		}
	}
	// PRG autostart via Board::Init (cfg.prg_path).

	board.SetFrameCallback([&](uint32_t frame, uint32_t) {
		if (o.max_frames && frame >= o.max_frames) board.RequestQuit(0);
	});

	REVM_LOG(REVM_DEBUG, "REVM — PAL SC%s",
	             o.no_load ? " (BASIC, --no-load)" : "");
	log_pacing(o.cfg);
	int rc = board.Run();
	clear_quit_signals();
	return rc;
}

} // namespace

int main(int argc, char ** argv) {
	Opts o;
	std::string error;
	if (!parse_args(argc, argv, o, error)) {
		REVM_LOG(REVM_ERROR, "%s", error.c_str());
		print_usage(argv[0]);
		return 1;
	}
	revm::RunSetupFlags flags;
	flags.no_twin = o.compare.no_twin;
	if (!revm::ResolveActiveSetup(revm::BuiltSetup(), flags, o.setup, error)) {
		REVM_LOG(REVM_ERROR, "%s", error.c_str());
		return 1;
	}
	if (!validate(o, error)) {
		REVM_LOG(REVM_ERROR, "%s", error.c_str());
		return 1;
	}

	revm::log::Init(o.cfg.log_verbose_from_cycle, o.cfg.log_debug_from_cycle);
	revm::log::Banner(argv[0]);
	REVM_LOG(REVM_INFO, "REVM boards: %s",
	         revm::FormatActiveBoards(o.setup).c_str());

	int rc;
	if (!o.record_play.empty()) rc = run_record_play(o);
	else if (!o.use_play.empty()) rc = run_use_play(o);
	else rc = run_interactive(o);

	revm::log::Shutdown();
	return rc;
}
