// Created  : 2026-07-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "debug/KbWatch.hpp"
#include "debug/KbWatchSample.hpp"
#include "core/Board.hpp"

#include "C64.h"

#define REVM_LOG_MODULE "kb-watch"
#include "util/Log.hpp"

#include <cstdio>
#include <cstring>

namespace revm {

void KbWatch::Reset() {
	slots_.clear();
	views_.clear();
	print_stderr_ = false;
	dirty_ = false;
	need_prime_paint_ = false;
	timeline_ = nullptr;
}

void KbWatch::Configure(const KnowledgeBase & kb, bool include_yes, bool include_verbose,
                        bool print_stderr) {
	Reset();
	print_stderr_ = print_stderr;
	if (!include_yes && !include_verbose) return;

	for (const auto & o : kb.Objects()) {
		if (!KbWatchEnroll(o)) continue; // DRAM or color plane; never I/O map
		if (o.watch == KbWatchLevel::No) continue;
		if (o.watch == KbWatchLevel::Yes && !include_yes) continue;
		if (o.watch == KbWatchLevel::Verbose && !include_verbose) continue;
		Slot s;
		s.obj = &o;
		s.addr = o.addr;
		const uint16_t hi = o.end ? *o.end : o.addr;
		s.len = uint16_t(hi - o.addr + 1);
		if (s.len == 0) s.len = 1;
		s.last.assign(s.len, 0);
		slots_.push_back(std::move(s));
	}
	need_prime_paint_ = !slots_.empty();
	dirty_ = need_prime_paint_;
}

std::string KbWatch::FormatBytes(const uint8_t * p, size_t n) {
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

void KbWatch::OnFrame(Board & board, uint32_t frame, uint32_t cycle) {
	if (slots_.empty()) return;
	C64 * c64 = board.Machine();
	if (!c64 || !c64->RAM) return;

	bool any_change = false;
	std::vector<uint8_t> cur;
	for (auto & s : slots_) {
		cur.resize(s.len);
		const KbObject dummy{};
		const KbObject & obj = s.obj ? *s.obj : dummy;
		for (uint16_t i = 0; i < s.len; ++i) {
			cur[i] = KbWatchSample(c64, obj, uint16_t(s.addr + i));
		}
		if (!s.primed) {
			s.last = cur;
			s.primed = true;
			any_change = true;
			continue;
		}
		if (std::memcmp(s.last.data(), cur.data(), s.len) == 0) continue;

		any_change = true;
		const char * label =
		    (s.obj && !s.obj->name.empty()) ? s.obj->name.c_str() : "(unnamed)";
		const Board::LastTrace & tr = board.GetLastTrace();
		if (print_stderr_) {
			const char * tr_name = tr.valid ? tr.name.c_str() : "";
			const uint16_t tr_addr = tr.valid ? tr.addr : 0;
			if (s.len == 1) {
				if (tr.valid) {
					REVM_LOG_TIMED(REVM_VERBOSE,
					               "watch %s $%04X: $%02X -> $%02X  [trace %s $%04X]",
					               label, s.addr, s.last[0], cur[0], tr_name,
					               tr_addr);
				} else {
					REVM_LOG_TIMED(REVM_VERBOSE, "watch %s $%04X: $%02X -> $%02X",
					               label, s.addr, s.last[0], cur[0]);
				}
			} else if (tr.valid) {
				REVM_LOG_TIMED(REVM_VERBOSE,
				               "watch %s $%04X-$%04X: [%s] -> [%s]  [trace %s $%04X]",
				               label, s.addr, uint16_t(s.addr + s.len - 1),
				               FormatBytes(s.last.data(), s.len).c_str(),
				               FormatBytes(cur.data(), s.len).c_str(), tr_name,
				               tr_addr);
			} else {
				REVM_LOG_TIMED(REVM_VERBOSE, "watch %s $%04X-$%04X: [%s] -> [%s]",
				               label, s.addr, uint16_t(s.addr + s.len - 1),
				               FormatBytes(s.last.data(), s.len).c_str(),
				               FormatBytes(cur.data(), s.len).c_str());
			}
		}
		if (timeline_) {
			const std::string old_s = FormatBytes(s.last.data(), s.len);
			const std::string new_s = FormatBytes(cur.data(), s.len);
			std::fprintf(timeline_,
			             "{\"frame\":%u,\"cycle\":%u,\"name\":\"%s\",\"addr\":\"%04X\","
			             "\"old\":\"%s\",\"new\":\"%s\"",
			             frame, cycle, label, s.addr, old_s.c_str(), new_s.c_str());
			if (tr.valid) {
				std::fprintf(timeline_,
				             ",\"trace\":\"%s\",\"trace_addr\":\"%04X\"",
				             tr.name.c_str(), tr.addr);
			} else {
				std::fprintf(timeline_, ",\"trace\":null,\"trace_addr\":null");
			}
			std::fprintf(timeline_, "}\n");
			std::fflush(timeline_);
		}
		s.last = cur;
		s.changed_at_frame = frame;
	}

	views_.clear();
	views_.reserve(slots_.size());
	for (const auto & s : slots_) {
		KbWatchSlotView v;
		v.name = (s.obj && !s.obj->name.empty()) ? s.obj->name.c_str() : "(unnamed)";
		v.addr = s.addr;
		v.len = s.len;
		v.bytes = s.last.data();
		v.changed_at_frame = s.changed_at_frame;
		v.modifiable = s.obj && s.obj->kind == KbKind::Variable;
		views_.push_back(v);
	}

	if (any_change || need_prime_paint_) {
		dirty_ = true;
		need_prime_paint_ = false;
	}
}

} // namespace revm
