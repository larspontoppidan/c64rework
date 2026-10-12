// Project  : REVM - C64rework

#pragma once

#include <cstdint>

// reSID choices a game can make in code (CpuMockHost / GameHost SetResid and
// SetResidAdsr); they override the command line. Sound only: Twin compares
// SID registers, never audio.

// reSID's resampling, as --resid (Normal) and --resid-hq (High).
enum class ResidQuality : uint8_t {
	Normal = 0,  // interpolated sampling
	High = 1,    // FastMem resampling: higher quality, more memory
};

// The chip reSID models, as --sid-model.
enum class SidModel : uint8_t {
	Mos6581 = 0,  // the original C64 chip (default)
	Mos8580 = 1,  // the later chip: different filter, louder combined waveforms
};

// How reSID treats the real SID's "ADSR delay bug": an envelope rate counter
// that is already past a newly selected rate period wraps through 0x8000
// before the envelope steps, up to ~33 ms late. C64 Rework patches reSID
// (src/revm/patches/resid-adsr-delay-fix.patch) so this can be chosen.
enum class ResidAdsr : uint8_t {
	Real = 0,           // the real chip (default)
	FixAttack = 1,      // note-ons on time; releases keep the chip's delay
	Fix = 2,            // no delay anywhere
	FixAttackSoft = 3,  // as FixAttack, but note-ons keep the chip's delay / 5
};
