// Created  : 2026-07-23
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace revm {

// Multi-plane coverage (REVMCOV2).
//
// Four 64K planes of u16 saturating hit counts:
//   pc_ram  — opcode fetches that resolved to RAM
//   pc_rom  — opcode fetches that resolved to ROM (BASIC/KERNAL/CHAR/cart)
//   mem_ram — data R/W that resolved to RAM (incl. under-ROM writes)
//   mem_rom — data reads that resolved to ROM
//
// I/O ($D000 when CHAREN selects I/O) is not recorded in MEM planes.
//
// On-disk: magic "REVMCOV2" + u16 version=1 + u16 reserved + 4×65536×u16.
class Coverage {
public:
	static constexpr size_t kPlaneAddrs = 0x10000;
	static constexpr char kMagic[8] = {'R', 'E', 'V', 'M', 'C', 'O', 'V', '2'};
	static constexpr uint16_t kVersion = 1;
	static constexpr size_t kFileSize =
		8 + 2 + 2 + 4 * kPlaneAddrs * sizeof(uint16_t); // 524300

	enum class Plane : unsigned {
		PcRam = 0,
		PcRom = 1,
		MemRam = 2,
		MemRom = 3,
		Count = 4,
	};

	void Clear();

	void MarkPcRam(uint16_t addr) { add(Plane::PcRam, addr); }
	void MarkPcRom(uint16_t addr) { add(Plane::PcRom, addr); }
	void MarkMemRam(uint16_t addr) { add(Plane::MemRam, addr); }
	void MarkMemRom(uint16_t addr) { add(Plane::MemRom, addr); }

	bool HitPcRam(uint16_t addr) const { return Get(Plane::PcRam, addr) != 0; }
	bool HitPcRom(uint16_t addr) const { return Get(Plane::PcRom, addr) != 0; }
	bool HitMemRam(uint16_t addr) const { return Get(Plane::MemRam, addr) != 0; }
	bool HitMemRom(uint16_t addr) const { return Get(Plane::MemRom, addr) != 0; }

	uint16_t Get(Plane p, uint16_t addr) const;

	// Stage-2 default: PC in RAM (game code).
	bool Hit(uint16_t addr) const { return HitPcRam(addr); }
	void Mark(uint16_t addr) { MarkPcRam(addr); }

	// Number of addresses with nonzero count.
	size_t Count(Plane p) const;
	size_t Count() const { return Count(Plane::PcRam); }

	// Sum of all counts on a plane (u64; each cell caps at 0xFFFF).
	uint64_t TotalHits(Plane p) const;

	bool Save(const std::string & path, std::string & error) const;
	bool Load(const std::string & path, std::string & error);

	void Merge(const Coverage & other);

	// Coalesce nonzero hits on one plane → [lo, hi) ranges.
	std::vector<std::pair<uint16_t, uint16_t>> Coalesce(
		Plane p = Plane::PcRam, uint16_t max_gap = 0) const;
	std::vector<std::pair<uint16_t, uint16_t>> Coalesce(uint16_t max_gap) const {
		return Coalesce(Plane::PcRam, max_gap);
	}

private:
	void add(Plane p, uint16_t addr);
	bool hit(Plane p, uint16_t addr) const { return Get(p, addr) != 0; }

	uint16_t counts_[static_cast<unsigned>(Plane::Count)][kPlaneAddrs]{};
};

// Temp-file roundtrip / merge saturating self-test. Returns 0 on success.
int RunCoverageSelfTest(std::string & report);

} // namespace revm
