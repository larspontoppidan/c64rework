// Created  : 2026-07-25
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "goldens/PlayRecorder.hpp"
#include "core/Board.hpp"

#include "C64.h"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <vector>
#define REVM_LOG_MODULE "play-rec"
#include "util/Log.hpp"

namespace fs = std::filesystem;

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

std::vector<std::string> InputFrameToInputs(const InputFrame & in) {
	std::vector<std::string> out;
	append_joy(out, "J1", in.joy1);
	append_joy(out, "J2", in.joy2);
	for (const auto & name : keyboard_to_key_names(in.keyboard)) {
		out.push_back("K " + name);
	}
	return out;
}

} // namespace

void PlayRecorder::Configure(const std::string & out_path, PlaySource source) {
	out_path_ = out_path;
	source_ = std::move(source);
	rand_seed_ = 42;
	start_frame_ = 0;
	start_cycle_ = 0;
	end_frame_ = 0;
	events_.clear();
	last_input_ = InputFrame{};
	have_end_ = false;
	last_frame_ = 0;
	board_ = nullptr;
}

void PlayRecorder::Start(Board & board) {
	board_ = &board;
	rand_seed_ = board.GetConfig().rand_seed;
	start_frame_ = board.FrameCounter();
	start_cycle_ = board.CycleCounter();
	events_.clear();
	last_input_ = InputFrame{};
	have_end_ = false;
	REVM_LOG(REVM_DEBUG, "start_frame=%u start_cycle=%llu rand_seed=%u → %s",
		start_frame_,
		static_cast<unsigned long long>(start_cycle_),
		rand_seed_,
		out_path_.c_str());
}

void PlayRecorder::OnFrame(uint32_t frame, const InputFrame & input) {
	if (!(input == last_input_)) {
		events_[frame] = input;
		last_input_ = input;
	}
	last_frame_ = frame;
	have_end_ = true;
}

bool PlayRecorder::Finish(std::string & error) {
	if (have_end_) {
		end_frame_ = last_frame_;
	} else {
		end_frame_ = start_frame_;
	}
	if (end_frame_ == 0) {
		error = "Cannot save play without a positive end_frame: " + out_path_;
		return false;
	}

	fs::path p(out_path_);
	if (p.has_parent_path()) {
		std::error_code ec;
		fs::create_directories(p.parent_path(), ec);
	}

	nlohmann::ordered_json j;
	j["format_version"] = 1;
	j["source"] = {
		{"type", source_.type},
		{"path", source_.path},
		{"sha256", source_.sha256},
	};
	j["rand_seed"] = rand_seed_;
	j["no-twin"] = true;
	j["start_frame"] = start_frame_;
	j["start_cycle"] = start_cycle_;
	j["end_frame"] = end_frame_;
	j["timestamps"] = nlohmann::ordered_json::array();

	nlohmann::ordered_json input_events = nlohmann::ordered_json::object();
	for (const auto & [frame, state] : events_) {
		input_events[std::to_string(frame)] = InputFrameToInputs(state);
	}
	j["input_events"] = std::move(input_events);
	j["mod_events"] = nlohmann::ordered_json::object();
	j["snapshots"] = nlohmann::ordered_json::array();

	std::ofstream f(out_path_);
	if (!f) {
		error = "Cannot write " + out_path_;
		return false;
	}
	f << j.dump(2) << '\n';
	if (!f) {
		error = "Cannot write " + out_path_;
		return false;
	}
	REVM_LOG(REVM_DEBUG,
	         "Play recorded to %s  (%zu input events, end_frame=%u)",
	         out_path_.c_str(), events_.size(), end_frame_);
	return true;
}

} // namespace revm
