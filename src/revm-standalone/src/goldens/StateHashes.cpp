// Created  : 2026-10-12
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "goldens/StateHashes.hpp"
#include "util/Crc32.hpp"

#include "C64.h"
#include "CIA.h"
#include "Display.h"
#include "SID.h"
#include "VIC.h"

#include <cstring>

namespace revm {

namespace {

constexpr char kMagic[8] = {'R', 'E', 'V', 'M', 'H', 'A', 'S', 'H'};
constexpr size_t kHeaderFixedBytes = 8 + 4 + 4 + 4;

const char * const kChannelNames[StateHashRecorder::kChannelCount] = {
	"screen", "sid", "vic", "cia1", "cia2",
};

void put_u32(uint8_t * out, uint32_t value) {
	out[0] = uint8_t(value & 0xffu);
	out[1] = uint8_t((value >> 8) & 0xffu);
	out[2] = uint8_t((value >> 16) & 0xffu);
	out[3] = uint8_t((value >> 24) & 0xffu);
}

uint32_t get_u32(const uint8_t * in) {
	return uint32_t(in[0]) | (uint32_t(in[1]) << 8) | (uint32_t(in[2]) << 16) |
	       (uint32_t(in[3]) << 24);
}

uint32_t crc_of(const uint8_t * data, size_t size) {
	return Crc32::Of(data, size);
}

} // namespace

StateHashRecorder::~StateHashRecorder() {
	if (machine_ && machine_->TheSID)
		machine_->TheSID->SetWriteTap(nullptr, nullptr);
	if (file_)
		std::fclose(file_);
}

const char * StateHashRecorder::ChannelName(uint32_t channel) {
	return channel < kChannelCount ? kChannelNames[channel] : "?";
}

void StateHashRecorder::Configure(const std::string & path) {
	path_ = path;
}

void StateHashRecorder::Attach(C64 * machine) {
	machine_ = machine;
	if (Enabled() && machine_ && machine_->TheSID)
		machine_->TheSID->SetWriteTap(&StateHashRecorder::sid_write_tap, this);
}

void StateHashRecorder::sid_write_tap(uint16_t adr, uint8_t byte, void * userdata) {
	auto * self = static_cast<StateHashRecorder *>(userdata);
	SidWrite write;
	write.cycle = self->machine_->CycleCounter() - self->frame_start_cycle_;
	write.reg = uint8_t(adr & 0x1f);
	write.value = byte;
	self->sid_writes_.push_back(write);
}

bool StateHashRecorder::write_bytes(const void * data, size_t size) {
	return std::fwrite(data, 1, size, file_) == size;
}

bool StateHashRecorder::open_file(uint32_t start_frame, std::string & error) {
	file_ = std::fopen(path_.c_str(), "wb");
	if (!file_) {
		error = "state-hashes: cannot open " + path_;
		return false;
	}
	uint8_t header[kHeaderFixedBytes];
	std::memcpy(header, kMagic, sizeof kMagic);
	put_u32(header + 8, kVersion);
	put_u32(header + 12, kChannelCount);
	put_u32(header + 16, start_frame);
	bool ok = write_bytes(header, sizeof header);
	for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
		char name[kNameBytes] = {};
		std::strncpy(name, kChannelNames[channel], kNameBytes);
		ok = ok && write_bytes(name, kNameBytes);
	}
	if (!ok) {
		error = "state-hashes: cannot write header to " + path_;
		return false;
	}
	return true;
}

uint32_t StateHashRecorder::hash_screen() const {
	const uint8_t * pixels = machine_->TheDisplay ? machine_->TheDisplay->BitmapBase()
	                                              : nullptr;
	if (!pixels)
		return 0;
	return crc_of(pixels, size_t(DISPLAY_X) * DISPLAY_Y);
}

uint32_t StateHashRecorder::hash_sid() const {
	// Six bytes per write so the timing inside the frame is part of the hash.
	std::vector<uint8_t> bytes;
	bytes.reserve(sid_writes_.size() * 6);
	for (const SidWrite & write : sid_writes_) {
		uint8_t entry[6];
		put_u32(entry, write.cycle);
		entry[4] = write.reg;
		entry[5] = write.value;
		bytes.insert(bytes.end(), entry, entry + 6);
	}
	return crc_of(bytes.data(), bytes.size());
}

uint32_t StateHashRecorder::hash_vic() const {
	MOS6569State vic{};
	machine_->TheVIC->GetState(&vic);
	// Register order $D000-$D02E as a program reads them (unused bits read as
	// 1, as in MOS6569::ReadRegister, so a write of a different unused bit is
	// not a difference), then the raster IRQ compare line. The internal
	// pipeline state is not something a program observes.
	const uint8_t bytes[] = {
		vic.m0x, vic.m0y, vic.m1x, vic.m1y, vic.m2x, vic.m2y, vic.m3x, vic.m3y,
		vic.m4x, vic.m4y, vic.m5x, vic.m5y, vic.m6x, vic.m6y, vic.m7x, vic.m7y,
		vic.mx8, vic.ctrl1, vic.raster, vic.lpx, vic.lpy, vic.me,
		uint8_t(vic.ctrl2 | 0xc0), vic.mye, uint8_t(vic.vbase | 0x01),
		uint8_t(vic.irq_flag | 0x70), uint8_t(vic.irq_mask | 0xf0), vic.mdp, vic.mmc,
		vic.mxe, vic.mm, vic.md,
		uint8_t(vic.ec | 0xf0), uint8_t(vic.b0c | 0xf0), uint8_t(vic.b1c | 0xf0),
		uint8_t(vic.b2c | 0xf0), uint8_t(vic.b3c | 0xf0), uint8_t(vic.mm0 | 0xf0),
		uint8_t(vic.mm1 | 0xf0), uint8_t(vic.m0c | 0xf0), uint8_t(vic.m1c | 0xf0),
		uint8_t(vic.m2c | 0xf0), uint8_t(vic.m3c | 0xf0), uint8_t(vic.m4c | 0xf0),
		uint8_t(vic.m5c | 0xf0), uint8_t(vic.m6c | 0xf0), uint8_t(vic.m7c | 0xf0),
		uint8_t(vic.irq_raster & 0xffu), uint8_t(vic.irq_raster >> 8),
	};
	return crc_of(bytes, sizeof bytes);
}

uint32_t StateHashRecorder::hash_cia(int which) const {
	MOS6526State cia{};
	if (which == 1)
		machine_->TheCIA1->GetState(&cia);
	else
		machine_->TheCIA2->GetState(&cia);
	// Register state only. A port contributes the bits it drives (latch AND
	// DDR); latch bits under input pins are invisible to the program and the
	// machine, and the input pins themselves (keyboard, joystick, IEC) are left
	// out: input is the play's, and the standalone CIA 2 has no IEC bus, so a
	// read-modify-write of $DD00 stores different input-pin bits there.
	const uint8_t bytes[] = {
		uint8_t(cia.pra & cia.ddra), cia.ddra, uint8_t(cia.prb & cia.ddrb), cia.ddrb,
		cia.ta_lo, cia.ta_hi, cia.tb_lo, cia.tb_hi,
		cia.tod_10ths, cia.tod_sec, cia.tod_min, cia.tod_hr,
		cia.sdr, cia.int_flags, cia.cra, cia.crb,
		uint8_t(cia.ta_latch & 0xffu), uint8_t(cia.ta_latch >> 8),
		uint8_t(cia.tb_latch & 0xffu), uint8_t(cia.tb_latch >> 8),
		cia.alm_10ths, cia.alm_sec, cia.alm_min, cia.alm_hr,
		cia.int_mask,
	};
	return crc_of(bytes, sizeof bytes);
}

bool StateHashRecorder::OnFrame(uint32_t frame, uint32_t vblank_cycle,
                                std::string & error) {
	if (!Enabled() || !machine_)
		return true;
	if (!error_.empty()) {
		error = error_;
		return false;
	}
	if (!have_first_frame_) {
		if (!open_file(frame, error)) {
			error_ = error;
			return false;
		}
		have_first_frame_ = true;
		next_frame_ = frame;
	}
	// Records carry no frame number, so a gap would silently misalign the
	// file; refuse it instead.
	if (frame != next_frame_) {
		error_ = "state-hashes: frame " + std::to_string(frame) +
		         " follows frame " + std::to_string(next_frame_ - 1) +
		         "; records must be consecutive";
		error = error_;
		return false;
	}

	uint32_t crcs[kChannelCount];
	crcs[0] = hash_screen();
	crcs[1] = hash_sid();
	crcs[2] = hash_vic();
	crcs[3] = hash_cia(1);
	crcs[4] = hash_cia(2);
	uint8_t record[kChannelCount * 4];
	for (uint32_t channel = 0; channel < kChannelCount; ++channel)
		put_u32(record + channel * 4, crcs[channel]);
	if (!write_bytes(record, sizeof record)) {
		error_ = "state-hashes: write failed: " + path_;
		error = error_;
		return false;
	}

	sid_writes_.clear();
	frame_start_cycle_ = vblank_cycle;
	++next_frame_;
	return true;
}

bool StateHashRecorder::Finish(std::string & error) {
	if (file_) {
		if (std::fclose(file_) != 0 && error_.empty())
			error_ = "state-hashes: close failed: " + path_;
		file_ = nullptr;
	}
	if (!error_.empty()) {
		error = error_;
		return false;
	}
	return true;
}

bool ReadStateHashFile(const std::string & path, StateHashFile & out,
                       std::string & error) {
	out = StateHashFile{};
	FILE * file = std::fopen(path.c_str(), "rb");
	if (!file) {
		error = "cannot open " + path;
		return false;
	}
	std::vector<uint8_t> data;
	uint8_t buffer[65536];
	size_t got = 0;
	while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0)
		data.insert(data.end(), buffer, buffer + got);
	std::fclose(file);

	if (data.size() < kHeaderFixedBytes ||
	    std::memcmp(data.data(), kMagic, sizeof kMagic) != 0) {
		error = path + ": not a state-hash file";
		return false;
	}
	out.version = get_u32(data.data() + 8);
	const uint32_t channel_count = get_u32(data.data() + 12);
	out.start_frame = get_u32(data.data() + 16);
	if (out.version != StateHashRecorder::kVersion) {
		error = path + ": unsupported state-hash version " +
		        std::to_string(out.version);
		return false;
	}
	const size_t header_bytes =
		kHeaderFixedBytes + size_t(channel_count) * StateHashRecorder::kNameBytes;
	if (channel_count == 0 || channel_count > 64 || data.size() < header_bytes) {
		error = path + ": bad state-hash header";
		return false;
	}
	for (uint32_t channel = 0; channel < channel_count; ++channel) {
		const char * name = reinterpret_cast<const char *>(
			data.data() + kHeaderFixedBytes + channel * StateHashRecorder::kNameBytes);
		size_t length = 0;
		while (length < StateHashRecorder::kNameBytes && name[length] != '\0')
			++length;
		out.channels.push_back(std::string(name, length));
	}
	const size_t record_bytes = size_t(channel_count) * 4;
	const size_t frames = (data.size() - header_bytes) / record_bytes;
	// A run killed mid-write may leave a partial last record; it is ignored.
	out.records.reserve(frames * channel_count);
	for (size_t i = 0; i < frames * channel_count; ++i)
		out.records.push_back(get_u32(data.data() + header_bytes + i * 4));
	return true;
}

} // namespace revm
