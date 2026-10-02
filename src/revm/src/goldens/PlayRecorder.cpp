// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "goldens/PlayRecorder.hpp"
#include "core/Board.hpp"
#include "snapshot/Snapshot.hpp"
#include "util/Hash.hpp"

#include <cstdio>
#define REVM_LOG_MODULE "play-rec"
#include "util/Log.hpp"

namespace revm {
namespace {

bool append_board_hash(PlayLog & log, Board & board, const char * label,
                       std::string & error) {
	const std::string hex = Sha256MachineState(board);
	if (hex.empty()) {
		error = std::string("Failed to hash machine state at ") + label;
		return false;
	}
	UpsertPlaySnapshot(log, board.CycleCounter(), board.FrameCounter(), hex, /*filename=*/{});
	REVM_LOG(REVM_DEBUG, "snapshots[+%s] cycle=%u frame=%u %s",
	             label, board.CycleCounter(), board.FrameCounter(), hex.c_str());
	return true;
}

} // namespace

void PlayRecorder::Configure(const std::string & out_path, PlaySource source) {
	out_path_ = out_path;
	log_ = PlayLog{};
	log_.format_version = 1;
	log_.source = std::move(source);
	last_input_ = InputFrame{};
	have_end_ = false;
	start_hashed_ = false;
	last_frame_ = 0;
	board_ = nullptr;
}

void PlayRecorder::SetNoTwin(bool no_twin) {
	log_.no_twin = no_twin;
}

void PlayRecorder::Start(Board & board) {
	board_ = &board;
	log_.rand_seed = board.GetConfig().rand_seed;
	log_.roms = board.LoadedRoms();
	log_.start_frame = board.FrameCounter();
	log_.start_cycle = board.CycleCounter();
	log_.events.clear();
	log_.mod_events.clear();
	log_.timestamps.clear();
	log_.snapshots.clear();
	last_input_ = InputFrame{};
	have_end_ = false;
	start_hashed_ = false;
	REVM_LOG(REVM_DEBUG, "start_frame=%u start_cycle=%llu rand_seed=%u roms basic=%s → %s",
		log_.start_frame,
		static_cast<unsigned long long>(log_.start_cycle),
		log_.rand_seed,
		log_.roms.basic.c_str(),
		out_path_.c_str());
}

void PlayRecorder::CaptureStartHash() {
	if (!board_ || start_hashed_) return;
	log_.start_frame = board_->FrameCounter();
	log_.start_cycle = board_->CycleCounter();
	std::string err;
	if (!append_board_hash(log_, *board_, "start", err)) {
		REVM_LOG(REVM_ERROR, "%s", err.c_str());
	}
	start_hashed_ = true;
}

void PlayRecorder::OnFrame(uint32_t frame, const InputFrame & input,
                           const std::vector<MemoryModification> & modifications) {
	if (!(input == last_input_)) {
		log_.events[frame] = input;
		last_input_ = input;
	}
	if (!modifications.empty()) {
		log_.mod_events[frame] = modifications;
	}
	last_frame_ = frame;
	have_end_ = true;
}

void PlayRecorder::AddTimestamp(uint32_t frame, uint64_t cycle) {
	log_.timestamps.push_back(PlayTimestamp{frame, cycle});
	REVM_LOG(REVM_DEBUG, "timestamp at frame %u cycle %llu",
		frame, static_cast<unsigned long long>(cycle));
}

bool PlayRecorder::Finish(std::string & error) {
	if (have_end_) {
		log_.end_frame = last_frame_;
	} else {
		log_.end_frame = log_.start_frame;
	}
	if (board_ && have_end_) {
		if (!append_board_hash(log_, *board_, "end", error)) return false;
	}
	if (!SavePlayLog(out_path_, log_, error)) return false;
	REVM_LOG(REVM_DEBUG,
	         "Play recorded to %s  (%zu input events, %zu mod frames, %zu timestamps, %zu snapshots, end_frame=%u)",
	         out_path_.c_str(), log_.events.size(), log_.mod_events.size(),
	         log_.timestamps.size(), log_.snapshots.size(), log_.end_frame);
	return true;
}

} // namespace revm
