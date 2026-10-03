// Created  : 2026-10-02
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "util/Png.hpp"

#include <cstdint>
#include <cstdio>
#include <png.h>
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

	// Build C++ storage before setjmp: libpng errors must not skip destructors.
	std::vector<png_bytep> rows(h);
	for (unsigned y = 0; y < h; ++y)
		rows[y] = const_cast<png_bytep>(pixels + size_t(y) * w);
	png_color palette[256]{};
	for (unsigned i = 0; i < n_colors; ++i) {
		palette[i].red = palette_rgb[i * 3];
		palette[i].green = palette_rgb[i * 3 + 1];
		palette[i].blue = palette_rgb[i * 3 + 2];
	}
	FILE * file = std::fopen(path.c_str(), "wb");
	if (!file) {
		error = "cannot write " + path;
		return false;
	}
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr,
	                                        nullptr, nullptr);
	png_infop info = png ? png_create_info_struct(png) : nullptr;
	if (!png || !info) {
		if (png) png_destroy_write_struct(&png, nullptr);
		std::fclose(file);
		error = "png: cannot allocate encoder";
		return false;
	}
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		std::fclose(file);
		error = "png: write failed: " + path;
		return false;
	}
	png_init_io(png, file);
	png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_PALETTE,
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
	             PNG_FILTER_TYPE_DEFAULT);
	png_set_PLTE(png, info, palette, int(n_colors));
	png_write_info(png, info);
	png_write_image(png, rows.data());
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
	if (std::fclose(file) != 0) {
		error = "write failed: " + path;
		return false;
	}
	return true;
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
