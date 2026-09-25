// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace revm {

// Thread-safe WAV writer for SID PCM (S16LE mono).
class WavWriter {
public:
	~WavWriter() {
		std::string err;
		Close(err);
	}

	bool Open(const std::string & path, int sample_rate, std::string & error);
	void WriteSamples(const int16_t * samples, size_t count);
	bool Close(std::string & error);

	void SetArmed(bool armed) { armed_ = armed; }
	bool IsOpen() const { return fp_ != nullptr; }
	uint64_t SampleCount() const { return sample_count_; }
	int SampleRate() const { return sample_rate_; }

	static void SidTap(const int16_t * samples, size_t count, void * userdata);

private:
	bool rewrite_header_unlocked(std::string & error);

	std::FILE * fp_ = nullptr;
	std::string path_;
	int sample_rate_ = 48000;
	uint64_t sample_count_ = 0;
	std::mutex mu_;
	bool armed_ = false;
};

} // namespace revm
