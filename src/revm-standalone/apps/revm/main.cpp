// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "core/Config.hpp"
#include "core/StandaloneRunner.hpp"
#include "input/JoystickConfig.hpp"
#define REVM_LOG_MODULE "game"
#include "util/Log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

struct Options {
	revm::Config config;
	std::string config_path;
	std::string use_play;
	std::string record_play;
	uint32_t max_frames = 0;
	bool no_audio = false;
};

void print_usage(const char * program) {
	std::fprintf(stderr,
	             "Usage: %s [options]\n"
	             "\n"
	             "  --config FILE    Input configuration\n"
	             "  --record-play FILE  Record play JSON (inputs, rand_seed; always no-twin)\n"
	             "  --use-play FILE  Replay recorded inputs\n"
	             "  --headless       Hidden; requires --max-frames N or --use-play FILE\n"
	             "  --max-frames N   Stop at frame N (test convenience)\n"
	             "  --save-screen FRAME FILE  Pepto P6 PPM of Main at VBLANK FRAME\n"
	             "  --no-audio       Disable SID audio\n"
	             "  --resid          Use the reSID 6581 renderer\n"
	             "  --version        Print the C64 Rework framework version\n"
	             "  -h, --help       Show this help\n",
	             program);
}

bool parse_uint32(const char * text, uint32_t & value) {
	if (!text || !*text || *text == '-') return false;
	errno = 0;
	char * end = nullptr;
	const unsigned long parsed = std::strtoul(text, &end, 10);
	if (errno || end == text || *end || parsed > 0xfffffffful) return false;
	value = static_cast<uint32_t>(parsed);
	return true;
}

bool parse_options(int argc, char ** argv, Options & options, std::string & error) {
	for (int i = 1; i < argc; ++i) {
		const char * argument = argv[i];
		auto value_after = [&](const char * name) -> const char * {
			if (++i < argc) return argv[i];
			error = std::string(name) + " requires a value";
			return nullptr;
		};
		if (std::strcmp(argument, "--version") == 0) {
			std::printf("revm %s\n", C64REWORK_VERSION);
			std::exit(0);
		} else if (std::strcmp(argument, "-h") == 0 ||
		    std::strcmp(argument, "--help") == 0) {
			print_usage(argv[0]);
			std::exit(0);
		} else if (std::strcmp(argument, "--config") == 0) {
			const char * value = value_after(argument);
			if (!value) return false;
			options.config_path = value;
		} else if (std::strcmp(argument, "--record-play") == 0) {
			const char * value = value_after(argument);
			if (!value) return false;
			options.record_play = value;
		} else if (std::strcmp(argument, "--use-play") == 0) {
			const char * value = value_after(argument);
			if (!value) return false;
			options.use_play = value;
		} else if (std::strcmp(argument, "--headless") == 0) {
			options.config.headless = true;
			options.config.limit_speed = false;
			if (options.config.sid == revm::SidMode::Digital)
				options.config.sid = revm::SidMode::None;
		} else if (std::strcmp(argument, "--max-frames") == 0) {
			const char * value = value_after(argument);
			if (!value) return false;
			if (!parse_uint32(value, options.max_frames)) {
				error = "--max-frames requires an unsigned integer";
				return false;
			}
		} else if (std::strcmp(argument, "--save-screen") == 0) {
			if (i + 2 >= argc) {
				error = "--save-screen requires FRAME FILE";
				return false;
			}
			const char * frame_text = argv[++i];
			const char * file = argv[++i];
			uint32_t frame = 0;
			if (!parse_uint32(frame_text, frame) || !file || !*file) {
				error = "--save-screen requires FRAME FILE";
				return false;
			}
			options.config.save_screen_frame = frame;
			options.config.save_screen_path = file;
		} else if (std::strcmp(argument, "--no-audio") == 0) {
			options.no_audio = true;
			options.config.sid = revm::SidMode::None;
		} else if (std::strcmp(argument, "--resid") == 0) {
			if (!options.no_audio)
				options.config.sid = revm::SidMode::Resid;
		} else {
			error = std::string("unknown option: ") + argument;
			return false;
		}
	}
	return true;
}

std::string executable_directory(const char * argv0) {
#if defined(__linux__)
	std::error_code link_error;
	const fs::path executable = fs::read_symlink("/proc/self/exe", link_error);
	if (!link_error && !executable.empty()) return executable.parent_path().string();
#endif
	if (!argv0 || !*argv0) return {};
	std::error_code path_error;
	fs::path fallback_executable = fs::absolute(argv0, path_error);
	return path_error ? std::string{} : fallback_executable.parent_path().string();
}

bool load_input_config(const Options & options, const char * argv0,
                       revm::JoystickConfig & config, std::string & error) {
	std::string path = options.config_path;
	if (path.empty() && fs::exists("revm.cfg")) path = "revm.cfg";
	if (path.empty()) {
		const std::string directory = executable_directory(argv0);
		const fs::path beside_binary = fs::path(directory) / "revm.cfg";
		if (!directory.empty() && fs::exists(beside_binary))
			path = beside_binary.lexically_normal().string();
	}
	if (path.empty()) {
		path = "revm.cfg";
		if (!revm::JoystickConfig::WriteDefaultFile(path, error)) return false;
		REVM_LOG(REVM_INFO, "Created default input config: %s", path.c_str());
	}
	return config.LoadFile(path, error);
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
	shortcut("Ctrl+F9", "Save screenshot as frameN.ppm in the current directory");
	shortcut("F10", "Pause / resume");
	shortcut("Ctrl+Tab", "Swap joystick ports 1 / 2");
	shortcut("F12", "Quit cleanly (finalize recording)");
	shortcut("Keypad Enter", "Toggle fullscreen");
	std::fprintf(stderr, "\n");
}

} // namespace

int main(int argc, char ** argv) {
	Options options;

	std::string error;
	if (!parse_options(argc, argv, options, error)) {
		std::fprintf(stderr, "game: %s\n", error.c_str());
		print_usage(argv[0]);
		return 2;
	}
	if (!options.record_play.empty() && !options.use_play.empty()) {
		std::fprintf(stderr,
		             "game: --record-play and --use-play are mutually exclusive\n");
		return 2;
	}
	if (options.config.headless && options.max_frames == 0 &&
	    options.use_play.empty()) {
		std::fprintf(stderr,
		             "game: --headless requires a positive --max-frames or "
		             "--use-play\n");
		return 2;
	}

	revm::log::Init({}, {});
	revm::log::Banner(argv[0]);
	revm::JoystickConfig joystick;
	if (!load_input_config(options, argv[0], joystick, error)) {
		REVM_LOG(REVM_ERROR, "%s", error.c_str());
		revm::log::Shutdown();
		return 1;
	}
	if (!options.config.headless)
		print_live_key_sheet(joystick);

	revm::StandaloneRunner runner;
	revm::StandaloneRun run;
	run.config = options.config;
	run.max_frames = options.max_frames;
	run.joystick = &joystick;
	int result = 0;
	if (!options.record_play.empty())
		result = runner.RunRecord(options.record_play, run);
	else if (!options.use_play.empty())
		result = runner.RunPlayback(options.use_play, run);
	else
		result = runner.RunPlay(run);
	revm::log::Shutdown();
	return result;
}
