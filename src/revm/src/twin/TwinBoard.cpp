// Created  : 2026-07-21
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "twin/TwinBoard.hpp"
#include "snapshot/Snapshot.hpp"

#include "C64.h"
#include "CPUC64.h"

#include <cstring>

namespace revm {

bool TwinBoard::Init(std::string & error) {
	ready_ = false;
	Config cfg;
	cfg.headless = true;
	cfg.audio_enabled = false;
	cfg.limit_speed = false;
	board_.Configure(cfg);
	if (!board_.InitSecondary(error)) return false;
	ready_ = true;
	return true;
}

bool TwinBoard::LoadSnap(const std::string & path, std::string & error) {
	if (!ready_) {
		error = "TwinBoard not initialized";
		return false;
	}
	return board_.LoadBeginSnap(path, nullptr, error);
}

bool TwinBoard::LoadSnap(const FullSnapshot & snap, std::string & error) {
	if (!ready_) {
		error = "TwinBoard not initialized";
		return false;
	}
	if (!RestoreFullSnapshot(board_, snap)) {
		error = "RestoreFullSnapshot failed";
		return false;
	}
	board_.SyncClockFromMachine();
	return true;
}

void TwinBoard::ApplyInput(const InputFrame & in) {
	board_.ApplyInput(in);
}

void TwinBoard::WatchPc(uint16_t pc, std::string label) {
	board_.WatchPc(pc, std::move(label), /*verbose=*/false);
}

void TwinBoard::ClearPcWatches() { board_.ClearPcWatches(); }

uint32_t TwinBoard::RunToVSync() {
	constexpr uint64_t kMax = 20'000ull * 400;
	for (uint64_t i = 0; i < kMax; ++i) {
		if (board_.EmulateCycle()) {
			return board_.FrameCounter();
		}
	}
	return board_.FrameCounter();
}

AdvanceResult TwinBoard::RunToPc(uint16_t pc, uint64_t max_cycles) {
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return AdvanceResult::Timeout;
	const uint32_t frame0 = board_.FrameCounter();
	for (uint64_t i = 0; i < max_cycles; ++i) {
		if (c64->TheCPU->InstructionComplete() && c64->TheCPU->GetPC() == pc) {
			return AdvanceResult::Ok;
		}
		if (board_.EmulateCycle()) {
			return AdvanceResult::HitVSync;
		}
		if (board_.FrameCounter() != frame0) {
			return AdvanceResult::HitVSync;
		}
	}
	return AdvanceResult::Timeout;
}

AdvanceResult TwinBoard::Run(uint64_t cycles) {
	if (cycles == 0) return AdvanceResult::Ok;
	for (uint64_t i = 0; i < cycles; ++i) {
		if (board_.EmulateCycle()) {
			return AdvanceResult::HitVSync;
		}
	}
	return AdvanceResult::Ok;
}

AdvanceResult TwinBoard::RunToIrq(uint64_t max_cycles) {
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return AdvanceResult::Timeout;
	const uint32_t frame0 = board_.FrameCounter();
	for (uint64_t i = 0; i < max_cycles; ++i) {
		// About to accept IRQ: pending and I clear. Park before the CPU
		// consumes it so the plugin can observe pre-handler state.
		if (c64->TheCPU->IrqPending() && !c64->TheCPU->IFlag()) {
			return AdvanceResult::HitIrq;
		}
		if (board_.EmulateCycle()) {
			return AdvanceResult::HitVSync;
		}
		if (board_.FrameCounter() != frame0) {
			return AdvanceResult::HitVSync;
		}
	}
	return AdvanceResult::Timeout;
}

AdvanceResult TwinBoard::RecordHwAcceptsUntilVSync(HwAcceptCalendar & out,
                                                   uint64_t max_cycles) {
	out.irq_at.clear();
	out.nmi_at.clear();
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return AdvanceResult::Timeout;
	const uint32_t frame0 = board_.FrameCounter();

	// Frodo keeps nmi_pending/irq_pending set through the 7-cycle steal
	// (cleared at 0x0014 / 0x000c). Record the fetch-cycle accept once, then
	// drain until pending clears so one Twin accept is one calendar slot.
	auto consume_pending = [&](bool nmi) -> AdvanceResult {
		for (uint64_t j = 0; j < max_cycles; ++j) {
			if (board_.EmulateCycle())
				return AdvanceResult::HitVSync;
			if (board_.FrameCounter() != frame0)
				return AdvanceResult::HitVSync;
			if (nmi ? !c64->TheCPU->NmiPending() : !c64->TheCPU->IrqPending())
				return AdvanceResult::Ok;
		}
		return AdvanceResult::Timeout;
	};

	for (uint64_t i = 0; i < max_cycles; ++i) {
		const bool at_fetch =
			c64->TheCPU->InstructionComplete() && !c64->TheCPU->BALow;
		if (at_fetch && c64->TheCPU->NmiPending()) {
			out.nmi_at.push_back(board_.CycleCounter());
			const AdvanceResult r = consume_pending(true);
			if (r != AdvanceResult::Ok)
				return r;
			continue;
		}
		if (at_fetch && c64->TheCPU->IrqPending() && !c64->TheCPU->IFlag()) {
			out.irq_at.push_back(board_.CycleCounter());
			const AdvanceResult r = consume_pending(false);
			if (r != AdvanceResult::Ok)
				return r;
			continue;
		}
		if (board_.EmulateCycle())
			return AdvanceResult::HitVSync;
		if (board_.FrameCounter() != frame0)
			return AdvanceResult::HitVSync;
	}
	return AdvanceResult::Timeout;
}

uint32_t TwinBoard::CycleCounter() const { return board_.CycleCounter(); }
uint32_t TwinBoard::FrameCounter() const { return board_.FrameCounter(); }

uint16_t TwinBoard::Pc() const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return 0;
	return c64->TheCPU->GetPC();
}

uint8_t TwinBoard::Sp() const {
	return GetCpuState().sp;
}

CpuState TwinBoard::GetCpuState() const {
	CpuState out{};
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return out;
	MOS6510State s;
	c64->TheCPU->GetState(&s);
	out.a = s.a;
	out.x = s.x;
	out.y = s.y;
	out.p = s.p;
	out.pc = s.pc;
	out.sp = uint8_t(s.sp);
	out.irq_pending = s.irq_pending;
	out.nmi_pending = s.nmi_pending;
	return out;
}

bool TwinBoard::AtOpcodeFetch() const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU)
		return false;
	return c64->TheCPU->InstructionComplete() && !c64->TheCPU->BALow;
}

bool TwinBoard::AtPc(uint16_t pc) const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU)
		return false;
	return c64->TheCPU->InstructionComplete() && c64->TheCPU->GetPC() == pc;
}

bool TwinBoard::FetchWouldTakeHw() const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU)
		return false;
	if (!c64->TheCPU->InstructionComplete() || c64->TheCPU->BALow)
		return false;
	// O_FETCH: nmi_pending → O_NMI, else irq_pending → O_IRQ (no I re-check).
	return c64->TheCPU->NmiPending() || c64->TheCPU->IrqPending();
}

bool TwinBoard::InIrqSequence() const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU)
		return false;
	return c64->TheCPU->CurrentState() == 0x09; // Frodo O_IRQ
}

bool TwinBoard::InNmiSequence() const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU)
		return false;
	return c64->TheCPU->CurrentState() == 0x11; // Frodo O_NMI
}

uint8_t TwinBoard::Peek(uint16_t addr) const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return 0;
	return c64->TheCPU->REUReadByte(addr);
}

bool TwinBoard::LastIoRead(IoAccess & out) const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return false;
	return c64->TheCPU->LastIoRead(out.addr, out.value, out.cycle);
}

bool TwinBoard::LastIoWrite(IoAccess & out) const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return false;
	return c64->TheCPU->LastIoWrite(out.addr, out.value, out.cycle);
}

bool TwinBoard::Capture(FullSnapshot & out) const {
	return CaptureFullSnapshot(board_, out);
}

bool TwinBoard::CaptureChips(ChipSnapshot & out) const {
	return CaptureChipSnapshot(board_, out);
}

bool TwinBoard::CaptureScreen(ScreenSnapshot & out) const {
	return CaptureScreenSnapshot(board_, out);
}

} // namespace revm
