#pragma once

#include "cpumock/CpuMockHost.hpp"
#include "cpumock/Mem.hpp"

namespace replica {

class Game {
	revm::cpumock::CpuMockHost & host_;

	// Illustrative addresses only. Replace every one with facts from Stage 1,
	// the game KB, and the disassembly before setting kConfigured to true.
	static constexpr bool kConfigured = false;

	struct Ram {
		explicit Ram(revm::cpumock::CpuMockHost & host) : host_(host) {}

	private:
		revm::cpumock::CpuMockHost & host_;

	public:
		revm::cpumock::Mem<0x0002> example_counter{host_};
	} mem{host_};

	revm::cpumock::IoMap io{host_};

	void first_routine();

public:
	explicit Game(revm::cpumock::CpuMockHost & host);
	void run();
};

} // namespace replica
