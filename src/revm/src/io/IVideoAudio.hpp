// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstddef>
#include <cstdint>

namespace revm {

class IVideoSink {
public:
	virtual ~IVideoSink() = default;

	// Present a completed frame. `pixels` is DISPLAY_X * DISPLAY_Y indexed palette bytes.
	virtual void PresentFrame(const uint8_t * pixels, int width, int height, int pitch) = 0;

	virtual void SetTitle(const char * title) = 0;
};

class IAudioSink {
public:
	virtual ~IAudioSink() = default;
	// SID is currently driven by Frodo's internal SDL audio; this interface
	// is reserved for stage-later decoupling / recording.
	virtual void WriteSamples(const float * samples, std::size_t count) = 0;
};

} // namespace revm
