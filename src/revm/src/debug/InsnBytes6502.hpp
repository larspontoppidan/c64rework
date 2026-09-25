// Created  : 2026-08-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>

namespace revm {

// Instruction length in bytes (1–3) for a documented opcode; undocumented → 1.
unsigned InsnBytes6502(uint8_t opcode);

} // namespace revm
