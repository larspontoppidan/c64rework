// Stage entry (linked into the game-specific REVM executable).

#include "cpumock/CpuMockHost.hpp"
#include "game.hpp"

namespace revm::cpumock {

void InstallGame(CpuMockHost & host) {
	host.SetEntryHandler([&host] {
		replica::Game game(host);

		// Install IRQ/NMI thunks here only after their complete handler bodies
		// have been translated. Until then, an interrupt is a loud frontier.
		game.run();
	});
}

} // namespace revm::cpumock
