// Created  : 2026-10-02
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "util/Png.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace revm {

const uint8_t kPeptoRgb[16][3] = {
	{0x00, 0x00, 0x00}, {0xff, 0xff, 0xff}, {0x86, 0x19, 0x01},
	{0x4c, 0xc1, 0xe3}, {0x88, 0x17, 0xbd}, {0x35, 0xac, 0x0a},
	{0x20, 0x07, 0xc0}, {0xcf, 0xf2, 0x2d}, {0x88, 0x3e, 0x00},
	{0x40, 0x2a, 0x00}, {0xcb, 0x55, 0x37}, {0x34, 0x34, 0x34},
	{0x68, 0x68, 0x68}, {0x8b, 0xff, 0x59}, {0x68, 0x4a, 0xff},
	{0xa1, 0xa1, 0xa1},
};

namespace {

void put_be32(std::vector<uint8_t> & out, uint32_t v) {
	out.push_back(uint8_t(v >> 24));
	out.push_back(uint8_t(v >> 16));
	out.push_back(uint8_t(v >> 8));
	out.push_back(uint8_t(v));
}

uint32_t crc32(const uint8_t * p, size_t n) {
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; ++i) {
		c ^= p[i];
		for (int k = 0; k < 8; ++k)
			c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
	}
	return c ^ 0xffffffffu;
}

uint32_t adler32(const uint8_t * p, size_t n) {
	uint32_t s1 = 1;
	uint32_t s2 = 0;
	for (size_t i = 0; i < n; ++i) {
		s1 += p[i];
		if (s1 >= 65521u) s1 -= 65521u;
		s2 += s1;
		if (s2 >= 65521u) s2 -= 65521u;
	}
	return (s2 << 16) | s1;
}

void append_chunk(std::vector<uint8_t> & out, const char type[4],
                  const uint8_t * data, size_t n) {
	put_be32(out, uint32_t(n));
	const size_t type_at = out.size();
	out.insert(out.end(), type, type + 4);
	if (n) out.insert(out.end(), data, data + n);
	put_be32(out, crc32(out.data() + type_at, 4 + n));
}

bool write_bytes(const std::string & path, const uint8_t * data, size_t n,
                 std::string & error) {
	std::ofstream f(path, std::ios::binary);
	if (!f) {
		error = "cannot write " + path;
		return false;
	}
	f.write(reinterpret_cast<const char *>(data), std::streamsize(n));
	if (!f) {
		error = "write failed: " + path;
		return false;
	}
	return true;
}

} // namespace

bool WritePngIndexed(const std::string & path, unsigned w, unsigned h,
                     const uint8_t * pixels, const uint8_t * palette_rgb,
                     unsigned n_colors, std::string & error) {
	error.clear();
	if (!pixels || w == 0 || h == 0) {
		error = "png: empty image";
		return false;
	}
	if (!palette_rgb || n_colors < 1 || n_colors > 256) {
		error = "png: palette must have 1–256 colors";
		return false;
	}
	const size_t npix = size_t(w) * size_t(h);
	for (size_t i = 0; i < npix; ++i) {
		if (pixels[i] >= n_colors) {
			error = "png: pixel index out of palette";
			return false;
		}
	}

	std::vector<uint8_t> raw;
	raw.resize(size_t(h) * (size_t(w) + 1));
	for (unsigned y = 0; y < h; ++y) {
		raw[size_t(y) * (size_t(w) + 1)] = 0; // filter None
		std::memcpy(raw.data() + size_t(y) * (size_t(w) + 1) + 1,
		            pixels + size_t(y) * w, w);
	}

	std::vector<uint8_t> z;
	z.push_back(0x78);
	z.push_back(0x01);
	size_t off = 0;
	while (off < raw.size()) {
		const size_t remain = raw.size() - off;
		const uint16_t len = remain > 65535u ? uint16_t(65535) : uint16_t(remain);
		const bool last = off + len == raw.size();
		z.push_back(last ? 0x01 : 0x00); // BFINAL | BTYPE=stored
		z.push_back(uint8_t(len));
		z.push_back(uint8_t(len >> 8));
		const uint16_t nlen = uint16_t(~len);
		z.push_back(uint8_t(nlen));
		z.push_back(uint8_t(nlen >> 8));
		z.insert(z.end(), raw.data() + off, raw.data() + off + len);
		off += len;
	}
	put_be32(z, adler32(raw.data(), raw.size()));

	std::vector<uint8_t> png;
	static constexpr uint8_t kSig[8] = {0x89, 0x50, 0x4e, 0x47,
	                                    0x0d, 0x0a, 0x1a, 0x0a};
	png.insert(png.end(), kSig, kSig + 8);

	uint8_t ihdr[13];
	ihdr[0] = uint8_t(w >> 24);
	ihdr[1] = uint8_t(w >> 16);
	ihdr[2] = uint8_t(w >> 8);
	ihdr[3] = uint8_t(w);
	ihdr[4] = uint8_t(h >> 24);
	ihdr[5] = uint8_t(h >> 16);
	ihdr[6] = uint8_t(h >> 8);
	ihdr[7] = uint8_t(h);
	ihdr[8] = 8; // bit depth
	ihdr[9] = 3; // indexed
	ihdr[10] = 0;
	ihdr[11] = 0;
	ihdr[12] = 0;
	append_chunk(png, "IHDR", ihdr, sizeof(ihdr));
	append_chunk(png, "PLTE", palette_rgb, size_t(n_colors) * 3);
	append_chunk(png, "IDAT", z.data(), z.size());
	append_chunk(png, "IEND", nullptr, 0);

	return write_bytes(path, png.data(), png.size(), error);
}

bool WritePngPepto(const std::string & path, unsigned w, unsigned h,
                   const uint8_t * pixels, std::string & error) {
	if (!pixels || w == 0 || h == 0) {
		error = "png: empty image";
		return false;
	}
	const size_t npix = size_t(w) * size_t(h);
	std::vector<uint8_t> idx(npix);
	for (size_t i = 0; i < npix; ++i)
		idx[i] = pixels[i] & 0x0f;
	return WritePngIndexed(path, w, h, idx.data(), &kPeptoRgb[0][0], 16, error);
}

} // namespace revm
