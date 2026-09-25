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

} // namespace revm
