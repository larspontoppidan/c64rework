// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "snapshot/Snapshot.hpp"
#include "core/Board.hpp"
#include "core/Config.hpp"
#include "util/Hash.hpp"

#include "C64.h"
#include "Display.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace revm {

bool CaptureFullSnapshot(const Board & board, FullSnapshot & out) {
	const C64 * c64 = board.Machine();
	if (!c64) return false;

	std::memset(&out, 0, sizeof(out));
	std::memcpy(out.magic, FullSnapshot::kMagic, 16);
	out.version = FullSnapshot::kVersion;
	out.cycle = board.CycleCounter();
	out.frame = board.FrameCounter();

	std::memcpy(out.ram, c64->RAM, C64_RAM_SIZE);
	std::memcpy(out.color, c64->Color, COLOR_RAM_SIZE);

	c64->TheCPU->GetState(&out.cpu);
	c64->TheVIC->GetState(&out.vic);
	c64->TheSID->GetState(&out.sid);
	c64->TheCIA1->GetState(&out.cia1);
	c64->TheCIA2->GetState(&out.cia2);
	c64->TheVIC->GetScPipeline(&out.vic_sc);
	out.ba_low = c64->TheCPU->BALow ? 1 : 0;
	out.cia1_ta_idle = c64->TheCIA1->TimerAIdle() ? 1 : 0;
	out.cia1_tb_idle = c64->TheCIA1->TimerBIdle() ? 1 : 0;
	out.cia2_ta_idle = c64->TheCIA2->TimerAIdle() ? 1 : 0;
	out.cia2_tb_idle = c64->TheCIA2->TimerBIdle() ? 1 : 0;
	return true;
}

bool CaptureCompareSnapshot(const Board & board, FullSnapshot & out) {
	const C64 * c64 = board.Machine();
	if (!c64) return false;

	// Zero the whole blob (incl. padding) so SHA-256 / memcmp stay deterministic.
	// Trailer stays zero — not compared in Stage-1 playback.
	std::memset(&out, 0, sizeof(out));
	std::memcpy(out.magic, FullSnapshot::kMagic, 16);
	out.version = FullSnapshot::kVersion;
	out.cycle = board.CycleCounter();
	out.frame = board.FrameCounter();

	std::memcpy(out.ram, c64->RAM, C64_RAM_SIZE);
	std::memcpy(out.color, c64->Color, COLOR_RAM_SIZE);

	c64->TheCPU->GetState(&out.cpu);
	c64->TheVIC->GetState(&out.vic);
	c64->TheSID->GetState(&out.sid);
	c64->TheCIA1->GetState(&out.cia1);
	c64->TheCIA2->GetState(&out.cia2);
	return true;
}

bool CaptureCpuView(const Board & board, uint8_t out[C64_RAM_SIZE]) {
	const C64 * c64 = board.Machine();
	if (!c64 || !c64->TheCPU) return false;
	for (uint32_t a = 0; a < C64_RAM_SIZE; ++a) {
		out[a] = c64->TheCPU->REUReadByte(uint16_t(a));
	}
	return true;
}

bool WriteCpuViewFile(const Board & board, const std::string & path, std::string & error) {
	uint8_t buf[C64_RAM_SIZE];
	if (!CaptureCpuView(board, buf)) {
		error = "CaptureCpuView failed";
		return false;
	}
	std::ofstream f(path, std::ios::binary);
	if (!f) {
		error = "cannot write " + path;
		return false;
	}
	f.write(reinterpret_cast<const char *>(buf), C64_RAM_SIZE);
	if (!f) {
		error = "write failed: " + path;
		return false;
	}
	return true;
}

bool WriteCpuViewFromFullSnapshot(const FullSnapshot & snap, const std::string & path,
                                  std::string & error) {
	Board board;
	Config cfg;
	cfg.headless = true;
	cfg.audio_enabled = false;
	cfg.limit_speed = false;
	board.Configure(cfg);
	if (!board.Init(error)) return false;
	if (!RestoreFullSnapshot(board, snap)) {
		error = "RestoreFullSnapshot failed";
		return false;
	}
	return WriteCpuViewFile(board, path, error);
}

bool RestoreFullSnapshot(Board & board, const FullSnapshot & in) {
	C64 * c64 = board.Machine();
	if (!c64) return false;
	if (std::memcmp(in.magic, FullSnapshot::kMagic, 16) != 0) return false;
	if (in.version != 1 && in.version != FullSnapshot::kVersion) return false;

	std::memcpy(c64->RAM, in.ram, C64_RAM_SIZE);
	std::memcpy(c64->Color, in.color, COLOR_RAM_SIZE);
	// Restore the timeline before chip state.  In particular, SID's reSID
	// renderer uses the board cycle as its cycle base when SetState resets it;
	// setting counters afterward would make the first flush catch up from
	// cycle zero to the snapshot's cycle.
	c64->SetCounters(in.cycle, in.frame);

	c64->TheCPU->SetState(&in.cpu);
	c64->TheVIC->SetState(&in.vic);
	c64->TheSID->SetState(&in.sid);
	c64->TheCIA1->SetState(&in.cia1);
	c64->TheCIA2->SetState(&in.cia2);
	// prev_lp is not in MOS6526State; avoid a false lightpen edge on first PB write.
	c64->TheCIA1->SyncLightpenEdge();
	if (in.version >= 2) {
		c64->TheVIC->SetScPipeline(&in.vic_sc);
		c64->TheCPU->BALow = in.ba_low != 0;
		// SetState forces timers awake; re-apply saved idle bits.
		c64->TheCIA1->SetTimerIdle(in.cia1_ta_idle != 0, in.cia1_tb_idle != 0);
		c64->TheCIA2->SetTimerIdle(in.cia2_ta_idle != 0, in.cia2_tb_idle != 0);
	}
	return true;
}

bool CaptureChipSnapshot(const Board & board, ChipSnapshot & out) {
	const C64 * c64 = board.Machine();
	if (!c64) return false;

	std::memset(&out, 0, sizeof(out));
	std::memcpy(out.magic, ChipSnapshot::kMagic, 16);
	out.version = ChipSnapshot::kVersion;
	out.cycle = board.CycleCounter();
	out.frame = board.FrameCounter();

	c64->TheVIC->GetState(&out.vic);
	c64->TheSID->GetState(&out.sid);
	c64->TheCIA1->GetState(&out.cia1);
	c64->TheCIA2->GetState(&out.cia2);
	return true;
}

bool CaptureScreenSnapshot(const Board & board, ScreenSnapshot & out) {
	const C64 * c64 = board.Machine();
	if (!c64 || !c64->TheDisplay) return false;
	static_assert(ScreenSnapshot::kWidth == DISPLAY_X, "screen width");
	static_assert(ScreenSnapshot::kHeight == DISPLAY_Y, "screen height");

	std::memset(&out, 0, sizeof(out));
	out.cycle = board.CycleCounter();
	out.frame = board.FrameCounter();
	const uint8_t * src = c64->TheDisplay->BitmapBase();
	if (!src) return false;
	std::memcpy(out.pixels, src, ScreenSnapshot::kBytes);
	return true;
}

namespace {

template <typename T>
bool write_pod(const std::string & path, const T & obj, std::string & error) {
	std::ofstream f(path, std::ios::binary);
	if (!f) {
		error = "Cannot open for write: " + path;
		return false;
	}
	f.write(reinterpret_cast<const char *>(&obj), sizeof(T));
	if (!f) {
		error = "Write failed: " + path;
		return false;
	}
	return true;
}

template <typename T>
bool read_pod(const std::string & path, T & obj, std::string & error) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		error = "Cannot open for read: " + path;
		return false;
	}
	f.read(reinterpret_cast<char *>(&obj), sizeof(T));
	if (!f) {
		error = "Read failed: " + path;
		return false;
	}
	return true;
}

} // namespace

bool SaveFullSnapshotFile(const std::string & path, const FullSnapshot & snap, std::string & error) {
	return write_pod(path, snap, error);
}

bool LoadFullSnapshotFile(const std::string & path, FullSnapshot & snap, std::string & error) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		error = "Cannot open for read: " + path;
		return false;
	}
	const auto sz = static_cast<size_t>(f.tellg());
	f.seekg(0);
	std::memset(&snap, 0, sizeof(snap));
	if (sz == sizeof(FullSnapshot)) {
		f.read(reinterpret_cast<char *>(&snap), sizeof(snap));
		if (!f) {
			error = "Read failed: " + path;
			return false;
		}
		return true;
	}
	if (sz == FullSnapshot::kVersion1Size) {
		f.read(reinterpret_cast<char *>(&snap), FullSnapshot::kVersion1Size);
		if (!f) {
			error = "Read failed: " + path;
			return false;
		}
		// Trailer left zeroed — cold restore of v1 begin snaps is incomplete.
		snap.version = 1;
		return true;
	}
	error = "Unexpected FullSnapshot size " + std::to_string(sz) + " (want " +
	        std::to_string(sizeof(FullSnapshot)) + " or " +
	        std::to_string(FullSnapshot::kVersion1Size) + ")";
	return false;
}

bool SaveChipSnapshotFile(const std::string & path, const ChipSnapshot & snap, std::string & error) {
	return write_pod(path, snap, error);
}

bool LoadChipSnapshotFile(const std::string & path, ChipSnapshot & snap, std::string & error) {
	return read_pod(path, snap, error);
}

namespace {

bool boot_and_hash(bool headless, bool audio, std::string & hex, FullSnapshot & snap,
                   std::string & error) {
	Config cfg;
	cfg.headless = headless;
	cfg.limit_speed = false;
	cfg.audio_enabled = audio;
	cfg.no_audio = !audio;
	Board board;
	board.Configure(cfg);
	if (!board.Init(error)) return false;
	board.PrepareRun();
	if (!CaptureFullSnapshot(board, snap)) {
		error = "CaptureFullSnapshot failed";
		return false;
	}
	hex = Sha256FullSnapshot(snap);
	if (hex.empty()) {
		error = "Sha256FullSnapshot failed";
		return false;
	}
	return true;
}

} // namespace

int RunBootSnapshotSelfTest(std::string & report) {
	std::ostringstream r;
	int fails = 0;
	auto check = [&](bool ok, const char * name) {
		if (ok) r << "  PASS  " << name << "\n";
		else {
			r << "  FAIL  " << name << "\n";
			++fails;
		}
	};

	// Dummy SDL so the windowed+audio board can Init without a display.
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("SDL_AUDIODRIVER", "dummy", 1);

	std::string err;
	std::string headless_hex;
	std::string windowed_hex;
	FullSnapshot headless_snap{};
	FullSnapshot windowed_snap{};
	const int fails_before = fails;
	check(boot_and_hash(true, false, headless_hex, headless_snap, err),
	      "headless --no-audio boot");
	if (fails > fails_before) r << "    " << err << "\n";
	const int fails_mid = fails;
	check(boot_and_hash(false, true, windowed_hex, windowed_snap, err),
	      "windowed + SID boot");
	if (fails > fails_mid) r << "    " << err << "\n";

	const bool same_hash = !headless_hex.empty() && headless_hex == windowed_hex;
	check(same_hash, "windowed vs headless cycle-0 FullSnapshot hash");
	if (!same_hash && !headless_hex.empty() && !windowed_hex.empty()) {
		r << "    headless " << headless_hex << "\n";
		r << "    windowed " << windowed_hex << "\n";
		const auto * h = reinterpret_cast<const uint8_t *>(&headless_snap);
		const auto * w = reinterpret_cast<const uint8_t *>(&windowed_snap);
		for (size_t i = 0; i < sizeof(FullSnapshot); ++i) {
			if (h[i] != w[i]) {
				char buf[80];
				std::snprintf(buf, sizeof(buf),
				              "    first byte diff at offset %zu headless=0x%02X windowed=0x%02X\n",
				              i, h[i], w[i]);
				r << buf;
				break;
			}
		}
	}

	if (fails == 0) r << "All boot-snapshot self-tests passed.\n";
	else r << fails << " boot-snapshot self-test(s) failed.\n";
	report = r.str();
	return fails == 0 ? 0 : 1;
}

} // namespace revm
