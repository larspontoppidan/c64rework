// Created  : 2026-08-08
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "util/Log.hpp"

#include <cstdio>
#include <cstring>

namespace revm::log {

namespace {

struct State {
	std::optional<uint32_t> verbose_from_cycle;
	std::optional<uint32_t> debug_from_cycle;
	ClockSource clock;
	bool in_clock = false;
};

State & state() {
	static State s;
	return s;
}

Clocks sample_clocks() {
	State & s = state();
	if (!s.clock || s.in_clock) return {};
	s.in_clock = true;
	Clocks c = s.clock();
	s.in_clock = false;
	return c;
}

bool from_cycle(std::optional<uint32_t> thresh, const Clocks & c) {
	return thresh.has_value() && c.main.cycle >= *thresh;
}

// Verbose implies Debug from the same cycle.
bool debug_active(const Clocks & c) {
	const State & s = state();
	return from_cycle(s.debug_from_cycle, c) ||
	       from_cycle(s.verbose_from_cycle, c);
}

bool verbose_active(const Clocks & c) {
	return from_cycle(state().verbose_from_cycle, c);
}

int frame_pct(const BoardClock & b) {
	if (b.frame_start == 0 || b.frame_end <= b.frame_start ||
	    b.cycle < b.frame_start)
		return -1;
	const uint64_t span = b.frame_end - b.frame_start;
	return int((uint64_t(b.cycle - b.frame_start) * 100u) / span);
}

void append_timing(char * out, size_t cap, const Clocks & c) {
	const int main_pct = frame_pct(c.main);
	int n = 0;
	if (main_pct >= 0) {
		n = std::snprintf(out, cap, "[main=%u:%u %d%%", c.main.frame,
		                  c.main.cycle, main_pct);
	} else {
		n = std::snprintf(out, cap, "[main=%u:%u", c.main.frame, c.main.cycle);
	}
	if (n < 0 || size_t(n) >= cap) {
		out[cap - 1] = '\0';
		return;
	}
	size_t used = size_t(n);
	if (c.twin_valid) {
		if (c.main.cycle == c.twin.cycle) {
			n = std::snprintf(out + used, cap - used, " twin=EQUAL]");
		} else {
			const int twin_pct = frame_pct(c.twin);
			if (twin_pct >= 0) {
				n = std::snprintf(out + used, cap - used,
				                  " twin=%u:%u %d%%]", c.twin.frame,
				                  c.twin.cycle, twin_pct);
			} else {
				n = std::snprintf(out + used, cap - used, " twin=%u:%u]",
				                  c.twin.frame, c.twin.cycle);
			}
		}
	} else {
		n = std::snprintf(out + used, cap - used, "]");
	}
	if (n < 0 || used + size_t(n) >= cap) out[cap - 1] = '\0';
}

void write_line(FILE * out, Level level, bool with_timing, const char * timing,
                const char * module, const char * text) {
	const char * mod = (module && *module) ? module : "?";
	if (with_timing && timing && timing[0]) {
		std::fprintf(out, "%s: %s %s\n", mod, timing, text ? text : "");
	} else {
		std::fprintf(out, "%s: %s\n", mod, text ? text : "");
	}
	if (level == Level::Error) std::fflush(out);
}

const char * basename_of(const char * path) {
	if (!path || !*path) return "revm";
	const char * slash = std::strrchr(path, '/');
	return slash ? slash + 1 : path;
}

} // namespace

void Init(std::optional<uint32_t> verbose_from_cycle,
          std::optional<uint32_t> debug_from_cycle) {
	state().verbose_from_cycle = verbose_from_cycle;
	state().debug_from_cycle = debug_from_cycle;
}

void Banner(const char * argv0) {
	FILE * out = Stream();
	std::fprintf(out, "\n======== %s ========\n", basename_of(argv0));
	const State & s = state();
	if (s.verbose_from_cycle && s.debug_from_cycle &&
	    *s.debug_from_cycle != *s.verbose_from_cycle) {
		Logf(Level::Info, false, "revm",
		     "log-debug from cycle %u, log-verbose from cycle %u",
		     *s.debug_from_cycle, *s.verbose_from_cycle);
	} else if (s.verbose_from_cycle) {
		Logf(Level::Info, false, "revm", "log-verbose from cycle %u",
		     *s.verbose_from_cycle);
	} else if (s.debug_from_cycle) {
		Logf(Level::Info, false, "revm", "log-debug from cycle %u",
		     *s.debug_from_cycle);
	} else {
		Logf(Level::Info, false, "revm", "Info+Error (Debug off)");
	}
	std::fflush(out);
}

void Shutdown() {
	State & s = state();
	s.clock = nullptr;
	s.verbose_from_cycle.reset();
	s.debug_from_cycle.reset();
	std::fflush(Stream());
}

void SetClockSource(ClockSource fn) { state().clock = std::move(fn); }

void ClearClockSource() { state().clock = nullptr; }

bool HasClockSource() { return static_cast<bool>(state().clock); }

Clocks SampleClocks() { return sample_clocks(); }

FILE * Stream() { return stderr; }

bool Enabled(Level level) {
	if (level == Level::Info || level == Level::Error) return true;
	const Clocks c = sample_clocks();
	if (level == Level::Debug) return debug_active(c);
	if (level == Level::Verbose) return verbose_active(c);
	return false;
}

void Logf(Level level, bool timed, const char * module, const char * fmt, ...) {
	std::va_list ap;
	va_start(ap, fmt);
	Logv(level, timed, module, fmt, ap);
	va_end(ap);
}

void Logv(Level level, bool timed, const char * module, const char * fmt,
          std::va_list ap) {
	Clocks clocks{};
	bool sampled = false;
	auto ensure_clocks = [&]() -> const Clocks & {
		if (!sampled) {
			clocks = sample_clocks();
			sampled = true;
		}
		return clocks;
	};

	if (level == Level::Verbose) {
		if (!verbose_active(ensure_clocks())) return;
	} else if (level == Level::Debug) {
		if (!debug_active(ensure_clocks())) return;
	} else if (level != Level::Info && level != Level::Error) {
		return;
	}

	char buf[2048];
	std::vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
	// Callers must not end fmt with '\n' — write_line / the multi-line path
	// always terminate the entry. Embedded '\n' for continuations is fine.

	FILE * out = Stream();
	const bool with_timing = timed && HasClockSource();
	char timing[128];
	timing[0] = '\0';
	if (with_timing) append_timing(timing, sizeof timing, ensure_clocks());

	const char * nl = std::strchr(buf, '\n');
	if (!nl) {
		write_line(out, level, with_timing, timing, module, buf);
		return;
	}

	// First line gets module (+ optional timing); the rest print verbatim.
	const size_t first_len = size_t(nl - buf);
	char first[2048];
	if (first_len >= sizeof first) {
		write_line(out, level, with_timing, timing, module, buf);
		return;
	}
	std::memcpy(first, buf, first_len);
	first[first_len] = '\0';
	write_line(out, level, with_timing, timing, module, first);
	std::fputs(nl + 1, out);
	std::fputc('\n', out);
	if (level == Level::Error) std::fflush(out);
}

} // namespace revm::log
