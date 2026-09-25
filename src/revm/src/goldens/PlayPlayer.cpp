// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "goldens/PlayPlayer.hpp"
#include "core/Board.hpp"
#include "util/Hash.hpp"
#define REVM_LOG_MODULE "play"
#include "util/Log.hpp"

#include <algorithm>

namespace revm {

const std::vector<MemoryModification> & PlayPlayer::ModificationsAt(uint32_t frame) const {
	static const std::vector<MemoryModification> none;
	const auto it = log_.mod_events.find(frame);
	return it == log_.mod_events.end() ? none : it->second;
}

bool PlayPlayer::Load(const std::string & play_path, std::string & error) {
	play_path_ = play_path;
	if (!LoadPlayLog(play_path, log_, error)) return false;
	std::sort(log_.snapshots.begin(), log_.snapshots.end(),
	          [](const PlaySnapshot & a, const PlaySnapshot & b) {
		          return a.cycle < b.cycle;
	          });
	input_.LoadEvents(log_.events);
	hashes_checked_.clear();
	next_snapshot_ = 0;
	comparisons_ = 0;
	failures_ = 0;
	return true;
}

bool PlayPlayer::HashCheckDue(uint64_t cycle) {
	if (opts_.ignore_play_hashes) return false;
	while (next_snapshot_ < log_.snapshots.size() &&
	       (log_.snapshots[next_snapshot_].cycle < cycle ||
	        hashes_checked_.count(log_.snapshots[next_snapshot_].cycle))) {
		++next_snapshot_;
	}
	return next_snapshot_ < log_.snapshots.size() &&
	       log_.snapshots[next_snapshot_].cycle == cycle;
}

bool PlayPlayer::OnFrame(uint32_t /*frame*/, uint32_t /*cycle*/, Board & /*main*/) {
	return false;
}

bool PlayPlayer::CheckHashes(Board & board) {
	if (opts_.ignore_play_hashes) return false;
	const uint64_t cycle = board.CycleCounter();
	bool abort = false;
	for (const auto & snap : log_.snapshots) {
		// Exact cycle only — hashing after later cycles would compare wrong state.
		if (snap.cycle != cycle) continue;
		if (hashes_checked_.count(snap.cycle)) continue;
		hashes_checked_.insert(snap.cycle);

		++comparisons_;
		const std::string actual = Sha256MachineState(board);
		if (actual.empty()) {
			REVM_LOG(REVM_ERROR, "FAIL cycle %llu: snapshot hash capture failed",
			               static_cast<unsigned long long>(snap.cycle));
			++failures_;
			abort = true;
			continue;
		}
		if (actual != snap.sha256) {
			REVM_LOG(REVM_ERROR, "FAIL cycle %llu frame %u: snapshot hash mismatch\n"
				"  expected %s\n"
				"  actual   %s",
				static_cast<unsigned long long>(snap.cycle), snap.frame,
				snap.sha256.c_str(), actual.c_str());
			++failures_;
			abort = true;
		} else {
			REVM_LOG(REVM_DEBUG, "OK snapshots[cycle=%llu frame=%u]=%s",
			              static_cast<unsigned long long>(snap.cycle), snap.frame,
			              actual.c_str());
		}
	}
	while (next_snapshot_ < log_.snapshots.size() &&
	       hashes_checked_.count(log_.snapshots[next_snapshot_].cycle)) {
		++next_snapshot_;
	}
	return abort;
}

} // namespace revm
