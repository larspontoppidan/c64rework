// Created  : 2026-09-05
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <optional>
#include <string>

namespace revm {

// Fixed board roles. A build has one primary and at most one secondary.
enum class BoardRole {
	C64Real,
	CpuMock,
	C64Twin,
};

// How a Board is constructed. Replaces the old Init / InitSecondary boolean.
enum class BoardInitRole {
	Primary,             // SDL/media owner; normal frame processing
	SecondaryTwinQuiet,  // existing c64-twin: no SDL/media, quiet VBLANK
	SecondaryRunner,     // no SDL/media ownership; normal frame processing
};

struct BuiltRunSetup {
	BoardRole primary = BoardRole::C64Real;
	std::optional<BoardRole> secondary;
};

struct ActiveRunSetup {
	BoardRole primary = BoardRole::C64Real;
	std::optional<BoardRole> secondary; // nullopt when disabled or absent
	bool secondary_enabled = false;
};

const char * BoardRoleName(BoardRole role);
std::string FormatActiveBoards(const ActiveRunSetup & active);

// Compile-time inventory of this binary. Runtime may only disable the
// configured secondary; it cannot construct an arbitrary topology.
BuiltRunSetup BuiltSetup();

struct RunSetupFlags {
	bool no_twin = false;       // valid only when secondary is c64-twin
};

bool ResolveActiveSetup(const BuiltRunSetup & built, const RunSetupFlags & flags,
                        ActiveRunSetup & active, std::string & error);

bool SetupHasCpuMock(const ActiveRunSetup & active);
bool SetupHasTwin(const ActiveRunSetup & active);

} // namespace revm
