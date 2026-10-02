// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "roms/Roms.hpp"
#include "input/InputState.hpp"
#include "input/MemoryModification.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace revm {

// Standalone play log (JSON). Input track for recording and replay.
// Events are sparse sticky InputFrame changes keyed by absolute VSYNC frame
// (JSON object input_events: { "234": ["K SPACE"], "238": [], … }).
// Each value lists currently held inputs as strings:
//   "J1 up"|"J1 down"|"J1 left"|"J1 right"|"J1 fire" (same for J2),
//   "K SPACE"|"K F1"|… (Frodo C64 key names, rest of string after "K ").
// Absence = released. Kind is always the token before the first space.
// Memory modifications are non-sticky operations at a VSYNC, keyed by frame:
//   "mod_events": { "234": [{"address": 49152, "operation": "increment"}] }
// Optional "no-twin": true when recorded under --no-twin. Absent = original.
struct PlaySource {
	std::string type; // "prg" | "d64" | "snapshot"
	std::string path;
	std::string sha256;
	// Older D64 plays omitted this and replayed with autoload enabled.
	bool auto_load_d64 = true;
};

// One machine-state checkpoint in a play (JSON "snapshots" array entry).
// sha256 = SHA-256 of the machine image (= sha256sum of .bin). PRG snaps are
// the FullSnapshot POD; d64-mode snaps are POD plus the 1541/GCR trailer.
// filename is optional (set when a .bin was written / registered).
struct PlaySnapshot {
	uint64_t cycle = 0;
	uint32_t frame = 0;
	std::string sha256;
	std::string filename;
};

// F9 stamp at a VSYNC (JSON timestamps[] entry).
// Legacy plays may store bare frame numbers; cycle is then 0 (unknown).
struct PlayTimestamp {
	uint32_t frame = 0;
	uint64_t cycle = 0;
};

struct PlayLog {
	int format_version = 1;
	PlaySource source;
	unsigned rand_seed = 42; // Frodo color-RAM LCG; must match for hash replay
	bool no_twin = false;    // JSON "no-twin": true when recorded under --no-twin; absent = original
	RomHashes roms;          // SHA-256 of loaded dumps; recorded, not checked on replay
	uint32_t start_frame = 0;
	uint64_t start_cycle = 0;
	uint32_t end_frame = 0;
	std::vector<PlayTimestamp> timestamps; // F9 stamps
	std::map<uint32_t, InputFrame> events;
	std::map<uint32_t, std::vector<MemoryModification>> mod_events;
	std::vector<PlaySnapshot> snapshots;
};

bool SavePlayLog(const std::string & path, const PlayLog & log, std::string & error);
bool LoadPlayLog(const std::string & path, PlayLog & log, std::string & error);

// Warn when a recording's kind does not match the current system.
// Playback continues.
void WarnPlayUsage(const PlayLog & log, bool system_no_twin);

bool LoadInputEvents(const std::string & path,
                     std::map<uint32_t, InputFrame> & events,
                     std::string & error);

bool PlayEndFrame(const std::string & path, uint32_t & frame_out, std::string & error);

std::vector<std::string> InputFrameToInputs(const InputFrame & in);
bool InputFrameFromInputs(const std::vector<std::string> & inputs, InputFrame & out,
                          std::string & error);

// Upsert a snapshot entry by cycle (sorted by cycle). Empty filename leaves
// any existing filename unchanged when updating sha256/frame only.
void UpsertPlaySnapshot(PlayLog & log, uint64_t cycle, uint32_t frame,
                        const std::string & sha256, const std::string & filename);

// Register a snapshot bin into an on-disk play JSON (read-modify-write).
bool RegisterPlaySnapshot(const std::string & play_path, uint64_t cycle,
                          uint32_t frame, const std::string & snap_rel_path,
                          const std::string & sha256, std::string & error);

const PlaySnapshot * FindPlaySnapshot(const PlayLog & log, uint64_t cycle);

} // namespace revm
