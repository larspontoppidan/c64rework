// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "cpumock/CpuMockHost.hpp"
#include "cpumock/LinkedRegistry.hpp"

#include "cpumock/ScreenContext.hpp"
#include "goldens/CompareReport.hpp"
#include "goldens/PlayLog.hpp"
#include "goldens/PlayPlayer.hpp"
#include "goldens/PlayRecorder.hpp"
#include "input/GoldenInput.hpp"
#include "input/LiveInput.hpp"
#include "snapshot/ChipIo.hpp"
#include "snapshot/Snapshot.hpp"
#define REVM_LOG_MODULE "cpumock"
#include "util/Event.hpp"
#include "util/Log.hpp"

#include "C64.h"
#include "CIA.h"
#include "CPUC64.h"
#include "SID.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace revm::cpumock {

namespace {

constexpr int kFrameTimeUs = 1000000 / 50; // PAL

void fill_cia_config_detail(char * buf, size_t n, const char * which,
                            const MOS6526State & m, const MOS6526State & t,
                            uint8_t pra_mask) {
	buf[0] = 0;
	auto note8 = [&](const char * f, uint8_t mv, uint8_t tv) {
		if (mv == tv || buf[0]) return;
		std::snprintf(buf, n, "%s.%s main=$%02X twin=$%02X", which, f, mv, tv);
	};
	auto note16 = [&](const char * f, uint16_t mv, uint16_t tv) {
		if (mv == tv || buf[0]) return;
		std::snprintf(buf, n, "%s.%s main=$%04X twin=$%04X", which, f, mv, tv);
	};
	if (pra_mask)
		note8("pra", uint8_t(m.pra & pra_mask), uint8_t(t.pra & pra_mask));
	note8("ddra", m.ddra, t.ddra);
	note8("ddrb", m.ddrb, t.ddrb);
	note8("cra", m.cra, t.cra);
	note8("crb", m.crb, t.crb);
	note16("ta_latch", m.ta_latch, t.ta_latch);
	note16("tb_latch", m.tb_latch, t.tb_latch);
	note8("int_mask", m.int_mask, t.int_mask);
}

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

// Programmed CIA setup — not live counters / SC delay pipelines / IFR / TOD clock.
bool cia_config_equal(const MOS6526State & a, const MOS6526State & b) {
	return a.ddra == b.ddra && a.ddrb == b.ddrb &&
	       a.cra == b.cra && a.crb == b.crb &&
	       a.ta_latch == b.ta_latch && a.tb_latch == b.tb_latch &&
	       a.int_mask == b.int_mask && a.sdr == b.sdr &&
	       a.alm_10ths == b.alm_10ths && a.alm_sec == b.alm_sec &&
	       a.alm_min == b.alm_min && a.alm_hr == b.alm_hr;
}

void print_cia_config_diff(FILE * out, const char * label,
                           const MOS6526State & twin, const MOS6526State & main) {
	auto u8 = [&](const char * name, uint8_t t, uint8_t m) {
		if (t != m)
			std::fprintf(out, "    %s: twin=$%02X main=$%02X\n", name, t, m);
	};
	auto u16 = [&](const char * name, uint16_t t, uint16_t m) {
		if (t != m)
			std::fprintf(out, "    %s: twin=$%04X main=$%04X\n", name, t, m);
	};
	std::fprintf(out, "  %s config diffs:\n", label);
	u8("ddra", twin.ddra, main.ddra);
	u8("ddrb", twin.ddrb, main.ddrb);
	u8("cra", twin.cra, main.cra);
	u8("crb", twin.crb, main.crb);
	u16("ta_latch", twin.ta_latch, main.ta_latch);
	u16("tb_latch", twin.tb_latch, main.tb_latch);
	u8("int_mask", twin.int_mask, main.int_mask);
	u8("sdr", twin.sdr, main.sdr);
	u8("alm_10ths", twin.alm_10ths, main.alm_10ths);
	u8("alm_sec", twin.alm_sec, main.alm_sec);
	u8("alm_min", twin.alm_min, main.alm_min);
	u8("alm_hr", twin.alm_hr, main.alm_hr);
}

std::string screen_diff_report(const ScreenSnapshot & expected,
                               const ScreenSnapshot & actual) {
	char * buf = nullptr;
	size_t buf_sz = 0;
	FILE * rf = open_memstream(&buf, &buf_sz);
	if (!rf) return {};
	PrintScreenDiff(rf, expected, actual);
	std::fclose(rf);
	std::string report;
	if (buf) {
		report.assign(buf, buf_sz);
		std::free(buf);
	}
	return report;
}

bool twin_op_is_control_flow(uint8_t op) {
	switch (op) {
	case 0x00: // BRK
	case 0x20: // JSR
	case 0x40: // RTI
	case 0x4C: // JMP abs
	case 0x60: // RTS
	case 0x6C: // JMP ind
		return true;
	default:
		return (op & 0x1F) == 0x10; // Bxx
	}
}

uint8_t twin_current_op(TwinBoard * twin) {
	C64 * c64 = twin->board().Machine();
	if (!c64 || !c64->TheCPU)
		return 0;
	return c64->TheCPU->CurrentOp();
}

// Cap on per-compare diverging-byte records in the event stream; further
// diffs collapse into one overflow marker.
constexpr size_t kEventFailCap = 32;

void emit_compare_fail(const char * channel, uint32_t addr, uint8_t main_v,
                       uint8_t twin_v) {
	REVM_EVENT("compare_fail", "channel", channel, "addr",
	           revm::event::Hex{addr}, "main", main_v, "twin", twin_v);
}

void emit_overflow_marker(const char * channel) {
	REVM_EVENT("compare_fail", "channel", channel, "overflow", 1);
}

// --- Screen-fail context oracle (failure-only; zero cost otherwise). ---
// Feeds each board's own frozen VIC/CIAs + RAM/color views into the pure
// ScreenContext decoder; see cpumock/ScreenContext.hpp.

uint8_t screen_ctx_fetch(const void * user, uint16_t addr) {
	const C64 * c64 = static_cast<const C64 *>(user);
	if (!c64) return 0;
	if (addr >= 0xD800 && addr < 0xDBE8)
		return c64->Color[addr - 0xD800]; // shared color-RAM view
	if (addr >= 0xD000 && addr < 0xE000)
		return 0; // other I/O not modeled by the decoder
	return c64->RAM[addr];
}

struct ScreenCtxDiffMem {
	const ScreenSnapshot * main_snap;
	const ScreenSnapshot * twin_snap;
};

bool screen_ctx_diff(const void * user, unsigned x, unsigned y,
                     uint8_t & out_main, uint8_t & out_twin) {
	const auto * d = static_cast<const ScreenCtxDiffMem *>(user);
	if (!d || !d->main_snap || !d->twin_snap) return false;
	const size_t i = size_t(y) * ScreenSnapshot::kWidth + x;
	out_main = d->main_snap->pixels[i];
	out_twin = d->twin_snap->pixels[i];
	return out_main != out_twin;
}

screenctx::Side screen_ctx_side(const ChipSnapshot & chip) {
	static constexpr uint8_t MOS6569State::* kMxReg[8] = {
		&MOS6569State::m0x, &MOS6569State::m1x, &MOS6569State::m2x,
		&MOS6569State::m3x, &MOS6569State::m4x, &MOS6569State::m5x,
		&MOS6569State::m6x, &MOS6569State::m7x};
	static constexpr uint8_t MOS6569State::* kMyReg[8] = {
		&MOS6569State::m0y, &MOS6569State::m1y, &MOS6569State::m2y,
		&MOS6569State::m3y, &MOS6569State::m4y, &MOS6569State::m5y,
		&MOS6569State::m6y, &MOS6569State::m7y};
	const MOS6569State & v = chip.vic;
	screenctx::Side s;
	s.d011 = v.ctrl1;
	s.d016 = v.ctrl2;
	s.d018 = v.vbase;
	s.dd00 = chip.cia2.pra;
	s.d015 = v.me;
	s.d017 = v.mye;
	s.d01c = v.mmc;
	s.d01d = v.mxe;
	s.d020 = v.ec;  // $D020 border color
	s.d021 = v.b0c; // $D021 background color 0
	for (unsigned n = 0; n < 8; ++n) {
		s.mx[n] = uint16_t(v.*kMxReg[n] |
		                   ((v.mx8 >> n & 1u) << 8)); // 9-bit X incl MSB
		s.my[n] = v.*kMyReg[n];
	}
	return s;
}

// SID config registers compared by SidConfigEqual, as {offset, member}.
struct SidField {
	uint16_t addr;
	uint8_t MOS6581State::* ptr;
};

const SidField kSidFields[] = {
	{0x00, &MOS6581State::freq_lo_1}, {0x01, &MOS6581State::freq_hi_1},
	{0x02, &MOS6581State::pw_lo_1},   {0x03, &MOS6581State::pw_hi_1},
	{0x04, &MOS6581State::ctrl_1},    {0x05, &MOS6581State::AD_1},
	{0x06, &MOS6581State::SR_1},
	{0x07, &MOS6581State::freq_lo_2}, {0x08, &MOS6581State::freq_hi_2},
	{0x09, &MOS6581State::pw_lo_2},   {0x0A, &MOS6581State::pw_hi_2},
	{0x0B, &MOS6581State::ctrl_2},    {0x0C, &MOS6581State::AD_2},
	{0x0D, &MOS6581State::SR_2},
	{0x0E, &MOS6581State::freq_lo_3}, {0x0F, &MOS6581State::freq_hi_3},
	{0x10, &MOS6581State::pw_lo_3},   {0x11, &MOS6581State::pw_hi_3},
	{0x12, &MOS6581State::ctrl_3},    {0x13, &MOS6581State::AD_3},
	{0x14, &MOS6581State::SR_3},
	{0x15, &MOS6581State::fc_lo},     {0x16, &MOS6581State::fc_hi},
	{0x17, &MOS6581State::res_filt},  {0x18, &MOS6581State::mode_vol},
	{0x19, &MOS6581State::pot_x},     {0x1A, &MOS6581State::pot_y},
};

// VicConfigEqual fields as {$D0xx offset, member, compare mask}.
// Live raster, RST8, irq_flag, collisions, and lightpen are omitted.
struct VicField {
	uint16_t addr;
	uint8_t MOS6569State::* ptr;
	uint8_t mask = 0xff;
};

const VicField kVicConfigFields[] = {
	{0x00, &MOS6569State::m0x}, {0x01, &MOS6569State::m0y},
	{0x02, &MOS6569State::m1x}, {0x03, &MOS6569State::m1y},
	{0x04, &MOS6569State::m2x}, {0x05, &MOS6569State::m2y},
	{0x06, &MOS6569State::m3x}, {0x07, &MOS6569State::m3y},
	{0x08, &MOS6569State::m4x}, {0x09, &MOS6569State::m4y},
	{0x0A, &MOS6569State::m5x}, {0x0B, &MOS6569State::m5y},
	{0x0C, &MOS6569State::m6x}, {0x0D, &MOS6569State::m6y},
	{0x0E, &MOS6569State::m7x}, {0x0F, &MOS6569State::m7y},
	{0x10, &MOS6569State::mx8},
	{0x11, &MOS6569State::ctrl1, 0x7f},
	{0x15, &MOS6569State::me},   {0x16, &MOS6569State::ctrl2},
	{0x17, &MOS6569State::mye},  {0x18, &MOS6569State::vbase},
	{0x1A, &MOS6569State::irq_mask},
	{0x1B, &MOS6569State::mdp},  {0x1C, &MOS6569State::mmc},
	{0x1D, &MOS6569State::mxe},
	{0x20, &MOS6569State::ec},   {0x21, &MOS6569State::b0c},
	{0x22, &MOS6569State::b1c},  {0x23, &MOS6569State::b2c},
	{0x24, &MOS6569State::b3c},  {0x25, &MOS6569State::mm0},
	{0x26, &MOS6569State::mm1},  {0x27, &MOS6569State::m0c},
	{0x28, &MOS6569State::m1c},  {0x29, &MOS6569State::m2c},
	{0x2A, &MOS6569State::m3c},  {0x2B, &MOS6569State::m4c},
	{0x2C, &MOS6569State::m5c},  {0x2D, &MOS6569State::m6c},
	{0x2E, &MOS6569State::m7c},
};

void note_vic_config_fails(const MOS6569State & m, const MOS6569State & t,
                           char * detail, size_t detail_n) {
	detail[0] = 0;
	unsigned shown = 0;
	auto note8 = [&](uint16_t addr, uint8_t mv, uint8_t tv) {
		if (mv == tv) return;
		if (detail[0] == 0)
			std::snprintf(detail, detail_n, "$D0%02X main=$%02X twin=$%02X",
			              addr, mv, tv);
		if (shown < kEventFailCap)
			emit_compare_fail("vic", 0xD000u + addr, mv, tv);
		++shown;
	};
	for (const VicField & f : kVicConfigFields) {
		note8(f.addr, uint8_t(m.*f.ptr & f.mask), uint8_t(t.*f.ptr & f.mask));
	}
	if (m.irq_raster != t.irq_raster) {
		if (detail[0] == 0)
			std::snprintf(detail, detail_n, "irq_raster main=$%03X twin=$%03X",
			              unsigned(m.irq_raster), unsigned(t.irq_raster));
		++shown;
	}
	if (shown > kEventFailCap) emit_overflow_marker("vic");
}

void note_cia_config_fails(const char * channel, uint16_t base,
                           const MOS6526State & m, const MOS6526State & t,
                           uint8_t pra_mask) {
	unsigned shown = 0;
	auto note8 = [&](uint16_t off, uint8_t mv, uint8_t tv) {
		if (mv == tv) return;
		if (shown < kEventFailCap)
			emit_compare_fail(channel, uint32_t(base) + off, mv, tv);
		++shown;
	};
	auto note16 = [&](uint16_t off, uint16_t mv, uint16_t tv) {
		if (mv == tv) return;
		note8(off, uint8_t(mv), uint8_t(tv));
		note8(uint16_t(off + 1), uint8_t(mv >> 8), uint8_t(tv >> 8));
	};
	if (pra_mask)
		note8(0x00, uint8_t(m.pra & pra_mask), uint8_t(t.pra & pra_mask));
	note8(0x02, m.ddra, t.ddra);
	note8(0x03, m.ddrb, t.ddrb);
	note16(0x04, m.ta_latch, t.ta_latch);
	note16(0x06, m.tb_latch, t.tb_latch);
	note8(0x0D, m.int_mask, t.int_mask);
	note8(0x0E, m.cra, t.cra);
	note8(0x0F, m.crb, t.crb);
	if (shown > kEventFailCap) emit_overflow_marker(channel);
}

} // namespace

void CpuMockHost::MockIrqThunk(void * userdata) {
	auto * self = static_cast<CpuMockHost *>(userdata);
	// Thunk: run the plugin C++ IRQ body. Twin's 6510 ISR is separate.
	if (self->irq_depth_ > 0) {
		// Already in a C++ IRQ; keep I set so EmulateCycle won't re-enter.
		return;
	}

	MOS6510 * hw = self->board_.Machine() ? self->board_.Machine()->TheCPU
	                                      : nullptr;
	if (!hw) return;

	// Pure C++ IRQ: no 6502 stack frame. EmulateCycle sets I on accept
	// *before* this hook, so CpuMockIFlag() here is already true — that is
	// not the pre-IRQ I. IRQ is only accepted when I was clear, matching
	// the status a real 6510 would pull on RTI. Restore I=0 after the
	// handler; hold I for 8 Φ2 so CIA IR-clear delay elapses (0-cycle C++
	// IRQ otherwise double-fires). ReturnIrq may prepend accounted body Φ2.
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

void CpuMockHost::FinishMockIrq(uint32_t body_cycles) {
	MOS6510 * hw = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	if (!hw) return;

	irq_finish_called_ = true;

	// Same RTI contract as the default thunk: I=0 after the hold.
	// kIrClear is IR-clear / RTI-ish delay — not a Twin-body knob.
	hw->SetCpuMockIFlag(false);
	constexpr uint32_t kIrClear = 8;
	hw->SetCpuMockIrqAckAt(0, nullptr);
	hw->SetCpuMockPostIrqHold(body_cycles + kIrClear, false);
}

void CpuMockHost::FinishMockNmi(uint32_t cycles) {
	if (cycles == 0)
		return;
	nmi_finish_called_ = true;
	nmi_hold_cycles_ = cycles;
}

void CpuMockHost::AddMockIrqHold(uint32_t cycles) {
	MOS6510 * hw = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	if (!hw || cycles == 0 || !hw->CpuMockIrqHoldActive())
		return;
	hw->AddCpuMockIrqHold(cycles);
}

bool CpuMockHost::IrqHoldActive() const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU)
		return false;
	return c64->TheCPU->CpuMockIrqHoldActive();
}

void CpuMockHost::MockNmiThunk(void * userdata) {
	auto * self = static_cast<CpuMockHost *>(userdata);
	// Thunk: plugin C++ NMI body (not Twin executing $A0xx / the original ISR).
	if (self->nmi_depth_ > 0)
		return;

	MOS6510 * hw = self->board_.Machine() ? self->board_.Machine()->TheCPU
	                                      : nullptr;
	if (!hw) return;

	// NMI is not masked by I; still hold I during the handler (6502 does)
	// and restore afterward (RTI-equivalent — no stack frame on Main).
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

void CpuMockHost::dispatch_irq() {
	if (!irq_handler_) {
		static bool warned = false;
		if (!warned) {
			REVM_LOG(REVM_ERROR, "IRQ with no C++ handler (InstallIrqHandler)");
			warned = true;
		}
		return;
	}
	irq_handler_();
}

void CpuMockHost::dispatch_nmi() {
	if (!nmi_handler_) {
		static bool warned = false;
		if (!warned) {
			REVM_LOG(REVM_ERROR, "NMI with no C++ handler (InstallNmiHandler)");
			warned = true;
		}
		return;
	}
	nmi_handler_();
}

void CpuMockHost::InstallIrqHandler(IrqHandler handler) {
	irq_handler_ = std::move(handler);
}

void CpuMockHost::InstallNmiHandler(NmiHandler handler) {
	nmi_handler_ = std::move(handler);
}

void CpuMockHost::track_main_lines() {
	MOS6510 * cpu = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	if (!cpu)
		return;
	const bool irq = cpu->CpuMockIrqPending();
	if (irq && !main_irq_line_)
		main_irq_since_ = board_.CycleCounter();
	main_irq_line_ = irq;
	const bool nmi = cpu->CpuMockNmiPending();
	if (nmi && !main_nmi_line_)
		main_nmi_since_ = board_.CycleCounter();
	main_nmi_line_ = nmi;
}

// EnterInterrupt — the one door into a handler with Twin.
//
// Twin is parked at its accept boundary (the yielding walk stopped there).
// Burn Main forward to the steal cycle, then take the skew verdict:
// Main's line must have fired within [T-K_EARLY, T+K_LATE]. Early is
// tolerated generously (Main was only held back); late pair-steps both
// boards a bounded number of Φ2. A silent line or wrong-type divergence
// is a SoftQuit. On success Main's pending flag is consumed and the
// sequence binds to this dispatch.
void CpuMockHost::enter_interrupt(bool take_nmi) {
	step_ctx_ = "verdict";
	MOS6510 * hw = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	const char * kind = take_nmi ? "NMI" : "IRQ";
	if (NoTwin())
		SoftQuit(1, "enter_interrupt %s requires Twin", kind);
	if (!hw)
		SoftQuit(1, "enter_interrupt %s missing machine", kind);
	if (!twin_at_hw_steal()) {
		SoftQuit(1,
		         "enter_interrupt %s without Twin steal twin_pc=$%04X "
		         "(walk protocol violation)",
		         kind, twin_->Pc());
	}
	// Resolve-then-classify: the verdict pairs against the ORIGINAL
	// boundary cycle (where detection parked Twin), not Twin's post-
	// resolution position a few Φ2 into the sequence.
	const uint32_t t_steal =
		resolving_steal_ != kNoSince ? resolving_steal_
		                             : twin_->CycleCounter();
	resolving_steal_ = kNoSince;
	const uint32_t m0 = board_.CycleCounter();
	// Twin accepted ⇒ the original had I=0 at this instant. Main's
	// translated Sei/Cli timing lags inside its chunks; adopt Twin's
	// regime at the accept so the verdict sees the line and Main's
	// masking stays synchronized.
	if (!take_nmi && hw->CpuMockIFlag()) {
		hw->SetCpuMockIFlag(false);
		track_main_lines();
	}
	track_main_lines();
	REVM_LOG(REVM_VERBOSE,
	         "enter_interrupt %s begin m0=%u steal=%u pend=%d edge=%s", kind,
	         m0, t_steal, take_nmi ? int(hw->CpuMockNmiPending())
	                               : int(hw->CpuMockIrqPending()),
	         [&] {
		         const uint32_t s =
			         take_nmi ? main_nmi_since_ : main_irq_since_;
		         return s == kNoSince ? "none"
		                              : std::to_string(s).c_str();
	         }());

	int32_t skew = 0; // >0: Main early by N Φ2; <0: Main late by N Φ2
	const bool pending =
		take_nmi ? hw->CpuMockNmiPending() : hw->CpuMockIrqPending();
	uint32_t & prev_steal = take_nmi ? prev_nmi_steal_ : prev_irq_steal_;
	uint32_t & prev_edge = take_nmi ? prev_nmi_edge_ : prev_irq_edge_;

	// Freshness: Main's edge must postdate the previously PAIRED steal —
	// an older edge means Main's chip schedule diverged (missed or stuck
	// line). Absolute earliness is NOT a failure: Twin's accept lags its
	// own line rise whenever its previous ISR runs longer than the period
	// (both sides coalesce identically).
	auto edge_fresh = [&](uint32_t edge) {
		return prev_steal == kNoSince || edge >= prev_steal;
	};

	bool paired = false;
	bool counted_silent = false; // verdict counted into stats.silent
	uint32_t since = kNoSince;
	SkewStats & stats = skew_stats_[take_nmi ? 1 : 0];
	if (pending) {
		since = take_nmi ? main_nmi_since_ : main_irq_since_;
		skew = since == kNoSince
			       ? 0
			       : int32_t(t_steal - since);
		if (since != kNoSince && !edge_fresh(since)) {
			SoftQuit(1,
			         "%s schedule FAIL: Main edge %u predates previous "
			         "steal %u (stale / diverged chip schedule frame=%u)",
			         kind, since, prev_steal, board_.FrameCounter());
		}
		silent_streak_[take_nmi ? 1 : 0] = 0;
		++stats.paired;
		if (!stats.any || skew < stats.min)
			stats.min = skew;
		if (!stats.any || skew > stats.max)
			stats.max = skew;
		stats.any = true;
		paired = true;
	} else if (m0 > t_steal) {
		// Late discovery: Main ran past the steal via a raw corner
		// (extra VSYNC / inject burn). Twin cannot be rewound and its
		// line already fired without Main's — that is divergence.
		SoftQuit(1,
		         "%s mismatch: Main ahead of steal main=%u steal=%u with "
		         "Main line silent",
		         kind, m0, t_steal);
	} else {
		if (m0 < t_steal &&
		    main_advance(uint64_t(t_steal - m0), /*run_past_vsync=*/true) !=
		        AdvanceResult::Ok) {
			SoftQuit(1, "enter_interrupt %s Main burn to steal failed", kind);
		}
		track_main_lines();
		since = take_nmi ? main_nmi_since_ : main_irq_since_;
		const bool now_pending =
			take_nmi ? hw->CpuMockNmiPending() : hw->CpuMockIrqPending();
		if (now_pending && since != kNoSince) {
			skew = int32_t(t_steal - since);
			silent_streak_[take_nmi ? 1 : 0] = 0;
			paired = true;
		} else {
			// Lateness probe: give Main's line a short grace window
			// (Main-only advance; Twin parked so handler fences stay on
			// the original timeline). Beyond it, pair anyway — Twin owns
			// WHEN, and skipping Main's thunk would guarantee state
			// divergence. Repeated silence IS divergence: the streak
			// counter stops the run after kSilentStreak misses.
			const uint32_t m0b = board_.CycleCounter();
			for (uint32_t i = 1; i <= kSkewLateGrace; ++i) {
				if (main_advance(1, true) != AdvanceResult::Ok)
					break;
				track_main_lines();
				since = take_nmi ? main_nmi_since_ : main_irq_since_;
				const bool now = take_nmi ? hw->CpuMockNmiPending()
			                                  : hw->CpuMockIrqPending();
				if (now && since != kNoSince) {
					skew = -int32_t(i);
					paired = true;
					break;
				}
			}
			if (!paired) {
				// A silent line while Main is still SEI'd is an
				// interrupt-regime TRANSITION: Twin's native CLI lands
				// mid-walk, Main's translated Cli() executes a few
				// cycles later in its chunk. Expected — not divergence.
				// Only silence with I=0 counts toward the streak.
				const bool main_sei =
					hw ? hw->CpuMockIFlag() : false;
				if (main_sei) {
					REVM_LOG(REVM_VERBOSE,
					         "%s skew: silent but Main SEI'd "
					         "(regime transition) steal=%u",
					         kind, t_steal);
					skew = 0;
					paired = true; // dispatch; parity via thunk
					since = kNoSince;
				}
			}
			if (!paired) {
				++silent_streak_[take_nmi ? 1 : 0];
				++stats.silent;
				counted_silent = true;
				REVM_EVENT("accept", "phase", "silent", "kind",
				           take_nmi ? "nmi" : "irq", "boundary_cyc", t_steal);
				REVM_LOG(REVM_ERROR,
				         "%s skew: Main line silent at accept "
				         "(steal=%u main=%u streak=%u frame=%u)",
				         kind, t_steal, m0b,
				         silent_streak_[take_nmi ? 1 : 0],
				         board_.FrameCounter());
				if (silent_streak_[take_nmi ? 1 : 0] == 1)
					LogVsTwinIrqSources("silent");
				if (silent_streak_[take_nmi ? 1 : 0] >= kSilentStreak) {
					SoftQuit(1,
					         "%s schedule FAIL: Main line silent for %u "
					         "consecutive accepts (diverged)",
					         kind, silent_streak_[take_nmi ? 1 : 0]);
				}
				skew = 0;
				paired = true; // dispatch anyway; streak polices divergence
				since = kNoSince;
			} else {
				++stats.paired;
				if (!stats.any || skew < stats.min)
					stats.min = skew;
				if (!stats.any || skew > stats.max)
					stats.max = skew;
				stats.any = true;
				silent_streak_[take_nmi ? 1 : 0] = 0;
			}
			if (paired && skew < 0) {
				// Late edge: Main waited -skew Φ2 past the parked
				// boundary. Bring Twin level again — nothing may leave
				// Main ahead, or every later align/inject degrades by
				// that amount.
				for (int32_t k = 0; k < -skew; ++k)
					(void)twin_step_only();
			}
		}
		if (paired && since != kNoSince && !edge_fresh(since)) {
			SoftQuit(1,
			         "%s schedule FAIL: Main edge %u predates previous "
			         "steal %u (stale / diverged chip schedule frame=%u)",
			         kind, since, prev_steal, board_.FrameCounter());
		}
	}
	// NOTE: no automatic schedule repair here. Large single skews are
	// usually legitimate transitions (one side's re-init lands an NMI
	// before the other's); the skew telemetry + irq-sources dump surface
	// anything persistent for plugin-side fixes.
	prev_steal = t_steal;
	if (take_nmi)
		prev_nmi_edge_ = main_nmi_since_ != kNoSince ? main_nmi_since_
		                                             : t_steal;
	else
		prev_irq_edge_ = main_irq_since_ != kNoSince ? main_irq_since_
		                                             : t_steal;
	REVM_LOG(REVM_VERBOSE,
	         "%s paired skew=%+d steal=%u frame=%u", kind, skew, t_steal,
	         board_.FrameCounter());
	// Structured verdict record: one skew + one dispatched accept per
	// enter_interrupt. Silent accepts additionally emitted their `silent`
	// phase above.
	event::EmitSkew(take_nmi ? "nmi" : "irq", !counted_silent, skew);
	REVM_EVENT("accept", "phase", "dispatched", "kind",
	           take_nmi ? "nmi" : "irq", "boundary_cyc", t_steal);
	if (take_nmi) {
		hw->CpuMockClearNmiPending();
		main_nmi_since_ = kNoSince;
		nmi_seq_active_ = true;
	} else {
		hw->CpuMockClearIrqPending();
		main_irq_since_ = kNoSince;
		irq_seq_active_ = true;
	}
	// Resolve-then-classify left Twin a few Φ2 past the boundary (its
	// sequence ran to the vector decision while Main stayed parked for
	// the verdict above). Restore Φ2 AND chip lockstep: Main catches up
	// to Twin's post-resolution position, so handler walks cannot
	// straddle VBLANK half-aligned and both boards' chips tick the same
	// Φ2 again. Raw Main-only advance — Twin is mid-sequence here.
	if (twin_->CycleCounter() > board_.CycleCounter()) {
		(void)main_advance(
			uint64_t(twin_->CycleCounter() - board_.CycleCounter()),
			/*run_past_vsync=*/true);
	}
}

CpuMockHost::AdvanceResult CpuMockHost::lock_step_pair(uint64_t max_cycles,
                                                       bool stop_at_vsync) {
	step_ctx_ = "lsp";
	C64 * c64 = board_.Machine();
	if (!c64)
		return AdvanceResult::Stopped;
	ThrowIfQuitRequested();
	// Runaway bound (~400 frames) even when the caller passes an unbounded
	// budget; steppers re-enter with fresh budgets after dispatches.
	constexpr uint64_t kMax = 20'000ull * 400;
	uint64_t budget = max_cycles == 0 ? kMax : max_cycles;
	if (budget > kMax)
		budget = kMax;

	for (uint64_t i = 0; i < budget; ++i) {
		if (c64->QuitRequested())
			throw SoftQuitException{board_.ExitCode()};

		// Resolve-then-classify. (1) An unowned NMI sequence — including
		// a mid-steal hijack tail — nests over ANY live nest: yield it
		// first. (2) Otherwise an unowned boundary standing at a fetch or
		// inside an unresolved IRQ sequence yields its RESOLVED kind.
		if (!nmi_seq_active_ && twin_in_nmi_family()) {
			bool nmi_r = false;
			if (!twin_resolve_accept_kind(nmi_r) || !nmi_r)
				SoftQuit(1, "lsp NMI family did not resolve as NMI");
			REVM_LOG(REVM_VERBOSE,
			         "lsp yield resolved nmi=1 main=%u twin=%u",
			         board_.CycleCounter(), twin_->CycleCounter());
			return AdvanceResult::HitNmi;
		}
		if (!irq_seq_active_ && !nmi_seq_active_ &&
		    (twin_->FetchWouldTakeHw() || twin_in_irq_family())) {
			bool nmi_r = false;
			if (!twin_resolve_accept_kind(nmi_r))
				SoftQuit(1, "lsp accept boundary did not resolve");
			REVM_LOG(REVM_VERBOSE,
			         "lsp yield resolved nmi=%d main=%u twin=%u", int(nmi_r),
			         board_.CycleCounter(), twin_->CycleCounter());
			return nmi_r ? AdvanceResult::HitNmi : AdvanceResult::HitIrq;
		}

		// Twin half-step FIRST: park at an accept boundary and yield so
		// Sync can dispatch before the steal executes. twin_step_only —
		// the explicit Main half-step below completes the pair. A
		// boundary the running handler owns (same-source re-pulse /
		// coalesce) is stepped over silently — predictive, ZERO extra
		// stepping, matching the long-standing token semantics; a hijack
		// hiding in such a sequence surfaces later through the family-
		// aware nested-return paths. An UNOWNED boundary is resolved to
		// its entered vector before yielding — dispatch never trusts the
		// detection-time prediction.
		if (twin_->FetchWouldTakeHw()) {
			const CpuState tw0 = twin_->GetCpuState();
			const bool owned =
				tw0.nmi_pending ? nmi_seq_active_ : irq_seq_active_;
			if (!owned) {
				bool nmi_r = false;
				if (!twin_resolve_accept_kind(nmi_r))
					SoftQuit(1, "lsp pending fetch did not resolve");
				REVM_LOG(REVM_VERBOSE,
				         "lsp yield pend nmi=%d main=%u twin=%u",
				         int(nmi_r), board_.CycleCounter(),
				         twin_->CycleCounter());
				return nmi_r ? AdvanceResult::HitNmi
				             : AdvanceResult::HitIrq;
			}
			REVM_LOG(REVM_VERBOSE,
			         "lsp swallow owned nmi=%d irq_own=%d nmi_own=%d "
			         "main=%u twin=%u",
			         int(tw0.nmi_pending), int(irq_seq_active_),
			         int(nmi_seq_active_), board_.CycleCounter(),
			         twin_->CycleCounter());
		}
		const bool in_irq0 = twin_in_irq_family();
		const bool in_nmi0 = twin_in_nmi_family();
		const bool twin_vb = twin_step_only();
		// BA-stall hole / fresh steals: the step can ENTER a sequence.
		// An unowned entry is resolved to its vector decision before
		// yielding (a hijack out of a fresh IRQ begin surfaces as NMI);
		// an owned nest progresses silently.
		const bool fam_nmi1 = twin_in_nmi_family();
		const bool fam_irq1 = twin_in_irq_family();
		if (fam_nmi1 && !in_nmi0) {
			if (!nmi_seq_active_) {
				bool nmi_r = false;
				if (!twin_resolve_accept_kind(nmi_r) || !nmi_r)
					SoftQuit(1,
					         "lsp nmi steal confirm did not resolve "
					         "as NMI");
				REVM_LOG(REVM_VERBOSE,
				         "nmi steal confirm EmulateCycle cycle=%u pc=$%04X",
				         twin_->CycleCounter(), twin_->Pc());
				return AdvanceResult::HitNmi;
			}
		} else if (fam_irq1 && !in_irq0 && !irq_seq_active_) {
			bool nmi_r = false;
			if (!twin_resolve_accept_kind(nmi_r))
				SoftQuit(1, "lsp irq steal confirm did not resolve");
			REVM_LOG(REVM_VERBOSE,
			         "%s steal confirm EmulateCycle cycle=%u pc=$%04X",
			         nmi_r ? "hijacked-nmi" : "irq", twin_->CycleCounter(),
			         twin_->Pc());
			return nmi_r ? AdvanceResult::HitNmi : AdvanceResult::HitIrq;
		}

		// Main half-step.
		track_main_lines();
		const bool vb = board_.EmulateCycle();
		track_main_lines();
		if (vb || twin_vb) {
			if (vb)
				pace_after_vsync();
			if (c64->QuitRequested())
				throw SoftQuitException{board_.ExitCode()};
			if (stop_at_vsync && vb)
				return AdvanceResult::HitVSync;
		}
	}
	return AdvanceResult::Ok;
}

void CpuMockHost::RunIrqHandler() {
	ThrowIfQuitRequested();
	MOS6510 * hw = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	if (NoTwin()) {
		// Chip-driven dispatch without an oracle: consume and thunk.
		if (hw)
			hw->CpuMockClearIrqPending();
		event::EmitHandler("irq", "enter");
		MockIrqThunk(this);
		event::EmitHandler("irq", "exit");
		return;
	}
	// A handler's teardown walks (ReturnIrq) can surface FOLLOW-UP accepts
	// (Twin RTI'd, next line already pending). Each gets its own dispatch —
	// LeftNest only retires THIS nest, it does not consume the next event.
	int guard = 0;
	for (;;) {
		enter_interrupt(false);
		step_ctx_ = "irq-thunk";
		event::EmitHandler("irq", "enter");
		MockIrqThunk(this);
		consume_twin_steal();
		event::EmitHandler("irq", "exit");
		REVM_LOG(REVM_VERBOSE, "RunIrqHandler done main=%u twin=%u",
		         board_.CycleCounter(), twin_ ? twin_->CycleCounter() : 0);
		if (++guard > 8)
			SoftQuit(1, "RunIrqHandler follow-up storm");
		if (!twin_at_hw_steal() || irq_seq_active_)
			break;
		// Resolve-then-classify: a follow-up accept is claimed only when
		// its RESOLVED vector says IRQ. An NMI-shaped boundary (dedicated
		// NMI sequence, or the shared $FFFA tail a mid-steal hijack
		// entered) belongs to the dispatcher: claiming it here would arm
		// the IRQ token before Twin's real line fires and starve Main's
		// music path. Leave it; Sync's next walk yields it as HitNmi in
		// order.
		bool nmi_r = false;
		if (!twin_resolve_accept_kind(nmi_r))
			break;
		if (nmi_r) {
			twin_repair_resolve_gap();
			break;
		}
		REVM_LOG(REVM_VERBOSE,
		         "follow-up IRQ accept main=%u twin=%u", board_.CycleCounter(),
		         twin_->CycleCounter());
	}
}

void CpuMockHost::RunNmiHandler() {
	ThrowIfQuitRequested();
	MOS6510 * hw = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	if (NoTwin()) {
		if (hw)
			hw->CpuMockClearNmiPending();
		event::EmitHandler("nmi", "enter");
		MockNmiThunk(this);
		event::EmitHandler("nmi", "exit");
		return;
	}
	enter_interrupt(true);
	int guard = 0;
	for (;;) {
		step_ctx_ = "nmi-thunk";
		event::EmitHandler("nmi", "enter");
		MockNmiThunk(this);
		consume_twin_steal();
		event::EmitHandler("nmi", "exit");
		REVM_LOG(REVM_VERBOSE, "RunNmiHandler done main=%u twin=%u",
		         board_.CycleCounter(), twin_ ? twin_->CycleCounter() : 0);
		if (++guard > 8)
			SoftQuit(1, "RunNmiHandler follow-up storm");
		if (!twin_at_hw_steal() || nmi_seq_active_)
			break;
		// Mirror of the IRQ kind check, resolved from the entered vector:
		// an IRQ-shaped boundary ($FFFE tail) belongs to the IRQ path —
		// leave it for the dispatcher instead of claiming it as an NMI
		// accept.
		bool nmi_r = false;
		if (!twin_resolve_accept_kind(nmi_r))
			break;
		if (!nmi_r) {
			twin_repair_resolve_gap();
			break;
		}
		REVM_LOG(REVM_VERBOSE,
		         "follow-up NMI accept main=%u twin=%u",
		         board_.CycleCounter(), twin_->CycleCounter());
		enter_interrupt(true);
	}
}

bool CpuMockHost::TwinInInterruptNest() const {
	if (NoTwin())
		return false;
	if (InNmi())
		return nmi_seq_active_;
	if (InIrq())
		return irq_seq_active_;
	return twin_at_hw_steal();
}

bool CpuMockHost::TwinInIrqSequence() const {
	return twin_ && twin_->InIrqSequence();
}

bool CpuMockHost::TwinInNmiSequence() const {
	return twin_ && twin_->InNmiSequence();
}

bool CpuMockHost::TwinIoLandedOnLastPhi2() const {
	if (NoTwin())
		return false;
	const uint32_t now = twin_->CycleCounter();
	TwinIoAccess a{};
	if (TwinLastIoRead(a) && a.cycle + 1 == now)
		return true;
	if (TwinLastIoWrite(a) && a.cycle + 1 == now)
		return true;
	return false;
}

bool CpuMockHost::twin_rti_in_progress() const {
	if (!twin_)
		return false;
	C64 * c64 = twin_->board().Machine();
	if (!c64 || !c64->TheCPU)
		return false;
	return c64->TheCPU->CurrentOp() == 0x40 &&
	       !c64->TheCPU->InstructionComplete();
}

// Step Twin one Φ2 without touching Main. Sequence-ownership maintenance
// only; used where Main must stay parked (I/O AtPc inject walks).
bool CpuMockHost::twin_step_only() {
	const bool rti0 = twin_rti_in_progress();
	const bool irq0 = twin_->InIrqSequence();
	const bool nmi0 = twin_->InNmiSequence();
	const bool hit_vb = twin_->board().EmulateCycle();
	if ((!irq0 && twin_->InIrqSequence()) ||
	    (!nmi0 && twin_->InNmiSequence())) {
		const bool owned =
			twin_->InNmiSequence() ? nmi_seq_active_ : irq_seq_active_;
		REVM_LOG(REVM_VERBOSE,
		         "seq-enter irq=%d nmi=%d owned=%d main=%u twin=%u",
		         int(twin_->InIrqSequence()), int(twin_->InNmiSequence()),
		         int(owned), board_.CycleCounter(), twin_->CycleCounter());
		REVM_LOG(REVM_VERBOSE, "seq-enter ctx=%s", step_ctx_);
	}
	// Ownership spans the WHOLE ISR body: it ends when Twin executes its
	// RTI (not when the 7-cycle steal finishes). Same-source re-pulses
	// (CIA ACK vs delayed-underflow race) are owned by the running
	// handler, matching the old token semantics. NMI nests over IRQ, so
	// a completed RTI retires the innermost active nest only.
	if (rti0 && !twin_rti_in_progress()) {
		if (nmi_seq_active_ && !twin_->InNmiSequence()) {
			REVM_LOG(REVM_VERBOSE, "nmi nest retired (RTI)");
			nmi_seq_active_ = false;
		} else if (irq_seq_active_ && !twin_->InIrqSequence()) {
			REVM_LOG(REVM_VERBOSE, "irq nest retired (RTI)");
			irq_seq_active_ = false;
		}
	}
	return hit_vb;
}

// Step Twin one Φ2 — always paired with one Main Φ2 (Twin leads by
// construction). Chip time stays locked: Main's CIA/VIC tick the same Φ2
// as Twin's, so enter_interrupt's skew verdict compares like with like,
// and nested accepts surface on both boards together. When Twin's RTI
// completes, the sequence stops being owned by the dispatched handler.
// Returns whether TWIN crossed VBLANK (Main's crossing is handled by the
// frame-boundary hook; the boards stay frame-locked).
bool CpuMockHost::twin_emulate_cycle() {
	track_main_lines();
	const bool hit_vb = twin_step_only();
	const bool main_vb = board_.EmulateCycle();
	track_main_lines();
	if (main_vb)
		pace_after_vsync();
	return hit_vb;
}

bool CpuMockHost::twin_at_hw_steal() const {
	// Whole-sequence families, not just the first state: a resolved
	// accept rests past O_IRQ/O_NMI proper (vector-tail states) while it
	// waits for its dispatch.
	return twin_ && (twin_->FetchWouldTakeHw() || twin_in_irq_family() ||
	                 twin_in_nmi_family());
}

// Execute Twin's parked steal sequence (7 Φ2) so the accept boundary is
// consumed — otherwise every later stepper pre-check re-yields the same
// accept. Then walk ~40 more Φ2 of ISR preamble (bounded, like the classic
// drain): handlers whose top ACKs the source (STA/LDA ICR) must get past
// that point or Twin's still-asserted line re-yields immediately. Handlers
// with fences walk Twin themselves; those without (e.g. pure-C++ music_nmi)
// rely on this. Paired stepping keeps Main locked.
//
// Only consumes while THIS dispatch still owns its nest: if the handler's
// teardown walks already retired ownership (Twin RTI'd early), any boundary
// here belongs to the NEXT accept — leave it for the follow-up dispatch.
void CpuMockHost::consume_twin_steal() {
	if (NoTwin())
		return;
	step_ctx_ = "consume";
	int guard = 0;
	while ((irq_seq_active_ || nmi_seq_active_) &&
	       !twin_->InIrqSequence() && !twin_->InNmiSequence() &&
	       twin_->FetchWouldTakeHw()) {
		if (++guard > 32) {
			SoftQuit(1, "consume_twin_steal stuck twin_pc=$%04X",
			         twin_->Pc());
			return;
		}
		(void)twin_emulate_cycle();
	}
	constexpr int kPreamblePhi2 = 40;
	step_ctx_ = "preamble";
	for (int i = 0; i < kPreamblePhi2; ++i) {
		if (!irq_seq_active_ && !nmi_seq_active_)
			return; // ownership retired (RTI seen) — stop consuming
		if (!twin_in_irq_family() && !twin_in_nmi_family())
			return; // sequence ended; next boundary is not ours
		const bool irq0 = twin_in_irq_family();
		const bool nmi0 = twin_in_nmi_family();
		const bool vb = twin_emulate_cycle();
		if (!nmi0 && twin_in_nmi_family())
			return; // nested / hijacked accept — let steppers yield it
		if (!irq0 && twin_in_irq_family())
			return;
		if (vb)
			return;
	}
}

uint8_t CpuMockHost::Read(uint16_t addr) {
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return 0;
	return c64->TheCPU->REUReadByte(addr);
}

void CpuMockHost::Write(uint16_t addr, uint8_t value) {
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return;
	c64->TheCPU->REUWriteByte(addr, value);
}

void CpuMockHost::InstallAsset(uint16_t addr, std::span<const uint8_t> bytes) {
	C64 * c64 = board_.Machine();
	if (!c64) return;
	for (std::size_t i = 0; i < bytes.size(); ++i)
		c64->RAM[uint16_t(addr + i)] = bytes[i];
}

void CpuMockHost::Sei() {
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return;
	// Don't drop the post-IRQ I hold mid-run (CIA IR clear delay).
	if (c64->TheCPU->CpuMockIrqHoldActive())
		return;
	c64->TheCPU->SetCpuMockIFlag(true);
}

void CpuMockHost::Cli() {
	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return;
	if (c64->TheCPU->CpuMockIrqHoldActive())
		return;
	c64->TheCPU->SetCpuMockIFlag(false);
}

bool CpuMockHost::IFlag() const {
	const C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) return true;
	return c64->TheCPU->CpuMockIFlag();
}

void CpuMockHost::enable_mock_cpu() {
	MOS6510 * cpu = board_.Machine()->TheCPU;
	cpu->SetCpuMock(true, nullptr, this, nullptr);
	cpu->SetCpuMockIFlag(true);
	REVM_LOG(REVM_DEBUG, "mock 6510 ON (no opcode fetch; chips drive "
		"irq_pending/nmi_pending; Sync dispatches RunIrqHandler/RunNmiHandler)");
}

bool CpuMockHost::setup_twin() {
	twin_ = std::make_unique<TwinBoard>();
	std::string error;
	if (!twin_->Init(error)) {
		REVM_LOG(REVM_ERROR, "twin Init failed: %s", error.c_str());
		twin_.reset();
		return false;
	}
	const std::string & path = begin_snap_path();
	if (path.empty()) {
		REVM_LOG(REVM_ERROR, "--load-snapshot required (twin needs BEGIN FullSnapshot)");
		twin_.reset();
		return false;
	}
	if (!twin_->LoadSnap(path, error)) {
		REVM_LOG(REVM_ERROR, "twin LoadSnap failed: %s", error.c_str());
		twin_.reset();
		return false;
	}
	REVM_LOG(REVM_DEBUG, "twin live oracle ON");
	return true;
}

void CpuMockHost::apply_user_cfg(Config & cfg, const Config * user_cfg) {
	if (!user_cfg) return;
	cfg.kb_path = user_cfg->kb_path;
	cfg.kb_watch = user_cfg->kb_watch;
	cfg.kb_watch_verbose = user_cfg->kb_watch_verbose;
	cfg.kb_watch_window = user_cfg->kb_watch_window;
	cfg.kb_watch_font = user_cfg->kb_watch_font;
	cfg.kb_trace_calls = user_cfg->kb_trace_calls;
	cfg.kb_check = user_cfg->kb_check;
	cfg.kb_check_verbose = user_cfg->kb_check_verbose;
	cfg.rom_dir = user_cfg->rom_dir;
	cfg.begin_snap_path = user_cfg->begin_snap_path;
	cfg.rand_seed = user_cfg->rand_seed;
	cfg.save_snapshot_cycle = user_cfg->save_snapshot_cycle;
	cfg.save_snapshot_path = user_cfg->save_snapshot_path;
	cfg.add_play_snapshot_cycle = user_cfg->add_play_snapshot_cycle;
	cfg.play_path = user_cfg->play_path.empty() ? cfg.play_path : user_cfg->play_path;
	cfg.save_audio_path = user_cfg->save_audio_path;
	cfg.save_video_path = user_cfg->save_video_path;
	cfg.save_screen_frame = user_cfg->save_screen_frame;
	cfg.save_screen_path = user_cfg->save_screen_path;
	cfg.frame_observations_path = user_cfg->frame_observations_path;
	cfg.max_cycles = user_cfg->max_cycles;
	cfg.max_seconds = user_cfg->max_seconds;
	cfg.log_verbose_from_cycle = user_cfg->log_verbose_from_cycle;
	cfg.log_debug_from_cycle = user_cfg->log_debug_from_cycle;
	cfg.events_path = user_cfg->events_path;
	cfg.report_path = user_cfg->report_path;
	cfg.no_audio = user_cfg->no_audio;
	cfg.sid_resid = user_cfg->sid_resid;
	cfg.main_blank = user_cfg->main_blank;
	if (cfg.no_audio) {
		cfg.audio_enabled = false;
		cfg.sid_resid = false;
	}
}

const std::string & CpuMockHost::begin_snap_path() const {
	return board_.GetConfig().begin_snap_path;
}

void CpuMockHost::setup_kb_check() {
	kb_check_.Reset();
	// Stage 3: kb-check KB watch:"yes" at CompareNow / JoinAtPc (not VBLANK).
	// --kb-check-verbose adds watch:"verbose" slots.
	if (board_.Kb().Empty() || NoTwin()) return;
	const Config & cfg = board_.GetConfig();
	kb_check_.Configure(board_.Kb(), true, cfg.kb_check_verbose);
	REVM_LOG(REVM_DEBUG, "kb-check: %zu slot(s), %zu byte(s)%s",
	              kb_check_.SlotCount(), kb_check_.ByteCount(),
	              cfg.kb_check_verbose ? " (verbose)" : "");
}


CpuMockHost::AdvanceResult CpuMockHost::twin_guard_before_advance() {
	const uint32_t main_f = board_.FrameCounter();
	const uint32_t twin_f = twin_->FrameCounter();
	if (twin_f > main_f + 1) {
		REVM_LOG(REVM_ERROR, "twin FAIL: twin frame=%u main frame=%u (more than one VSYNC ahead)",
			twin_f, main_f);
		return AdvanceResult::Error;
	}
	if (twin_f == main_f + 1) {
		// Already parked at next barrier.
		return AdvanceResult::HitVSync;
	}
	if (twin_f < main_f) {
		REVM_LOG(REVM_ERROR, "twin FAIL: twin frame=%u behind main frame=%u "
			"(Main VSYNC should have caught Twin up)",
			twin_f, main_f);
		return AdvanceResult::Error;
	}
	return AdvanceResult::Ok;
}

void CpuMockHost::on_main_vsync(uint32_t frame, uint32_t cycle,
                                const InputFrame & in) {
	(void)cycle;
	++tally_vsyncs_;
	tally_tick(frame);
	if (NoTwin()) return;
	const uint32_t twin_f = twin_->FrameCounter();
	// Lockstep keeps the boards frame-locked; the raw extra-VSYNC / inject
	// corners can drift one frame either way until the next paired walk.
	if (twin_f == frame || twin_f == frame + 1 || twin_f + 1 == frame) {
		if (twin_f != frame)
			REVM_LOG(REVM_VERBOSE,
			         "on_main_vsync frame drift main=%u twin=%u (deferred "
			         "input applies as-is)",
			         frame, twin_f);
		twin_->ApplyInput(in);
		return;
	}
	SoftQuit(1, "on_main_vsync frame drift main=%u twin=%u", frame, twin_f);
}

uint16_t CpuMockHost::RasterLine() const {
	const C64 * machine = board_.Machine();
	return machine && machine->TheVIC
	           ? uint16_t(machine->TheVIC->RasterY())
	           : uint16_t(0);
}

void CpuMockHost::note_fence(const char * op, uint16_t pc) {
	last_fence_op_ = op; // literal at every ev_fence call site
	last_fence_pc_ = pc;
}

void CpuMockHost::emit_fail_context(CompareMask mask,
                                    const RitualCheck & kb_check,
                                    const char * plane) {
	// Which channels the mask asked for, and what actually ran: "ok" /
	// "fail" / "off" — the same tri-state as the "compare" record's
	// 1 / 0 / -1 channel values.
	std::string mask_used;
	auto add_mask = [&](bool on, const char * name) {
		if (!on) return;
		if (!mask_used.empty()) mask_used += '+';
		mask_used += name;
	};
	add_mask(mask.screen, "screen");
	add_mask(mask.sid, "sid");
	add_mask(mask.vic, "vic");
	add_mask(mask.vic_state, "vic_state");
	add_mask(mask.cia1, "cia1");
	add_mask(mask.cia2, "cia2");
	add_mask(mask.kb, "kb");

	const bool compare_ran = last_screen_.ran || last_sid_.ran ||
	                         last_vic_.ran || last_vic_state_.ran ||
	                         last_cia1_.ran || last_cia2_.ran || kb_check.ran;
	auto fmt_channel = [](const RitualCheck & c) {
		return c.ran ? (c.ok ? "ok" : "fail") : "off";
	};
	char chans[160];
	std::snprintf(chans, sizeof chans,
	              "{\"screen\":\"%s\",\"sid\":\"%s\",\"vic\":\"%s\","
	              "\"vic_state\":\"%s\",\"cia1\":\"%s\",\"cia2\":\"%s\","
	              "\"kb\":\"%s\"}",
	              fmt_channel(last_screen_), fmt_channel(last_sid_),
	              fmt_channel(last_vic_), fmt_channel(last_vic_state_),
	              fmt_channel(last_cia1_), fmt_channel(last_cia2_),
	              fmt_channel(kb_check));

	// kb diff detail, pre-serialized (null when the kb compare did not run
	// or found nothing).
	std::string kb_diffs = "null";
	if (kb_check.ran && !kb_check.ok && kb_check_.HasFirstDiff()) {
		char b[192];
		std::snprintf(b, sizeof b,
		              ",\"addr\":\"0x%04X\","
		              "\"main\":\"0x%02X\",\"twin\":\"0x%02X\"}",
		              kb_check_.FirstDiffAddr(),
		              kb_check_.FirstDiffMain(), kb_check_.FirstDiffTwin());
		kb_diffs = "{\"name\":" + event::QuoteString(kb_check_.FirstDiffName()) + b;
	}

	char bbox[96];
	std::snprintf(bbox, sizeof bbox,
	              "{\"pixels\":%u,\"x0\":%u,\"y0\":%u,\"x1\":%u,\"y1\":%u}",
	              last_screen_pixels_, last_screen_x0_, last_screen_y0_,
	              last_screen_x1_, last_screen_y1_);

	// Screen fields are null when this failure has no screen-plane data
	// (fence misses; capture failures).
	const bool have_screen = !last_screen_summary_.empty();
	const std::string recent = event::RecentJsonArray();
	REVM_EVENT("fail_context", "plane", plane, "op", last_fence_op_, "pc",
	           revm::event::Hex{last_fence_pc_}, "mask_used", mask_used.c_str(),
	           "compare_ran", compare_ran, "raster_line", RasterLine(),
	           "channels", revm::event::RawJson{chans}, "kb_diffs",
	           revm::event::RawJson{kb_diffs.c_str()}, "screen_bbox",
	           revm::event::RawJson{have_screen ? bbox : nullptr},
	           "screen_summary",
	           have_screen ? revm::event::Val{last_screen_summary_.c_str()}
	                       : revm::event::Val{revm::event::RawJson{nullptr}},
	           "last_events", revm::event::RawJson{recent.c_str()});
}

void CpuMockHost::emit_fence_miss_context(CompareMask mask) {
	static const RitualCheck none{};
	last_screen_ = {};
	last_sid_ = {};
	last_vic_ = {};
	last_vic_state_ = {};
	last_cia1_ = {};
	last_cia2_ = {};
	last_screen_summary_.clear(); // no screen data at a fence miss
	emit_fail_context(mask, none, "fence");
}

void CpuMockHost::compare_screen(uint32_t frame, RitualCheck & out) {
	bool ok = true;
	ScreenSnapshot main_screen{};
	ScreenSnapshot twin_screen{};
	if (!CaptureScreenSnapshot(board_, main_screen) ||
	    !twin_->CaptureScreen(twin_screen)) {
		ok = false;
		REVM_LOG(REVM_ERROR,
		         "FAIL frame %u [main↔twin]: screen capture failed", frame);
	} else if (std::memcmp(main_screen.pixels, twin_screen.pixels,
	                       ScreenSnapshot::kBytes) != 0) {
		ok = false;
		REVM_LOG(REVM_ERROR, "FAIL frame %u [main↔twin]: screen mismatch",
		         frame);
		const std::string report =
			screen_diff_report(twin_screen, main_screen);
		if (!report.empty()) {
			REVM_LOG(REVM_ERROR, "%s", report.c_str());
		} else {
			PrintScreenDiff(log::Stream(), twin_screen, main_screen);
		}
		// Structured diverging-byte records (capped) + bounding box.
		{
			unsigned diffs = 0;
			unsigned shown = 0;
			unsigned x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			for (size_t i = 0; i < ScreenSnapshot::kBytes; ++i) {
				if (main_screen.pixels[i] == twin_screen.pixels[i]) continue;
				const unsigned x = unsigned(i % ScreenSnapshot::kWidth);
				const unsigned y = unsigned(i / ScreenSnapshot::kWidth);
				if (diffs == 0) {
					x0 = x1 = x;
					y0 = y1 = y;
				} else {
					x0 = x0 < x ? x0 : x;
					y0 = y0 < y ? y0 : y;
					x1 = x1 > x ? x1 : x;
					y1 = y1 > y ? y1 : y;
				}
				++diffs;
				if (shown < kEventFailCap) {
					emit_compare_fail("screen", uint32_t(i),
					                  main_screen.pixels[i],
					                  twin_screen.pixels[i]);
					++shown;
				}
			}
			if (diffs > shown) emit_overflow_marker("screen");
			// Context oracle: decode WHAT diverged from each board's own
			// state. Failure-only cost; pure after-the-fact arithmetic.
			{
				ChipSnapshot main_chip{};
				ChipSnapshot twin_chip{};
				if (CaptureChipSnapshot(board_, main_chip) &&
				    twin_->CaptureChips(twin_chip)) {
					screenctx::Params p{};
					p.main = screen_ctx_side(main_chip);
					p.twin = screen_ctx_side(twin_chip);
					ScreenCtxDiffMem diff_mem{&main_screen, &twin_screen};
					p.main_fetch = screen_ctx_fetch;
					p.main_user = board_.Machine();
					p.twin_fetch = screen_ctx_fetch;
					p.twin_user = twin_->board().Machine();
					p.diff_at = screen_ctx_diff;
					p.diff_user = &diff_mem;
					p.x0 = x0;
					p.y0 = y0;
					p.x1 = x1;
					p.y1 = y1;
					const screenctx::Result cr = screenctx::Analyze(p);
					last_screen_summary_ = cr.summary;
					// Fail-context state for net_fail / fail_context, plus
					// the pixel count + rows on the human context line.
					last_screen_pixels_ = diffs;
					last_screen_x0_ = x0;
					last_screen_y0_ = y0;
					last_screen_x1_ = x1;
					last_screen_y1_ = y1;
					last_screen_cells_json_ = cr.cells_json;
					{
						char bb[64];
						std::snprintf(bb, sizeof bb, " [pixels %u, rows %u-%u]",
						              diffs, y0, y1);
						last_screen_summary_ += bb;
					}
					REVM_LOG(REVM_ERROR, "%s", last_screen_summary_.c_str());
					REVM_EVENT(
						"screen_fail", "pixels", diffs, "x0", x0, "y0", y0,
						"x1", x1, "y1", y1,
						"context", revm::event::RawJson{cr.context_json.c_str()},
						"cells", revm::event::RawJson{cr.cells_json.c_str()},
						"sprites", revm::event::RawJson{cr.sprites_json.c_str()},
						"background_involved", cr.background_involved,
						"border_involved", cr.border_involved);
					// One first-class net-fail row (rendered_frame is the
					// frame-observation native_frame label that renders this
					// frame's settled picture; MEASURED one past the report
					// frame on the Paradroid play-chain net, 333/333 rows).
					{
						char repro[48];
						std::snprintf(repro, sizeof repro, "--max-frames %u",
						              frame + 1);
						REVM_EVENT(
							"net_fail", "plane", "screen", "play",
							board_.GetConfig().play_path.c_str(), "report_frame",
							frame, "rendered_frame", frame + 1, "pixels", diffs,
							"x0", x0, "y0", y0, "x1", x1, "y1", y1, "cells",
							revm::event::RawJson{cr.cells_json.c_str()},
							"repro_command", repro);
					}
				} else {
					REVM_EVENT("screen_fail", "pixels", diffs, "x0", x0,
					           "y0", y0, "x1", x1, "y1", y1);
				}
			}
		}
		if (compare_opts_.dump_screen_images &&
		    (compare_opts_.dump_fail_limit == 0 ||
		     dump_images_written_ < compare_opts_.dump_fail_limit)) {
			const std::string img_dir = compare_opts_.dump_fail_dir.empty()
				? std::string("revm-fail")
				: compare_opts_.dump_fail_dir;
			std::string dump_err;
			if (!WriteScreenFailImages(img_dir, frame, twin_screen,
			                           main_screen, dump_err)) {
				REVM_LOG(REVM_ERROR, "screen dump failed: %s",
				         dump_err.c_str());
			} else {
				++dump_images_written_;
			}
		}
	}
	out = {true, false, ok};
	screen_tally_.Note(false, ok);
}

void CpuMockHost::compare_sid(uint32_t frame, RitualCheck & out) {
	bool ok = true;
	ChipSnapshot main_chip{};
	ChipSnapshot twin_chip{};
	if (!CaptureChipSnapshot(board_, main_chip) ||
	    !twin_->CaptureChips(twin_chip)) {
		ok = false;
		REVM_LOG(REVM_ERROR,
		         "FAIL frame %u [main↔twin]: chip capture failed", frame);
	} else if (!SidConfigEqual(main_chip.sid, twin_chip.sid)) {
		ok = false;
		REVM_LOG(REVM_ERROR, "FAIL frame %u [main↔twin]: SID mismatch",
		         frame);
		PrintSidConfigDiff(log::Stream(), twin_chip.sid, main_chip.sid);
		last_sid_detail_[0] = 0;
		unsigned shown = 0;
		for (const SidField & f : kSidFields) {
			const uint8_t m = main_chip.sid.*f.ptr;
			const uint8_t t = twin_chip.sid.*f.ptr;
			if (m == t) continue;
			if (last_sid_detail_[0] == 0)
				std::snprintf(last_sid_detail_, sizeof last_sid_detail_,
				              "$D4%02X main=$%02X twin=$%02X", f.addr, m, t);
			if (shown < kEventFailCap)
				emit_compare_fail("sid", 0xD400u + f.addr, m, t);
			++shown;
		}
		if (shown > kEventFailCap) emit_overflow_marker("sid");
	}
	out = {true, false, ok};
	sid_tally_.Note(false, ok);
}

bool CpuMockHost::capture_compare_chips(ChipSnapshot & main_chip,
                                        ChipSnapshot & twin_chip,
                                        uint32_t frame) {
	if (CaptureChipSnapshot(board_, main_chip) && twin_->CaptureChips(twin_chip))
		return true;
	REVM_LOG(REVM_ERROR, "FAIL frame %u [main↔twin]: chip capture failed",
	         frame);
	return false;
}

void CpuMockHost::compare_vic(uint32_t frame, RitualCheck & out) {
	bool ok = true;
	ChipSnapshot main_chip{};
	ChipSnapshot twin_chip{};
	last_vic_detail_[0] = 0;
	if (!capture_compare_chips(main_chip, twin_chip, frame)) {
		ok = false;
	} else if (!VicConfigEqual(main_chip.vic, twin_chip.vic)) {
		ok = false;
		REVM_LOG(REVM_ERROR, "FAIL frame %u [main↔twin]: VIC mismatch", frame);
		PrintVicConfigDiff(log::Stream(), twin_chip.vic, main_chip.vic);
		note_vic_config_fails(main_chip.vic, twin_chip.vic, last_vic_detail_,
		                      sizeof last_vic_detail_);
	}
	out = {true, false, ok};
	vic_tally_.Note(false, ok);
}

void CpuMockHost::compare_vic_state(uint32_t frame, RitualCheck & out) {
	bool ok = true;
	ChipSnapshot main_chip{};
	ChipSnapshot twin_chip{};
	if (!capture_compare_chips(main_chip, twin_chip, frame)) {
		ok = false;
	} else if (std::memcmp(&main_chip.vic, &twin_chip.vic, sizeof(MOS6569State)) !=
	           0) {
		ok = false;
		REVM_LOG(REVM_ERROR,
		         "FAIL frame %u [main↔twin]: VIC state mismatch", frame);
		if (!VicConfigEqual(main_chip.vic, twin_chip.vic))
			PrintVicConfigDiff(log::Stream(), twin_chip.vic, main_chip.vic);
		else
			REVM_LOG(REVM_ERROR,
			         "config regs match — divergence is status latches / SC internals");
	}
	out = {true, false, ok};
	vic_state_tally_.Note(false, ok);
}

void CpuMockHost::compare_cia(int which, uint32_t frame, RitualCheck & out) {
	bool ok = true;
	ChipSnapshot main_chip{};
	ChipSnapshot twin_chip{};
	const char * name = which == 0 ? "CIA1" : "CIA2";
	const uint8_t pra_mask = which == 0 ? uint8_t(0) : uint8_t(0x03);
	char * detail = which == 0 ? last_cia1_detail_ : last_cia2_detail_;
	detail[0] = 0;
	if (!capture_compare_chips(main_chip, twin_chip, frame)) {
		ok = false;
	} else {
		const MOS6526State & m = which == 0 ? main_chip.cia1 : main_chip.cia2;
		const MOS6526State & t = which == 0 ? twin_chip.cia1 : twin_chip.cia2;
		if (!CiaConfigEqual(m, t, pra_mask)) {
			ok = false;
			REVM_LOG(REVM_ERROR, "FAIL frame %u [main↔twin]: %s config mismatch",
			         frame, name);
			fill_cia_config_detail(detail, 96, which == 0 ? "cia1" : "cia2", m, t,
			                       pra_mask);
			note_cia_config_fails(which == 0 ? "cia1" : "cia2",
			                      which == 0 ? uint16_t(0xDC00) : uint16_t(0xDD00),
			                      m, t, pra_mask);
			if (detail[0])
				REVM_LOG(REVM_ERROR, "  %s", detail);
		}
	}
	out = {true, false, ok};
	(which == 0 ? cia1_tally_ : cia2_tally_).Note(false, ok);
}

void CpuMockHost::CompareNow(CompareMask mask) {
	ThrowIfQuitRequested();
	if (NoTwin() || !mask.any())
		return;

	const uint32_t main_f = board_.FrameCounter();
	const uint32_t twin_f = twin_->FrameCounter();
	if (twin_f != main_f) {
		SoftQuit(1,
		         "CompareNow Twin on another frame (main=%u twin=%u) — "
		         "not cycle-aligned",
		         main_f, twin_f);
	}

	++total_compares_;

	const uint32_t frame = main_f;
	const uint32_t cycle = board_.CycleCounter();
	last_screen_ = {};
	last_sid_ = {};
	last_vic_ = {};
	last_vic_state_ = {};
	last_cia1_ = {};
	last_cia2_ = {};
	last_screen_summary_.clear();
	last_screen_pixels_ = 0;
	last_screen_x0_ = 0;
	last_screen_y0_ = 0;
	last_screen_x1_ = 0;
	last_screen_y1_ = 0;
	last_screen_cells_json_ = "[]";
	last_sid_detail_[0] = 0;
	last_vic_detail_[0] = 0;
	last_cia1_detail_[0] = 0;
	last_cia2_detail_[0] = 0;
	RitualCheck kb{};
	bool abort_check = false;

	if (mask.screen) {
		compare_screen(frame, last_screen_);
		if (!last_screen_.ok)
			abort_check = true;
	}
	if (mask.sid) {
		compare_sid(frame, last_sid_);
		if (!last_sid_.ok)
			abort_check = true;
	}
	if (mask.vic) {
		compare_vic(frame, last_vic_);
		if (!last_vic_.ok)
			abort_check = true;
	}
	if (mask.vic_state) {
		compare_vic_state(frame, last_vic_state_);
		if (!last_vic_state_.ok)
			abort_check = true;
	}
	if (mask.cia1) {
		compare_cia(0, frame, last_cia1_);
		if (!last_cia1_.ok)
			abort_check = true;
	}
	if (mask.cia2) {
		compare_cia(1, frame, last_cia2_);
		if (!last_cia2_.ok)
			abort_check = true;
	}
	if (mask.kb && kb_check_.Active()) {
		const bool twin_ok =
			kb_check_.OnFrame(board_, *twin_, frame, cycle,
			                  /*log_mismatches=*/true, &linked_registry_);
		kb = {true, false, twin_ok};
		kb_check_.Tally().Note(false, twin_ok);
		if (!twin_ok)
			abort_check = true;
	}

	auto ch = [](const RitualCheck & c) {
		return c.ran ? (c.ok ? 1 : 0) : -1;
	};
	if (last_screen_.ran || last_sid_.ran || last_vic_.ran ||
	    last_vic_state_.ran || last_cia1_.ran || last_cia2_.ran || kb.ran) {
		REVM_LOG(REVM_VERBOSE,
		         "compare frame=%u cycle=%u screen=%s SID=%s vic=%s vic_state=%s "
		         "cia1=%s cia2=%s kb=%s",
		         frame, cycle, last_screen_.Format(), last_sid_.Format(),
		         last_vic_.Format(), last_vic_state_.Format(), last_cia1_.Format(),
		         last_cia2_.Format(), kb.Format());
	}
	event::EmitCompareResult(ch(last_screen_), ch(last_sid_), ch(kb), ch(last_vic_),
	                         ch(last_vic_state_), ch(last_cia1_), ch(last_cia2_),
	                         int(RasterLine()));

	if (abort_check) {
		++total_fails_;
		// Name the aborting plane for the context record (first channel
		// that ran and failed; the QuitOnCheck chain below reports it in
		// the same order).
		const char * plane = "none";
		if (last_screen_.ran && !last_screen_.ok) plane = "screen";
		else if (last_sid_.ran && !last_sid_.ok) plane = "sid";
		else if (last_vic_.ran && !last_vic_.ok) plane = "vic";
		else if (last_vic_state_.ran && !last_vic_state_.ok) plane = "vic_state";
		else if (last_cia1_.ran && !last_cia1_.ok) plane = "cia1";
		else if (last_cia2_.ran && !last_cia2_.ok) plane = "cia2";
		else if (kb.ran && !kb.ok) plane = "kb";
		emit_fail_context(mask, kb, plane);
		static bool repro_hinted = false;
		if (!repro_hinted && std::strcmp(plane, "screen") == 0) {
			// MEASURED convention (Paradroid play-chain net, 333/333 rows):
			// a screen-fail REPORT N is shown by --max-frames N+1, while
			// --max-frames N exits 0 (the compare runs inside frame N).
			repro_hinted = true;
			REVM_LOG(REVM_ERROR,
			         "hint: a screen-fail report %u reproduces with "
			         "--max-frames %u",
			         frame, frame + 1);
		}
		if (!last_screen_.ok && last_screen_.ran) {
			if (!last_screen_summary_.empty())
				QuitOnCheck("screen FAIL frame %u %s", frame,
				            last_screen_summary_.c_str());
			else
				QuitOnCheck("screen FAIL frame %u", frame);
		}
		if (!last_sid_.ok && last_sid_.ran) {
			if (last_sid_detail_[0])
				QuitOnCheck("SID FAIL frame %u %s", frame, last_sid_detail_);
			else
				QuitOnCheck("SID FAIL frame %u", frame);
		}
		if (!last_vic_.ok && last_vic_.ran) {
			if (last_vic_detail_[0])
				QuitOnCheck("VIC FAIL frame %u %s", frame, last_vic_detail_);
			else
				QuitOnCheck("VIC FAIL frame %u", frame);
		}
		if (!last_vic_state_.ok && last_vic_state_.ran)
			QuitOnCheck("VIC state FAIL frame %u", frame);
		if (!last_cia1_.ok && last_cia1_.ran) {
			if (last_cia1_detail_[0])
				QuitOnCheck("CIA1 FAIL frame %u %s", frame, last_cia1_detail_);
			else
				QuitOnCheck("CIA1 FAIL frame %u", frame);
		}
		if (!last_cia2_.ok && last_cia2_.ran) {
			if (last_cia2_detail_[0])
				QuitOnCheck("CIA2 FAIL frame %u %s", frame, last_cia2_detail_);
			else
				QuitOnCheck("CIA2 FAIL frame %u", frame);
		}
		if (kb_check_.HasFirstDiff())
			QuitOnCheck("kb-check FAIL frame %u cycle %u %s $%04X main=$%02X "
			            "twin=$%02X",
			            frame, cycle, kb_check_.FirstDiffName(),
			            kb_check_.FirstDiffAddr(), kb_check_.FirstDiffMain(),
			            kb_check_.FirstDiffTwin());
		QuitOnCheck("kb-check FAIL frame %u cycle %u", frame, cycle);
	}
}

void CpuMockHost::print_compare_tallies() const {
	if (NoTwin() && total_compares_ == 0)
		return;
	REVM_LOG(REVM_DEBUG, "compares=%llu fail=%llu",
	         static_cast<unsigned long long>(total_compares_),
	         static_cast<unsigned long long>(total_fails_));
	for (int k = 0; k < 2; ++k) {
		const SkewStats & s = skew_stats_[k];
		if (!s.any && s.silent == 0)
			continue;
		REVM_LOG(REVM_INFO,
		         "skew %s: paired=%llu silent=%llu min=%+d max=%+d Φ2",
		         k == 0 ? "irq" : "nmi",
		         static_cast<unsigned long long>(s.paired),
		         static_cast<unsigned long long>(s.silent),
		         s.any ? s.min : 0, s.any ? s.max : 0);
	}
	if (screen_tally_.compared > 0)
		screen_tally_.LogLine(REVM_LOG_MODULE, "screen");
	if (sid_tally_.compared > 0)
		sid_tally_.LogLine(REVM_LOG_MODULE, "sid");
	if (vic_tally_.compared > 0)
		vic_tally_.LogLine(REVM_LOG_MODULE, "vic");
	if (vic_state_tally_.compared > 0)
		vic_state_tally_.LogLine(REVM_LOG_MODULE, "vic_state");
	if (cia1_tally_.compared > 0)
		cia1_tally_.LogLine(REVM_LOG_MODULE, "cia1");
	if (cia2_tally_.compared > 0)
		cia2_tally_.LogLine(REVM_LOG_MODULE, "cia2");
	kb_check_.PrintTally();
}

void CpuMockHost::finish_media() {
	std::string media_err;
	if (!board_.FinishMedia(media_err) && !media_err.empty()) {
		REVM_LOG(REVM_ERROR, "%s", media_err.c_str());
		if (board_.ExitCode() == 0)
			board_.RequestQuit(1);
	}
}

void CpuMockHost::tally_tick(uint32_t frame) {
	if (!tally_started_) {
		tally_started_ = true;
		tally_frame_ = frame;
		tally_vsyncs_ = 0;
		return;
	}
	if (frame - tally_frame_ < kTallyFrames) return;
	tally_frame_ = frame;

	REVM_LOG(REVM_INFO,
		"tally f=%u vsync=%u compares=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"fails=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"(screen/sid/vic/vic_state/cia1/cia2/kb)",
		frame, tally_vsyncs_,
		static_cast<unsigned long long>(screen_tally_.window.compared),
		static_cast<unsigned long long>(sid_tally_.window.compared),
		static_cast<unsigned long long>(vic_tally_.window.compared),
		static_cast<unsigned long long>(vic_state_tally_.window.compared),
		static_cast<unsigned long long>(cia1_tally_.window.compared),
		static_cast<unsigned long long>(cia2_tally_.window.compared),
		static_cast<unsigned long long>(kb_check_.Window().compared),
		static_cast<unsigned long long>(screen_tally_.window.failed),
		static_cast<unsigned long long>(sid_tally_.window.failed),
		static_cast<unsigned long long>(vic_tally_.window.failed),
		static_cast<unsigned long long>(vic_state_tally_.window.failed),
		static_cast<unsigned long long>(cia1_tally_.window.failed),
		static_cast<unsigned long long>(cia2_tally_.window.failed),
		static_cast<unsigned long long>(kb_check_.Window().failed));

	REVM_EVENT("tally_window",
	           "vsyncs", tally_vsyncs_,
	           "screen_cmp", screen_tally_.window.compared,
	           "screen_fail", screen_tally_.window.failed,
	           "sid_cmp", sid_tally_.window.compared,
	           "sid_fail", sid_tally_.window.failed,
	           "vic_cmp", vic_tally_.window.compared,
	           "vic_fail", vic_tally_.window.failed,
	           "vic_state_cmp", vic_state_tally_.window.compared,
	           "vic_state_fail", vic_state_tally_.window.failed,
	           "cia1_cmp", cia1_tally_.window.compared,
	           "cia1_fail", cia1_tally_.window.failed,
	           "cia2_cmp", cia2_tally_.window.compared,
	           "cia2_fail", cia2_tally_.window.failed,
	           "kb_cmp", kb_check_.Window().compared,
	           "kb_fail", kb_check_.Window().failed);

	tally_vsyncs_ = 0;
	screen_tally_.ResetWindow();
	sid_tally_.ResetWindow();
	vic_tally_.ResetWindow();
	vic_state_tally_.ResetWindow();
	cia1_tally_.ResetWindow();
	cia2_tally_.ResetWindow();
	kb_check_.ResetWindow();
}

void CpuMockHost::pace_after_vsync() {
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

CpuMockHost::AdvanceResult CpuMockHost::TwinRunToPc(uint16_t pc,
                                                    bool soft_fail,
                                                    bool yield_at_target,
                                                    bool pair_main) {
	// Register this walk as a fence (dedup: a retry of the same suspended
	// fence reactivates the top entry instead of growing the stack). The
	// registration lets interrupt teardown alignment recognize a fetch a
	// suspended fence still has to pair on — see twin_fenced_fetch_pending.
	int slot = -1;
	if (walk_fences_n_ > 0) {
		WalkFence & top = walk_fences_[walk_fences_n_ - 1];
		if (!top.active && top.pc == pc &&
		    top.yield_at_target == yield_at_target) {
			top.active = true;
			slot = walk_fences_n_ - 1;
		}
	}
	if (slot < 0) {
		if (walk_fences_n_ < kMaxWalkFences) {
			walk_fences_[walk_fences_n_] = {pc, yield_at_target, true};
			slot = walk_fences_n_++;
		} else {
			REVM_LOG(REVM_VERBOSE,
			         "walk fence table full want=$%04X depth=%d", pc,
			         walk_fences_n_);
		}
	}
	const AdvanceResult r =
		twin_run_to_pc_inner(pc, soft_fail, yield_at_target, pair_main);
	if (slot >= 0) {
		if (r == AdvanceResult::HitIrq || r == AdvanceResult::HitNmi)
			walk_fences_[slot].active = false; // suspended; dispatch retries
		else
			walk_fences_n_ = slot; // settled — pop it and anything above
	}
	return r;
}

bool CpuMockHost::twin_fenced_fetch_pending() const {
	if (!twin_)
		return false;
	for (int i = 0; i < walk_fences_n_; ++i) {
		const WalkFence & w = walk_fences_[i];
		// Only SUSPENDED fences matter: an active walk recognizes its own
		// target inside twin_run_to_pc_inner.
		if (!w.active && w.yield_at_target && twin_->AtPc(w.pc))
			return true;
	}
	return false;
}

// Game-agnostic interrupt-kind predicate: NMI wins the fetch (6510
// priority) and nests over IRQ. An accept is OWNED only while the
// matching dispatch is live (same-source re-pulse / coalesce semantics —
// identical to lock_step_pair's owned test). Anything else standing at or
// inside a boundary must be parked, not stepped over.
bool CpuMockHost::twin_unowned_accept_boundary() const {
	if (!twin_)
		return false;
	const CpuState tw = twin_->GetCpuState();
	if (twin_->InNmiSequence())
		return !nmi_seq_active_;
	if (twin_->InIrqSequence())
		return !irq_seq_active_;
	if (tw.nmi_pending)
		return !nmi_seq_active_;
	return tw.irq_pending && !irq_seq_active_;
}

// Raw sequence-state families (emulated core CPU_emulcycle states):
// IRQ accept = O_IRQ(0x09) + 0x0a..0x0e ($FFFE vector tail); NMI accept =
// O_NMI(0x11) + 0x12..0x14 + shared $FFFA tail 0x15/0x16. The shared tail
// is also where an in-flight IRQ steal is re-vectored when the NMI edge
// arrives during 0x0a–0x0c — hence NMI-family membership always pins NMI,
// while 0x09–0x0c stay UNRESOLVED until the vector decision.
int CpuMockHost::twin_hw_seq_state() const {
	if (!twin_)
		return -1;
	C64 * c64 = twin_->board().Machine();
	if (!c64 || !c64->TheCPU)
		return -1;
	return int(c64->TheCPU->CurrentState());
}

bool CpuMockHost::twin_in_irq_family() const {
	const int st = twin_hw_seq_state();
	return st >= 0x09 && st <= 0x0e;
}

bool CpuMockHost::twin_in_nmi_family() const {
	const int st = twin_hw_seq_state();
	return st >= 0x11 && st <= 0x16;
}

bool CpuMockHost::twin_accept_kind_observed(bool & nmi) const {
	const int st = twin_hw_seq_state();
	if (st < 0)
		return false;
	if ((st >= 0x11 && st <= 0x14) || st == 0x15 || st == 0x16) {
		nmi = true;
		return true;
	}
	if (st == 0x0d || st == 0x0e) {
		nmi = false;
		return true;
	}
	return false; // pre-sequence, or hijack still possible (0x09–0x0c)
}

// Step Twin only (Main stays parked — enter_interrupt burns Main forward
// to the recorded boundary afterwards) until the standing accept's kind
// is observable. A handful of Φ2 in the common case: a plain IRQ needs
// its vector decision executed (up to state 0x0d); a hijacked one shows
// NMI from 0x15; a plain NMI resolves at its first sequence state.
bool CpuMockHost::twin_resolve_accept_kind(bool & nmi) {
	nmi = false;
	if (NoTwin())
		return false;
	resolving_steal_ = twin_->CycleCounter(); // boundary = steal instant
	step_ctx_ = "resolve";
	// Bounded: BA stalls stretch individual cycles but the whole
	// decision spans ≤6 CPU cycles; 512 Φ2 leaves orders of magnitude.
	constexpr int kMaxResolvePhi2 = 512;
	for (int i = 0; i < kMaxResolvePhi2; ++i) {
		if (twin_accept_kind_observed(nmi)) {
			REVM_EVENT("accept", "phase", "resolved", "kind", nmi ? "nmi" : "irq",
			           "boundary_cyc", resolving_steal_);
			return true;
		}
		(void)twin_step_only(); // VBLANK here is handled by callers' checks
	}
	resolving_steal_ = kNoSince;
	SoftQuit(1,
	         "accept kind unresolved after %d Φ2 twin_pc=$%04X seq_state=%d",
	         kMaxResolvePhi2, twin_->Pc(), twin_hw_seq_state());
	return false;
}

// Close the Φ2 gap resolution opened (Twin-only steps) when the accept is
// not dispatched. Raw Main-only advance: Twin is mid-sequence or parked.
void CpuMockHost::twin_repair_resolve_gap() {
	if (NoTwin())
		return;
	REVM_EVENT("accept", "phase", "repair");
	resolving_steal_ = kNoSince;
	if (twin_->CycleCounter() > board_.CycleCounter()) {
		(void)main_advance(
			uint64_t(twin_->CycleCounter() - board_.CycleCounter()),
			/*run_past_vsync=*/true);
	}
}

CpuMockHost::AdvanceResult CpuMockHost::twin_run_to_pc_inner(
	uint16_t pc, bool soft_fail, bool yield_at_target, bool pair_main) {
	ThrowIfQuitRequested();
	step_ctx_ = "walk";
	if (NoTwin()) return AdvanceResult::Ok;

	auto fail = [&](AdvanceResult r) -> AdvanceResult {
		if (soft_fail) {
			REVM_LOG(REVM_VERBOSE, "TwinRunToPc soft-failed want=$%04X twin_pc=$%04X "
				"result=%s frame main=%u twin=%u",
				pc, twin_->Pc(), revm::AdvanceResultName(r),
				board_.FrameCounter(), twin_->FrameCounter());
		} else {
			QuitOnAssert(
				"TwinRunToPc failed want=$%04X twin_pc=$%04X "
				"result=%s frame main=%u twin=%u",
				pc, twin_->Pc(), revm::AdvanceResultName(r),
				board_.FrameCounter(), twin_->FrameCounter());
		}
		return r;
	};

	auto log_hit_vsync = [&](const char * why) {
		const CpuState tw = twin_->GetCpuState();
		REVM_LOG(soft_fail ? REVM_VERBOSE : REVM_ERROR,
		         "TwinRunToPc HitVSync (%s) want=$%04X twin_pc=$%04X "
		         "irq=%d nmi=%d frame main=%u twin=%u cycle main=%u twin=%u",
		         why, pc, twin_->Pc(), int(tw.irq_pending), int(tw.nmi_pending),
		         board_.FrameCounter(), twin_->FrameCounter(),
		         board_.CycleCounter(), twin_->CycleCounter());
	};

	if (HasTwin() && twin_->FrameCounter() > board_.FrameCounter()) {
		// I/O on the VBLANK Φ2: Twin is already at pc_after next frame.
		// ReadIoAtPc skips extra Main VSYNC (would pass the access
		// cycle). yield_at_target is false there — Ok, do not fail.
		// JoinAtPc / RAM still need HitVSync so Main extra-VSYNCs.
		if (!yield_at_target && twin_->AtPc(pc))
			return AdvanceResult::Ok;
		return fail(AdvanceResult::HitVSync);
	}

	const uint32_t frame0 = twin_->FrameCounter();
	constexpr uint64_t kMax = 20000;

	auto yield_accept = [&](bool take_nmi, bool take_irq) -> AdvanceResult {
		if (take_nmi) {
			if (nmi_seq_active_) {
				REVM_LOG(REVM_VERBOSE,
				         "walk owned-swallow nmi main=%u twin=%u",
				         board_.CycleCounter(), twin_->CycleCounter());
				return AdvanceResult::Ok; // own nest in progress
			}
			if (InNmi()) {
				SoftQuit(1,
				         "Twin NMI accept while already in C++ NMI "
				         "want=$%04X twin_pc=$%04X",
				         pc, twin_->Pc());
				return AdvanceResult::Error;
			}
			return AdvanceResult::HitNmi;
		}
		if (take_irq) {
			if (irq_seq_active_) {
				REVM_LOG(REVM_VERBOSE,
				         "walk owned-swallow irq main=%u twin=%u",
				         board_.CycleCounter(), twin_->CycleCounter());
				return AdvanceResult::Ok; // own nest in progress
			}
			if (InIrq())
				return AdvanceResult::LeftNest; // new IRQ after teardown
			return AdvanceResult::HitIrq;
		}
		return AdvanceResult::Ok;
	};

	for (uint64_t i = 0; i < kMax; ++i) {
		// An already-entered sequence (e.g. left parked by a previous
		// walk or pair-step) yields just like a fresh transition —
		// classified from the RESOLVED entered vector, never predicted.
		// An owned nest progresses silently (its own walks drain it).
		if (twin_in_nmi_family() || twin_in_irq_family()) {
			const bool seq_nmi = twin_in_nmi_family();
			const bool owned =
				seq_nmi ? nmi_seq_active_ : irq_seq_active_;
			if (!owned) {
				bool nmi_r = seq_nmi;
				if (!twin_resolve_accept_kind(nmi_r))
					SoftQuit(1,
					         "walk parked accept did not resolve "
					         "want=$%04X",
					         pc);
				const AdvanceResult y = yield_accept(nmi_r, !nmi_r);
				if (y != AdvanceResult::Ok) {
					if (y == AdvanceResult::LeftNest)
						twin_repair_resolve_gap();
					return y;
				}
				twin_repair_resolve_gap(); // flipped-to-owned corner
			}
		}
		if (InNmi() && !nmi_seq_active_ && !twin_->InNmiSequence())
			return AdvanceResult::LeftNest;
		if (InIrq() && !irq_seq_active_ && !twin_->InIrqSequence())
			return AdvanceResult::LeftNest;

		// Fence at O_FETCH of `pc` even during BA stall. AtOpcodeFetch is
		// false while BA is low; EmulateCycle then runs VIC (BA rises) and
		// the CPU fetch in the same Φ2, so the join would miss RTS/STA.
		// If Twin already crossed VBLANK on the way here, HitVSync so the
		// caller can extra-VSYNC Main — do not Ok on the next frame.
		if (twin_->AtPc(pc)) {
			if (twin_->FrameCounter() != frame0)
				return fail(AdvanceResult::HitVSync);
			if (!yield_at_target)
				return AdvanceResult::Ok;
			if (twin_->FetchWouldTakeHw()) {
				// Owned coalesce at the target: zero-step park, exactly
				// like the pre-resolution machinery (the owner consumes
				// this accept natively in its own stepping). An unowned
				// pending fetch is RESOLVED to its entered vector before
				// yielding — the fenced fetch itself is untouched (it is
				// the pushed PC); the retried walk pairs it after the
				// handler.
				const CpuState tw0 = twin_->GetCpuState();
				if (tw0.nmi_pending ? nmi_seq_active_ : irq_seq_active_)
					return AdvanceResult::Ok;
				bool nmi_r = false;
				if (!twin_resolve_accept_kind(nmi_r))
					SoftQuit(1,
					         "fence-target accept did not resolve "
					         "want=$%04X",
					         pc);
				const AdvanceResult y = yield_accept(nmi_r, !nmi_r);
				if (y != AdvanceResult::Ok) {
					// Hit* keeps resolving_steal_ for enter_interrupt;
					// LeftNest has no dispatch to consume it.
					if (y == AdvanceResult::LeftNest)
						twin_repair_resolve_gap();
					return y;
				}
				twin_repair_resolve_gap(); // flipped-to-owned corner
				return AdvanceResult::Ok;
			}
			// Yield-at-target + pending + BA stall: FetchWouldTakeHw is
			// false. An UNOWNED pending accept must nest first — do not
			// Ok; wait for BA rise or steal confirm. An OWNED token still
			// pending-unconfirmed parks: this fence owns the arrival, and
			// stepping across the fetch would let Twin accumulate native
			// program passes before the steal confirms.
			const CpuState tw = twin_->GetCpuState();
			if (!tw.nmi_pending && !tw.irq_pending)
				return AdvanceResult::Ok;
			if ((tw.nmi_pending && nmi_seq_active_) ||
			    (tw.irq_pending && !tw.nmi_pending && irq_seq_active_))
				return AdvanceResult::Ok;
		}

		if (twin_->FetchWouldTakeHw()) {
			// Owned coalesce: zero-step, the owner's own walking consumes
			// this accept. Unowned: resolve to the entered vector, then
			// yield — dispatch never trusts the prediction.
			const CpuState tw0 = twin_->GetCpuState();
			const bool pred_owned =
				tw0.nmi_pending ? nmi_seq_active_ : irq_seq_active_;
			if (!pred_owned) {
				bool nmi_r = false;
				if (!twin_resolve_accept_kind(nmi_r))
					SoftQuit(1, "walk accept did not resolve want=$%04X",
					         pc);
				const AdvanceResult y = yield_accept(nmi_r, !nmi_r);
				if (y != AdvanceResult::Ok) {
					if (y == AdvanceResult::LeftNest)
						twin_repair_resolve_gap();
					return y;
				}
				twin_repair_resolve_gap(); // flipped-to-owned corner
			}
		}

		const bool in_irq0 = twin_in_irq_family();
		const bool in_nmi0 = twin_in_nmi_family();
		const bool hit_vb =
			pair_main ? twin_emulate_cycle() : twin_step_only();
		// Twin leaving this nest beats HitIrq/HitNmi and HitVSync.
		if (InNmi() && !nmi_seq_active_ && !twin_->InNmiSequence())
			return AdvanceResult::LeftNest;
		if (InIrq() && !irq_seq_active_ && !twin_->InIrqSequence())
			return AdvanceResult::LeftNest;
		// Fresh steals (incl. the BA-stall hole): an unowned entry is
		// resolved to its vector decision before yielding — a hijack out
		// of a fresh IRQ begin surfaces as NMI here; an owned nest
		// progresses silently (its hijacks surface via the nested-return
		// paths). Prefer HitIrq/HitNmi over HitVSync when the steal starts
		// on the VBLANK Φ2 — otherwise Twin enters the handler on the
		// next frame without Main pairing.
		const bool fam_nmi1 = twin_in_nmi_family();
		const bool fam_irq1 = twin_in_irq_family();
		if (fam_nmi1 && !in_nmi0) {
			if (!nmi_seq_active_) {
				REVM_LOG(REVM_VERBOSE,
				         "nmi steal confirm EmulateCycle cycle=%u pc=$%04X "
				         "want=$%04X vb=%d",
				         twin_->CycleCounter(), twin_->Pc(), pc, int(hit_vb));
				bool nmi_r = false;
				if (!twin_resolve_accept_kind(nmi_r) || !nmi_r)
					SoftQuit(1,
					         "walk nmi steal confirm did not resolve as NMI "
					         "want=$%04X",
					         pc);
				const AdvanceResult y = yield_accept(true, false);
				if (y != AdvanceResult::Ok)
					return y;
				twin_repair_resolve_gap(); // flipped-to-owned corner
			}
		} else if (fam_irq1 && !in_irq0 && !irq_seq_active_) {
			bool nmi_r = false;
			if (!twin_resolve_accept_kind(nmi_r))
				SoftQuit(1,
				         "walk irq steal confirm did not resolve "
				         "want=$%04X",
				         pc);
			REVM_LOG(REVM_VERBOSE,
			         "%s steal confirm EmulateCycle cycle=%u pc=$%04X "
			         "want=$%04X vb=%d",
			         nmi_r ? "hijacked-nmi" : "irq", twin_->CycleCounter(),
			         twin_->Pc(), pc, int(hit_vb));
			const AdvanceResult y = yield_accept(nmi_r, !nmi_r);
			if (y != AdvanceResult::Ok) {
				if (y == AdvanceResult::LeftNest)
					twin_repair_resolve_gap();
				return y;
			}
			twin_repair_resolve_gap(); // flipped-to-owned corner
		}
		if (hit_vb) {
			log_hit_vsync("EmulateCycle");
			return AdvanceResult::HitVSync;
		}
		if (twin_->FrameCounter() != frame0) {
			log_hit_vsync("frame");
			return AdvanceResult::HitVSync;
		}
	}
	return fail(AdvanceResult::Timeout);
}

CpuMockHost::AdvanceResult CpuMockHost::TwinFinishInstruction() {
	ThrowIfQuitRequested();
	step_ctx_ = "finish";
	if (NoTwin()) return AdvanceResult::Ok;
	if (twin_->AtOpcodeFetch() || twin_in_irq_family() ||
	    twin_in_nmi_family() || twin_->FetchWouldTakeHw())
		return AdvanceResult::Ok;
	// JSR/JMP/RTS would enter the join fence.
	if (twin_op_is_control_flow(twin_current_op(twin_.get())))
		return AdvanceResult::Ok;
	REVM_LOG(REVM_DEBUG, "TwinFinishInstruction pc=$%04X op=$%02X",
	         twin_->Pc(), unsigned(twin_current_op(twin_.get())));
	// Longest official op is 7 Φ2; BA can stretch a badline. Do not pair
	// and do not twin_guard — Twin is often already the next frame.
	constexpr uint64_t kMax = 256;
	for (uint64_t i = 0; i < kMax; ++i) {
		if (twin_emulate_cycle())
			return AdvanceResult::HitVSync;
		if (twin_->AtOpcodeFetch() || twin_in_irq_family() ||
		    twin_in_nmi_family() || twin_->FetchWouldTakeHw())
			return AdvanceResult::Ok;
	}
	return AdvanceResult::Timeout;
}

CpuMockHost::AdvanceResult CpuMockHost::TwinExecuteIrqRti() {
	ThrowIfQuitRequested();
	step_ctx_ = "rti-exec";
	if (NoTwin())
		return AdvanceResult::Ok;
	if (!InIrq() || !irq_seq_active_) {
		SoftQuit(1, "TwinExecuteIrqRti requires active Twin IRQ nest");
	}
	if (!twin_rti_in_progress() &&
	    (!twin_->AtOpcodeFetch() || twin_->Peek(twin_->Pc()) != 0x40)) {
		SoftQuit(1, "TwinExecuteIrqRti requires RTI fetch twin_pc=$%04X",
		         twin_->Pc());
	}

	const uint32_t frame0 = twin_->FrameCounter();
	// RTI is six Φ2; permit BA stretching without introducing another
	// lifecycle latch. This owned walk alone closes the IRQ sequence.
	constexpr uint64_t kMax = 256;
	for (uint64_t i = 0; i < kMax; ++i) {
		const bool in_rti = twin_rti_in_progress();
		const bool hit_vb = twin_emulate_cycle();
		if (in_rti &&
		    (!twin_rti_in_progress() || twin_->InIrqSequence() ||
		     twin_->InNmiSequence())) {
			irq_seq_active_ = false;
			return AdvanceResult::LeftNest;
		}
		if (!irq_seq_active_)
			return AdvanceResult::LeftNest;
		if (hit_vb || twin_->FrameCounter() != frame0)
			return AdvanceResult::HitVSync;
	}
	return AdvanceResult::Timeout;
}

bool CpuMockHost::TwinLastIoRead(TwinIoAccess & out) const {
	if (NoTwin())
		return false;
	return twin_->LastIoRead(out);
}

bool CpuMockHost::TwinLastIoWrite(TwinIoAccess & out) const {
	if (NoTwin())
		return false;
	return twin_->LastIoWrite(out);
}

void CpuMockHost::main_run_to_io_cycle(uint32_t cycle) {
	ThrowIfQuitRequested();
	const uint32_t mn = board_.CycleCounter();
	if (mn > cycle) {
		REVM_LOG(REVM_ERROR,
		         "MainInjectIo FATAL — Main ahead "
		         "(main=%u access=%u frame main=%u twin=%u)",
		         mn, cycle, board_.FrameCounter(),
		         twin_ ? twin_->FrameCounter() : 0);
		SoftQuit(1, "MainInjectIo Main ahead main=%u access=%u", mn, cycle);
	}
	if (mn == cycle)
		return;

	const AdvanceResult r =
		main_advance(uint64_t(cycle - mn), /*run_past_vsync=*/true);
	if (r != AdvanceResult::Ok || board_.CycleCounter() != cycle) {
		REVM_LOG(REVM_ERROR,
		         "MainInjectIo FATAL — catch-up failed "
		         "(main=%u want=%u result=%s)",
		         board_.CycleCounter(), cycle, revm::AdvanceResultName(r));
		SoftQuit(1, "MainInjectIo catch-up failed");
	}
}

void CpuMockHost::main_emulate_inject_cycle(
	const std::function<void()> & cpu_slot) {
	C64 * c64 = board_.Machine();
	if (!c64)
		SoftQuit(1, "MainInjectIo missing machine");
	ThrowIfQuitRequested();
	if (c64->QuitRequested())
		throw SoftQuitException{board_.ExitCode()};
	const bool vb = board_.EmulateCycleInjectCpu(cpu_slot);
	if (vb) {
		pace_after_vsync();
		if (c64->QuitRequested())
			throw SoftQuitException{board_.ExitCode()};
	}
}

uint8_t CpuMockHost::MainInjectReadAtCycle(uint32_t cycle, uint16_t addr) {
	if (NoTwin())
		return Read(addr);
	main_run_to_io_cycle(cycle);
	uint8_t v = 0;
	main_emulate_inject_cycle([&] { v = Read(addr); });
	return v;
}

void CpuMockHost::MainInjectWriteAtCycle(uint32_t cycle, uint16_t addr,
                                         uint8_t value) {
	if (NoTwin()) {
		Write(addr, value);
		return;
	}
	main_run_to_io_cycle(cycle);
	main_emulate_inject_cycle([&] { Write(addr, value); });
}

CpuMockHost::AdvanceResult CpuMockHost::MainRunToTwinCycle() {
	ThrowIfQuitRequested();
	if (NoTwin())
		return AdvanceResult::Ok;

	const uint32_t main_f = board_.FrameCounter();
	const uint32_t twin_f = twin_->FrameCounter();
	if (twin_f > main_f)
		return AdvanceResult::Error;
	if (twin_f < main_f) {
		REVM_LOG(REVM_ERROR,
		         "MainRunToTwinCycle FATAL — Twin behind Main "
		         "(main_frame=%u twin_frame=%u)",
		         main_f, twin_f);
		Quit(1);
	}

	const uint32_t tw = twin_->CycleCounter();
	const uint32_t mn = board_.CycleCounter();
	if (mn > tw)
		return AdvanceResult::Error;
	if (mn == tw)
		return AdvanceResult::Ok;

	const AdvanceResult r =
		main_advance(uint64_t(tw - mn), /*run_past_vsync=*/false);
	if (r == AdvanceResult::HitIrq || r == AdvanceResult::HitNmi)
		return r;
	if (r == AdvanceResult::HitVSync) {
		REVM_LOG(REVM_ERROR,
		         "MainRunToTwinCycle FATAL — hit VSYNC during same-frame lock "
		         "(main_frame=%u twin_frame=%u main_cyc=%u twin_cyc=%u)",
		         main_f, twin_f, board_.CycleCounter(), tw);
		Quit(1);
	}
	if (r != AdvanceResult::Ok ||
	    board_.CycleCounter() != twin_->CycleCounter()) {
		REVM_LOG(REVM_ERROR,
		         "MainRunToTwinCycle FATAL — cycle mismatch after run "
		         "(main_cyc=%u twin_cyc=%u result=%s)",
		         board_.CycleCounter(), twin_->CycleCounter(),
		         revm::AdvanceResultName(r));
		Quit(1);
	}
	return AdvanceResult::Ok;
}

bool CpuMockHost::MainTakeTwinPhaseCia1() { return main_take_twin_phase_cia(1); }

bool CpuMockHost::MainTakeTwinPhaseCia2() { return main_take_twin_phase_cia(2); }

bool CpuMockHost::main_take_twin_phase_cia(int which) {
	if (NoTwin()) return true;

	const uint32_t mn_cyc = board_.CycleCounter();
	const uint32_t tw_cyc = twin_->CycleCounter();
	if (mn_cyc != tw_cyc) {
		QuitOnAssert(
			"MainTakeTwinPhaseCia%d FATAL — cycle mismatch "
			"(main=%u twin=%u frame main=%u twin=%u)",
			which, mn_cyc, tw_cyc, board_.FrameCounter(), twin_->FrameCounter());
		return false;
	}

	C64 * main_c64 = board_.Machine();
	C64 * twin_c64 = twin_->board().Machine();
	MOS6526 * main_cia =
		which == 1
			? static_cast<MOS6526 *>(main_c64 ? main_c64->TheCIA1 : nullptr)
			: static_cast<MOS6526 *>(main_c64 ? main_c64->TheCIA2 : nullptr);
	MOS6526 * twin_cia =
		which == 1
			? static_cast<MOS6526 *>(twin_c64 ? twin_c64->TheCIA1 : nullptr)
			: static_cast<MOS6526 *>(twin_c64 ? twin_c64->TheCIA2 : nullptr);
	if (!main_cia || !twin_cia) {
		QuitOnAssert("MainTakeTwinPhaseCia%d missing chip", which);
		return false;
	}

	MOS6526State tw{}, mn{};
	twin_cia->GetState(&tw);
	main_cia->GetState(&mn);
	if (!cia_config_equal(tw, mn)) {
		REVM_LOG(REVM_ERROR, "MainTakeTwinPhaseCia%d FATAL — config mismatch "
			"(cycle=%u frame main=%u twin=%u)",
			which, mn_cyc, board_.FrameCounter(), twin_->FrameCounter());
		char label[8];
		std::snprintf(label, sizeof(label), "CIA%d", which);
		print_cia_config_diff(log::Stream(), label, tw, mn);
		QuitOnAssert("MainTakeTwinPhaseCia%d FATAL — config mismatch",
		         which);
		return false;
	}

	const bool ta_idle = twin_cia->TimerAIdle();
	const bool tb_idle = twin_cia->TimerBIdle();
	main_cia->SetState(&tw);
	main_cia->SetTimerIdle(ta_idle, tb_idle);
	if (which == 1 && main_c64 && main_c64->TheCIA1)
		main_c64->TheCIA1->SyncLightpenEdge();
	return true;
}

CpuMockHost::AdvanceResult CpuMockHost::AdvanceCycles(
	uint64_t cycles, VSyncPolicy vsync_policy) {
	if (HasTwin())
		SoftQuit(1, "CpuMockHost::AdvanceCycles is Main-only; Twin waits stay on Sync");
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
					SoftQuit(1,
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

CpuMockHost::AdvanceResult CpuMockHost::AdvanceToVSync() {
	if (HasTwin())
		SoftQuit(1, "CpuMockHost::AdvanceToVSync is Main-only; Twin waits stay on Sync");
	int retries = 0;
	for (;;) {
		const AdvanceResult r = main_advance_to_vsync();
		if (r != AdvanceResult::HitNmi && r != AdvanceResult::HitIrq)
			return r;
		if (++retries > 128) {
			SoftQuit(1, "Main dispatch: too many HitIrq/HitNmi retries (%d)",
			         retries);
			return AdvanceResult::Error;
		}
		if (r == AdvanceResult::HitNmi)
			RunNmiHandler();
		else
			RunIrqHandler();
	}
}

uint32_t CpuMockHost::AdvanceFrames(uint32_t frames) {
	if (HasTwin())
		SoftQuit(1, "CpuMockHost::AdvanceFrames is Main-only; Twin waits stay on Sync");
	for (uint32_t i = 0; i < frames; ++i) {
		const AdvanceResult r = AdvanceToVSync();
		if (r != AdvanceResult::HitVSync)
			SoftQuit(1, "AdvanceFrames VSYNC miss n=%u of %u result=%s",
			         i, frames, revm::AdvanceResultName(r));
	}
	return frames;
}

void CpuMockHost::ReturnIrq(uint32_t body_cycles) {
	if (HasTwin())
		SoftQuit(1, "CpuMockHost::ReturnIrq is Main-only; Twin IRQ teardown stays on Sync");
	if (!InIrq())
		SoftQuit(1, "ReturnIrq requires InIrq");
	FinishMockIrq(body_cycles);
}

void CpuMockHost::ReturnNmi(uint32_t cycles) {
	if (HasTwin())
		SoftQuit(1, "CpuMockHost::ReturnNmi is Main-only; Twin NMI teardown stays on Sync");
	if (!InNmi())
		SoftQuit(1, "ReturnNmi requires InNmi");
	FinishMockNmi(cycles);
}

CpuMockHost::AdvanceResult CpuMockHost::main_advance(uint64_t cycles,
                                                     bool run_past_vsync) {
	if (cycles == 0) return AdvanceResult::Ok;
	C64 * c64 = board_.Machine();
	if (!c64) return AdvanceResult::Stopped;
	ThrowIfQuitRequested();
	if (cycles > 256 && HasTwin())
		REVM_LOG(REVM_VERBOSE,
		         "main_advance big n=%llu main=%u twin=%u frame=%u",
		         static_cast<unsigned long long>(cycles),
		         board_.CycleCounter(),
		         twin_ ? twin_->CycleCounter() : 0, board_.FrameCounter());

	// Raw Main-only Φ2 advance. Twin is NOT touched (inject catch-up keeps
	// Twin parked; Sync::AdvanceCycles locksteps instead). With Twin, accepts are
	// never dispatched here — the line edges are recorded and enter_interrupt
	// takes the skew verdict at Twin's steal. --no-twin yields Hit* from
	// Main's own chips (chip-driven dispatch without an oracle).
	for (uint64_t i = 0; i < cycles; ++i) {
		if (c64->QuitRequested()) {
			throw SoftQuitException{board_.ExitCode()};
		}
		if (NoTwin()) {
			MOS6510 * hw = c64->TheCPU;
			if (hw->CpuMockNmiPending() && nmi_depth_ == 0)
				return AdvanceResult::HitNmi;
			if (hw->CpuMockIrqPending() && !hw->CpuMockIFlag() &&
			    irq_depth_ == 0 && !IrqHoldActive())
				return AdvanceResult::HitIrq;
		}
		track_main_lines();
		const bool vb = board_.EmulateCycle();
		track_main_lines();
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

CpuMockHost::AdvanceResult CpuMockHost::main_advance_to_vsync() {
	C64 * c64 = board_.Machine();
	if (!c64) return AdvanceResult::Stopped;
	ThrowIfQuitRequested();

	constexpr uint64_t kMax = 20'000ull * 400;
	for (uint64_t i = 0; i < kMax; ++i) {
		if (QuitRequested()) {
			throw SoftQuitException{board_.ExitCode()};
		}
		track_main_lines();
		if (board_.EmulateCycle()) {
			pace_after_vsync();
			ThrowIfQuitRequested();
			return AdvanceResult::HitVSync;
		}
		track_main_lines();
		if (NoTwin()) {
			// Chip-driven dispatch without an oracle.
			MOS6510 * hw = c64->TheCPU;
			if (hw->CpuMockNmiPending() && nmi_depth_ == 0)
				return AdvanceResult::HitNmi;
			if (hw->CpuMockIrqPending() && !hw->CpuMockIFlag() &&
			    irq_depth_ == 0 && !IrqHoldActive())
				return AdvanceResult::HitIrq;
		}
	}
	REVM_LOG(REVM_ERROR, "AdvanceToVSync: timeout without VSYNC");
	return AdvanceResult::Timeout;
}

bool CpuMockHost::ignore_asserts() const {
	return compare_opts_.ignore_asserts;
}

bool CpuMockHost::ignore_checks() const {
	return compare_opts_.ignore_checks;
}

void CpuMockHost::QuitOnAssert(const char * fmt, ...) {
	char msg[1536];
	std::va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(msg, sizeof msg, fmt ? fmt : "", ap);
	va_end(ap);
	REVM_LOG(REVM_ERROR, "frame %u: %s", board_.FrameCounter(), msg);
	if (ignore_asserts()) {
		REVM_LOG(REVM_ERROR, "assert ignored (--ignore-asserts)");
		return;
	}
	event::EmitSoftquit(1, msg);
	board_.RequestQuit(1);
	throw SoftQuitException{1};
}

void CpuMockHost::maybe_fail_hint() {
	static bool hinted = false;
	if (hinted)
		return;
	hinted = true;
	REVM_LOG(REVM_ERROR,
	         "hint: rerun with --events - --report /tmp/s3.json for the "
	         "structured fence/accept trail");
	// Repro conventions (frame-bounded replay): fence misses and join
	// compares raised during frame N are shown by --max-frames N.
	// Screen-check fails report at frame N but reproduce with --max-frames
	// N+1 — the compare runs inside frame N, so a run capped at N quits
	// before it (MEASURED on the Paradroid play-chain net, 333/333 rows;
	// the screen branch below prints that form for screen fails).
	REVM_LOG(REVM_ERROR,
	         "hint: this failure reports at frame %u; fence fails reproduce "
	         "with --max-frames %u, screen-check fails with --max-frames %u",
	         board_.FrameCounter(), board_.FrameCounter(),
	         board_.FrameCounter() + 1);
}

void CpuMockHost::QuitOnCheck(const char * fmt, ...) {
	char msg[1536];
	std::va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(msg, sizeof msg, fmt ? fmt : "", ap);
	va_end(ap);
	REVM_LOG(REVM_ERROR, "frame %u: %s", board_.FrameCounter(), msg);
	maybe_fail_hint();
	event::EmitSoftquit(1, msg);
	if (ignore_checks()) {
		REVM_LOG(REVM_ERROR, "check ignored (--ignore-checks)");
		return;
	}
	board_.RequestQuit(1);
	throw SoftQuitException{1};
}

void CpuMockHost::SoftQuit(int code, const char * fmt, ...) {
	char msg[1536];
	std::va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(msg, sizeof msg, fmt ? fmt : "", ap);
	va_end(ap);
	REVM_LOG(REVM_ERROR, "frame %u: %s", board_.FrameCounter(), msg);
	maybe_fail_hint();
	event::EmitSoftquit(code, msg);
	board_.RequestQuit(code);
	throw SoftQuitException{code};
}

void CpuMockHost::ThrowIfQuitRequested() {
	if (QuitRequested())
		throw SoftQuitException{board_.ExitCode()};
}

bool CpuMockHost::QuitRequested() const {
	if (embedded_.quit_probe && embedded_.quit_probe()) return true;
	const C64 * c64 = board_.Machine();
	return c64 && c64->QuitRequested();
}

uint8_t CpuMockHost::TwinPeekMem(uint16_t addr) {
	if (NoTwin()) {
		REVM_LOG(REVM_ERROR, "TwinPeekMem($%04X) requires twin (--no-twin)", addr);
		Quit(1);
	}
	return twin_->Peek(addr);
}

CpuState CpuMockHost::TwinCpu() {
	if (NoTwin()) {
		REVM_LOG(REVM_ERROR, "TwinCpu() requires twin (--no-twin)");
		Quit(1);
	}
	return twin_->GetCpuState();
}

uint8_t CpuMockHost::TwinPeek(uint16_t addr) {
	if (NoTwin()) {
		REVM_LOG(REVM_ERROR, "TwinPeek() requires twin (--no-twin)");
		Quit(1);
	}
	return TwinPeekMem(addr);
}

uint8_t CpuMockHost::PeekMain(uint16_t addr) { return Read(addr); }

uint64_t CpuMockHost::TwinWatchHits(uint16_t pc) {
	if (NoTwin())
		return 0;
	return twin_->board().WatchHits(pc);
}

bool CpuMockHost::TwinAtOpcodeFetch() const {
	if (NoTwin())
		return false;
	return twin_->AtOpcodeFetch();
}

void CpuMockHost::Quit(int code) {
	// Unconditional stop (unlike QuitOnCheck/QuitOnAssert): unwind through the
	// SoftQuitException door so tallies, event flush, and --report all run.
	// Only the max-seconds watchdog escalates to _Exit after its grace period.
	if (code != 0) {
		REVM_LOG(REVM_ERROR, "frame %u: Quit(%d)",
		         board_.FrameCounter(), code);
	} else {
		REVM_LOG(REVM_DEBUG, "Quit(0)");
	}
	event::EmitSoftquit(code, "Quit()");
	board_.RequestQuit(code);
	throw SoftQuitException{code};
}

void CpuMockHost::WatchTwinPc(uint16_t pc, const char * label) {
	if (NoTwin()) return;
	// Idempotent: same pc does not push a second watch. A non-empty label
	// refreshes the existing entry; WatchMark's empty label keeps it.
	twin_->WatchPc(pc, label ? label : "");
}

void CpuMockHost::ClearTwinPcWatches() {
	if (NoTwin()) return;
	twin_->ClearPcWatches();
}

bool CpuMockHost::AssertTwinMem(uint16_t addr, uint8_t expected) {
	if (NoTwin()) return true;
	const uint8_t got = twin_->Peek(addr);
	if (got == expected) return true;
	QuitOnAssert("twin assert FAIL $%04X: expected=$%02X actual=$%02X", addr,
	         expected, got);
	return false;
}

bool CpuMockHost::AssertTwinMemRange(uint16_t lo, uint16_t hi,
                                     const uint8_t * expected) {
	if (NoTwin()) return true;
	if (!expected || hi < lo) return false;
	bool ok = true;
	for (uint32_t a = lo; a <= hi; ++a) {
		const uint8_t got = twin_->Peek(uint16_t(a));
		const uint8_t exp = expected[a - lo];
		if (got != exp) {
			REVM_LOG(REVM_ERROR, "twin assert FAIL $%04X: expected=$%02X actual=$%02X",
				unsigned(a), exp, got);
			ok = false;
		}
	}
	if (!ok) QuitOnAssert("twin assert FAIL range $%04X-$%04X", lo, hi);
	return ok;
}

void CpuMockHost::AssertBegin(uint16_t pc) {
	if (board_.GetConfig().main_blank) {
		if (!main_start_) {
			QuitOnAssert("--main-blank requires InstallMainStart");
		} else if (main_start_->entry_pc != pc) {
			QuitOnAssert(
				"Main start PC mismatch: installed=$%04X asserted=$%04X",
				main_start_->entry_pc, pc);
		}
	}
	(void)AssertTwinPc(pc);
	(void)AssertVsTwinCycleSync();
}

bool CpuMockHost::AssertTwinPc(uint16_t pc) {
	if (NoTwin()) return true;
	const uint16_t got = twin_->Pc();
	if (got == pc) return true;
	QuitOnAssert("twin assert FAIL pc: expected=$%04X actual=$%04X", pc, got);
	return false;
}

bool CpuMockHost::AssertVsTwinCycleSync() {
	if (NoTwin()) return true;
	const uint32_t mn = board_.CycleCounter();
	const uint32_t tw = twin_->CycleCounter();
	if (mn == tw) return true;
	QuitOnAssert(
		"twin assert FAIL cycle sync: main=%u twin=%u "
		"(frame main=%u twin=%u)",
		mn, tw, board_.FrameCounter(), twin_->FrameCounter());
	return false;
}

bool CpuMockHost::AssertKbCheck() {
	if (NoTwin()) return true;
	if (!kb_check_.Active()) return true;
	const uint32_t frame = board_.FrameCounter();
	const uint32_t cycle = board_.CycleCounter();
	if (kb_check_.OnFrame(board_, *twin_, frame, cycle, true,
	                     &linked_registry_))
		return true;
	QuitOnAssert("kb-check assert FAIL frame %u cycle %u — aborting", frame,
	             cycle);
	return false;
}

bool CpuMockHost::AssertVsTwin(uint16_t addr) {
	if (NoTwin()) return true;
	const uint8_t main_v = Read(addr);
	const uint8_t twin_v = twin_->Peek(addr);
	if (main_v == twin_v) return true;
	QuitOnAssert("vs-twin FAIL $%04X: main=$%02X twin=$%02X", addr, main_v, twin_v);
	return false;
}

bool CpuMockHost::AssertVsTwinRange(uint16_t lo, uint16_t hi) {
	if (NoTwin()) return true;
	if (hi < lo) return false;
	bool ok = true;
	for (uint32_t a = lo; a <= hi; ++a) {
		const uint8_t main_v = Read(uint16_t(a));
		const uint8_t twin_v = twin_->Peek(uint16_t(a));
		if (main_v != twin_v) {
			REVM_LOG(REVM_ERROR, "vs-twin FAIL $%04X: main=$%02X twin=$%02X",
			               unsigned(a), main_v, twin_v);
			ok = false;
		}
	}
	if (!ok) QuitOnAssert("vs-twin FAIL range $%04X-$%04X", lo, hi);
	return ok;
}

// IRQ-relevant chip state, side-by-side. VIC: raster compare + current
// raster (low 8 bits + RST8), IRF/IMR; CIA1/2: pending ICR, mask, timer
// counters and latches, control registers.
bool CpuMockHost::LogVsTwinIrqSources(const char * why) {
	ChipSnapshot mn{}, tw{};
	if (!CaptureChipSnapshot(board_, mn) || !twin_->CaptureChips(tw)) {
		REVM_LOG(REVM_ERROR,
		         "irq-sources %s: chip capture failed", why ? why : "");
		return false;
	}
	MOS6510 * cpu = board_.Machine() ? board_.Machine()->TheCPU : nullptr;
	if (cpu)
		REVM_LOG(REVM_ERROR,
		         "irq-sources %s cpu: main_i=%d hold=%d irq_pend=%d "
		         "nmi_pend=%d",
		         why ? why : "", int(cpu->CpuMockIFlag()),
		         int(cpu->CpuMockIrqHoldActive()),
		         int(cpu->CpuMockIrqPending()),
		         int(cpu->CpuMockNmiPending()));
	auto u8 = [&](const char * name, uint8_t t, uint8_t m) {
		REVM_LOG(REVM_ERROR, "irq-sources %s %s: twin=$%02X main=$%02X%s",
		         why ? why : "", name, t, m, t == m ? "" : "  <-- DIFF");
		return t == m;
	};
	auto u16 = [&](const char * name, uint16_t t, uint16_t m) {
		REVM_LOG(REVM_ERROR,
		         "irq-sources %s %s: twin=$%04X main=$%04X%s",
		         why ? why : "", name, t, m, t == m ? "" : "  <-- DIFF");
		return t == m;
	};
	bool all_ok = true;
	const MOS6569State & v_t = tw.vic;
	const MOS6569State & v_m = mn.vic;
	all_ok &= u16("vic.raster_cmp", v_t.irq_raster,
	              uint16_t(v_m.irq_raster));
	all_ok &= u8("vic.raster_lo", v_t.raster, v_m.raster);
	all_ok &= u8("vic.ctrl1_rst8", uint8_t(v_t.ctrl1 & 0x80),
	             uint8_t(v_m.ctrl1 & 0x80));
	all_ok &= u8("vic.irq_flag", v_t.irq_flag, v_m.irq_flag);
	all_ok &= u8("vic.irq_mask", v_t.irq_mask, v_m.irq_mask);
	for (int c = 0; c < 2; ++c) {
		const MOS6526State & c_t = c == 0 ? tw.cia1 : tw.cia2;
		const MOS6526State & c_m = c == 0 ? mn.cia1 : mn.cia2;
		char label[8];
		std::snprintf(label, sizeof label, "cia%d", c + 1);
		char name[24];
		std::snprintf(name, sizeof name, "%s.int_flags", label);
		all_ok &= u8(name, c_t.int_flags, c_m.int_flags);
		std::snprintf(name, sizeof name, "%s.int_mask", label);
		all_ok &= u8(name, c_t.int_mask, c_m.int_mask);
		std::snprintf(name, sizeof name, "%s.ta_counter", label);
		all_ok &= u16(name, uint16_t((uint16_t(c_t.ta_hi) << 8) | c_t.ta_lo),
		              uint16_t((uint16_t(c_m.ta_hi) << 8) | c_m.ta_lo));
		std::snprintf(name, sizeof name, "%s.tb_counter", label);
		all_ok &= u16(name, uint16_t((uint16_t(c_t.tb_hi) << 8) | c_t.tb_lo),
		              uint16_t((uint16_t(c_m.tb_hi) << 8) | c_m.tb_lo));
		std::snprintf(name, sizeof name, "%s.ta_latch", label);
		all_ok &= u16(name, c_t.ta_latch, c_m.ta_latch);
		std::snprintf(name, sizeof name, "%s.tb_latch", label);
		all_ok &= u16(name, c_t.tb_latch, c_m.tb_latch);
		std::snprintf(name, sizeof name, "%s.cra", label);
		all_ok &= u8(name, c_t.cra, c_m.cra);
		std::snprintf(name, sizeof name, "%s.crb", label);
		all_ok &= u8(name, c_t.crb, c_m.crb);
	}
	return all_ok;
}

bool CpuMockHost::AssertVsTwinIrqSources() {
	if (NoTwin()) return true;
	const bool ok = LogVsTwinIrqSources("assert");
	if (!ok)
		QuitOnAssert("irq-sources assert FAIL");
	return ok;
}

bool CpuMockHost::handoff_from_begin_snap(GoldenInput * golden_input) {
	const std::string & path = begin_snap_path();
	if (board_.GetConfig().main_blank) {
		if (HasTwin() && path.empty()) {
			REVM_LOG(REVM_ERROR,
			         "--load-snapshot required (Twin needs BEGIN FullSnapshot)");
			return false;
		}
		return start_blank_main(golden_input);
	}
	if (path.empty()) {
		REVM_LOG(REVM_ERROR, "--load-snapshot required (Stage 3 never boots the 6510)");
		return false;
	}

	std::string error;
	if (!board_.LoadBeginSnap(path, golden_input, error)) {
		REVM_LOG(REVM_ERROR, "cannot load begin snap %s: %s\n"
			"Create it with Stage-1:\n"
			"./bin/revm --load-prg <game.prg> --use-play work/<Game>/plays/<name>.json \\\n"
			"    --headless --save-snapshot <begin_cycle> %s --max-cycles <begin+64>",
			path.c_str(), error.c_str(), path.c_str());
		return false;
	}

	enable_mock_cpu();
	return true;
}

bool CpuMockHost::start_blank_main(GoldenInput * golden_input) {
	if (!main_start_) {
		REVM_LOG(REVM_ERROR,
		         "--main-blank requires InstallMainStart(...) in InstallGame");
		return false;
	}
	const CpuMockStart & start = *main_start_;

	if (HasTwin()) {
		if (twin_->Pc() != start.entry_pc ||
		    twin_->CycleCounter() != start.cycle ||
		    twin_->FrameCounter() != start.frame) {
			REVM_LOG(REVM_ERROR,
			         "installed Main start does not match Twin BEGIN: "
			         "main pc=$%04X cycle=%u frame=%u; "
			         "twin pc=$%04X cycle=%u frame=%u",
			         start.entry_pc, start.cycle, start.frame, twin_->Pc(),
			         twin_->CycleCounter(), twin_->FrameCounter());
			return false;
		}
	}
	if (!playback_path_.empty()) {
		const PlayLog & play = playback_player_.Log();
		if ((play.start_cycle != 0 && play.start_cycle != start.cycle) ||
		    play.start_frame != start.frame) {
			REVM_LOG(REVM_ERROR,
			         "installed Main start does not match play origin: "
			         "main cycle=%u frame=%u; play cycle=%llu frame=%u",
			         start.cycle, start.frame,
			         static_cast<unsigned long long>(play.start_cycle),
			         play.start_frame);
			return false;
		}
	}

	C64 * c64 = board_.Machine();
	if (!c64 || !c64->TheCPU) {
		REVM_LOG(REVM_ERROR, "--main-blank Main machine is not initialized");
		return false;
	}

	// Main receives nothing from BEGIN: start with zero mutable memory and the
	// canonical cycle-zero machine state (the same PrepareRun path covered by
	// the boot-snapshot hash self-test). Advance only to the declared PAL phase
	// so its raster clock shares Twin's timeline coordinate; the mock CPU fetches
	// no ROM or game instructions during this phase alignment.
	board_.PrepareRun();
	std::memset(c64->RAM, 0, C64_RAM_SIZE);
	std::memset(c64->Color, 0, COLOR_RAM_SIZE);
	enable_mock_cpu();
	const uint32_t phase_cycles = start.cycle % kCyclesPerFrame;
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
	board_.SyncClockFromMachine();
	apply_declared_cia_live(c64, start);

	if (golden_input) {
		golden_input->SeekTo(start.frame);
		board_.ApplyInput(golden_input->PollFrame(start.frame));
	} else {
		board_.ApplyInput(InputFrame{});
	}

	REVM_LOG(REVM_DEBUG,
	         "Main blank start — fresh chips, zero RAM/color, "
	         "pc=$%04X cycle=%u frame=%u phase=%u cia1_live=%d cia2_live=%d",
	         start.entry_pc, start.cycle, start.frame, phase_cycles,
	         start.cia1.has_value() ? 1 : 0, start.cia2.has_value() ? 1 : 0);
	return true;
}

void CpuMockHost::wire_defaults() {
	linked_registry_.Clear();
	board_.SetMemoryModificationHandler(
		[this](const MemoryModification &modification) {
			return linked_registry_.Apply(modification);
		});
	if (embedded_.own_clock_source) {
		log::SetClockSource([this] {
			auto fill = [](const Board & b) {
				log::BoardClock c;
				c.frame = b.FrameCounter();
				c.cycle = b.CycleCounter();
				if (b.HasLastVBlank()) {
					c.frame_start = b.LastVBlankCycle();
					c.frame_end = c.frame_start + b.VBlankPeriod();
				}
				return c;
			};
			log::Clocks out;
			out.main = fill(board_);
			if (HasTwin()) {
				out.twin = fill(twin_->board());
				out.twin_valid = true;
			}
			return out;
		});
	}
	board_.SetFrameBoundaryHook(
		[this](uint32_t frame, uint32_t cycle, const InputFrame & in) {
			on_main_vsync(frame, cycle, in);
		});
}

int CpuMockHost::RunPlayback(const std::string & play_path, uint32_t max_frames,
                            bool headless, bool limit_speed, bool audio,
                            const PlaybackCompareOpts & compare,
                            const Config * user_cfg) {
	if (!PreparePlayback(play_path, max_frames, headless, limit_speed, audio,
	                     compare, user_cfg))
		return 1;
	return RunPreparedPlayback();
}

bool CpuMockHost::PreparePlayback(const std::string & play_path, uint32_t max_frames,
                                 bool headless, bool limit_speed, bool audio,
                                 const PlaybackCompareOpts & compare,
                                 const Config * user_cfg,
                                 EmbeddedRunOptions embedded) {
	playback_prepared_ = false;
	prepared_live_ = false;
	embedded_ = std::move(embedded);
	playback_path_ = play_path;
	playback_max_frames_ = max_frames;
	std::string error;
	if (!playback_player_.Load(play_path, error)) {
		REVM_LOG(REVM_ERROR, "Failed to load play: %s", error.c_str());
		return false;
	}
	PlaybackCompareOpts cmp = compare;
	playback_player_.SetCompareOpts(cmp);
	compare_opts_ = cmp;
	revm::WarnPlayUsage(playback_player_.Log(), cmp.no_twin);

	Config cfg;
	cfg.stage = Stage::Stage3;
	cfg.run_mode = RunMode::Playback;
	cfg.prg_path.clear();
	cfg.headless = headless;
	cfg.audio_enabled = audio;
	cfg.limit_speed = limit_speed;
	cfg.play_path = play_path;
	apply_user_cfg(cfg, user_cfg);
	// A recording owns the deterministic machine seed. Keep this invariant in
	// CpuMockHost rather than relying on a particular CLI caller to apply it.
	cfg.rand_seed = playback_player_.Log().rand_seed;
	if (cfg.begin_snap_path.empty() && !(cfg.main_blank && cmp.no_twin)) {
		REVM_LOG(REVM_ERROR, "Stage 3 --use-play requires --load-snapshot (BEGIN FullSnapshot)");
		return false;
	}

	board_.Configure(cfg);
	if (!board_.InitAs(embedded_.board_init_role, error)) {
		REVM_LOG(REVM_ERROR, "Init failed: %s", error.c_str());
		return false;
	}

	board_.SetInputSource(&playback_player_.Input());
	board_.SetPlayPlayer(&playback_player_);
	wire_defaults();
	if (cmp.no_twin) {
		REVM_LOG(REVM_DEBUG, "twin OFF (--no-twin)");
	} else if (!setup_twin()) {
		return false;
	} else {
		board_.SetSnapshotSource(&twin_->board());
		board_.SetMemoryModificationMirror(&twin_->board());
	}
	setup_kb_check();
	main_start_.reset();
	InstallGame(*this);

	if (cmp.no_twin || cmp.ignore_checks || cmp.ignore_asserts ||
	    cmp.ignore_play_hashes) {
		REVM_LOG(REVM_DEBUG, "ignores:%s%s%s%s",
			cmp.no_twin ? " no-twin" : "",
			cmp.ignore_checks ? " checks" : "",
			cmp.ignore_asserts ? " asserts" : "",
			cmp.ignore_play_hashes ? " play-hashes" : "");
	} else {
		REVM_LOG(REVM_DEBUG, "Stage 3 — Twin snapshot hashes; Main↔Twin compares at joins");
	}

	if (max_frames == 0 &&
	    embedded_.board_init_role != BoardInitRole::SecondaryRunner) {
		if (!PlayEndFrame(play_path, max_frames, error)) {
			REVM_LOG(REVM_ERROR, "Failed to find play end: %s", error.c_str());
			return false;
		}
		playback_max_frames_ = max_frames;
	}
	if (max_frames) {
		board_.SetFrameCallback([this, max_frames](uint32_t frame, uint32_t) {
			if (frame >= max_frames) board_.RequestQuit(0);
		});
	}

	REVM_LOG(REVM_DEBUG, "--use-play %s — snap=%s (single-thread)",
	              play_path.c_str(), begin_snap_path().c_str());

	if (!handoff_from_begin_snap(&playback_player_.Input())) {
		return false;
	}

	pace_ = limit_speed;
	frame_start_ = std::chrono::steady_clock::now();
	playback_prepared_ = true;
	return true;
}

bool CpuMockHost::PrepareLive(IInputSource * input, uint32_t max_frames,
                             bool headless, bool limit_speed, bool audio,
                             const PlaybackCompareOpts & compare,
                             const Config * user_cfg,
                             EmbeddedRunOptions embedded) {
	playback_prepared_ = false;
	prepared_live_ = false;
	embedded_ = std::move(embedded);
	playback_path_.clear();
	playback_max_frames_ = max_frames;
	compare_opts_ = compare;

	Config cfg;
	cfg.stage = Stage::Stage3;
	cfg.run_mode = RunMode::Play;
	cfg.prg_path.clear();
	cfg.headless = headless;
	cfg.audio_enabled = audio;
	cfg.limit_speed = limit_speed;
	apply_user_cfg(cfg, user_cfg);
	if (cfg.begin_snap_path.empty() &&
	    !(cfg.main_blank && compare_opts_.no_twin)) {
		REVM_LOG(REVM_ERROR, "Stage 3 live requires --load-snapshot");
		return false;
	}

	std::string error;
	board_.Configure(cfg);
	if (!board_.InitAs(embedded_.board_init_role, error)) {
		REVM_LOG(REVM_ERROR, "Init failed: %s", error.c_str());
		return false;
	}

	if (input)
		board_.SetInputSource(input);
	wire_defaults();
	if (compare_opts_.no_twin) {
		REVM_LOG(REVM_DEBUG, "twin OFF (--no-twin)");
	} else if (!setup_twin()) {
		return false;
	} else {
		board_.SetMemoryModificationMirror(&twin_->board());
	}
	setup_kb_check();
	main_start_.reset();
	InstallGame(*this);

	if (max_frames) {
		board_.SetFrameCallback([this, max_frames](uint32_t frame, uint32_t) {
			if (frame >= max_frames) board_.RequestQuit(0);
		});
	}

	REVM_LOG(REVM_DEBUG, "live — snap=%s (embedded=%s)",
	         begin_snap_path().c_str(),
	         embedded_.board_init_role == BoardInitRole::SecondaryRunner
	             ? "secondary"
	             : "primary");

	if (!handoff_from_begin_snap(nullptr)) {
		return false;
	}

	pace_ = limit_speed;
	frame_start_ = std::chrono::steady_clock::now();
	prepared_live_ = true;
	playback_prepared_ = true;
	return true;
}

int CpuMockHost::RunPreparedPlayback() {
	if (!playback_prepared_) {
		REVM_LOG(REVM_ERROR, "cpu-mock playback is not prepared");
		return 1;
	}
	if (!entry_handler_) {
		REVM_LOG(REVM_ERROR, "no EntryPoint (SetEntryHandler)");
		return 1;
	}
	const Config & cfg = board_.GetConfig();
	if (!cfg.events_path.empty() || !cfg.report_path.empty()) {
		event::Open(cfg.events_path, cfg.report_path);
		event::Record r("run_start");
		r.kv("mode", prepared_live_ ? "live" : "use-play")
			.kv("play", playback_path_.c_str())
			.kv("snap", begin_snap_path().c_str())
			.kv("main_blank", cfg.main_blank)
			.kv("kb", cfg.kb_path.c_str())
			.kv("rand_seed", cfg.rand_seed)
			.kv("max_frames", playback_max_frames_)
			.kv("no_twin", compare_opts_.no_twin)
			.kv("audio", cfg.audio_enabled)
			.kv("limit_speed", cfg.limit_speed)
			.kv("headless", cfg.headless)
			.kv("ignore_checks", compare_opts_.ignore_checks)
			.kv("ignore_asserts", compare_opts_.ignore_asserts)
			.kv("ignore_play_hashes", compare_opts_.ignore_play_hashes);
		r.end();
	}
	board_.StartWallClockLimit();
	try {
		if (embedded_.before_entry) embedded_.before_entry();
		if (!QuitRequested())
			entry_handler_();
	} catch (const SoftQuitException &) {
		// RequestQuit already set exit_code_; fall through to tallies.
	}
	board_.StopWallClockLimit();
	finish_media();

	if (auto * cpu = board_.Machine() ? board_.Machine()->TheCPU : nullptr) {
		cpu->SetCpuMock(false);
	}
	twin_.reset();
	if (embedded_.own_clock_source)
		log::ClearClockSource();

	print_compare_tallies();
	if (prepared_live_) {
		event::SetRunEnd(board_.ExitCode() != 0
		                     ? (event::HasFail() ? "softquit" : "fail")
		                     : "ok",
		                 board_.FrameCounter());
		event::Shutdown();
		return board_.ExitCode();
	}
	const bool failed = board_.ExitCode() != 0 || !playback_player_.Ok();
	event::SetRunEnd(failed ? (event::HasFail() ? "softquit" : "fail") : "ok",
	                 board_.FrameCounter());
	event::Shutdown();
	if (playback_player_.Comparisons() > 0 || playback_player_.Failures() > 0) {
		REVM_LOG(REVM_DEBUG, "play hashes: compared=%llu failed=%llu",
		         static_cast<unsigned long long>(playback_player_.Comparisons()),
		         static_cast<unsigned long long>(playback_player_.Failures()));
	}
	if (board_.ExitCode() != 0) return board_.ExitCode();
	return playback_player_.Ok() ? 0 : 1;
}

int CpuMockHost::RunPlay(const std::string & prg_path, IInputSource * input,
                        const JoystickConfig * joy, uint32_t max_frames,
                        bool headless, bool limit_speed, bool audio,
                        const PlaybackCompareOpts & compare,
                        const Config * user_cfg,
                        ::revm::PlayRecorder * recorder) {
	(void)prg_path;
	// Interactive playback is a standalone run; do not inherit hooks from a
	// prior embedded PreparePlayback call on a reused host.
	embedded_ = {};
	compare_opts_ = compare;
	Config cfg;
	cfg.stage = Stage::Stage3;
	cfg.run_mode = recorder ? RunMode::Record : RunMode::Play;
	cfg.prg_path.clear();
	cfg.headless = headless;
	cfg.audio_enabled = audio;
	cfg.limit_speed = limit_speed;
	apply_user_cfg(cfg, user_cfg);

	std::string error;
	board_.Configure(cfg);
	if (!board_.Init(error)) {
		REVM_LOG(REVM_ERROR, "Init failed: %s", error.c_str());
		return 1;
	}

	// Interactive: own LiveInput for the run. Caller-supplied input wins.
	LiveInput live(board_.Machine());
	if (joy)
		live.SetJoystickConfig(*joy);
	if (input)
		board_.SetInputSource(input);
	else
		board_.SetInputSource(&live);
	board_.SetPlayRecorder(recorder);

	wire_defaults();
	if (compare_opts_.no_twin) {
		REVM_LOG(REVM_DEBUG, "twin OFF (--no-twin)");
	} else if (!setup_twin()) {
		return 1;
	} else {
		board_.SetMemoryModificationMirror(&twin_->board());
	}
	setup_kb_check();
	main_start_.reset();
	InstallGame(*this);

	if (compare_opts_.no_twin || compare_opts_.ignore_checks ||
	    compare_opts_.ignore_asserts) {
		REVM_LOG(REVM_DEBUG, "ignores:%s%s%s",
			compare_opts_.no_twin ? " no-twin" : "",
			compare_opts_.ignore_checks ? " checks" : "",
			compare_opts_.ignore_asserts ? " asserts" : "");
	} else {
		REVM_LOG(REVM_DEBUG, "Stage 3 live — Main↔Twin compares at joins");
	}

	if (max_frames) {
		board_.SetFrameCallback([this, max_frames](uint32_t frame, uint32_t) {
			if (frame >= max_frames) board_.RequestQuit(0);
		});
	}

	REVM_LOG(REVM_DEBUG, "play — snap=%s (single-thread)",
	              begin_snap_path().empty() ? "(none)"
	                                       : begin_snap_path().c_str());

	if (!handoff_from_begin_snap(nullptr)) {
		return 1;
	}
	if (recorder) {
		recorder->Start(board_);
		recorder->SetNoTwin(compare_opts_.no_twin);
		recorder->CaptureStartHash();
	}

	pace_ = limit_speed;
	frame_start_ = std::chrono::steady_clock::now();

	if (!entry_handler_) {
		REVM_LOG(REVM_ERROR, "no EntryPoint (SetEntryHandler)");
		return 1;
	}
	if (!cfg.events_path.empty() || !cfg.report_path.empty()) {
		event::Open(cfg.events_path, cfg.report_path);
		event::Record r("run_start");
		r.kv("mode", "play")
			.kv("snap", begin_snap_path().empty() ? ""
			                                      : begin_snap_path().c_str())
			.kv("kb", cfg.kb_path.c_str())
			.kv("rand_seed", cfg.rand_seed)
			.kv("max_frames", max_frames)
			.kv("no_twin", compare_opts_.no_twin)
			.kv("audio", audio)
			.kv("limit_speed", limit_speed)
			.kv("headless", headless)
			.kv("ignore_checks", compare_opts_.ignore_checks)
			.kv("ignore_asserts", compare_opts_.ignore_asserts);
		r.end();
	}
	board_.StartWallClockLimit();
	try {
		entry_handler_();
	} catch (const SoftQuitException &) {
		// RequestQuit already set exit_code_; fall through to tallies.
	}
	board_.StopWallClockLimit();
	finish_media();

	if (auto * cpu = board_.Machine() ? board_.Machine()->TheCPU : nullptr) {
		cpu->SetCpuMock(false);
	}
	twin_.reset();
	log::ClearClockSource();
	print_compare_tallies();
	event::SetRunEnd(board_.ExitCode() != 0
	                     ? (event::HasFail() ? "softquit" : "fail")
	                     : "ok",
	                 board_.FrameCounter());
	event::Shutdown();
	return board_.ExitCode();
}

} // namespace revm::cpumock
