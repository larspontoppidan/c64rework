// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "input/InputState.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace revm {

class Board;

// Same JSON source object REVM writes. Standalone recordings use type "none"
// (blank Main / InstallMainStart; no PRG or FullSnapshot load).
struct PlaySource {
	std::string type;
	std::string path;
	std::string sha256;
};

// Slim --record-play writer: sticky input_events + timing + rand_seed.
// Always writes "no-twin": true. No snapshot hashes, ROM records, F9
// timestamps, or mod_events (standalone has none of those machines).
class PlayRecorder {
public:
	void Configure(const std::string & out_path, PlaySource source);
	void Start(Board & board);

	void OnFrame(uint32_t frame, const InputFrame & input);

	bool Finish(std::string & error);
	bool Started() const { return board_ != nullptr; }

private:
	std::string out_path_;
	PlaySource source_;
	unsigned rand_seed_ = 42;
	uint32_t start_frame_ = 0;
	uint64_t start_cycle_ = 0;
	uint32_t end_frame_ = 0;
	std::map<uint32_t, InputFrame> events_;
	InputFrame last_input_;
	bool have_end_ = false;
	uint32_t last_frame_ = 0;
	Board * board_ = nullptr;
};

} // namespace revm
