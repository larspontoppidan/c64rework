// Created  : 2026-09-10
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "cpumock/VideoAssets.hpp"

#include <array>
#include <cstdint>

namespace revm::cpumock {

// Compile-only API probe.  Keeping this as an object target catches accidental
// changes to the typed asset sizes and the live byte-view spelling without
// requiring a machine (or a BEGIN snapshot) at build time.
void video_assets_compile_probe(CpuMockHost &host) {
	VideoAssets video{host};
	std::array<uint8_t, VideoAssets::Charset::kBytes> charset{};
	std::array<uint8_t, VideoAssets::SpriteSlot::kBytes> sprite{};

	video.charset.install(charset);
	video.sprites.slot(uint8_t{0x70}).install(sprite);
	video.charset.glyph(0x41).row(0).write(0xFF);
	video.sprites.slot(0x70).byte(63).write(0xFF);
}

} // namespace revm::cpumock
