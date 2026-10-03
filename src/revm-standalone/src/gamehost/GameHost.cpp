// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "gamehost/GameHost.hpp"

#include "goldens/PlayInput.hpp"
#include "input/InputState.hpp"
#define REVM_LOG_MODULE "gamehost"
#include "util/Log.hpp"

#include "C64.h"
#include "CIA.h"
#include "CPUC64.h"
#include "SID.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace gamehost {

// Unwinds the translated entry so the runner can finish a normal shutdown.
struct SoftQuitException {
	int code = 1;
};

namespace {

constexpr int kFrameTimeUs = 1000000 / 50; // PAL

void apply_cia_live_phase(MOS6526 * cia, const CiaLivePhase & live) {
	if (!cia)
		return;
	MOS6526State s{};
	cia->GetState(&s);
	s.ta_lo = uint8_t(live.ta.counter);
	s.ta_hi = uint8_t(live.ta.counter >> 8);
	s.tb_lo = uint8_t(live.tb.counter);
	s.tb_hi = uint8_t(live.tb.counter >> 8);
	s.ta_latch = live.ta.latch;
	s.tb_latch = live.tb.latch;
	s.ta_count_delay = live.ta.count_delay;
	s.tb_count_delay = live.tb.count_delay;
	s.ta_load_delay = live.ta.load_delay;
	s.tb_load_delay = live.tb.load_delay;
	s.ta_oneshot_delay = live.ta.oneshot_delay;
	s.tb_oneshot_delay = live.tb.oneshot_delay;
	if (live.ta.idle)
		s.cra = uint8_t(s.cra & ~uint8_t(0x01));
	else
		s.cra = uint8_t(s.cra | uint8_t(0x01));
	if (live.tb.idle)
		s.crb = uint8_t(s.crb & ~uint8_t(0x01));
	else
		s.crb = uint8_t(s.crb | uint8_t(0x01));
	cia->SetState(&s);
	cia->SetTimerIdle(live.ta.idle, live.tb.idle);
}

void apply_declared_cia_live(C64 * c64, const CpuMockStart & start) {
	if (!c64)
		return;
	if (start.cia1) {
		apply_cia_live_phase(c64->TheCIA1, *start.cia1);
		if (c64->TheCIA1)
			c64->TheCIA1->SyncLightpenEdge();
	}
	if (start.cia2)
		apply_cia_live_phase(c64->TheCIA2, *start.cia2);
}

} // namespace

void GameHost::MockIrqThunk(void * userdata) {
	auto * self = static_cast<GameHost *>(userdata);
	if (self->irq_depth_ > 0) {
		return;
	}

	MOS6510 * hw = self->board_.Machine() ? self->board_.Machine()->TheCPU
	                                      : nullptr;
	if (!hw) return;

	hw->SetCpuMockIFlag(true);
	hw->SetCpuMockIrqAckAt(0, nullptr);

	self->irq_finish_called_ = false;

	++self->irq_depth_;
	self->dispatch_irq();
	--self->irq_depth_;

	if (!self->irq_finish_called_) {
		hw->SetCpuMockIFlag(false);
		hw->SetCpuMockPostIrqHold(8, false);
	}
}

void GameHost::FinishMockIrq(uint32_t body_cycles) {
	MOS6510 * hw = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	if (!hw) return;

	irq_finish_called_ = true;

	hw->SetCpuMockIFlag(false);
	constexpr uint32_t kIrClear = 8;
	hw->SetCpuMockIrqAckAt(0, nullptr);
	hw->SetCpuMockPostIrqHold(body_cycles + kIrClear, false);
}

void GameHost::FinishMockNmi(uint32_t cycles) {
	if (cycles == 0)
		return;
	nmi_finish_called_ = true;
	nmi_hold_cycles_ = cycles;
}

void GameHost::AddMockIrqHold(uint32_t cycles) {
	MOS6510 * hw = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	if (!hw || cycles == 0 || !hw->CpuMockIrqHoldActive())
		return;
	hw->AddCpuMockIrqHold(cycles);
}

bool GameHost::IrqHoldActive() const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU)
		return false;
	return c64->TheCPU->CpuMockIrqHoldActive();
}

void GameHost::MockNmiThunk(void * userdata) {
	auto * self = static_cast<GameHost *>(userdata);
	if (self->nmi_depth_ > 0)
		return;

	MOS6510 * hw = self->board_.Machine() ? self->board_.Machine()->TheCPU
	                                      : nullptr;
	if (!hw) return;

	const bool i_before = hw->CpuMockIFlag();
	hw->SetCpuMockIFlag(true);

	self->nmi_finish_called_ = false;
	self->nmi_hold_cycles_ = 0;

	++self->nmi_depth_;
	self->dispatch_nmi();
	--self->nmi_depth_;

	if (self->nmi_finish_called_ && self->nmi_hold_cycles_ > 0) {
		if (self->irq_depth_ > 0 && self->IrqHoldActive())
			self->AddMockIrqHold(self->nmi_hold_cycles_);
		else
			hw->SetCpuMockPostIrqHold(self->nmi_hold_cycles_, i_before);
		return;
	}
	hw->SetCpuMockIFlag(i_before);
}

void GameHost::dispatch_irq() {
	if (!irq_handler_) {
		static bool warned = false;
		if (!warned) {
			REVM_LOG(REVM_ERROR, "IRQ with no C++ handler (SetIrqHandler)");
			warned = true;
		}
		return;
	}
	irq_handler_();
}

void GameHost::dispatch_nmi() {
	if (!nmi_handler_) {
		static bool warned = false;
		if (!warned) {
			REVM_LOG(REVM_ERROR, "NMI with no C++ handler (SetNmiHandler)");
			warned = true;
		}
		return;
	}
	nmi_handler_();
}

void GameHost::SetIrqHandler(IrqHandler handler) {
	irq_handler_ = std::move(handler);
}

void GameHost::SetNmiHandler(NmiHandler handler) {
	nmi_handler_ = std::move(handler);
}

void GameHost::RunIrqHandler() {
	ThrowIfQuitRequested();
	MOS6510 * hw = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	if (hw) hw->CpuMockClearIrqPending();
	MockIrqThunk(this);
}

void GameHost::RunNmiHandler() {
	ThrowIfQuitRequested();
	MOS6510 * hw = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	if (hw) hw->CpuMockClearNmiPending();
	MockNmiThunk(this);
}

uint8_t GameHost::Read(uint16_t addr) {
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return 0;
	return c64->TheCPU->CpuMockReadByte(addr);
}

void GameHost::Write(uint16_t addr, uint8_t value) {
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return;
	c64->TheCPU->CpuMockWriteByte(addr, value);
}

void GameHost::InstallAsset(uint16_t addr, std::span<const uint8_t> bytes) {
	C64 * c64 = board_.Machine();
	if (!c64) return;
	for (std::size_t i = 0; i < bytes.size(); ++i)
		c64->RAM[uint16_t(addr + i)] = bytes[i];
}

void GameHost::IrqDisable() {
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return;
	if (c64->TheCPU->CpuMockIrqHoldActive())
		return;
	c64->TheCPU->SetCpuMockIFlag(true);
}

void GameHost::IrqEnable() {
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return;
	if (c64->TheCPU->CpuMockIrqHoldActive())
		return;
	c64->TheCPU->SetCpuMockIFlag(false);
}

void GameHost::enable_mock_cpu() {
	MOS6510 * cpu = board_.Machine()->TheCPU;
	cpu->SetCpuMock(true, nullptr, this, nullptr);
	cpu->SetCpuMockIFlag(true);
	REVM_LOG(REVM_DEBUG, "mock 6510 ON (no opcode fetch; chips drive "
		"irq_pending/nmi_pending; host dispatches RunIrqHandler/RunNmiHandler)");
}

bool GameHost::init_board(const revm::Config & cfg) {
	std::string error;
	board_.Configure(cfg);
	if (!board_.Init(error)) {
		REVM_LOG(REVM_ERROR, "Init failed: %s", error.c_str());
		return false;
	}
	return true;
}

void GameHost::pace_after_vsync() {
	if (!pace_) return;
	using steady = std::chrono::steady_clock;
	frame_start_ += std::chrono::microseconds(kFrameTimeUs);
	const auto now = steady::now();
	if (frame_start_ > now) {
		std::this_thread::sleep_until(frame_start_);
	} else if (now - frame_start_ > std::chrono::milliseconds(100)) {
		frame_start_ = now;
	}
}

GameHost::AdvanceResult GameHost::AdvanceCycles(
	uint64_t cycles, VSyncPolicy vsync_policy) {
	if (cycles == 0)
		return AdvanceResult::Ok;

	uint64_t left = cycles;
	int no_progress = 0;
	while (left > 0) {
		const uint32_t c0 = board_.CycleCounter();
		const bool cross_vsync = vsync_policy == VSyncPolicy::Cross;
		const AdvanceResult r = main_advance(left, cross_vsync);
		if (r == AdvanceResult::HitNmi || r == AdvanceResult::HitIrq) {
			if (board_.CycleCounter() == c0) {
				if (++no_progress > 128) {
					Fail(1,
					     "Main dispatch: too many HitIrq/HitNmi retries (%d)",
					     no_progress);
					return AdvanceResult::Error;
				}
			} else {
				no_progress = 0;
			}
			if (r == AdvanceResult::HitNmi)
				RunNmiHandler();
			else
				RunIrqHandler();
			const uint32_t ran = board_.CycleCounter() - c0;
			if (uint64_t(ran) >= left)
				left = 0;
			else
				left -= ran;
			continue;
		}
		return r;
	}
	return AdvanceResult::Ok;
}

GameHost::AdvanceResult GameHost::AdvanceToVSync() {
	int retries = 0;
	for (;;) {
		const AdvanceResult r = main_advance_to_vsync();
		if (r != AdvanceResult::HitNmi && r != AdvanceResult::HitIrq)
			return r;
		if (++retries > 128) {
			Fail(1, "Main dispatch: too many HitIrq/HitNmi retries (%d)",
			     retries);
			return AdvanceResult::Error;
		}
		if (r == AdvanceResult::HitNmi)
			RunNmiHandler();
		else
			RunIrqHandler();
	}
}

uint32_t GameHost::AdvanceFrames(uint32_t frames) {
	for (uint32_t i = 0; i < frames; ++i) {
		const AdvanceResult r = AdvanceToVSync();
		if (r != AdvanceResult::HitVSync)
			Fail(1, "AdvanceFrames VSYNC miss n=%u of %u result=%s",
			     i, frames, revm::AdvanceResultName(r));
	}
	return frames;
}

void GameHost::ReturnIrq(uint32_t body_cycles) {
	if (!InIrq())
		Fail(1, "ReturnIrq requires InIrq");
	FinishMockIrq(body_cycles);
}

void GameHost::ReturnNmi(uint32_t cycles) {
	if (!InNmi())
		Fail(1, "ReturnNmi requires InNmi");
	FinishMockNmi(cycles);
}

GameHost::AdvanceResult GameHost::main_advance(uint64_t cycles,
                                               bool run_past_vsync) {
	if (cycles == 0) return AdvanceResult::Ok;
	C64 * c64 = board_.Machine();
	if (!c64) return AdvanceResult::Stopped;
	ThrowIfQuitRequested();
	for (uint64_t i = 0; i < cycles; ++i) {
		if (c64->QuitRequested()) {
			throw SoftQuitException{board_.ExitCode()};
		}
		MOS6510 * hw = c64->TheCPU;
		if (hw->CpuMockNmiPending() && nmi_depth_ == 0)
			return AdvanceResult::HitNmi;
		if (hw->CpuMockIrqPending() && !hw->CpuMockIFlag() &&
		    irq_depth_ == 0 && !IrqHoldActive())
			return AdvanceResult::HitIrq;
		const bool vb = board_.EmulateCycle();
		if (vb) {
			pace_after_vsync();
			if (c64->QuitRequested()) {
				throw SoftQuitException{board_.ExitCode()};
			}
			if (!run_past_vsync) {
				return AdvanceResult::HitVSync;
			}
			continue;
		}
	}
	return AdvanceResult::Ok;
}

GameHost::AdvanceResult GameHost::main_advance_to_vsync() {
	C64 * c64 = board_.Machine();
	if (!c64) return AdvanceResult::Stopped;
	ThrowIfQuitRequested();

	constexpr uint64_t kMax = 20'000ull * 400;
	for (uint64_t i = 0; i < kMax; ++i) {
		if (ShouldQuit()) {
			throw SoftQuitException{board_.ExitCode()};
		}
		if (board_.EmulateCycle()) {
			pace_after_vsync();
			ThrowIfQuitRequested();
			return AdvanceResult::HitVSync;
		}
		MOS6510 * hw = c64->TheCPU;
		if (hw->CpuMockNmiPending() && nmi_depth_ == 0)
			return AdvanceResult::HitNmi;
		if (hw->CpuMockIrqPending() && !hw->CpuMockIFlag() &&
		    irq_depth_ == 0 && !IrqHoldActive())
			return AdvanceResult::HitIrq;
	}
	REVM_LOG(REVM_ERROR, "AdvanceToVSync: timeout without VSYNC");
	return AdvanceResult::Timeout;
}

void GameHost::Fail(int code, const char * fmt, ...) {
	char msg[1536];
	std::va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(msg, sizeof msg, fmt ? fmt : "", ap);
	va_end(ap);
	fail_message(code, msg);
}

void GameHost::fail_message(int code, const char * message) {
	REVM_LOG(REVM_ERROR, "frame %u: %s", board_.FrameCounter(),
	         message ? message : "");
	board_.RequestQuit(code);
	throw SoftQuitException{code};
}

void GameHost::ThrowIfQuitRequested() {
	if (ShouldQuit())
		throw SoftQuitException{board_.ExitCode()};
}

bool GameHost::ShouldQuit() const {
	const C64 * c64 = board_.Machine();
	return c64 && c64->QuitRequested();
}

void GameHost::AssertEntry(uint16_t pc) {
	if (!main_start_) {
		Fail(1, "blank Main requires InstallMainStart");
	} else if (main_start_->entry_pc != pc) {
		Fail(1, "Main start PC mismatch: installed=$%04X asserted=$%04X",
		     main_start_->entry_pc, pc);
	}
}

bool GameHost::apply_blank_start(revm::PlayInput * playback) {
	if (!main_start_) {
		REVM_LOG(REVM_ERROR,
		         "blank Main requires InstallMainStart(...) in gamehost::InstallGame");
		return false;
	}
	const CpuMockStart & start = *main_start_;

	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) {
		REVM_LOG(REVM_ERROR, "blank Main machine is not initialized");
		return false;
	}

	board_.PrepareRun();
	std::memset(c64->RAM, 0, C64_RAM_SIZE);
	std::memset(c64->Color, 0, COLOR_RAM_SIZE);
	enable_mock_cpu();
	const uint32_t phase_cycles = start.cycle % revm::kCyclesPerFrame;
	for (uint32_t i = 0; i < phase_cycles; ++i)
		(void)c64->EmulateCycle();

	MOS6510State cpu{};
	c64->TheCPU->GetState(&cpu);
	cpu.pc = start.entry_pc;
	cpu.ddr = start.cpu_port_ddr;
	cpu.pr = start.cpu_port;
	cpu.pr_out = start.cpu_port;
	if (start.interrupts_disabled)
		cpu.p |= 0x04;
	else
		cpu.p &= uint8_t(~0x04u);
	c64->TheCPU->SetState(&cpu);
	c64->SetCounters(start.cycle, start.frame);
	// BEGIN relocates the clock without simulating the omitted loader time.
	c64->TheSID->RebaseRendererClock();
	// Match CpuMockHost::start_blank_main: leave SID as the phase-walked chip.
	// Standalone Board has no shadow Clock; C64 counters are the timeline.
	apply_declared_cia_live(c64, start);

	if (playback) {
		playback->SeekTo(start.frame);
		board_.ApplyInput(playback->PollFrame(start.frame));
	} else {
		board_.ApplyInput(revm::InputFrame{});
	}

	REVM_LOG(REVM_DEBUG,
	         "Main blank start — fresh chips, zero RAM/color, "
	         "pc=$%04X cycle=%u frame=%u phase=%u cia1_live=%d cia2_live=%d",
	         start.entry_pc, start.cycle, start.frame, phase_cycles,
	         start.cia1.has_value() ? 1 : 0, start.cia2.has_value() ? 1 : 0);
	return true;
}

int GameHost::run_entry() {
	if (!entry_handler_) {
		REVM_LOG(REVM_ERROR, "no EntryPoint (SetEntryHandler)");
		return 1;
	}
	board_.StartWallClockLimit();
	try {
		if (!ShouldQuit())
			entry_handler_();
	} catch (const SoftQuitException &) {
	}
	board_.StopWallClockLimit();
	{
		std::string screen_err;
		if (!board_.FinishSaveScreen(screen_err) && !screen_err.empty()) {
			REVM_LOG(REVM_ERROR, "%s", screen_err.c_str());
			if (board_.ExitCode() == 0)
				board_.RequestQuit(1);
		}
	}

	if (auto * cpu = board_.Machine() ? board_.Machine()->TheCPU : nullptr)
		cpu->SetCpuMock(false);
	return board_.ExitCode();
}

} // namespace gamehost
