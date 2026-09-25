// Created  : 2026-08-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

// Compiles against reSID only (see CMake include dirs). Do not include Frodo.

#include "sid/ResidEngine.hpp"

#include "sid.h"

namespace {

constexpr reSID::sampling_method kMethod = reSID::SAMPLE_INTERPOLATE;

} // namespace

struct ResidEngine::Impl {
	reSID::SID sid;
};

ResidEngine::ResidEngine() : impl_(std::make_unique<Impl>()) {
	SetChip6581();
}

ResidEngine::~ResidEngine() = default;

void ResidEngine::Reset() {
	impl_->sid.reset();
}

void ResidEngine::SetChip6581() {
	impl_->sid.set_chip_model(reSID::MOS6581);
}

bool ResidEngine::SetSampling(double clock_hz, double sample_hz) {
	return impl_->sid.set_sampling_parameters(clock_hz, kMethod, sample_hz);
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
