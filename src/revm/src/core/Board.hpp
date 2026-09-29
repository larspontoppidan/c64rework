// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "core/Clock.hpp"
#include "core/Config.hpp"
#include "core/RunSetup.hpp"
#include "roms/Roms.hpp"
#include "debug/Coverage.hpp"
#include "debug/KbWatch.hpp"
#include "debug/KbWatchWindow.hpp"
#include "debug/KnowledgeBase.hpp"
#include "goldens/FrameObservation.hpp"
#include "goldens/WavWriter.hpp"
#include "input/IInputSource.hpp"
#include "input/InputState.hpp"
#include "input/MemoryModification.hpp"

#include <functional>
#include <memory>
#include <cstdio>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include <pthread.h>

class C64;
class Prefs;

namespace revm {

class GoldenInput;
class LiveInput;
class PlayRecorder;
class PlayPlayer;
class Board {
public:
	Board();
	~Board();

	Board(const Board &) = delete;
	Board & operator=(const Board &) = delete;

	void Configure(const Config & cfg);
	const Config & GetConfig() const { return cfg_; }
	const RomHashes & LoadedRoms() const { return rom_hashes_; }

	bool Init(std::string & error);
	bool InitSecondary(std::string & error);
	bool InitAs(BoardInitRole role, std::string & error);
	BoardInitRole InitRole() const { return init_role_; }

	void ApplyInput(const InputFrame & in);
	void ApplyMemoryModification(const MemoryModification & modification);
	void SetMemoryModificationMirror(Board * mirror) { mod_mirror_ = mirror; }
	using MemoryModificationHandler =
		std::function<bool(const MemoryModification & modification)>;
	void SetMemoryModificationHandler(MemoryModificationHandler handler) {
		mod_handler_ = std::move(handler);
	}

	void Reset(bool clear_memory = false);

	bool LoadPrg(const std::string & path, std::string & error);
	void RequestAutostart();

	bool EmulateCycle();
	// VIC+CIA as normal, then `cpu_slot` instead of TheCPU->EmulateCycle,
	// then tape and ++cycle_counter. Same Board VBLANK/snapshot post-path.
	bool EmulateCycleInjectCpu(const std::function<void()> & cpu_slot);
	int Run();
	void PrepareRun();

	void RequestQuit(int exit_code = 0);
	int ExitCode() const { return exit_code_; }

	uint32_t CycleCounter() const;
	uint32_t FrameCounter() const;

	// CycleCounter at the most recent VBlank (incl. QuietVBlank). Unset until
	// the first VBlank after boot/restore — fine for debug Timing().
	uint32_t LastVBlankCycle() const { return last_vblank_cycle_; }
	// Gap between the last two VBlanks; kCyclesPerFrame until a second VBlank.
	uint32_t VBlankPeriod() const { return vblank_period_; }
	bool HasLastVBlank() const { return have_last_vblank_; }

	// Debug probe: log when this board is about to fetch an opcode at `pc`.
	// Same pc is idempotent (no second entry; a non-empty label refreshes).
	// Module is watch-main / watch-twin (from secondary_). verbose=true keeps
	// the CLI --watch-main-pc regs/stack/pc-hist dump.
	void WatchPc(uint16_t pc, std::string label = {}, bool verbose = false);
	void ClearPcWatches();
	// Total hit count across watches registered at pc (read-only probe).
	uint64_t WatchHits(uint16_t pc) const;

	C64 * Machine() { return c64_; }
	const C64 * Machine() const { return c64_; }

	void SetInputSource(IInputSource * src) { input_ = src; }
	IInputSource * GetInputSource() const { return input_; }

	void SetPlayRecorder(PlayRecorder * r) { play_recorder_ = r; }
	void SetPlayPlayer(PlayPlayer * p) { play_player_ = p; }
	PlayPlayer * GetPlayPlayer() const { return play_player_; }

	// When set, --save-snapshot / --add-play-snapshot capture this board
	// (Stage 3 Twin). Null = capture self (Main).
	void SetSnapshotSource(Board * src) { snapshot_source_ = src; }

	void MarkBeginCycle();
	void SyncClockFromMachine() { sync_clock_from_machine(); }

	// Arm/disarm the wall-clock supervisor for VSYNC liveness and
	// --max-seconds. Safe to call repeatedly.
	void StartWallClockLimit();
	void StopWallClockLimit();

	bool LoadBeginSnap(const std::string & path, GoldenInput * input,
	                   std::string & error);

	using FrameCallback = std::function<void(uint32_t frame, uint32_t cycle)>;
	void SetFrameCallback(FrameCallback cb) { frame_cb_ = std::move(cb); }
	void SetFrameObserver(FrameObserver * observer) { frame_observer_ = observer; }
	void SetShutdownHook(std::function<void()> hook) { shutdown_hook_ = std::move(hook); }

	void SetQuietVBlank(bool quiet) { quiet_vblank_ = quiet; }
	bool QuietVBlank() const { return quiet_vblank_; }
	// Main-only load: VBLANK without advancing frame_counter (Twin parked).
	void SetFreezeFrameCounter(bool freeze);

	using FrameBoundaryHook =
		std::function<void(uint32_t frame, uint32_t cycle, const InputFrame & in)>;
	void SetFrameBoundaryHook(FrameBoundaryHook cb) {
		frame_boundary_hook_ = std::move(cb);
	}

	using PostCompareHook =
		std::function<void(uint32_t frame, uint32_t cycle)>;
	void SetPostCompareHook(PostCompareHook cb) {
		post_compare_hook_ = std::move(cb);
	}

	const Coverage & GetCoverage() const { return coverage_; }
	Coverage & GetCoverage() { return coverage_; }
	void RecordMemAccess(uint16_t adr, bool is_rom);

	struct LastTrace {
		std::string name;
		uint16_t addr = 0;
		uint32_t cycle = 0;
		uint32_t frame = 0;
		bool valid = false;
	};
	const LastTrace & GetLastTrace() const { return last_trace_; }

	const KnowledgeBase & Kb() const { return kb_; }
	KnowledgeBase & Kb() { return kb_; }

	// Flush WAV / finalize video after Run().
	bool FinishMedia(std::string & error);

private:
	bool init_machine(std::string & error, BoardInitRole role);
	void on_frame_boundary();
	void note_vblank();
	void apply_input(const InputFrame & in);
	void apply_memory_modifications(const std::vector<MemoryModification> & modifications);
	void poll_live_controls(LiveInput & live, InputFrame & in);
	void wait_while_paused();
	void sync_clock_from_machine();
	bool finish_cycle(bool vblank);
	void check_watch_pc_before_cycle();
	void maybe_save_snapshot();
	void maybe_save_screen();
	void poll_host_screenshot();
	void save_live_screenshot();
	void maybe_check_play_hashes();
	void pump_host_events();
	void arm_audio_if_needed();
	void capture_video_frame();
	void flush_coverage();
	void check_max_cycles();
	void check_max_seconds();
	static void PcFetchThunk(uint16_t pc, void * userdata);

	Config cfg_;
	RomHashes rom_hashes_;
	C64 * c64_ = nullptr;
	IInputSource * input_ = nullptr;
	PlayRecorder * play_recorder_ = nullptr;
	PlayPlayer * play_player_ = nullptr;
	Board * snapshot_source_ = nullptr;
	Board * mod_mirror_ = nullptr;
	MemoryModificationHandler mod_handler_;
	FrameCallback frame_cb_;
	FrameBoundaryHook frame_boundary_hook_;
	PostCompareHook post_compare_hook_;
	std::function<void()> shutdown_hook_;

	Clock clock_;
	bool quit_ = false;
	int exit_code_ = 0;
	bool machine_owned_ = false;
	bool sdl_initialized_ = false;
	bool secondary_ = false;
	BoardInitRole init_role_ = BoardInitRole::Primary;
	uint32_t vsync_ordinal_ = 0;
	FrameObserver * frame_observer_ = nullptr;
	std::atomic<bool> paused_{false};
	uint32_t last_vblank_cycle_ = 0;
	uint32_t vblank_period_ = kCyclesPerFrame;
	bool have_last_vblank_ = false;

	struct PcWatch {
		uint16_t pc = 0;
		std::string label;
		uint64_t hits = 0;
		bool verbose = false;
	};
	std::vector<PcWatch> pc_watches_;
	// Ring of recent opcode-fetch PCs (while any watch is active).
	static constexpr size_t kWatchPcHist = 32;
	uint16_t watch_pc_hist_[kWatchPcHist]{};
	size_t watch_pc_hist_next_ = 0;  // next write index
	size_t watch_pc_hist_count_ = 0; // min(kWatchPcHist, pushes)
	bool quiet_vblank_ = false;

	std::optional<std::chrono::steady_clock::time_point> wall_deadline_;
	std::atomic<bool> supervisor_stop_{true};
	// One wall-clock supervisor handles both --max-seconds and the VSYNC
	// liveness guard.  It is armed for every run so a mock entry which never
	// reaches EmulateCycle cannot spin forever even without --max-seconds.
	std::thread supervisor_;
	std::mutex supervisor_mutex_;
	std::condition_variable supervisor_wake_;
	pthread_t supervised_thread_{};
	std::atomic<int> supervisor_quit_code_{0};
	uint32_t wall_check_counter_ = 0;
	std::atomic<uint64_t> vblank_generation_{0};
	std::atomic<uint32_t> supervisor_last_vblank_frame_{0};
	std::atomic<uint32_t> supervisor_last_vblank_cycle_{0};

	Coverage coverage_;
	bool coverage_active_ = false;
	bool save_snapshot_done_ = false;
	bool save_screen_done_ = false;
	bool screenshot_chord_was_down_ = false;
	bool add_play_snapshot_done_ = false;
	bool snapshot_capture_pending_ = false;
	bool keep_state_on_run_ = false;

	KnowledgeBase kb_;
	std::string kb_load_error_;
	KbWatch kb_watch_;
	std::unique_ptr<KbWatchWindow> kb_watch_window_;
	bool trace_calls_ = false;
	LastTrace last_trace_;
	FILE * watch_timeline_file_ = nullptr;

	WavWriter wav_;
	bool audio_armed_ = false;
	std::string media_wav_path_;
	bool media_wav_is_temp_ = false;
	std::vector<uint8_t> video_rgb_; // RGB24 frames concatenated
	uint32_t video_frames_ = 0;
	std::unique_ptr<FrameObservationWriter> frame_observations_;
};

} // namespace revm
