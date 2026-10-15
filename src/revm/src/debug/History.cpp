// Created  : 2026-10-12
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "debug/History.hpp"

#include <cstring>

#include <zstd.h>

namespace revm {

namespace {

constexpr size_t kInputChunk = 1 << 20;
constexpr int kZstdLevel = 3;

void put_u16(uint8_t * p, uint16_t v) {
	p[0] = uint8_t(v);
	p[1] = uint8_t(v >> 8);
}

} // namespace

HistoryWriter::~HistoryWriter() {
	if (file_) {
		std::string error;
		Finish(last_cycle_, error);
	}
}

bool HistoryWriter::Begin(const std::string & path, uint64_t start_cycle,
                          const std::string & metadata_json, const uint8_t ram[0x10000],
                          const uint8_t color_ram[0x400], uint8_t ddr, uint8_t pr,
                          uint16_t pc, std::string & error) {
	file_ = std::fopen(path.c_str(), "wb");
	if (!file_) {
		error = "cannot write " + path;
		return false;
	}
	cctx_ = ZSTD_createCCtx();
	ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, kZstdLevel);
	in_.reserve(kInputChunk + 64);
	out_.resize(ZSTD_CStreamOutSize());

	put_bytes("REVMHIST", 8);
	uint8_t head[12];
	put_u16(head, 1);
	put_u16(head + 2, 0);
	for (int i = 0; i < 8; ++i) head[4 + i] = uint8_t(start_cycle >> (8 * i));
	put_bytes(head, sizeof(head));
	const uint32_t meta_len = uint32_t(metadata_json.size());
	uint8_t len_bytes[4];
	for (int i = 0; i < 4; ++i) len_bytes[i] = uint8_t(meta_len >> (8 * i));
	put_bytes(len_bytes, 4);
	put_bytes(metadata_json.data(), metadata_json.size());
	put_bytes(ram, 0x10000);
	put_bytes(color_ram, 0x400);
	uint8_t tail[4] = {ddr, pr, 0, 0};
	put_u16(tail + 2, pc);
	put_bytes(tail, sizeof(tail));

	last_cycle_ = start_cycle;
	records_ = 0;
	have_pending_ = false;
	return true;
}

void HistoryWriter::put_bytes(const void * data, size_t size) {
	const uint8_t * p = static_cast<const uint8_t *>(data);
	in_.insert(in_.end(), p, p + size);
	if (in_.size() >= kInputChunk) compress_out(false);
}

bool HistoryWriter::compress_out(bool end_frame) {
	ZSTD_inBuffer input = {in_.data(), in_.size(), 0};
	const ZSTD_EndDirective mode = end_frame ? ZSTD_e_end : ZSTD_e_continue;
	bool done = false;
	while (!done) {
		ZSTD_outBuffer output = {out_.data(), out_.size(), 0};
		const size_t remaining = ZSTD_compressStream2(cctx_, &output, &input, mode);
		if (ZSTD_isError(remaining)) {
			io_error_ = true;
			break;
		}
		if (output.pos > 0 && std::fwrite(out_.data(), 1, output.pos, file_) != output.pos) {
			io_error_ = true;
			break;
		}
		if (end_frame) done = remaining == 0;
		else done = input.pos == input.size;
	}
	in_.clear();
	return !io_error_;
}

void HistoryWriter::flush_pending() {
	if (!have_pending_) return;
	put_bytes(pending_, 5);
	have_pending_ = false;
}

void HistoryWriter::Access(uint64_t cycle, HistoryKind kind, uint16_t addr, uint8_t value) {
	if (!file_) return;
	flush_pending();
	uint64_t gap = cycle >= last_cycle_ ? cycle - last_cycle_ : 0;
	while (gap > 255) {
		uint8_t tick[5] = {uint8_t(HistoryKind::Tick), 0, 0, 0, 255};
		put_bytes(tick, 5);
		gap -= 255;
		++records_;
	}
	pending_[0] = uint8_t(kind);
	pending_[1] = value;
	put_u16(pending_ + 2, addr);
	pending_[4] = uint8_t(gap);
	have_pending_ = true;
	last_cycle_ = cycle;
	++records_;
}

void HistoryWriter::MarkExec(uint16_t pc) {
	if (!have_pending_) return;
	const uint16_t addr = uint16_t(pending_[2] | (pending_[3] << 8));
	const bool is_read = pending_[0] == uint8_t(HistoryKind::ReadRam) ||
	                     pending_[0] == uint8_t(HistoryKind::ReadRom) ||
	                     pending_[0] == uint8_t(HistoryKind::ReadIo);
	if (is_read && addr == pc) pending_[0] = uint8_t(HistoryKind::Exec);
}

bool HistoryWriter::Finish(uint64_t last_cycle, std::string & error) {
	if (!file_) return true;
	flush_pending();
	uint8_t end[13] = {uint8_t(HistoryKind::End), 0, 0, 0, 0};
	for (int i = 0; i < 8; ++i) end[5 + i] = uint8_t(last_cycle >> (8 * i));
	put_bytes(end, sizeof(end));
	compress_out(true);
	const bool ok = !io_error_ && std::fclose(file_) == 0;
	file_ = nullptr;
	ZSTD_freeCCtx(cctx_);
	cctx_ = nullptr;
	if (!ok) error = "history write failed";
	return ok;
}

} // namespace revm
