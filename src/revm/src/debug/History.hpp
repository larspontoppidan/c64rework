// Created  : 2026-10-12
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct ZSTD_CCtx_s;

namespace revm {

// Full CPU bus history of Main from cycle FROM to cycle TO (REVMHIST).
//
// Every CPU bus access is one record: opcode fetches, operand and data reads,
// dummy reads, stack and vector accesses, writes, and the bytes --load-prg
// injects without the CPU. Interrupts are simply part of the stream (their
// stack pushes and $FFFA-$FFFF vector reads show them). The file is one zstd
// stream; the decompressed bytes are:
//
//   char[8]  "REVMHIST"
//   u16      version (1)
//   u16      reserved (0)
//   u64      start cycle: Main's cycle counter at the first record
//   u32      metadata length N, then N bytes of JSON (source, options)
//   u8[65536] RAM at the start (DRAM, including RAM under ROM and I/O)
//   u8[1024]  color RAM at the start (low nybbles)
//   u8 ddr, u8 pr   CPU port $00/$01 at the start
//   u16      PC at the start
//   records, 5 bytes each:
//     u8 kind, u8 value, u16 addr, u8 dcycle
//   dcycle is the cycles since the previous record (the first counts from the
//   start cycle). A gap over 255 cycles is bridged with Tick records.
//   The stream ends with one End record, then u64 the last cycle.
//
// All multi-byte fields are little-endian.
enum class HistoryKind : uint8_t {
	Exec = 0,     // opcode fetch at addr; value = opcode
	ReadRam = 1,
	ReadRom = 2,
	ReadIo = 3,   // I/O chips, colour RAM, cartridge areas
	WriteRam = 4, // includes writes under ROM
	WriteIo = 5,
	Dma = 6,      // byte placed without the CPU (--load-prg); value = byte
	Tick = 7,     // no access; only advances time by dcycle
	End = 0xFF,
};

class HistoryWriter {
public:
	HistoryWriter() = default;
	~HistoryWriter();
	HistoryWriter(const HistoryWriter &) = delete;
	HistoryWriter & operator=(const HistoryWriter &) = delete;

	// Opens the file and writes the header and start state.
	bool Begin(const std::string & path, uint64_t start_cycle,
	           const std::string & metadata_json, const uint8_t ram[0x10000],
	           const uint8_t color_ram[0x400], uint8_t ddr, uint8_t pr, uint16_t pc,
	           std::string & error);
	bool Active() const { return file_ != nullptr; }

	void Access(uint64_t cycle, HistoryKind kind, uint16_t addr, uint8_t value);
	// The read just recorded at pc was an opcode fetch.
	void MarkExec(uint16_t pc);

	// Writes the End record and closes the file. Returns false on I/O error.
	bool Finish(uint64_t last_cycle, std::string & error);

	uint64_t RecordCount() const { return records_; }

private:
	void flush_pending();
	void put_bytes(const void * data, size_t size);
	bool compress_out(bool end_frame);

	FILE * file_ = nullptr;
	ZSTD_CCtx_s * cctx_ = nullptr;
	std::vector<uint8_t> in_;
	std::vector<uint8_t> out_;
	bool io_error_ = false;

	uint64_t last_cycle_ = 0;
	uint64_t records_ = 0;

	// The newest record is held back so MarkExec can turn it into Exec.
	bool have_pending_ = false;
	uint8_t pending_[5] = {};
};

} // namespace revm
