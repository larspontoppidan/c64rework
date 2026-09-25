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

// SHA-256 of an in-memory FullSnapshot POD — identical to `sha256sum` of a
// file written by SaveFullSnapshotFile (entire struct, incl. header + v2 trailer).
std::string Sha256FullSnapshot(const FullSnapshot & snap);

// CaptureFullSnapshot on board, then Sha256FullSnapshot.
std::string Sha256MachineState(const Board & board);

} // namespace revm
