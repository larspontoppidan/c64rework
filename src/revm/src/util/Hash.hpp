// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace revm {

class Board;
struct FullSnapshot;

// Hex-encoded SHA-256 (lowercase). Empty string on failure.
std::string Sha256File(const std::string & path);
std::string Sha256Bytes(const void * data, size_t len);

// SHA-256 of an in-memory FullSnapshot POD. PRG .bin files without a drive
// trailer match sha256sum of this blob. D64-mode snaps hash POD + trailer via
// Sha256MachineSnapshot / Sha256MachineState.
std::string Sha256FullSnapshot(const FullSnapshot & snap);

// Capture the live machine (C64 POD plus 1541/GCR when the drive is on) and
// hash the same bytes SaveMachineSnapshotFile would write.
std::string Sha256MachineState(const Board & board);

} // namespace revm
