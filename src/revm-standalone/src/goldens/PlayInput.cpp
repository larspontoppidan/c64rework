// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "goldens/PlayInput.hpp"

#include "C64.h"
#include "nlohmann/json.hpp"
#define REVM_LOG_MODULE "play"
#include "util/Log.hpp"

#include <cstdlib>
#include <fstream>
#include <vector>
using json = nlohmann::json;

namespace revm {
namespace {

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

} // namespace

bool PlayInput::Load(const std::string & path, std::string & error) {
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

	*this = PlayInput{};
	try {
		if (j.contains("rand_seed")) rand_seed_ = j["rand_seed"].get<unsigned>();
		if (j.contains("no-twin")) no_twin_ = j["no-twin"].get<bool>();
		if (j.contains("start_frame")) start_frame_ = j["start_frame"].get<uint32_t>();
		if (j.contains("start_cycle")) start_cycle_ = j["start_cycle"].get<uint64_t>();
		if (!j.contains("end_frame")) {
			error = "Play log is missing required end_frame; retrofit a positive "
			        "end_frame explicitly: " + path;
			return false;
		}
		end_frame_ = j["end_frame"].get<uint32_t>();
		if (end_frame_ == 0) {
			error = "Play log end_frame must be positive: " + path;
			return false;
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
			events_[uint32_t(frame_u)] = in;
		}
		bool has_mod_events = false;
		if (j.contains("mod_events")) {
			if (!j["mod_events"].is_object()) {
				error = "mod_events must be an object";
				return false;
			}
			for (auto it = j["mod_events"].begin(); it != j["mod_events"].end(); ++it) {
				if (it.value().is_array() && !it.value().empty()) {
					has_mod_events = true;
					break;
				}
			}
		}
		if (has_mod_events) {
			REVM_LOG(REVM_INFO,
			         "Play contains mod_events, standalone does not support them, playback will not be faithful");
		}
	} catch (const json::exception & ex) {
		error = std::string("JSON schema error: ") + ex.what();
		return false;
	}
	if (!no_twin_) {
		REVM_LOG(REVM_INFO,
		         "Play was recorded original, this system is no-twin, playback will not be faithful");
	}
	return true;
}

void PlayInput::SeekTo(uint32_t frame) {
	current_ = InputFrame{};
	for (const auto & [f, st] : events_) {
		if (f > frame) break;
		current_ = st;
	}
}

InputFrame PlayInput::PollFrame(uint32_t frame) {
	auto it = events_.find(frame);
	if (it != events_.end())
		current_ = it->second;
	return current_;
}

} // namespace revm
