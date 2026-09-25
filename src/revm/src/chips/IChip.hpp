// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>

namespace revm {

// Interchangeable chip (VIC/SID/CIA). Stage 1 uses Frodo SC implementations.
class IChip {
public:
	virtual ~IChip() = default;
	virtual void Reset() = 0;
	virtual void EmulateCycle() = 0;
};

} // namespace revm
