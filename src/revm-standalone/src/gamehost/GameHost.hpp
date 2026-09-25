// Created  : 2026-09-13
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Dedicated game-facing host for standalone exports.
//
// Exported game source binds only to this type: control flow, register bags,
// and video surfaces. Launch and JSON replay live on revm::StandaloneRunner.
// No clock, chip, I/O, or logging behavior is reimplemented here — GameHost
// owns the one Main Board and dispatches translated IRQ/NMI.

#include "cpumock/AdvanceResult.hpp"
#include "core/Board.hpp"
#include "core/Config.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace revm {
class StandaloneRunner;
class PlayInput;
}

namespace revm::cpumock {
namespace video_detail { struct HostAccess; }
template <typename Policy> struct ByteAt;
template <typename Policy, uint16_t Addr> struct FixedByte;
template <typename Derived> struct LocalRmwOps;
template <uint16_t Addr, uint16_t Size> struct MemTable;
template <uint16_t Addr> struct Io;
}

namespace gamehost {

class GameHost;

// Defined by the exported game (src/game/plugin.cpp): declares the start
// specification and installs the entry/IRQ/NMI handlers. The runner calls
// it once per run.
void InstallGame(GameHost & host);

using AdvanceResult = revm::AdvanceResult;

enum class VSyncPolicy { Cross, Stop };

// Live CIA timer internals that no register-write sequence can reconstruct
// (a mid-flight counter, its latch pair, SC delay pipeline, and idle flag).
// Applied only on blank Main start; boot code still programs ports, ICR,
// TOD, and the timer latches/control registers.
struct CiaTimerLive {
	uint16_t counter = 0;
	uint16_t latch = 0xFFFF;
	uint8_t count_delay = 0;
	uint8_t load_delay = 0;
	uint8_t oneshot_delay = 0;
	bool idle = false;
};

struct CiaLivePhase {
	CiaTimerLive ta{};
	CiaTimerLive tb{};
};

// Explicit origin for a blank Main that does not restore a C64 snapshot
// wholesale. Optional CIA live phase seeds in-flight timers so standalone
// matches Stage 4 `--main-blank --no-twin`.
struct CpuMockStart {
	uint16_t entry_pc = 0;
	uint32_t cycle = 0;
	uint32_t frame = 0;
	uint8_t cpu_port_ddr = 0xFF;
	uint8_t cpu_port = 0x35;
	bool interrupts_disabled = true;
	std::optional<CiaLivePhase> cia1;
	std::optional<CiaLivePhase> cia2;
};

class GameHost {
public:
	using IrqHandler = std::function<void()>;
	using NmiHandler = std::function<void()>;
	using EntryHandler = std::function<void()>;
	using AdvanceResult = revm::AdvanceResult;

	GameHost() = default;

	// 6510 I flag: the translated SEI/CLI.
	void IrqDisable();
	void IrqEnable();

	// Loud checked failure: log, request quit with `code`, and unwind the
	// entry handler.
	void Fail(int code, const char * fmt, ...)
		__attribute__((format(printf, 3, 4)));

	// Probe for long-running loops: honor a user or runtime quit request.
	bool ShouldQuit() const;

	// Tripwire: the game's coldstart entry matches the declared start PC.
	void AssertEntry(uint16_t pc);

	void SetIrqHandler(IrqHandler handler);
	void SetNmiHandler(NmiHandler handler);
	void SetEntryHandler(EntryHandler handler) {
		entry_handler_ = std::move(handler);
	}
	void InstallMainStart(CpuMockStart start) { main_start_ = start; }

	AdvanceResult AdvanceCycles(uint64_t cycles,
	                            VSyncPolicy vsync_policy = VSyncPolicy::Cross);
	AdvanceResult AdvanceToVSync();
	uint32_t AdvanceFrames(uint32_t frames);
	void ReturnIrq(uint32_t body_cycles);
	void ReturnNmi(uint32_t cycles);

private:
	friend class revm::StandaloneRunner;
	template <typename Policy> friend struct revm::cpumock::ByteAt;
	template <typename Policy, uint16_t Addr>
	friend struct revm::cpumock::FixedByte;
	template <typename Derived> friend struct revm::cpumock::LocalRmwOps;
	template <uint16_t Addr, uint16_t Size>
	friend struct revm::cpumock::MemTable;
	template <uint16_t Addr> friend struct revm::cpumock::Io;
	friend struct revm::cpumock::video_detail::HostAccess;

	[[noreturn]] void fail_message(int code, const char * message);

	bool InIrq() const { return irq_depth_ > 0; }
	bool InNmi() const { return nmi_depth_ > 0; }
	AdvanceResult main_advance_to_vsync();
	void RunIrqHandler();
	void RunNmiHandler();
	void FinishMockIrq(uint32_t body_cycles);
	void FinishMockNmi(uint32_t cycles);
	void AddMockIrqHold(uint32_t cycles);
	bool IrqHoldActive() const;

	uint8_t Read(uint16_t addr);
	void Write(uint16_t addr, uint8_t value);
	void InstallAsset(uint16_t addr, std::span<const uint8_t> bytes);

	static void MockIrqThunk(void * userdata);
	static void MockNmiThunk(void * userdata);
	void dispatch_irq();
	void dispatch_nmi();

	void enable_mock_cpu();
	bool init_board(const revm::Config & cfg);
	bool apply_blank_start(revm::PlayInput * playback);
	int run_entry();
	void ThrowIfQuitRequested();
	void pace_after_vsync();
	AdvanceResult main_advance(uint64_t cycles, bool run_past_vsync = true);

	revm::Board board_;
	EntryHandler entry_handler_;
	std::optional<CpuMockStart> main_start_;
	IrqHandler irq_handler_;
	NmiHandler nmi_handler_;
	int irq_depth_ = 0;
	int nmi_depth_ = 0;
	bool irq_finish_called_ = false;
	bool nmi_finish_called_ = false;
	uint32_t nmi_hold_cycles_ = 0;

	bool pace_ = false;
	std::chrono::steady_clock::time_point frame_start_{};
};

} // namespace gamehost

#ifndef GAMEHOST_BARE
#include "cpumock/Mem.hpp"
#include "cpumock/VideoAssets.hpp"

namespace gamehost {

using IoMap = revm::cpumock::IoMap;
using VideoAssets = revm::cpumock::VideoAssets;
using RamSync = revm::cpumock::RamSync;

template <typename Policy>
using ByteAt = revm::cpumock::ByteAt<Policy>;
template <uint16_t Addr, uint16_t Size>
using MemTable = revm::cpumock::MemTable<Addr, Size>;

} // namespace gamehost
#endif
