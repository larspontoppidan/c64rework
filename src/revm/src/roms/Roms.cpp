// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "roms/Roms.hpp"
#include "util/Hash.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace revm {

namespace {

namespace fs = std::filesystem;

bool check_rom_size(const std::string & path, std::size_t expected, std::string & error) {
	std::error_code ec;
	const auto n = fs::file_size(path, ec);
	if (ec) {
		error = "Cannot read size of " + path;
		return false;
	}
	if (n != expected) {
		error = path + " is " + std::to_string(n) + " bytes, expected " +
		        std::to_string(expected);
		return false;
	}
	return true;
}

bool hash_rom(const std::string & path, std::string & hex, std::string & error) {
	hex = Sha256File(path);
	if (hex.empty()) {
		error = "Cannot hash " + path;
		return false;
	}
	return true;
}

bool read_exact(const std::string & path, std::vector<uint8_t> & out, std::size_t size,
                std::string & error) {
	if (!check_rom_size(path, size, error)) return false;
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		error = "Cannot open " + path;
		return false;
	}
	out.resize(size);
	f.read(reinterpret_cast<char *>(out.data()), std::streamsize(size));
	if (f.gcount() != std::streamsize(size)) {
		error = "Short read: " + path;
		return false;
	}
	return true;
}

char ascii_lower(char c) {
	return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

std::string ascii_lowered(std::string_view s) {
	std::string out(s);
	for (char & c : out) c = ascii_lower(c);
	return out;
}

bool match_glob(std::string_view name, std::string_view glob) {
	const std::string lowered_name = ascii_lowered(name);
	const std::string lowered_glob = ascii_lowered(glob);
	const auto star = lowered_glob.find('*');
	if (star == std::string::npos) return lowered_name == lowered_glob;
	const auto prefix = std::string_view(lowered_glob).substr(0, star);
	const auto suffix = std::string_view(lowered_glob).substr(star + 1);
	if (lowered_glob.find('*', star + 1) != std::string::npos) return false;
	if (lowered_name.size() < prefix.size() + suffix.size()) return false;
	return std::string_view(lowered_name).substr(0, prefix.size()) == prefix &&
	       std::string_view(lowered_name).substr(lowered_name.size() - suffix.size()) ==
	           suffix;
}

std::string find_rom_glob(const fs::path & dir, std::string_view glob) {
	std::vector<fs::path> matches;
	std::error_code ec;
	for (const auto & entry : fs::directory_iterator(dir, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		const std::string name = entry.path().filename().string();
		if (match_glob(name, glob)) matches.push_back(entry.path());
	}
	if (matches.empty()) return {};
	std::sort(matches.begin(), matches.end());
	return matches.front().string();
}

} // namespace

bool ResolveRomDir(const std::string & dir, RomDirFiles & out, std::string & error) {
	out = {};
	if (dir.empty()) {
		error = "ROM directory is empty";
		return false;
	}
	const fs::path root(dir);
	if (!fs::is_directory(root)) {
		error = "ROM directory does not exist: " + dir;
		return false;
	}

	out.basic = find_rom_glob(root, kBasicRomGlob);
	out.kernal = find_rom_glob(root, kKernalRomGlob);
	out.chars = find_rom_glob(root, kCharRomGlob);
	out.drive = find_rom_glob(root, kDriveRomGlob);

	if (out.basic.empty() || out.kernal.empty() || out.chars.empty()) {
		error = "ROM directory '" + dir +
		        "' must contain BASIC, KERNAL, and character images "
		        "(" +
		        std::string(kBasicRomGlob) + ", " + kKernalRomGlob + ", " +
		        kCharRomGlob + ")";
		return false;
	}
	return true;
}

bool InspectRomDir(const std::string & dir, RomDirFiles & files, RomHashes & hashes,
                   std::string & error) {
	hashes = {};
	if (!ResolveRomDir(dir, files, error)) return false;
	if (!check_rom_size(files.basic, kBasicRomSize, error)) return false;
	if (!check_rom_size(files.kernal, kKernalRomSize, error)) return false;
	if (!check_rom_size(files.chars, kCharRomSize, error)) return false;
	if (!files.drive.empty() &&
	    !check_rom_size(files.drive, kDriveRomSize, error)) {
		return false;
	}
	if (!hash_rom(files.basic, hashes.basic, error)) return false;
	if (!hash_rom(files.kernal, hashes.kernal, error)) return false;
	if (!hash_rom(files.chars, hashes.chargen, error)) return false;
	if (!files.drive.empty() && !hash_rom(files.drive, hashes.dos1541ii, error)) {
		return false;
	}
	return true;
}

bool LoadRomsFromDir(const std::string & dir, RomImages & out, std::string & error) {
	RomDirFiles files;
	RomHashes hashes;
	if (!InspectRomDir(dir, files, hashes, error)) return false;
	if (!read_exact(files.basic, out.basic, kBasicRomSize, error)) return false;
	if (!read_exact(files.kernal, out.kernal, kKernalRomSize, error)) return false;
	if (!read_exact(files.chars, out.chars, kCharRomSize, error)) return false;
	out.drive.clear();
	if (!files.drive.empty() &&
	    !read_exact(files.drive, out.drive, kDriveRomSize, error)) {
		return false;
	}
	return true;
}

} // namespace revm
