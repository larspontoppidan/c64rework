// Created  : 2026-09-05
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "core/RunSetup.hpp"

namespace revm {

const char * BoardRoleName(BoardRole role) {
	switch (role) {
	case BoardRole::C64Real: return "c64-real";
	case BoardRole::CpuMock: return "cpu-mock";
	case BoardRole::C64Twin: return "c64-twin";
	}
	return "?";
}

std::string FormatActiveBoards(const ActiveRunSetup & active) {
	std::string line = BoardRoleName(active.primary);
	if (active.secondary_enabled && active.secondary)
		line.append(", ").append(BoardRoleName(*active.secondary));
	return line;
}

BuiltRunSetup BuiltSetup() {
	BuiltRunSetup built;
#if defined(REVM_HAS_CPUMOCK)
	built.primary = BoardRole::CpuMock;
	built.secondary = BoardRole::C64Twin;
#else
	built.primary = BoardRole::C64Real;
#endif
	return built;
}

bool ResolveActiveSetup(const BuiltRunSetup & built, const RunSetupFlags & flags,
                        ActiveRunSetup & active, std::string & error) {
	if (flags.no_twin &&
	    !(built.secondary && *built.secondary == BoardRole::C64Twin)) {
		error = "--no-twin is only valid when this build's secondary is c64-twin";
		return false;
	}
	active.primary = built.primary;
	active.secondary = built.secondary;
	active.secondary_enabled = bool(built.secondary);
	if (flags.no_twin) {
		active.secondary_enabled = false;
		active.secondary.reset();
	}
	return true;
}

bool SetupHasCpuMock(const ActiveRunSetup & active) {
	return active.primary == BoardRole::CpuMock ||
	       (active.secondary_enabled && active.secondary &&
	        *active.secondary == BoardRole::CpuMock);
}

bool SetupHasTwin(const ActiveRunSetup & active) {
	return active.secondary_enabled && active.secondary &&
	       *active.secondary == BoardRole::C64Twin;
}

} // namespace revm
