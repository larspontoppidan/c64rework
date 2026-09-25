// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "input/InputState.hpp"

#include <cstdint>

namespace revm {

// Input is resolved once per frame at VSYNC (never mid-frame).
class IInputSource {
public:
	virtual ~IInputSource() = default;

	// Called at frame boundary. `frame` is the frame that just completed.
	virtual InputFrame PollFrame(uint32_t frame) = 0;
};

} // namespace revm
