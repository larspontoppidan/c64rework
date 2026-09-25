// Created  : 2026-09-04
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>

namespace revm {

enum class MemoryModOperation {
	Increment,
	Decrement,
};

struct MemoryModification {
	uint16_t address = 0;
	MemoryModOperation operation = MemoryModOperation::Increment;

	bool operator==(const MemoryModification &) const = default;
};

} // namespace revm
