// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>

#include "VIC.h"
#include "SID.h"
#include "CIA.h"

namespace revm {

// Config-register equality for Stage 3 CompareMask channels and any caller
// that wants the same set. Live beam/timer/status fields and Frodo SC
// internals are out so VSYNC-faithful lockstep does not demand Φ2 identity.

// Programmed VIC file: sprite coords, ctrl (no RST8), enable/expand,
// vbase, irq_mask, irq_raster, priority/multicolor, colours. Not live
// raster, irq_flag, collision latches, or lightpen.
bool VicConfigEqual(const MOS6569State & a, const MOS6569State & b);

// $D400–$D418 write regs + pot readbacks. Not fake-v3 / last_sid_* internals.
bool SidConfigEqual(const MOS6581State & a, const MOS6581State & b);

// DDR, CRA/CRB, timer latches, interrupt mask. Not live counters, ICR
// flags, TOD, or port input. pra_mask (CIA2: 0x03) compares those PRA bits.
bool CiaConfigEqual(const MOS6526State & a, const MOS6526State & b,
                    uint8_t pra_mask = 0);

bool ChipConfigEqual(const MOS6569State & vic_a, const MOS6569State & vic_b,
                     const MOS6581State & sid_a, const MOS6581State & sid_b,
                     const MOS6526State & cia1_a, const MOS6526State & cia1_b,
                     const MOS6526State & cia2_a, const MOS6526State & cia2_b,
                     bool compare_sid = true);

} // namespace revm
