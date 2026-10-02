// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "goldens/PlayLog.hpp"

#include "C64.h"
#include "nlohmann/json.hpp"
#define REVM_LOG_MODULE "play"
#include "util/Log.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace revm {
namespace {

void append_joy(std::vector<std::string> & out, const char * prefix,
                const JoystickState & j) {
	if (j.up) out.push_back(std::string(prefix) + " up");
	if (j.down) out.push_back(std::string(prefix) + " down");
	if (j.left) out.push_back(std::string(prefix) + " left");
	if (j.right) out.push_back(std::string(prefix) + " right");
	if (j.fire) out.push_back(std::string(prefix) + " fire");
}

bool apply_joy_dir(JoystickState & j, const std::string & dir, std::string & error) {
	if (dir == "up") j.up = true;
	else if (dir == "down") j.down = true;
	else if (dir == "left") j.left = true;
	else if (dir == "right") j.right = true;
	else if (dir == "fire") j.fire = true;
	else {
		error = "Unknown joystick direction: " + dir;
		return false;
	}
	return true;
}

std::vector<std::string> keyboard_to_key_names(const KeyboardState & k) {
	std::vector<std::string> out;
	for (int row = 0; row < 8; ++row) {
		for (int bit = 0; bit < 8; ++bit) {
			if ((k.matrix[row] & (1u << bit)) == 0) {
				out.push_back(StringForKeycode(unsigned((row << 3) | bit)));
			}
		}
	}
	return out;
}

bool keyboard_from_key_names(const std::vector<std::string> & names, KeyboardState & k,
                             std::string & error) {
	k.Clear();
	for (const auto & name : names) {
		const int kc = KeycodeFromString(name);
		if (kc < 0 || kc >= 64) {
			error = "Unknown or non-matrix C64 key: " + name;
			return false;
		}
		const int row = (kc >> 3) & 7;
		const int bit = kc & 7;
		k.matrix[row] = uint8_t(k.matrix[row] & ~(1u << bit));
		k.rev_matrix[bit] = uint8_t(k.rev_matrix[bit] & ~(1u << row));
	}
	return true;
}

std::vector<std::string> json_string_array(const json & arr) {
	std::vector<std::string> out;
	if (!arr.is_array()) return out;
	out.reserve(arr.size());
	for (const auto & v : arr) {
		if (v.is_string()) out.push_back(v.get<std::string>());
	}
	return out;
}

bool parse_u64_key(const std::string & key, uint64_t & out, std::string & error,
                   const char * what) {
	char * end = nullptr;
	const unsigned long long v = std::strtoull(key.c_str(), &end, 10);
	if (end == key.c_str() || (end && *end != '\0')) {
		error = std::string("Bad ") + what + " key: " + key;
		return false;
	}
	out = uint64_t(v);
	return true;
}

} // namespace

std::vector<std::string> InputFrameToInputs(const InputFrame & in) {
	std::vector<std::string> out;
	append_joy(out, "J1", in.joy1);
	append_joy(out, "J2", in.joy2);
	for (const auto & name : keyboard_to_key_names(in.keyboard)) {
		out.push_back("K " + name);
	}
	return out;
}

bool InputFrameFromInputs(const std::vector<std::string> & inputs, InputFrame & out,
                          std::string & error) {
	error.clear();
	out = InputFrame{};
	out.joykey = 0xff;
	std::vector<std::string> keys;
	for (const auto & tok : inputs) {
		const size_t sp = tok.find(' ');
		if (sp == std::string::npos || sp == 0 || sp + 1 >= tok.size()) {
			error = "Bad input token \"" + tok + "\" (expected \"KIND rest\")";
			return false;
		}
		const std::string kind = tok.substr(0, sp);
		const std::string rest = tok.substr(sp + 1);
		if (kind == "J1" || kind == "J2") {
			JoystickState & j = (kind == "J1") ? out.joy1 : out.joy2;
			if (!apply_joy_dir(j, rest, error)) {
				error = "Bad joystick token \"" + tok + "\": " + error;
				return false;
			}
		} else if (kind == "K") {
			keys.push_back(rest);
		} else {
			error = "Unknown input kind \"" + kind + "\" in \"" + tok + "\"";
			return false;
		}
	}
	return keyboard_from_key_names(keys, out.keyboard, error);
}

bool SavePlayLog(const std::string & path, const PlayLog & log, std::string & error) {
	if (log.end_frame == 0) {
		error = "Cannot save play without a positive end_frame: " + path;
		return false;
	}
	fs::path p(path);
	if (p.has_parent_path()) {
		std::error_code ec;
		fs::create_directories(p.parent_path(), ec);
	}

	nlohmann::ordered_json j;
	j["format_version"] = log.format_version > 0 ? log.format_version : 1;
	j["source"] = {
		{"type", log.source.type},
		{"path", log.source.path},
		{"sha256", log.source.sha256},
	};
	if (log.source.type == "d64")
		j["source"]["auto_load_d64"] = log.source.auto_load_d64;
	j["rand_seed"] = log.rand_seed;
	if (log.no_twin) j["no-twin"] = true;
	nlohmann::ordered_json roms;
	if (!log.roms.basic.empty()) roms["basic"] = log.roms.basic;
	if (!log.roms.kernal.empty()) roms["kernal"] = log.roms.kernal;
	if (!log.roms.chargen.empty()) roms["chargen"] = log.roms.chargen;
	if (!log.roms.dos1541ii.empty()) roms["dos1541ii"] = log.roms.dos1541ii;
	if (!roms.empty()) j["roms"] = std::move(roms);
	j["start_frame"] = log.start_frame;
	j["start_cycle"] = log.start_cycle;
	j["end_frame"] = log.end_frame;

	nlohmann::ordered_json timestamps = nlohmann::ordered_json::array();
	for (const auto & m : log.timestamps) {
		timestamps.push_back({
			{"frame", m.frame},
			{"cycle", m.cycle},
		});
	}
	j["timestamps"] = std::move(timestamps);

	nlohmann::ordered_json input_events = nlohmann::ordered_json::object();
	for (const auto & [frame, state] : log.events) {
		input_events[std::to_string(frame)] = InputFrameToInputs(state);
	}
	j["input_events"] = std::move(input_events);

	nlohmann::ordered_json mod_events = nlohmann::ordered_json::object();
	for (const auto & [frame, mods] : log.mod_events) {
		nlohmann::ordered_json entries = nlohmann::ordered_json::array();
		for (const auto & mod : mods) {
			entries.push_back({
				{"address", mod.address},
				{"operation", mod.operation == MemoryModOperation::Increment
				                  ? "increment"
				                  : "decrement"},
			});
		}
		mod_events[std::to_string(frame)] = std::move(entries);
	}
	j["mod_events"] = std::move(mod_events);

	nlohmann::ordered_json snapshots = nlohmann::ordered_json::array();
	for (const auto & s : log.snapshots) {
		nlohmann::ordered_json e;
		e["cycle"] = s.cycle;
		e["frame"] = s.frame;
		e["sha256"] = s.sha256;
		if (!s.filename.empty()) e["filename"] = s.filename;
		snapshots.push_back(std::move(e));
	}
	j["snapshots"] = std::move(snapshots);

	std::ofstream f(path);
	if (!f) {
		error = "Cannot write " + path;
		return false;
	}
	f << j.dump(2) << '\n';
	return bool(f);
}

bool LoadPlayLog(const std::string & path, PlayLog & log, std::string & error) {
	std::ifstream f(path);
	if (!f) {
		error = "Cannot read " + path;
		return false;
	}

	json j;
	try {
		f >> j;
	} catch (const json::exception & ex) {
		error = std::string("JSON parse error: ") + ex.what();
		return false;
	}

	log = PlayLog{};
	try {
		if (j.contains("format_version")) log.format_version = j["format_version"].get<int>();
		if (j.contains("source") && j["source"].is_object()) {
			const auto & s = j["source"];
			if (s.contains("type")) log.source.type = s["type"].get<std::string>();
			if (s.contains("path")) log.source.path = s["path"].get<std::string>();
			if (s.contains("sha256")) log.source.sha256 = s["sha256"].get<std::string>();
			log.source.auto_load_d64 = s.value("auto_load_d64", true);
		}
		if (j.contains("rand_seed")) log.rand_seed = j["rand_seed"].get<unsigned>();
		if (j.contains("no-twin")) log.no_twin = j["no-twin"].get<bool>();
		if (j.contains("roms") && j["roms"].is_object()) {
			const auto & r = j["roms"];
			if (r.contains("basic")) log.roms.basic = r["basic"].get<std::string>();
			if (r.contains("kernal")) log.roms.kernal = r["kernal"].get<std::string>();
			if (r.contains("chargen")) log.roms.chargen = r["chargen"].get<std::string>();
			if (r.contains("dos1541ii")) log.roms.dos1541ii = r["dos1541ii"].get<std::string>();
		}
		if (j.contains("start_frame")) log.start_frame = j["start_frame"].get<uint32_t>();
		if (j.contains("start_cycle")) log.start_cycle = j["start_cycle"].get<uint64_t>();
		if (!j.contains("end_frame")) {
			error = "Play log is missing required end_frame; retrofit a positive "
			        "end_frame explicitly: " + path;
			return false;
		}
		log.end_frame = j["end_frame"].get<uint32_t>();
		if (log.end_frame == 0) {
			error = "Play log end_frame must be positive: " + path;
			return false;
		}
		const json * marks = nullptr;
		if (j.contains("timestamps") && j["timestamps"].is_array()) {
			marks = &j["timestamps"];
		} else if (j.contains("frame_marks") && j["frame_marks"].is_array()) {
			// Legacy key.
			marks = &j["frame_marks"];
		}
		if (marks) {
			for (const auto & t : *marks) {
				PlayTimestamp m;
				if (t.is_number_unsigned() || t.is_number_integer()) {
					// Legacy: bare frame number.
					m.frame = t.get<uint32_t>();
				} else if (t.is_object()) {
					if (t.contains("frame")) m.frame = t["frame"].get<uint32_t>();
					if (t.contains("cycle")) m.cycle = t["cycle"].get<uint64_t>();
				} else {
					error = "timestamps entries must be objects or frame numbers";
					return false;
				}
				log.timestamps.push_back(m);
			}
		}
		if (!j.contains("input_events") || !j["input_events"].is_object()) {
			error = "Missing input_events object";
			return false;
		}
		for (auto it = j["input_events"].begin(); it != j["input_events"].end(); ++it) {
			uint64_t frame_u = 0;
			if (!parse_u64_key(it.key(), frame_u, error, "input_events")) return false;
			if (!it.value().is_array()) {
				error = "input_events[" + it.key() + "] must be an array";
				return false;
			}
			InputFrame in;
			if (!InputFrameFromInputs(json_string_array(it.value()), in, error)) {
				return false;
			}
			log.events[uint32_t(frame_u)] = in;
		}
		if (j.contains("mod_events")) {
			if (!j["mod_events"].is_object()) {
				error = "mod_events must be an object";
				return false;
			}
			for (auto it = j["mod_events"].begin(); it != j["mod_events"].end(); ++it) {
				uint64_t frame_u = 0;
				if (!parse_u64_key(it.key(), frame_u, error, "mod_events")) return false;
				if (frame_u > UINT32_MAX) {
					error = "mod_events frame out of range: " + it.key();
					return false;
				}
				if (!it.value().is_array()) {
					error = "mod_events[" + it.key() + "] must be an array";
					return false;
				}
				auto & mods = log.mod_events[uint32_t(frame_u)];
				for (const auto & entry : it.value()) {
					if (!entry.is_object() || !entry.contains("address") ||
					    !entry.contains("operation")) {
						error = "mod_events[" + it.key() +
						        "] entries need address and operation";
						return false;
					}
					const uint64_t address = entry["address"].get<uint64_t>();
					if (address > UINT16_MAX) {
						error = "mod_events[" + it.key() + "] address out of range";
						return false;
					}
					const std::string operation = entry["operation"].get<std::string>();
					MemoryModification mod;
					mod.address = uint16_t(address);
					if (operation == "increment") {
						mod.operation = MemoryModOperation::Increment;
					} else if (operation == "decrement") {
						mod.operation = MemoryModOperation::Decrement;
					} else {
						error = "Unknown mod_events operation: " + operation;
						return false;
					}
					mods.push_back(mod);
				}
			}
		}
		if (j.contains("snapshots") && j["snapshots"].is_array()) {
			for (const auto & e : j["snapshots"]) {
				if (!e.is_object()) {
					error = "snapshots[] entries must be objects";
					return false;
				}
				PlaySnapshot s;
				if (!e.contains("cycle") || !e.contains("sha256")) {
					error = "snapshots[] entry needs cycle and sha256";
					return false;
				}
				s.cycle = e["cycle"].get<uint64_t>();
				s.sha256 = e["sha256"].get<std::string>();
				if (e.contains("frame")) s.frame = e["frame"].get<uint32_t>();
				if (e.contains("filename")) s.filename = e["filename"].get<std::string>();
				log.snapshots.push_back(std::move(s));
			}
		} else if (j.contains("snapshot_hashes") && j["snapshot_hashes"].is_object()) {
			// Legacy: cycle→sha256 map (+ optional cycle→path snapshots object).
			std::map<uint64_t, std::string> filenames;
			if (j.contains("snapshots") && j["snapshots"].is_object()) {
				for (auto it = j["snapshots"].begin(); it != j["snapshots"].end(); ++it) {
					uint64_t cycle = 0;
					if (!parse_u64_key(it.key(), cycle, error, "snapshots")) return false;
					if (it.value().is_string()) filenames[cycle] = it.value().get<std::string>();
				}
			}
			for (auto it = j["snapshot_hashes"].begin(); it != j["snapshot_hashes"].end();
			     ++it) {
				uint64_t cycle = 0;
				if (!parse_u64_key(it.key(), cycle, error, "snapshot_hashes")) return false;
				if (!it.value().is_string()) {
					error = "snapshot_hashes[" + it.key() + "] must be a string";
					return false;
				}
				PlaySnapshot s;
				s.cycle = cycle;
				s.sha256 = it.value().get<std::string>();
				auto fit = filenames.find(cycle);
				if (fit != filenames.end()) s.filename = fit->second;
				log.snapshots.push_back(std::move(s));
			}
		}
	} catch (const json::exception & ex) {
		error = std::string("JSON schema error: ") + ex.what();
		return false;
	}
	return true;
}

bool LoadInputEvents(const std::string & path,
                     std::map<uint32_t, InputFrame> & events,
                     std::string & error) {
	events.clear();
	PlayLog log;
	if (!LoadPlayLog(path, log, error)) return false;
	events = std::move(log.events);
	return true;
}

bool PlayEndFrame(const std::string & path, uint32_t & frame_out, std::string & error) {
	PlayLog log;
	if (!LoadPlayLog(path, log, error)) return false;
	frame_out = log.end_frame;
	return true;
}

void UpsertPlaySnapshot(PlayLog & log, uint64_t cycle, uint32_t frame,
                        const std::string & sha256, const std::string & filename) {
	for (auto & s : log.snapshots) {
		if (s.cycle == cycle) {
			s.frame = frame;
			s.sha256 = sha256;
			if (!filename.empty()) s.filename = filename;
			return;
		}
	}
	PlaySnapshot s;
	s.cycle = cycle;
	s.frame = frame;
	s.sha256 = sha256;
	s.filename = filename;
	log.snapshots.push_back(std::move(s));
	std::sort(log.snapshots.begin(), log.snapshots.end(),
	          [](const PlaySnapshot & a, const PlaySnapshot & b) {
		          return a.cycle < b.cycle;
	          });
}

const PlaySnapshot * FindPlaySnapshot(const PlayLog & log, uint64_t cycle) {
	for (const auto & s : log.snapshots) {
		if (s.cycle == cycle) return &s;
	}
	return nullptr;
}

bool RegisterPlaySnapshot(const std::string & play_path, uint64_t cycle,
                          uint32_t frame, const std::string & snap_rel_path,
                          const std::string & sha256, std::string & error) {
	PlayLog log;
	if (!LoadPlayLog(play_path, log, error)) return false;
	UpsertPlaySnapshot(log, cycle, frame, sha256, snap_rel_path);
	return SavePlayLog(play_path, log, error);
}

void WarnPlayUsage(const PlayLog & log, bool system_no_twin) {
	if (log.no_twin != system_no_twin) {
		if (system_no_twin) {
			REVM_LOG(REVM_INFO,
			         "Play was recorded original, this system is no-twin, playback will not be faithful");
		} else {
			REVM_LOG(REVM_INFO,
			         "Play was recorded no-twin, this system is original, playback will not be faithful");
		}
	}
}

} // namespace revm
