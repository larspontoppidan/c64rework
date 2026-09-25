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

#include "cpumock/Sync.hpp"
#include "cpumock/LinkedRegistry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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

} // namespace detail

template <uint16_t Addr>
class LinkedByte {
public:
	static constexpr uint16_t addr = Addr;
	static constexpr uint16_t size = 1;

	constexpr LinkedByte(uint8_t initial = 0) : value_(initial) {}
	LinkedByte(const LinkedByte &) = delete;
	LinkedByte &operator=(const LinkedByte &) = delete;
	LinkedByte(LinkedByte &&) = delete;
	LinkedByte &operator=(LinkedByte &&) = delete;

	uint8_t read() const {
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
		registry.Register(Addr, 1, std::move(name),
		                  [this](size_t) { return value_; },
		                  [this](size_t, uint8_t value) { value_ = value; });
		registry_ = &registry;
	}
private:
	uint8_t rmw(Sync::RmwOp op, bool *carry) {
		Sync::ArmedAtPc ticket{};
		if (detail::consume_linked_at_pc(registry_, ticket))
			Sync::LinkedRmwAtPc(*ticket.host, ticket.pc, Addr, op, carry,
			                   ticket.slack);
		return detail::apply_rmw(value_, op, carry);
	}

	uint8_t value_ = 0;
	LinkedRegistry *registry_ = nullptr;
};

template <uint16_t Addr>
class LinkedWord {
public:
	static_assert(Addr != 0xFFFFu, "LinkedWord must fit in the C64 address space");
	static constexpr uint16_t addr = Addr;
	static constexpr uint16_t size = 2;

	constexpr LinkedWord(uint16_t initial = 0) : value_(initial) {}
	LinkedWord(const LinkedWord &) = delete;
	LinkedWord &operator=(const LinkedWord &) = delete;
	LinkedWord(LinkedWord &&) = delete;
	LinkedWord &operator=(LinkedWord &&) = delete;

	uint16_t read() const {
		// One LinkedWord operation is one logical migration fence at Addr. It
		// does not claim to model two separate 6510 byte bus accesses; use
		// LinkedByte/LinkedArray for per-byte timing.
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
	}

	uint16_t inc() { return rmw(Sync::RmwOp::Inc); }
	uint16_t dec() { return rmw(Sync::RmwOp::Dec); }

	void Register(LinkedRegistry &registry, std::string name = {}) {
		if (registry_ && registry_ != &registry)
			throw std::logic_error("linked value cannot change registries");
		registry.Register(
			Addr, 2, std::move(name), [this](size_t offset) {
				return offset == 0 ? uint8_t(value_) : uint8_t(value_ >> 8);
			}, [this](size_t offset, uint8_t byte) {
				if (offset == 0)
					value_ = uint16_t((value_ & 0xFF00u) | byte);
				else
					value_ = uint16_t((value_ & 0x00FFu) | (uint16_t(byte) << 8));
			});
		registry_ = &registry;
	}
private:
	uint16_t rmw(Sync::RmwOp op) {
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
	LinkedArray(const LinkedArray &) = delete;
	LinkedArray &operator=(const LinkedArray &) = delete;
	LinkedArray(LinkedArray &&) = delete;
	LinkedArray &operator=(LinkedArray &&) = delete;

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
		registry.Register(Addr, Size, std::move(name),
		                  [this](size_t offset) { return value_[offset]; },
		                  [this](size_t offset, uint8_t value) {
			                  value_[offset] = value;
		                  });
		registry_ = &registry;
	}
private:
	friend class Element;
	uint8_t read_at(size_t index) const {
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
	}
	uint8_t rmw_at(size_t index, Sync::RmwOp op, bool *carry) {
		const uint16_t a = uint16_t(Addr + index);
		Sync::ArmedAtPc ticket{};
		if (detail::consume_linked_at_pc(registry_, ticket))
			Sync::LinkedRmwAtPc(*ticket.host, ticket.pc, a, op, carry,
			                   ticket.slack);
		return detail::apply_rmw(value_[index], op, carry);
	}

	std::array<uint8_t, Size> value_{};
	LinkedRegistry *registry_ = nullptr;
};

} // namespace revm::cpumock
