// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>

#include "VIC.h"
#include "SID.h"
#include "CIA.h"

namespace revm {

// Compare programmer-visible chip I/O register mirrors only.
// Frodo SC pipeline / BA / EG / timer-delay internals are ignored so Stage 2+
// can stay VSYNC-faithful without bus-accurate lockstep.

bool PublicVicIoEqual(const MOS6569State & a, const MOS6569State & b);
bool PublicSidIoEqual(const MOS6581State & a, const MOS6581State & b);
bool PublicCiaIoEqual(const MOS6526State & a, const MOS6526State & b,
                      bool compare_ifr = true);

bool PublicChipIoEqual(const MOS6569State & vic_a, const MOS6569State & vic_b,
                       const MOS6581State & sid_a, const MOS6581State & sid_b,
                       const MOS6526State & cia1_a, const MOS6526State & cia1_b,
                       const MOS6526State & cia2_a, const MOS6526State & cia2_b,
                       bool compare_sid = true, bool compare_cia_ifr = true);

} // namespace revm
