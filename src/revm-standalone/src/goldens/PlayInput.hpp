// Created  : 2026-09-14
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "input/IInputSource.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace revm {

// One standalone playback source: load and validate a play JSON once, then
// supply sticky VSYNC input. Non-empty mod_events are ignored with a warning.
// Optional JSON "no-twin": true marks a no-Twin recording; missing/false is
// original and warns that this no-twin system will not replay it faithfully.
class PlayInput : public IInputSource {
public:
	bool Load(const std::string & path, std::string & error);

	unsigned RandSeed() const { return rand_seed_; }
	bool NoTwin() const { return no_twin_; }
	uint32_t StartFrame() const { return start_frame_; }
	uint64_t StartCycle() const { return start_cycle_; }
	uint32_t EndFrame() const { return end_frame_; }

	// Advance held state to the last event at or before `frame`.
	void SeekTo(uint32_t frame);

	InputFrame PollFrame(uint32_t frame) override;

private:
	unsigned rand_seed_ = 42;
	bool no_twin_ = false;
	uint32_t start_frame_ = 0;
	uint64_t start_cycle_ = 0;
	uint32_t end_frame_ = 0;
	std::map<uint32_t, InputFrame> events_;
	InputFrame current_{};
};

} // namespace revm
