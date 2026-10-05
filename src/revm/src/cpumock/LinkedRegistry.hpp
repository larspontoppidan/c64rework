// Created  : 2026-09-06
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "input/MemoryModification.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace revm::cpumock {

// Tag: a Linked value declared `{uninitialized}` has NO starting value. The
// game must write every byte before anything reads it (the registry fails the
// run loudly otherwise) and the KB compare ignores the unwritten bytes (Twin's
// RAM value is taken) until they are written. Use it for state the game
// initialises itself, instead of seeding the BEGIN image's leftovers.
struct Uninitialized {};
inline constexpr Uninitialized uninitialized{};

// Type-erased view of one Linked owner's written-state. `tracking` is true
// only while some byte is still unwritten; once every byte has been written
// it turns off for good and the object behaves like any declared-value one.
struct LinkedTrack {
	bool tracking = false;
	uint32_t unwritten = 0; // bytes not yet written
	uint32_t total = 0;     // bytes tracked (object size), 0 = never tracked
	// Probe for one byte offset; only valid while `tracking`.
	bool (*written)(const LinkedTrack &, size_t offset) = nullptr;
};

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
		// Owner's written-state; null unless declared `uninitialized`.
		const LinkedTrack * track = nullptr;
	};
	// Fail hook: report a read/rmw/mod_event of a never-written byte. It must
	// not return (CpuMockHost installs one that SoftQuits).
	using FailHook = std::function<void(const std::string & message)>;
	// Called once when a tracked owner has had every byte written.
	using InitHook = std::function<void(const Entry & entry)>;

	// A linked address has one owner. Invalid or overlapping registration is a
	// migration error, not a condition callers may safely ignore.
	// Returns the entry index (stable until Clear()).
	size_t Register(uint16_t addr, size_t size, std::string name, ReadFn read,
	                WriteFn write, const LinkedTrack * track = nullptr) {
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
		                         std::move(read), std::move(write), track});
		return entries_.size() - 1;
	}

	void SetFailHook(FailHook hook) { fail_hook_ = std::move(hook); }
	void SetInitHook(InitHook hook) { init_hook_ = std::move(hook); }

	// Read-before-write of an `uninitialized` Linked byte. `op` is "read",
	// "rmw" or "mod_event". Never returns.
	[[noreturn]] void FailUninitialized(size_t entry, size_t offset,
	                                    const char * op) const {
		std::string msg = Describe(entry, offset);
		msg = std::string("linked ") + op + " of an uninitialized byte: " + msg +
		      " was never written (declared `uninitialized`; the game must "
		      "write it before reading it)";
		if (fail_hook_) fail_hook_(msg);
		throw std::logic_error(msg);
	}
	// A tracked owner just became fully written.
	void NoteInitialized(size_t entry) const {
		if (init_hook_ && entry < entries_.size())
			init_hook_(entries_[entry]);
	}
	// "name[idx] ($ADDR)" for diagnostics.
	std::string Describe(size_t entry, size_t offset) const {
		if (entry >= entries_.size()) return "(unregistered)";
		const Entry & e = entries_[entry];
		char buf[48];
		if (e.size > 1)
			std::snprintf(buf, sizeof buf, "[%zu] ($%04X)", offset,
			              unsigned(e.addr + offset));
		else
			std::snprintf(buf, sizeof buf, " ($%04X)", unsigned(e.addr));
		return (e.name.empty() ? std::string("(unnamed)") : e.name) + buf;
	}
	// Registry-lifetime token: owners keep a weak_ptr so a destructor running
	// after the registry is gone (static-lifetime state) does not touch it.
	std::weak_ptr<const bool> LifeToken() const { return life_; }

	// An owner is being destroyed (the Game, with all its Linked members, is
	// destroyed before the host prints the census): drop the entry's pointer
	// into it and, if it never finished initialising, remember what is left.
	void Retire(size_t entry, const LinkedTrack * track) {
		if (entry >= entries_.size()) return;
		Entry & e = entries_[entry];
		if (!track || e.track != track) return;
		if (track->tracking)
			retired_.push_back(Pending{e.name, e.addr, e.size, track->unwritten});
		e.track = nullptr;
	}

	// `uninitialized` objects that still have unwritten bytes (live ones, plus
	// those retired by destruction).
	struct Pending {
		std::string name;
		uint16_t addr = 0;
		uint16_t size = 0;
		uint32_t unwritten = 0;
	};
	std::vector<Pending> PendingTracked() const {
		std::vector<Pending> out = retired_;
		for (const Entry & e : entries_)
			if (e.track && e.track->tracking)
				out.push_back(Pending{e.name, e.addr, e.size, e.track->unwritten});
		return out;
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
			if (e.track && e.track->tracking && !e.track->written(*e.track, offset))
				FailUninitialized(size_t(idx), offset, "mod_event");
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
		retired_.clear();
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
	FailHook fail_hook_;
	InitHook init_hook_;
	std::vector<Pending> retired_;
	std::shared_ptr<const bool> life_ = std::make_shared<const bool>(true);
};

} // namespace revm::cpumock
