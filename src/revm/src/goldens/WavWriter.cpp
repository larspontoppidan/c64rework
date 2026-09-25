// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "goldens/WavWriter.hpp"

#include <cstring>

namespace revm {

bool WavWriter::Open(const std::string & path, int sample_rate, std::string & error) {
	std::lock_guard lock(mu_);
	if (fp_) {
		std::string e;
		rewrite_header_unlocked(e);
		std::fclose(fp_);
		fp_ = nullptr;
	}
	path_ = path;
	sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
	sample_count_ = 0;
	armed_ = false;

	fp_ = std::fopen(path.c_str(), "wb+");
	if (!fp_) {
		error = "Cannot create " + path;
		return false;
	}
	// Placeholder header; rewritten on Close.
	uint8_t hdr[44]{};
	if (std::fwrite(hdr, 1, 44, fp_) != 44) {
		error = "Cannot write WAV header";
		std::fclose(fp_);
		fp_ = nullptr;
		return false;
	}
	return true;
}

void WavWriter::WriteSamples(const int16_t * samples, size_t count) {
	if (!fp_ || !armed_ || !samples || count == 0) return;
	std::lock_guard lock(mu_);
	if (!fp_ || !armed_) return;
	const size_t bytes = count * sizeof(int16_t);
	if (std::fwrite(samples, 1, bytes, fp_) == bytes) {
		sample_count_ += count;
	}
}

bool WavWriter::rewrite_header_unlocked(std::string & error) {
	if (!fp_) return true;
	const uint32_t data_bytes = uint32_t(sample_count_ * sizeof(int16_t));
	const uint32_t riff_size = 36 + data_bytes;
	const uint16_t audio_format = 1; // PCM
	const uint16_t channels = 1;
	const uint32_t byte_rate = uint32_t(sample_rate_) * channels * 2;
	const uint16_t block_align = channels * 2;
	const uint16_t bits = 16;

	uint8_t hdr[44];
	std::memcpy(hdr + 0, "RIFF", 4);
	std::memcpy(hdr + 4, &riff_size, 4);
	std::memcpy(hdr + 8, "WAVE", 4);
	std::memcpy(hdr + 12, "fmt ", 4);
	const uint32_t fmt_size = 16;
	std::memcpy(hdr + 16, &fmt_size, 4);
	std::memcpy(hdr + 20, &audio_format, 2);
	std::memcpy(hdr + 22, &channels, 2);
	std::memcpy(hdr + 24, &sample_rate_, 4);
	std::memcpy(hdr + 28, &byte_rate, 4);
	std::memcpy(hdr + 32, &block_align, 2);
	std::memcpy(hdr + 34, &bits, 2);
	std::memcpy(hdr + 36, "data", 4);
	std::memcpy(hdr + 40, &data_bytes, 4);

	if (std::fseek(fp_, 0, SEEK_SET) != 0) {
		error = "WAV header seek failed";
		return false;
	}
	if (std::fwrite(hdr, 1, 44, fp_) != 44) {
		error = "WAV header write failed";
		return false;
	}
	return true;
}

bool WavWriter::Close(std::string & error) {
	std::lock_guard lock(mu_);
	if (!fp_) return true;
	const bool ok = rewrite_header_unlocked(error);
	std::fclose(fp_);
	fp_ = nullptr;
	armed_ = false;
	return ok;
}

void WavWriter::SidTap(const int16_t * samples, size_t count, void * userdata) {
	if (!userdata) return;
	static_cast<WavWriter *>(userdata)->WriteSamples(samples, count);
}

} // namespace revm
