// Created  : 2026-07-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "goldens/RamCompareMask.hpp"
#include "snapshot/Snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace revm {

// Golden / Stage-3 compare options. Stage 3 Main↔Twin screen/SID/kb run at
// JoinAtPc / CompareNow (not VBLANK). Twin↔play hashes stay on Board VSYNC.
struct PlaybackCompareOpts {
	// --no-twin (Stage 3): skip TwinBoard; implies the three ignore flags below.
	bool no_twin = false;
	// --ignore-checks: still run Main↔Twin / kb-check / golden compares and
	// tally; log failures but do not abort.
	bool ignore_checks = false;
	// --ignore-asserts: log Assert*/QuitOnAssert failures but do not hard Quit.
	bool ignore_asserts = false;
	// --ignore-play-hashes: skip Twin ↔ play.json snapshot SHA checks.
	bool ignore_play_hashes = false;
	// If non-empty, also used as the directory for screen PPM dumps.
	std::string dump_fail_dir;
	// Cap how many screen image sets to write (0 = unlimited).
	unsigned dump_fail_limit = 1;
	// On screen mismatch: write expected/actual/diff PPM images (Pepto palette).
	// Uses dump_fail_dir, or "revm-fail" when that is empty.
	bool dump_screen_images = true;

};

// Counters for the rolling once-per-second Info tally (reset every window).
struct TallyWindow {
	uint64_t compared = 0; // CompareNow calls that ran this channel
	uint64_t ignored = 0;  // unused (join mask omits a channel instead)
	uint64_t failed = 0;

	uint64_t passed() const { return compared > failed ? compared - failed : 0; }
};

// Per-channel Main↔Twin compare accounting (screen / SID / kb-check).
struct IgnoreTally {
	uint64_t compared = 0;
	uint64_t ignored = 0;
	uint64_t stretch = 0;
	uint64_t longest_ignore_stretch = 0;
	uint64_t failed = 0;
	TallyWindow window{};

	// One compare that ran this channel. `was_ignored` is unused (always false);
	// `ok` is the actual compare result.
	void Note(bool was_ignored, bool ok) {
		++compared;
		++window.compared;
		if (was_ignored) {
			++ignored;
			++stretch;
			++window.ignored;
			if (stretch > longest_ignore_stretch)
				longest_ignore_stretch = stretch;
		} else {
			stretch = 0;
		}
		if (!ok) {
			++failed;
			++window.failed;
		}
	}

	void ResetWindow() { window = {}; }

	// Indented Info line for end-of-run / summary dumps.
	void LogLine(const char * module, const char * label) const;
};

// Per-channel Stage-3 join-compare outcome (for the collapsed cpumock summary).
struct RitualCheck {
	bool ran = false;
	bool ignored = false;
	bool ok = true;

	// "off" | "ok" | "fail"
	const char * Format() const {
		if (!ran) return "off";
		return ok ? "ok" : "fail";
	}
};

struct MemDiffHit {
	uint16_t addr = 0;
	uint8_t expected = 0;
	uint8_t actual = 0;
};

// Collect up to `max_hits` differing bytes, honoring exclude ranges. Returns total mismatches.
size_t CollectMemDiffs(const uint8_t * expected, const uint8_t * actual, size_t size,
                       const RamCompareMask & mask, size_t max_hits,
                       std::vector<MemDiffHit> & out);

void PrintMemDiffs(FILE * out, const char * label, const std::vector<MemDiffHit> & hits,
                   size_t total);

void PrintCpuDiff(FILE * out, const MOS6510State & expected, const MOS6510State & actual);

void PrintChipConfigDiff(FILE * out,
                         const MOS6569State & vic_e, const MOS6569State & vic_a,
                         const MOS6581State & sid_e, const MOS6581State & sid_a,
                         const MOS6526State & cia1_e, const MOS6526State & cia1_a,
                         const MOS6526State & cia2_e, const MOS6526State & cia2_a,
                         bool compare_sid = true);

void PrintVicConfigDiff(FILE * out, const MOS6569State & expected,
                        const MOS6569State & actual);

void PrintSidConfigDiff(FILE * out, const MOS6581State & expected,
                        const MOS6581State & actual);

void PrintCiaConfigDiff(FILE * out, const char * label, const MOS6526State & expected,
                        const MOS6526State & actual, uint8_t pra_mask = 0);

void PrintScreenDiff(FILE * out, const ScreenSnapshot & expected,
                     const ScreenSnapshot & actual, size_t max_hits = 16);

// Human-readable multi-section report (debug / Assert helpers).
void PrintFullCompareReport(FILE * out, uint32_t frame, uint32_t cycle, size_t snap_index,
                            const FullSnapshot & expected, const FullSnapshot & actual,
                            const RamCompareMask & ram_mask, bool compare_cpu,
                            bool chip_stream_mismatch, bool compare_sid = true);

bool WriteFailDumpPair(const std::string & dir, uint32_t frame, size_t snap_index,
                       const FullSnapshot & expected, const FullSnapshot & actual,
                       const std::string & report_text, std::string & error);

// Write Pepto-palette PPM images for a screen mismatch:
//   <dir>/fail_fNNNNNN_expected.ppm
//   <dir>/fail_fNNNNNN_actual.ppm
//   <dir>/fail_fNNNNNN_diff.ppm   (red where pixels differ)
bool WriteScreenFailImages(const std::string & dir, uint32_t frame,
                           const ScreenSnapshot & expected,
                           const ScreenSnapshot & actual, std::string & error);

// Write a Pepto-palette binary P6 PPM of `screen` to `path`.
bool WriteScreenPpm(const std::string & path, const ScreenSnapshot & screen,
                    std::string & error);

} // namespace revm
