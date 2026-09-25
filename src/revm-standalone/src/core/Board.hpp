// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "core/Clock.hpp"
#include "core/Config.hpp"
#include "input/IInputSource.hpp"
#include "input/InputState.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

class C64;

namespace revm {

class LiveInput;
class PlayRecorder;

// The standalone board owns one real Frodo machine. It deliberately has no
// knowledge of Twin, snapshots, or analysis/debug probes. Optional
// PlayRecorder captures sticky C64 input at VSYNC. The GameHost drives this
// same board through EmulateCycle().
class Board {
public:
	Board();
	~Board();

	Board(const Board &) = delete;
	Board & operator=(const Board &) = delete;

	void Configure(const Config & cfg);
	const Config & GetConfig() const { return cfg_; }

	bool Init(std::string & error);

	void ApplyInput(const InputFrame & in);

	bool EmulateCycle();
	void PrepareRun();

	void RequestQuit(int exit_code = 0);
	int ExitCode() const { return exit_code_; }

	uint32_t CycleCounter() const;
	uint32_t FrameCounter() const;

	// Timing probes used by the standalone log clock and CPU mock hooks; these
	// are not comparison machinery.
	uint32_t LastVBlankCycle() const { return last_vblank_cycle_; }
	uint32_t VBlankPeriod() const { return vblank_period_; }
	bool HasLastVBlank() const { return have_last_vblank_; }

	C64 * Machine() { return c64_; }
	const C64 * Machine() const { return c64_; }

	void SetInputSource(IInputSource * src) { input_ = src; }
	void SetPlayRecorder(PlayRecorder * r) { play_recorder_ = r; }

	// Arm/disarm the wall-clock supervisor for VSYNC liveness and
	// --max-seconds. Safe to call repeatedly.
	void StartWallClockLimit();
	void StopWallClockLimit();

	// Error if --save-screen was requested but FRAME was never reached.
	bool FinishSaveScreen(std::string & error);

	using FrameCallback = std::function<void(uint32_t frame, uint32_t cycle)>;
	void SetFrameCallback(FrameCallback cb) { frame_cb_ = std::move(cb); }

private:
	bool init_machine(std::string & error);
	void on_frame_boundary();
	void note_vblank();
	void apply_input(const InputFrame & in);
	void poll_live_controls(LiveInput & live, InputFrame & in);
	void wait_while_paused();
	bool finish_cycle(bool vblank);
	void pump_host_events();
	void maybe_save_screen();
	void poll_host_screenshot();
	void save_live_screenshot();
	bool write_display_ppm(const std::string & path, std::string & error);
	void check_max_cycles();
	void check_max_seconds();

	Config cfg_;
	C64 * c64_ = nullptr;
	IInputSource * input_ = nullptr;
	PlayRecorder * play_recorder_ = nullptr;
	FrameCallback frame_cb_;

	bool quit_ = false;
	int exit_code_ = 0;
	bool machine_owned_ = false;
	bool sdl_initialized_ = false;
	std::atomic<bool> paused_{false};
	uint32_t last_vblank_cycle_ = 0;
	uint32_t vblank_period_ = kCyclesPerFrame;
	bool have_last_vblank_ = false;

	std::optional<std::chrono::steady_clock::time_point> wall_deadline_;
	std::atomic<bool> supervisor_stop_{true};
	// One wall-clock supervisor handles both --max-seconds and the VSYNC
	// liveness guard. It is armed for every run so a translated entry which
	// never returns to the machine clock cannot spin forever.
	std::thread supervisor_;
	std::mutex supervisor_mutex_;
	std::condition_variable supervisor_wake_;
	pthread_t supervised_thread_{};
	std::atomic<int> supervisor_quit_code_{0};
	uint32_t wall_check_counter_ = 0;
	std::atomic<uint64_t> vblank_generation_{0};
	std::atomic<uint32_t> supervisor_last_vblank_frame_{0};
	std::atomic<uint32_t> supervisor_last_vblank_cycle_{0};
	bool save_screen_done_ = false;
	bool screenshot_chord_was_down_ = false;
};

} // namespace revm
