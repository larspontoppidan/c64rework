// Created  : 2026-07-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace revm {

// Inclusive address ranges excluded from cpumock+ golden RAM compares.
// Default: hardware stack $0100–$01FF (not meaningful without a 6510).
class RamCompareMask {
public:
	struct Range {
		uint16_t lo = 0;
		uint16_t hi = 0; // inclusive
	};

	static RamCompareMask CpuMockDefaults() {
		RamCompareMask m;
		m.Exclude(0x0100, 0x01FF); // hardware stack — not meaningful without 6510
		m.Exclude(0x0000, 0x0001); // 6510 DDR/port; Frodo RAM[0/1] = LastVICByte
		m.Exclude(0x00A0, 0x00A2); // Kernal jiffy clock (UDTIM not run under mock)
		return m;
	}

	void Exclude(uint16_t lo, uint16_t hi) {
		if (hi < lo) std::swap(lo, hi);
		ranges_.push_back({lo, hi});
	}

	void Clear() { ranges_.clear(); }

	const std::vector<Range> & Ranges() const { return ranges_; }

	bool Equal(const uint8_t * a, const uint8_t * b, size_t size = 65536) const;

private:
	std::vector<Range> ranges_;
};

} // namespace revm
