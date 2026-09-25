// Created  : 2026-08-18
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Replica RAM / I/O bags. Explicit .read()/.write(v) are the canonical access
// surface; ordinary assignment and uint8_t(mem.x) remain compatibility-only
// operations and never fence Twin. A timing sample is Sync::AtPc(host, pc[,
// slack]) followed by one explicit .read(), .write(v), or member RMW operation.
// Routine joins are Sync::JoinAtPc.
// Local mem.*/io.* asl/lsr/rol/ror/inc/dec (no AtPc) are Main-only RMW.
// Free asl/lsr/rol/ror/inc/dec(uint8_t&) are register/temp helpers.
// Plugins must not call CpuMockHost::Read / Write.

#include "cpumock/CpuMockHost.hpp"
#include "cpumock/Sync.hpp"

#include <cstdint>

namespace revm::cpumock {

// Sync backends for ByteAt / FixedByte (RAM vs I/O AtPc protocol).
struct RamSync {
	static constexpr bool kSlack = true;
	static uint8_t readArmed(CpuMockHost & h, uint16_t pc, uint16_t a,
	                         FenceSlack slack) {
		return Sync::ReadRamAtPc(h, pc, a, slack);
	}
	static void writeArmed(CpuMockHost & h, uint16_t pc, uint16_t a, uint8_t v,
	                       FenceSlack slack) {
		Sync::WriteRamAtPc(h, pc, a, v, slack);
	}
	static uint8_t rmwArmed(CpuMockHost & h, uint16_t pc, uint16_t a,
	                        Sync::RmwOp op, bool *carry, FenceSlack slack) {
		return Sync::RmwRamAtPc(h, pc, a, op, carry, slack);
	}
};

struct IoSync {
	static constexpr bool kSlack = false;
	static uint8_t readArmed(CpuMockHost & h, uint16_t pc, uint16_t a) {
		return Sync::ReadIoAtPc(h, pc, a);
	}
	static void writeArmed(CpuMockHost & h, uint16_t pc, uint16_t a, uint8_t v) {
		Sync::WriteIoAtPc(h, pc, a, v);
	}
	static uint8_t rmwArmed(CpuMockHost & h, uint16_t pc, uint16_t a,
	                        Sync::RmwOp op, bool *carry) {
		return Sync::RmwIoAtPc(h, pc, a, op, carry);
	}
};

// --- Register / temp RMW (A, X, Y, locals). No host, no Twin fence. ---
// Shifts take C by reference (in for ROL/ROR, out for all four).

inline uint8_t inc(uint8_t & v) { return v = uint8_t(v + 1); }
inline uint8_t dec(uint8_t & v) { return v = uint8_t(v - 1); }

inline uint8_t asl(uint8_t & v, bool & carry) {
	carry = (v & 0x80) != 0;
	return v = uint8_t(v << 1);
}
inline uint8_t lsr(uint8_t & v, bool & carry) {
	carry = (v & 0x01) != 0;
	return v = uint8_t(v >> 1);
}
inline uint8_t rol(uint8_t & v, bool & carry) {
	const bool cout = (v & 0x80) != 0;
	v = uint8_t(uint8_t(v << 1) | (carry ? 1u : 0u));
	carry = cout;
	return v;
}
inline uint8_t ror(uint8_t & v, bool & carry) {
	const bool cout = (v & 0x01) != 0;
	v = uint8_t(uint8_t(v >> 1) | (carry ? 0x80u : 0u));
	carry = cout;
	return v;
}

// Local (non-AtPc) memory RMW: Main Read/apply/Write, no Twin fence.
// Same carry rules as *AtPc (required for shifts). Derived has host + a.
template <typename Derived>
struct LocalRmwOps {
	uint8_t inc() {
		uint8_t v = d().host.Read(d().a);
		::revm::cpumock::inc(v);
		d().host.Write(d().a, v);
		return v;
	}
	uint8_t dec() {
		uint8_t v = d().host.Read(d().a);
		::revm::cpumock::dec(v);
		d().host.Write(d().a, v);
		return v;
	}
	uint8_t asl(bool *carry) {
		if (!carry)
			d().host.SoftQuit(1, "asl requires bool *carry");
		uint8_t v = d().host.Read(d().a);
		::revm::cpumock::asl(v, *carry);
		d().host.Write(d().a, v);
		return v;
	}
	uint8_t lsr(bool *carry) {
		if (!carry)
			d().host.SoftQuit(1, "lsr requires bool *carry");
		uint8_t v = d().host.Read(d().a);
		::revm::cpumock::lsr(v, *carry);
		d().host.Write(d().a, v);
		return v;
	}
	uint8_t rol(bool *carry) {
		if (!carry)
			d().host.SoftQuit(1, "rol requires bool *carry");
		uint8_t v = d().host.Read(d().a);
		::revm::cpumock::rol(v, *carry);
		d().host.Write(d().a, v);
		return v;
	}
	uint8_t ror(bool *carry) {
		if (!carry)
			d().host.SoftQuit(1, "ror requires bool *carry");
		uint8_t v = d().host.Read(d().a);
		::revm::cpumock::ror(v, *carry);
		d().host.Write(d().a, v);
		return v;
	}

private:
	Derived & d() { return static_cast<Derived &>(*this); }
	const Derived & d() const { return static_cast<const Derived &>(*this); }
};

// AtPc / RMW surface. Derived exposes CpuMockHost & host and uint16_t a
// (runtime) or static constexpr uint16_t a (fixed Addr).
// RAM policies take FenceSlack; I/O policies do not (VBLANK is retried internally).
template <typename Derived, typename Policy>
struct AtPcOps {
	// Canonical two-line migration surface. These methods also provide ordinary
	// Main-only access when no marker is armed. The implicit conversion and
	// assignment operators below are compatibility shims only. When Sync::AtPc
	// armed a marker, consume it before entering the existing Policy::*AtPc
	// path; that path remains responsible for all Twin walking, IRQ/NMI
	// dispatch, and no-Twin behavior.
	uint8_t read() const {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(d().host, ticket))
			return d().get();
		if constexpr (Policy::kSlack)
			return Policy::readArmed(d().host, ticket.pc, d().a, ticket.slack);
		else
			return Policy::readArmed(d().host, ticket.pc, d().a);
	}

	void write(uint8_t v) {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(d().host, ticket)) {
			d().set(v);
			return;
		}
		if constexpr (Policy::kSlack)
			Policy::writeArmed(d().host, ticket.pc, d().a, v, ticket.slack);
		else
			Policy::writeArmed(d().host, ticket.pc, d().a, v);
	}

	static uint8_t rmwArmed(
		Derived &d, const Sync::ArmedAtPc &ticket, Sync::RmwOp op,
		bool *carry) {
		if constexpr (Policy::kSlack)
			return Policy::rmwArmed(d.host, ticket.pc, d.a, op, carry,
			                        ticket.slack);
		else
			return Policy::rmwArmed(d.host, ticket.pc, d.a, op, carry);
	}

private:
	Derived & d() { return static_cast<Derived &>(*this); }
	const Derived & d() const { return static_cast<const Derived &>(*this); }
};

// Runtime (host, addr) byte. Explicit read/write are canonical; they are
// ordinary Main-only R/W without a marker and use the AtPc protocol when
// Sync::AtPc arms them. Implicit conversion/assignment and get/set remain
// compatibility-only Main accesses. Local asl/lsr/rol/ror/inc/dec are also
// Main-only without a marker.
template <typename Policy>
struct ByteAt : AtPcOps<ByteAt<Policy>, Policy>, LocalRmwOps<ByteAt<Policy>> {
	CpuMockHost & host;
	uint16_t a;

	ByteAt(CpuMockHost & h, uint16_t addr) : host(h), a(addr) {}

	ByteAt & operator=(uint8_t v) {
		host.Write(a, v);
		return *this;
	}
	ByteAt & operator=(const ByteAt & o) {
		host.Write(a, o.host.Read(o.a));
		return *this;
	}
	operator uint8_t() const { return host.Read(a); }

	uint8_t get() const { return host.Read(a); }
	void set(uint8_t v) {
		host.Write(a, v);
	}

	uint8_t inc() {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<ByteAt<Policy>>::inc();
		return AtPcOps<ByteAt<Policy>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Inc, nullptr);
	}
	uint8_t dec() {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<ByteAt<Policy>>::dec();
		return AtPcOps<ByteAt<Policy>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Dec, nullptr);
	}
	uint8_t asl(bool *carry) {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<ByteAt<Policy>>::asl(carry);
		return AtPcOps<ByteAt<Policy>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Asl, carry);
	}
	uint8_t lsr(bool *carry) {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<ByteAt<Policy>>::lsr(carry);
		return AtPcOps<ByteAt<Policy>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Lsr, carry);
	}
	uint8_t rol(bool *carry) {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<ByteAt<Policy>>::rol(carry);
		return AtPcOps<ByteAt<Policy>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Rol, carry);
	}
	uint8_t ror(bool *carry) {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<ByteAt<Policy>>::ror(carry);
		return AtPcOps<ByteAt<Policy>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Ror, carry);
	}
};

// Compile-time address byte (Mem / Io object level). Explicit read/write are
// canonical; implicit conversion/assignment and get/set are compatibility
// shims for existing projects.
template <typename Policy, uint16_t Addr>
struct FixedByte : AtPcOps<FixedByte<Policy, Addr>, Policy>,
                   LocalRmwOps<FixedByte<Policy, Addr>> {
	explicit FixedByte(CpuMockHost & h) : host(h) {}

	CpuMockHost & host;
	static constexpr uint16_t a = Addr;
	static constexpr uint16_t addr = Addr;

	FixedByte & operator=(uint8_t v) {
		host.Write(Addr, v);
		return *this;
	}
	operator uint8_t() const { return host.Read(Addr); }

	uint8_t get() const { return host.Read(Addr); }
	void set(uint8_t v) {
		host.Write(Addr, v);
	}

	uint8_t inc() {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<FixedByte<Policy, Addr>>::inc();
		return AtPcOps<FixedByte<Policy, Addr>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Inc, nullptr);
	}
	uint8_t dec() {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<FixedByte<Policy, Addr>>::dec();
		return AtPcOps<FixedByte<Policy, Addr>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Dec, nullptr);
	}
	uint8_t asl(bool *carry) {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<FixedByte<Policy, Addr>>::asl(carry);
		return AtPcOps<FixedByte<Policy, Addr>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Asl, carry);
	}
	uint8_t lsr(bool *carry) {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<FixedByte<Policy, Addr>>::lsr(carry);
		return AtPcOps<FixedByte<Policy, Addr>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Lsr, carry);
	}
	uint8_t rol(bool *carry) {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<FixedByte<Policy, Addr>>::rol(carry);
		return AtPcOps<FixedByte<Policy, Addr>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Rol, carry);
	}
	uint8_t ror(bool *carry) {
		Sync::ArmedAtPc ticket{};
		if (!Sync::ConsumeAtPc(host, ticket))
			return LocalRmwOps<FixedByte<Policy, Addr>>::ror(carry);
		return AtPcOps<FixedByte<Policy, Addr>, Policy>::rmwArmed(
			*this, ticket, Sync::RmwOp::Ror, carry);
	}
};

// 8-bit RAM/ROM scalar at Addr. Tables use MemTable (range-checked []).
template <uint16_t Addr>
struct Mem : FixedByte<RamSync, Addr> {
	using FixedByte<RamSync, Addr>::FixedByte;
	using FixedByte<RamSync, Addr>::operator=;
};

// Fixed-size byte table at Addr..Addr+Size-1. operator[] SoftQuits on OOB.
template <uint16_t Addr, uint16_t Size>
struct MemTable {
	static_assert(Size > 0, "MemTable Size must be > 0");
	static_assert(uint32_t(Addr) + Size - 1u <= 0xFFFFu,
	              "MemTable spans past $FFFF");

	explicit MemTable(CpuMockHost & host) : host_(host) {}

	static constexpr uint16_t addr = Addr;
	static constexpr uint16_t size = Size;

	using At = ByteAt<RamSync>;

	At operator[](int i) {
		if (unsigned(i) >= Size) {
			host_.SoftQuit(1, "MemTable<$%04X,%u>[%d] out of range", Addr,
			               unsigned(Size), i);
			i = 0;
		}
		return At{host_, uint16_t(Addr + i)};
	}
	At operator[](int i) const {
		if (unsigned(i) >= Size) {
			host_.SoftQuit(1, "MemTable<$%04X,%u>[%d] out of range", Addr,
			               unsigned(Size), i);
			i = 0;
		}
		return At{host_, uint16_t(Addr + i)};
	}

private:
	CpuMockHost & host_;
};

// Little-endian 16-bit word at Addr / Addr+1.
// operator[] indexes the word itself (off 0 = lo, 1 = hi); SoftQuit otherwise.
// at(off) is (ptr),Y: byte at read() + off; SoftQuit if off not in 0..255.
// Both return ByteAt (full ordinary + AtPc / RMW utility).
template <uint16_t Addr>
struct Mem16 {
	explicit Mem16(CpuMockHost & host) : host_(host) {}

	static constexpr uint16_t addr = Addr;

	using At = ByteAt<RamSync>;

	// Canonical word access. A Mem16 operation is two ordinary RAM byte
	// accesses; it has no single-byte AtPc form. Reject an armed byte marker
	// instead of allowing it to leak through and fence a later operation.
	uint16_t read() const {
		Sync::RejectArmed(host_, "Mem16::read");
		return uint16_t(host_.Read(Addr) |
		                (uint16_t(host_.Read(uint16_t(Addr + 1))) << 8));
	}
	void write(uint16_t v) {
		Sync::RejectArmed(host_, "Mem16::write");
		host_.Write(Addr, uint8_t(v));
		host_.Write(uint16_t(Addr + 1), uint8_t(v >> 8));
	}

	// Compatibility-only operators. New code should use read()/write().
	Mem16 & operator=(uint16_t v) {
		write(v);
		return *this;
	}

	operator uint16_t() const {
		return read();
	}

	uint16_t get() const { return read(); }
	void set(uint16_t v) { write(v); }

	At operator[](int off) {
		if (unsigned(off) > 1u) {
			host_.SoftQuit(1, "Mem16<$%04X>[%d] not in 0..1", Addr, off);
			off = 0;
		}
		return At{host_, uint16_t(Addr + off)};
	}
	At operator[](int off) const {
		if (unsigned(off) > 1u) {
			host_.SoftQuit(1, "Mem16<$%04X>[%d] not in 0..1", Addr, off);
			off = 0;
		}
		return At{host_, uint16_t(Addr + off)};
	}

	At at(int off) {
		if (unsigned(off) > 255u) {
			host_.SoftQuit(1, "Mem16<$%04X>.at(%d) Y out of range", Addr, off);
			off = 0;
		}
		// Address formation is not the pointed-to byte access. Preserve an
		// armed marker for the explicit operation on the returned ByteAt.
		const uint16_t ptr = uint16_t(host_.Read(Addr) |
			(uint16_t(host_.Read(uint16_t(Addr + 1))) << 8));
		return At{host_, uint16_t(ptr + uint8_t(off))};
	}
	At at(int off) const {
		if (unsigned(off) > 255u) {
			host_.SoftQuit(1, "Mem16<$%04X>.at(%d) Y out of range", Addr, off);
			off = 0;
		}
		const uint16_t ptr = uint16_t(host_.Read(Addr) |
			(uint16_t(host_.Read(uint16_t(Addr + 1))) << 8));
		return At{host_, uint16_t(ptr + uint8_t(off))};
	}

private:
	CpuMockHost & host_;
};

// 8-bit I/O space at Addr (VIC/CIA/SID/color RAM). Same accessors as Mem;
// AtPc uses the I/O protocol (opcode PC → pc_after, Twin chip operand).
// Object-level AtPc uses Addr (cia1.talo). Indexed AtPc is on At (runtime
// address).
template <uint16_t Addr>
struct Io : FixedByte<IoSync, Addr> {
	using FixedByte<IoSync, Addr>::FixedByte;
	using FixedByte<IoSync, Addr>::operator=;

	using At = ByteAt<IoSync>;

	At operator[](int i) {
		return At{this->host, uint16_t(Addr + i)};
	}
	At operator[](int i) const {
		return At{this->host, uint16_t(Addr + i)};
	}
};

// Stock chip set used by current replicas. VIC/SID are two-layer chips:
// raw[offset] at the canonical base (never $D040 / $D500 mirrors) plus named
// registers. VIC names $D010–$D026; sprite X/Y/color are sp[0..7]. SID names
// the full public file $D400–$D41C. $D500-page mirrors are the same 32-byte
// register file; a write to +18 is `vol`. CIA1/CIA2 name the full 6526 file
// $00–$0F. Indexed AtPc is on Io::At (runtime address), not the Io template
// base.
struct IoMap {
	struct Vic {
		explicit Vic(CpuMockHost & host)
			: raw(host), msigx(host), scroly(host), raster(host), lpx(host),
			  lpy(host), spena(host), scrolx(host), spexpy(host),
			  vmcsb(host), irr(host), irqmask(host), spbgpr(host),
			  spmc(host), spexpx(host), spspcl(host), spbgcl(host),
			  border(host), bg0(host), bg1(host), bg2(host), bg3(host),
			  spmc0(host), spmc1(host), sp{host} {}

		Io<0xD000> raw;
		Io<0xD010> msigx;
		Io<0xD011> scroly;
		Io<0xD012> raster;
		Io<0xD013> lpx;
		Io<0xD014> lpy;
		Io<0xD015> spena;
		Io<0xD016> scrolx;
		Io<0xD017> spexpy;
		Io<0xD018> vmcsb;
		Io<0xD019> irr;
		Io<0xD01A> irqmask;
		Io<0xD01B> spbgpr;
		Io<0xD01C> spmc;
		Io<0xD01D> spexpx;
		Io<0xD01E> spspcl;
		Io<0xD01F> spbgcl;
		Io<0xD020> border;
		Io<0xD021> bg0;
		Io<0xD022> bg1;
		Io<0xD023> bg2;
		Io<0xD024> bg3;
		Io<0xD025> spmc0;
		Io<0xD026> spmc1;

		struct Sprite {
			Io<0xD000>::At x;
			Io<0xD000>::At y;
			Io<0xD000>::At color;
		};

		struct Sprites {
			CpuMockHost & host;
			Sprite operator[](int n) const {
				if (unsigned(n) > 7u) {
					host.SoftQuit(1, "io.vic.sp[%d] not in 0..7", n);
					n = 0;
				}
				return Sprite{
					Io<0xD000>::At{host, uint16_t(0xD000 + n * 2)},
					Io<0xD000>::At{host, uint16_t(0xD001 + n * 2)},
					Io<0xD000>::At{host, uint16_t(0xD027 + n)},
				};
			}
		} sp;
	} vic;

	struct Sid {
		explicit Sid(CpuMockHost & host)
			: raw(host), fc_lo(host), fc_hi(host), res_filt(host), vol(host),
			  pot_x(host), pot_y(host), osc3(host), env3(host), voice{host} {}

		Io<0xD400> raw;
		Io<0xD415> fc_lo;
		Io<0xD416> fc_hi;
		Io<0xD417> res_filt;
		Io<0xD418> vol;
		Io<0xD419> pot_x;
		Io<0xD41A> pot_y;
		Io<0xD41B> osc3;
		Io<0xD41C> env3;

		struct Voice {
			Io<0xD400>::At freq_lo;
			Io<0xD400>::At freq_hi;
			Io<0xD400>::At pw_lo;
			Io<0xD400>::At pw_hi;
			Io<0xD400>::At ctrl;
			Io<0xD400>::At ad;
			Io<0xD400>::At sr;
		};

		struct Voices {
			CpuMockHost & host;
			Voice operator[](int v) const {
				if (unsigned(v) > 2u) {
					host.SoftQuit(1, "io.sid.voice[%d] not in 0..2", v);
					v = 0;
				}
				const uint16_t b = uint16_t(0xD400 + v * 7);
				return Voice{
					Io<0xD400>::At{host, b},
					Io<0xD400>::At{host, uint16_t(b + 1)},
					Io<0xD400>::At{host, uint16_t(b + 2)},
					Io<0xD400>::At{host, uint16_t(b + 3)},
					Io<0xD400>::At{host, uint16_t(b + 4)},
					Io<0xD400>::At{host, uint16_t(b + 5)},
					Io<0xD400>::At{host, uint16_t(b + 6)},
				};
			}
		} voice;
	} sid;

	struct Cia1 {
		explicit Cia1(CpuMockHost & host)
			: pra(host), prb(host), ddra(host), ddrb(host), talo(host),
			  tahi(host), tblo(host), tbhi(host), tod10(host), todsec(host),
			  todmin(host), todhrs(host), sdr(host), icr(host), cra(host),
			  crb(host) {}
		Io<0xDC00> pra;
		Io<0xDC01> prb;
		Io<0xDC02> ddra;
		Io<0xDC03> ddrb;
		Io<0xDC04> talo;
		Io<0xDC05> tahi;
		Io<0xDC06> tblo;
		Io<0xDC07> tbhi;
		Io<0xDC08> tod10;
		Io<0xDC09> todsec;
		Io<0xDC0A> todmin;
		Io<0xDC0B> todhrs;
		Io<0xDC0C> sdr;
		Io<0xDC0D> icr;
		Io<0xDC0E> cra;
		Io<0xDC0F> crb;
	} cia1;

	struct Cia2 {
		explicit Cia2(CpuMockHost & host)
			: pra(host), prb(host), ddra(host), ddrb(host), talo(host),
			  tahi(host), tblo(host), tbhi(host), tod10(host), todsec(host),
			  todmin(host), todhrs(host), sdr(host), icr(host), cra(host),
			  crb(host) {}
		Io<0xDD00> pra;
		Io<0xDD01> prb;
		Io<0xDD02> ddra;
		Io<0xDD03> ddrb;
		Io<0xDD04> talo;
		Io<0xDD05> tahi;
		Io<0xDD06> tblo;
		Io<0xDD07> tbhi;
		Io<0xDD08> tod10;
		Io<0xDD09> todsec;
		Io<0xDD0A> todmin;
		Io<0xDD0B> todhrs;
		Io<0xDD0C> sdr;
		Io<0xDD0D> icr;
		Io<0xDD0E> cra;
		Io<0xDD0F> crb;
	} cia2;

	Io<0xD800> color_ram;

	explicit IoMap(CpuMockHost & host)
		: vic(host), sid(host), cia1(host), cia2(host), color_ram(host) {}
};

} // namespace revm::cpumock
