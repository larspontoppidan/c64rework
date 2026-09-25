// Created  : 2026-07-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "debug/KnowledgeBase.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace revm {
namespace {

void skip_ws(const std::string & s, size_t & i) {
	while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

bool match(const std::string & s, size_t & i, char c) {
	skip_ws(s, i);
	if (i >= s.size() || s[i] != c) return false;
	++i;
	return true;
}

bool parse_hex4(const std::string & s, size_t & i, unsigned & out,
                std::string & error) {
	if (i + 4 > s.size()) {
		error = "truncated \\u escape (want 4 hex digits)";
		return false;
	}
	unsigned v = 0;
	for (int k = 0; k < 4; ++k) {
		char d = s[i + k];
		v <<= 4;
		if (d >= '0' && d <= '9') v |= unsigned(d - '0');
		else if (d >= 'a' && d <= 'f') v |= unsigned(d - 'a' + 10);
		else if (d >= 'A' && d <= 'F') v |= unsigned(d - 'A' + 10);
		else {
			error = "invalid hex digit in \\u escape";
			return false;
		}
	}
	i += 4;
	out = v;
	return true;
}

void append_utf8(std::string & out, unsigned cp) {
	if (cp < 0x80) {
		out.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

bool parse_string(const std::string & s, size_t & i, std::string & out, std::string & error) {
	skip_ws(s, i);
	if (i >= s.size() || s[i] != '"') {
		error = "expected string";
		return false;
	}
	++i;
	out.clear();
	while (i < s.size()) {
		char c = s[i++];
		if (c == '"') return true;
		if (c == '\\') {
			if (i >= s.size()) {
				error = "unterminated string escape";
				return false;
			}
			char e = s[i++];
			switch (e) {
			case '"':
			case '\\':
			case '/':
				out.push_back(e);
				break;
			case 'n':
				out.push_back('\n');
				break;
			case 'b':
				out.push_back('\b');
				break;
			case 'f':
				out.push_back('\f');
				break;
			case 'u': {
				unsigned cp = 0;
				if (!parse_hex4(s, i, cp, error)) return false;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					// High surrogate: must be followed by a low-surrogate escape.
					if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u') {
						error = "lone high surrogate in \\u escape";
						return false;
					}
					i += 2;
					unsigned lo = 0;
					if (!parse_hex4(s, i, lo, error)) return false;
					if (lo < 0xDC00 || lo > 0xDFFF) {
						error = "high surrogate not followed by low surrogate";
						return false;
					}
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					error = "lone low surrogate in \\u escape";
					return false;
				}
				append_utf8(out, cp);
				break;
			}
			case 't':
				out.push_back('\t');
				break;
			case 'r':
				out.push_back('\r');
				break;
			default:
				out.push_back(e);
				break;
			}
		} else {
			out.push_back(c);
		}
	}
	error = "unterminated string";
	return false;
}

bool parse_hex_u16(const std::string & text, uint16_t & out, std::string & error) {
	if (text.empty()) {
		error = "empty address";
		return false;
	}
	const char * p = text.c_str();
	if (*p == '$') ++p;
	else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
	char * end = nullptr;
	unsigned long v = std::strtoul(p, &end, 16);
	if (end == p || *end != '\0' || v > 0xffffu) {
		error = "invalid address: " + text;
		return false;
	}
	out = static_cast<uint16_t>(v);
	return true;
}

bool parse_kind(const std::string & t, KbKind & out) {
	if (t == "variable") {
		out = KbKind::Variable;
		return true;
	}
	if (t == "function") {
		out = KbKind::Function;
		return true;
	}
	if (t == "label") {
		out = KbKind::Label;
		return true;
	}
	if (t == "blob") {
		out = KbKind::Blob;
		return true;
	}
	if (t == "opcodes") {
		out = KbKind::Opcodes;
		return true;
	}
	if (t == "comment") {
		out = KbKind::Comment;
		return true;
	}
	return false;
}

bool parse_watch(const std::string & t, KbWatchLevel & out) {
	if (t == "no") {
		out = KbWatchLevel::No;
		return true;
	}
	if (t == "yes") {
		out = KbWatchLevel::Yes;
		return true;
	}
	if (t == "verbose") {
		out = KbWatchLevel::Verbose;
		return true;
	}
	return false;
}

bool parse_yes_no(const std::string & t, bool & out) {
	if (t == "yes" || t == "true") {
		out = true;
		return true;
	}
	if (t == "no" || t == "false") {
		out = false;
		return true;
	}
	return false;
}

bool parse_bank(const std::string & t, std::string & out, std::string & error) {
	if (t.empty()) {
		error = "empty bank";
		return false;
	}
	// Accept any non-empty id; documented values: ram, kernal, basic, char.
	for (unsigned char c : t) {
		if (!std::isalnum(c) && c != '_' && c != '-') {
			error = "invalid bank (use letters/digits/_/-): " + t;
			return false;
		}
	}
	out = t;
	return true;
}

// Skip a JSON value (object/array/string/number/literal) starting at i.
bool skip_value(const std::string & s, size_t & i, std::string & error) {
	skip_ws(s, i);
	if (i >= s.size()) {
		error = "unexpected end while skipping value";
		return false;
	}
	char c = s[i];
	if (c == '"') {
		std::string tmp;
		return parse_string(s, i, tmp, error);
	}
	if (c == '{') {
		++i;
		skip_ws(s, i);
		if (i < s.size() && s[i] == '}') {
			++i;
			return true;
		}
		for (;;) {
			std::string key;
			if (!parse_string(s, i, key, error)) return false;
			if (!match(s, i, ':')) {
				error = "expected ':' in object";
				return false;
			}
			if (!skip_value(s, i, error)) return false;
			skip_ws(s, i);
			if (i < s.size() && s[i] == ',') {
				++i;
				continue;
			}
			if (!match(s, i, '}')) {
				error = "expected '}' in object";
				return false;
			}
			return true;
		}
	}
	if (c == '[') {
		++i;
		skip_ws(s, i);
		if (i < s.size() && s[i] == ']') {
			++i;
			return true;
		}
		for (;;) {
			if (!skip_value(s, i, error)) return false;
			skip_ws(s, i);
			if (i < s.size() && s[i] == ',') {
				++i;
				continue;
			}
			if (!match(s, i, ']')) {
				error = "expected ']' in array";
				return false;
			}
			return true;
		}
	}
	// number / true / false / null
	while (i < s.size()) {
		char d = s[i];
		if (std::isspace(static_cast<unsigned char>(d)) || d == ',' || d == ']' ||
		    d == '}')
			break;
		++i;
	}
	return true;
}

bool parse_object_fields(const std::string & s, size_t & i, KbObject & obj,
                         std::string & error) {
	if (!match(s, i, '{')) {
		error = "expected object '{'";
		return false;
	}
	skip_ws(s, i);
	if (i < s.size() && s[i] == '}') {
		++i;
		error = "KB object missing required \"addr\"";
		return false;
	}

	bool have_addr = false;
	for (;;) {
		std::string key;
		if (!parse_string(s, i, key, error)) return false;
		if (!match(s, i, ':')) {
			error = "expected ':' after key";
			return false;
		}
		if (key == "name" || key == "kind" || key == "addr" || key == "end" ||
		    key == "comment" || key == "watch" || key == "trace" || key == "format" ||
		    key == "bank") {
			std::string val;
			if (!parse_string(s, i, val, error)) return false;
			if (key == "name") {
				obj.name = val;
			} else if (key == "kind") {
				if (!parse_kind(val, obj.kind)) {
					error = "invalid kind: " + val;
					return false;
				}
			} else if (key == "addr") {
				if (!parse_hex_u16(val, obj.addr, error)) return false;
				have_addr = true;
			} else if (key == "end") {
				uint16_t e = 0;
				if (!parse_hex_u16(val, e, error)) return false;
				obj.end = e;
			} else if (key == "comment") {
				obj.comment = val;
			} else if (key == "format") {
				obj.format = val;
			} else if (key == "watch") {
				if (!parse_watch(val, obj.watch)) {
					error = "invalid watch: " + val;
					return false;
				}
			} else if (key == "trace") {
				if (!parse_yes_no(val, obj.trace)) {
					error = "invalid trace: " + val;
					return false;
				}
			} else if (key == "bank") {
				if (!parse_bank(val, obj.bank, error)) return false;
			}
		} else {
			if (!skip_value(s, i, error)) return false;
		}
		skip_ws(s, i);
		if (i < s.size() && s[i] == ',') {
			++i;
			continue;
		}
		if (!match(s, i, '}')) {
			error = "expected '}' after object fields";
			return false;
		}
		break;
	}
	if (!have_addr) {
		error = "KB object missing required \"addr\"";
		return false;
	}
	if (obj.end && *obj.end < obj.addr) {
		error = "KB object end < addr";
		return false;
	}
	if (obj.kind == KbKind::Blob && !obj.end) {
		error = "blob requires \"end\"";
		return false;
	}
	if (obj.kind == KbKind::Opcodes && !obj.end) {
		error = "opcodes requires \"end\"";
		return false;
	}
	return true;
}

} // namespace

void KnowledgeBase::Clear() {
	objects_.clear();
}

bool KnowledgeBase::LoadFile(const std::string & path, std::string & error) {
	std::ifstream f(path);
	if (!f) {
		error = "Cannot read " + path;
		return false;
	}
	std::stringstream buf;
	buf << f.rdbuf();
	return LoadString(buf.str(), error);
}

bool KnowledgeBase::LoadString(const std::string & json, std::string & error) {
	Clear();
	size_t i = 0;
	if (!match(json, i, '{')) {
		error = "KB root must be an object";
		return false;
	}

	bool have_version = false;
	bool have_objects = false;
	int version = 0;

	skip_ws(json, i);
	if (i < json.size() && json[i] == '}') {
		error = "KB missing version and objects";
		return false;
	}

	for (;;) {
		std::string key;
		if (!parse_string(json, i, key, error)) return false;
		if (!match(json, i, ':')) {
			error = "expected ':' after top-level key";
			return false;
		}

		if (key == "version") {
			skip_ws(json, i);
			char * end = nullptr;
			version = int(std::strtol(json.c_str() + i, &end, 10));
			if (end == json.c_str() + i) {
				error = "invalid version";
				return false;
			}
			i = size_t(end - json.c_str());
			have_version = true;
		} else if (key == "objects") {
			if (!match(json, i, '[')) {
				error = "objects must be an array";
				return false;
			}
			skip_ws(json, i);
			if (i < json.size() && json[i] == ']') {
				++i;
				have_objects = true;
			} else {
				for (;;) {
					KbObject obj;
					if (!parse_object_fields(json, i, obj, error)) return false;
					objects_.push_back(std::move(obj));
					skip_ws(json, i);
					if (i < json.size() && json[i] == ',') {
						++i;
						continue;
					}
					if (!match(json, i, ']')) {
						error = "expected ']' after objects";
						return false;
					}
					break;
				}
				have_objects = true;
			}
		} else {
			if (!skip_value(json, i, error)) return false;
		}

		skip_ws(json, i);
		if (i < json.size() && json[i] == ',') {
			++i;
			continue;
		}
		if (!match(json, i, '}')) {
			error = "expected '}' at end of KB";
			return false;
		}
		break;
	}

	if (!have_version) {
		error = "KB missing \"version\"";
		return false;
	}
	if (version != 1) {
		error = "unsupported KB version (want 1)";
		return false;
	}
	if (!have_objects) {
		error = "KB missing \"objects\"";
		return false;
	}
	return true;
}

const KbObject * KnowledgeBase::FindByName(const std::string & name) const {
	if (name.empty()) return nullptr;
	for (const auto & o : objects_) {
		if (o.name == name) return &o;
	}
	return nullptr;
}

const KbObject * KnowledgeBase::FindByAddr(uint16_t addr) const {
	const KbObject * exact_ram = nullptr;
	const KbObject * exact_rom = nullptr;
	const KbObject * cover_ram = nullptr;
	const KbObject * cover_rom = nullptr;
	for (const auto & o : objects_) {
		const uint16_t hi = o.end ? *o.end : o.addr;
		if (addr < o.addr || addr > hi) continue;
		if (o.addr == addr) {
			if (o.IsRamBank()) {
				if (!exact_ram) exact_ram = &o;
			} else if (!exact_rom) {
				exact_rom = &o;
			}
		} else if (o.IsRamBank()) {
			cover_ram = &o;
		} else {
			cover_rom = &o;
		}
	}
	if (exact_ram) return exact_ram;
	if (exact_rom) return exact_rom;
	if (cover_ram) return cover_ram;
	return cover_rom;
}

namespace {

const char * kind_str(KbKind k) {
	switch (k) {
	case KbKind::Variable: return "variable";
	case KbKind::Function: return "function";
	case KbKind::Label: return "label";
	case KbKind::Blob: return "blob";
	case KbKind::Opcodes: return "opcodes";
	case KbKind::Comment: return "comment";
	}
	return "comment";
}

void append_json_string(std::string & out, const std::string & s) {
	out.push_back('"');
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04X", c);
				out += buf;
			} else {
				out.push_back(static_cast<char>(c));
			}
			break;
		}
	}
	out.push_back('"');
}

void append_hex_u16(std::string & out, uint16_t v) {
	char buf[8];
	std::snprintf(buf, sizeof(buf), "\"%04X\"", v);
	out += buf;
}

} // namespace

void KnowledgeBase::SortByAddress() {
	std::stable_sort(objects_.begin(), objects_.end(),
	                 [](const KbObject & a, const KbObject & b) {
		                 const std::string ba = a.IsRamBank() ? "ram" : a.bank;
		                 const std::string bb = b.IsRamBank() ? "ram" : b.bank;
		                 if (ba != bb) return ba < bb;
		                 if (a.addr != b.addr) return a.addr < b.addr;
		                 if (a.kind != b.kind) return static_cast<int>(a.kind) <
		                                            static_cast<int>(b.kind);
		                 return a.name < b.name;
	                 });
}

bool KnowledgeBase::SaveFile(const std::string & path, std::string & error) const {
	std::string json;
	json.reserve(objects_.size() * 128 + 64);
	json += "{\n  \"version\": 1,\n  \"objects\": [\n";
	for (size_t i = 0; i < objects_.size(); ++i) {
		const KbObject & o = objects_[i];
		json += "    {\n";
		bool first = true;
		auto sep = [&] {
			if (!first) json += ",\n";
			first = false;
		};
		if (!o.name.empty()) {
			sep();
			json += "      \"name\": ";
			append_json_string(json, o.name);
		}
		if (o.kind != KbKind::Comment) {
			sep();
			json += "      \"kind\": \"";
			json += kind_str(o.kind);
			json += "\"";
		}
		sep();
		json += "      \"addr\": ";
		append_hex_u16(json, o.addr);
		if (o.end) {
			sep();
			json += "      \"end\": ";
			append_hex_u16(json, *o.end);
		}
		if (!o.format.empty()) {
			sep();
			json += "      \"format\": ";
			append_json_string(json, o.format);
		}
		if (o.watch == KbWatchLevel::Yes) {
			sep();
			json += "      \"watch\": \"yes\"";
		} else if (o.watch == KbWatchLevel::Verbose) {
			sep();
			json += "      \"watch\": \"verbose\"";
		}
		if (o.trace) {
			sep();
			json += "      \"trace\": \"yes\"";
		}
		if (!o.IsRamBank()) {
			sep();
			json += "      \"bank\": ";
			append_json_string(json, o.bank);
		}
		if (!o.comment.empty()) {
			sep();
			json += "      \"comment\": ";
			append_json_string(json, o.comment);
		}
		json += "\n    }";
		if (i + 1 < objects_.size()) json += ",";
		json += "\n";
	}
	json += "  ]\n}\n";

	std::ofstream f(path);
	if (!f) {
		error = "Cannot write " + path;
		return false;
	}
	f << json;
	if (!f) {
		error = "Write failed: " + path;
		return false;
	}
	return true;
}

} // namespace revm
