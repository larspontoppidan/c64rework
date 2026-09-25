// Created  : 2026-08-24
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Structured JSONL event stream + derived end-of-run summary report.
//
// Purpose: agents/tools consume machine-readable events instead of
// regex-grepping human logs. Human logging (REVM_LOG VERBOSE/DEBUG/INFO) is
// unaffected — this layer writes ONLY to its own sink.
//
// Sink: enabled by CLI --events <path> (one JSON object per line, appended).
// Path "-" writes "EVENT {…}" lines to stderr. Default off = zero cost
// (single bool check per site). --report <path> additionally aggregates the
// in-memory stream at shutdown into one JSON summary (see Event.cpp).
//
// Record shape (first four fields fixed, payload follows):
//   {"v":1,"ev":"<kind>","frame":<main frame>,"cyc":<main cycle>,
//    "twin_frame":…,"twin_cyc":…, <payload>}
// Twin fields are omitted while invalid (--no-twin).
//
// Payload: variadic key/value lists — alternating `const char*` keys and
// values (bool, integers, Hex{u16/u32/u64}, const char* strings). No
// external JSON library. Game-agnostic: plugins may emit their own kinds
// later via REVM_EVENT / revm::event::Record / EmitV.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace revm::event {

// True when --events and/or --report enabled this run. When false, every
// site costs a single predictable branch (no clock sampling, no formatting).
bool Enabled();

// Hex-formatted integer value (emitted as a JSON string "0x…").
struct Hex {
	uint64_t v = 0;
	Hex(uint16_t x) : v(x) {}
	Hex(uint32_t x) : v(x) {}
	Hex(uint64_t x) : v(x) {}
	Hex(int x) : v(uint64_t(x)) {}
};

// Pre-serialized JSON fragment (object/array) emitted verbatim. The caller
// owns the buffer for the duration of the Emit call (e.g. std::string bound
// to a local). Used by the screen-fail context oracle for nested payloads.
struct RawJson {
	const char * s = nullptr;
};

// Typed runtime value (runtime variant of the variadic emitter).
struct Val {
	enum class T { Int, UInt, Hex, Str, Bool, Raw };
	T t = T::Int;
	long long i = 0;
	unsigned long long u = 0;
	const char * s = nullptr;

	Val(bool b) : t(T::Bool), i(b ? 1 : 0) {}
	Val(int v) : t(T::Int), i(v) {}
	Val(long v) : t(T::Int), i(v) {}
	Val(long long v) : t(T::Int), i(v) {}
	Val(unsigned v) : t(T::UInt), u(v) {}
	Val(unsigned long v) : t(T::UInt), u(v) {}
	Val(unsigned long long v) : t(T::UInt), u(v) {}
	Val(Hex h) : t(T::Hex), u(h.v) {}
	Val(const char * str) : t(T::Str), s(str) {}
	Val(RawJson j) : t(T::Raw), s(j.s) {}
};

using Field = std::pair<const char *, Val>;

// Runtime variant with a dynamically assembled field vector.
void EmitV(const char * kind, const std::vector<Field> & fields);

namespace detail {
void EmitTail(const char * kind, std::vector<Field> & f);

template <typename V, typename... Rest>
void EmitTail(const char * kind, std::vector<Field> & f, const char * key,
              const V & value, Rest &&... rest) {
	f.emplace_back(key, Val(value));
	EmitTail(kind, f, std::forward<Rest>(rest)...);
}
} // namespace detail

// Variadic emitter: arguments alternate key / value.
//   revm::event::Emit("fence", "op", "join", "pc", Hex{0xEC3C});
template <typename... Args>
inline void Emit(const char * kind, Args &&... args) {
	if (!Enabled()) return;
	std::vector<Field> f;
	f.reserve(sizeof...(Args) / 2);
	detail::EmitTail(kind, f, std::forward<Args>(args)...);
}

// Streaming builder (runtime variant, incremental). Stamps are sampled at
// construction; `end()` writes the line.
class Record {
public:
	explicit Record(const char * kind);
	Record & kv(const char * key, const Val & v);
	void end();

private:
	std::string kind_;
	std::string buf_;
	bool closed_ = false;
};

// --- Named emitters for kinds the report aggregates. Each emits the event
// AND updates the aggregator; all no-op instantly when disabled. ---

void EmitSkew(const char * kind /*"irq"|"nmi"*/, bool paired, int32_t phi2);
void EmitHandler(const char * kind /*"irq"|"nmi"*/, const char * phase);
void EmitExtraVsync(const char * site /*"join"|"ram"|"nested"*/);
void EmitSoftquit(int code, const char * msg);
void EmitWatchHit(uint16_t pc, const char * label, uint64_t total);
// Per-compare channel outcomes (-1 sentinel = channel not run).
void EmitCompareResult(int screen_ok, int sid_ok, int kb_ok);
void SetRunEnd(const char * result /*"ok"|"fail"|"softquit"*/,
               uint32_t frames_run);

// True once any softquit-style failure has been recorded this run.
bool HasFail();

// --- Lifecycle (host-owned) ---

// Configure sink + aggregation. Call once before the first record. Either
// path may be empty (disabled); events_path "-" selects stderr. The caller
// emits the `run_start` record as the first line when a sink is active.
void Open(const std::string & events_path, const std::string & report_path);

// Write the aggregated report (if requested) and close the sink. Safe to
// call twice.
void Shutdown();

} // namespace revm::event

#define REVM_EVENT(kind, ...)                                                 \
	do {                                                                      \
		if (::revm::event::Enabled())                                         \
			::revm::event::Emit(kind, ##__VA_ARGS__);                         \
	} while (0)
