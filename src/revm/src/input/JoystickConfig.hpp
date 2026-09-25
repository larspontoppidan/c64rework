// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <SDL.h>

namespace revm {

struct JoyBindings {
	SDL_Scancode up = SDL_SCANCODE_UNKNOWN;
	SDL_Scancode down = SDL_SCANCODE_UNKNOWN;
	SDL_Scancode left = SDL_SCANCODE_UNKNOWN;
	SDL_Scancode right = SDL_SCANCODE_UNKNOWN;
	SDL_Scancode fire = SDL_SCANCODE_UNKNOWN;

	bool AnyBound() const {
		return up != SDL_SCANCODE_UNKNOWN || down != SDL_SCANCODE_UNKNOWN ||
		       left != SDL_SCANCODE_UNKNOWN || right != SDL_SCANCODE_UNKNOWN ||
		       fire != SDL_SCANCODE_UNKNOWN;
	}
};

struct CheatBindings {
	SDL_Scancode increment = SDL_SCANCODE_PAGEUP;
	SDL_Scancode decrement = SDL_SCANCODE_PAGEDOWN;
};

struct JoystickConfig {
	JoyBindings joy1;
	JoyBindings joy2;
	CheatBindings cheats;

	// Built-in Frodo-style keypad on joy2 if no file loaded / empty joy2.
	static JoystickConfig DefaultKeypadJoy2();
	static const char * DefaultFileTemplate();
	static bool WriteDefaultFile(const std::string & path, std::string & error);

	bool LoadFile(const std::string & path, std::string & error);
};

// Parse one key name → scancode. Returns false if unknown.
bool ParseScancodeName(const std::string & name, SDL_Scancode & out);

} // namespace revm
