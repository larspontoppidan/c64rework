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
// Five 64K planes of u16 saturating hit counts:
//   pc_ram  — opcode fetches that resolved to RAM
//   pc_rom  — opcode fetches that resolved to ROM (BASIC/KERNAL/CHAR/cart)
//   mem_ram — data R/W that resolved to RAM (incl. under-ROM writes)
//   mem_rom — data reads that resolved to ROM
//   vic_ram — RAM bytes the VIC fetched for display (screen, charset,
//             bitmap, sprite pointers and data); not refresh or idle reads
//
// I/O ($D000 when CHAREN selects I/O) is not recorded in MEM planes.
//
// On-disk: magic "REVMCOV2" + u16 version + u16 flags + planes×65536×u16.
// Version 1 has the first four planes; version 2 adds vic_ram, and flags
// bit 0 says it was recorded. A version 1 file loads with vic_ram empty and
// VicRecorded() false: "not recorded", not "nothing fetched".
class Coverage {
public:
	static constexpr size_t kPlaneAddrs = 0x10000;
	static constexpr char kMagic[8] = {'R', 'E', 'V', 'M', 'C', 'O', 'V', '2'};
	static constexpr uint16_t kVersion = 2;
	static constexpr uint16_t kFlagVicRecorded = 0x0001;
	static constexpr size_t kFileSizeV1 =
		8 + 2 + 2 + 4 * kPlaneAddrs * sizeof(uint16_t); // 524300
	static constexpr size_t kFileSize =
		8 + 2 + 2 + 5 * kPlaneAddrs * sizeof(uint16_t); // 655372

	enum class Plane : unsigned {
		PcRam = 0,
		PcRom = 1,
		MemRam = 2,
		MemRom = 3,
		VicRam = 4,
		Count = 5,
	};

	void Clear();

	void MarkPcRam(uint16_t addr) { add(Plane::PcRam, addr); }
	void MarkPcRom(uint16_t addr) { add(Plane::PcRom, addr); }
	void MarkMemRam(uint16_t addr) { add(Plane::MemRam, addr); }
	void MarkMemRom(uint16_t addr) { add(Plane::MemRom, addr); }
	void MarkVicRam(uint16_t addr) { add(Plane::VicRam, addr); }

	bool HitPcRam(uint16_t addr) const { return Get(Plane::PcRam, addr) != 0; }
	bool HitPcRom(uint16_t addr) const { return Get(Plane::PcRom, addr) != 0; }
	bool HitMemRam(uint16_t addr) const { return Get(Plane::MemRam, addr) != 0; }
	bool HitMemRom(uint16_t addr) const { return Get(Plane::MemRom, addr) != 0; }
	bool HitVicRam(uint16_t addr) const { return Get(Plane::VicRam, addr) != 0; }

	// Whether the vic_ram plane holds a recording. False for version 1 files
	// and for a merge that includes one.
	bool VicRecorded() const { return vic_recorded_; }
	void SetVicRecorded(bool recorded) { vic_recorded_ = recorded; }

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
	bool vic_recorded_ = true;
};

// Temp-file roundtrip / merge saturating self-test. Returns 0 on success.
int RunCoverageSelfTest(std::string & report);

} // namespace revm
