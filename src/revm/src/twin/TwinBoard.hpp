// Created  : 2026-07-21
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "core/Board.hpp"
#include "input/InputState.hpp"
#include "snapshot/Snapshot.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace revm {

// Result of plugin-driven board advances (Main or Twin).
enum class AdvanceResult {
	Ok = 0,     // finished the request inside the current frame
	HitVSync,   // stopped at VBLANK (not "Twin left this IRQ/NMI nest")
	HitIrq,     // parked at an IRQ accept; Sync runs RunIrqHandler
	HitNmi,     // parked at an NMI accept; Sync runs RunNmiHandler
	LeftNest,   // Twin RTI'd / left this accept; ReturnIrq done (not VBLANK)
	Timeout,    // max_cycles without meeting the goal
	Stopped,    // quit requested
	Error,      // protocol violation (e.g. Twin more than one VSYNC ahead)
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

// Architectural 6510 registers (subset of MOS6510State)
struct CpuState {
	uint8_t a = 0;
	uint8_t x = 0;
	uint8_t y = 0;
	uint8_t p = 0; // NV-BDIZC
	uint16_t pc = 0;
	uint8_t sp = 0; // low byte; page $0100
	bool irq_pending = false;
	bool nmi_pending = false;
};

// Headless live-oracle C64: same BEGIN snap + golden inputs as Main.
// Independent 6510 — does NOT mirror Main RAM.
//
// Stage 3 (single-threaded, plugin-driven):
//   Twin advances only when the plugin calls TwinRun / TwinRunTo*.
//   At each Main VSYNC the host winds Twin to that barrier
//   (and applies the frame's input) before golden compare.
// Twin may lead Main by at most one VSYNC.
class TwinBoard {
public:
	bool Init(std::string & error);
	bool LoadSnap(const std::string & path, std::string & error);
	bool LoadSnap(const FullSnapshot & snap, std::string & error);

	void ApplyInput(const InputFrame & in);
	// Advance until next VIC VBLANK. Returns the new frame counter.
	uint32_t RunToVSync();
	// Advance while PC != bp. Stops at PC (Ok) or at the next VBLANK (HitVSync).
	AdvanceResult RunToPc(uint16_t pc, uint64_t max_cycles = 2'000'000);
	// Advance up to `cycles` or next VBLANK.
	AdvanceResult Run(uint64_t cycles);
	// Advance until IRQ would be accepted (irq_pending && I clear), or VBLANK.
	AdvanceResult RunToIrq(uint64_t max_cycles = 2'000'000);
	// Probe helper: run until VBLANK, recording CycleCounter at each fetch-cycle
	// accept (instruction_complete, BA high, IRQ pending && I clear / NMI
	// pending). Drains the 7-cycle steal so pending-still-true cycles are not
	// extra slots. Live Twin is not this board.
	struct HwAcceptCalendar {
		std::vector<uint32_t> irq_at;
		std::vector<uint32_t> nmi_at;
	};
	AdvanceResult RecordHwAcceptsUntilVSync(HwAcceptCalendar & out,
	                                        uint64_t max_cycles = 2'000'000);

	// Forward to Board PC probe (log module watch-twin on secondary board).
	void WatchPc(uint16_t pc, std::string label);
	void ClearPcWatches();

	uint32_t CycleCounter() const;
	uint32_t FrameCounter() const;
	// Current 6510 PC (instruction fetch address when instruction_complete).
	uint16_t Pc() const;
	// Current 6510 stack pointer (low byte; page `$0100`).
	uint8_t Sp() const;
	// Architectural regs in one GetState.
	CpuState GetCpuState() const;
	// O_FETCH, instruction_complete, BA high — next EmulateCycle fetches or
	// takes IRQ/NMI. O_FETCH does not re-check I once irq_pending is latched.
	bool AtOpcodeFetch() const;
	// O_FETCH at `pc`, including BA stall. TwinRunToPc must fence here:
	// AtOpcodeFetch is false while BA is low, and the next EmulateCycle
	// can raise BA then fetch the opcode in the same Φ2 (VIC then CPU).
	bool AtPc(uint16_t pc) const;
	bool FetchWouldTakeHw() const;
	// True while Frodo is in the IRQ steal sequence (O_IRQ). FetchWouldTakeHw
	// can miss the accept when BA was low then rose in the same EmulateCycle.
	bool InIrqSequence() const;
	// True while Frodo is in the NMI steal sequence (O_NMI). Same BA-stall
	// hole as InIrqSequence: the confirm is the EmulateCycle that enters it.
	bool InNmiSequence() const;

	// Peek into Twin memory
	uint8_t Peek(uint16_t addr) const;

	// Last Twin 6510 I/O-space read_byte / write_byte (not rdbuf).
	// cycle is CycleCounter at the CPU access (before C64 ++). RMW: last write.
	struct IoAccess {
		uint32_t cycle = 0;
		uint16_t addr = 0;
		uint8_t value = 0;
	};
	bool LastIoRead(IoAccess & out) const;
	bool LastIoWrite(IoAccess & out) const;

	bool Capture(FullSnapshot & out) const;
	bool CaptureChips(ChipSnapshot & out) const;
	bool CaptureScreen(ScreenSnapshot & out) const;

	Board & board() { return board_; }
	const Board & board() const { return board_; }

private:
	Board board_;
	bool ready_ = false; // set by Init; host only keeps twin_ after LoadSnap succeeds
};

} // namespace revm
