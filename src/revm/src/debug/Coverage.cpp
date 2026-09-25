// Created  : 2026-07-23
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "debug/Coverage.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace revm {

void Coverage::Clear() {
	std::memset(counts_, 0, sizeof(counts_));
}

void Coverage::add(Plane p, uint16_t addr) {
	const unsigned pi = static_cast<unsigned>(p);
	uint16_t & c = counts_[pi][addr];
	if (c < 0xFFFF) ++c;
}

uint16_t Coverage::Get(Plane p, uint16_t addr) const {
	return counts_[static_cast<unsigned>(p)][addr];
}

size_t Coverage::Count(Plane p) const {
	const unsigned pi = static_cast<unsigned>(p);
	size_t n = 0;
	for (size_t i = 0; i < kPlaneAddrs; ++i) {
		if (counts_[pi][i] != 0) ++n;
	}
	return n;
}

uint64_t Coverage::TotalHits(Plane p) const {
	const unsigned pi = static_cast<unsigned>(p);
	uint64_t sum = 0;
	for (size_t i = 0; i < kPlaneAddrs; ++i) {
		sum += counts_[pi][i];
	}
	return sum;
}

bool Coverage::Save(const std::string & path, std::string & error) const {
	std::ofstream f(path, std::ios::binary);
	if (!f) {
		error = "cannot write " + path;
		return false;
	}
	char magic[8]{};
	std::memcpy(magic, kMagic, 8);
	const uint16_t version = kVersion;
	const uint16_t reserved = 0;
	f.write(magic, 8);
	f.write(reinterpret_cast<const char *>(&version), 2);
	f.write(reinterpret_cast<const char *>(&reserved), 2);
	for (unsigned p = 0; p < static_cast<unsigned>(Plane::Count); ++p) {
		f.write(reinterpret_cast<const char *>(counts_[p]),
		        static_cast<std::streamsize>(kPlaneAddrs * sizeof(uint16_t)));
	}
	if (!f) {
		error = "write failed: " + path;
		return false;
	}
	return true;
}

bool Coverage::Load(const std::string & path, std::string & error) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		error = "cannot read " + path;
		return false;
	}
	const auto sz = static_cast<size_t>(f.tellg());
	f.seekg(0);
	Clear();

	if (sz != kFileSize) {
		error = "unexpected coverage size " + std::to_string(sz) +
		        " (want REVMCOV2 " + std::to_string(kFileSize) + "): " + path;
		return false;
	}

	char magic[8]{};
	uint16_t version = 0;
	uint16_t reserved = 0;
	f.read(magic, 8);
	f.read(reinterpret_cast<char *>(&version), 2);
	f.read(reinterpret_cast<char *>(&reserved), 2);
	if (std::memcmp(magic, kMagic, 8) != 0) {
		error = "bad coverage magic (expected REVMCOV2): " + path;
		return false;
	}
	if (version != kVersion) {
		error = "unsupported coverage version " + std::to_string(version) +
		        ": " + path;
		return false;
	}
	(void)reserved;
	for (unsigned p = 0; p < static_cast<unsigned>(Plane::Count); ++p) {
		f.read(reinterpret_cast<char *>(counts_[p]),
		       static_cast<std::streamsize>(kPlaneAddrs * sizeof(uint16_t)));
	}
	if (!f) {
		error = "truncated REVMCOV2: " + path;
		return false;
	}
	return true;
}

void Coverage::Merge(const Coverage & other) {
	for (unsigned p = 0; p < static_cast<unsigned>(Plane::Count); ++p) {
		for (size_t i = 0; i < kPlaneAddrs; ++i) {
			const uint32_t sum =
			    uint32_t(counts_[p][i]) + uint32_t(other.counts_[p][i]);
			counts_[p][i] = sum > 0xFFFFu ? uint16_t(0xFFFF) : uint16_t(sum);
		}
	}
}

std::vector<std::pair<uint16_t, uint16_t>> Coverage::Coalesce(
	Plane plane, uint16_t max_gap) const {
	std::vector<std::pair<uint16_t, uint16_t>> out;
	bool in = false;
	uint16_t lo = 0;
	uint16_t last_hit = 0;
	for (uint32_t a = 0; a < 0x10000; ++a) {
		const bool is_hit = hit(plane, uint16_t(a));
		if (is_hit) {
			if (!in) {
				in = true;
				lo = uint16_t(a);
			} else if (max_gap > 0 && uint16_t(a - last_hit - 1) > max_gap) {
				out.emplace_back(lo, uint16_t(last_hit + 1));
				lo = uint16_t(a);
			}
			last_hit = uint16_t(a);
		} else if (in && max_gap == 0) {
			out.emplace_back(lo, uint16_t(a));
			in = false;
		}
	}
	if (in) {
		out.emplace_back(lo, uint16_t(last_hit + 1));
	}
	return out;
}

int RunCoverageSelfTest(std::string & report) {
	std::ostringstream r;
	int fails = 0;
	auto check = [&](bool ok, const char * name) {
		if (ok) r << "  PASS  " << name << "\n";
		else {
			r << "  FAIL  " << name << "\n";
			++fails;
		}
	};

	Coverage a;
	a.Clear();
	check(!a.HitPcRam(0x1000), "clear empty");
	a.MarkPcRam(0x1000);
	a.MarkPcRam(0x1000);
	check(a.Get(Coverage::Plane::PcRam, 0x1000) == 2, "add twice");
	a.MarkMemRam(0x00DA);
	for (int i = 0; i < 70000; ++i) a.MarkMemRam(0x0048);
	check(a.Get(Coverage::Plane::MemRam, 0x0048) == 0xFFFF, "saturate u16");

	Coverage b;
	b.MarkPcRam(0x1000);
	for (int i = 0; i < 100; ++i) b.MarkMemRam(0x0048);
	a.Merge(b);
	check(a.Get(Coverage::Plane::PcRam, 0x1000) == 3, "merge add");
	check(a.Get(Coverage::Plane::MemRam, 0x0048) == 0xFFFF, "merge saturate");

	const std::string path =
	    (std::filesystem::temp_directory_path() / "revm_cov2_selftest.bin").string();
	std::string err;
	check(a.Save(path, err), "save COV2");
	Coverage c;
	check(c.Load(path, err), "load COV2");
	check(c.Get(Coverage::Plane::PcRam, 0x1000) == 3, "roundtrip pc");
	check(c.Get(Coverage::Plane::MemRam, 0x00DA) == 1, "roundtrip mem");
	check(c.Count(Coverage::Plane::PcRam) == 1, "nonzero count");
	std::remove(path.c_str());

	if (fails == 0) r << "All coverage self-tests passed.\n";
	else r << fails << " coverage self-test(s) failed.\n";
	report = r.str();
	return fails == 0 ? 0 : 1;
}

} // namespace revm
