// Created  : 2026-09-06
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "input/MemoryModification.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace revm::cpumock {

// Host-independent hand-off surface for comparing detached local state to
// the original KB/RAM bytes at a join fence.
class LinkedRegistry {
public:
	using ReadFn = std::function<uint8_t(size_t offset)>;
	using WriteFn = std::function<void(size_t offset, uint8_t value)>;
	struct Entry {
		uint16_t addr = 0;
		uint16_t size = 0;
		std::string name;
		ReadFn read;
		WriteFn write;
	};

	// A linked address has one owner. Invalid or overlapping registration is a
	// migration error, not a condition callers may safely ignore.
	void Register(uint16_t addr, size_t size, std::string name, ReadFn read,
	              WriteFn write) {
		if (size == 0 || size > std::numeric_limits<uint16_t>::max())
			throw std::logic_error("linked registration has invalid size");
		const uint32_t end = uint32_t(addr) + uint32_t(size) - 1u;
		if (end > 0xFFFFu || !read || !write)
			throw std::logic_error("linked registration is outside C64 RAM");
		for (const Entry & e : entries_) {
			const uint32_t e_end = uint32_t(e.addr) + e.size - 1u;
			if (uint32_t(addr) <= e_end && uint32_t(e.addr) <= end)
				throw std::logic_error("duplicate/overlapping linked address");
		}
		entries_.push_back(Entry{addr, uint16_t(size), std::move(name),
		                         std::move(read), std::move(write)});
	}

	bool Read(uint16_t addr, uint8_t &value) const {
		for (const Entry & e : entries_) {
			if (addr < e.addr || uint32_t(addr) >= uint32_t(e.addr) + e.size)
				continue;
			value = e.read(size_t(addr - e.addr));
			return true;
		}
		return false;
	}

	// Recorded play modifications are inputs, so a detached Main must receive
	// them in its local owner while Board's existing mirror updates Twin RAM.
	bool Apply(const ::revm::MemoryModification &modification) const {
		for (const Entry & e : entries_) {
			if (modification.address < e.addr ||
			    uint32_t(modification.address) >= uint32_t(e.addr) + e.size)
				continue;
			const size_t offset = size_t(modification.address - e.addr);
			const uint8_t old_value = e.read(offset);
			const uint8_t new_value =
				modification.operation == ::revm::MemoryModOperation::Increment
					? uint8_t(old_value + 1)
					: uint8_t(old_value - 1);
			e.write(offset, new_value);
			return true;
		}
		return false;
	}

	void Clear() { entries_.clear(); }
	const std::vector<Entry> &Entries() const { return entries_; }
	size_t Size() const { return entries_.size(); }
	size_t ByteCount() const {
		size_t n = 0;
		for (const Entry & e : entries_) n += e.size;
		return n;
	}

private:
	std::vector<Entry> entries_;
};

} // namespace revm::cpumock
