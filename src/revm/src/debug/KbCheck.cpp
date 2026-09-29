// Created  : 2026-07-23
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "debug/KbCheck.hpp"
#include "debug/KbWatchSample.hpp"

#include "cpumock/LinkedRegistry.hpp"

#include "core/Board.hpp"
#include "twin/TwinBoard.hpp"
#define REVM_LOG_MODULE "kb-check"
#include "util/Event.hpp"
#include "util/Log.hpp"

#include "C64.h"
#include "CPUC64.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace revm {

void KbCheck::Reset() {
	slots_.clear();
	slot_bytes_ = 0;
	tally_ = {};
	have_first_fail_ = false;
	first_fail_frame_ = 0;
	first_fail_cycle_ = 0;
	have_first_byte_ = false;
	first_diff_name_ = "(unnamed)";
	first_diff_addr_ = 0;
	first_diff_main_ = 0;
	first_diff_twin_ = 0;
}

void KbCheck::Configure(const KnowledgeBase & kb, bool include_yes,
                        bool include_verbose) {
	Reset();
	if (!include_yes && !include_verbose) return;

	for (const auto & o : kb.Objects()) {
		if (!KbWatchEnroll(o)) continue;
		if (o.watch == KbWatchLevel::No) continue;
		if (o.watch == KbWatchLevel::Yes && !include_yes) continue;
		if (o.watch == KbWatchLevel::Verbose && !include_verbose) continue;
		Slot s;
		s.obj = &o;
		s.addr = o.addr;
		const uint16_t hi = o.end ? *o.end : o.addr;
		s.len = uint16_t(hi - o.addr + 1);
		if (s.len == 0) s.len = 1;
		slot_bytes_ += s.len;
		slots_.push_back(s);
	}
}

std::string KbCheck::FormatBytes(const uint8_t * p, size_t n) {
	constexpr size_t kMaxShow = 16;
	std::string out;
	out.reserve(n * 3);
	const size_t show = n > kMaxShow ? kMaxShow : n;
	char buf[8];
	for (size_t i = 0; i < show; ++i) {
		std::snprintf(buf, sizeof(buf), "%s%02X", i ? " " : "", p[i]);
		out += buf;
	}
	if (n > kMaxShow) out += " ...";
	return out;
}

bool KbCheck::OnFrame(Board & main, TwinBoard & twin, uint32_t frame,
                      uint32_t cycle, bool log_mismatches,
                      const cpumock::LinkedRegistry * linked) {
	if (slots_.empty()) return true;
	C64 * c64 = main.Machine();
	C64 * twin_c64 = twin.board().Machine();
	if (!c64 || !c64->RAM || !twin_c64 || !twin_c64->RAM) return true;

	bool ok = true;
	std::vector<uint8_t> main_bytes;
	std::vector<uint8_t> twin_bytes;
	// Diverging-byte records for the event stream: first 32 per compare,
	// then one overflow marker.
	constexpr size_t kEventFailCap = 32;
	size_t event_shown = 0;
	bool event_overflow = false;
	for (const auto & s : slots_) {
		main_bytes.resize(s.len);
		twin_bytes.resize(s.len);
		const KbObject dummy{};
		const KbObject & obj = s.obj ? *s.obj : dummy;
		for (uint16_t i = 0; i < s.len; ++i) {
			const uint16_t a = uint16_t(s.addr + i);
			if (!linked || !linked->Read(a, main_bytes[i]))
				main_bytes[i] = KbWatchSample(c64, obj, a);
			twin_bytes[i] = KbWatchSample(twin_c64, obj, a);
		}
		if (std::memcmp(main_bytes.data(), twin_bytes.data(), s.len) == 0)
			continue;

		ok = false;
		for (uint16_t i = 0; i < s.len && event_overflow == false; ++i) {
			if (main_bytes[i] == twin_bytes[i]) continue;
			if (event_shown < kEventFailCap) {
				REVM_EVENT("compare_fail", "channel", "kb", "addr",
				           revm::event::Hex{uint32_t(s.addr + i)}, "main",
				           main_bytes[i], "twin", twin_bytes[i]);
				++event_shown;
			} else {
				event_overflow = true;
				REVM_EVENT("compare_fail", "channel", "kb", "overflow", 1);
			}
		}
		if (!have_first_fail_) {
			have_first_fail_ = true;
			first_fail_frame_ = frame;
			first_fail_cycle_ = cycle;
		}
		if (!have_first_byte_) {
			for (uint16_t i = 0; i < s.len; ++i) {
				if (main_bytes[i] == twin_bytes[i])
					continue;
				have_first_byte_ = true;
				first_diff_name_ =
					(s.obj && !s.obj->name.empty())
						? s.obj->name.c_str()
						: "(unnamed)";
				first_diff_addr_ = uint16_t(s.addr + i);
				first_diff_main_ = main_bytes[i];
				first_diff_twin_ = twin_bytes[i];
				break;
			}
		}
		if (!log_mismatches) continue;
		const char * label =
		    (s.obj && !s.obj->name.empty()) ? s.obj->name.c_str() : "(unnamed)";
		if (s.len == 1) {
			REVM_LOG_TIMED(REVM_ERROR,
			               "FAIL frame %u cycle %u: %s $%04X: main=$%02X twin=$%02X",
			               frame, cycle, label, s.addr, main_bytes[0], twin_bytes[0]);
		} else {
			// Per-byte diffs for ranges so large tables stay readable.
			for (uint16_t i = 0; i < s.len; ++i) {
				if (main_bytes[i] == twin_bytes[i]) continue;
				REVM_LOG_TIMED(
					REVM_ERROR,
					"FAIL frame %u cycle %u: %s $%04X: main=$%02X twin=$%02X",
					frame, cycle, label, uint16_t(s.addr + i), main_bytes[i],
					twin_bytes[i]);
			}
		}
	}
	return ok;
}

void KbCheck::PrintTally() const {
	if (!Active() && tally_.compared == 0 && tally_.ignored == 0)
		return;
	if (have_first_fail_) {
		REVM_LOG(REVM_DEBUG,
		         "kb: compared=%llu failed=%llu first_fail_frame=%u cycle=%u "
		         "(slots=%zu bytes=%zu)",
		         static_cast<unsigned long long>(tally_.compared),
		         static_cast<unsigned long long>(tally_.failed), first_fail_frame_,
		         first_fail_cycle_, SlotCount(), ByteCount());
		return;
	}
	REVM_LOG(REVM_DEBUG, "kb: compared=%llu failed=%llu (slots=%zu bytes=%zu)",
	         static_cast<unsigned long long>(tally_.compared),
	         static_cast<unsigned long long>(tally_.failed), SlotCount(),
	         ByteCount());
}

} // namespace revm
