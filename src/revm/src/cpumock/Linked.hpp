// Created  : 2026-09-06
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Stage-4 migration state.
//
// Linked values own ordinary local C++ storage.  The template address is
// provenance for the old KB/RAM location; it is never used to initialize the
// value and linked operations never read or write Main RAM.  When Sync::AtPc
// has armed a ticket, an explicit operation consumes it and performs the
// shared RAM timing fence before operating on the local value.
//
// A value declared `{uninitialized}` has no starting value at all: it is
// tracked byte by byte until the game has written every byte. Reading (or
// read-modify-writing, or applying a recorded mod_event to) an unwritten byte
// fails the run loudly, and the KB compare ignores unwritten bytes. Once the
// last byte is written tracking switches off and the value is ordinary.

#include "cpumock/Sync.hpp"
#include "cpumock/LinkedRegistry.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace revm::cpumock {

namespace detail {

inline bool consume_linked_at_pc(const LinkedRegistry *registry,
	                             Sync::ArmedAtPc &ticket) {
	if (!Sync::ConsumeAtPc(ticket))
		return false;
	if (!registry || !ticket.host->OwnsLinkedRegistry(registry)) {
		ticket.host->SoftQuit(
			1, "Sync::AtPc linked value is not registered with this host");
		return false;
	}
	return true;
}

inline void require_carry(bool *carry, const char *op) {
	if (!carry)
		throw std::invalid_argument(std::string(op) + " requires bool *carry");
}

inline uint8_t apply_rmw(uint8_t &value, Sync::RmwOp op, bool *carry) {
	switch (op) {
	case Sync::RmwOp::Inc: return ++value;
	case Sync::RmwOp::Dec: return --value;
	case Sync::RmwOp::Asl: {
		require_carry(carry, "asl");
		*carry = (value & 0x80u) != 0;
		return value = uint8_t(value << 1);
	}
	case Sync::RmwOp::Lsr: {
		require_carry(carry, "lsr");
		*carry = (value & 0x01u) != 0;
		return value = uint8_t(value >> 1);
	}
	case Sync::RmwOp::Rol: {
		require_carry(carry, "rol");
		const bool cout = (value & 0x80u) != 0;
		value = uint8_t(uint8_t(value << 1) | (*carry ? 1u : 0u));
		*carry = cout;
		return value;
	}
	case Sync::RmwOp::Ror: {
		require_carry(carry, "ror");
		const bool cout = (value & 0x01u) != 0;
		value = uint8_t(uint8_t(value >> 1) | (*carry ? 0x80u : 0u));
		*carry = cout;
		return value;
	}
	}
	return value;
}

// Written-state of an `uninitialized` owner: one bit per byte (a bool for a
// LinkedByte, two bits for a LinkedWord, one bit per element for an array).
// A default-constructed one is not tracking and costs one flag test.
template <size_t N>
struct WrittenBits : LinkedTrack {
	std::bitset<N> bits;

	constexpr WrittenBits() = default;
	constexpr explicit WrittenBits(Uninitialized) {
		tracking = true;
		unwritten = uint32_t(N);
		total = uint32_t(N);
		written = &probe;
	}
	static bool probe(const LinkedTrack &t, size_t offset) {
		return static_cast<const WrittenBits &>(t).bits[offset];
	}
	// Returns true when this write completed the owner (tracking just ended).
	bool mark(size_t offset) {
		if (bits[offset]) return false;
		bits.set(offset);
		if (--unwritten != 0) return false;
		tracking = false;
		return true;
	}
};

[[noreturn]] inline void fail_uninitialized(const LinkedRegistry *registry,
                                            size_t entry, uint16_t addr,
                                            size_t offset, const char *op) {
	if (registry) registry->FailUninitialized(entry, offset, op);
	throw std::logic_error(
		std::string("linked ") + op + " of an uninitialized byte at $" +
		std::to_string(unsigned(addr + offset)) +
		" (value was never registered; declared `uninitialized`)");
}

} // namespace detail

template <uint16_t Addr>
class LinkedByte {
public:
	static constexpr uint16_t addr = Addr;
	static constexpr uint16_t size = 1;

	constexpr LinkedByte(uint8_t initial = 0) : value_(initial) {}
	constexpr LinkedByte(Uninitialized) : track_(uninitialized) {}
	LinkedByte(const LinkedByte &) = delete;
	LinkedByte &operator=(const LinkedByte &) = delete;
	LinkedByte(LinkedByte &&) = delete;
	LinkedByte &operator=(LinkedByte &&) = delete;
	~LinkedByte() { retire(); }

	uint8_t read() const {
		if (track_.tracking) [[unlikely]] require_written("read");
		Sync::ArmedAtPc ticket{};
		if (!detail::consume_linked_at_pc(registry_, ticket))
			return value_;
		Sync::LinkedReadAtPc(*ticket.host, ticket.pc, Addr, ticket.slack);
		return value_;
	}

	void write(uint8_t value) {
		Sync::ArmedAtPc ticket{};
		if (detail::consume_linked_at_pc(registry_, ticket))
			Sync::LinkedWriteAtPc(*ticket.host, ticket.pc, Addr, value,
			                      ticket.slack);
		value_ = value;
		if (track_.tracking) [[unlikely]] note_written();
	}

	uint8_t inc() { return rmw(Sync::RmwOp::Inc, nullptr); }
	uint8_t dec() { return rmw(Sync::RmwOp::Dec, nullptr); }
	uint8_t asl(bool *carry) { return rmw(Sync::RmwOp::Asl, carry); }
	uint8_t lsr(bool *carry) { return rmw(Sync::RmwOp::Lsr, carry); }
	uint8_t rol(bool *carry) { return rmw(Sync::RmwOp::Rol, carry); }
	uint8_t ror(bool *carry) { return rmw(Sync::RmwOp::Ror, carry); }

	void Register(LinkedRegistry &registry, std::string name = {}) {
		if (registry_ && registry_ != &registry)
			throw std::logic_error("linked value cannot change registries");
		entry_ = registry.Register(
			Addr, 1, std::move(name), [this](size_t) { return value_; },
			[this](size_t, uint8_t value) { value_ = value; }, &track_);
		registry_ = &registry;
		life_ = registry.LifeToken();
	}
private:
	// Destruction: detach from the registry (if it still exists) so its entry
	// never dereferences this object again, and report bytes that were never
	// written to the end-of-run census.
	void retire() {
		if (registry_ && !life_.expired())
			registry_->Retire(entry_, &track_);
	}
	[[gnu::noinline, gnu::cold]] void require_written(const char *op) const {
		if (!track_.bits[0])
			detail::fail_uninitialized(registry_, entry_, Addr, 0, op);
	}
	[[gnu::noinline, gnu::cold]] void note_written() {
		if (track_.mark(0) && registry_) registry_->NoteInitialized(entry_);
	}
	uint8_t rmw(Sync::RmwOp op, bool *carry) {
		if (track_.tracking) [[unlikely]] require_written("rmw");
		Sync::ArmedAtPc ticket{};
		if (detail::consume_linked_at_pc(registry_, ticket))
			Sync::LinkedRmwAtPc(*ticket.host, ticket.pc, Addr, op, carry,
			                   ticket.slack);
		return detail::apply_rmw(value_, op, carry);
	}

	uint8_t value_ = 0;
	LinkedRegistry *registry_ = nullptr;
	size_t entry_ = 0;
	std::weak_ptr<const bool> life_;
	detail::WrittenBits<1> track_;
};

template <uint16_t Addr>
class LinkedWord {
public:
	static_assert(Addr != 0xFFFFu, "LinkedWord must fit in the C64 address space");
	static constexpr uint16_t addr = Addr;
	static constexpr uint16_t size = 2;

	constexpr LinkedWord(uint16_t initial = 0) : value_(initial) {}
	constexpr LinkedWord(Uninitialized) : track_(uninitialized) {}
	LinkedWord(const LinkedWord &) = delete;
	LinkedWord &operator=(const LinkedWord &) = delete;
	LinkedWord(LinkedWord &&) = delete;
	LinkedWord &operator=(LinkedWord &&) = delete;
	~LinkedWord() { retire(); }

	uint16_t read() const {
		// One LinkedWord operation is one logical migration fence at Addr. It
		// does not claim to model two separate 6510 byte bus accesses; use
		// LinkedByte/LinkedArray for per-byte timing.
		if (track_.tracking) [[unlikely]] require_written("read");
		Sync::ArmedAtPc ticket{};
		if (!detail::consume_linked_at_pc(registry_, ticket))
			return value_;
		Sync::LinkedReadAtPc(*ticket.host, ticket.pc, Addr, ticket.slack);
		return value_;
	}

	void write(uint16_t value) {
		Sync::ArmedAtPc ticket{};
		if (detail::consume_linked_at_pc(registry_, ticket))
			Sync::LinkedWriteAtPc(*ticket.host, ticket.pc, Addr,
			                      uint8_t(value), ticket.slack);
		value_ = value;
		if (track_.tracking) [[unlikely]] note_written();
	}

	uint16_t inc() { return rmw(Sync::RmwOp::Inc); }
	uint16_t dec() { return rmw(Sync::RmwOp::Dec); }

	void Register(LinkedRegistry &registry, std::string name = {}) {
		if (registry_ && registry_ != &registry)
			throw std::logic_error("linked value cannot change registries");
		entry_ = registry.Register(
			Addr, 2, std::move(name), [this](size_t offset) {
				return offset == 0 ? uint8_t(value_) : uint8_t(value_ >> 8);
			}, [this](size_t offset, uint8_t byte) {
				if (offset == 0)
					value_ = uint16_t((value_ & 0xFF00u) | byte);
				else
					value_ = uint16_t((value_ & 0x00FFu) | (uint16_t(byte) << 8));
			}, &track_);
		registry_ = &registry;
		life_ = registry.LifeToken();
	}
private:
	// Destruction: detach from the registry (if it still exists) so its entry
	// never dereferences this object again, and report bytes that were never
	// written to the end-of-run census.
	void retire() {
		if (registry_ && !life_.expired())
			registry_->Retire(entry_, &track_);
	}
	[[gnu::noinline, gnu::cold]] void require_written(const char *op) const {
		for (size_t i = 0; i < 2; ++i)
			if (!track_.bits[i])
				detail::fail_uninitialized(registry_, entry_, Addr, i, op);
	}
	[[gnu::noinline, gnu::cold]] void note_written() {
		const bool a = track_.mark(0);
		const bool b = track_.mark(1);
		if ((a || b) && registry_) registry_->NoteInitialized(entry_);
	}
	uint16_t rmw(Sync::RmwOp op) {
		if (track_.tracking) [[unlikely]] require_written("rmw");
		Sync::ArmedAtPc ticket{};
		if (detail::consume_linked_at_pc(registry_, ticket))
			Sync::LinkedRmwAtPc(*ticket.host, ticket.pc, Addr, op, nullptr,
			                   ticket.slack);
		value_ = op == Sync::RmwOp::Inc ? uint16_t(value_ + 1)
		                               : uint16_t(value_ - 1);
		return value_;
	}

	uint16_t value_ = 0;
	LinkedRegistry *registry_ = nullptr;
	size_t entry_ = 0;
	std::weak_ptr<const bool> life_;
	detail::WrittenBits<2> track_;
};

template <uint16_t Addr, size_t Size>
class LinkedArray {
public:
	static_assert(Size > 0, "LinkedArray must not be empty");
	static_assert(Size <= 0xFFFFu,
	              "LinkedArray size must fit in registry metadata");
	static_assert(uint32_t(Addr) + uint32_t(Size) - 1u <= 0xFFFFu,
	              "LinkedArray must fit in the C64 address space");
	static constexpr uint16_t addr = Addr;
	static constexpr uint16_t size = uint16_t(Size);

	class Element {
	public:
		uint8_t read() const { return owner_->read_at(index_); }
		void write(uint8_t value) { owner_->write_at(index_, value); }
		uint8_t inc() { return owner_->rmw_at(index_, Sync::RmwOp::Inc, nullptr); }
		uint8_t dec() { return owner_->rmw_at(index_, Sync::RmwOp::Dec, nullptr); }
		uint8_t asl(bool *carry) {
			return owner_->rmw_at(index_, Sync::RmwOp::Asl, carry);
		}
		uint8_t lsr(bool *carry) {
			return owner_->rmw_at(index_, Sync::RmwOp::Lsr, carry);
		}
		uint8_t rol(bool *carry) {
			return owner_->rmw_at(index_, Sync::RmwOp::Rol, carry);
		}
		uint8_t ror(bool *carry) {
			return owner_->rmw_at(index_, Sync::RmwOp::Ror, carry);
		}
		uint16_t address() const { return uint16_t(Addr + index_); }

	private:
		friend class LinkedArray;
		Element(LinkedArray *owner, size_t index) : owner_(owner), index_(index) {}
		LinkedArray *owner_ = nullptr;
		size_t index_ = 0;
	};
	class ConstElement {
	public:
		uint8_t read() const { return owner_->read_at(index_); }
		uint16_t address() const { return uint16_t(Addr + index_); }

	private:
		friend class LinkedArray;
		ConstElement(const LinkedArray *owner, size_t index)
			: owner_(owner), index_(index) {}
		const LinkedArray *owner_ = nullptr;
		size_t index_ = 0;
	};

	constexpr explicit LinkedArray(
		const std::array<uint8_t, Size> &initial = {}) : value_(initial) {}
	constexpr explicit LinkedArray(Uninitialized) : track_(uninitialized) {}
	LinkedArray(const LinkedArray &) = delete;
	LinkedArray &operator=(const LinkedArray &) = delete;
	LinkedArray(LinkedArray &&) = delete;
	LinkedArray &operator=(LinkedArray &&) = delete;
	~LinkedArray() { retire(); }

	Element operator[](size_t index) { return at(index); }
	ConstElement operator[](size_t index) const { return at(index); }
	Element at(size_t index) {
		if (index >= Size) throw std::out_of_range("LinkedArray index");
		return Element{this, index};
	}
	ConstElement at(size_t index) const {
		if (index >= Size) throw std::out_of_range("LinkedArray index");
		return ConstElement{this, index};
	}

	void Register(LinkedRegistry &registry, std::string name = {}) {
		if (registry_ && registry_ != &registry)
			throw std::logic_error("linked value cannot change registries");
		entry_ = registry.Register(
			Addr, Size, std::move(name),
			[this](size_t offset) { return value_[offset]; },
			[this](size_t offset, uint8_t value) { value_[offset] = value; },
			&track_);
		registry_ = &registry;
		life_ = registry.LifeToken();
	}
private:
	friend class Element;
	// Destruction: detach from the registry (if it still exists) so its entry
	// never dereferences this object again, and report bytes that were never
	// written to the end-of-run census.
	void retire() {
		if (registry_ && !life_.expired())
			registry_->Retire(entry_, &track_);
	}
	[[gnu::noinline, gnu::cold]] void require_written(size_t index, const char *op) const {
		if (!track_.bits[index])
			detail::fail_uninitialized(registry_, entry_, Addr, index, op);
	}
	[[gnu::noinline, gnu::cold]] void note_written(size_t index) {
		if (track_.mark(index) && registry_)
			registry_->NoteInitialized(entry_);
	}
	uint8_t read_at(size_t index) const {
		if (track_.tracking) [[unlikely]] require_written(index, "read");
		const uint16_t a = uint16_t(Addr + index);
		Sync::ArmedAtPc ticket{};
		if (detail::consume_linked_at_pc(registry_, ticket))
			Sync::LinkedReadAtPc(*ticket.host, ticket.pc, a, ticket.slack);
		return value_[index];
	}
	void write_at(size_t index, uint8_t value) {
		const uint16_t a = uint16_t(Addr + index);
		Sync::ArmedAtPc ticket{};
		if (detail::consume_linked_at_pc(registry_, ticket))
			Sync::LinkedWriteAtPc(*ticket.host, ticket.pc, a, value,
			                      ticket.slack);
		value_[index] = value;
		if (track_.tracking) [[unlikely]] note_written(index);
	}
	uint8_t rmw_at(size_t index, Sync::RmwOp op, bool *carry) {
		if (track_.tracking) [[unlikely]] require_written(index, "rmw");
		const uint16_t a = uint16_t(Addr + index);
		Sync::ArmedAtPc ticket{};
		if (detail::consume_linked_at_pc(registry_, ticket))
			Sync::LinkedRmwAtPc(*ticket.host, ticket.pc, a, op, carry,
			                   ticket.slack);
		return detail::apply_rmw(value_[index], op, carry);
	}

	std::array<uint8_t, Size> value_{};
	LinkedRegistry *registry_ = nullptr;
	size_t entry_ = 0;
	std::weak_ptr<const bool> life_;
	detail::WrittenBits<Size> track_;
};

} // namespace revm::cpumock

namespace revm {
// Game-facing spelling: `LinkedByte<0x0400> x{revm::uninitialized};`
using cpumock::Uninitialized;
using cpumock::uninitialized;
} // namespace revm
