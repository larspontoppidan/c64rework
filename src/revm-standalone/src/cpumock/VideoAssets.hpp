// Created  : 2026-09-10
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Named, mutable VIC data surfaces for Stage 4 replicas.
//
// `install` is a startup operation: it copies an asset into Main's current
// VIC bank and does not walk Twin or advance the emulated clock.  The objects
// returned by glyph()/row()/byte() are ordinary RAM byte views, so subsequent
// writes remain visible to the VIC and can be made live by the game.

#include "gamehost/GameHost.hpp"
#include "cpumock/Mem.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace revm::cpumock {

namespace video_detail {

// The host keeps raw memory operations private to replica memory facades.
// VideoAssets is one such facade; this bridge is intentionally not part of
// the game-facing API.
struct HostAccess {
	static uint8_t read(gamehost::GameHost &host, uint16_t addr) {
		return host.Read(addr);
	}
	static void install(gamehost::GameHost &host, uint16_t addr,
	                    std::span<const uint8_t> bytes) {
		host.InstallAsset(addr, bytes);
	}
};

// The VIC sees a 16 KiB bank selected by CIA2 PRA bits 0..1 (inverted).  Use
// a bank-relative offset and mask at 16 KiB so the last charset page has the
// same wraparound as the physical VIC bank (for example $F800 + $800).
inline uint16_t vic_bank(gamehost::GameHost & host) {
	return uint16_t(0xC000u -
	                (uint16_t(HostAccess::read(host, 0xDD00)) & 3u) * 0x4000u);
}

inline uint16_t vic_address(gamehost::GameHost & host, uint16_t bank_offset) {
	return uint16_t(vic_bank(host) | (bank_offset & 0x3FFFu));
}

template <std::size_t Expected>
inline void install_asset(gamehost::GameHost & host, std::span<const uint8_t> asset,
                          const char *kind, uint16_t bank_offset) {
	if (asset.size() != Expected) {
		host.Fail(1, "video.%s asset has %zu bytes; want %zu", kind,
		              asset.size(), Expected);
		return;
	}
	HostAccess::install(host, vic_address(host, bank_offset), asset);
}

} // namespace video_detail

class VideoAssets {
	gamehost::GameHost &host_;

public:
	using Byte = ByteAt<RamSync>;

	class Charset {
	public:
		static constexpr std::size_t kBytes = 2048;
		static constexpr std::size_t kGlyphs = 256;
		static constexpr std::size_t kRows = 8;

		explicit Charset(gamehost::GameHost & host) : host_(host) {}

		template <std::size_t N>
		void install(const std::array<uint8_t, N> &asset) const {
			static_assert(N == kBytes,
			              "video.charset.install requires exactly 2048 bytes");
			video_detail::install_asset<kBytes>(host_,
			                              std::span<const uint8_t>{asset},
			                              "charset", charset_offset());
		}

		// C-array overload keeps the same compile-time size check for plain
		// exported asset blobs.
		template <std::size_t N>
		void install(const uint8_t (&asset)[N]) const {
			static_assert(N == kBytes,
			              "video.charset.install requires exactly 2048 bytes");
			video_detail::install_asset<kBytes>(host_,
			                              std::span<const uint8_t>{asset},
			                              "charset", charset_offset());
		}

		// Dynamic spans are useful for loaders; retain a loud runtime size
		// check when the asset did not originate as a fixed-size C++ object.
		void install(std::span<const uint8_t> asset) const {
			video_detail::install_asset<kBytes>(host_, asset, "charset",
			                              charset_offset());
		}

		class Glyph {
		public:
			Glyph(gamehost::GameHost &host, uint8_t glyph) : host_(host), glyph_(glyph) {}

			Byte row(int row) const {
				if (unsigned(row) >= kRows) {
					host_.Fail(1, "video.charset.glyph(%u).row(%d) not in 0..7",
					               unsigned(glyph_), row);
					return Byte{host_, 0};
				}
				return Byte{host_, video_detail::vic_address(
					                     host_, uint16_t(charset_offset() + glyph_ * 8u + row))};
			}

		private:
			uint16_t charset_offset() const {
				return uint16_t((uint16_t(video_detail::HostAccess::read(host_, 0xD018)) &
				                 0x0Eu)
				                << 10);
			}

			gamehost::GameHost &host_;
			uint8_t glyph_;
		};

		Glyph glyph(int glyph) const {
			if (unsigned(glyph) >= kGlyphs) {
				host_.Fail(1, "video.charset.glyph(%d) not in 0..255", glyph);
				return Glyph{host_, 0};
			}
			return Glyph{host_, uint8_t(glyph)};
		}

		// Direct character-page byte view for remastering tools and bulk
		// transforms that already operate in the VIC's linear byte layout.
		Byte byte(int offset) const {
			if (unsigned(offset) >= kBytes) {
				host_.Fail(1, "video.charset.byte(%d) not in 0..2047", offset);
				return Byte{host_, 0};
			}
			return Byte{host_, video_detail::vic_address(host_,
			                                      uint16_t(charset_offset() + offset))};
		}

	private:
		uint16_t charset_offset() const {
			return uint16_t((uint16_t(video_detail::HostAccess::read(host_, 0xD018)) &
			                 0x0Eu)
			                << 10);
		}

		gamehost::GameHost &host_;
	};

	class SpriteSlot {
	public:
		static constexpr std::size_t kBytes = 64;

		SpriteSlot(gamehost::GameHost &host, uint8_t pointer)
			: host_(host), pointer_(pointer) {}

		template <std::size_t N>
		void install(const std::array<uint8_t, N> &asset) const {
			static_assert(N == kBytes,
			              "video.sprites.slot.install requires exactly 64 bytes");
			video_detail::install_asset<kBytes>(host_,
			                              std::span<const uint8_t>{asset},
			                              "sprite", sprite_offset());
		}

		template <std::size_t N>
		void install(const uint8_t (&asset)[N]) const {
			static_assert(N == kBytes,
			              "video.sprites.slot.install requires exactly 64 bytes");
			video_detail::install_asset<kBytes>(host_,
			                              std::span<const uint8_t>{asset},
			                              "sprite", sprite_offset());
		}

		void install(std::span<const uint8_t> asset) const {
			video_detail::install_asset<kBytes>(host_, asset, "sprite",
			                              sprite_offset());
		}

		Byte byte(int offset) const {
			if (unsigned(offset) >= kBytes) {
				host_.Fail(1,
				               "video.sprites.slot($%02X).byte(%d) not in 0..63",
				               unsigned(pointer_), offset);
				return Byte{host_, 0};
			}
			return Byte{host_, video_detail::vic_address(host_,
			                                      uint16_t(sprite_offset() + offset))};
		}

		uint8_t pointer() const { return pointer_; }

	private:
		uint16_t sprite_offset() const { return uint16_t(pointer_) << 6; }

		gamehost::GameHost &host_;
		uint8_t pointer_;
	};

	class Sprites {
	public:
		explicit Sprites(gamehost::GameHost &host) : host_(host) {}

		// A VIC sprite pointer is an 8-bit index into the active 16 KiB bank.
		// Accept a wider integer so accidental negative/out-of-range values are
		// diagnosed instead of silently truncating to another sprite.
		SpriteSlot slot(uint16_t pointer) const {
			if (pointer > 0xFFu) {
				host_.Fail(1, "video.sprites.slot($%04X) not in $00..$FF",
				               unsigned(pointer));
				return SpriteSlot{host_, 0};
			}
			return SpriteSlot{host_, uint8_t(pointer)};
		}

	private:
		gamehost::GameHost &host_;
	};

	explicit VideoAssets(gamehost::GameHost &host) : host_(host), charset(host), sprites(host) {}

	gamehost::GameHost &host() const { return host_; }

	Charset charset;
	Sprites sprites;
};

} // namespace revm::cpumock
