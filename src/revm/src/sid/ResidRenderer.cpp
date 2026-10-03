// Created  : 2026-08-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "sid/ResidRenderer.hpp"

#include "C64.h"
#include "Prefs.h"
#include "VIC.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// Wall-clock pacing is SCREEN_FREQ (50 Hz PAL), not the true Φ2 rate.
// reSID's cycle→sample ratio must match that pace or the speaker ring
// slowly underruns and SDL inserts gaps that sound like a fast vibrato.
constexpr double kPacedClockHz =
	double(CYCLES_PER_LINE) * double(TOTAL_RASTERS) * double(SCREEN_FREQ);

ResidRenderer::ResidRenderer(MOS6581 * sid) : the_sid_(sid)
{
	ring_.assign(kRingSamples, 0);
	open_speakers_if_needed();
	ready_ = true;
	Reset();
}

ResidRenderer::~ResidRenderer()
{
	close_speakers();
}

void ResidRenderer::apply_sampling()
{
	if (!engine_.SetSampling(kPacedClockHz, double(sample_rate_), high_quality_)) {
		std::fprintf(stderr, "WARNING: reSID set_sampling_parameters failed\n");
	}
	if (the_sid_) {
		the_sid_->SetSampleRate(sample_rate_);
	}
}

void ResidRenderer::sync_cycle_base()
{
	C64 * c64 = the_sid_ ? the_sid_->GetC64() : nullptr;
	last_cycle_ = c64 ? c64->CycleCounter() : 0;
	have_cycle_ = true;
}

void ResidRenderer::Reset()
{
	engine_.SetChipModel(ThePrefs.SIDType == SIDTYPE_RESID_8580);
	high_quality_ = ThePrefs.ResidHQ;
	engine_.Reset();
	apply_sampling();
	{
		std::lock_guard<std::mutex> lock(ring_mu_);
		ring_r_ = ring_w_ = ring_count_ = 0;
		last_sample_ = 0;
		std::fill(ring_.begin(), ring_.end(), int16_t(0));
	}
	audio_started_ = false;
	if (device_id_) {
		SDL_PauseAudioDevice(device_id_, true);
	}
	sync_cycle_base();
}

void ResidRenderer::NewPrefs(const Prefs * prefs)
{
	flush_cycles();
	engine_.SetChipModel(prefs->SIDType == SIDTYPE_RESID_8580);
	if (high_quality_ != prefs->ResidHQ) {
		high_quality_ = prefs->ResidHQ;
		apply_sampling();
	}
}

void ResidRenderer::Pause()
{
	paused_ = true;
	if (device_id_) {
		SDL_PauseAudioDevice(device_id_, true);
	}
}

void ResidRenderer::Resume()
{
	paused_ = false;
	if (device_id_ && audio_started_) {
		SDL_PauseAudioDevice(device_id_, false);
	}
}

void ResidRenderer::try_start_audio()
{
	if (audio_started_ || paused_ || !device_id_) return;

	const size_t want = 
		size_t(double(sample_rate_) * kStartFrames / double(SCREEN_FREQ) + 0.5);
	{
		std::lock_guard<std::mutex> lock(ring_mu_);
		if (ring_count_ < want) return;
	}

	audio_started_ = true;
	SDL_PauseAudioDevice(device_id_, false);
}

void ResidRenderer::push_samples(const int16_t * samples, int count)
{
	if (!samples || count <= 0) return;

	if (the_sid_) {
		auto tap = the_sid_->GetSampleTap();
		if (tap) {
			tap(samples, size_t(count), the_sid_->GetSampleTapUserdata());
		}
	}

	if (!device_id_) return;

	{
		std::lock_guard<std::mutex> lock(ring_mu_);
		for (int i = 0; i < count; ++i) {
			if (ring_count_ >= kRingSamples) {
				// Drop oldest sample on overrun.
				ring_r_ = (ring_r_ + 1) % kRingSamples;
				--ring_count_;
			}
			ring_[ring_w_] = samples[i];
			ring_w_ = (ring_w_ + 1) % kRingSamples;
			++ring_count_;
		}
	}
	try_start_audio();
}

void ResidRenderer::flush_cycles()
{
	if (!ready_ || !the_sid_) return;

	C64 * c64 = the_sid_->GetC64();
	if (!c64) return;

	const uint32_t now = c64->CycleCounter();
	if (!have_cycle_) {
		last_cycle_ = now;
		have_cycle_ = true;
		return;
	}

	int delta = int(now - last_cycle_); // uint32 wrap-safe via unsigned subtract
	last_cycle_ = now;
	if (delta <= 0) return;

	int16_t buf[1024];
	while (delta > 0) {
		const int before = delta;
		const int n = engine_.Clock(delta, buf, 1024);
		if (n > 0) {
			push_samples(buf, n);
		}
		if (delta == before) {
			// No progress (should not happen); burn remaining silently.
			engine_.ClockSilent(delta);
			break;
		}
	}
}

void ResidRenderer::EmulateLine()
{
	flush_cycles();
}

void ResidRenderer::WriteRegister(uint16_t adr, uint8_t byte)
{
	if (!ready_) return;
	flush_cycles();
	engine_.Write(uint8_t(adr & 0x1f), byte);
}

void ResidRenderer::open_speakers_if_needed()
{
	if (device_id_) return;
	if (!SDL_WasInit(SDL_INIT_AUDIO)) return;

	SDL_AudioSpec desired;
	SDL_zero(desired);
	desired.freq = kSampleRate;
	desired.format = AUDIO_S16SYS;
	desired.channels = 1;
	desired.samples = 256;
	desired.callback = sdl_callback;
	desired.userdata = this;

	SDL_AudioSpec obtained;
	SDL_zero(obtained);
	device_id_ = SDL_OpenAudioDevice(
		nullptr, false, &desired, &obtained,
		SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
	if (!device_id_) {
		std::fprintf(stderr, "WARNING: reSID cannot open audio: %s\n", SDL_GetError());
		return;
	}
	if (obtained.freq > 0 && obtained.freq != sample_rate_) {
		sample_rate_ = obtained.freq;
		apply_sampling();
	}
	// Stay paused until the ring has kStartFrames of PCM. Starting empty
	// makes the first callbacks race the emulator and insert gaps.
	SDL_PauseAudioDevice(device_id_, true);
}

void ResidRenderer::close_speakers()
{
	if (device_id_) {
		SDL_CloseAudioDevice(device_id_);
		device_id_ = 0;
	}
}

void ResidRenderer::sdl_callback(void * userdata, uint8_t * stream, int len)
{
	auto * self = static_cast<ResidRenderer *>(userdata);
	auto * out = reinterpret_cast<int16_t *>(stream);
	const int want = len / int(sizeof(int16_t));

	std::lock_guard<std::mutex> lock(self->ring_mu_);
	int i = 0;
	for (; i < want && self->ring_count_ > 0; ++i) {
		self->last_sample_ = self->ring_[self->ring_r_];
		out[i] = self->last_sample_;
		self->ring_r_ = (self->ring_r_ + 1) % kRingSamples;
		--self->ring_count_;
	}
	for (; i < want; ++i) {
		out[i] = self->last_sample_;
	}
}
