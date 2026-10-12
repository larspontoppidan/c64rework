// Created  : 2026-10-12
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// State-hash records (--state-hashes FILE): one CRC32 per channel per frame of
// what the player sees and hears. They are made on demand to prove that two
// runs are the same (export == Stage 4.5, a refactor changed nothing, where a
// no-twin replay drifts) and then thrown away; they are never stored in plays
// and never a gate.
//
// This file is shared byte-for-byte with the standalone runtime, so it only
// depends on the Frodo chips, never on a Board.
//
// File format (all integers little-endian):
//   char     magic[8]        "REVMHASH"
//   uint32   version         1
//   uint32   channel_count   N (5 today)
//   uint32   start_frame     frame number of the first record
//   char     names[N][8]     channel names, NUL-padded
//   then one record per frame: uint32 crc[N]
// The frame count is (file size - header size) / (4 * N), so a run that
// stops early still leaves a valid file (a partial last record is ignored).
//
// Channels (standard CRC-32, util/Crc32.hpp):
//   screen  the indexed frame buffer --save-screen writes (DISPLAY_X*DISPLAY_Y)
//   sid     every SID register write of the frame, in order, as
//           uint32 cycle-in-frame, uint8 register, uint8 value. Cycle-in-frame
//           counts from the previous recorded VBLANK (before the first one it
//           is the machine cycle counter).
//   vic     the VIC registers $D000-$D02E as a program reads them (unused
//           bits as 1; collision registers without clearing them) plus the
//           raster IRQ compare line (uint16), at the VBLANK
//   cia1    CIA register state at the VBLANK: the driven port bits (PRA AND
//   cia2    DDRA, PRB AND DDRB) and DDRA DDRB, timer A and B counters and
//           latches, TOD and alarm, SDR, pending and enabled interrupts, CRA
//           CRB. Port input pins are not included.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class C64;

namespace revm {

class StateHashRecorder {
public:
	static constexpr uint32_t kVersion = 1;
	static constexpr uint32_t kChannelCount = 5;
	static constexpr size_t kNameBytes = 8;

	StateHashRecorder() = default;
	~StateHashRecorder();
	StateHashRecorder(const StateHashRecorder &) = delete;
	StateHashRecorder & operator=(const StateHashRecorder &) = delete;

	// Remember the output path; the file is created at the first frame so the
	// header can carry that frame's number.
	void Configure(const std::string & path);
	bool Enabled() const { return !path_.empty(); }

	// Install the SID write tap on `machine`. Call once the machine exists.
	void Attach(C64 * machine);

	// Called at every recorded VBLANK with the frame number and the machine
	// cycle counter at that VBLANK. Writes one record.
	bool OnFrame(uint32_t frame, uint32_t vblank_cycle, std::string & error);

	// Flush and close; reports a write error that happened earlier.
	bool Finish(std::string & error);

	static const char * ChannelName(uint32_t channel);

private:
	struct SidWrite {
		uint32_t cycle;
		uint8_t reg;
		uint8_t value;
	};

	static void sid_write_tap(uint16_t adr, uint8_t byte, void * userdata);
	bool open_file(uint32_t start_frame, std::string & error);
	bool write_bytes(const void * data, size_t size);
	uint32_t hash_screen() const;
	uint32_t hash_sid() const;
	uint32_t hash_vic() const;
	uint32_t hash_cia(int which) const;

	std::string path_;
	FILE * file_ = nullptr;
	C64 * machine_ = nullptr;
	bool have_first_frame_ = false;
	uint32_t next_frame_ = 0;
	uint32_t frame_start_cycle_ = 0;
	std::vector<SidWrite> sid_writes_;
	std::string error_;
};

// Reader used by `revm-tool hash-compare`.
struct StateHashFile {
	uint32_t version = 0;
	uint32_t start_frame = 0;
	std::vector<std::string> channels;
	// records[i * channels.size() + c] is channel c of frame start_frame + i.
	std::vector<uint32_t> records;

	size_t FrameCount() const {
		return channels.empty() ? 0 : records.size() / channels.size();
	}
};

bool ReadStateHashFile(const std::string & path, StateHashFile & out,
                       std::string & error);

} // namespace revm
