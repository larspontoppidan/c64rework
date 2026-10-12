// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
	bool sid_resid_hq = false;
	bool sid_8580 = false; // --sid-model; default 6581

	std::optional<uint64_t> max_cycles;
	unsigned rand_seed = 42;

	// --save-screen FRAME FILE (repeatable): indexed Pepto PNG at VBLANK FRAME
	// (same counter as --max-frames). Empty = disabled.
	struct SaveScreen {
		uint32_t frame = 0;
		std::string path;
	};
	std::vector<SaveScreen> save_screens;
	// --state-hashes FILE: per-frame CRC32 of screen, SID writes, VIC and CIA
	// state (goldens/StateHashes.hpp). Empty = disabled.
	std::string state_hashes_path;
};

} // namespace revm
