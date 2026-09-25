// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "input/GoldenInput.hpp"

namespace revm {

void GoldenInput::Clear() {
	events_.clear();
	current_ = InputFrame{};
}

void GoldenInput::SetEvent(uint32_t frame, const InputFrame & state) {
	events_[frame] = state;
}

void GoldenInput::LoadEvents(const std::map<uint32_t, InputFrame> & events) {
	events_ = events;
	current_ = InputFrame{};
}

void GoldenInput::SeekTo(uint32_t frame) {
	current_ = InputFrame{};
	for (const auto & [f, st] : events_) {
		if (f > frame) break;
		current_ = st;
	}
}

InputFrame GoldenInput::PollFrame(uint32_t frame) {
	auto it = events_.find(frame);
	if (it != events_.end()) {
		current_ = it->second;
	}
	return current_;
}

} // namespace revm
