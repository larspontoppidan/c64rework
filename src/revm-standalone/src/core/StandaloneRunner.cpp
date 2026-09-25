// Created  : 2026-09-14
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "core/StandaloneRunner.hpp"

#include "gamehost/GameHost.hpp"
#include "goldens/PlayInput.hpp"
#include "goldens/PlayRecorder.hpp"
#include "input/LiveInput.hpp"
#define REVM_LOG_MODULE "runner"
#include "util/Log.hpp"

#include <chrono>

namespace revm {

void StandaloneRunner::install_game(gamehost::GameHost & host,
                                   uint32_t max_frames) {
	host.main_start_.reset();
	gamehost::InstallGame(host);
	if (max_frames) {
		host.board_.SetFrameCallback([&host, max_frames](uint32_t frame, uint32_t) {
			if (frame >= max_frames) host.board_.RequestQuit(0);
		});
	}
}

int StandaloneRunner::RunPlayback(const std::string & play_path,
                                  StandaloneRun run) {
	gamehost::GameHost host;
	PlayInput playback;
	std::string error;
	if (!playback.Load(play_path, error)) {
		REVM_LOG(REVM_ERROR, "Failed to load play: %s", error.c_str());
		return 1;
	}
	run.config.rand_seed = playback.RandSeed();
	if (!host.init_board(run.config))
		return 1;

	host.board_.SetInputSource(&playback);
	uint32_t max_frames = run.max_frames;
	if (max_frames == 0)
		max_frames = playback.EndFrame();
	install_game(host, max_frames);

	if (!host.main_start_) {
		REVM_LOG(REVM_ERROR,
		         "blank Main requires InstallMainStart(...) in gamehost::InstallGame");
		return 1;
	}
	const gamehost::CpuMockStart & start = *host.main_start_;
	if ((playback.StartCycle() != 0 && playback.StartCycle() != start.cycle) ||
	    playback.StartFrame() != start.frame) {
		REVM_LOG(REVM_ERROR,
		         "installed Main start does not match play origin: "
		         "main cycle=%u frame=%u; play cycle=%llu frame=%u",
		         start.cycle, start.frame,
		         static_cast<unsigned long long>(playback.StartCycle()),
		         playback.StartFrame());
		return 1;
	}

	REVM_LOG(REVM_DEBUG, "--use-play %s — blank Main", play_path.c_str());

	if (!host.apply_blank_start(&playback))
		return 1;
	host.pace_ = host.board_.GetConfig().limit_speed;
	host.frame_start_ = std::chrono::steady_clock::now();
	return host.run_entry();
}

int StandaloneRunner::RunPlay(StandaloneRun run) {
	gamehost::GameHost host;
	if (!host.init_board(run.config))
		return 1;

	LiveInput live(host.board_.Machine());
	if (run.joystick)
		live.SetJoystickConfig(*run.joystick);
	host.board_.SetInputSource(&live);
	install_game(host, run.max_frames);

	REVM_LOG(REVM_DEBUG, "play — blank Main (single-thread)");

	if (!host.apply_blank_start(nullptr))
		return 1;
	host.pace_ = host.board_.GetConfig().limit_speed;
	host.frame_start_ = std::chrono::steady_clock::now();
	return host.run_entry();
}

int StandaloneRunner::RunRecord(const std::string & play_path,
                                StandaloneRun run) {
	gamehost::GameHost host;
	if (!host.init_board(run.config))
		return 1;

	LiveInput live(host.board_.Machine());
	if (run.joystick)
		live.SetJoystickConfig(*run.joystick);
	host.board_.SetInputSource(&live);

	PlaySource src;
	src.type = "none";
	PlayRecorder play;
	play.Configure(play_path, src);
	host.board_.SetPlayRecorder(&play);
	install_game(host, run.max_frames);

	REVM_LOG(REVM_DEBUG, "--record-play %s — blank Main (no-twin)",
	         play_path.c_str());

	if (!host.apply_blank_start(nullptr))
		return 1;
	play.Start(host.board_);
	host.pace_ = host.board_.GetConfig().limit_speed;
	host.frame_start_ = std::chrono::steady_clock::now();
	const int result = host.run_entry();
	std::string error;
	if (play.Started() && !play.Finish(error)) {
		REVM_LOG(REVM_ERROR, "Failed to write play log: %s", error.c_str());
		return 1;
	}
	return result;
}

} // namespace revm
