// Project  : REVM - C64rework

#pragma once

#include <cstdint>

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
