// Created  : 2026-08-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "sysdeps.h"
#include "SID.h"

#include "sid/ResidEngine.hpp"

#include <SDL.h>
#include <SDL_audio.h>

#include <cstdint>
#include <mutex>
#include <vector>

// Cycle-clocked reSID renderer. PCM is produced on the emulation thread;
// SDL (when available) only drains a ring for speakers.
class ResidRenderer : public SIDRenderer {
public:
	explicit ResidRenderer(MOS6581 * sid);
	~ResidRenderer() override;

	void Reset() override;
	void EmulateLine() override;
	void WriteRegister(uint16_t adr, uint8_t byte) override;
	void NewPrefs(const Prefs * prefs) override;
	void Pause() override;
	void Resume() override;
	void RebaseCycleClock() override;

private:
	void sync_cycle_base();
	void flush_cycles();
	void push_samples(const int16_t * samples, int count);
	void apply_sampling();
	void open_speakers_if_needed();
	void close_speakers();
	void try_start_audio();

	static void sdl_callback(void * userdata, uint8_t * stream, int len);

	MOS6581 * the_sid_ = nullptr;
	ResidEngine engine_;

	uint32_t last_cycle_ = 0;
	bool have_cycle_ = false;
	bool ready_ = false;
	bool high_quality_ = false;

	// Standing speaker latency in PAL frames (1.0 ≈ 20 ms). Raise if --resid
	// underruns; 3.0 was the original conservative fill.
	static constexpr double kStartFrames = 1.0;
	static constexpr int kSampleRate = 48000;
	static constexpr size_t kRingSamples = 8192;

	int sample_rate_ = kSampleRate;

	std::mutex ring_mu_;
	std::vector<int16_t> ring_;
	size_t ring_r_ = 0;
	size_t ring_w_ = 0;
	size_t ring_count_ = 0;
	int16_t last_sample_ = 0;

	SDL_AudioDeviceID device_id_ = 0;
	bool paused_ = false;
	bool audio_started_ = false;
};
