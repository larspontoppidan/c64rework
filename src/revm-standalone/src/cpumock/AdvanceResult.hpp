// Created  : 2026-09-11
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

namespace revm {

// Result of a plugin-driven Main board advance.
enum class AdvanceResult {
	Ok = 0,
	HitVSync,
	HitIrq,
	HitNmi,
	LeftNest,
	Timeout,
	Stopped,
	Error,
};

inline const char * AdvanceResultName(AdvanceResult r) {
	switch (r) {
	case AdvanceResult::Ok: return "Ok";
	case AdvanceResult::HitVSync: return "HitVSync";
	case AdvanceResult::HitIrq: return "HitIrq";
	case AdvanceResult::HitNmi: return "HitNmi";
	case AdvanceResult::LeftNest: return "LeftNest";
	case AdvanceResult::Timeout: return "Timeout";
	case AdvanceResult::Stopped: return "Stopped";
	case AdvanceResult::Error: return "Error";
	}
	return "?";
}

} // namespace revm
