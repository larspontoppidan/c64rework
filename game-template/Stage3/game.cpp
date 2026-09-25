#include "game.hpp"

#include "cpumock/Sync.hpp"

namespace replica {

using revm::cpumock::Sync;

Game::Game(revm::cpumock::CpuMockHost & host) : host_(host) {}

void Game::run() {
	if constexpr (!kConfigured) {
		host_.SoftQuit(1,
			"Stage 3 not configured: replace the illustrative PCs and addresses");
	}

	// Replace $1000 with the frozen BEGIN PC from Stage1/NOTES.md.
	host_.AssertBegin(0x1000);
	first_routine();
}

// $1000 first_routine — illustrative shape, not game behavior.
// Keep the original instruction order visible and put its PC beside each line.
void Game::first_routine() {
	Sync::JoinAtPc(host_, 0x1000);              // $1000 routine entry

	uint8_t a = mem.example_counter.read();     // $1000 LDA $02
	a = uint8_t(a + 1);                         // $1002 CLC / ADC #$01
	mem.example_counter.write(a);               // $1005 STA $02

	// Hardware-facing accesses whose timing matters are armed at the original
	// opcode PC. The immediately following explicit bag access consumes it.
	Sync::AtPc(host_, 0x1007);                  // $1007 STA $D020
	io.vic.border.write(a);

	// Stop loudly at the first untranslated instruction. Move this frontier
	// only after the preceding translation passes its last-pass/first-fail gate.
	host_.SoftQuit(1, "not yet translated $100A next_routine");
}

} // namespace replica
