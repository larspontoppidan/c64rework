// Created  : 2026-09-29
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "debug/KnowledgeBase.hpp"

#include "C64.h"

#include <cstdint>

namespace revm {

// Watch planes: DRAM (omitted / "ram") or the $D800-$DBFF nybble file
// ("color"). Never the CPU-visible I/O map — watches do not follow io_in.
inline bool KbWatchEnroll(const KbObject & o) {
	return o.IsRamBank() || o.IsColorBank();
}

inline uint8_t KbWatchSample(const C64 * c64, const KbObject & o, uint16_t addr) {
	if (!c64) return 0;
	if (o.IsColorBank()) {
		if (!c64->Color) return 0;
		return uint8_t(c64->Color[addr & 0x03FF] & 0x0F);
	}
	if (!c64->RAM) return 0;
	return c64->RAM[addr];
}

inline uint8_t KbWatchSamplePlanes(const KbObject & o, uint16_t addr,
                                   const uint8_t ram[C64_RAM_SIZE],
                                   const uint8_t color[COLOR_RAM_SIZE]) {
	if (o.IsColorBank())
		return uint8_t(color[addr & 0x03FF] & 0x0F);
	return ram[addr];
}

} // namespace revm
