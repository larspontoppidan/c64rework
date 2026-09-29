// Created  : 2026-07-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "debug/KnowledgeBase.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace revm {

class Board;

// Snapshot of one watch slot for UI / log.
struct KbWatchSlotView {
	const char * name = "(unnamed)";
	uint16_t addr = 0;
	uint16_t len = 1;
	const uint8_t * bytes = nullptr; // points into slot storage; valid until next OnFrame
	uint32_t changed_at_frame = 0;   // 0 = never changed after prime
	bool modifiable = false;         // KB kind=variable
};

// Polls KB objects with watch=yes/verbose at each VSYNC.
// Samples DRAM or bank:"color" nybble file — never the CPU I/O map.
class KbWatch {
public:
	void Reset();
	// include_yes / include_verbose select slots; print_stderr enables change log.
	void Configure(const KnowledgeBase & kb, bool include_yes, bool include_verbose,
	               bool print_stderr);

	bool Active() const { return !slots_.empty(); }
	size_t SlotCount() const { return slots_.size(); }

	void OnFrame(Board & board, uint32_t frame, uint32_t cycle);

	// Optional JSONL sink (one object per change after prime).
	void SetTimelineFile(FILE * f) { timeline_ = f; }

	bool Dirty() const { return dirty_; }
	void ClearDirty() { dirty_ = false; }
	// Force a UI refresh (e.g. after resize).
	void MarkDirty() { dirty_ = true; }

	// Views are rebuilt each OnFrame; pointers valid until the next OnFrame.
	const std::vector<KbWatchSlotView> & Views() const { return views_; }

	static std::string FormatBytes(const uint8_t * p, size_t n);

private:
	struct Slot {
		const KbObject * obj = nullptr;
		uint16_t addr = 0;
		uint16_t len = 1;
		std::vector<uint8_t> last;
		bool primed = false;
		uint32_t changed_at_frame = 0;
	};

	std::vector<Slot> slots_;
	std::vector<KbWatchSlotView> views_;
	bool print_stderr_ = false;
	bool dirty_ = false;
	bool need_prime_paint_ = false;
	FILE * timeline_ = nullptr;
};

} // namespace revm
