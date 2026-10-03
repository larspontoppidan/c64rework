// Created  : 2026-08-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

// Compiles against reSID only (see CMake include dirs). Do not include Frodo.

#include "sid/ResidEngine.hpp"

#include "sid.h"

struct ResidEngine::Impl {
	reSID::SID sid;
};

ResidEngine::ResidEngine() : impl_(std::make_unique<Impl>()) {
	SetChipModel(false);
}

ResidEngine::~ResidEngine() = default;

void ResidEngine::Reset() {
	impl_->sid.reset();
}

void ResidEngine::SetChipModel(bool is8580) {
	impl_->sid.set_chip_model(is8580 ? reSID::MOS8580 : reSID::MOS6581);
}

bool ResidEngine::SetSampling(double clock_hz, double sample_hz, bool high_quality) {
	const auto method = high_quality ? reSID::SAMPLE_RESAMPLE_FASTMEM
	                                 : reSID::SAMPLE_INTERPOLATE;
	return impl_->sid.set_sampling_parameters(clock_hz, method, sample_hz);
}

void ResidEngine::Write(uint8_t reg, uint8_t value) {
	impl_->sid.write(reg, value);
}

int ResidEngine::Clock(int & delta_cycles, int16_t * buf, int n) {
	reSID::cycle_count delta = delta_cycles;
	const int produced = impl_->sid.clock(delta, buf, n);
	delta_cycles = int(delta);
	return produced;
}

void ResidEngine::ClockSilent(int delta_cycles) {
	if (delta_cycles > 0) {
		impl_->sid.clock(reSID::cycle_count(delta_cycles));
	}
}
