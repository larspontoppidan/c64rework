// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>
#include <cstring>

namespace revm {

// Joystick mask bits (matches Frodo / CIA convention: 0 = pressed when applied)
struct JoystickState {
	bool up = false;
	bool down = false;
	bool left = false;
	bool right = false;
	bool fire = false;

	bool operator==(const JoystickState & o) const = default;

	// Frodo CIA joystick byte: bits cleared when pressed
	uint8_t ToCiaMask() const {
		uint8_t m = 0xff;
		if (up)    m &= ~0x01;
		if (down)  m &= ~0x02;
		if (left)  m &= ~0x04;
		if (right) m &= ~0x08;
		if (fire)  m &= ~0x10;
		return m;
	}

	static JoystickState FromCiaMask(uint8_t m) {
		JoystickState s;
		s.up    = !(m & 0x01);
		s.down  = !(m & 0x02);
		s.left  = !(m & 0x04);
		s.right = !(m & 0x08);
		s.fire  = !(m & 0x10);
		return s;
	}
};

// Full keyboard matrix state (8x8), 0-bit = key down (Frodo convention)
struct KeyboardState {
	uint8_t matrix[8]{};
	uint8_t rev_matrix[8]{};

	KeyboardState() { Clear(); }

	void Clear() {
		std::memset(matrix, 0xff, sizeof(matrix));
		std::memset(rev_matrix, 0xff, sizeof(rev_matrix));
	}

	bool operator==(const KeyboardState & o) const {
		return std::memcmp(matrix, o.matrix, 8) == 0 &&
		       std::memcmp(rev_matrix, o.rev_matrix, 8) == 0;
	}
};

struct InputFrame {
	JoystickState joy1;
	JoystickState joy2;
	KeyboardState keyboard;
	uint8_t joykey = 0xff; // Keyboard-joystick emulation mask

	bool operator==(const InputFrame & o) const = default;
};

} // namespace revm
