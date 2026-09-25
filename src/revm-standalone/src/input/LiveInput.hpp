// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "input/IInputSource.hpp"
#include "input/JoystickConfig.hpp"

class C64;

namespace revm {

// Live SDL input: C64 keyboard via Frodo Display; joysticks via JoystickConfig.
class LiveInput : public IInputSource {
public:
	explicit LiveInput(C64 * c64);

	void SetJoystickConfig(const JoystickConfig & cfg) { joy_cfg_ = cfg; }

	InputFrame PollFrame(uint32_t frame) override;

	bool PauseToggleRequested() const { return pause_toggle_requested_; }
	void ClearPauseToggleRequest() { pause_toggle_requested_ = false; }
	bool QuitRequested() const { return quit_requested_; }
	void ClearQuitRequest() { quit_requested_ = false; }
	bool JoystickSwapToggled() const { return joystick_swap_toggled_; }
	void ClearJoystickSwapToggle() { joystick_swap_toggled_ = false; }
	bool JoysticksSwapped() const { return joysticks_swapped_; }

private:
	static JoystickState ReadBindings(const JoyBindings & b, const Uint8 * keys);

	C64 * c64_;
	JoystickConfig joy_cfg_ = JoystickConfig::DefaultKeypadJoy2();
	KeyboardState keyboard_; // persistent — Frodo PollKeyboard applies event deltas
	bool pause_toggle_requested_ = false;
	bool quit_requested_ = false;
	bool joystick_swap_toggled_ = false;
	bool joysticks_swapped_ = false;
	bool f10_was_down_ = false;
	bool f12_was_down_ = false;
	bool swap_chord_was_down_ = false;
};

} // namespace revm
