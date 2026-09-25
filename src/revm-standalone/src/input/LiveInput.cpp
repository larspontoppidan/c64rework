// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "input/LiveInput.hpp"

#include "C64.h"
#include "Display.h"

#include <SDL.h>

#include <utility>

namespace revm {

namespace {

// Frodo MATRIX(a,b) = (a<<3)|b — must match Display.cpp
constexpr int frodo_matrix(int a, int b) { return (a << 3) | b; }

// Host scancode → Frodo C64 matrix encoding (matches Display.cpp translate_key).
// Bit 0x80 = also presses Right Shift (cursor up/left, Insert, shifted F-keys).
// Returns -1 if unknown / not a matrix key.
int frodo_c64_key_for_scancode(SDL_Scancode sc) {
	switch (sc) {
		case SDL_SCANCODE_A: return frodo_matrix(1, 2);
		case SDL_SCANCODE_B: return frodo_matrix(3, 4);
		case SDL_SCANCODE_C: return frodo_matrix(2, 4);
		case SDL_SCANCODE_D: return frodo_matrix(2, 2);
		case SDL_SCANCODE_E: return frodo_matrix(1, 6);
		case SDL_SCANCODE_F: return frodo_matrix(2, 5);
		case SDL_SCANCODE_G: return frodo_matrix(3, 2);
		case SDL_SCANCODE_H: return frodo_matrix(3, 5);
		case SDL_SCANCODE_I: return frodo_matrix(4, 1);
		case SDL_SCANCODE_J: return frodo_matrix(4, 2);
		case SDL_SCANCODE_K: return frodo_matrix(4, 5);
		case SDL_SCANCODE_L: return frodo_matrix(5, 2);
		case SDL_SCANCODE_M: return frodo_matrix(4, 4);
		case SDL_SCANCODE_N: return frodo_matrix(4, 7);
		case SDL_SCANCODE_O: return frodo_matrix(4, 6);
		case SDL_SCANCODE_P: return frodo_matrix(5, 1);
		case SDL_SCANCODE_Q: return frodo_matrix(7, 6);
		case SDL_SCANCODE_R: return frodo_matrix(2, 1);
		case SDL_SCANCODE_S: return frodo_matrix(1, 5);
		case SDL_SCANCODE_T: return frodo_matrix(2, 6);
		case SDL_SCANCODE_U: return frodo_matrix(3, 6);
		case SDL_SCANCODE_V: return frodo_matrix(3, 7);
		case SDL_SCANCODE_W: return frodo_matrix(1, 1);
		case SDL_SCANCODE_X: return frodo_matrix(2, 7);
		case SDL_SCANCODE_Y: return frodo_matrix(3, 1);
		case SDL_SCANCODE_Z: return frodo_matrix(1, 4);
		case SDL_SCANCODE_0: return frodo_matrix(4, 3);
		case SDL_SCANCODE_1: return frodo_matrix(7, 0);
		case SDL_SCANCODE_2: return frodo_matrix(7, 3);
		case SDL_SCANCODE_3: return frodo_matrix(1, 0);
		case SDL_SCANCODE_4: return frodo_matrix(1, 3);
		case SDL_SCANCODE_5: return frodo_matrix(2, 0);
		case SDL_SCANCODE_6: return frodo_matrix(2, 3);
		case SDL_SCANCODE_7: return frodo_matrix(3, 0);
		case SDL_SCANCODE_8: return frodo_matrix(3, 3);
		case SDL_SCANCODE_9: return frodo_matrix(4, 0);
		case SDL_SCANCODE_ESCAPE: return frodo_matrix(7, 7);
		case SDL_SCANCODE_RETURN: return frodo_matrix(0, 1);
		case SDL_SCANCODE_HOME: return frodo_matrix(6, 3);      // CLR/HOME
		case SDL_SCANCODE_END: return frodo_matrix(6, 0);       // £
		case SDL_SCANCODE_DELETE:
		case SDL_SCANCODE_BACKSPACE: return frodo_matrix(0, 0); // INS/DEL
		case SDL_SCANCODE_INSERT: return frodo_matrix(0, 0) | 0x80;
		case SDL_SCANCODE_PAGEDOWN: return frodo_matrix(6, 5);  // =
		case SDL_SCANCODE_PAGEUP: return frodo_matrix(6, 6);    // ↑
		case SDL_SCANCODE_UP: return frodo_matrix(0, 7) | 0x80; // CRSR ↑↓ + RSHIFT
		case SDL_SCANCODE_DOWN: return frodo_matrix(0, 7);      // CRSR ↑↓
		case SDL_SCANCODE_LEFT: return frodo_matrix(0, 2) | 0x80; // CRSR ←→ + RSHIFT
		case SDL_SCANCODE_RIGHT: return frodo_matrix(0, 2);     // CRSR ←→
		case SDL_SCANCODE_F1: return frodo_matrix(0, 4);
		case SDL_SCANCODE_F2: return frodo_matrix(0, 4) | 0x80;
		case SDL_SCANCODE_F3: return frodo_matrix(0, 5);
		case SDL_SCANCODE_F4: return frodo_matrix(0, 5) | 0x80;
		case SDL_SCANCODE_F5: return frodo_matrix(0, 6);
		case SDL_SCANCODE_F6: return frodo_matrix(0, 6) | 0x80;
		case SDL_SCANCODE_F7: return frodo_matrix(0, 3);
		case SDL_SCANCODE_F8: return frodo_matrix(0, 3) | 0x80;
		case SDL_SCANCODE_LCTRL:
		case SDL_SCANCODE_RCTRL:
		case SDL_SCANCODE_TAB: return frodo_matrix(7, 2);       // CTRL
		case SDL_SCANCODE_LSHIFT: return frodo_matrix(1, 7);
		case SDL_SCANCODE_RSHIFT: return frodo_matrix(6, 4);
		case SDL_SCANCODE_LALT:
		case SDL_SCANCODE_RALT: return frodo_matrix(7, 5);      // C=
		case SDL_SCANCODE_SPACE: return frodo_matrix(7, 4);
		case SDL_SCANCODE_GRAVE: return frodo_matrix(7, 1);
		case SDL_SCANCODE_BACKSLASH: return frodo_matrix(6, 6);
		case SDL_SCANCODE_COMMA: return frodo_matrix(5, 7);
		case SDL_SCANCODE_PERIOD: return frodo_matrix(5, 4);
		case SDL_SCANCODE_SLASH: return frodo_matrix(6, 7);
		case SDL_SCANCODE_SEMICOLON: return frodo_matrix(5, 5);
		case SDL_SCANCODE_APOSTROPHE: return frodo_matrix(6, 2);
		case SDL_SCANCODE_MINUS: return frodo_matrix(5, 0);
		case SDL_SCANCODE_EQUALS: return frodo_matrix(5, 3);
		case SDL_SCANCODE_LEFTBRACKET: return frodo_matrix(5, 6);
		case SDL_SCANCODE_RIGHTBRACKET: return frodo_matrix(6, 1);
		default:
			return -1;
	}
}

void release_c64_key(uint8_t *key_matrix, uint8_t *rev_matrix, int c64_key) {
	if (c64_key < 0) return;
	const bool shifted = (c64_key & 0x80) != 0;
	const int c64_byte = (c64_key >> 3) & 7;
	const int c64_bit = c64_key & 7;
	key_matrix[c64_byte] |= static_cast<uint8_t>(1 << c64_bit);
	rev_matrix[c64_bit] |= static_cast<uint8_t>(1 << c64_byte);
	if (shifted) {
		// Frodo also holds Right Shift (MATRIX(6,4)) for shifted host keys.
		key_matrix[6] |= 0x10;
		rev_matrix[4] |= 0x40;
	}
}

void suppress_joy_keys_from_matrix(const JoyBindings & b, uint8_t *key_matrix, uint8_t *rev_matrix) {
	const SDL_Scancode keys[] = {b.up, b.down, b.left, b.right, b.fire};
	for (SDL_Scancode sc : keys) {
		if (sc == SDL_SCANCODE_UNKNOWN) continue;
		release_c64_key(key_matrix, rev_matrix, frodo_c64_key_for_scancode(sc));
	}
}

} // namespace

LiveInput::LiveInput(C64 * c64) : c64_(c64) {}

JoystickState LiveInput::ReadBindings(const JoyBindings & b, const Uint8 * keys) {
	JoystickState j;
	if (b.up != SDL_SCANCODE_UNKNOWN && keys[b.up]) j.up = true;
	if (b.down != SDL_SCANCODE_UNKNOWN && keys[b.down]) j.down = true;
	if (b.left != SDL_SCANCODE_UNKNOWN && keys[b.left]) j.left = true;
	if (b.right != SDL_SCANCODE_UNKNOWN && keys[b.right]) j.right = true;
	if (b.fire != SDL_SCANCODE_UNKNOWN && keys[b.fire]) j.fire = true;
	return j;
}

InputFrame LiveInput::PollFrame(uint32_t /*frame*/) {
	InputFrame out;
	pause_toggle_requested_ = false;
	quit_requested_ = false;
	joystick_swap_toggled_ = false;

	if (!c64_ || !c64_->TheDisplay) {
		return out;
	}

	uint8_t joykey_unused = 0xff;
	c64_->TheDisplay->PollKeyboard(keyboard_.matrix, keyboard_.rev_matrix, &joykey_unused);
	out.keyboard = keyboard_;
	out.joykey = 0xff;

	// Keep joy-bound host keys off the C64 matrix.
	suppress_joy_keys_from_matrix(joy_cfg_.joy1, out.keyboard.matrix, out.keyboard.rev_matrix);
	suppress_joy_keys_from_matrix(joy_cfg_.joy2, out.keyboard.matrix, out.keyboard.rev_matrix);
	// Do not mutate keyboard_ with suppress — Frodo's event deltas assume real state.

	const Uint8 * keys = SDL_GetKeyboardState(nullptr);
	out.joy1 = ReadBindings(joy_cfg_.joy1, keys);
	out.joy2 = ReadBindings(joy_cfg_.joy2, keys);
	const bool swap_chord_down = keys[SDL_SCANCODE_TAB] &&
	                             (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]);
	if (swap_chord_down && !swap_chord_was_down_) {
		joysticks_swapped_ = !joysticks_swapped_;
		joystick_swap_toggled_ = true;
	}
	swap_chord_was_down_ = swap_chord_down;
	if (joysticks_swapped_) std::swap(out.joy1, out.joy2);
	if (swap_chord_down) {
		// Tab alone remains Frodo's C64 Ctrl alias. Consume the C64 Ctrl matrix
		// key only while the host Ctrl+Tab swap chord is held.
		release_c64_key(out.keyboard.matrix, out.keyboard.rev_matrix,
		                frodo_matrix(7, 2));
	}
	if (keys[SDL_SCANCODE_F9] &&
	    (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL])) {
		// Ctrl+F9 is a host screenshot chord (Board), not a C64 CTRL press
		// that should enter a recording.
		release_c64_key(out.keyboard.matrix, out.keyboard.rev_matrix,
		                frodo_matrix(7, 2));
	}

	bool f10 = keys[SDL_SCANCODE_F10];
	if (f10 && !f10_was_down_) {
		pause_toggle_requested_ = true;
	}
	f10_was_down_ = f10;
	bool f12 = keys[SDL_SCANCODE_F12];
	if (f12 && !f12_was_down_) {
		quit_requested_ = true;
	}
	f12_was_down_ = f12;

	return out;
}

} // namespace revm
