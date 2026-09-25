// Created  : 2026-09-14
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "core/Config.hpp"
#include "input/JoystickConfig.hpp"

#include <cstdint>
#include <string>

namespace gamehost {
class GameHost;
}

namespace revm {

// One configuration for both executable entry paths. Board options live in
// `config`; `max_frames` is the run bound; `joystick` is live-only.
struct StandaloneRun {
	Config config{};
	uint32_t max_frames = 0;
	const JoystickConfig * joystick = nullptr;
};

// Owns launch, JSON replay, and JSON recording. The game-facing machine lives
// on gamehost::GameHost; this runner constructs one per run.
class StandaloneRunner {
public:
	int RunPlayback(const std::string & play_path, StandaloneRun run = {});
	int RunPlay(StandaloneRun run = {});
	int RunRecord(const std::string & play_path, StandaloneRun run = {});

private:
	static void install_game(gamehost::GameHost & host, uint32_t max_frames);
};

} // namespace revm
