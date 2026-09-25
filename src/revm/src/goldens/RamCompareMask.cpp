// Created  : 2026-07-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "goldens/RamCompareMask.hpp"

#include <algorithm>
#include <cstring>

namespace revm {

bool RamCompareMask::Equal(const uint8_t * a, const uint8_t * b, size_t size) const {
	if (!a || !b) return false;
	if (ranges_.empty()) {
		return std::memcmp(a, b, size) == 0;
	}

	std::vector<Range> sorted = ranges_;
	std::sort(sorted.begin(), sorted.end(),
	          [](const Range & x, const Range & y) { return x.lo < y.lo; });

	size_t pos = 0;
	for (const Range & r : sorted) {
		const size_t lo = std::min(size_t(r.lo), size);
		const size_t hi_excl = std::min(size_t(r.hi) + 1, size);
		if (pos < lo) {
			if (std::memcmp(a + pos, b + pos, lo - pos) != 0) return false;
		}
		pos = std::max(pos, hi_excl);
	}
	if (pos < size) {
		if (std::memcmp(a + pos, b + pos, size - pos) != 0) return false;
	}
	return true;
}

} // namespace revm
