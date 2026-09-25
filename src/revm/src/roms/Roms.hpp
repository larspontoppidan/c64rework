// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace revm {

constexpr std::size_t kBasicRomSize = 0x2000;
constexpr std::size_t kKernalRomSize = 0x2000;
constexpr std::size_t kCharRomSize = 0x1000;
constexpr std::size_t kDriveRomSize = 0x4000;

inline constexpr const char * kBasicRomGlob = "basic*.bin";
inline constexpr const char * kKernalRomGlob = "kernal*.bin";
inline constexpr const char * kCharRomGlob = "chargen*.bin";
inline constexpr const char * kDriveRomGlob = "dos1541ii*.bin";

struct RomDirFiles {
	std::string basic;
	std::string kernal;
	std::string chars;
	std::string drive; // empty when the optional 1541 image is absent
};

struct RomHashes {
	std::string basic;
	std::string kernal;
	std::string chargen;
	std::string dos1541ii; // empty when the optional 1541 image is absent
};

struct RomImages {
	std::vector<uint8_t> basic;
	std::vector<uint8_t> kernal;
	std::vector<uint8_t> chars;
	std::vector<uint8_t> drive; // empty when not loaded
};

bool ResolveRomDir(const std::string & dir, RomDirFiles & out, std::string & error);
// Resolve, require exact dump sizes, and SHA-256 the files (same hash as snapshots).
bool InspectRomDir(const std::string & dir, RomDirFiles & files, RomHashes & hashes,
                   std::string & error);
bool LoadRomsFromDir(const std::string & dir, RomImages & out, std::string & error);

} // namespace revm
