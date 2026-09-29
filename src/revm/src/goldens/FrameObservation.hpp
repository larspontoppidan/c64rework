// Created  : 2026-09-05
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

struct MOS6581State;

namespace revm {

class Board;

struct FrameSnapshot {
	static constexpr unsigned kWidth = 0x180;
	static constexpr unsigned kHeight = 0x110;
	static constexpr size_t kPixels = size_t(kWidth) * kHeight;
	static constexpr uint32_t kSidBytes = 27;

	uint32_t ordinal = 0;
	uint32_t native_frame = 0;
	uint32_t native_cycle = 0;
	std::array<uint8_t, kPixels> pixels{};
	std::array<uint8_t, kSidBytes> sid{};
};

class FrameObserver {
public:
	virtual ~FrameObserver() = default;
	virtual void onFrame(const FrameSnapshot & snap) = 0;
};

bool CaptureFrameSnapshot(const Board & board, FrameSnapshot & out);
void FillSidConfigBytes(const MOS6581State & sid, uint8_t out[FrameSnapshot::kSidBytes]);

// Optional file adapter for per-frame observation output.
class FrameObservationWriter : public FrameObserver {
public:
	static constexpr uint32_t kVersion = 1;
	static constexpr uint32_t kSidBytes = FrameSnapshot::kSidBytes;

	FrameObservationWriter() = default;
	FrameObservationWriter(const FrameObservationWriter &) = delete;
	FrameObservationWriter & operator=(const FrameObservationWriter &) = delete;

	bool Open(const std::string & path, std::string & error);
	// FrameObserver is intentionally a void callback so direct comparison
	// observers do not need file-I/O error handling.  Board uses this typed
	// adapter entry point when a file-backed observation is enabled.
	bool WriteFrame(const FrameSnapshot & snap, std::string & error);
	void onFrame(const FrameSnapshot & snap) override;
	bool Write(const Board & board, std::string & error);

private:
	bool write_u32(uint32_t value);
	bool write_payload(const FrameSnapshot & snap, std::string & error);

	std::ofstream file_;
	std::string path_;
	std::string error_;
};

} // namespace revm
