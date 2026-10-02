// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace revm {

enum class Stage {
	Stage1,
	Stage2,
	Stage3,
};

enum class RunMode {
	Play,
	Record,
	Playback,
};

struct Config {
	Stage stage = Stage::Stage1;
	RunMode run_mode = RunMode::Play;

	std::string rom_dir;
	std::string prg_path;
	// --load-d64: mount drive 8 (1541 processor + GCR). Host file is a seed.
	std::string d64_path;
	bool disk_auto_load = false; // --disk-auto-load: LOAD"*",8,1 then RUN
	// --disk-warp: skip 50 Hz pacing while the 1541 CPU is running.
	bool disk_warp = false;
	// Snapshot file carries a 1541/GCR trailer; enable the drive at Init.
	bool drive_from_snapshot = false;

	bool limit_speed = true;
	bool audio_enabled = true;
	bool no_audio = false;   // --no-audio: force SIDTYPE_NONE (wins over --resid)
	bool sid_resid = false;  // --resid: use reSID 6581 renderer on Main
	bool headless = false;
	// --main-blank: CpuMock Main starts from fresh chips and zero memory instead
	// of restoring BEGIN. Twin still restores BEGIN as the oracle.
	bool main_blank = false;

	std::optional<uint64_t> begin_cycle_count;

	// --watch-main-pc ADDR[,ADDR…]  (hex); empty = disabled
	std::vector<uint16_t> watch_main_pcs;
	std::optional<uint64_t> max_cycles;
	// Wall-clock limit (seconds). Soft quit via RequestQuit, then hard _Exit
	// if the process is stuck in a C++ loop that never polls quit.
	std::optional<double> max_seconds;

	std::string kb_path;
	bool kb_watch = false;
	bool kb_watch_verbose = false;
	bool kb_watch_window = false;
	std::string kb_watch_font = "12x24";
	bool kb_trace_calls = false;
	bool kb_check = false;
	bool kb_check_verbose = false;

	std::string coverage_path;
	std::string watch_timeline_path; // --watch-timeline FILE (JSONL)

	// --save-snapshot CYCLE FILE
	std::optional<uint64_t> save_snapshot_cycle;
	std::string save_snapshot_path;

	// --add-play-snapshot CYCLE (requires --use-play path)
	std::optional<uint64_t> add_play_snapshot_cycle;
	std::string play_path; // --use-play / --record-play path for registration

	// --save-audio / --save-video / --save-screen (Main board)
	std::string save_audio_path;
	std::string save_video_path;
	// --save-screen FRAME FILE: Pepto P6 PPM at VBLANK FRAME (same counter
	// as --max-frames). Unset = disabled.
	std::optional<uint32_t> save_screen_frame;
	std::string save_screen_path;
	// --frame-observations FILE: per-VSYNC indexed pixels + public SID bytes.
	std::string frame_observations_path;

	std::string begin_snap_path;
	unsigned rand_seed = 42;

	// --log-verbose CYCLE: emit Verbose (and Debug) once Main CycleCounter >= CYCLE.
	// Unset = Verbose suppressed for the whole run.
	std::optional<uint32_t> log_verbose_from_cycle;
	// --log-debug CYCLE: emit Debug logs once Main CycleCounter >= CYCLE.
	// Unset = Debug off unless --log-verbose is set (verbose implies debug).
	std::optional<uint32_t> log_debug_from_cycle;

	// --events FILE: structured JSONL event stream ("-" = stderr lines).
	// Empty = disabled (zero cost).
	std::string events_path;
	// --report FILE: aggregated end-of-run summary of the event stream.
	std::string report_path;
};

} // namespace revm
