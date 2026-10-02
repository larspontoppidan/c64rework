// Created  : 2026-08-24
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "util/Event.hpp"

#define REVM_LOG_MODULE "event"
#include "util/Log.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>

namespace revm::event {

namespace {

struct Chan {
	uint64_t compared = 0;
	uint64_t failed = 0;
};

struct SkewAgg {
	uint64_t paired = 0;
	uint64_t silent = 0;
	int32_t min = 0;
	int32_t max = 0;
	bool any = false;
};

struct HandlerAgg {
	uint64_t enter = 0;
	uint64_t exit_ = 0;
};

struct SoftquitRec {
	int code = 0;
	std::string msg;
};

struct State {
	bool active = false; // events and/or report requested
	FILE * sink = nullptr; // JSONL sink; nullptr when only aggregating
	bool sink_stderr = false;

	std::string report_path;

	uint64_t total_events = 0;
	std::map<std::string, uint64_t> kind_counts;

	Chan screen;
	Chan sid;
	Chan vic;
	Chan vic_state;
	Chan cia1;
	Chan cia2;
	Chan kb;
	SkewAgg skew[2] = {}; // [irq, nmi]
	HandlerAgg handlers[2] = {};
	uint64_t extra_vsync[3] = {}; // [join, ram, nested]
	std::map<uint16_t, std::pair<std::string, uint64_t>> watch_hits;
	std::vector<SoftquitRec> softquits; // first kMaxSoftquits kept
	bool have_first_fail = false;
	std::string run_end_result;
	uint32_t run_end_frames = 0;
	bool shutdown_done = false;

	// Bounded recent-record ring for fail-context embedding (completed
	// JSONL lines, oldest first). Filled only while a sink/aggregation is
	// active; costs one string move per record.
	std::deque<std::string> recent;
};

static constexpr size_t kMaxSoftquits = 32;
static constexpr size_t kMaxWatchRows = 64;

State & state() {
	static State s;
	return s;
}

void append_escaped(std::string & out, const char * s) {
	const char * p = s ? s : "";
	for (; *p; ++p) {
		const unsigned char c = static_cast<unsigned char>(*p);
		switch (*p) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char b[8];
				std::snprintf(b, sizeof b, "\\u%04x", c);
				out += b;
			} else {
				out += char(c);
			}
		}
	}
}

// Quoted JSON string (used for keys too — they are literals today).
void append_quoted(std::string & out, const char * s) {
	out += '"';
	append_escaped(out, s);
	out += '"';
}

void append_key(std::string & out, const char * key) {
	append_quoted(out, key ? key : "?");
}

void append_val(std::string & out, const Val & v) {
	char num[32];
	switch (v.t) {
	case Val::T::Bool:
		out += v.i ? "true" : "false";
		break;
	case Val::T::Int:
		std::snprintf(num, sizeof num, "%lld", v.i);
		out += num;
		break;
	case Val::T::UInt:
		std::snprintf(num, sizeof num, "%llu", v.u);
		out += num;
		break;
	case Val::T::Hex:
		out += '"';
		std::snprintf(num, sizeof num, "0x%llX",
		              static_cast<unsigned long long>(v.u));
		out += num;
		out += '"';
		break;
	case Val::T::Str:
		out += '"';
		append_escaped(out, v.s);
		out += '"';
		break;
	case Val::T::Raw:
		out += (v.s && *v.s) ? v.s : "null";
		break;
	}
}

void write_line(const std::string & line, bool flush_now) {
	State & s = state();
	if (!s.sink) return;
	if (s.sink_stderr) {
		std::fprintf(s.sink, "EVENT %s\n", line.c_str());
	} else {
		std::fwrite(line.data(), 1, line.size(), s.sink);
		std::fputc('\n', s.sink);
	}
	// Flush only failure/terminal records so their context survives a hard
	// _Exit; everything else relies on stdio buffering and Shutdown's fclose.
	if (flush_now) std::fflush(s.sink);
}

void aggregate(const char * kind) {
	State & s = state();
	++s.total_events;
	++s.kind_counts[kind];
}

void finish_record(const char * kind, std::string line) {
	aggregate(kind);
	line += '}';
	// Context records already embed this history. Keeping them would
	// recursively duplicate it across repeated ignored failures.
	State & s = state();
	if (std::strcmp(kind, "fail_context") != 0) {
		if (s.recent.size() >= kRecentMax) s.recent.pop_front();
		s.recent.push_back(line);
	}
	write_line(line, std::strcmp(kind, "softquit") == 0 ||
	                     std::strcmp(kind, "run_end") == 0 ||
	                     std::strcmp(kind, "fail_context") == 0);
}

int site_index(const char * site) {
	if (site && std::strcmp(site, "ram") == 0) return 1;
	if (site && std::strcmp(site, "nested") == 0) return 2;
	return 0; // "join" + unknown sites
}

std::string report_json() {
	State & s = state();
	std::string o = "{\"v\":1\n";

	auto chan_obj = [&](const Chan & c) {
		char b[128];
		std::snprintf(b, sizeof b,
		              "{\"compared\":%llu,\"failed\":%llu}",
		              static_cast<unsigned long long>(c.compared),
		              static_cast<unsigned long long>(c.failed));
		return std::string(b);
	};
	o += ",\"channels\":{\"screen\":" + chan_obj(s.screen) +
	     ",\"sid\":" + chan_obj(s.sid) + ",\"vic\":" + chan_obj(s.vic) +
	     ",\"vic_state\":" + chan_obj(s.vic_state) +
	     ",\"cia1\":" + chan_obj(s.cia1) + ",\"cia2\":" + chan_obj(s.cia2) +
	     ",\"kb\":" + chan_obj(s.kb) + "}";

	auto skew_obj = [&](const SkewAgg & k) {
		char b[192];
		std::snprintf(b, sizeof b,
		              "{\"paired\":%llu,\"silent\":%llu,\"min\":%d,\"max\":%d}",
		              static_cast<unsigned long long>(k.paired),
		              static_cast<unsigned long long>(k.silent), k.any ? k.min : 0,
		              k.any ? k.max : 0);
		return std::string(b);
	};
	o += ",\"skew\":{\"irq\":" + skew_obj(s.skew[0]) +
	     ",\"nmi\":" + skew_obj(s.skew[1]) + "}";

	auto handler_obj = [&](const HandlerAgg & h) {
		char b[96];
		std::snprintf(b, sizeof b, "{\"enter\":%llu,\"exit\":%llu}",
		              static_cast<unsigned long long>(h.enter),
		              static_cast<unsigned long long>(h.exit_));
		return std::string(b);
	};
	o += ",\"handlers\":{\"irq\":" + handler_obj(s.handlers[0]) +
	     ",\"nmi\":" + handler_obj(s.handlers[1]) + "}";

	// Parity: paired accepts vs handler enters (only meaningful with Twin;
	// without skew data it is reported as "n/a").
	auto parity = [&](int idx) {
		const SkewAgg & k = s.skew[idx];
		const HandlerAgg & h = s.handlers[idx];
		if (!k.any && k.silent == 0) return "\"n/a\"";
		return (k.paired == h.enter && h.exit_ == h.enter) ? "\"pass\""
		                                                   : "\"fail\"";
	};
	o += std::string(",\"parity\":{\"irq\":") + parity(0) +
	     ",\"nmi\":" + parity(1) + "}";

	char b[160];
	std::snprintf(
		b, sizeof b,
		",\"extra_vsync\":{\"join\":%llu,\"ram\":%llu,\"nested\":%llu}",
		static_cast<unsigned long long>(s.extra_vsync[0]),
		static_cast<unsigned long long>(s.extra_vsync[1]),
		static_cast<unsigned long long>(s.extra_vsync[2]));
	o += b;

	o += ",\"watch_hits\":[";
	{
		size_t rows = 0;
		for (const auto & [pc, hit] : s.watch_hits) {
			if (rows >= kMaxWatchRows) {
				o += ",{\"overflow\":true}";
				break;
			}
			if (rows++) o += ',';
			char pb[48];
			std::snprintf(pb, sizeof pb, "{\"pc\":\"0x%04X\",\"label\":", pc);
			o += pb;
			o += '"';
			append_escaped(o, hit.first.c_str());
			o += "\",\"total\":";
			std::snprintf(pb, sizeof pb, "%llu}",
			              static_cast<unsigned long long>(hit.second));
			o += pb;
		}
	}
	o += ']';

	o += ",\"softquits\":[";
	for (size_t i = 0; i < s.softquits.size(); ++i) {
		if (i) o += ',';
		o += "{\"code\":";
		std::snprintf(b, sizeof b, "%d,\"msg\":\"", s.softquits[i].code);
		o += b;
		append_escaped(o, s.softquits[i].msg.c_str());
		o += "\"}";
	}
	o += ']';

	if (s.have_first_fail) {
		o += ",\"first_fail\":{\"code\":";
		std::snprintf(b, sizeof b, "%d,\"msg\":\"", s.softquits[0].code);
		o += b;
		append_escaped(o, s.softquits[0].msg.c_str());
		o += "\"}";
	} else {
		o += ",\"first_fail\":null";
	}

	if (!s.run_end_result.empty()) {
		o += ",\"run_end\":{\"result\":\"";
		o += s.run_end_result;
		o += "\",\"frames_run\":";
		std::snprintf(b, sizeof b, "%u}", s.run_end_frames);
		o += b;
	} else {
		o += ",\"run_end\":null";
	}

	o += ",\"kinds\":{";
	{
		bool first = true;
		for (const auto & [kind, count] : s.kind_counts) {
			if (!first) o += ',';
			first = false;
			append_key(o, kind.c_str());
			char kb2[32];
			std::snprintf(kb2, sizeof kb2, ":%llu",
			              static_cast<unsigned long long>(count));
			o += kb2;
		}
	}
	o += '}';

	o += ",\"totals\":{\"events\":";
	std::snprintf(b, sizeof b, "%llu",
	              static_cast<unsigned long long>(s.total_events));
	o += b;
	o += "}}";
	return o;
}

} // namespace

bool Enabled() { return state().active; }

std::string QuoteString(const char * value) {
	std::string out;
	append_quoted(out, value);
	return out;
}

std::string RecentJsonArray() {
	State & s = state();
	if (!s.active || s.recent.empty()) return "[]";
	std::string o = "[";
	for (const std::string & line : s.recent) {
		if (o.size() > 1) o += ',';
		o += line;
	}
	o += ']';
	return o;
}

void EmitV(const char * kind, const std::vector<Field> & fields) {
	if (!Enabled()) return;

	const log::Clocks c = log::HasClockSource() ? log::SampleClocks()
	                                            : log::Clocks{};
	std::string line = "{\"v\":1,\"ev\":\"";
	line += kind ? kind : "?";
	line += "\",\"frame\":";
	char num[40];
	std::snprintf(num, sizeof num, "%u", c.main.frame);
	line += num;
	line += ",\"cyc\":";
	std::snprintf(num, sizeof num, "%u", c.main.cycle);
	line += num;
	if (c.twin_valid) {
		line += ",\"twin_frame\":";
		std::snprintf(num, sizeof num, "%u", c.twin.frame);
		line += num;
		line += ",\"twin_cyc\":";
		std::snprintf(num, sizeof num, "%u", c.twin.cycle);
		line += num;
	}
	for (const Field & f : fields) {
		line += ',';
		append_key(line, f.first);
		line += ':';
		append_val(line, f.second);
	}
	finish_record(kind, std::move(line));
}

namespace detail {
void EmitTail(const char * kind, std::vector<Field> & f) { EmitV(kind, f); }
} // namespace detail

Record::Record(const char * kind) {
	kind_ = kind ? kind : "?";
	buf_ = "{\"v\":1,\"ev\":\"";
	buf_ += kind_;
	buf_ += "\",\"frame\":";
	char num[40];
	const log::Clocks c =
		log::HasClockSource() ? log::SampleClocks() : log::Clocks{};
	std::snprintf(num, sizeof num, "%u,\"cyc\":%u", c.main.frame, c.main.cycle);
	buf_ += num;
	if (c.twin_valid) {
		std::snprintf(num, sizeof num, ",\"twin_frame\":%u,\"twin_cyc\":%u",
		              c.twin.frame, c.twin.cycle);
		buf_ += num;
	}
}

Record & Record::kv(const char * key, const Val & v) {
	buf_ += ',';
	append_key(buf_, key);
	buf_ += ':';
	append_val(buf_, v);
	return *this;
}

void Record::end() {
	if (closed_) return;
	closed_ = true;
	buf_ += '}';
	aggregate(kind_.c_str());
	write_line(buf_, kind_ == "softquit" || kind_ == "run_end");
}

void EmitSkew(const char * kind, bool paired, int32_t phi2) {
	if (!Enabled()) return;
	const int idx = (kind && std::strcmp(kind, "nmi") == 0) ? 1 : 0;
	REVM_EVENT("skew", "kind", kind ? kind : "?", "paired", paired ? 1 : 0,
	           "phi2", phi2);
	SkewAgg & k = state().skew[idx];
	if (paired) {
		++k.paired;
		if (!k.any || phi2 < k.min) k.min = phi2;
		if (!k.any || phi2 > k.max) k.max = phi2;
		k.any = true;
	} else {
		++k.silent;
	}
}

void EmitHandler(const char * kind, const char * phase) {
	if (!Enabled()) return;
	const int idx = (kind && std::strcmp(kind, "nmi") == 0) ? 1 : 0;
	REVM_EVENT("handler", "kind", kind ? kind : "?", "phase",
	           phase ? phase : "?");
	HandlerAgg & h = state().handlers[idx];
	if (phase && std::strcmp(phase, "exit") == 0)
		++h.exit_;
	else
		++h.enter;
}

void EmitExtraVsync(const char * site) {
	if (!Enabled()) return;
	REVM_EVENT("extra_vsync", "site", site ? site : "?");
	++state().extra_vsync[site_index(site)];
}

void EmitSoftquit(int code, const char * msg) {
	if (!Enabled()) return;
	REVM_EVENT("softquit", "code", code, "msg", msg ? msg : "");
	State & s = state();
	s.have_first_fail = true;
	if (s.softquits.size() < kMaxSoftquits)
		s.softquits.push_back({code, msg ? msg : ""});
}

void EmitWatchHit(uint16_t pc, const char * label, uint64_t total) {
	if (!Enabled()) return;
	REVM_EVENT("watch_hit", "pc", Hex{pc}, "label", label ? label : "",
	           "total", total);
	auto & row = state().watch_hits[pc];
	row.first = label ? label : "";
	row.second = total;
}

void EmitCompareResult(int screen_ok, int sid_ok, int kb_ok, int vic_ok,
                       int vic_state_ok, int cia1_ok, int cia2_ok,
                       int raster_line) {
	if (!Enabled()) return;
	if (raster_line >= 0) {
		REVM_EVENT("compare", "screen", screen_ok, "sid", sid_ok, "vic",
		           vic_ok, "vic_state", vic_state_ok, "cia1", cia1_ok,
		           "cia2", cia2_ok, "kb", kb_ok, "raster_line",
		           raster_line);
	} else {
		REVM_EVENT("compare", "screen", screen_ok, "sid", sid_ok, "vic",
		           vic_ok, "vic_state", vic_state_ok, "cia1", cia1_ok,
		           "cia2", cia2_ok, "kb", kb_ok);
	}
	State & s = state();
	auto note = [](Chan & c, int ok) {
		if (ok < 0) return;
		++c.compared;
		if (ok == 0) ++c.failed;
	};
	note(s.screen, screen_ok);
	note(s.sid, sid_ok);
	note(s.vic, vic_ok);
	note(s.vic_state, vic_state_ok);
	note(s.cia1, cia1_ok);
	note(s.cia2, cia2_ok);
	note(s.kb, kb_ok);
}

void SetRunEnd(const char * result, uint32_t frames_run) {
	if (!Enabled()) return;
	REVM_EVENT("run_end", "result", result ? result : "?", "frames_run",
	           frames_run);
	state().run_end_result = result ? result : "";
	state().run_end_frames = frames_run;
}

bool HasFail() { return state().have_first_fail; }

void Open(const std::string & events_path, const std::string & report_path) {
	State & s = state();
	if (events_path.empty() && report_path.empty()) return;
	if (s.active) return;
	s.active = true;
	s.report_path = report_path;
	if (!events_path.empty()) {
		if (events_path == "-") {
			s.sink = stderr;
			s.sink_stderr = true;
		} else {
			s.sink = std::fopen(events_path.c_str(), "a");
			s.sink_stderr = false;
			if (!s.sink) {
				// CLI validated writability earlier; degrade to aggregation
				// only rather than crashing mid-run.
				REVM_LOG(REVM_ERROR, "--events: cannot open %s (report-only)",
				         events_path.c_str());
			}
		}
	}
}

void Shutdown() {
	State & s = state();
	if (s.shutdown_done) return;
	s.shutdown_done = true;
	if (!s.report_path.empty()) {
		const std::string json = report_json();
		if (FILE * f = std::fopen(s.report_path.c_str(), "w")) {
			std::fwrite(json.data(), 1, json.size(), f);
			std::fclose(f);
		} else {
			REVM_LOG(REVM_ERROR, "--report: cannot write %s",
			         s.report_path.c_str());
		}
	}
	if (s.sink && !s.sink_stderr) std::fclose(s.sink);
	s.sink = nullptr;
	s.active = false;
}

} // namespace revm::event
