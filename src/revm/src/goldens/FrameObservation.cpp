// Created  : 2026-09-05
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "goldens/FrameObservation.hpp"

#include "SID.h"
#include "core/Board.hpp"
#include "snapshot/Snapshot.hpp"

#include <cstring>

namespace revm {

namespace {

constexpr char kMagic[16] = {
	'R', 'E', 'V', 'M', 'F', 'r', 'a', 'm', 'e', 'O', 'b', 's', '1', '\0', '\0', '\0'
};

} // namespace

void FillSidConfigBytes(const MOS6581State & sid,
                        uint8_t out[FrameSnapshot::kSidBytes]) {
	const uint8_t values[FrameSnapshot::kSidBytes] = {
		sid.freq_lo_1, sid.freq_hi_1, sid.pw_lo_1, sid.pw_hi_1,
		sid.ctrl_1, sid.AD_1, sid.SR_1,
		sid.freq_lo_2, sid.freq_hi_2, sid.pw_lo_2, sid.pw_hi_2,
		sid.ctrl_2, sid.AD_2, sid.SR_2,
		sid.freq_lo_3, sid.freq_hi_3, sid.pw_lo_3, sid.pw_hi_3,
		sid.ctrl_3, sid.AD_3, sid.SR_3,
		sid.fc_lo, sid.fc_hi, sid.res_filt, sid.mode_vol,
		sid.pot_x, sid.pot_y,
	};
	std::memcpy(out, values, FrameSnapshot::kSidBytes);
}

bool CaptureFrameSnapshot(const Board & board, FrameSnapshot & out) {
	ScreenSnapshot screen{};
	ChipSnapshot chips{};
	if (!CaptureScreenSnapshot(board, screen) || !CaptureChipSnapshot(board, chips))
		return false;
	out.native_frame = screen.frame;
	out.native_cycle = screen.cycle;
	std::memcpy(out.pixels.data(), screen.pixels, FrameSnapshot::kPixels);
	FillSidConfigBytes(chips.sid, out.sid.data());
	return true;
}

bool FrameObservationWriter::Open(const std::string & path, std::string & error) {
	path_ = path;
	error_.clear();
	file_.open(path, std::ios::binary | std::ios::trunc);
	if (!file_) {
		error = "cannot open frame observation file: " + path;
		return false;
	}
	file_.write(kMagic, sizeof(kMagic));
	if (!write_u32(kVersion) || !write_u32(FrameSnapshot::kWidth) ||
	    !write_u32(FrameSnapshot::kHeight) || !write_u32(kSidBytes)) {
		error = "cannot write frame observation header: " + path;
		file_.close();
		return false;
	}
	return true;
}

bool FrameObservationWriter::write_u32(uint32_t value) {
	const char bytes[4] = {
		static_cast<char>(value & 0xffu),
		static_cast<char>((value >> 8) & 0xffu),
		static_cast<char>((value >> 16) & 0xffu),
		static_cast<char>((value >> 24) & 0xffu),
	};
	file_.write(bytes, sizeof(bytes));
	return bool(file_);
}

bool FrameObservationWriter::write_payload(const FrameSnapshot & snap,
                                           std::string & error) {
	if (!write_u32(snap.ordinal) || !write_u32(snap.native_frame) ||
	    !write_u32(snap.native_cycle)) {
		error = "failed to write frame observation metadata";
		return false;
	}
	file_.write(reinterpret_cast<const char *>(snap.pixels.data()),
	            FrameSnapshot::kPixels);
	file_.write(reinterpret_cast<const char *>(snap.sid.data()),
	            FrameSnapshot::kSidBytes);
	if (!file_) {
		error = "failed to write frame observation payload";
		return false;
	}
	return true;
}

bool FrameObservationWriter::WriteFrame(const FrameSnapshot & snap,
                                        std::string & error) {
	if (!error_.empty()) {
		error = error_;
		return false;
	}
	if (!file_) {
		error_ = "frame observation file is not open";
		error = error_;
		return false;
	}
	if (!write_payload(snap, error)) {
		error_ = error;
		return false;
	}
	return true;
}

void FrameObservationWriter::onFrame(const FrameSnapshot & snap) {
	// Keep the FrameObserver callback usable by callers that only have the
	// interface.  The typed Board path calls WriteFrame and propagates error.
	std::string error;
	(void)WriteFrame(snap, error);
}

bool FrameObservationWriter::Write(const Board & board, std::string & error) {
	if (!error_.empty()) {
		error = error_;
		return false;
	}
	if (!file_) {
		error = "frame observation file is not open";
		return false;
	}
	FrameSnapshot snap;
	if (!CaptureFrameSnapshot(board, snap)) {
		error = "failed to capture frame observation";
		return false;
	}
	// Legacy helper: caller has not assigned a comparison ordinal.
	return WriteFrame(snap, error);
}

} // namespace revm
