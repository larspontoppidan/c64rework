// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "input/IInputSource.hpp"

#include <map>
#include <vector>

namespace revm {

// Plays back recorded input events keyed by frame number.
class GoldenInput : public IInputSource {
public:
	void Clear();
	void SetEvent(uint32_t frame, const InputFrame & state);
	void LoadEvents(const std::map<uint32_t, InputFrame> & events);

	// Advance held state to the last event at or before `frame` (Stage-2
	// begin restore skips earlier frames without walking them).
	void SeekTo(uint32_t frame);

	InputFrame PollFrame(uint32_t frame) override;

	const std::map<uint32_t, InputFrame> & Events() const { return events_; }

private:
	std::map<uint32_t, InputFrame> events_;
	InputFrame current_;
};

} // namespace revm
