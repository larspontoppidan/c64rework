// Created  : 2026-10-12
// Project  : REVM - C64rework

// Compile-time checks of CompareMask's CPU-flag channel. Built with every
// Stage 3/4 build (like LinkedCompileProbe), so a broken mask helper stops
// the build instead of silently comparing the wrong bits.

#include "cpumock/CpuMockHost.hpp"

using revm::cpumock::CompareMask;
using revm::cpumock::CpuFlag;

// A flags-only mask compares something, and only the flags.
static_assert(CompareMask::Flags(CpuFlag::D, CpuFlag::D).any());
static_assert(!CompareMask::Flags(CpuFlag::D, CpuFlag::D).kb);
static_assert(CompareMask::Flags(CpuFlag::D, CpuFlag::D).flags == 0x08);
static_assert(CompareMask::Flags(CpuFlag::D, CpuFlag::D).flags_main == 0x08);
static_assert(CompareMask::Flags(CpuFlag::D, 0).flags_main == 0x00);

// Main's value outside the selected bits is dropped, so a translation may
// pass a whole modelled P.
static_assert(CompareMask::Flags(CpuFlag::C, 0xFF).flags_main == 0x01);
static_assert(CompareMask::Flags(CpuFlag::C | CpuFlag::D, 0x08).flags_main ==
              0x08);

// WithFlags adds the channel and keeps the rest of the mask.
static_assert(CompareMask::Kb().WithFlags(CpuFlag::C, 1).kb);
static_assert(CompareMask::Kb().WithFlags(CpuFlag::C, 1).flags == 0x01);
static_assert(CompareMask::Kb().WithFlags(CpuFlag::C, 1).flags_main == 0x01);
static_assert(CompareMask::All().WithFlags(CpuFlag::N, 0).screen);

// The channel is off by default: None() still compares nothing.
static_assert(!CompareMask::None().any());
static_assert(CompareMask::Kb().flags == 0);
static_assert(CompareMask::All().flags == 0);

// The named bits are the 6510's P layout (NV-BDIZC).
static_assert(CpuFlag::C == 0x01 && CpuFlag::Z == 0x02 && CpuFlag::I == 0x04 &&
              CpuFlag::D == 0x08 && CpuFlag::V == 0x40 && CpuFlag::N == 0x80);
