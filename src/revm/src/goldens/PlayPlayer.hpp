// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "goldens/CompareReport.hpp"
#include "goldens/PlayLog.hpp"
#include "input/GoldenInput.hpp"
#include "input/InputState.hpp"

#include <cstdint>
#include <set>
#include <string>

namespace revm {

class Board;

// --use-play: replay inputs + verify FullSnapshot SHA-256 hashes.
// Main↔Twin screen/SID/kb compares live on CpuMockHost JoinAtPc / CompareNow.
class PlayPlayer {
public:
	bool Load(const std::string & play_path, std::string & error);

	GoldenInput & Input() { return input_; }
	const PlayLog & Log() const { return log_; }
	const std::string & Path() const { return play_path_; }
	const std::vector<MemoryModification> & ModificationsAt(uint32_t frame) const;

	void SetOpts(const PlaybackCompareOpts & opts) { opts_ = opts; }
	void SetCompareOpts(const PlaybackCompareOpts & opts) { opts_ = opts; }
	const PlaybackCompareOpts & CompareOpts() const { return opts_; }

	// Verify hashes whose cycle matches current (once each). board = Main or Twin.
	// Returns true if a failure should abort (unless --ignore-play-hashes).
	bool CheckHashes(Board & board);
	// Fast per-cycle gate: advances past checkpoints preceding `cycle` and says
	// whether CheckHashes has work at this exact cycle.
	bool HashCheckDue(uint64_t cycle);

	// Board VSYNC hook — Twin↔play hashes; Main↔Twin compares are on CpuMockHost.
	bool OnFrame(uint32_t frame, uint32_t cycle, Board & main);

	uint64_t Comparisons() const { return comparisons_; }
	uint64_t Failures() const { return failures_; }
	bool HadFailure() const { return failures_ > 0; }
	bool Ok() const { return failures_ == 0; }

	// After --add-play-snapshot updates on-disk play.json.
	void UpdateSnapshotHash(uint64_t cycle, uint32_t frame, const std::string & hex,
	                        const std::string & filename = {}) {
		UpsertPlaySnapshot(log_, cycle, frame, hex, filename);
		hashes_checked_.erase(cycle); // allow re-check against new expected
		next_snapshot_ = 0;
	}

private:
	std::string play_path_;
	PlayLog log_;
	GoldenInput input_;
	PlaybackCompareOpts opts_{};
	std::set<uint64_t> hashes_checked_;
	size_t next_snapshot_ = 0;
	uint64_t comparisons_ = 0;
	uint64_t failures_ = 0;
};

} // namespace revm
