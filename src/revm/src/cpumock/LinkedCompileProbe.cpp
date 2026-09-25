// Created  : 2026-09-06
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "cpumock/Linked.hpp"

#include <type_traits>

using revm::cpumock::LinkedArray;
using revm::cpumock::LinkedByte;
using revm::cpumock::LinkedWord;
using revm::cpumock::Sync;

template <typename T>
concept ByteAssignable = requires(T &v, uint8_t x) { v = x; };
template <typename T>
concept Incrementable = requires(T &v) { ++v; };
template <typename T>
concept Decrementable = requires(T &v) { --v; };
template <typename T>
concept WordAssignable = requires(T &v, uint16_t x) { v = x; };

static_assert(!std::is_convertible_v<LinkedByte<0x003F>, uint8_t>);
static_assert(!ByteAssignable<LinkedByte<0x003F>>);
static_assert(!Incrementable<LinkedByte<0x003F>>);
static_assert(!Decrementable<LinkedByte<0x003F>>);
static_assert(!WordAssignable<LinkedWord<0x0040>>);
static_assert(!std::is_convertible_v<LinkedWord<0x0040>, uint16_t>);

void linked_compile_probe(revm::cpumock::CpuMockHost &host) {
	host.InstallMainStart({
		.entry_pc = 0x1234,
		.cycle = 12345678,
		.frame = 628,
	});
	LinkedByte<0x003F> b = 1;
	LinkedWord<0x0040> w = 0x1234;
	LinkedArray<0x0050, 4> a{};
	b.Register(host.Links(), "b");
	w.Register(host.Links(), "w");
	a.Register(host.Links(), "a");
	const uint8_t old = b.read();
	b.write(old);
	a[1].write(a[1].read());
	Sync::AtPc(host, 0x1234);
	b.write(2);
	(void)w.read();
}
