// Created  : 2026-08-08
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>

namespace revm::log {

enum class Level {
	Verbose = 0,
	Debug = 1,
	Info = 2,
	Error = 3,
};

// Same shape as the Board timing the host samples for timed logs. Zeroed
// until a clock source is registered.
struct BoardClock {
	uint32_t frame = 0;
	uint32_t cycle = 0;
	uint32_t frame_start = 0;
	uint32_t frame_end = 0;
};

using ClockSource = std::function<BoardClock()>;

// Verbose / Debug print only when their from-cycle is set and Main cycle >= it.
// --log-verbose also enables Debug from the same cycle. Unset = that level off.
void Init(std::optional<uint32_t> verbose_from_cycle,
          std::optional<uint32_t> debug_from_cycle = {});
// Clear separator so build&&revm output is easy to spot. Call after Init.
void Banner(const char * argv0 = nullptr);
void Shutdown();

void SetClockSource(ClockSource fn);
void ClearClockSource();
bool HasClockSource();

// Diagnostics sink (stderr). Use for FAIL detail dumps so they stay with logs.
FILE * Stream();

bool Enabled(Level level);

// `module` is a short stable tag (e.g. "cpumock", "watch-main") — printed as
// "module: …". Prefer REVM_LOG / REVM_LOG_TIMED with REVM_LOG_MODULE; use
// REVM_LOG_M / REVM_LOG_TIMED_M when the module is runtime. Do not end fmt with
// '\n' (the logger adds it). Embedded '\n' is allowed for multi-line bodies.
void Logf(Level level, bool timed, const char * module, const char * fmt, ...)
	__attribute__((format(printf, 4, 5)));
void Logv(Level level, bool timed, const char * module, const char * fmt,
          std::va_list ap);

} // namespace revm::log

// Severity tokens for REVM_LOG / REVM_LOG_TIMED (not syslog LOG_INFO etc.).
#define REVM_VERBOSE ::revm::log::Level::Verbose
#define REVM_DEBUG ::revm::log::Level::Debug
#define REVM_INFO ::revm::log::Level::Info
#define REVM_ERROR ::revm::log::Level::Error

// Per .cpp file, before first REVM_LOG / REVM_LOG_TIMED use:
//   #define REVM_LOG_MODULE "cpumock"
// REVM_LOG_MODULE is expanded at the call site, so a transitive include of
// this header (Mem.hpp → Sync.hpp) does not capture an empty module.
// Init / Banner / Logf / REVM_LOG_M do not require it. revm-tool stays on
// plain fprintf(stderr) — it is not a run/play harness.

#define REVM_LOG(level, ...) \
	::revm::log::Logf((level), false, REVM_LOG_MODULE, __VA_ARGS__)
#define REVM_LOG_TIMED(level, ...) \
	::revm::log::Logf((level), true, REVM_LOG_MODULE, __VA_ARGS__)

#define REVM_LOG_M(mod, level, ...) \
	::revm::log::Logf((level), false, (mod), __VA_ARGS__)
#define REVM_LOG_TIMED_M(mod, level, ...) \
	::revm::log::Logf((level), true, (mod), __VA_ARGS__)
