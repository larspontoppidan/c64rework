// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "snapshot/Snapshot.hpp"
#include "core/Board.hpp"
#include "core/Config.hpp"
#include "util/Hash.hpp"

#include "C64.h"
#include "Prefs.h"
#include "CPU1541.h"
#include "1541gcr.h"
#include "Display.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

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

namespace {

bool enable_1541_processor(C64 * c64, std::string * error) {
	if (!c64 || !c64->TheGCRDisk || !c64->TheCPU1541) {
		if (error) *error = "1541 objects missing";
		return false;
	}
	Prefs next = ThePrefs;
	next.Emul1541Proc = true;
	next.DrivePath[0].clear();
	next.AutoStart = false;
	next.LoadProgram.clear();
	c64->NewPrefs(&next);
	ThePrefs = next;
	return true;
}

void capture_drive(const C64 * c64, MachineSnapshot & out) {
	out.has_drive = true;
	std::memset(&out.drive, 0, sizeof(out.drive));
	std::memcpy(out.drive.magic, DriveSnapshotHeader::kMagic, 8);
	out.drive.version = DriveSnapshotHeader::kVersion;
	c64->TheCPU1541->GetState(&out.drive.cpu);
	std::memcpy(out.drive.ram, c64->RAM1541, DRIVE_RAM_SIZE);
	c64->TheGCRDisk->GetState(&out.drive.gcr);
	out.drive.num_tracks = static_cast<uint8_t>(c64->TheGCRDisk->NumTracks());
	out.drive.disk_id1 = c64->TheGCRDisk->DiskId1();
	out.drive.disk_id2 = c64->TheGCRDisk->DiskId2();
	std::memcpy(out.drive.error_info, c64->TheGCRDisk->ErrorInfo(), NUM_SECTORS_40);
	out.gcr_bytes.clear();
	for (unsigned ht = 0; ht < MAX_NUM_HALFTRACKS; ++ht) {
		size_t len = 0;
		const uint8_t * data = c64->TheGCRDisk->TrackData(ht, len);
		out.drive.track_len[ht] = static_cast<uint32_t>(len);
		if (len && data) {
			out.gcr_bytes.insert(out.gcr_bytes.end(), data, data + len);
		}
	}
}

bool restore_drive(C64 * c64, const MachineSnapshot & in, std::string * error) {
	if (!enable_1541_processor(c64, error)) return false;
	GCRDisk * gcr = c64->TheGCRDisk;
	gcr->ClearTracks();
	gcr->SetDiskMeta(in.drive.num_tracks, in.drive.disk_id1, in.drive.disk_id2,
	                 in.drive.error_info);
	size_t off = 0;
	for (unsigned ht = 0; ht < MAX_NUM_HALFTRACKS; ++ht) {
		const uint32_t len = in.drive.track_len[ht];
		if (len == 0) {
			if (!gcr->SetTrack(ht, nullptr, 0)) return false;
			continue;
		}
		if (off + len > in.gcr_bytes.size()) {
			if (error) *error = "drive trailer GCR truncated";
			return false;
		}
		if (!gcr->SetTrack(ht, in.gcr_bytes.data() + off, len)) {
			if (error) *error = "drive trailer GCR track rejected";
			return false;
		}
		off += len;
	}
	gcr->SetState(&in.drive.gcr);
	std::memcpy(c64->RAM1541, in.drive.ram, DRIVE_RAM_SIZE);
	c64->TheCPU1541->SetState(&in.drive.cpu);
	return true;
}

} // namespace

bool CaptureMachineSnapshot(const Board & board, MachineSnapshot & out) {
	out = MachineSnapshot{};
	if (!CaptureFullSnapshot(board, out.c64)) return false;
	const C64 * c64 = board.Machine();
	if (ThePrefs.Emul1541Proc && c64 && c64->TheGCRDisk && c64->TheCPU1541) {
		capture_drive(c64, out);
	}
	return true;
}

bool RestoreMachineSnapshot(Board & board, const MachineSnapshot & in) {
	C64 * c64 = board.Machine();
	if (!c64) return false;
	if (in.has_drive) {
		std::string err;
		if (!restore_drive(c64, in, &err)) return false;
	}
	return RestoreFullSnapshot(board, in.c64);
}

std::vector<uint8_t> EncodeMachineSnapshot(const MachineSnapshot & snap) {
	std::vector<uint8_t> out(sizeof(FullSnapshot));
	std::memcpy(out.data(), &snap.c64, sizeof(FullSnapshot));
	if (!snap.has_drive) return out;
	const size_t hdr = sizeof(DriveSnapshotHeader);
	out.resize(sizeof(FullSnapshot) + hdr + snap.gcr_bytes.size());
	std::memcpy(out.data() + sizeof(FullSnapshot), &snap.drive, hdr);
	if (!snap.gcr_bytes.empty()) {
		std::memcpy(out.data() + sizeof(FullSnapshot) + hdr, snap.gcr_bytes.data(),
		            snap.gcr_bytes.size());
	}
	return out;
}

std::string Sha256MachineSnapshot(const MachineSnapshot & snap) {
	const auto bytes = EncodeMachineSnapshot(snap);
	return Sha256Bytes(bytes.data(), bytes.size());
}

bool SaveMachineSnapshotFile(const std::string & path, const MachineSnapshot & snap,
                             std::string & error) {
	const auto bytes = EncodeMachineSnapshot(snap);
	std::ofstream f(path, std::ios::binary);
	if (!f) {
		error = "Cannot open for write: " + path;
		return false;
	}
	f.write(reinterpret_cast<const char *>(bytes.data()),
	        static_cast<std::streamsize>(bytes.size()));
	if (!f) {
		error = "Write failed: " + path;
		return false;
	}
	return true;
}

bool LoadMachineSnapshotFile(const std::string & path, MachineSnapshot & snap,
                             std::string & error) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		error = "Cannot open for read: " + path;
		return false;
	}
	const auto sz = static_cast<size_t>(f.tellg());
	f.seekg(0);
	snap = MachineSnapshot{};
	if (sz == FullSnapshot::kVersion1Size) {
		f.read(reinterpret_cast<char *>(&snap.c64), FullSnapshot::kVersion1Size);
		if (!f) {
			error = "Read failed: " + path;
			return false;
		}
		snap.c64.version = 1;
		return true;
	}
	if (sz < sizeof(FullSnapshot)) {
		error = "Unexpected FullSnapshot size " + std::to_string(sz);
		return false;
	}
	f.read(reinterpret_cast<char *>(&snap.c64), sizeof(FullSnapshot));
	if (!f) {
		error = "Read failed: " + path;
		return false;
	}
	if (sz == sizeof(FullSnapshot)) return true;
	const size_t hdr = sizeof(DriveSnapshotHeader);
	if (sz < sizeof(FullSnapshot) + hdr) {
		error = "Truncated drive trailer in " + path;
		return false;
	}
	f.read(reinterpret_cast<char *>(&snap.drive), hdr);
	if (!f) {
		error = "Read failed: " + path;
		return false;
	}
	if (std::memcmp(snap.drive.magic, DriveSnapshotHeader::kMagic, 8) != 0) {
		error = "Not a REVM drive trailer: " + path;
		return false;
	}
	if (snap.drive.version != DriveSnapshotHeader::kVersion) {
		error = "Unsupported drive trailer version in " + path;
		return false;
	}
	uint64_t expect = 0;
	for (unsigned ht = 0; ht < MAX_NUM_HALFTRACKS; ++ht)
		expect += snap.drive.track_len[ht];
	const size_t rest = sz - sizeof(FullSnapshot) - hdr;
	if (rest != expect) {
		error = "Drive GCR size mismatch in " + path;
		return false;
	}
	snap.gcr_bytes.resize(rest);
	if (rest) {
		f.read(reinterpret_cast<char *>(snap.gcr_bytes.data()),
		       static_cast<std::streamsize>(rest));
		if (!f) {
			error = "Read failed: " + path;
			return false;
		}
	}
	snap.has_drive = true;
	return true;
}

bool FullSnapshotFileHasDrive(const std::string & path) {
	std::error_code ec;
	const auto sz = std::filesystem::file_size(path, ec);
	if (ec) return false;
	return sz > sizeof(FullSnapshot);
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
	if (sz >= sizeof(FullSnapshot)) {
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
	        std::to_string(FullSnapshot::kVersion1Size) + " or POD+drive trailer)";
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
