// Created  : 2026-08-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Thin C++ bridge over reSID so Frodo TUs never include resid/sid.h
// (collides with Frodo SID.h on case-insensitive filesystems).

#include <cstdint>
#include <memory>

class ResidEngine {
public:
	ResidEngine();
	~ResidEngine();

	ResidEngine(const ResidEngine &) = delete;
	ResidEngine & operator=(const ResidEngine &) = delete;

	void Reset();
	void SetChip6581();
	bool SetSampling(double clock_hz, double sample_hz);

	void Write(uint8_t reg, uint8_t value);

	// Consume up to *delta_cycles of SID time into buf[0..n). Returns samples
	// written; *delta_cycles is reduced by the cycles actually used.
	int Clock(int & delta_cycles, int16_t * buf, int n);

	// Advance the chip without producing samples.
	void ClockSilent(int delta_cycles);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
