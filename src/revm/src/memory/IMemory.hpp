// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>
#include <cstddef>

namespace revm {

// Interchangeable C64 memory map (RAM/ROM/color, banking).
class IMemory {
public:
	virtual ~IMemory() = default;
	virtual uint8_t Read(uint16_t addr) const = 0;
	virtual void Write(uint16_t addr, uint8_t value) = 0;
	virtual uint8_t * Ram() = 0;
	virtual uint8_t * ColorRam() = 0;
	virtual void GetState(uint8_t * ram64k, uint8_t * color1k) const = 0;
	virtual void SetState(const uint8_t * ram64k, const uint8_t * color1k) = 0;
};

} // namespace revm
