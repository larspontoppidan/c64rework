// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "core/Board.hpp"

#include "goldens/PlayRecorder.hpp"
#include "input/LiveInput.hpp"
#include "util/Png.hpp"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>


#include <SDL.h>

namespace revm {


Board::Board() = default;

Board::~Board() {
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
	save_screens_done_.assign(cfg_.save_screens.size(), false);
	state_hashes_.Configure(cfg_.state_hashes_path);
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
	ThePrefs.ResidHQ = cfg_.sid_resid_hq;
	ThePrefs.ResidAdsr = cfg_.sid_resid_adsr;
	switch (cfg_.sid) {
	case SidMode::Resid:
		ThePrefs.SIDType = cfg_.sid_8580 ? SIDTYPE_RESID_8580 : SIDTYPE_RESID_6581;
		REVM_LOG(REVM_DEBUG, "SID: reSID %s (%s)",
		         cfg_.sid_8580 ? "8580" : "6581",
		         cfg_.sid_resid_hq ? "FastMem resampling" : "interpolate");
		break;
	case SidMode::Digital:
		ThePrefs.SIDType = cfg_.sid_8580 ? SIDTYPE_DIGITAL_8580 : SIDTYPE_DIGITAL_6581;
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
	if (state_hashes_.Enabled())
		state_hashes_.Attach(c64_);

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
	if (cfg_.max_cycles)
		check_max_cycles();
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
	record_state_hashes(frame, cycle);
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
	if (have_last_vblank_)
		vblank_period_ = cycle - last_vblank_cycle_;
	last_vblank_cycle_ = cycle;
	have_last_vblank_ = true;
}

void Board::maybe_save_screen() {
	const uint32_t frame = FrameCounter();
	for (size_t i = 0; i < cfg_.save_screens.size(); ++i) {
		const Config::SaveScreen & request = cfg_.save_screens[i];
		if (save_screens_done_[i] || request.frame != frame)
			continue;
		save_screens_done_[i] = true;

		std::string err;
		if (!write_display_png(request.path, err)) {
			REVM_LOG(REVM_ERROR, "save-screen failed: %s", err.c_str());
			RequestQuit(1);
			return;
		}
		REVM_LOG(REVM_INFO, "save-screen → %s (frame=%u)",
		         request.path.c_str(), frame);
		std::fflush(stderr);
	}
}

void Board::record_state_hashes(uint32_t frame, uint32_t cycle) {
	if (!state_hashes_.Enabled())
		return;
	std::string error;
	if (!state_hashes_.OnFrame(frame, cycle, error)) {
		REVM_LOG(REVM_ERROR, "%s", error.c_str());
		RequestQuit(1);
	}
}

bool Board::write_display_png(const std::string & path, std::string & error) {
	const uint8_t * pixels =
		(c64_ && c64_->TheDisplay) ? c64_->TheDisplay->BitmapBase() : nullptr;
	if (!pixels) {
		error = "capture failed";
		return false;
	}
	return WritePngPepto(path, unsigned(DISPLAY_X), unsigned(DISPLAY_Y), pixels,
	                     error);
}

void Board::save_live_screenshot() {
	const uint32_t frame = FrameCounter();
	const std::string path = "frame" + std::to_string(frame) + ".png";
	std::string err;
	if (!write_display_png(path, err)) {
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

bool Board::FinishOutputs(std::string & error) {
	error.clear();
	if (!state_hashes_.Finish(error))
		return false;
	for (size_t i = 0; i < cfg_.save_screens.size(); ++i) {
		if (save_screens_done_[i])
			continue;
		error = "save-screen: frame " +
		        std::to_string(cfg_.save_screens[i].frame) +
		        " never reached (ended at frame " +
		        std::to_string(FrameCounter()) + ")";
		return false;
	}
	return true;
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
