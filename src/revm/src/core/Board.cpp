// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "core/Board.hpp"
#include "goldens/CompareReport.hpp"
#include "goldens/PlayLog.hpp"
#include "goldens/PlayPlayer.hpp"
#include "goldens/PlayRecorder.hpp"
#include "goldens/FrameObservation.hpp"
#include "input/GoldenInput.hpp"
#include "input/LiveInput.hpp"
#include "snapshot/Snapshot.hpp"
#include "util/Hash.hpp"
#define REVM_LOG_MODULE "board"
#include "util/Event.hpp"
#include "util/Log.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

#include "C64.h"
#include "Prefs.h"
#include "roms/Roms.hpp"
#include "main.h"
#include "CIA.h"
#include "CPUC64.h"
#include "VIC.h"
#include "SID.h"
#include "CPU1541.h"
#include "1541gcr.h"
#include "Tape.h"
#include "Display.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>

#include <pthread.h>
#include <unistd.h>

#include <SDL.h>

namespace fs = std::filesystem;

namespace revm {

namespace {

void mem_access_thunk(uint16_t adr, bool /*is_write*/, MOS6510::CoverSpace space,
                      void * userdata) {
	if (space == MOS6510::CoverSpace::Io) return;
	auto * self = static_cast<Board *>(userdata);
	self->RecordMemAccess(adr, space == MOS6510::CoverSpace::Rom);
}

void supervisor_diagnostic(const char * reason, uint32_t frame,
                           uint32_t cycle, uint64_t generation) {
	char message[320];
	const int length = std::snprintf(
		message, sizeof message,
		"revm: supervisor: %s; last VBlank frame=%u cycle=%u generation=%llu\n",
		reason, frame, cycle, static_cast<unsigned long long>(generation));
	if (length > 0) {
		const size_t count = std::min<size_t>(static_cast<size_t>(length),
		                                      sizeof message - 1);
		const ssize_t written = ::write(STDERR_FILENO, message, count);
		(void)written;
	}
}

[[noreturn]] void supervisor_hard_abort(pthread_t supervised_thread,
                                        const char * reason,
                                        uint32_t frame, uint32_t cycle,
                                        uint64_t generation) {
	supervisor_diagnostic(reason, frame, cycle, generation);
	static constexpr char message[] =
		"revm: supervisor: sending SIGABRT to the emulation thread for a "
		"core/stack trace\n";
	const ssize_t written = ::write(STDERR_FILENO, message, sizeof message - 1);
	(void)written;
	if (::pthread_kill(supervised_thread, SIGABRT) == 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	std::_Exit(124);
}

bool drive_busy_for_warp(const C64 * c64) {
	// Frodo parks the 1541 CPU when DOS hits its idle loop (Idle=true) and
	// does not tick it again until IEC ATN. Motor-on is not a busy signal:
	// the DOS motor timeout never runs while Idle, so the spindle would
	// stay "on" and warp would never end.
	return c64 && ThePrefs.Emul1541Proc && c64->TheCPU1541 &&
	       !c64->TheCPU1541->Idle;
}

} // namespace

Board::Board() = default;

Board::~Board() {
	StopWallClockLimit();
	flush_coverage();
	if (!secondary_) log::ClearClockSource();
	if (watch_timeline_file_) {
		kb_watch_.SetTimelineFile(nullptr);
		std::fclose(watch_timeline_file_);
		watch_timeline_file_ = nullptr;
	}
	kb_watch_window_.reset();
	if (machine_owned_ && c64_) {
		if (!secondary_ && TheC64 == c64_) {
			TheC64 = nullptr;
		}
		delete c64_;
		c64_ = nullptr;
	}
	if (sdl_initialized_ && !secondary_) {
		SDL_Quit();
		sdl_initialized_ = false;
	}
}

void Board::Configure(const Config & cfg) {
	cfg_ = cfg;
	mod_mirror_ = nullptr;
	coverage_active_ = !cfg_.coverage_path.empty();
	save_snapshot_done_ = false;
	save_screen_done_ = false;
	add_play_snapshot_done_ = false;
	snapshot_capture_pending_ =
		(!cfg_.save_snapshot_path.empty() && cfg_.save_snapshot_cycle) ||
		(!cfg_.play_path.empty() && cfg_.add_play_snapshot_cycle);
	audio_armed_ = false;
	media_wav_path_.clear();
	media_wav_is_temp_ = false;
	video_rgb_.clear();
	video_frames_ = 0;
	frame_observations_.reset();
	if (!cfg_.frame_observations_path.empty()) {
		frame_observations_ = std::make_unique<FrameObservationWriter>();
		std::string observation_error;
		if (!frame_observations_->Open(cfg_.frame_observations_path,
		                              observation_error)) {
			REVM_LOG(REVM_ERROR, "%s", observation_error.c_str());
			frame_observations_.reset();
		}
	}
	pc_watches_.clear();
	for (uint16_t pc : cfg_.watch_main_pcs)
		pc_watches_.push_back(PcWatch{pc, {}, 0, /*verbose=*/true});
	watch_pc_hist_next_ = 0;
	watch_pc_hist_count_ = 0;
	if (coverage_active_) coverage_.Clear();

	kb_.Clear();
	kb_load_error_.clear();
	kb_watch_.Reset();
	kb_watch_window_.reset();
	trace_calls_ = false;
	last_trace_ = {};
	if (watch_timeline_file_) {
		std::fclose(watch_timeline_file_);
		watch_timeline_file_ = nullptr;
	}
	if (!cfg_.kb_path.empty()) {
		std::string err;
		if (!kb_.LoadFile(cfg_.kb_path, err)) {
			kb_load_error_ = err;
			REVM_LOG(REVM_ERROR, "KB load failed: %s", err.c_str());
		} else {
			REVM_LOG(REVM_DEBUG, "KB: %s (%zu objects)", cfg_.kb_path.c_str(),
			             kb_.Size());
			const bool want_timeline = !cfg_.watch_timeline_path.empty();
			const bool want_watch = cfg_.kb_watch || cfg_.kb_watch_verbose ||
			                       cfg_.kb_watch_window || want_timeline;
			if (want_watch) {
				const bool print_stderr =
				    cfg_.kb_watch && !cfg_.kb_watch_window;
				kb_watch_.Configure(kb_, true, cfg_.kb_watch_verbose,
				                    print_stderr);
				if (want_timeline) {
					watch_timeline_file_ =
					    std::fopen(cfg_.watch_timeline_path.c_str(), "w");
					if (!watch_timeline_file_) {
						REVM_LOG(REVM_ERROR, "watch-timeline: cannot write %s",
						             cfg_.watch_timeline_path.c_str());
					} else {
						kb_watch_.SetTimelineFile(watch_timeline_file_);
						REVM_LOG(REVM_DEBUG, "watch-timeline → %s",
						             cfg_.watch_timeline_path.c_str());
					}
				}
				REVM_LOG(REVM_DEBUG, "KB watch: %zu slot(s)%s%s",
				             kb_watch_.SlotCount(),
				             cfg_.kb_watch_verbose ? " (verbose)" : "",
				             print_stderr ? " [log]"
				             : cfg_.kb_watch_window ? " [window]"
				                                   : "");
			}
			if (cfg_.kb_trace_calls) {
				size_t n = 0;
				for (const auto & o : kb_.Objects()) {
					if (o.trace && !o.name.empty()) ++n;
				}
				trace_calls_ = n > 0;
				if (trace_calls_) {
					REVM_LOG(REVM_DEBUG, "KB trace-calls: %zu object(s)", n);
				}
			}
		}
	} else if (cfg_.kb_watch || cfg_.kb_watch_verbose || cfg_.kb_watch_window ||
	           cfg_.kb_trace_calls || cfg_.kb_check ||
	           cfg_.kb_check_verbose ||
	           !cfg_.watch_timeline_path.empty()) {
		REVM_LOG(REVM_ERROR, "KB feature requested but no --kb FILE");
	}
}

bool Board::Init(std::string & error) {
	return InitAs(BoardInitRole::Primary, error);
}

bool Board::InitSecondary(std::string & error) {
	return InitAs(BoardInitRole::SecondaryTwinQuiet, error);
}

bool Board::InitAs(BoardInitRole role, std::string & error) {
	return init_machine(error, role);
}

bool Board::init_machine(std::string & error, BoardInitRole role) {
	if (c64_) {
		error = "Board already initialized";
		return false;
	}
	if (!kb_load_error_.empty()) {
		error = "KB load failed: " + kb_load_error_;
		return false;
	}

	init_role_ = role;
	const bool secondary = role != BoardInitRole::Primary;
	secondary_ = secondary;
	vsync_ordinal_ = 0;

	if (!secondary) {
		// Frodo's main() did this before constructing C64; SID needs AUDIO ready.
		Uint32 sdl_flags = SDL_INIT_TIMER | SDL_INIT_EVENTS;
		if (!cfg_.headless) {
			sdl_flags |= SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;
		} else {
			// VIC still needs a Display/pixel buffer; use SDL's dummy driver so no
			// real window appears (TestBench also sets SDL_WINDOW_HIDDEN).
			SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
			sdl_flags |= SDL_INIT_VIDEO;
		}
		if (cfg_.audio_enabled) {
			sdl_flags |= SDL_INIT_AUDIO;
		}

		if (SDL_Init(sdl_flags) < 0) {
			error = std::string("SDL_Init failed: ") + SDL_GetError();
			return false;
		}
		sdl_initialized_ = true;
	} else if (!SDL_WasInit(SDL_INIT_VIDEO)) {
		error = "Secondary board requires a primary Board (SDL not initialized)";
		return false;
	}

	// Snapshot prefs we may mutate for a secondary oracle board.
	const bool saved_limit = ThePrefs.LimitSpeed;
	const bool saved_1541 = ThePrefs.Emul1541Proc;
	const bool saved_leds = ThePrefs.ShowLEDs;
	const bool saved_autostart = ThePrefs.AutoStart;
	const int saved_sid = ThePrefs.SIDType;
	const bool saved_testbench = ThePrefs.TestBench;
	const std::string saved_load = ThePrefs.LoadProgram;
	const std::string saved_drive0 = ThePrefs.DrivePath[0];

	ThePrefs.LimitSpeed = secondary ? false : cfg_.limit_speed;
	ThePrefs.Emul1541Proc = false;
	ThePrefs.DrivePath[0].clear();
	ThePrefs.ShowLEDs = secondary ? false : !cfg_.headless;
	ThePrefs.AutoStart = false;
	if (secondary || cfg_.no_audio) {
		ThePrefs.SIDType = SIDTYPE_NONE;
	} else if (cfg_.sid_resid) {
		ThePrefs.SIDType = SIDTYPE_RESID_6581;
		if (!secondary) {
			REVM_LOG(REVM_DEBUG, "SID: reSID 6581 (--resid)");
		}
	} else if (cfg_.audio_enabled) {
		ThePrefs.SIDType = SIDTYPE_DIGITAL_6581;
	} else {
		ThePrefs.SIDType = SIDTYPE_NONE;
	}
	ThePrefs.TestBench = secondary || cfg_.headless;
	ThePrefs.LoadProgram.clear();

	if (!secondary) {
		if (cfg_.rom_dir.empty()) {
			error = "ROM directory required (--rom-dir); set rom_dir in rework.toml";
			return false;
		}
		revm::RomDirFiles files;
		if (!InspectRomDir(cfg_.rom_dir, files, rom_hashes_, error)) return false;
		ROMPaths paths;
		paths.BasicROMPath = files.basic;
		paths.KernalROMPath = files.kernal;
		paths.CharROMPath = files.chars;
		paths.DriveROMPath = files.drive;
		ThePrefs.ROMSetDefs["revm"] = paths;
		ThePrefs.ROMSet = "revm";
		REVM_LOG(REVM_DEBUG,
		         "ROMs basic=%s kernal=%s chargen=%s dos1541ii=%s",
		         rom_hashes_.basic.c_str(), rom_hashes_.kernal.c_str(),
		         rom_hashes_.chargen.c_str(),
		         rom_hashes_.dos1541ii.empty() ? "-" : rom_hashes_.dos1541ii.c_str());

		if (!cfg_.prg_path.empty()) {
			ThePrefs.LoadProgram = cfg_.prg_path;
			ThePrefs.AutoStart = true;
		} else {
			if (!cfg_.begin_snap_path.empty() && !cfg_.drive_from_snapshot) {
				cfg_.drive_from_snapshot = FullSnapshotFileHasDrive(cfg_.begin_snap_path);
			}
			const bool want_drive =
				!cfg_.d64_path.empty() || cfg_.drive_from_snapshot;
			if (want_drive) {
				if (files.drive.empty()) {
					error = "--load-d64 / d64 snapshot requires a 1541 ROM in "
					        "rom_dir (dos1541ii*.bin)";
					return false;
				}
				ThePrefs.Emul1541Proc = true;
				ThePrefs.DrivePath[0] = cfg_.d64_path;
				ThePrefs.AutoStart = cfg_.disk_auto_load && !cfg_.d64_path.empty();
				REVM_LOG(REVM_DEBUG, "1541 GCR drive on%s%s%s",
				         cfg_.d64_path.empty() ? " (from snapshot)" : "",
				         ThePrefs.AutoStart ? " disk-auto-load" : "",
				         cfg_.disk_warp ? " disk-warp" : "");
			}
		}
	}

	// Color RAM power-on noise seed (private LCG in init_memory; not libc rand).
	SetFrodoColorRamSeed(cfg_.rand_seed);
	if (!secondary) {
		REVM_LOG(REVM_DEBUG, "rand seed %u (Frodo color RAM; --rand-seed)",
		             cfg_.rand_seed);
	}

	c64_ = new C64();
	if (!secondary) {
		TheC64 = c64_;
		if (!cfg_.d64_path.empty() &&
		    (!c64_->TheGCRDisk || c64_->TheGCRDisk->NumTracks() == 0)) {
			error = "Failed to mount D64 (not a disk image?): " + cfg_.d64_path;
			return false;
		}
	}
	machine_owned_ = true;
	clock_.Reset();

	if (secondary) {
		// Restore primary prefs so twin construction does not stick.
		ThePrefs.LimitSpeed = saved_limit;
		ThePrefs.Emul1541Proc = saved_1541;
		ThePrefs.ShowLEDs = saved_leds;
		ThePrefs.AutoStart = saved_autostart;
		ThePrefs.SIDType = saved_sid;
		ThePrefs.TestBench = saved_testbench;
		ThePrefs.LoadProgram = saved_load;
		ThePrefs.DrivePath[0] = saved_drive0;
		if (role == BoardInitRole::SecondaryTwinQuiet)
			SetQuietVBlank(true);
	}

	if ((coverage_active_ || trace_calls_) && c64_->TheCPU) {
		c64_->TheCPU->SetPcFetchHook(&Board::PcFetchThunk, this);
	}
	if (coverage_active_ && c64_->TheCPU) {
		c64_->TheCPU->SetMemAccessHook(&mem_access_thunk, this);
	}

	if (!secondary_) {
		// Stage 3 CpuMockHost::wire_defaults replaces this with Main+Twin.
		log::SetClockSource([this] {
			log::Clocks c;
			c.main.frame = FrameCounter();
			c.main.cycle = CycleCounter();
			if (HasLastVBlank()) {
				c.main.frame_start = LastVBlankCycle();
				c.main.frame_end = c.main.frame_start + VBlankPeriod();
			}
			return c;
		});
	}

	if (!secondary && cfg_.kb_watch_window) {
		if (cfg_.headless) {
			REVM_LOG(REVM_DEBUG, "KB watch-window ignored under --headless");
		} else if (!kb_watch_.Active()) {
			REVM_LOG(REVM_DEBUG, "KB watch-window: no watch slots (set watch:yes in KB)");
		} else {
			kb_watch_window_ = std::make_unique<KbWatchWindow>();
			std::string werr;
			WatchFont font = WatchFont::Spleen12x24;
			if (!KbWatchWindow::ParseFontName(cfg_.kb_watch_font, font, werr)) {
				REVM_LOG(REVM_ERROR, "KB watch-window: %s", werr.c_str());
				kb_watch_window_.reset();
			} else if (!kb_watch_window_->Open(kb_watch_.SlotCount(), font, werr)) {
				REVM_LOG(REVM_ERROR, "KB watch-window failed: %s", werr.c_str());
				kb_watch_window_.reset();
			} else {
				REVM_LOG(REVM_DEBUG, "KB watch-window: open (%zu slots, font=%s)",
				             kb_watch_.SlotCount(), cfg_.kb_watch_font.c_str());
			}
		}
	}
	return true;
}

void Board::ApplyInput(const InputFrame & in) {
	apply_input(in);
}

void Board::SetFreezeFrameCounter(bool freeze) {
	if (c64_)
		c64_->SetFreezeFrameCounter(freeze);
}

void Board::Reset(bool clear_memory) {
	if (!c64_) return;
	if (clear_memory) {
		SetFrodoColorRamSeed(cfg_.rand_seed);
	}
	c64_->Reset(clear_memory);
	c64_->ResetCounters();
	clock_.Reset();
}

bool Board::LoadPrg(const std::string & path, std::string & error) {
	if (!c64_) {
		error = "Board not initialized";
		return false;
	}
	return c64_->DMALoad(path, error);
}

void Board::RequestAutostart() {
	if (!c64_) return;
	ThePrefs.AutoStart = true;
	c64_->ResetAndAutoStart();
}

bool Board::finish_cycle(bool vblank) {
	const int supervisor_code =
		supervisor_quit_code_.exchange(0, std::memory_order_acq_rel);
	if (supervisor_code != 0)
		RequestQuit(supervisor_code);
	if (snapshot_capture_pending_) maybe_save_snapshot();
	if (cfg_.max_cycles) check_max_cycles();
	if (wall_deadline_) check_max_seconds();

	if (vblank) {
		note_vblank();
		on_frame_boundary();
	}
	// After VSYNC handlers: play end hashes are recorded in Finish() post-boundary
	// (TOD + input applied). Checking before boundary made end-of-play hashes diverge.
	if (play_player_) {
		Board & hash_board = snapshot_source_ ? *snapshot_source_ : *this;
		if (play_player_->HashCheckDue(hash_board.CycleCounter()))
			maybe_check_play_hashes();
	}

	return vblank;
}

bool Board::EmulateCycle() {
	if (!c64_) return false;

	if (!pc_watches_.empty()) check_watch_pc_before_cycle();
	return finish_cycle(c64_->EmulateCycle());
}

bool Board::EmulateCycleInjectCpu(const std::function<void()> & cpu_slot) {
	if (!c64_) return false;

	if (!pc_watches_.empty()) check_watch_pc_before_cycle();
	const unsigned flags = c64_->EmulateCycleBeforeCpu();
	cpu_slot();
	return finish_cycle(c64_->EmulateCycleAfterCpu(flags));
}

void Board::WatchPc(uint16_t pc, std::string label, bool verbose) {
	for (auto & w : pc_watches_) {
		if (w.pc != pc)
			continue;
		if (!label.empty())
			w.label = std::move(label);
		w.verbose = w.verbose || verbose;
		return;
	}
	pc_watches_.push_back(PcWatch{pc, std::move(label), 0, verbose});
}

void Board::ClearPcWatches() { pc_watches_.clear(); }

uint64_t Board::WatchHits(uint16_t pc) const {
	uint64_t n = 0;
	for (const auto & w : pc_watches_)
		if (w.pc == pc)
			n += w.hits;
	return n;
}

void Board::check_watch_pc_before_cycle() {
	if (!c64_ || !c64_->TheCPU) return;

	// About to fetch an opcode (skip BA stalls).
	if (!c64_->TheCPU->InstructionComplete() || c64_->TheCPU->BALow) return;

	const uint16_t pc = c64_->TheCPU->GetPC();
	const char * mod =
		init_role_ == BoardInitRole::SecondaryTwinQuiet ? "watch-twin"
		                                                 : "watch-main";
	for (auto & w : pc_watches_) {
		if (pc != w.pc) continue;
		++w.hits;
		// Emits + aggregates for the report (no-op when events are off).
		revm::event::EmitWatchHit(pc, w.label.c_str(), w.hits);

		const char * label =
			w.label.empty() ? "(unnamed)" : w.label.c_str();
		// Module = which board; PC= in the message marks the probe kind.
		REVM_LOG_TIMED_M(mod, REVM_VERBOSE, "%s PC=$%04X hit=#%llu", label, pc,
		                 static_cast<unsigned long long>(w.hits));

		if (w.verbose) {
			MOS6510State s;
			c64_->TheCPU->GetState(&s);
			// Short stack dump: bytes at SP+1 .. (up to 8, only stacked depth).
			const uint8_t sp_lo = uint8_t(s.sp & 0xff);
			const unsigned stacked = unsigned(0xFF - sp_lo);
			const unsigned nstack = stacked < 8 ? stacked : 8u;
			char stack_buf[64];
			stack_buf[0] = '\0';
			if (nstack > 0 && c64_->RAM) {
				char * p = stack_buf;
				char * end = stack_buf + sizeof(stack_buf);
				p += std::snprintf(p, size_t(end - p), " stack:");
				for (unsigned b = 0; b < nstack && p < end; ++b) {
					const uint8_t slot = uint8_t((sp_lo + 1 + b) & 0xff);
					p += std::snprintf(p, size_t(end - p), " %02X",
					                   c64_->RAM[0x0100 | slot]);
				}
			}

			const uint16_t * from_pc = nullptr;
			if (watch_pc_hist_count_ > 0) {
				const size_t last =
					(watch_pc_hist_next_ + kWatchPcHist - 1) % kWatchPcHist;
				from_pc = &watch_pc_hist_[last];
			}

			if (from_pc) {
				REVM_LOG_TIMED_M(mod, REVM_VERBOSE,
					"A=$%02X X=$%02X Y=$%02X from $%04X $01=$%02X "
					"L=%u H=%u C=%u SP=$%04X%s",
					s.a, s.x, s.y, *from_pc, s.pr_out,
					unsigned(s.pr_out & 1u), unsigned((s.pr_out >> 1) & 1u),
					unsigned((s.pr_out >> 2) & 1u), s.sp, stack_buf);
			} else {
				REVM_LOG_TIMED_M(mod, REVM_VERBOSE,
					"A=$%02X X=$%02X Y=$%02X from ? $01=$%02X "
					"L=%u H=%u C=%u SP=$%04X%s",
					s.a, s.x, s.y, s.pr_out, unsigned(s.pr_out & 1u),
					unsigned((s.pr_out >> 1) & 1u),
					unsigned((s.pr_out >> 2) & 1u), s.sp, stack_buf);
			}

			char hist[512];
			char * hp = hist;
			char * hend = hist + sizeof(hist);
			hp += std::snprintf(hp, size_t(hend - hp), "pc-hist (%zu):",
			                    watch_pc_hist_count_);
			if (watch_pc_hist_count_ == 0) {
				std::snprintf(hp, size_t(hend - hp), " (empty) -> hit $%04X",
				              pc);
			} else {
				const size_t oldest = (watch_pc_hist_count_ < kWatchPcHist)
					? 0
					: watch_pc_hist_next_;
				for (size_t h = 0; h < watch_pc_hist_count_ && hp < hend; ++h) {
					const size_t idx = (oldest + h) % kWatchPcHist;
					hp += std::snprintf(hp, size_t(hend - hp), " $%04X",
					                    watch_pc_hist_[idx]);
				}
				std::snprintf(hp, size_t(hend - hp), " -> hit $%04X", pc);
			}
			REVM_LOG_TIMED_M(mod, REVM_VERBOSE, "%s", hist);
		}

		// CLI --watch-main-pc (verbose) stops after first match.
		if (w.verbose)
			break;
	}

	// Record this fetch PC for later verbose hits / "from".
	watch_pc_hist_[watch_pc_hist_next_] = pc;
	watch_pc_hist_next_ = (watch_pc_hist_next_ + 1) % kWatchPcHist;
	if (watch_pc_hist_count_ < kWatchPcHist) ++watch_pc_hist_count_;
}

void Board::maybe_save_snapshot() {
	Board & src = snapshot_source_ ? *snapshot_source_ : *this;

	if (!save_snapshot_done_ && !cfg_.save_snapshot_path.empty() &&
	    cfg_.save_snapshot_cycle && CycleCounter() >= *cfg_.save_snapshot_cycle) {
		MachineSnapshot snap;
		if (!CaptureMachineSnapshot(src, snap)) {
			REVM_LOG(REVM_ERROR, "save-snapshot capture failed");
		} else {
			std::string err;
			if (!SaveMachineSnapshotFile(cfg_.save_snapshot_path, snap, err)) {
				REVM_LOG(REVM_ERROR, "save-snapshot failed: %s", err.c_str());
			} else {
				REVM_LOG(REVM_DEBUG, "snapshot saved → %s (cycle=%u frame=%u pc=$%04X)%s%s",
				             cfg_.save_snapshot_path.c_str(), snap.c64.cycle, snap.c64.frame,
				             snap.c64.cpu.pc, snapshot_source_ ? " [twin]" : "",
				             snap.has_drive ? " [drive]" : "");
			}
		}
		save_snapshot_done_ = true;
	}

	if (!add_play_snapshot_done_ && cfg_.add_play_snapshot_cycle &&
	    !cfg_.play_path.empty() &&
	    CycleCounter() >= *cfg_.add_play_snapshot_cycle) {
		MachineSnapshot snap;
		if (!CaptureMachineSnapshot(src, snap)) {
			REVM_LOG(REVM_ERROR, "add-play-snapshot capture failed");
		} else {
			const std::string hex = Sha256MachineSnapshot(snap);
			fs::path play_p(cfg_.play_path);
			const std::string stem = play_p.stem().string();
			fs::path dir = play_p.parent_path() / (stem + ".snaps");
			std::error_code ec;
			fs::create_directories(dir, ec);
			const std::string fname = std::to_string(snap.c64.cycle) + ".bin";
			const fs::path abs = dir / fname;
			const std::string rel =
				(fs::path(stem + ".snaps") / fname).generic_string();
			std::string err;
			if (!SaveMachineSnapshotFile(abs.string(), snap, err)) {
				REVM_LOG(REVM_ERROR, "add-play-snapshot write failed: %s",
				             err.c_str());
			} else if (!RegisterPlaySnapshot(cfg_.play_path, snap.c64.cycle, snap.c64.frame,
			                                 rel, hex, err)) {
				REVM_LOG(REVM_ERROR, "add-play-snapshot register failed: %s",
				             err.c_str());
			} else {
				if (play_player_) {
					play_player_->UpdateSnapshotHash(snap.c64.cycle, snap.c64.frame, hex, rel);
				}
				REVM_LOG(REVM_DEBUG, "play snapshot registered cycle %u frame %u → %s (%s)%s",
				             snap.c64.cycle, snap.c64.frame, rel.c_str(), hex.c_str(),
				             snapshot_source_ ? " [twin]" : "");
			}
		}
		add_play_snapshot_done_ = true;
	}

	snapshot_capture_pending_ =
		(!save_snapshot_done_ && !cfg_.save_snapshot_path.empty() &&
		 cfg_.save_snapshot_cycle) ||
		(!add_play_snapshot_done_ && !cfg_.play_path.empty() &&
		 cfg_.add_play_snapshot_cycle);
}

void Board::maybe_save_screen() {
	if (save_screen_done_ || secondary_ || cfg_.save_screen_path.empty() ||
	    !cfg_.save_screen_frame) {
		return;
	}
	const uint32_t frame = FrameCounter();
	if (frame != *cfg_.save_screen_frame) return;
	save_screen_done_ = true;

	ScreenSnapshot screen{};
	std::string err;
	if (!CaptureScreenSnapshot(*this, screen) ||
	    !WriteScreenPng(cfg_.save_screen_path, screen, err)) {
		REVM_LOG(REVM_ERROR, "save-screen failed: %s",
		         err.empty() ? "capture failed" : err.c_str());
		RequestQuit(1);
		return;
	}
	REVM_LOG(REVM_INFO, "save-screen → %s (frame=%u)",
	         cfg_.save_screen_path.c_str(), frame);
	std::fflush(stderr);
}

void Board::save_live_screenshot() {
	const uint32_t frame = FrameCounter();
	const std::string path = "frame" + std::to_string(frame) + ".png";
	ScreenSnapshot screen{};
	std::string err;
	if (!CaptureScreenSnapshot(*this, screen) || !WriteScreenPng(path, screen, err)) {
		REVM_LOG(REVM_ERROR, "screenshot failed: %s",
		         err.empty() ? "capture failed" : err.c_str());
		return;
	}
	REVM_LOG(REVM_INFO, "screenshot → %s (frame=%u)", path.c_str(), frame);
	std::fflush(stderr);
}

void Board::poll_host_screenshot() {
	if (cfg_.headless || secondary_ || !c64_ || !c64_->TheDisplay) return;
	const Uint8 * keys = SDL_GetKeyboardState(nullptr);
	if (!keys) return;
	const bool chord = keys[SDL_SCANCODE_F9] &&
	                   (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]);
	if (chord && !screenshot_chord_was_down_)
		save_live_screenshot();
	screenshot_chord_was_down_ = chord;
}

void Board::maybe_check_play_hashes() {
	if (!play_player_) return;
	Board & hash_board = snapshot_source_ ? *snapshot_source_ : *this;
	if (play_player_->CheckHashes(hash_board)) {
		RequestQuit(1);
	}
}

void Board::RecordMemAccess(uint16_t adr, bool is_rom) {
	if (!coverage_active_) return;
	if (cfg_.begin_cycle_count) {
		const uint64_t begin = *cfg_.begin_cycle_count;
		const uint64_t early = begin > 16 ? begin - 16 : 0;
		if (CycleCounter() < early) return;
	}
	if (is_rom) {
		coverage_.MarkMemRom(adr);
	} else {
		coverage_.MarkMemRam(adr);
	}
}

void Board::PcFetchThunk(uint16_t pc, void * userdata) {
	auto * self = static_cast<Board *>(userdata);
	if (self->coverage_active_) {
		bool mark = true;
		if (self->cfg_.begin_cycle_count) {
			const uint64_t begin = *self->cfg_.begin_cycle_count;
			const uint64_t early = begin > 16 ? begin - 16 : 0;
			if (self->CycleCounter() < early) mark = false;
		}
		if (mark && self->c64_ && self->c64_->TheCPU) {
			const auto space = self->c64_->TheCPU->ClassifyRead(pc);
			if (space == MOS6510::CoverSpace::Rom) {
				self->coverage_.MarkPcRom(pc);
			} else {
				// Ram (or unexpected Io on a fetch) → pc_ram
				self->coverage_.MarkPcRam(pc);
			}
		}
	}
	if (!self->trace_calls_) return;
	if (self->cfg_.begin_cycle_count &&
	    self->CycleCounter() < *self->cfg_.begin_cycle_count) {
		return;
	}
	for (const auto & o : self->kb_.Objects()) {
		if (!o.trace || o.name.empty()) continue;
		if (o.addr != pc) continue;
		REVM_LOG_TIMED(REVM_VERBOSE, "call %s ($%04X)", o.name.c_str(), pc);
		self->last_trace_.name = o.name;
		self->last_trace_.addr = pc;
		self->last_trace_.cycle = self->CycleCounter();
		self->last_trace_.frame = self->FrameCounter();
		self->last_trace_.valid = true;
		break;
	}
}

void Board::flush_coverage() {
	if (!coverage_active_ || cfg_.coverage_path.empty()) return;
	std::string err;
	if (!coverage_.Save(cfg_.coverage_path, err)) {
		REVM_LOG(REVM_ERROR, "coverage save failed: %s", err.c_str());
	} else {
		REVM_LOG(REVM_DEBUG, "coverage → %s  (pc_ram=%zu/%llu pc_rom=%zu/%llu "
		             "mem_ram=%zu/%llu mem_rom=%zu/%llu)",
		             cfg_.coverage_path.c_str(),
		             coverage_.Count(Coverage::Plane::PcRam),
		             (unsigned long long)coverage_.TotalHits(Coverage::Plane::PcRam),
		             coverage_.Count(Coverage::Plane::PcRom),
		             (unsigned long long)coverage_.TotalHits(Coverage::Plane::PcRom),
		             coverage_.Count(Coverage::Plane::MemRam),
		             (unsigned long long)coverage_.TotalHits(Coverage::Plane::MemRam),
		             coverage_.Count(Coverage::Plane::MemRom),
		             (unsigned long long)coverage_.TotalHits(Coverage::Plane::MemRom));
	}
	coverage_active_ = false; // avoid double-save from destructor after Run()
	if (c64_ && c64_->TheCPU) {
		c64_->TheCPU->SetMemAccessHook(nullptr, nullptr);
	}
}

void Board::check_max_cycles() {
	if (!cfg_.max_cycles) return;
	if (CycleCounter() >= *cfg_.max_cycles) {
		REVM_LOG(REVM_DEBUG, "max-cycles reached: cycle=%u frame=%u",
		             CycleCounter(), FrameCounter());
		RequestQuit(0);
	}
}

void Board::check_max_seconds() {
	if (!wall_deadline_) return;
	// Cheap throttle: chrono every 64k cycles (~3ms @ 1MHz equiv).
	if ((++wall_check_counter_ & 0xffffu) != 0) return;
	if (std::chrono::steady_clock::now() >= *wall_deadline_) {
		const double lim = cfg_.max_seconds ? *cfg_.max_seconds : 0.0;
		REVM_LOG(REVM_DEBUG, "max-seconds reached (%.3fs wall): cycle=%u frame=%u",
		             lim, CycleCounter(), FrameCounter());
		RequestQuit(124);
	}
}

void Board::StopWallClockLimit() {
	supervisor_stop_.store(true, std::memory_order_release);
	supervisor_wake_.notify_all();
	if (supervisor_.joinable())
		supervisor_.join();
	const int supervisor_code =
		supervisor_quit_code_.exchange(0, std::memory_order_acq_rel);
	if (supervisor_code != 0)
		RequestQuit(supervisor_code);
	wall_deadline_.reset();
	wall_check_counter_ = 0;
}

void Board::StartWallClockLimit() {
	StopWallClockLimit();
	using steady = std::chrono::steady_clock;
	constexpr auto kPoll = std::chrono::seconds(1);
	constexpr auto kGrace = std::chrono::seconds(2);

	if (cfg_.max_seconds && *cfg_.max_seconds > 0.0) {
		const double secs = *cfg_.max_seconds;
		// Keep this as an absolute deadline.  The supervisor may be busy with a
		// liveness poll, but must not turn --max-seconds into a moving timeout.
		wall_deadline_ = steady::now() +
		                 std::chrono::duration_cast<steady::duration>(
		                     std::chrono::duration<double>(secs));
		REVM_LOG(REVM_DEBUG, "Will stop after %.3fs wall-clock (--max-seconds)",
		         secs);
	}

	const auto supervisor_start = steady::now();
	supervised_thread_ = ::pthread_self();
	const uint64_t initial_generation =
		vblank_generation_.load(std::memory_order_acquire);
	supervisor_quit_code_.store(0, std::memory_order_release);
	supervisor_stop_.store(false, std::memory_order_release);
	supervisor_ = std::thread([this, supervisor_start, initial_generation] {
		using steady = std::chrono::steady_clock;
		constexpr auto kPoll = std::chrono::seconds(1);
		constexpr auto kGrace = std::chrono::seconds(2);

		uint64_t observed_generation = initial_generation;
		auto next_poll = supervisor_start + kPoll;
		auto previous_wake = supervisor_start;
		std::optional<steady::time_point> hard_deadline;

		while (!supervisor_stop_.load(std::memory_order_acquire)) {
			auto wake = next_poll;
			if (wall_deadline_ && *wall_deadline_ < wake)
				wake = *wall_deadline_;
			if (hard_deadline && *hard_deadline < wake)
				wake = *hard_deadline;
			if (steady::now() < wake) {
				std::unique_lock lock(supervisor_mutex_);
				supervisor_wake_.wait_until(lock, wake, [this] {
					return supervisor_stop_.load(std::memory_order_acquire);
				});
			}
			if (supervisor_stop_.load(std::memory_order_acquire)) return;

			const auto now = steady::now();
			const bool process_was_suspended = now - previous_wake > 2 * kPoll;
			previous_wake = now;

			if (!hard_deadline && wall_deadline_ && now >= *wall_deadline_) {
				// Cross-thread access is atomics-only. The emulation thread consumes
				// this in finish_cycle(); a mock C++ loop which never returns reaches
				// the hard deadline below.
				supervisor_quit_code_.store(124, std::memory_order_release);
				supervisor_diagnostic(
					"--max-seconds elapsed; cooperative quit requested",
					supervisor_last_vblank_frame_.load(std::memory_order_acquire),
					supervisor_last_vblank_cycle_.load(std::memory_order_acquire),
					vblank_generation_.load(std::memory_order_acquire));
				hard_deadline = now + kGrace;
			}

			if (hard_deadline && now >= *hard_deadline) {
				supervisor_hard_abort(
					supervised_thread_,
					"--max-seconds soft quit was ignored for two seconds",
					supervisor_last_vblank_frame_.load(std::memory_order_acquire),
					supervisor_last_vblank_cycle_.load(std::memory_order_acquire),
					vblank_generation_.load(std::memory_order_acquire));
			}

			if (now < next_poll) continue;
			next_poll = now + kPoll;

			// SIGSTOP and debugger pauses suspend all threads, including this
			// supervisor.  Do not mistake the resulting long wake-up gap for a
			// dead emulation; the absolute max-seconds deadline remains active.
			if (process_was_suspended) {
				observed_generation =
					vblank_generation_.load(std::memory_order_acquire);
				continue;
			}

			const uint64_t generation =
				vblank_generation_.load(std::memory_order_acquire);
			if (!paused_.load(std::memory_order_acquire) &&
			    generation == observed_generation) {
				supervisor_hard_abort(
					supervised_thread_, "no Main VBlank for one second",
					supervisor_last_vblank_frame_.load(std::memory_order_acquire),
					supervisor_last_vblank_cycle_.load(std::memory_order_acquire),
					generation);
			}
			observed_generation = generation;
		}
	});
}

void Board::sync_clock_from_machine() {
	if (!c64_) return;
	clock_.Set(c64_->CycleCounter(), c64_->FrameCounter());
}

void Board::apply_input(const InputFrame & in) {
	if (!c64_) return;
	uint8_t j1 = in.joy1.ToCiaMask();
	uint8_t j2 = in.joy2.ToCiaMask();
	if (ThePrefs.JoystickSwap) {
		std::swap(j1, j2);
	}
	c64_->TheCIA1->Joystick1 = j1;
	c64_->TheCIA1->Joystick2 = j2;
	std::memcpy(c64_->TheCIA1->KeyMatrix, in.keyboard.matrix, 8);
	std::memcpy(c64_->TheCIA1->RevMatrix, in.keyboard.rev_matrix, 8);
}

void Board::ApplyMemoryModification(const MemoryModification & modification) {
	if (mod_handler_ && mod_handler_(modification))
		return;
	if (!c64_ || !c64_->TheCPU) return;
	const uint8_t old_value = c64_->TheCPU->REUReadByte(modification.address);
	const uint8_t new_value = modification.operation == MemoryModOperation::Increment
	                            ? uint8_t(old_value + 1)
	                            : uint8_t(old_value - 1);
	c64_->TheCPU->REUWriteByte(modification.address, new_value);
	REVM_LOG(REVM_DEBUG, "cheat $%04X: $%02X -> $%02X (%s)",
	         modification.address, old_value, new_value,
	         modification.operation == MemoryModOperation::Increment ? "increment"
	                                                                 : "decrement");
}

void Board::apply_memory_modifications(
	const std::vector<MemoryModification> & modifications) {
	for (const auto & modification : modifications) {
		ApplyMemoryModification(modification);
		if (mod_mirror_) mod_mirror_->ApplyMemoryModification(modification);
	}
}

void Board::poll_live_controls(LiveInput & live, InputFrame & in) {
	if (live.QuitRequested()) {
		live.ClearQuitRequest();
		RequestQuit(0);
	}
	if (live.PauseToggleRequested()) {
		paused_ = !paused_;
		live.ClearPauseToggleRequest();
		REVM_LOG(REVM_INFO, "%s", paused_ ? "paused (F10 to resume)" : "resumed");
	}
	if (live.JoystickSwapToggled()) {
		if (live.JoysticksSwapped()) {
			REVM_LOG(REVM_INFO,
			         "Ctrl+Tab: joystick ports swapped (Joy 1 -> port 2, Joy 2 -> port 1)");
		} else {
			REVM_LOG(REVM_INFO,
			         "Ctrl+Tab: joystick ports restored (Joy 1 -> port 1, Joy 2 -> port 2)");
		}
		live.ClearJoystickSwapToggle();
	}
	if (!kb_watch_window_) return;
	const auto & cheats = live.GetJoystickConfig().cheats;
	kb_watch_window_->PollCheatKeys(cheats.increment, cheats.decrement);
	if (kb_watch_window_->ConsumesCheatKeys()) live.SuppressCheatKeys(in);
}

void Board::wait_while_paused() {
	auto * live = dynamic_cast<LiveInput *>(input_);
	if (!live) {
		paused_ = false;
		return;
	}
	while (paused_ && !quit_ && !c64_->QuitRequested()) {
		InputFrame ignored = live->PollFrame(c64_->FrameCounter());
		poll_live_controls(*live, ignored);
		if (kb_watch_window_ && kb_watch_window_->IsOpen()) {
			kb_watch_window_->OnFrame(kb_watch_, c64_->FrameCounter());
		}
		poll_host_screenshot();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

void Board::on_frame_boundary() {
	const uint32_t frame = c64_->FrameCounter();
	const uint32_t cycle = c64_->CycleCounter();

	InputFrame in;
	if (input_) {
		in = input_->PollFrame(frame);
		if (auto * live = dynamic_cast<LiveInput *>(input_)) {
			poll_live_controls(*live, in);
		}
		apply_input(in);
	}

	if (quiet_vblank_) {
		c64_->TheCIA1->CountTOD();
		c64_->TheCIA2->CountTOD();
		// Quiet = nested EmulateCycle from micro-runs / Twin secondary.
		// Do NOT golden-compare / frame hook / display here.
		return;
	}

	// LiveInput F9 → BEGIN_CYCLE_COUNT + play timestamps
	if (auto * live = dynamic_cast<LiveInput *>(input_)) {
		if (live->BeginCycleRequested()) {
			REVM_LOG(REVM_INFO, "F9: timestamp frame=%u cycle=%u", frame, cycle);
			MarkBeginCycle();
			live->ClearBeginCycleRequest();
			if (play_recorder_) {
				play_recorder_->AddTimestamp(frame, cycle);
			}
		}
	}

	c64_->TheCIA1->CountTOD();
	c64_->TheCIA2->CountTOD();

	// Stage-3: CpuMockHost winds Twin to this VSYNC + ApplyInput (bookkeeping).
	if (frame_boundary_hook_) {
		frame_boundary_hook_(frame, cycle, in);
	}

	std::vector<MemoryModification> modifications;
	if (play_player_) {
		const auto & replayed = play_player_->ModificationsAt(frame);
		modifications.insert(modifications.end(), replayed.begin(), replayed.end());
	}
	if (kb_watch_window_ && kb_watch_window_->IsOpen()) {
		auto live = kb_watch_window_->TakePendingModifications();
		modifications.insert(modifications.end(), live.begin(), live.end());
	}
	apply_memory_modifications(modifications);

	if (play_recorder_) {
		play_recorder_->OnFrame(frame, in, modifications);
	}
	if (play_player_) {
		if (play_player_->OnFrame(frame, cycle, *this)) {
			RequestQuit(1);
		}
	}
	if (kb_watch_.Active()) {
		kb_watch_.OnFrame(*this, frame, cycle);
	}
	if (kb_watch_window_ && kb_watch_window_->IsOpen()) {
		kb_watch_window_->OnFrame(kb_watch_, frame);
	}
	if (post_compare_hook_) {
		post_compare_hook_(frame, cycle);
	}

	arm_audio_if_needed();
	capture_video_frame();
	maybe_save_screen();
	if (frame_observations_ || frame_observer_) {
		FrameSnapshot snap;
		if (!CaptureFrameSnapshot(*this, snap)) {
			REVM_LOG(REVM_ERROR, "failed to capture frame snapshot");
			RequestQuit(1);
		} else {
			snap.ordinal = vsync_ordinal_++;
			if (frame_observer_)
				frame_observer_->onFrame(snap);
			if (frame_observations_) {
				std::string observation_error;
				if (!frame_observations_->WriteFrame(snap, observation_error)) {
					REVM_LOG(REVM_ERROR, "%s", observation_error.c_str());
					RequestQuit(1);
				}
			}
		}
	}

	if (!cfg_.headless && c64_->TheDisplay) {
		// LiveInput drains SDL via PollKeyboard; golden/play replay does not —
		// still pump events so macOS doesn't beach-ball the window.
		if (!dynamic_cast<LiveInput *>(input_)) {
			pump_host_events();
		}
		c64_->TheDisplay->Update();
	}
	poll_host_screenshot();

	if (frame_cb_) {
		frame_cb_(frame, cycle);
	}

	if (c64_->QuitRequested()) {
		quit_ = true;
	}
	if (paused_ && !quit_) wait_while_paused();
}

void Board::pump_host_events() {
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
			case SDL_QUIT:
				RequestQuit(0);
				break;
			case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
					RequestQuit(0);
				}
				break;
			case SDL_KEYDOWN:
				// Replay has no LiveInput; keep host quit shortcuts active.
				if ((e.key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
				     e.key.keysym.scancode == SDL_SCANCODE_F12) &&
				    !e.key.repeat) {
					RequestQuit(0);
				}
				break;
			default:
				break;
		}
	}
}

void Board::MarkBeginCycle() {
	cfg_.begin_cycle_count = CycleCounter();
	REVM_LOG(REVM_DEBUG, "timestamp at cycle %llu frame %u",
	             static_cast<unsigned long long>(*cfg_.begin_cycle_count),
	             FrameCounter());
}

void Board::RequestQuit(int exit_code) {
	// A later orderly stop must not erase an error already reported during the
	// same frame. Allow a failure to upgrade an earlier successful stop, then
	// preserve the first non-zero reason.
	if (!quit_ || (exit_code_ == 0 && exit_code != 0)) {
		exit_code_ = exit_code;
	}
	quit_ = true;
	if (c64_) {
		c64_->RequestQuit(exit_code_);
	}
	if (shutdown_hook_) {
		auto hook = shutdown_hook_;
		shutdown_hook_ = nullptr;
		hook();
	}
}

bool Board::LoadBeginSnap(const std::string & path, GoldenInput * input,
                          std::string & error) {
	if (!c64_) {
		error = "Board not initialized";
		return false;
	}

	MachineSnapshot snap;
	if (!LoadMachineSnapshotFile(path, snap, error)) return false;
	if (!RestoreMachineSnapshot(*this, snap)) {
		error = "RestoreFullSnapshot failed";
		return false;
	}
	sync_clock_from_machine();

	if (input) {
		input->SeekTo(snap.c64.frame);
		apply_input(input->PollFrame(snap.c64.frame));
	} else {
		// Live record: CIA KeyMatrix/joysticks are host-side (not in the POD).
		// Always apply a neutral frame so record matches golden playback.
		apply_input(InputFrame{});
	}

	cfg_.begin_snap_path = path;
	if (!cfg_.begin_cycle_count) {
		cfg_.begin_cycle_count = snap.c64.cycle;
	} else if (*cfg_.begin_cycle_count != snap.c64.cycle) {
		REVM_LOG(REVM_ERROR, "Warning: begin snap cycle %u != manifest begin_cycle_count %llu",
			snap.c64.cycle,
			static_cast<unsigned long long>(*cfg_.begin_cycle_count));
	}

	keep_state_on_run_ = true;
	REVM_LOG(REVM_DEBUG, "Restored snapshot %s — cycle=%u frame=%u pc=$%04X (boot skipped)%s%s",
		path.c_str(), snap.c64.cycle, snap.c64.frame, snap.c64.cpu.pc,
		snap.c64.version < 2 ? " [v1: no VIC SC pipeline — re-dump recommended]" : "",
		snap.has_drive ? " [drive]" : "");
	return true;
}

uint32_t Board::CycleCounter() const {
	return c64_ ? c64_->CycleCounter() : clock_.Cycle();
}

uint32_t Board::FrameCounter() const {
	return c64_ ? c64_->FrameCounter() : clock_.Frame();
}

void Board::note_vblank() {
	const uint32_t c = CycleCounter();
	const uint32_t frame = FrameCounter();
	if (have_last_vblank_)
		vblank_period_ = c - last_vblank_cycle_;
	last_vblank_cycle_ = c;
	have_last_vblank_ = true;
	supervisor_last_vblank_frame_.store(frame, std::memory_order_relaxed);
	supervisor_last_vblank_cycle_.store(c, std::memory_order_relaxed);
	vblank_generation_.fetch_add(1, std::memory_order_release);
}

int Board::Run() {
	if (!c64_) return 1;
	paused_ = false;

	if (keep_state_on_run_) {
		// Already restored from begin snap — do not wipe chips/counters.
		quit_ = false;
		exit_code_ = 0;
		keep_state_on_run_ = false;
	} else {
		PrepareRun();
	}
	if (play_recorder_) {
		play_recorder_->CaptureStartHash();
	}
	StartWallClockLimit();
	// Verify any snapshot hash at the current cycle before the first tick
	// (covers start cycle 0 / begin-snap cycle).
	maybe_check_play_hashes();

	using steady = std::chrono::steady_clock;
	auto frame_start = steady::now();
	constexpr int FRAME_TIME_us = 1000000 / SCREEN_FREQ;

	while (!quit_ && !c64_->QuitRequested()) {
		bool vblank = EmulateCycle();

		if (vblank && cfg_.limit_speed) {
			if (cfg_.disk_warp && drive_busy_for_warp(c64_)) {
				frame_start = steady::now();
			} else {
				frame_start += std::chrono::microseconds(FRAME_TIME_us);
				auto now = steady::now();
				if (frame_start > now) {
					std::this_thread::sleep_until(frame_start);
				} else if (now - frame_start > std::chrono::milliseconds(100)) {
					frame_start = now;
				}
			}
		}
	}

	StopWallClockLimit();
	flush_coverage();
	{
		std::string media_err;
		if (!FinishMedia(media_err) && !media_err.empty()) {
			REVM_LOG(REVM_ERROR, "%s", media_err.c_str());
			if (exit_code_ == 0) exit_code_ = 1;
		}
	}
	return exit_code_;
}

void Board::arm_audio_if_needed() {
	if (audio_armed_ || secondary_ || !c64_) return;
	const bool want_wav = !cfg_.save_audio_path.empty();
	const bool want_video = !cfg_.save_video_path.empty();
	if (!want_wav && !want_video) return;
	if (cfg_.no_audio || !c64_->TheSID) return;

	media_wav_path_ = cfg_.save_audio_path;
	media_wav_is_temp_ = false;
	if (media_wav_path_.empty()) {
		media_wav_path_ = cfg_.save_video_path + ".sid.wav";
		media_wav_is_temp_ = true;
	}

	std::string err;
	const int rate = c64_->TheSID->GetSampleRate();
	if (!wav_.Open(media_wav_path_, rate > 0 ? rate : 48000, err)) {
		REVM_LOG(REVM_ERROR, "save-audio open failed: %s", err.c_str());
		return;
	}
	c64_->TheSID->SetSampleTap(&WavWriter::SidTap, &wav_);
	wav_.SetArmed(true);
	audio_armed_ = true;
	REVM_LOG(REVM_DEBUG, "save-audio → %s (%d Hz)", media_wav_path_.c_str(),
	             wav_.SampleRate());
}

void Board::capture_video_frame() {
	if (cfg_.save_video_path.empty() || secondary_) return;
	ScreenSnapshot screen{};
	if (!CaptureScreenSnapshot(*this, screen)) return;
	// Pepto palette (Frodo default)
	static constexpr uint8_t kR[16] = {
		0x00, 0xff, 0x86, 0x4c, 0x88, 0x35, 0x20, 0xcf, 0x88, 0x40, 0xcb, 0x34, 0x68, 0x8b, 0x68, 0xa1};
	static constexpr uint8_t kG[16] = {
		0x00, 0xff, 0x19, 0xc1, 0x17, 0xac, 0x07, 0xf2, 0x3e, 0x2a, 0x55, 0x34, 0x68, 0xff, 0x4a, 0xa1};
	static constexpr uint8_t kB[16] = {
		0x00, 0xff, 0x01, 0xe3, 0xbd, 0x0a, 0xc0, 0x2d, 0x00, 0x00, 0x37, 0x34, 0x68, 0x59, 0xff, 0xa1};
	const size_t off = video_rgb_.size();
	video_rgb_.resize(off + ScreenSnapshot::kBytes * 3);
	for (size_t i = 0; i < ScreenSnapshot::kBytes; ++i) {
		const uint8_t c = screen.pixels[i] & 0x0f;
		video_rgb_[off + i * 3 + 0] = kR[c];
		video_rgb_[off + i * 3 + 1] = kG[c];
		video_rgb_[off + i * 3 + 2] = kB[c];
	}
	++video_frames_;
}

bool Board::FinishMedia(std::string & error) {
	error.clear();
	if (audio_armed_ && c64_ && c64_->TheSID) {
		c64_->TheSID->SetSampleTap(nullptr, nullptr);
		wav_.SetArmed(false);
		std::string err;
		if (!wav_.Close(err)) {
			error = "save-audio close failed: " + err;
			return false;
		}
		audio_armed_ = false;
	}
	const uint64_t wav_samples = wav_.SampleCount();
	const bool want_audio_file = !cfg_.save_audio_path.empty();
	const bool want_video = !cfg_.save_video_path.empty();
	if (want_audio_file || want_video) {
		REVM_LOG(REVM_DEBUG, "save-audio closed (%llu samples)",
		             static_cast<unsigned long long>(wav_samples));
	}
	if ((want_audio_file || want_video) && wav_samples == 0) {
		if (media_wav_is_temp_ && !media_wav_path_.empty()) {
			std::error_code ec;
			fs::remove(media_wav_path_, ec);
		}
		error = want_video
			? "save-video: no SID samples captured"
			: "save-audio: no SID samples captured";
		return false;
	}
	if (want_video) {
		if (video_frames_ == 0) {
			if (media_wav_is_temp_ && !media_wav_path_.empty()) {
				std::error_code ec;
				fs::remove(media_wav_path_, ec);
			}
			error = "save-video: no frames captured";
			return false;
		}
		const unsigned w = ScreenSnapshot::kWidth;
		const unsigned h = ScreenSnapshot::kHeight;
		const std::string cmd =
			"ffmpeg -y -nostdin -hide_banner -loglevel error -f rawvideo -pix_fmt rgb24 -s " +
			std::to_string(w) + "x" + std::to_string(h) + " -r " +
			std::to_string(SCREEN_FREQ) +
			" -i - -i \"" + media_wav_path_ +
			"\" -vf scale=iw*2:ih*2:flags=neighbor -c:v libx264 -pix_fmt yuv420p "
			"-c:a aac -b:a 192k -shortest \"" +
			cfg_.save_video_path + "\"";
		REVM_LOG(REVM_INFO, "save-video: encoding %u frames (2× %ux%u) with ffmpeg…",
		             video_frames_, w, h);
		std::fflush(stderr);
		FILE * pipe = popen(cmd.c_str(), "w");
		if (!pipe) {
			error = "save-video: failed to spawn ffmpeg";
			return false;
		}
		const size_t n = fwrite(video_rgb_.data(), 1, video_rgb_.size(), pipe);
		const int rc = pclose(pipe);
		if (n != video_rgb_.size() || rc != 0) {
			error = "save-video: ffmpeg failed (is ffmpeg installed?)";
			if (media_wav_is_temp_ && !media_wav_path_.empty()) {
				std::error_code ec;
				fs::remove(media_wav_path_, ec);
			}
			return false;
		}
		REVM_LOG(REVM_INFO, "save-video → %s (%u frames, %llu SID samples)",
		             cfg_.save_video_path.c_str(), video_frames_,
		             static_cast<unsigned long long>(wav_samples));
		video_rgb_.clear();
		video_frames_ = 0;
	}
	if (media_wav_is_temp_ && !media_wav_path_.empty()) {
		std::error_code ec;
		fs::remove(media_wav_path_, ec);
		media_wav_path_.clear();
		media_wav_is_temp_ = false;
	}
	if (!cfg_.save_screen_path.empty() && !save_screen_done_) {
		error = "save-screen: frame " +
		        std::to_string(*cfg_.save_screen_frame) +
		        " never reached (ended at frame " +
		        std::to_string(FrameCounter()) + ")";
		return false;
	}
	return true;
}

void Board::PrepareRun() {
	if (!c64_) return;
	quit_ = false;
	exit_code_ = 0;

	c64_->ResetCounters();
	clock_.Reset();

	c64_->TheCPU->Reset();
	c64_->TheSID->Reset();
	c64_->TheCIA1->Reset();
	c64_->TheCIA2->Reset();
	c64_->TheCPU1541->Reset();
	c64_->TheGCRDisk->Reset();
	c64_->TheTape->Reset();
}

} // namespace revm
