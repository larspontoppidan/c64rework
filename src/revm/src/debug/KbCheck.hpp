// Created  : 2026-07-23
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "debug/KnowledgeBase.hpp"
#include "goldens/CompareReport.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace revm {

class Board;
class TwinBoard;
namespace cpumock { class LinkedRegistry; }

// Join compare: KB watch:"yes" slots Main vs Twin at CompareNow / JoinAtPc
// (not VBLANK). A LinkedRegistry substitutes detached local bytes by address.
// + watch:"verbose" with --kb-check-verbose.
// (Debugger --watch / PC watches are separate — this is a check, not a probe.)
class KbCheck {
public:
	void Reset();
	void Configure(const KnowledgeBase & kb, bool include_yes, bool include_verbose);

	bool Active() const { return !slots_.empty(); }
	size_t SlotCount() const { return slots_.size(); }
	size_t ByteCount() const { return slot_bytes_; }

	IgnoreTally & Tally() { return tally_; }
	const IgnoreTally & Tally() const { return tally_; }
	const TallyWindow & Window() const { return tally_.window; }
	void ResetWindow() { tally_.ResetWindow(); }

	// Call when Main and Twin are same-frame (JoinAtPc / CompareNow).
	// Returns false if any difference was found. log_mismatches=false still
	// compares but suppresses per-slot ERROR rows.
	bool OnFrame(Board & main, TwinBoard & twin, uint32_t frame, uint32_t cycle,
	             bool log_mismatches = true,
	             const cpumock::LinkedRegistry * linked = nullptr);

	// End-of-run summary (no-op if never configured and never compared).
	// On any mismatch, includes first_fail_frame / cycle (join fence).
	void PrintTally() const;

	// First diverging watch byte of the run (for the QuitOnCheck one-liner).
	bool HasFirstDiff() const { return have_first_byte_; }
	const char * FirstDiffName() const { return first_diff_name_; }
	uint16_t FirstDiffAddr() const { return first_diff_addr_; }
	uint8_t FirstDiffMain() const { return first_diff_main_; }
	uint8_t FirstDiffTwin() const { return first_diff_twin_; }

	static std::string FormatBytes(const uint8_t * p, size_t n);

private:
	struct Slot {
		const KbObject * obj = nullptr;
		uint16_t addr = 0;
		uint16_t len = 1;
	};

	std::vector<Slot> slots_;
	size_t slot_bytes_ = 0;
	IgnoreTally tally_{};
	bool have_first_fail_ = false;
	uint32_t first_fail_frame_ = 0;
	uint32_t first_fail_cycle_ = 0;
	bool have_first_byte_ = false;
	const char * first_diff_name_ = "(unnamed)";
	uint16_t first_diff_addr_ = 0;
	uint8_t first_diff_main_ = 0;
	uint8_t first_diff_twin_ = 0;
};

} // namespace revm
