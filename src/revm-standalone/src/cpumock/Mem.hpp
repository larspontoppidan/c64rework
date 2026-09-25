// Created  : 2026-08-18
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Standalone register-shaped RAM / I/O bags. All accesses are Main-only:
// explicit .read()/.write(v) and member asl/lsr/rol/ror/inc/dec RMW.
// Free asl/lsr/rol/ror/inc/dec(uint8_t&) are internal implementation helpers;
// exported game code reaches only the member RMW surface.
// Plugins must not call GameHost::Read / Write directly.

#define GAMEHOST_BARE
#include "gamehost/GameHost.hpp"
#undef GAMEHOST_BARE

#include <cstdint>

namespace revm::cpumock {

template <typename Policy> struct ByteAt;

struct RamSync {};
struct IoSync {};

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
			d().host.Fail(1, "asl requires bool *carry");
		uint8_t v = d().host.Read(d().a);
		::revm::cpumock::asl(v, *carry);
		d().host.Write(d().a, v);
		return v;
	}
	uint8_t lsr(bool *carry) {
		if (!carry)
			d().host.Fail(1, "lsr requires bool *carry");
		uint8_t v = d().host.Read(d().a);
		::revm::cpumock::lsr(v, *carry);
		d().host.Write(d().a, v);
		return v;
	}
	uint8_t rol(bool *carry) {
		if (!carry)
			d().host.Fail(1, "rol requires bool *carry");
		uint8_t v = d().host.Read(d().a);
		::revm::cpumock::rol(v, *carry);
		d().host.Write(d().a, v);
		return v;
	}
	uint8_t ror(bool *carry) {
		if (!carry)
			d().host.Fail(1, "ror requires bool *carry");
		uint8_t v = d().host.Read(d().a);
		::revm::cpumock::ror(v, *carry);
		d().host.Write(d().a, v);
		return v;
	}

private:
	Derived & d() { return static_cast<Derived &>(*this); }
	const Derived & d() const { return static_cast<const Derived &>(*this); }
};

// Runtime (host, addr) byte. Explicit read/write and member RMW only.
template <typename Policy>
struct ByteAt : LocalRmwOps<ByteAt<Policy>> {
	gamehost::GameHost & host;
	uint16_t a;

	ByteAt(gamehost::GameHost & h, uint16_t addr) : host(h), a(addr) {}

	uint8_t read() const { return host.Read(a); }
	void write(uint8_t v) { host.Write(a, v); }

	uint8_t inc() {
		return LocalRmwOps<ByteAt<Policy>>::inc();
	}
	uint8_t dec() {
		return LocalRmwOps<ByteAt<Policy>>::dec();
	}
	uint8_t asl(bool *carry) {
		return LocalRmwOps<ByteAt<Policy>>::asl(carry);
	}
	uint8_t lsr(bool *carry) {
		return LocalRmwOps<ByteAt<Policy>>::lsr(carry);
	}
	uint8_t rol(bool *carry) {
		return LocalRmwOps<ByteAt<Policy>>::rol(carry);
	}
	uint8_t ror(bool *carry) {
		return LocalRmwOps<ByteAt<Policy>>::ror(carry);
	}
};

// Compile-time address byte (Io object level). Explicit read/write and
// member RMW only.
template <typename Policy, uint16_t Addr>
struct FixedByte : LocalRmwOps<FixedByte<Policy, Addr>> {
	explicit FixedByte(gamehost::GameHost & h) : host(h) {}

	gamehost::GameHost & host;
	static constexpr uint16_t a = Addr;
	static constexpr uint16_t addr = Addr;

	uint8_t read() const { return host.Read(Addr); }
	void write(uint8_t v) { host.Write(Addr, v); }

	uint8_t inc() {
		return LocalRmwOps<FixedByte<Policy, Addr>>::inc();
	}
	uint8_t dec() {
		return LocalRmwOps<FixedByte<Policy, Addr>>::dec();
	}
	uint8_t asl(bool *carry) {
		return LocalRmwOps<FixedByte<Policy, Addr>>::asl(carry);
	}
	uint8_t lsr(bool *carry) {
		return LocalRmwOps<FixedByte<Policy, Addr>>::lsr(carry);
	}
	uint8_t rol(bool *carry) {
		return LocalRmwOps<FixedByte<Policy, Addr>>::rol(carry);
	}
	uint8_t ror(bool *carry) {
		return LocalRmwOps<FixedByte<Policy, Addr>>::ror(carry);
	}
};

// Fixed-size byte table at Addr..Addr+Size-1. operator[] Fails on OOB.
template <uint16_t Addr, uint16_t Size>
struct MemTable {
	static_assert(Size > 0, "MemTable Size must be > 0");
	static_assert(uint32_t(Addr) + Size - 1u <= 0xFFFFu,
	              "MemTable spans past $FFFF");

	explicit MemTable(gamehost::GameHost & host) : host_(host) {}

	static constexpr uint16_t addr = Addr;
	static constexpr uint16_t size = Size;

	using At = ByteAt<RamSync>;

	At operator[](int i) {
		if (unsigned(i) >= Size) {
			host_.Fail(1, "MemTable<$%04X,%u>[%d] out of range", Addr,
			               unsigned(Size), i);
			i = 0;
		}
		return At{host_, uint16_t(Addr + i)};
	}
	At operator[](int i) const {
		if (unsigned(i) >= Size) {
			host_.Fail(1, "MemTable<$%04X,%u>[%d] out of range", Addr,
			               unsigned(Size), i);
			i = 0;
		}
		return At{host_, uint16_t(Addr + i)};
	}

private:
	gamehost::GameHost & host_;
};

// 8-bit I/O space at Addr (VIC/CIA/SID/color RAM). Same accessors as the
// compile-time byte bag. Indexed access is a runtime-address ByteAt.
template <uint16_t Addr>
struct Io : FixedByte<IoSync, Addr> {
	using FixedByte<IoSync, Addr>::FixedByte;

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
// $00–$0F. Indexed access is on Io::At (runtime address), not the Io template
// base.
struct IoMap {
	struct Vic {
		explicit Vic(gamehost::GameHost & host)
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
			gamehost::GameHost & host;
			Sprite operator[](int n) const {
				if (unsigned(n) > 7u) {
					host.Fail(1, "io.vic.sp[%d] not in 0..7", n);
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
		explicit Sid(gamehost::GameHost & host)
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
			gamehost::GameHost & host;
			Voice operator[](int v) const {
				if (unsigned(v) > 2u) {
					host.Fail(1, "io.sid.voice[%d] not in 0..2", v);
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
		explicit Cia1(gamehost::GameHost & host)
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
		explicit Cia2(gamehost::GameHost & host)
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

	explicit IoMap(gamehost::GameHost & host)
		: vic(host), sid(host), cia1(host), cia2(host), color_ram(host) {}
};

} // namespace revm::cpumock
