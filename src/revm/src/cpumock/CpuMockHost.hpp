// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "cpumock/LinkedRegistry.hpp"
#include "core/Board.hpp"
#include "core/Config.hpp"
#include "core/RunSetup.hpp"
#include "debug/KbCheck.hpp"
#include "goldens/CompareReport.hpp"
#include "goldens/PlayPlayer.hpp"
#include "input/GoldenInput.hpp"
#include "input/IInputSource.hpp"
#include "input/JoystickConfig.hpp"
#include "twin/TwinBoard.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace revm { class PlayRecorder; }

namespace revm::cpumock {

struct Sync;
namespace video_detail { struct HostAccess; }
	template <typename Policy> struct ByteAt;
	template <typename Policy, uint16_t Addr> struct FixedByte;
	template <typename Derived> struct LocalRmwOps;
	template <uint16_t Addr> struct Mem;
template <uint16_t Addr, uint16_t Size> struct MemTable;
template <uint16_t Addr> struct Mem16;
template <uint16_t Addr> struct Io;

class CpuMockHost;

// Provided by the Stage 3 game (work/<Game>/Stage3/plugin.cpp). Sets the
// entry handler (and any other host wiring) before handoff.
void InstallGame(CpuMockHost & host);

// SoftQuit / QuitOnCheck / RequestQuit during AdvanceCycles: abort the Stage 3
// entry handler. RunPlay* catches this so tallies still print. Needed because
// many game AdvanceCycles loops ignore AdvanceResult::Stopped and would otherwise spin.
struct SoftQuitException {
	int code = 1;
};

// Main↔Twin compare mask at a join fence (or Sync::Compare).
// All flags default false: CompareMask{} / None() is PC-only (no compare).
// JoinAtPc / Sync::Compare default to Kb(). All() is screen + sid + vic +
// cia1 + cia2 + kb. vic_state is opt-in (SC internals / raster pipeline).
struct CompareMask {
	bool screen = false;
	bool sid = false;
	bool vic = false;
	bool vic_state = false;
	bool cia1 = false;
	bool cia2 = false;
	bool kb = false;

	static CompareMask None() { return {}; }
	static CompareMask Kb() { return {.kb = true}; }
	static CompareMask All() {
		return {.screen = true,
		        .sid = true,
		        .vic = true,
		        .cia1 = true,
		        .cia2 = true,
		        .kb = true};
	}
	bool any() const {
		return screen || sid || vic || vic_state || cia1 || cia2 || kb;
	}
};

// Join / AtPc slack: allow one extra Main VSYNC if Twin misses the fence
// (silent bookkeeping), or require an exact same-frame arrival.
enum class FenceSlack { AllowOneVSync, Exact };

// Whether a bounded cycle advance may cross a VSYNC boundary.
enum class VSyncPolicy { Cross, Stop };

// Configuration for a playback embedded in another runner.  Keep these
// hooks together so a reference runner is configured before preparation,
// rather than through a sequence of temporal setters.
struct EmbeddedRunOptions {
	BoardInitRole board_init_role = BoardInitRole::Primary;
	bool own_clock_source = true;
	std::function<bool()> quit_probe;
	std::function<void()> before_entry;
};

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

// Explicit origin for a Stage 4.5 Main that does not restore BEGIN memory
// or chips wholesale. Timeline fields align recorded input and Twin fences.
// Processor-port and interrupt fields are the mock-CPU context that remains
// observable. Optional CIA live phase seeds in-flight timers so blank Main
// matches a BEGIN-restored no-Twin Main.
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

// Stage-3/4 cpumock host: normally restore BEGIN; Stage 4.5 may start Main
// intrinsically while Twin alone restores BEGIN. Mock the 6510 forever.
// Single-threaded: EntryPoint drives Main/Twin via blocking advance calls.
// Steppers yield HitIrq/HitNmi at an accept; Sync retries and calls
// RunIrqHandler / RunNmiHandler. Twin-run does not nest.
class CpuMockHost {
public:
	using IrqHandler = std::function<void()>;
	using NmiHandler = std::function<void()>;
	using EntryHandler = std::function<void()>;
	using AdvanceResult = revm::AdvanceResult;

	// Presence predicates let call sites state either branch without double
	// negatives. NoTwin mirrors the command-line spelling (`--no-twin`).
	bool HasTwin() const { return bool(twin_); }
	bool NoTwin() const { return !HasTwin(); }

	// `QuitRequested()` may be queried in long running tight loops to allow
	// program to quit. Prefer avoiding such loops.
	bool QuitRequested() const;

	// --- IRQ / NMI. Sync dispatches Run*Handler; plugins install thunks
	// (C++ handler bodies — not Twin executing the original 6510 ISR). ---
	void InstallIrqHandler(IrqHandler handler);
	void InstallNmiHandler(NmiHandler handler);
	// Interrupt regime is chip-driven on both boards, always: Main's chips
	// raise its line; Twin's chips raise its own. Handler entry happens only
	// when a Twin-led walk crosses Twin's accept; enter_interrupt() then
	// verifies Main's line fired within the skew window and aligns Φ2.
	void Sei(); // mock-CPU I flag
	void Cli();

	// Log Error, RequestQuit, then throw SoftQuitException (unconditional).
	void SoftQuit(int code, const char * fmt, ...)
		__attribute__((format(printf, 3, 4)));

	// BEGIN tripwire (QuitOnAssert): Twin PC + Φ2 lock. Replaces
	// AssertTwinPc + AssertVsTwinCycleSync.
	void AssertBegin(uint16_t pc);

	// Oracle door — logs/diagnostics, not translation. Use inside REVM_LOG*
	// arguments; lint S3-014 flags other uses.
	struct Diag {
		CpuMockHost & h;
		CpuState TwinCpu() { return h.TwinCpu(); }
		uint8_t TwinPeek(uint16_t addr) { return h.TwinPeek(addr); }
		uint8_t PeekMain(uint16_t addr) { return h.PeekMain(addr); }
		void WatchTwinPc(uint16_t pc, const char * label = nullptr) {
			h.WatchTwinPc(pc, label); // idempotent: same pc, keep/refresh label
		}
		bool LogVsTwinIrqSources(const char * why) {
			return h.LogVsTwinIrqSources(why);
		}
		bool AlignMainCiaPhaseToTwin() {
			return h.MainTakeTwinPhaseCia1() && h.MainTakeTwinPhaseCia2();
		}
	};
	Diag diag() { return Diag{*this}; }

	// --- Run entry (BEGIN restore or declared blank start → plugin EntryPoint). ---
	// Game main after handoff (coldstart → title → …). Same thread.
	// Stage 3/4 plugins drive the machine through Sync (Twin walks stay
	// there).
	void SetEntryHandler(EntryHandler handler) {
		entry_handler_ = std::move(handler);
	}
	void InstallMainStart(CpuMockStart start) { main_start_ = start; }
	// Dispatching Main-only clock and interrupt completion. These are the
	// no-Twin semantics previously reached only through Sync: every device
	// advances, and IRQ/NMI handlers run when chips raise a line. Twin
	// walks stay on Sync. AdvanceCycles / AdvanceToVSync / AdvanceFrames /
	// ReturnIrq / ReturnNmi SoftQuit if Twin is present so a plugin cannot
	// silently skip the oracle.
	AdvanceResult AdvanceCycles(uint64_t cycles,
	                            VSyncPolicy vsync_policy = VSyncPolicy::Cross);
	AdvanceResult AdvanceToVSync();
	uint32_t AdvanceFrames(uint32_t frames);
	void ReturnIrq(uint32_t body_cycles);
	void ReturnNmi(uint32_t cycles);

	Board & MainBoard() { return board_; }
	const Board & MainBoard() const { return board_; }

	// Stage-4 detached state registry. The host owns it so linked fields cannot
	// accidentally bind to another CpuMockHost's AtPc ticket.
	LinkedRegistry & Links() { return linked_registry_; }
	const LinkedRegistry & Links() const { return linked_registry_; }
	bool OwnsLinkedRegistry(const LinkedRegistry * registry) const {
		return registry == &linked_registry_;
	}

	bool PreparePlayback(const std::string & play_path, uint32_t max_frames = 0,
	                     bool headless = true, bool limit_speed = false,
	                     bool audio = false, const PlaybackCompareOpts & compare = {},
	                     const Config * user_cfg = nullptr,
	                     EmbeddedRunOptions embedded = {});
	bool PrepareLive(IInputSource * input, uint32_t max_frames = 0,
	                 bool headless = true, bool limit_speed = false,
	                 bool audio = false, const PlaybackCompareOpts & compare = {},
	                 const Config * user_cfg = nullptr,
	                 EmbeddedRunOptions embedded = {});
	int RunPreparedPlayback();

	int RunPlayback(const std::string & play_path, uint32_t max_frames = 0,
	                bool headless = true, bool limit_speed = false,
	                bool audio = false, const PlaybackCompareOpts & compare = {},
	                const Config * user_cfg = nullptr);

	int RunPlay(const std::string & prg_path, IInputSource * input,
	            const JoystickConfig * joy = nullptr, uint32_t max_frames = 0,
	            bool headless = false, bool limit_speed = true, bool audio = true,
	            const PlaybackCompareOpts & compare = {},
	            const Config * user_cfg = nullptr,
	            ::revm::PlayRecorder * recorder = nullptr);

private:
	template <typename Policy> friend struct ByteAt;
	template <typename Policy, uint16_t Addr> friend struct FixedByte;
	template <typename Derived> friend struct LocalRmwOps;
	template <uint16_t Addr> friend struct Mem;
	template <uint16_t Addr, uint16_t Size> friend struct MemTable;
	template <uint16_t Addr> friend struct Mem16;
	template <uint16_t Addr> friend struct Io;
	friend struct video_detail::HostAccess;
	friend struct Sync;

	CpuState TwinCpu();
	uint8_t TwinPeek(uint16_t addr);
	uint8_t PeekMain(uint16_t addr);
	// Main's VIC raster line right now (Frodo raster_y; display lines start
	// at $10 on PAL). 0 when no machine is live. Diagnostic/event field only
	// — no compare or timing behavior may read it.
	uint16_t RasterLine() const;
	// Note the most recent Sync fence (op string is a literal) so a
	// fail-context record can name the fence the failing compare belongs to.
	void note_fence(const char * op, uint16_t pc);
	uint64_t TwinWatchHits(uint16_t pc);
	bool MainTakeTwinPhaseCia1();
	bool MainTakeTwinPhaseCia2();
	bool IFlag() const;
	bool InIrq() const { return irq_depth_ > 0; }
	bool InNmi() const { return nmi_depth_ > 0; }
	bool TwinInInterruptNest() const;
	void Quit(int code = 1);
	void WatchTwinPc(uint16_t pc, const char * label = nullptr);
	void ClearTwinPcWatches();
	bool AssertTwinMem(uint16_t addr, uint8_t expected);
	bool AssertTwinMemRange(uint16_t lo, uint16_t hi, const uint8_t * expected);
	bool AssertTwinPc(uint16_t pc);
	bool AssertVsTwinCycleSync();
	bool AssertKbCheck();
	bool AssertVsTwin(uint16_t addr);
	bool AssertVsTwinRange(uint16_t lo, uint16_t hi);
	bool AssertVsTwinIrqSources();
	bool LogVsTwinIrqSources(const char * why);

	uint8_t TwinPeekMem(uint16_t addr);
	bool TwinAtOpcodeFetch() const;
	// Raw steppers: yield HitIrq/HitNmi and stop. Sync dispatches.
	// LeftNest = Twin left this accept (not VBLANK). HitVSync is VBLANK only.
	// pair_main: step Main 1:1 (chip-time lock); false parks Main for the
	// I/O AtPc inject walk.
	AdvanceResult TwinRunToPc(uint16_t pc, bool soft_fail = false,
	                          bool yield_at_target = false,
	                          bool pair_main = true);
	AdvanceResult twin_run_to_pc_inner(uint16_t pc, bool soft_fail,
	                                   bool yield_at_target, bool pair_main);
	// Outstanding Sync walk fences. TwinRunToPc registers {pc,
	// yield_at_target} for the duration of the walk; an exit via HitIrq/
	// HitNmi SUSPENDS the registration instead of popping it (the Sync
	// dispatch layer retries the identical walk after the handler), so
	// interrupt teardown can recognize an opcode fetch a suspended fence
	// still has to pair on. Depth beyond kMaxWalkFences degrades to the
	// unregistered (legacy) behavior.
	struct WalkFence {
		uint16_t pc = 0;
		bool yield_at_target = false;
		bool active = false;
	};
	static constexpr int kMaxWalkFences = 8;
	WalkFence walk_fences_[kMaxWalkFences] = {};
	int walk_fences_n_ = 0;
	// True when Twin stands at an opcode fetch that some SUSPENDED
	// yield-at-target walk fence still has to pair on.
	bool twin_fenced_fetch_pending() const;
	// True when Twin stands ON a fresh interrupt accept boundary (pending
	// fetch or already-entered steal sequence) that no active dispatch
	// owns: another Twin-only step here would consume the accept NATIVELY
	// instead of yielding it for a kind-checked dispatch. Teardown
	// leveling / residue closure must park at such a boundary and leave it
	// to the post-return follow-up dispatcher. Conservative parking test
	// only — the boundary's final kind always comes from
	// twin_resolve_accept_kind below.
	bool twin_unowned_accept_boundary() const;
	// --- Resolve-then-classify: THE one accept-kind door. ---
	// Invariant (game-agnostic): an interrupt accept's kind is committed
	// only AFTER Twin's emulated CPU has begun the actual interrupt
	// sequence far enough that the entered VECTOR is observable — never
	// predicted from pending flags at detection time. The emulated core
	// re-vectors an in-flight IRQ steal to the NMI vector mid-sequence
	// when the NMI edge lands during the first sequence cycles (clearing
	// both pending flags without ever entering the dedicated NMI state),
	// so detection-time labels can be wrong. Every yield / owned-swallow /
	// follow-up decision keys off the RESOLVED accept, and each resolved
	// accept dispatches exactly once through the existing kind-checked
	// Run*Handler doors. Resolution steps Twin only (Main stays parked);
	// enter_interrupt then pairs Main against the ORIGINAL boundary cycle
	// recorded here.
	//
	// Raw sequence-state families of the emulated 6510 core:
	//   IRQ: O_IRQ + its push/vector-tail states ($FFFE path)
	//   NMI: O_NMI body + the shared $FFFA vector tail (also the hijack
	//        target). States before the vector decision are UNRESOLVED:
	//        the hijack can still happen.
	int twin_hw_seq_state() const;
	bool twin_in_irq_family() const;
	bool twin_in_nmi_family() const;
	// True when the current CPU state pins the entered vector; `nmi` says
	// which. False while still inside the hijack window / pre-sequence.
	bool twin_accept_kind_observed(bool & nmi) const;
	// Step Twin (Main parked) until the standing accept's kind is
	// observable. Records the boundary cycle for enter_interrupt's skew
	// verdict. Returns false if there is nothing resolvable standing.
	bool twin_resolve_accept_kind(bool & nmi);
	// Resolution steps Twin-only; every resolved accept that is NOT
	// immediately dispatched (owned coalesce / kind-mismatch follow-up)
	// must restore Φ2 lockstep right away — Main catches up to Twin's
	// post-resolution position — or the boards' chip phases and the
	// input-application instant drift against Twin's poll grid. Dispatch
	// paths are repaired at the end of enter_interrupt instead (after the
	// verdict has burned Main to the recorded boundary).
	void twin_repair_resolve_gap();
	// Boundary (steal) cycle of the accept currently being resolved;
	// kNoSince when none. Set by twin_resolve_accept_kind, consumed and
	// cleared by enter_interrupt, cleared on owned-swallow outcomes.
	uint32_t resolving_steal_ = kNoSince;
	AdvanceResult TwinFinishInstruction();
	// ReturnIrq only: execute the RTI currently parked at its opcode fetch
	// and report LeftNest after the active IRQ token observes completion.
	AdvanceResult TwinExecuteIrqRti();
	using TwinIoAccess = TwinBoard::IoAccess;
	bool TwinLastIoRead(TwinIoAccess & out) const;
	bool TwinLastIoWrite(TwinIoAccess & out) const;
	uint8_t MainInjectReadAtCycle(uint32_t cycle, uint16_t addr);
	void MainInjectWriteAtCycle(uint32_t cycle, uint16_t addr, uint8_t value);
	AdvanceResult main_advance_to_vsync();
	AdvanceResult MainRunToTwinCycle();
	void CompareNow(CompareMask mask);
	void RunIrqHandler();
	void RunNmiHandler();
	// The active token still obligates or tracks Twin for this accept.
	// Public read-only twin probe above; mutation stays private.
	// Not the I flag — a new accept after RTI and title SEI both have I=1.
	bool TwinInIrqSequence() const;
	bool TwinInNmiSequence() const;
	bool TwinIoLandedOnLastPhi2() const;
	// IRQ RTI-equivalent: `body_cycles` + 8 Φ2 IR-clear, then I=0.
	// Caller ACK in C++ then Sync::ReturnIrq. `cycles == 0` is IR-clear only.
	void FinishMockIrq(uint32_t body_cycles);
	// NMI duration when Twin is not the oracle. `cycles == 0` is a no-op.
	void FinishMockNmi(uint32_t cycles);
	void AddMockIrqHold(uint32_t cycles);
	bool IrqHoldActive() const;

	// C64 map through Main's mock 6510 (RAM/ROM/IO banking). Plugins use
	// Mem / Mem16 / Io; raw Read/Write from a replica is a compile error.
	uint8_t Read(uint16_t addr);
	void Write(uint16_t addr, uint8_t value);
	// Setup-only copy to Main's physical RAM. Unlike Write(), this deliberately
	// bypasses the 6510 ROM/I/O banking used by live translated accesses.
	void InstallAsset(uint16_t addr, std::span<const uint8_t> bytes);

	// Default C++ IRQ/NMI entry (plugin Install*Handler body). Twin still
	// runs the original 6510 ISR on its own board.
	static void MockIrqThunk(void * userdata);
	static void MockNmiThunk(void * userdata);
	void dispatch_irq();
	void dispatch_nmi();

	void enable_mock_cpu();
	bool start_blank_main(GoldenInput * golden_input);
	void wire_defaults();
	void apply_user_cfg(Config & cfg, const Config * user_cfg);
	void setup_kb_check();
	bool handoff_from_begin_snap(GoldenInput * golden_input);
	bool setup_twin();
	const std::string & begin_snap_path() const;
	bool main_take_twin_phase_cia(int which);
	void main_run_to_io_cycle(uint32_t cycle);
	void main_emulate_inject_cycle(const std::function<void()> & cpu_slot);
	// Log + hard Quit(1) unless --ignore-asserts.
	void QuitOnAssert(const char * fmt, ...) __attribute__((format(printf, 2, 3)));
	// Log + SoftQuit (throw) unless --ignore-checks.
	void QuitOnCheck(const char * fmt, ...) __attribute__((format(printf, 2, 3)));
	void maybe_fail_hint();
	void ThrowIfQuitRequested();
	bool ignore_asserts() const;
	bool ignore_checks() const;

	// Twin leads Main within a frame (lockstep); frames re-align at barriers.
	AdvanceResult twin_guard_before_advance();
	// Board frame-boundary: tallies + ApplyInput (Twin tracks Main in
	// lockstep; input lands on both boards at the same barrier).
	void on_main_vsync(uint32_t frame, uint32_t cycle, const InputFrame & in);
	void compare_screen(uint32_t frame, RitualCheck & out);
	void compare_sid(uint32_t frame, RitualCheck & out);
	bool capture_compare_chips(ChipSnapshot & main_chip, ChipSnapshot & twin_chip,
	                           uint32_t frame);
	void compare_vic(uint32_t frame, RitualCheck & out);
	void compare_vic_state(uint32_t frame, RitualCheck & out);
	void compare_cia(int which, uint32_t frame, RitualCheck & out);
	void print_compare_tallies() const;
	void finish_media();
	void tally_tick(uint32_t frame);
	void pace_after_vsync();
	// Twin-led lockstep: alternate Twin/Main Φ2. Yields HitIrq/HitNmi when
	// Twin enters an accept sequence (Main trails ≤1 Φ2), HitVSync on Main's
	// VBLANK (when stop_at_vsync). Records Main line edges each Φ2.
	AdvanceResult lock_step_pair(uint64_t max_cycles, bool stop_at_vsync);
	// Raw Main-only advance (Twin untouched; inject catch-up, --no-twin).
	// Tracks Main line edges; yields Hit* only without Twin.
	AdvanceResult main_advance(uint64_t cycles, bool run_past_vsync = true);
	// EnterInterrupt: Twin is parked at its accept boundary. Burn Main to
	// the steal cycle, take the skew verdict (K_EARLY / K_LATE, type match),
	// consume the pending flag and bind the sequence to this dispatch.
	void enter_interrupt(bool take_nmi);
	// Rising-edge tracking of Main's IRQ/NMI lines (level-mirrored pending).
	void track_main_lines();
	bool twin_at_hw_steal() const;
	bool twin_rti_in_progress() const;
	// Twin-only Φ2 (Main parked; I/O AtPc inject walks).
	bool twin_step_only();
	const char * step_ctx_ = "?";
	// Execute Twin's parked steal sequence so the boundary is consumed.
	void consume_twin_steal();
	// Step Twin one Φ2; close seq-ownership when its RTI completes.
	bool twin_emulate_cycle();

	Board board_;
	EmbeddedRunOptions embedded_;
	PlayPlayer playback_player_;
	std::string playback_path_;
	uint32_t playback_max_frames_ = 0;
	bool playback_prepared_ = false;
	bool prepared_live_ = false;
	EntryHandler entry_handler_;
	std::optional<CpuMockStart> main_start_;
	IrqHandler irq_handler_;
	NmiHandler nmi_handler_;
	int irq_depth_ = 0;
	int nmi_depth_ = 0;
	bool irq_finish_called_ = false;
	bool nmi_finish_called_ = false;
	uint32_t nmi_hold_cycles_ = 0;

	std::unique_ptr<TwinBoard> twin_;
	// Twin is inside the interrupt sequence owned by the dispatched C++
	// handler (replaces AcceptToken: one live flag per interrupt kind).
	bool irq_seq_active_ = false;
	bool nmi_seq_active_ = false;
	// Main IRQ/NMI line rising-edge cycles (~0u = clear / not yet seen).
	static constexpr uint32_t kNoSince = ~0u;
	uint32_t main_irq_since_ = kNoSince;
	uint32_t main_nmi_since_ = kNoSince;
	bool main_irq_line_ = false;
	bool main_nmi_line_ = false;
	// Previous paired event per kind: Twin steal cycle and Main line edge.
	// The verdict compares INTERVALS (edge delta vs steal delta) — Twin's
	// accept can legitimately lag its line rise while it finishes a long
	// ISR, but the underlying chip schedules must agree.
	uint32_t prev_irq_steal_ = kNoSince;
	uint32_t prev_nmi_steal_ = kNoSince;
	uint32_t prev_irq_edge_ = kNoSince;
	uint32_t prev_nmi_edge_ = kNoSince;
	// Skew verdict window (Φ2). Earliness beyond K_EARLY is a schedule
	// divergence; lateness up to K_LATE is tolerated and measured (chip
	// reloads via unpaired writes shift the next edge), beyond that the
	// source is considered missing.
	static constexpr uint32_t kSkewEarly = 64;
	static constexpr uint32_t kSkewLate = 2048;
	// Lateness grace: Main-only wait before pairing anyway. Timer phase
	// jitter between the boards reaches ~tens of Φ2 between reloads.
	static constexpr uint32_t kSkewLateGrace = 64;
	// Consecutive silent acceptances tolerated before declaring the chip
	// schedule diverged (per kind: [irq, nmi]).
	static constexpr uint32_t kSilentStreak = 3;
	uint32_t silent_streak_[2] = {0, 0};
	// Skew telemetry per kind ([irq, nmi]): paired/silent counts and the
	// signed skew range (positive = Main early). Printed at run end.
	struct SkewStats {
		uint64_t paired = 0;
		uint64_t silent = 0;
		int32_t min = 0;
		int32_t max = 0;
		bool any = false;
	};
	SkewStats skew_stats_[2] = {};
	KbCheck kb_check_;
	LinkedRegistry linked_registry_;
	PlaybackCompareOpts compare_opts_{};
	IgnoreTally screen_tally_{};
	IgnoreTally sid_tally_{};
	IgnoreTally vic_tally_{};
	IgnoreTally vic_state_tally_{};
	IgnoreTally cia1_tally_{};
	IgnoreTally cia2_tally_{};
	RitualCheck last_screen_{};
	RitualCheck last_sid_{};
	RitualCheck last_vic_{};
	RitualCheck last_vic_state_{};
	RitualCheck last_cia1_{};
	RitualCheck last_cia2_{};
	std::string last_screen_summary_;
	char last_sid_detail_[96]{};
	char last_vic_detail_[96]{};
	char last_cia1_detail_[96]{};
	char last_cia2_detail_[96]{};
	unsigned dump_images_written_ = 0;
	// Screen-fail context state for net_fail / fail_context records; reset
	// at the top of every CompareNow. The bbox is canvas coordinates
	// (inclusive), as in the screen_fail event.
	const char * last_fence_op_ = "none";
	uint16_t last_fence_pc_ = 0;
	unsigned last_screen_pixels_ = 0;
	unsigned last_screen_x0_ = 0;
	unsigned last_screen_y0_ = 0;
	unsigned last_screen_x1_ = 0;
	unsigned last_screen_y1_ = 0;
	std::string last_screen_cells_json_ = "[]";

	// One structured record for a failing compare: which fence it belongs
	// to, what the mask asked for, what actually ran, where the beam was,
	// the bounded recent-event ring, and the kb/screen fail detail.
	void emit_fail_context(CompareMask mask, const RitualCheck & kb_check,
	                       const char * plane);
	// Same record for a fence-miss SoftQuit (JoinAtPc / JoinAtPcBounded):
	// the fence fields name the fence that missed, nothing compared, and
	// the kb/screen fields are null.
	void emit_fence_miss_context(CompareMask mask);

	static constexpr uint32_t kTallyFrames = 50; // ~1 PAL second
	uint32_t tally_frame_ = 0;
	bool tally_started_ = false;
	uint32_t tally_vsyncs_ = 0;
	uint64_t total_compares_ = 0;
	uint64_t total_fails_ = 0;

	bool pace_ = false;
	std::chrono::steady_clock::time_point frame_start_{};
};

} // namespace revm::cpumock
