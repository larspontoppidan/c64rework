// Created  : 2026-10-02
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>
#include <string>

namespace revm {

// Pepto palette (Frodo Display.cpp default), C64 colors 0–15.
extern const uint8_t kPeptoRgb[16][3];

// Indexed PNG (color type 3, 8-bit). `pixels` is w*h bytes, each < n_colors.
// `palette_rgb` is n_colors RGB triples. n_colors is 1..256.
bool WritePngIndexed(const std::string & path, unsigned w, unsigned h,
                     const uint8_t * pixels, const uint8_t * palette_rgb,
                     unsigned n_colors, std::string & error);

// 16-color Pepto PLTE; pixels are masked to 0–15.
bool WritePngPepto(const std::string & path, unsigned w, unsigned h,
                   const uint8_t * pixels, std::string & error);

} // namespace revm
