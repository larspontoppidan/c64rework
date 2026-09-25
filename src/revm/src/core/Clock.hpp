// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>

namespace revm {

// PAL C64 timing constants (shared with Frodo vendor defs)
constexpr unsigned kScreenFreqHz = 50;
constexpr unsigned kCyclesPerLine = 63;
constexpr unsigned kTotalRasters = 0x138; // 312
constexpr unsigned kCyclesPerFrame = kCyclesPerLine * kTotalRasters;

// Faithful counters: cycle 0 and frame 0 = machine boot.
class Clock {
public:
	void Reset() {
		cycle_ = 0;
		frame_ = 0;
	}

	void Set(uint32_t cycle, uint32_t frame) {
		cycle_ = cycle;
		frame_ = frame;
	}

	void TickCycle() { ++cycle_; }

	void TickFrame() { ++frame_; }

	uint32_t Cycle() const { return cycle_; }
	uint32_t Frame() const { return frame_; }

private:
	uint32_t cycle_ = 0;
	uint32_t frame_ = 0;
};

} // namespace revm
