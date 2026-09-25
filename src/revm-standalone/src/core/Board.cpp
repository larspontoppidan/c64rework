// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "core/Board.hpp"

#include "goldens/PlayRecorder.hpp"
#include "input/LiveInput.hpp"
#define REVM_LOG_MODULE "board"
#include "util/Log.hpp"

#include "C64.h"
#include "CIA.h"
#include "CPUC64.h"
#include "Display.h"
#include "Prefs.h"
#include "SID.h"
#include "main.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <pthread.h>
#include <unistd.h>

#include <SDL.h>

namespace revm {

namespace {

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

} // namespace

Board::Board() = default;

Board::~Board() {
	StopWallClockLimit();
	log::ClearClockSource();
	if (machine_owned_ && c64_) {
		if (TheC64 == c64_)
			TheC64 = nullptr;
		delete c64_;
		c64_ = nullptr;
	}
	if (sdl_initialized_) {
		SDL_Quit();
		sdl_initialized_ = false;
	}
}

void Board::Configure(const Config & cfg) {
	cfg_ = cfg;
	paused_ = false;
	last_vblank_cycle_ = 0;
	vblank_period_ = kCyclesPerFrame;
	have_last_vblank_ = false;
	save_screen_done_ = false;
}

bool Board::Init(std::string & error) {
	return init_machine(error);
}

bool Board::init_machine(std::string & error) {
	if (c64_) {
		error = "Board already initialized";
		return false;
	}

	// Frodo's main() normally performs SDL setup before constructing C64; the
	// standalone board owns that small part of the lifecycle itself.
	Uint32 sdl_flags = SDL_INIT_TIMER | SDL_INIT_EVENTS;
	if (!cfg_.headless) {
		sdl_flags |= SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;
	} else {
		// VIC still needs a Display/pixel buffer in headless tests.
		SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
		sdl_flags |= SDL_INIT_VIDEO;
	}
	if (!cfg_.headless && cfg_.sid != SidMode::None)
		sdl_flags |= SDL_INIT_AUDIO;
	if (SDL_Init(sdl_flags) < 0) {
		error = std::string("SDL_Init failed: ") + SDL_GetError();
		return false;
	}
	sdl_initialized_ = true;

	ThePrefs.LimitSpeed = cfg_.limit_speed;
	ThePrefs.TestBench = cfg_.headless;
	switch (cfg_.sid) {
	case SidMode::Resid:
		ThePrefs.SIDType = SIDTYPE_RESID_6581;
		REVM_LOG(REVM_DEBUG, "SID: reSID 6581 (--resid)");
		break;
	case SidMode::Digital:
		ThePrefs.SIDType = SIDTYPE_DIGITAL_6581;
		break;
	case SidMode::None:
		ThePrefs.SIDType = SIDTYPE_NONE;
		break;
	}

	// Color RAM has Frodo's deterministic power-on noise. Main-blank later
	// explicitly clears RAM and Color after PrepareRun.
	SetFrodoColorRamSeed(cfg_.rand_seed);
	c64_ = new C64();
	TheC64 = c64_;
	machine_owned_ = true;

	log::SetClockSource([this] {
		log::BoardClock clock;
		clock.frame = FrameCounter();
		clock.cycle = CycleCounter();
		if (HasLastVBlank()) {
			clock.frame_start = LastVBlankCycle();
			clock.frame_end = clock.frame_start + VBlankPeriod();
		}
		return clock;
	});
	return true;
}

void Board::ApplyInput(const InputFrame & in) {
	apply_input(in);
}

bool Board::finish_cycle(bool vblank) {
	const int supervisor_code =
		supervisor_quit_code_.exchange(0, std::memory_order_acq_rel);
	if (supervisor_code != 0)
		RequestQuit(supervisor_code);
	if (cfg_.max_cycles)
		check_max_cycles();
	if (wall_deadline_)
		check_max_seconds();
	if (vblank) {
		note_vblank();
		on_frame_boundary();
	}
	return vblank;
}

bool Board::EmulateCycle() {
	if (!c64_) return false;
	return finish_cycle(c64_->EmulateCycle());
}

void Board::check_max_cycles() {
	if (cfg_.max_cycles && CycleCounter() >= *cfg_.max_cycles) {
		REVM_LOG(REVM_DEBUG, "max-cycles reached: cycle=%u frame=%u",
		         CycleCounter(), FrameCounter());
		RequestQuit(0);
	}
}

void Board::check_max_seconds() {
	// Polling every 65536 cycles keeps the hot emulation path cheap. The
	// watchdog thread handles a mock C++ loop that never ticks.
	if ((++wall_check_counter_ & 0xffffu) != 0)
		return;
	if (wall_deadline_ && std::chrono::steady_clock::now() >= *wall_deadline_) {
		const double limit = cfg_.max_seconds ? *cfg_.max_seconds : 0.0;
		REVM_LOG(REVM_DEBUG,
		         "max-seconds reached (%.3fs wall): cycle=%u frame=%u",
		         limit, CycleCounter(), FrameCounter());
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
		const double seconds = *cfg_.max_seconds;
		wall_deadline_ = steady::now() +
		                 std::chrono::duration_cast<steady::duration>(
		                     std::chrono::duration<double>(seconds));
		REVM_LOG(REVM_DEBUG, "Will stop after %.3fs wall-clock (--max-seconds)",
		         seconds);
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

void Board::apply_input(const InputFrame & in) {
	if (!c64_ || !c64_->TheCIA1)
		return;
	uint8_t j1 = in.joy1.ToCiaMask();
	uint8_t j2 = in.joy2.ToCiaMask();
	if (ThePrefs.JoystickSwap)
		std::swap(j1, j2);
	c64_->TheCIA1->Joystick1 = j1;
	c64_->TheCIA1->Joystick2 = j2;
	std::memcpy(c64_->TheCIA1->KeyMatrix, in.keyboard.matrix, 8);
	std::memcpy(c64_->TheCIA1->RevMatrix, in.keyboard.rev_matrix, 8);
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
		if (live.JoysticksSwapped())
			REVM_LOG(REVM_INFO, "Ctrl+Tab: joystick ports swapped");
		else
			REVM_LOG(REVM_INFO, "Ctrl+Tab: joystick ports restored");
		live.ClearJoystickSwapToggle();
	}
	(void)in;
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
		if (auto * live = dynamic_cast<LiveInput *>(input_))
			poll_live_controls(*live, in);
		apply_input(in);
	}
	if (play_recorder_)
		play_recorder_->OnFrame(frame, in);

	c64_->TheCIA1->CountTOD();
	c64_->TheCIA2->CountTOD();
	maybe_save_screen();
	if (!cfg_.headless && c64_->TheDisplay) {
		if (!dynamic_cast<LiveInput *>(input_))
			pump_host_events();
		c64_->TheDisplay->Update();
	}
	poll_host_screenshot();
	if (frame_cb_)
		frame_cb_(frame, cycle);
	if (c64_->QuitRequested())
		quit_ = true;
	if (paused_ && !quit_)
		wait_while_paused();
}

void Board::pump_host_events() {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			RequestQuit(0);
			break;
		case SDL_WINDOWEVENT:
			if (event.window.event == SDL_WINDOWEVENT_CLOSE)
				RequestQuit(0);
			break;
		case SDL_KEYDOWN:
			if ((event.key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
			     event.key.keysym.scancode == SDL_SCANCODE_F12) &&
			    !event.key.repeat)
				RequestQuit(0);
			break;
		default:
			break;
		}
	}
}

void Board::note_vblank() {
	const uint32_t cycle = CycleCounter();
	const uint32_t frame = FrameCounter();
	if (have_last_vblank_)
		vblank_period_ = cycle - last_vblank_cycle_;
	last_vblank_cycle_ = cycle;
	have_last_vblank_ = true;
	supervisor_last_vblank_frame_.store(frame, std::memory_order_relaxed);
	supervisor_last_vblank_cycle_.store(cycle, std::memory_order_relaxed);
	vblank_generation_.fetch_add(1, std::memory_order_release);
}

void Board::maybe_save_screen() {
	if (save_screen_done_ || cfg_.save_screen_path.empty() ||
	    !cfg_.save_screen_frame) {
		return;
	}
	const uint32_t frame = FrameCounter();
	if (frame != *cfg_.save_screen_frame)
		return;
	save_screen_done_ = true;

	std::string err;
	if (!write_display_ppm(cfg_.save_screen_path, err)) {
		REVM_LOG(REVM_ERROR, "save-screen failed: %s", err.c_str());
		RequestQuit(1);
		return;
	}
	REVM_LOG(REVM_INFO, "save-screen → %s (frame=%u)",
	         cfg_.save_screen_path.c_str(), frame);
	std::fflush(stderr);
}

bool Board::write_display_ppm(const std::string & path, std::string & error) {
	const uint8_t * pixels =
		(c64_ && c64_->TheDisplay) ? c64_->TheDisplay->BitmapBase() : nullptr;
	if (!pixels) {
		error = "capture failed";
		return false;
	}

	// Pepto palette (same as Frodo Display.cpp default / full-revm PPM dumps)
	static constexpr uint8_t kR[16] = {
		0x00, 0xff, 0x86, 0x4c, 0x88, 0x35, 0x20, 0xcf,
		0x88, 0x40, 0xcb, 0x34, 0x68, 0x8b, 0x68, 0xa1};
	static constexpr uint8_t kG[16] = {
		0x00, 0xff, 0x19, 0xc1, 0x17, 0xac, 0x07, 0xf2,
		0x3e, 0x2a, 0x55, 0x34, 0x68, 0xff, 0x4a, 0xa1};
	static constexpr uint8_t kB[16] = {
		0x00, 0xff, 0x01, 0xe3, 0xbd, 0x0a, 0xc0, 0x2d,
		0x00, 0x00, 0x37, 0x34, 0x68, 0x59, 0xff, 0xa1};

	std::ofstream out(path, std::ios::binary);
	if (!out) {
		error = "cannot write " + path;
		return false;
	}
	out << "P6\n" << DISPLAY_X << " " << DISPLAY_Y << "\n255\n";
	const size_t count = size_t(DISPLAY_X) * DISPLAY_Y;
	for (size_t i = 0; i < count; ++i) {
		const uint8_t color = pixels[i] & 0x0f;
		const uint8_t rgb[3] = {kR[color], kG[color], kB[color]};
		out.write(reinterpret_cast<const char *>(rgb), 3);
	}
	if (!out) {
		error = "write failed: " + path;
		return false;
	}
	return true;
}

void Board::save_live_screenshot() {
	const uint32_t frame = FrameCounter();
	const std::string path = "frame" + std::to_string(frame) + ".ppm";
	std::string err;
	if (!write_display_ppm(path, err)) {
		REVM_LOG(REVM_ERROR, "screenshot failed: %s", err.c_str());
		return;
	}
	REVM_LOG(REVM_INFO, "screenshot → %s (frame=%u)", path.c_str(), frame);
	std::fflush(stderr);
}

void Board::poll_host_screenshot() {
	if (cfg_.headless || !c64_ || !c64_->TheDisplay) return;
	const Uint8 * keys = SDL_GetKeyboardState(nullptr);
	if (!keys) return;
	const bool chord = keys[SDL_SCANCODE_F9] &&
	                   (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]);
	if (chord && !screenshot_chord_was_down_)
		save_live_screenshot();
	screenshot_chord_was_down_ = chord;
}

bool Board::FinishSaveScreen(std::string & error) {
	error.clear();
	if (cfg_.save_screen_path.empty() || save_screen_done_)
		return true;
	error = "save-screen: frame " +
	        std::to_string(*cfg_.save_screen_frame) +
	        " never reached (ended at frame " +
	        std::to_string(FrameCounter()) + ")";
	return false;
}

void Board::RequestQuit(int exit_code) {
	if (!quit_ || (exit_code_ == 0 && exit_code != 0))
		exit_code_ = exit_code;
	quit_ = true;
	if (c64_)
		c64_->RequestQuit(exit_code_);
}

void Board::PrepareRun() {
	if (!c64_)
		return;
	quit_ = false;
	exit_code_ = 0;
	c64_->ResetCounters();
	c64_->TheCPU->Reset();
	c64_->TheSID->Reset();
	c64_->TheCIA1->Reset();
	c64_->TheCIA2->Reset();
}

uint32_t Board::CycleCounter() const {
	return c64_ ? c64_->CycleCounter() : 0;
}

uint32_t Board::FrameCounter() const {
	return c64_ ? c64_->FrameCounter() : 0;
}

} // namespace revm
