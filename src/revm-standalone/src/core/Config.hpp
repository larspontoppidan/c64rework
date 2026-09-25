// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace revm {

enum class SidMode {
	None,
	Digital,
	Resid,
};

// Runtime configuration retained by the standalone board. Every run is a
// blank Main. CLI/debugger, snapshot, recording, Twin, and event-stream
// options belong to the broad REVM host and are deliberately absent here.
struct Config {
	bool limit_speed = true;
	bool headless = false;
	SidMode sid = SidMode::Digital;

	std::optional<uint64_t> max_cycles;
	std::optional<double> max_seconds;
	unsigned rand_seed = 42;

	// --save-screen FRAME FILE: Pepto P6 PPM at VBLANK FRAME (same counter
	// as --max-frames). Unset = disabled.
	std::optional<uint32_t> save_screen_frame;
	std::string save_screen_path;
};

} // namespace revm
