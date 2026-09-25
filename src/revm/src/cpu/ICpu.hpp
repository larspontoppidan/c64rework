// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>

struct MOS6510State;

namespace revm {

// Interchangeable CPU component (stage 1: Frodo MOS6510 SC).
class ICpu {
public:
	virtual ~ICpu() = default;
	virtual void Reset() = 0;
	virtual void EmulateCycle() = 0;
	virtual void GetState(MOS6510State * s) const = 0;
	virtual void SetState(const MOS6510State * s) = 0;
};

} // namespace revm
