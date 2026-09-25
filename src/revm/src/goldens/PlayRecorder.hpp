// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "goldens/PlayLog.hpp"
#include "input/InputState.hpp"

#include <string>

namespace revm {

class Board;

// Records input-only play logs (no snaps / WAV).
class PlayRecorder {
public:
	void Configure(const std::string & out_path, PlaySource source);
	void Start(Board & board);
	void SetNoTwin(bool no_twin);

	// Call once the run loop has the real starting machine state
	// (after PrepareRun / begin-snap restore). Writes snapshots[0] (start).
	void CaptureStartHash();

	void OnFrame(uint32_t frame, const InputFrame & input,
	             const std::vector<MemoryModification> & modifications);
	void AddTimestamp(uint32_t frame, uint64_t cycle);

	bool Finish(std::string & error);
	bool Started() const { return board_ != nullptr; }

	const PlayLog & Log() const { return log_; }

private:
	std::string out_path_;
	PlayLog log_;
	InputFrame last_input_;
	bool have_end_ = false;
	bool start_hashed_ = false;
	uint32_t last_frame_ = 0;
	Board * board_ = nullptr;
};

} // namespace revm
