// Created  : 2026-08-09
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Stage 3 Main↔Twin join helpers. Logs use REVM_LOG_M("sync", …) so this
// header can be included from Mem.hpp without REVM_LOG_MODULE.
// Plugin-callable surface: JoinAtPc, JoinAtPcBounded, ReturnIrq,
// ReturnNmi, Compare, AdvanceCycles, AdvanceToVSync. Read/Write/Rmw AtPc are
// private; plugins use mem.*/io.* (Mem.hpp). Host steppers
// (TwinRunToPc, MainRunToTwinCycle, MainCatchUpVSync, FrameWait) are
// private to this header. --no-twin delays are AdvanceCycles /
// AdvanceToVSync.
//
// With --no-twin (`NoTwin()` true): only Main VSYNCs run; Twin fence /
// Φ2-align steps are skipped.
//
// IRQ/NMI: chip-driven on both boards, always. Twin-led lockstep steps
// (lock_step_pair) and Twin walks yield HitIrq/HitNmi when Twin enters an
// accept sequence; dispatch() retries and calls RunIrqHandler /
// RunNmiHandler. Run*Handler → enter_interrupt: burn Main to the steal
// cycle, skew verdict vs Main's own line edge (K_EARLY/K_LATE), consume,
// then thunk. Handlers may use JoinAtPc or an armed explicit mem.*/io.*
// operation. They must
// not call raw TwinRunToPc / AdvanceCycles.
// Handler JoinAtPc / AtPc may extra-VSYNC once (silent) if Twin is
// still in the nest; SoftQuit if Twin RTI'd / LeftNest. I/O landed on
// the VBLANK Φ2 SoftQuits for join fences; RAM-fence AtPc (mem.*)
// downgrades that one case to the standard silent extra VSYNC (RAM
// fences never consume the I/O cache). Nested extra VSYNC must not
// pair a new IRQ (sequence ownership stays with the running handler;
// NMI may nest).
// JoinAtPc(..., FenceSlack::Exact) is exact. ReturnIrq / ReturnNmi are the
// inverted RTI fence: sequence ownership (not I) tracks the nest across
// VBLANK extra-VSYNCs once; Twin already RTI'd is done (not SoftQuit).
// HitVSync is VBLANK only. Sync::Compare is a snapshot (no dispatch).
// ReturnIrq owns IRQ teardown (FinishMockIrq). Caller ACK then Return.
// JoinAtPcBounded logs VERBOSE when the walk took 0 frames, INFO when n>0
// (the measured value to set max_frames); --report fence op is "bounded".
// WatchMark is idempotent (WatchTwinPc does not double-register the same pc).

#include "cpumock/CpuMockHost.hpp"
#include "debug/InsnBytes6502.hpp"
#include "util/Event.hpp"
#include "util/Log.hpp"

#include <cstdint>
#include <cstring>
#include <source_location>

namespace revm::cpumock {

struct Sync {
	using Host = CpuMockHost;
	using AdvanceResult = revm::AdvanceResult;

	// Official 6510 memory RMW. INC/DEC do not take C. ASL/LSR/ROL/ROR
	// pass bool *carry (required; in/out for ROL/ROR, out for ASL/LSR).
	enum class RmwOp : uint8_t { Inc, Dec, Asl, Lsr, Rol, Ror };

	template <typename Policy> friend struct ByteAt;
	template <typename Policy, uint16_t Addr> friend struct FixedByte;
	template <uint16_t Addr> friend struct Mem;
	template <uint16_t Addr, uint16_t Size> friend struct MemTable;
	template <uint16_t Addr> friend struct Mem16;
	template <uint16_t Addr> friend struct Io;
	friend struct RamSync;
	friend struct IoSync;

	// A migration-only two-line AtPc fence. The ticket is deliberately kept
	// outside CpuMockHost rather than becoming part of the host's hardware state.
	// The next explicit operation consumes it.
	struct ArmedAtPc {
		CpuMockHost *host;
		uint16_t pc;
		FenceSlack slack;
		bool armed;
	};

	// Plugins are single-threaded, but a thread-local ticket prevents an
	// unrelated embedded host on another thread from consuming this host's
	// marker.  The host pointer is checked again on consumption.
	static inline thread_local ArmedAtPc armed_at_pc_{
		nullptr, 0, FenceSlack::AllowOneVSync, false};

	// Arm the next explicit Mem/Io operation. Mem.hpp consumes this ticket after
	// clearing it, so nested IRQ/NMI code cannot inherit it.
	static inline void AtPc(Host &host, uint16_t pc,
	                       FenceSlack slack = FenceSlack::AllowOneVSync) {
		if (armed_at_pc_.armed) {
			host.SoftQuit(1,
			              "Sync::AtPc double-arm want=$%04X pending=$%04X",
			              pc, armed_at_pc_.pc);
			return;
		}
		armed_at_pc_ = ArmedAtPc{&host, pc, slack, true};
	}

	// Consume the pending marker.  Clearing happens before the caller invokes
	// Read/Write/RMW AtPc, because those paths may dispatch a nested IRQ/NMI.
	// Returns false when no marker is armed; callers then retain ordinary
	// Main-only access semantics.
	static inline bool ConsumeAtPc(Host &host, ArmedAtPc &out) {
		if (!armed_at_pc_.armed)
			return false;
		if (armed_at_pc_.host != &host) {
			host.SoftQuit(1,
			              "Sync::AtPc host mismatch pending=%p current=%p",
			              static_cast<void *>(armed_at_pc_.host),
			              static_cast<void *>(&host));
			return false;
		}
		out = armed_at_pc_;
		armed_at_pc_ = ArmedAtPc{};
		return true;
	}

	// Linked migration values do not have a Main-board address to read or
	// write.  They still consume the same two-line AtPc ticket, however, so
	// their timed operation can use the RAM fence and remain interrupt-safe.
	// The ticket carries the host that armed it; linked values deliberately do
	// not need to retain a host pointer just to support ordinary local access.
	static inline bool ConsumeAtPc(ArmedAtPc &out) {
		if (!armed_at_pc_.armed)
			return false;
		out = armed_at_pc_;
		armed_at_pc_ = ArmedAtPc{};
		return true;
	}

	// Fence a timed operation on Stage-4 local state.  Unlike ReadRamAtPc /
	// WriteRamAtPc / RmwRamAtPc these helpers never touch Main RAM: the caller
	// performs the local read/write/RMW after the fence.  The original address
	// is retained for diagnostics and migration provenance only.
	static inline void LinkedReadAtPc(Host &host, uint16_t pc, uint16_t addr,
	                                 FenceSlack slack = FenceSlack::AllowOneVSync) {
		if (host.NoTwin()) {
			REVM_LOG_M("sync", REVM_VERBOSE,
			           "LinkedReadAtPc pc=$%04X addr=$%04X (no-twin)", pc, addr);
			return;
		}
		const bool extra = ramFence(host, pc, "LinkedReadAtPc", "linked_read",
		                            slack);
		ev_fence(host, "linked_read", pc, extra ? "hit_vsync" : "ok", extra);
		REVM_LOG_M("sync", REVM_VERBOSE,
		           "LinkedReadAtPc pc=$%04X addr=$%04X", pc, addr);
	}

	static inline void LinkedWriteAtPc(Host &host, uint16_t pc, uint16_t addr,
	                                  uint8_t value,
	                                  FenceSlack slack = FenceSlack::AllowOneVSync) {
		if (host.NoTwin()) {
			REVM_LOG_M("sync", REVM_VERBOSE,
			           "LinkedWriteAtPc pc=$%04X addr=$%04X val=$%02X (no-twin)",
			           pc, addr, value);
			return;
		}
		const bool extra = ramFence(host, pc, "LinkedWriteAtPc", "linked_write",
		                            slack);
		ev_fence(host, "linked_write", pc, extra ? "hit_vsync" : "ok", extra);
		REVM_LOG_M("sync", REVM_VERBOSE,
		           "LinkedWriteAtPc pc=$%04X addr=$%04X val=$%02X", pc, addr,
		           value);
	}

	static inline void LinkedRmwAtPc(Host &host, uint16_t pc, uint16_t addr,
	                                RmwOp op, bool *carry,
	                                FenceSlack slack = FenceSlack::AllowOneVSync) {
		rmw_check_carry(host, op, carry);
		if (host.NoTwin()) {
			REVM_LOG_M("sync", REVM_VERBOSE,
			           "LinkedRmwAtPc pc=$%04X addr=$%04X op=%s (no-twin)", pc,
			           addr, rmw_op_name(op));
			return;
		}
		const bool extra = ramFence(host, pc, "LinkedRmwAtPc", "linked_rmw",
		                            slack);
		ev_fence(host, "linked_rmw", pc, extra ? "hit_vsync" : "ok", extra);
		REVM_LOG_M("sync", REVM_VERBOSE,
		           "LinkedRmwAtPc %s pc=$%04X addr=$%04X", rmw_op_name(op), pc,
		           addr);
	}

	// Synchronization boundaries and unsupported aggregate accesses must not
	// silently skip an armed marker. Source lint separately enforces immediate
	// adjacency to an explicit byte operation. This is a SoftQuit rather than an
	// assertion so a stale marker is visible in the normal Stage 3 report.
	static inline void RejectArmed(Host &host, const char *where) {
		if (armed_at_pc_.armed) {
			host.SoftQuit(1, "Sync::AtPc unconsumed before %s pc=$%04X", where,
						  armed_at_pc_.pc);
		}
	}

private:
	static constexpr int kMaxDispatchRetries = 128;

	static constexpr bool rmw_uses_carry(RmwOp op) {
		return op != RmwOp::Inc && op != RmwOp::Dec;
	}

	static constexpr const char * rmw_op_name(RmwOp op) {
		switch (op) {
		case RmwOp::Inc: return "INC";
		case RmwOp::Dec: return "DEC";
		case RmwOp::Asl: return "ASL";
		case RmwOp::Lsr: return "LSR";
		case RmwOp::Rol: return "ROL";
		case RmwOp::Ror: return "ROR";
		}
		return "?";
	}

	struct RmwApply {
		uint8_t value = 0;
		bool cout = false;
	};

	static inline RmwApply apply_rmw(uint8_t v, RmwOp op, bool cin) {
		switch (op) {
		case RmwOp::Inc:
			return {uint8_t(v + 1), cin};
		case RmwOp::Dec:
			return {uint8_t(v - 1), cin};
		case RmwOp::Asl:
			return {uint8_t(v << 1), (v & 0x80) != 0};
		case RmwOp::Lsr:
			return {uint8_t(v >> 1), (v & 0x01) != 0};
		case RmwOp::Rol:
			return {uint8_t(uint8_t(v << 1) | (cin ? 1u : 0u)),
			        (v & 0x80) != 0};
		case RmwOp::Ror:
			return {uint8_t(uint8_t(v >> 1) | (cin ? 0x80u : 0u)),
			        (v & 0x01) != 0};
		}
		return {v, cin};
	}

	static inline void rmw_check_carry(Host & host, RmwOp op, bool *carry) {
		if (rmw_uses_carry(op)) {
			if (!carry)
				host.SoftQuit(1, "%sAtPc requires bool *carry", rmw_op_name(op));
		} else if (carry) {
			host.SoftQuit(1, "%sAtPc does not take carry", rmw_op_name(op));
		}
	}

	// NMI before IRQ. HitVSync / LeftNest / Ok / Timeout bubble out.
	// Cap retries so a same-steal re-yield cannot spin. Do not
	// auto-dispatch inside host TwinRunToPc (that re-nests).
	template <class Once>
	static inline AdvanceResult dispatch(Host & host, Once once) {
		int retries = 0;
		for (;;) {
			const AdvanceResult r = once();
			if (r == AdvanceResult::HitNmi || r == AdvanceResult::HitIrq) {
				REVM_EVENT("accept", "phase", "detected", "kind",
				           r == AdvanceResult::HitNmi ? "nmi" : "irq");
				if (++retries > kMaxDispatchRetries) {
					host.SoftQuit(1,
					              "Sync dispatch: too many HitIrq/HitNmi "
					              "retries (%d)",
					              retries);
					return AdvanceResult::Error;
				}
				if (r == AdvanceResult::HitNmi)
					host.RunNmiHandler();
				else
					host.RunIrqHandler();
				continue;
			}
			return r;
		}
	}

	static inline bool in_handler(Host & host) {
		return host.InIrq() || host.InNmi();
	}

	static inline const char * src_file(std::source_location loc) {
		const char * f = loc.file_name();
		if (const char * slash = std::strrchr(f, '/'))
			return slash + 1;
		return f;
	}

	static inline const char * slack_name(FenceSlack slack) {
		return slack == FenceSlack::Exact ? "Exact" : "AllowOneVSync";
	}

	static inline uint16_t twin_pc_or0(Host & host) {
		return host.NoTwin() ? uint16_t(0) : host.TwinCpu().pc;
	}

	// Structured fence record (single bool check when events are off).
	static inline void ev_fence(Host & host, const char * op, uint16_t pc,
	                            const char * result, bool extra_vsync) {
		host.note_fence(op, pc); // for fail-context records
		REVM_EVENT("fence", "op", op, "pc", revm::event::Hex{pc}, "result",
		           result, "extra_vsync", extra_vsync ? 1 : 0, "nested",
		           in_handler(host) ? 1 : 0, "raster_line", host.RasterLine());
	}

	static inline void fail_nested_vsync(Host & host, const char * tag) {
		host.SoftQuit(1, "%s HitVSync from IRQ/NMI handler", tag);
	}

	static inline void fail_nested_left_nest(Host & host, const char * tag,
	                                         uint16_t pc) {
		const auto tw = host.TwinCpu();
		host.SoftQuit(1,
		              "%s Twin left this IRQ/NMI nest want=$%04X twin_pc=$%04X",
		              tag, pc, tw.pc);
	}

	// Nested walk abort: LeftNest = Twin left this accept (SoftQuit).
	// HitVSync is extra-VSYNC only when allow_vsync.
	static inline void check_nested_walk(Host & host, const char * tag,
	                                     uint16_t pc, AdvanceResult r,
	                                     bool allow_vsync) {
		if (!in_handler(host))
			return;
		if (r == AdvanceResult::LeftNest)
			fail_nested_left_nest(host, tag, pc);
		if (allow_vsync) {
			if (r != AdvanceResult::HitVSync)
				fail_nested_vsync(host, tag);
		} else if (r == AdvanceResult::HitVSync) {
			fail_nested_vsync(host, tag);
		}
	}

	// Caller class for require_nested_extra_vsync_ok. The TwinIoLandedOn-
	// LastPhi2 clause keeps its SoftQuit for join fences; RAM-fence AtPc
	// callers downgrade it to the standard silent nested extra VSYNC.
	enum class FenceClass : uint8_t { Join, Ram };

	static inline void require_nested_extra_vsync_ok(Host & host,
	                                                 const char * tag,
	                                                 uint16_t pc,
	                                                 FenceClass cls) {
		if (!in_handler(host))
			return;
		const auto tw = host.TwinCpu();
		if (!host.TwinInInterruptNest()) {
			host.SoftQuit(1,
			              "%s nested extra VSYNC: Twin left this IRQ/NMI nest "
			              "want=$%04X twin_pc=$%04X",
			              tag, pc, tw.pc);
		}
		if (host.TwinIoLandedOnLastPhi2()) {
			if (cls != FenceClass::Ram)
				host.SoftQuit(1,
				              "%s nested extra VSYNC: I/O already on VBLANK "
				              "Φ2 want=$%04X twin_pc=$%04X",
				              tag, pc, tw.pc);
			// RAM fence: fall through silently — the caller logs its
			// standard extra-VSYNC line and runs run_extra_main_vsync.
		}
	}

	static inline void run_extra_main_vsync(Host & host, const char * tag,
	                                        const char * site) {
		// RAW Main-only VSYNC (host.main_advance_to_vsync): Twin stays parked or
		// ahead at its fence; this is bookkeeping, not a lockstep wait.
		// Paired walks carry Main across the same barrier as Twin (≤1 Φ2
		// behind) — an extra VSYNC there would run Main a full frame hot.
		if (host.board_.FrameCounter() >= host.twin_->FrameCounter())
			return;
		event::EmitExtraVsync(site);
		if (host.main_advance_to_vsync() != AdvanceResult::HitVSync)
			host.SoftQuit(1, "%s extra VSYNC failed", tag);
	}

	// Twin still in this nest across VBLANK → one extra Main VSYNC, then
	// retry the RTI PC. Twin already RTI'd / left → teardown without
	// chasing Twin to the next occurrence of rti_pc. One extra only;
	// still in nest and not at RTI afterwards is SoftQuit. Calendar off
	// or --no-twin: Main-owned `cycles` (not a Twin-PC chase).
	template <class StillIn, class Finish>
	static inline void return_hw(Host & host, uint16_t rti_pc, uint32_t cycles,
	                             CompareMask mask, const char * tag,
	                             StillIn still_in_nest, Finish finish) {
		if (host.NoTwin()) {
			finish(cycles);
			if (mask.any())
				host.CompareNow(mask);
			return;
		}

		auto done = [&] {
			finish(0);
			if (mask.any())
				host.CompareNow(mask);
		};

		auto align_at_rti = [&] {
			AdvanceResult a = MainRunToTwinCycle(host);
			if (a == AdvanceResult::Ok)
				return;
			// Main can sit ahead of Twin here: a late-edge verdict waited
			// for Main's line past the steal. Bring Twin up instead — any
			// boundary crossed belongs to the follow-up dispatch.
			if (host.board_.FrameCounter() != host.twin_->FrameCounter()) {
				REVM_LOG_M("sync", REVM_ERROR,
				           "%s align frame mismatch want=$%04X main=%u "
				           "twin=%u",
				           tag, rti_pc, host.board_.FrameCounter(),
				           host.twin_->FrameCounter());
				host.SoftQuit(1, "%s Φ2 align frame mismatch", tag);
			}
			int guard = 0;
			while (host.board_.CycleCounter() >
			       host.twin_->CycleCounter()) {
				// Teardown leveling must not free-run Twin across an
				// opcode fetch a SUSPENDED fence still has to pair on
				// (e.g. an accept that stole exactly at the fence target
				// and RTI'd back onto it). Park Twin at the fetch and
				// leave the Φ2 residue: the retried fence pairs on this
				// arrival and restores parity after it
				// (align_after_arrival).
				if (host.twin_fenced_fetch_pending())
					break;
				// Invariant: teardown leveling never consumes a fresh
				// accept natively. When the next Twin-only step would
				// cross an UNOWNED interrupt boundary, park Twin there
				// and leave the Φ2 residue — the post-return follow-up
				// dispatcher classifies it (NMI before IRQ) and dispatches
				// through the normal door with a real skew verdict.
				if (host.twin_unowned_accept_boundary()) {
					REVM_LOG_M("sync", REVM_VERBOSE,
					           "%s defer-park accept boundary "
					           "want=$%04X twin_pc=$%04X main=%u "
					           "twin=%u",
					           tag, rti_pc, host.TwinCpu().pc,
					           host.board_.CycleCounter(),
					           host.twin_->CycleCounter());
					break;
				}
				if (++guard > 4096) {
					host.SoftQuit(1, "%s Φ2 align twin catch-up stuck", tag);
					return;
				}
				(void)host.twin_step_only();
			}
		};

		auto execute_rti = [&] {
			AdvanceResult lr = host.TwinExecuteIrqRti();
			if (lr == AdvanceResult::HitVSync) {
				REVM_LOG_M("sync", REVM_DEBUG,
				           "%s RTI crossed VSYNC want=$%04X", tag, rti_pc);
				run_extra_main_vsync(host, tag, "nested");
				lr = host.TwinExecuteIrqRti();
			}
			if (lr != AdvanceResult::LeftNest) {
				host.SoftQuit(1,
				              "%s failed to leave RTI want=$%04X twin_pc=$%04X "
				              "result=%s",
				              tag, rti_pc, host.TwinCpu().pc,
				              revm::AdvanceResultName(lr));
			}
		};

		if (!still_in_nest()) {
			done();
			return;
		}

		auto walk = [&] {
			return TwinRunToPc(host, rti_pc, true, true);
		};

		AdvanceResult r = walk();
		REVM_LOG_M("sync", REVM_VERBOSE,
		           "%s walk1 r=%s want=$%04X twin_pc=$%04X main=%u twin=%u",
		           tag, revm::AdvanceResultName(r), rti_pc, host.TwinCpu().pc,
		           host.board_.CycleCounter(), host.twin_->CycleCounter());
		if (r == AdvanceResult::Ok) {
			if (host.board_.CycleCounter() > host.twin_->CycleCounter()) {
				execute_rti();
				align_at_rti();
			} else {
				align_at_rti();
				execute_rti();
			}
			done();
			return;
		}
		if (r == AdvanceResult::LeftNest) {
			done();
			return;
		}
		if (r != AdvanceResult::HitVSync) {
			REVM_LOG_M("sync", REVM_ERROR,
			           "%s TwinRunToPc failed want=$%04X twin_pc=$%04X "
			           "result=%s",
			           tag, rti_pc, host.TwinCpu().pc,
			           revm::AdvanceResultName(r));
			host.SoftQuit(1, "%s TwinRunToPc $%04X failed", tag, rti_pc);
		}

		if (!still_in_nest()) {
			done();
			return;
		}

		if (!host.TwinAtOpcodeFetch())
			(void)host.TwinFinishInstruction();
		const auto tw = host.TwinCpu();
		REVM_LOG_M("sync", REVM_DEBUG,
		           "%s extra VSYNC want=$%04X twin_pc=$%04X nested=1",
		           tag, rti_pc, tw.pc);
		run_extra_main_vsync(host, tag, "nested");

		if (!still_in_nest()) {
			done();
			return;
		}

		r = walk();
		if (r == AdvanceResult::Ok) {
			if (host.board_.CycleCounter() > host.twin_->CycleCounter()) {
				execute_rti();
				align_at_rti();
			} else {
				align_at_rti();
				execute_rti();
			}
			done();
			return;
		}
		if (r == AdvanceResult::LeftNest ||
		    (r == AdvanceResult::HitVSync && !still_in_nest())) {
			done();
			return;
		}
		const auto tw2 = host.TwinCpu();
		REVM_LOG_M("sync", REVM_ERROR,
		           "%s still in nest after extra VSYNC want=$%04X "
		           "twin_pc=$%04X result=%s",
		           tag, rti_pc, tw2.pc, revm::AdvanceResultName(r));
		host.SoftQuit(1, "%s $%04X still in nest", tag, rti_pc);
	}

public:
	// Dispatching steppers plugins may call. Host steppers are private
	// (yield Hit* and stop). --no-twin delays: AdvanceCycles / AdvanceToVSync
	// (raster loops wait with AdvanceToVSync). Do not JoinAtPc as a wait.
	// The no-Twin branch delegates to CpuMockHost's shared Main-only clock;
	// Twin waits remain lockstep here.
	//
	// With Twin, waits are TWIN-LED LOCKSTEP: lock_step_pair alternates
	// Twin/Main Φ2 (Twin first), so Twin's accepts dispatch promptly while
	// Main trails ≤1 Φ2 and enter_interrupt aligns cleanly at the steal.
	// Extra-VSYNC bookkeeping uses the RAW host.main_advance_to_vsync
	// (Main-only; Twin parked or ahead) via run_extra_main_vsync.
	static inline AdvanceResult AdvanceCycles(
		Host & host, uint64_t cycles,
		VSyncPolicy vsync_policy = VSyncPolicy::Cross) {
		RejectArmed(host, "AdvanceCycles");
		if (host.NoTwin())
			return host.AdvanceCycles(cycles, vsync_policy);
		if (cycles == 0)
			return AdvanceResult::Ok;

		// Cycle budget, not a target: retrying the same N after Hit* would
		// re-run N Φ2. Subtract elapsed (including handler/hold billed on
		// the next step) and continue. Same-steal with no Φ2 progress caps.
		uint64_t left = cycles;
		int no_progress = 0;
		while (left > 0) {
			const uint32_t c0 = host.board_.CycleCounter();
			const bool cross_vsync = vsync_policy == VSyncPolicy::Cross;
			const AdvanceResult r = host.lock_step_pair(left, !cross_vsync);
			if (r == AdvanceResult::HitNmi || r == AdvanceResult::HitIrq) {
				if (host.board_.CycleCounter() == c0 &&
				    host.twin_->CycleCounter() == c0) {
					if (++no_progress > kMaxDispatchRetries) {
						host.SoftQuit(1,
						              "Sync dispatch: too many HitIrq/"
						              "HitNmi retries (%d)",
						              no_progress);
						return AdvanceResult::Error;
					}
				} else {
					no_progress = 0;
				}
				if (r == AdvanceResult::HitNmi)
					host.RunNmiHandler();
				else
					host.RunIrqHandler();
				const uint32_t ran = host.board_.CycleCounter() - c0;
				if (uint64_t(ran) >= left)
					left = 0;
				else
					left -= ran;
				continue;
			}
			return r; // Ok (budget spent), HitVSync, or error-ish
		}
		return AdvanceResult::Ok;
	}

	static inline AdvanceResult AdvanceToVSync(Host & host) {
		RejectArmed(host, "AdvanceToVSync");
		return FrameWait(host);
	}

private:
	static inline AdvanceResult FrameWait(Host & host) {
		if (host.NoTwin())
			return host.AdvanceToVSync();
		return dispatch(host, [&] { return host.lock_step_pair(0, true); });
	}

	// RAW Main-only VSYNC for interval-exit patterns: Twin stays parked
	// (or ahead at its fence) while Main catches up to the same barrier.
	// No-op when Main is already on Twin's barrier (paired walks cross
	// barriers together) — otherwise it would run Main a frame hot.
	// Never use as a general wait — it freezes Twin's chips.
	static inline AdvanceResult MainCatchUpVSync(Host & host) {
		if (host.HasTwin() &&
		    host.board_.FrameCounter() >= host.twin_->FrameCounter())
			return AdvanceResult::HitVSync;
		return host.main_advance_to_vsync();
	}

	static inline AdvanceResult TwinRunToPc(Host & host, uint16_t pc,
	                                       bool soft_fail = false,
	                                       bool yield_at_target = false,
	                                       bool pair_main = true) {
		return dispatch(host, [&] {
			return host.TwinRunToPc(pc, soft_fail, yield_at_target, pair_main);
		});
	}

	static inline AdvanceResult MainRunToTwinCycle(Host & host) {
		return dispatch(host, [&] { return host.MainRunToTwinCycle(); });
	}

	// Φ2-align after a fence ARRIVAL (the walk returned Ok). Normally Main
	// trails or is level (MainRunToTwinCycle). A deferred-teardown park
	// (align_at_rti below) can leave Main up to a grace-window's Φ2 ahead
	// while Twin stands exactly on this fence's fetch: the arrival is
	// already recognized, so close the residue with bounded Twin-only
	// steps. Crossing the fetch now is inert — this fence owns the
	// instant. The same invariant as teardown leveling applies: a fresh
	// UNOWNED accept boundary surfacing mid-residue is NOT stepped over —
	// Twin parks there and the boundary surfaces at the next walk /
	// follow-up dispatch (kind-checked, NMI before IRQ).
	static inline void align_after_arrival(Host & host, const char * tag,
	                                       uint16_t pc) {
		if (host.NoTwin())
			return;
		if (MainRunToTwinCycle(host) == AdvanceResult::Ok)
			return;
		const uint32_t mf = host.board_.FrameCounter();
		const uint32_t tf = host.twin_->FrameCounter();
		if (mf != tf || host.board_.CycleCounter() <= host.twin_->CycleCounter())
			host.SoftQuit(1,
			              "%s align failed want=$%04X twin_pc=$%04X frame "
			              "main=%u twin=%u",
			              tag, pc, host.TwinCpu().pc, mf, tf);
		int guard = 0;
		while (host.board_.CycleCounter() > host.twin_->CycleCounter()) {
			if (++guard > 4096)
				host.SoftQuit(1, "%s align residue stuck want=$%04X", tag,
				              pc);
			if (host.twin_unowned_accept_boundary()) {
				REVM_LOG_M("sync", REVM_VERBOSE,
				           "%s defer-park accept boundary want=$%04X "
				           "twin_pc=$%04X main=%u twin=%u",
				           tag, pc, host.TwinCpu().pc,
				           host.board_.CycleCounter(),
				           host.twin_->CycleCounter());
				break;
			}
			(void)host.twin_step_only();
		}
	}

public:
	// Same-frame fence at the original entry PC, then compare. Never a wait.
	// FenceSlack::AllowOneVSync (default): extra Main VSYNC if Twin misses
	// the fence is silent. FenceSlack::Exact: extra VSYNC is a failure.
	// IRQ/NMI bodies may call this. Nested HitVSync extra-VSYNCs once if Twin is
	// nest; LeftNest / Twin already RTI'd SoftQuits; I/O on the VBLANK Φ2
	// SoftQuits. Do not call raw TwinRunToPc / AdvanceCycles from a handler.
	static inline void JoinAtPc(Host & host, uint16_t pc,
	                            CompareMask mask = CompareMask::Kb(),
	                            FenceSlack slack = FenceSlack::AllowOneVSync,
	                            std::source_location loc =
	                                std::source_location::current()) {
		RejectArmed(host, "JoinAtPc");
		REVM_LOG_M("sync", REVM_VERBOSE, "JoinAtPc pc=$%04X", pc);

		auto miss = [&](AdvanceResult r) {
			ev_fence(host, "join", pc,
			         r == AdvanceResult::Timeout ? "timeout" : "miss", false);
			host.emit_fence_miss_context(mask);
			host.SoftQuit(1,
			              "JoinAtPc miss want=$%04X twin_pc=$%04X result=%s "
			              "slack=%s nested=%d at %s:%u",
			              pc, twin_pc_or0(host), revm::AdvanceResultName(r),
			              slack_name(slack), int(in_handler(host)),
			              src_file(loc), loc.line());
		};

		if (host.NoTwin())
			return;

		bool extra = false;
		if (slack == FenceSlack::Exact) {
			const AdvanceResult r = TwinRunToPc(host, pc, false, true);
			if (r != AdvanceResult::Ok) {
				check_nested_walk(host, "JoinAtPc", pc, r, false);
				miss(r);
			}
		} else {
			const AdvanceResult r = TwinRunToPc(host, pc, true, true);
			if (r != AdvanceResult::Ok) {
				extra = true;
				check_nested_walk(host, "JoinAtPc", pc, r, true);
				require_nested_extra_vsync_ok(host, "JoinAtPc", pc,
				                              FenceClass::Join);
				// HitVSync can land on an operand byte. Finish that instruction
				// so extra VSYNC does not clone a mid-op park. Do not finish
				// JSR/JMP/RTS (those would enter the join fence).
				if (r == AdvanceResult::HitVSync && !host.TwinAtOpcodeFetch())
					(void)host.TwinFinishInstruction();
				const auto tw = host.TwinCpu();
				REVM_LOG_M("sync", REVM_DEBUG,
				         "JoinAtPc extra VSYNC want=$%04X twin_pc=$%04X "
				         "irq=%d nmi=%d fetch=%d i=%d nested=%d",
				         pc, tw.pc, int(tw.irq_pending), int(tw.nmi_pending),
				         int(host.TwinAtOpcodeFetch()),
				         int((tw.p & 0x04) != 0), int(in_handler(host)));
				run_extra_main_vsync(host, "JoinAtPc", "join");
			}

			const AdvanceResult r2 = TwinRunToPc(host, pc, false, true);
			if (r2 != AdvanceResult::Ok) {
				ev_fence(host, "join", pc, "miss", extra);
				miss(r2);
			}
		}

		align_after_arrival(host, "JoinAtPc", pc);

		ev_fence(host, "join", pc, extra ? "hit_vsync" : "ok", extra);
		host.CompareNow(mask);
	}

	// Interval exit: Twin is in original code Main executed in zero time.
	// Walk Twin PAIRED until it reaches `pc`, catching Main up one barrier
	// at a time (MainCatchUpVSync, never a lockstep frame), at most
	// max_frames. Φ2-align, Compare(mask), return the measured frame count.
	// --no-twin: run Main no_twin_frames VSYNCs.
	// Follow-up: FenceSlack::Exact as JoinAtPc default (not this task).
	struct Bounded {
		uint32_t max_frames;          // required
		uint32_t no_twin_frames;      // required
		CompareMask mask = CompareMask::Kb();
		uint16_t no_lap_pc = 0;
		// Twin must park ON the fetch so a pending accept is yielded before
		// the sample (FF $F3E7 place_pickups CIA wait). Default false.
		bool yield_at_target = false;
	};

	static inline uint32_t JoinAtPcBounded(Host & host, uint16_t pc, Bounded b,
	                                       std::source_location loc =
	                                           std::source_location::current()) {
		RejectArmed(host, "JoinAtPcBounded");
		auto fail = [&](AdvanceResult r, uint32_t n) {
			ev_fence(host, "bounded", pc,
			         r == AdvanceResult::Timeout ? "timeout" : "miss", n > 0);
			host.emit_fence_miss_context(b.mask);
			host.SoftQuit(1,
			              "JoinAtPcBounded miss want=$%04X twin_pc=$%04X "
			              "result=%s n=%u max=%u at %s:%u",
			              pc, twin_pc_or0(host), revm::AdvanceResultName(r),
			              n, b.max_frames, src_file(loc), loc.line());
		};

		if (host.NoTwin()) {
			return host.AdvanceFrames(b.no_twin_frames);
		}

		uint64_t lap0 = 0;
		if (b.no_lap_pc != 0) {
			host.WatchTwinPc(b.no_lap_pc);
			lap0 = host.TwinWatchHits(b.no_lap_pc);
		}

		uint32_t n = 0;
		for (;;) {
			const AdvanceResult r =
				TwinRunToPc(host, pc, true, b.yield_at_target);
			if (r == AdvanceResult::Ok)
				break;
			if (r != AdvanceResult::HitVSync || ++n > b.max_frames)
				fail(r, n);
			const AdvanceResult c = MainCatchUpVSync(host);
			if (c != AdvanceResult::HitVSync)
				fail(c, n);
		}

		if (b.no_lap_pc != 0) {
			const uint64_t got = host.TwinWatchHits(b.no_lap_pc) - lap0;
			if (got != 0)
				host.SoftQuit(1,
				              "JoinAtPcBounded lap $%04X delta=%llu "
				              "want=$%04X n=%u max=%u at %s:%u",
				              b.no_lap_pc, (unsigned long long)got, pc, n,
				              b.max_frames, src_file(loc), loc.line());
		}

		align_after_arrival(host, "JoinAtPcBounded", pc);

		ev_fence(host, "bounded", pc, n > 0 ? "hit_vsync" : "ok", n > 0);
		REVM_LOG_M("sync", n == 0 ? REVM_VERBOSE : REVM_INFO,
		           "JoinAtPcBounded $%04X arrived after %u frame(s) (max %u) "
		           "at %s:%u",
		           pc, n, b.max_frames, src_file(loc), loc.line());

		if (b.mask.any())
			host.CompareNow(b.mask);
		return n;
	}

	// Original RTI of an IRQ handler. Requires InIrq(). Caller ACK (LDA ICR)
	// then Return. Default mask is None() (duration fence; kb stays on outer
	// joins). Twin still obligated by this accept token (not I)
	// across VBLANK extra-VSYNCs once (silent). Twin already RTI'd /
	// LeftNest is success — unlike nested JoinAtPc, which SoftQuits.
	// --no-twin / calendar off: FinishMockIrq
	// (`cycles` + 8 Φ2 IR-clear). Do not chase Twin PC when the calendar is
	// off (Twin may be in interval 6510, not this handler).
	static inline void ReturnIrq(Host & host, uint16_t rti_pc, uint32_t cycles,
	                             CompareMask mask = CompareMask::None()) {
		RejectArmed(host, "ReturnIrq");
		if (!host.InIrq())
			host.SoftQuit(1, "ReturnIrq requires InIrq");
		if (host.NoTwin()) {
			host.ReturnIrq(cycles);
			if (mask.any())
				host.CompareNow(mask);
			return;
		}
		return_hw(host, rti_pc, cycles, mask, "ReturnIrq",
		          [&] { return host.TwinInInterruptNest(); },
		          [&](uint32_t c) { host.FinishMockIrq(c); });
	}

	// Original RTI of an NMI handler. Requires InNmi(). Caller ACK then
	// Return. Do not TwinRunToPc to rti_pc from the steal — that executes
	// the NMI body (SID/CIA2) now; drain + later fences own that duration.
	// Twin already left the steal is success (do not chase the next RTI).
	// Extra VSYNC while TwinInNmiSequence() across VBLANK is drain-trunc,
	// not a body chase. NMI *body* spanning VBLANK is unpaid until body
	// tracking exists. --no-twin / calendar off: FinishMockNmi(cycles);
	// 0 is today's no-op (thunk restores I).
	static inline void ReturnNmi(Host & host, uint32_t cycles,
	                             CompareMask mask = CompareMask::None()) {
		RejectArmed(host, "ReturnNmi");
		if (!host.InNmi())
			host.SoftQuit(1, "ReturnNmi requires InNmi");
		if (host.NoTwin()) {
			host.ReturnNmi(cycles);
			if (mask.any())
				host.CompareNow(mask);
			return;
		}
		host.FinishMockNmi(cycles);
		if (mask.any())
			host.CompareNow(mask);
	}

	// Explicit Main↔Twin compare at the current instant (same-frame).
	// Default mask is CompareMask::Kb(). None() / {} is a no-op.
	// Snapshot only — no dispatch. IRQ/NMI bodies may call this.
	static inline void Compare(Host & host,
	                           CompareMask mask = CompareMask::Kb()) {
		RejectArmed(host, "Compare");
		host.CompareNow(mask);
	}

	// Program-lockstep pass counting (STAGE3 §7 rule 2). Take a mark at one
	// plugin point, then assert Twin's watch-hit delta at another. Registers
	// the pc (idempotent — same pc does not double-count). --no-twin: mark
	// is 0 and the check no-ops (no Twin to count passes on).
	static inline uint64_t WatchMark(Host & host, uint16_t pc) {
		RejectArmed(host, "WatchMark");
		if (host.HasTwin())
			host.WatchTwinPc(pc);
		return host.TwinWatchHits(pc);
	}

	// SoftQuits when Twin crossed `pc` a different number of times than
	// `expected` since `mark` — each unexpected crossing is whole native
	// loop passes Main never mirrored. Emits a "watch_delta" event on
	// violation (the SoftQuit itself lands in --events/--report too).
	static inline void ExpectWatchDelta(Host & host, uint16_t pc,
	                                    uint64_t mark, uint64_t expected,
	                                    const char * what) {
		if (host.NoTwin())
			return;
		const uint64_t got = host.TwinWatchHits(pc) - mark;
		if (got == expected)
			return;
		REVM_EVENT("watch_delta", "pc", revm::event::Hex{pc}, "what", what, "expected",
		           expected, "got", got);
		host.SoftQuit(1,
		              "ExpectWatchDelta %s want=$%04X delta=%llu expected=%llu "
		              "(unmirrored native passes?)",
		              what ? what : "?", pc,
		              (unsigned long long)got,
		              (unsigned long long)expected);
	}

private:
	// I/O API takes the opcode PC. Park Twin at pc_after (next fetch) so the
	// chip access is in TwinLastIo* before inject. zp/abs/indexed: 2 or 3
	// bytes (InsnBytes6502). Implied/undocumented length 1 SoftQuits.
	static inline uint16_t io_pc_after(Host & host, uint16_t pc) {
		const uint8_t op = host.TwinPeekMem(pc);
		const unsigned n = revm::InsnBytes6502(op);
		if (n < 2 || n > 3) {
			host.SoftQuit(1,
			              "AtPc I/O: not an I/O-sized opcode pc=$%04X "
			              "op=$%02X len=%u",
			              pc, op, n);
			return pc;
		}
		return uint16_t(pc + n);
	}

	// SID $D400–$D7FF is one 32-byte register file, mirrored through I/O.
	// Named +18 (`io.sid.vol` / $D418) is the same chip register as Twin's
	// STA $D518.
	static inline bool io_addr_same(uint16_t a, uint16_t b) {
		if (a == b)
			return true;
		const bool a_sid = (a >= 0xD400 && a <= 0xD7FF);
		const bool b_sid = (b >= 0xD400 && b <= 0xD7FF);
		return a_sid && b_sid && ((a & 0x1Fu) == (b & 0x1Fu));
	}

	// --- Chip I/O (CIA/VIC/SID). Twin's operand; PAIRED walk. ---
	// `pc` is the I/O opcode. TwinRunToPc walks PAIRED (both boards stay
	// Φ2-locked) to O_FETCH of pc_after; Main's chip access then happens at
	// that aligned instant. Historical inject-at-cached-cycle is gone: with
	// chip-time locked there is no drift to repair, and parking Main's chips
	// behind Twin's (the old unpaired-walk shape) was itself the drift
	// source. Twin's cached operand still gates correctness: reads return
	// Twin's byte (Main's own read is verified against it in the log),
	// writes/RMWs SoftQuit when Main and Twin disagree. --no-twin:
	// host.Read/Write.
	static inline uint8_t read_io_exact(Host & host, uint16_t pc,
	                                    uint16_t pc_after, uint16_t io_addr) {
		if (host.NoTwin()) {
			const uint8_t v = host.Read(io_addr);
			REVM_LOG_M("sync", REVM_VERBOSE,
			         "ReadIoAtPc pc=$%04X addr=$%04X -> $%02X (no-twin)",
			         pc, io_addr, v);
			return v;
		}

		// A paired walk may cross VBLANK (HitVSync): retry — the boards
		// stay frame-locked, so continuing reaches pc_after.
		AdvanceResult r = AdvanceResult::Timeout;
		for (int attempt = 0; attempt < 3; ++attempt) {
			r = TwinRunToPc(host, pc_after, true, false, true);
			if (r == AdvanceResult::Ok)
				break;
			if (r != AdvanceResult::HitVSync)
				break;
		}
		if (r != AdvanceResult::Ok) {
			ev_fence(host, "io_read", pc,
			         r == AdvanceResult::Timeout ? "timeout" : "miss", false);
			REVM_LOG_M("sync", REVM_ERROR,
			         "ReadIoAtPc TwinRunToPc failed pc=$%04X pc_after=$%04X "
			         "addr=$%04X twin_pc=$%04X result=%s",
			         pc, pc_after, io_addr, host.TwinCpu().pc,
			         revm::AdvanceResultName(r));
			host.SoftQuit(1, "ReadIoAtPc TwinRunToPc $%04X failed", pc);
		}

		Host::TwinIoAccess cached{};
		const bool have = host.TwinLastIoRead(cached);
		if (!have || !io_addr_same(cached.addr, io_addr)) {
			REVM_LOG_M("sync", REVM_ERROR,
			         "ReadIoAtPc I/O cache %s pc=$%04X expected=$%04X "
			         "cached=$%04X",
			         have ? "addr mismatch" : "missing", pc, io_addr,
			         have ? cached.addr : 0);
			host.SoftQuit(1,
			              "ReadIoAtPc I/O cache %s pc=$%04X expected=$%04X "
			              "cached=$%04X",
			              have ? "addr mismatch" : "missing", pc, io_addr,
			              have ? cached.addr : 0);
		}

		const uint8_t main_v = host.Read(io_addr);
		if (main_v != cached.value) {
			REVM_LOG_M("sync", REVM_VERBOSE,
			         "ReadIoAtPc differ pc=$%04X addr=$%04X twin=$%02X "
			         "main=$%02X",
			         pc, io_addr, cached.value, main_v);
		} else {
			REVM_LOG_M("sync", REVM_VERBOSE,
			         "ReadIoAtPc pc=$%04X addr=$%04X -> $%02X", pc, io_addr,
			         cached.value);
		}
		return cached.value;
	}

	static inline uint8_t ReadIoAtPc(Host & host, uint16_t pc, uint16_t io_addr) {
		if (host.NoTwin())
			return read_io_exact(host, pc, pc, io_addr);

		const uint16_t pc_after = io_pc_after(host, pc);
		const uint8_t v = read_io_exact(host, pc, pc_after, io_addr);
		ev_fence(host, "io_read", pc, "ok", false);
		return v;
	}

	static inline void WriteIoAtPc(Host & host, uint16_t pc, uint16_t io_addr,
	                               uint8_t value) {
		if (host.NoTwin()) {
			host.Write(io_addr, value);
			REVM_LOG_M("sync", REVM_VERBOSE,
			         "WriteIoAtPc pc=$%04X addr=$%04X val=$%02X (no-twin)",
			         pc, io_addr, value);
			return;
		}

		const uint16_t pc_after = io_pc_after(host, pc);
		AdvanceResult r = AdvanceResult::Timeout;
		for (int attempt = 0; attempt < 3; ++attempt) {
			r = TwinRunToPc(host, pc_after, true, false, true);
			if (r == AdvanceResult::Ok)
				break;
			if (r != AdvanceResult::HitVSync)
				break;
		}
		if (r != AdvanceResult::Ok) {
			ev_fence(host, "io_write", pc,
			         r == AdvanceResult::Timeout ? "timeout" : "miss", false);
			REVM_LOG_M("sync", REVM_ERROR,
			         "WriteIoAtPc TwinRunToPc failed pc=$%04X pc_after=$%04X "
			         "addr=$%04X twin_pc=$%04X result=%s",
			         pc, pc_after, io_addr, host.TwinCpu().pc,
			         revm::AdvanceResultName(r));
			host.SoftQuit(1, "WriteIoAtPc TwinRunToPc $%04X failed", pc);
		}

		Host::TwinIoAccess cached{};
		const bool have = host.TwinLastIoWrite(cached);
		if (!have || !io_addr_same(cached.addr, io_addr)) {
			REVM_LOG_M("sync", REVM_ERROR,
			         "WriteIoAtPc I/O cache %s pc=$%04X expected=$%04X "
			         "cached=$%04X",
			         have ? "addr mismatch" : "missing", pc, io_addr,
			         have ? cached.addr : 0);
			host.SoftQuit(1,
			              "WriteIoAtPc I/O cache %s pc=$%04X expected=$%04X "
			              "cached=$%04X",
			              have ? "addr mismatch" : "missing", pc, io_addr,
			              have ? cached.addr : 0);
		}
		if (cached.value != value) {
			REVM_LOG_M("sync", REVM_ERROR,
			         "WriteIoAtPc value mismatch pc=$%04X addr=$%04X "
			         "twin=$%02X main=$%02X",
			         pc, io_addr, cached.value, value);
			host.SoftQuit(1,
			              "WriteIoAtPc value mismatch pc=$%04X addr=$%04X "
			              "twin=$%02X main=$%02X",
			              pc, io_addr, cached.value, value);
		}

		host.Write(io_addr, value);
		ev_fence(host, "io_write", pc, "ok", false);
		REVM_LOG_M("sync", REVM_VERBOSE,
		         "WriteIoAtPc pc=$%04X addr=$%04X val=$%02X", pc, io_addr,
		         value);
	}

	// Official memory RMW on I/O. One Twin walk. Inject Main's read at
	// Twin's operand Φ2, then apply(op) at Twin's last write Φ2. SoftQuit
	// if Main and Twin disagree. ASL/LSR/ROL/ROR require bool *carry.
	// Same AllowOneVSync VBLANK skip as ReadIoAtPc. --no-twin: ordinary apply.
	static inline uint8_t rmw_io_exact(Host & host, uint16_t pc,
	                                   uint16_t pc_after, uint16_t io_addr,
	                                   RmwOp op, bool *carry) {
		rmw_check_carry(host, op, carry);
		const bool cin = rmw_uses_carry(op) ? *carry : false;
		if (host.NoTwin()) {
			const RmwApply a = apply_rmw(host.Read(io_addr), op, cin);
			host.Write(io_addr, a.value);
			if (carry)
				*carry = a.cout;
			REVM_LOG_M("sync", REVM_VERBOSE,
			         "RmwIoAtPc %s pc=$%04X addr=$%04X -> $%02X (no-twin)",
			         rmw_op_name(op), pc, io_addr, a.value);
			return a.value;
		}

		AdvanceResult r = AdvanceResult::Timeout;
		for (int attempt = 0; attempt < 3; ++attempt) {
			r = TwinRunToPc(host, pc_after, true, false, true);
			if (r == AdvanceResult::Ok)
				break;
			if (r != AdvanceResult::HitVSync)
				break;
		}
		if (r != AdvanceResult::Ok) {
			ev_fence(host, "io_rmw", pc,
			         r == AdvanceResult::Timeout ? "timeout" : "miss", false);
			REVM_LOG_M("sync", REVM_ERROR,
			         "RmwIoAtPc TwinRunToPc failed pc=$%04X pc_after=$%04X "
			         "addr=$%04X twin_pc=$%04X result=%s",
			         pc, pc_after, io_addr, host.TwinCpu().pc,
			         revm::AdvanceResultName(r));
			host.SoftQuit(1, "RmwIoAtPc TwinRunToPc $%04X failed", pc);
		}

		Host::TwinIoAccess rd{};
		Host::TwinIoAccess wr{};
		const bool have_rd = host.TwinLastIoRead(rd);
		const bool have_wr = host.TwinLastIoWrite(wr);
		if (!have_rd || !io_addr_same(rd.addr, io_addr)) {
			host.SoftQuit(1,
			              "RmwIoAtPc read cache %s pc=$%04X expected=$%04X "
			              "cached=$%04X",
			              have_rd ? "addr mismatch" : "missing", pc, io_addr,
			              have_rd ? rd.addr : 0);
		}
		if (!have_wr || !io_addr_same(wr.addr, io_addr)) {
			host.SoftQuit(1,
			              "RmwIoAtPc write cache %s pc=$%04X expected=$%04X "
			              "cached=$%04X",
			              have_wr ? "addr mismatch" : "missing", pc, io_addr,
			              have_wr ? wr.addr : 0);
		}
		const RmwApply twin_a = apply_rmw(rd.value, op, cin);
		if (wr.value != twin_a.value) {
			host.SoftQuit(1,
			              "RmwIoAtPc Twin RMW != %s pc=$%04X addr=$%04X "
			              "read=$%02X write=$%02X",
			              rmw_op_name(op), pc, io_addr, rd.value, wr.value);
		}

		// Main's chip access at the aligned instant; the read must match
		// Twin's operand and the result must match Twin's RMW.
		const uint8_t main_old = host.Read(io_addr);
		if (main_old != rd.value) {
			REVM_LOG_M("sync", REVM_ERROR,
			         "RmwIoAtPc read mismatch pc=$%04X addr=$%04X "
			         "twin=$%02X main=$%02X",
			         pc, io_addr, rd.value, main_old);
			host.SoftQuit(1,
			              "RmwIoAtPc read mismatch pc=$%04X addr=$%04X "
			              "twin=$%02X main=$%02X",
			              pc, io_addr, rd.value, main_old);
		}
		const RmwApply main_a = apply_rmw(main_old, op, cin);
		if (main_a.value != wr.value) {
			REVM_LOG_M("sync", REVM_ERROR,
			         "RmwIoAtPc write mismatch pc=$%04X addr=$%04X "
			         "twin=$%02X main=$%02X",
			         pc, io_addr, wr.value, main_a.value);
			host.SoftQuit(1,
			              "RmwIoAtPc write mismatch pc=$%04X addr=$%04X "
			              "twin=$%02X main=$%02X",
			              pc, io_addr, wr.value, main_a.value);
		}

		host.Write(io_addr, main_a.value);
		if (carry)
			*carry = main_a.cout;
		REVM_LOG_M("sync", REVM_VERBOSE,
		         "RmwIoAtPc %s pc=$%04X addr=$%04X $%02X->$%02X",
		         rmw_op_name(op), pc, io_addr, main_old, main_a.value);
		return main_a.value;
	}

	static inline uint8_t RmwIoAtPc(Host & host, uint16_t pc, uint16_t io_addr,
	                                RmwOp op, bool *carry) {
		if (host.NoTwin())
			return rmw_io_exact(host, pc, pc, io_addr, op, carry);

		const uint16_t pc_after = io_pc_after(host, pc);
		const uint8_t v = rmw_io_exact(host, pc, pc_after, io_addr, op, carry);
		ev_fence(host, "io_rmw", pc, "ok", false);
		return v;
	}

	// --- RAM at Twin's opcode. Yield-at-target; Main's byte (not TwinPeek). ---
	// `pc` is the opcode of the load/ALU/store that touches `addr`.
	// TwinRunToPc yields a pending accept at that fetch so the
	// handler can JoinAtPc / nest before the sample or store. Returns/writes
	// host.Read/Write — Main's RAM after that steal. Not a compare. IRQ/NMI
	// bodies may call these via mem.*. Same one-VSYNC contract as
	// JoinAtPc (nested HitVSync extra-VSYNCs once if Twin is still in the
	// nest), except the I/O-on-VBLANK-Φ2 case is silent for RAM fences.
	// FenceSlack::Exact: extra VSYNC is a failure.
	// --no-twin: host.Read / host.Write.
	static inline void ramFenceExact(Host & host, uint16_t pc,
	                                 const char * tag, const char * op) {
		if (host.NoTwin())
			return;

		const AdvanceResult r = TwinRunToPc(host, pc, false, true);
		if (r != AdvanceResult::Ok) {
			check_nested_walk(host, tag, pc, r, false);
			ev_fence(host, op, pc,
			         r == AdvanceResult::Timeout ? "timeout" : "miss", false);
			const auto tw = host.TwinCpu();
			REVM_LOG_M("sync", REVM_ERROR,
			         "%s Exact failed want=$%04X twin_pc=$%04X result=%s "
			         "irq=%d nmi=%d",
			         tag, pc, tw.pc, revm::AdvanceResultName(r),
			         int(tw.irq_pending),
			         int(tw.nmi_pending));
			host.SoftQuit(1, "%s Exact TwinRunToPc $%04X failed", tag, pc);
		}

		align_after_arrival(host, tag, pc);
	}

	// Returns true when the fence crossed a VBLANK (extra-VSYNC path).
	static inline bool ramFence(
		Host & host, uint16_t pc, const char * tag, const char * op,
		FenceSlack slack = FenceSlack::AllowOneVSync) {
		if (host.NoTwin())
			return false;
		if (slack == FenceSlack::Exact) {
			ramFenceExact(host, pc, tag, op);
			return false;
		}

		const AdvanceResult r = TwinRunToPc(host, pc, true, true);
		bool extra = false;
		if (r != AdvanceResult::Ok) {
			extra = true;
			check_nested_walk(host, tag, pc, r, true);
			// RAM-fence caller class: the TwinIoLandedOnLastPhi2 clause of
			// require_nested_extra_vsync_ok is downgraded to the standard
			// silent nested extra VSYNC. Safe because a RAM fence never
			// consumes the I/O cache (no inject-at-cached-cycle, no
			// TwinLastIo* operand): Main samples its own board after Φ2
			// alignment and a wrong byte still surfaces at the next kb
			// compare — no channel is dropped. This resolves the four-point
			// asymmetry (ReadIo/RmwIo already skip the aligned case; Write*
			// did not) via the user-approved 2026-08-23 §14 decision.
			// NOTE: the clause's original protective intent is undocumented
			// in the tree; it is preserved unchanged for join fences, where
			// its refusal remains in force.
			require_nested_extra_vsync_ok(host, tag, pc, FenceClass::Ram);
			if (r == AdvanceResult::HitVSync && !host.TwinAtOpcodeFetch())
				(void)host.TwinFinishInstruction();
			const auto tw = host.TwinCpu();
			// RAM has no inject-at-cached-cycle. Twin at `pc` on the next
			// frame still needs extra Main VSYNC before Φ2-align. The I/O
			// skip (already at pc_after on the VBLANK Φ2) does not apply.
			REVM_LOG_M("sync", REVM_DEBUG,
			         "%s extra VSYNC pc=$%04X twin_pc=$%04X irq=%d nmi=%d "
			         "nested=%d",
			         tag, pc, tw.pc, int(tw.irq_pending), int(tw.nmi_pending),
			         int(in_handler(host)));
			run_extra_main_vsync(host, tag, "ram");
		}
		ramFenceExact(host, pc, tag, op);
		return extra;
	}

	static inline uint8_t ReadRamAtPc(
		Host & host, uint16_t pc, uint16_t addr,
		FenceSlack slack = FenceSlack::AllowOneVSync) {
		if (host.NoTwin()) {
			const uint8_t v = host.Read(addr);
			REVM_LOG_M("sync", REVM_VERBOSE,
			         "ReadRamAtPc pc=$%04X addr=$%04X -> $%02X (no-twin)",
			         pc, addr, v);
			return v;
		}
		const bool extra = ramFence(host, pc, "ReadRamAtPc", "ram_read", slack);
		const uint8_t v = host.Read(addr);
		ev_fence(host, "ram_read", pc, extra ? "hit_vsync" : "ok", extra);
		REVM_LOG_M("sync", REVM_VERBOSE,
		         "ReadRamAtPc pc=$%04X addr=$%04X main=$%02X", pc, addr, v);
		return v;
	}

	static inline void WriteRamAtPc(
		Host & host, uint16_t pc, uint16_t addr, uint8_t value,
		FenceSlack slack = FenceSlack::AllowOneVSync) {
		if (host.NoTwin()) {
			host.Write(addr, value);
			REVM_LOG_M("sync", REVM_VERBOSE,
			         "WriteRamAtPc pc=$%04X addr=$%04X val=$%02X "
			         "(no-twin)",
			         pc, addr, value);
			return;
		}
		const bool extra =
			ramFence(host, pc, "WriteRamAtPc", "ram_write", slack);
		host.Write(addr, value);
		ev_fence(host, "ram_write", pc, extra ? "hit_vsync" : "ok", extra);
		REVM_LOG_M("sync", REVM_VERBOSE,
		         "WriteRamAtPc pc=$%04X addr=$%04X val=$%02X", pc, addr,
		         value);
	}

	// Official memory RMW on RAM. Yield at the opcode fetch so IRQ/NMI can
	// nest *before* the RMW. Then Main
	// apply(op). Do not TwinPeek or SoftQuit on Twin's byte — later kb.
	// ASL/LSR/ROL/ROR require bool *carry. --no-twin: ordinary apply.
	static inline uint8_t RmwRamAtPc(
		Host & host, uint16_t pc, uint16_t addr, RmwOp op, bool *carry,
		FenceSlack slack = FenceSlack::AllowOneVSync) {
		rmw_check_carry(host, op, carry);
		const bool cin = rmw_uses_carry(op) ? *carry : false;
		if (host.NoTwin()) {
			const RmwApply a = apply_rmw(host.Read(addr), op, cin);
			host.Write(addr, a.value);
			if (carry)
				*carry = a.cout;
			REVM_LOG_M("sync", REVM_VERBOSE,
			         "RmwRamAtPc %s pc=$%04X addr=$%04X -> $%02X (no-twin)",
			         rmw_op_name(op), pc, addr, a.value);
			return a.value;
		}
		const bool extra = ramFence(host, pc, "RmwRamAtPc", "ram_rmw", slack);
		const uint8_t old = host.Read(addr);
		const RmwApply a = apply_rmw(old, op, cin);
		host.Write(addr, a.value);
		if (carry)
			*carry = a.cout;
		ev_fence(host, "ram_rmw", pc, extra ? "hit_vsync" : "ok", extra);
		REVM_LOG_M("sync", REVM_VERBOSE,
		         "RmwRamAtPc %s pc=$%04X addr=$%04X $%02X->$%02X",
		         rmw_op_name(op), pc, addr, old, a.value);
		return a.value;
	}
};

} // namespace revm::cpumock
