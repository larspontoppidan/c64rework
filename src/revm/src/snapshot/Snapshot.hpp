// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Frodo chip state structs
#include "CPUC64.h"
#include "VIC.h"
#include "SID.h"
#include "CIA.h"
#include "C64.h"

namespace revm {

class Board;

// Full machine state at an exact cycle (stage 1).
//
// Version history:
//   1 — magic..cia2 only (66880 bytes on this ABI); still readable
//   2 — + VicScPipeline + ba_low (needed for cold --load-snapshot restore)
struct FullSnapshot {
	static constexpr char kMagic[16] = {'R','E','V','M','S','n','a','p','s','h','o','t','1','\0','\0','\0'};
	static constexpr uint16_t kVersion = 2;
	// On-disk size of version-1 blobs (goldens / older begin_full.bin).
	static constexpr size_t kVersion1Size = 66880;

	uint8_t magic[16]{};
	uint16_t version = kVersion;
	uint32_t cycle = 0;
	uint32_t frame = 0;

	uint8_t ram[C64_RAM_SIZE]{};
	uint8_t color[COLOR_RAM_SIZE]{};

	MOS6510State cpu{};
	MOS6569State vic{};
	MOS6581State sid{};
	MOS6526State cia1{};
	MOS6526State cia2{};

	// v2 trailer (not compared in golden playback; used by begin-snap restore).
	VicScPipeline vic_sc{};
	uint8_t ba_low = 0;
	uint8_t cia1_ta_idle = 0;
	uint8_t cia1_tb_idle = 0;
	uint8_t cia2_ta_idle = 0;
	uint8_t cia2_tb_idle = 0;
};

static_assert(offsetof(FullSnapshot, vic_sc) == FullSnapshot::kVersion1Size,
              "v1 FullSnapshot size must match trailer offset");

// Chip-only snapshot (for later stages when memory map is not required).
struct ChipSnapshot {
	static constexpr char kMagic[16] = {'R','E','V','M','C','h','i','p','S','n','a','p','1','\0','\0','\0'};
	static constexpr uint16_t kVersion = 1;

	uint8_t magic[16]{};
	uint16_t version = kVersion;
	uint32_t cycle = 0;
	uint32_t frame = 0;

	MOS6569State vic{};
	MOS6581State sid{};
	MOS6526State cia1{};
	MOS6526State cia2{};
};

// VIC framebuffer screenshot (palette indices). DISPLAY_X × DISPLAY_Y.
struct ScreenSnapshot {
	static constexpr unsigned kWidth = 0x180;   // DISPLAY_X
	static constexpr unsigned kHeight = 0x110;  // DISPLAY_Y
	static constexpr size_t kBytes = size_t(kWidth) * kHeight;

	uint32_t cycle = 0;
	uint32_t frame = 0;
	uint8_t pixels[kBytes]{};
};

bool CaptureFullSnapshot(const Board & board, FullSnapshot & out);
// Like CaptureFullSnapshot but skips VIC SC / BA / CIA-idle trailer (not used by
// golden RAM/public-chip compares).
bool CaptureCompareSnapshot(const Board & board, FullSnapshot & out);

// CPU-visible 64K under the current LORAM/HIRAM/CHAREN map (incl. ROMs / I/O).
// This is the banked-in view the 6510 sees — not raw DRAM under ROM.
bool CaptureCpuView(const Board & board, uint8_t out[C64_RAM_SIZE]);
bool WriteCpuViewFile(const Board & board, const std::string & path, std::string & error);
// Restore snap into a temporary headless Board, then dump the banked 64K view.
bool WriteCpuViewFromFullSnapshot(const FullSnapshot & snap, const std::string & path,
                                  std::string & error);
bool RestoreFullSnapshot(Board & board, const FullSnapshot & in);

bool CaptureChipSnapshot(const Board & board, ChipSnapshot & out);
bool CaptureScreenSnapshot(const Board & board, ScreenSnapshot & out);

bool SaveFullSnapshotFile(const std::string & path, const FullSnapshot & snap, std::string & error);
bool LoadFullSnapshotFile(const std::string & path, FullSnapshot & snap, std::string & error);

bool SaveChipSnapshotFile(const std::string & path, const ChipSnapshot & snap, std::string & error);
bool LoadChipSnapshotFile(const std::string & path, ChipSnapshot & snap, std::string & error);

// Headless --no-audio vs windowed+SID boot hashes of FullSnapshot after PrepareRun.
int RunBootSnapshotSelfTest(std::string & report);

} // namespace revm
