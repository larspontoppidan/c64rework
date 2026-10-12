// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "goldens/PlayLog.hpp"
#include "input/InputState.hpp"

#include <mutex>
#include <string>

namespace revm {

class Board;

// Records input-only play logs (no snaps / WAV).
class PlayRecorder {
public:
	void Configure(const std::string & out_path, PlaySource source);
	void Start(Board & board);
	// How this play is being made (JSON "recorded_on"); call after Configure.
	void SetRecordedOn(PlayRecordedOn recorded_on);
	// Game repository revision (--record-git); kept in recorded_on.git only
	// for no-twin plays.
	void SetRecordGit(std::string git) { record_git_ = std::move(git); }

	// Call once the run loop has the real starting machine state
	// (after PrepareRun / begin-snap restore). Writes snapshots[0] (start).
	void CaptureStartHash();

	void OnFrame(uint32_t frame, const InputFrame & input,
	             const std::vector<MemoryModification> & modifications);
	void AddTimestamp(uint32_t frame, uint64_t cycle);

	bool Finish(std::string & error);

	// Supervisor-thread salvage before a watchdog hard abort: writes the
	// inputs recorded so far to the output path, with end_frame = the last
	// recorded VSYNC and NO end snapshot hash (the hung emulation thread's
	// machine state cannot be hashed). Gives up if the recorder lock is not
	// free within a short grace. Returns true when the file was written.
	bool SaveTruncated(std::string & error, uint32_t * end_frame = nullptr);
	bool Started() const { return board_ != nullptr; }

	const PlayLog & Log() const { return log_; }

private:
	std::string out_path_;
	std::string record_git_;
	PlayLog log_;
	InputFrame last_input_;
	bool have_end_ = false;
	bool start_hashed_ = false;
	uint32_t last_frame_ = 0;
	Board * board_ = nullptr;
	bool finished_ = false;
	// Guards log_/last_frame_/have_end_ between the emulation thread and
	// SaveTruncated() on the supervisor thread.
	std::timed_mutex mutex_;
};

} // namespace revm
