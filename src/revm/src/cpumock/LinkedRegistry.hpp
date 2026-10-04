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
		if (owner_.empty()) owner_.assign(0x10000, -1);
		for (uint32_t a = addr; a <= end; ++a)
			if (owner_[a] >= 0)
				throw std::logic_error("duplicate/overlapping linked address");
		for (uint32_t a = addr; a <= end; ++a)
			owner_[a] = int32_t(entries_.size());
		++version_;
		entries_.push_back(Entry{addr, uint16_t(size), std::move(name),
		                         std::move(read), std::move(write)});
	}

	bool Read(uint16_t addr, uint8_t &value) const {
		const int32_t idx = OwnerIndex(addr);
		if (idx < 0) return false;
		const Entry & e = entries_[size_t(idx)];
		value = e.read(size_t(addr - e.addr));
		return true;
	}

	// Index into Entries() of the entry owning addr, or -1. O(1).
	int32_t OwnerIndex(uint16_t addr) const {
		return owner_.empty() ? -1 : owner_[addr];
	}
	// Bumped on every Register/Clear; lets callers cache derived layouts.
	uint64_t Version() const { return version_; }

	// Recorded play modifications are inputs, so a detached Main must receive
	// them in its local owner while Board's existing mirror updates Twin RAM.
	bool Apply(const ::revm::MemoryModification &modification) const {
		const int32_t idx = OwnerIndex(modification.address);
		if (idx >= 0) {
			const Entry & e = entries_[size_t(idx)];
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

	void Clear() {
		entries_.clear();
		owner_.clear();
		++version_;
	}
	const std::vector<Entry> &Entries() const { return entries_; }
	size_t Size() const { return entries_.size(); }
	size_t ByteCount() const {
		size_t n = 0;
		for (const Entry & e : entries_) n += e.size;
		return n;
	}

private:
	std::vector<Entry> entries_;
	std::vector<int32_t> owner_; // addr -> entries_ index, -1 = none
	uint64_t version_ = 0;
};

} // namespace revm::cpumock
