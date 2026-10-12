// Created  : 2026-10-12
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Standard CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320; the same
// values as zlib's crc32()). Kept here instead of calling zlib so code that
// links revm_lib by hand needs no extra library. Shared with the standalone.

#include <array>
#include <cstddef>
#include <cstdint>

namespace revm {

class Crc32 {
public:
	void Update(const uint8_t * data, size_t size) {
		const std::array<uint32_t, 256> & table = Table();
		uint32_t crc = state_;
		for (size_t i = 0; i < size; ++i)
			crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
		state_ = crc;
	}

	uint32_t Value() const { return state_ ^ 0xffffffffu; }

	static uint32_t Of(const uint8_t * data, size_t size) {
		Crc32 crc;
		crc.Update(data, size);
		return crc.Value();
	}

private:
	static const std::array<uint32_t, 256> & Table() {
		static const std::array<uint32_t, 256> table = MakeTable();
		return table;
	}

	static std::array<uint32_t, 256> MakeTable() {
		std::array<uint32_t, 256> table{};
		for (uint32_t n = 0; n < 256; ++n) {
			uint32_t c = n;
			for (int bit = 0; bit < 8; ++bit)
				c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
			table[n] = c;
		}
		return table;
	}

	uint32_t state_ = 0xffffffffu;
};

} // namespace revm
