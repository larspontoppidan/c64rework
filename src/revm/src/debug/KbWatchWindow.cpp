// Created  : 2026-07-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "debug/KbWatchWindow.hpp"

#include "debug/Spleen8x16Font.h"
#include "debug/Spleen12x24Font.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#define REVM_LOG_MODULE "watch-win"
#include "util/Log.hpp"

namespace revm {
namespace {

constexpr uint32_t kBg = 0xFF1A1A1A;
constexpr uint32_t kFg = 0xFFD0D0D0;
constexpr uint32_t kHeader = 0xFF8EC8FF;
constexpr uint32_t kFlashBg = 0xFF3A3020;
constexpr uint32_t kFlashFg = 0xFFFFE080;
constexpr uint32_t kSelectedBg = 0xFF204060;
constexpr uint32_t kDim = 0xFF808080;

} // namespace

KbWatchWindow::KbWatchWindow() = default;

KbWatchWindow::~KbWatchWindow() {
	Close();
}

bool KbWatchWindow::ParseFontName(const std::string & name, WatchFont & out, std::string & error) {
	if (name == "8x16" || name == "8" || name == "spleen8x16") {
		out = WatchFont::Spleen8x16;
		return true;
	}
	if (name == "12x24" || name == "12" || name == "spleen12x24") {
		out = WatchFont::Spleen12x24;
		return true;
	}
	error = "unknown --watch-font '" + name + "' (use 8x16 or 12x24)";
	return false;
}

int KbWatchWindow::EventWatchThunk(void * userdata, SDL_Event * event) {
	if (userdata && event) {
		static_cast<KbWatchWindow *>(userdata)->on_event(*event);
	}
	return 0;
}

void KbWatchWindow::on_event(const SDL_Event & event) {
	if (!window_) return;
	if (event.type == SDL_WINDOWEVENT && event.window.windowID == window_id_) {
		if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
			close_requested_ = true;
		} else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
		           event.window.event == SDL_WINDOWEVENT_RESIZED) {
			size_dirty_ = true;
		}
	} else if (event.type == SDL_MOUSEBUTTONDOWN &&
	           event.button.windowID == window_id_ &&
	           event.button.button == SDL_BUTTON_LEFT) {
		const int rows_y = kPad + row_h_ + 2;
		const int rel_y = event.button.y - rows_y;
		if (rel_y >= 0) {
			const size_t row = size_t(rel_y / row_h_);
			if (row < displayed_addresses_.size() && displayed_addresses_[row]) {
				selected_address_ = *displayed_addresses_[row];
				paint_dirty_ = true;
			}
		}
	}
}

bool KbWatchWindow::Open(size_t slot_count, WatchFont font, std::string & error) {
	Close();
	font_ = font;
	if (font_ == WatchFont::Spleen8x16) {
		glyph_w_ = spleen_8x16_width;
		glyph_h_ = spleen_8x16_height;
	} else {
		glyph_w_ = spleen_12x24_width;
		glyph_h_ = spleen_12x24_height;
	}
	row_h_ = glyph_h_ + 4;

	const int rows = 2 + int(std::min(slot_count, size_t(kMaxRowsFit))) +
	                 (slot_count > size_t(kMaxRowsFit) ? 1 : 0) + 2;
	const int h = kPad * 2 + rows * row_h_;
	// ~72 columns of text for typical NAME/ADDR/VALUE layout
	const int w = std::max(640, kPad * 2 + 72 * glyph_w_);

	window_ = SDL_CreateWindow("REVM watches", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w,
	                           h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!window_) {
		error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
		return false;
	}
	window_id_ = SDL_GetWindowID(window_);

	renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
	if (!renderer_) {
		renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
	}
	if (!renderer_) {
		error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
		Close();
		return false;
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

	if (!resize_buffers(w, h, error)) {
		Close();
		return false;
	}

	SDL_AddEventWatch(EventWatchThunk, this);
	event_watch_installed_ = true;
	close_requested_ = false;
	size_dirty_ = false;
	paint_dirty_ = true;
	highlight_until_frame_ = 0;
	selected_address_.reset();
	displayed_addresses_.clear();
	pending_modifications_.clear();
	increment_was_down_ = false;
	decrement_was_down_ = false;
	return true;
}

void KbWatchWindow::Close() {
	if (event_watch_installed_) {
		SDL_DelEventWatch(EventWatchThunk, this);
		event_watch_installed_ = false;
	}
	if (texture_) {
		SDL_DestroyTexture(texture_);
		texture_ = nullptr;
	}
	if (renderer_) {
		SDL_DestroyRenderer(renderer_);
		renderer_ = nullptr;
	}
	if (window_) {
		SDL_DestroyWindow(window_);
		window_ = nullptr;
	}
	window_id_ = 0;
	pixels_.clear();
	width_ = height_ = 0;
	close_requested_ = false;
	size_dirty_ = false;
	paint_dirty_ = false;
	selected_address_.reset();
	displayed_addresses_.clear();
	pending_modifications_.clear();
	increment_was_down_ = false;
	decrement_was_down_ = false;
}

void KbWatchWindow::PollCheatKeys(SDL_Scancode increment_key, SDL_Scancode decrement_key) {
	const Uint8 * keys = SDL_GetKeyboardState(nullptr);
	const bool increment_down = keys && increment_key != SDL_SCANCODE_UNKNOWN && keys[increment_key];
	const bool decrement_down = keys && decrement_key != SDL_SCANCODE_UNKNOWN && keys[decrement_key];
	if (selected_address_) {
		if (increment_down && !increment_was_down_) {
			pending_modifications_.push_back(
			    {*selected_address_, MemoryModOperation::Increment});
		}
		if (decrement_down && !decrement_was_down_) {
			pending_modifications_.push_back(
			    {*selected_address_, MemoryModOperation::Decrement});
		}
	}
	increment_was_down_ = increment_down;
	decrement_was_down_ = decrement_down;
}

std::vector<MemoryModification> KbWatchWindow::TakePendingModifications() {
	std::vector<MemoryModification> out;
	out.swap(pending_modifications_);
	return out;
}

bool KbWatchWindow::ConsumesCheatKeys() const {
	return window_ && selected_address_.has_value();
}

bool KbWatchWindow::resize_buffers(int w, int h, std::string & error) {
	if (w < 64) w = 64;
	if (h < 48) h = 48;
	if (texture_) {
		SDL_DestroyTexture(texture_);
		texture_ = nullptr;
	}
	texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
	                             SDL_TEXTUREACCESS_STREAMING, w, h);
	if (!texture_) {
		error = std::string("SDL_CreateTexture failed: ") + SDL_GetError();
		return false;
	}
	width_ = w;
	height_ = h;
	pixels_.assign(size_t(w) * size_t(h), kBg);
	return true;
}

void KbWatchWindow::fill_rect(int x, int y, int w, int h, uint32_t argb) {
	if (w <= 0 || h <= 0) return;
	const int x0 = std::max(0, x);
	const int y0 = std::max(0, y);
	const int x1 = std::min(width_, x + w);
	const int y1 = std::min(height_, y + h);
	for (int py = y0; py < y1; ++py) {
		uint32_t * row = pixels_.data() + size_t(py) * size_t(width_);
		for (int px = x0; px < x1; ++px) row[px] = argb;
	}
}

void KbWatchWindow::draw_string(int x, int y, const char * str, uint32_t argb) {
	if (!str || y < 0 || y + glyph_h_ > height_) return;
	unsigned char c;
	int cx = x;
	while ((c = static_cast<unsigned char>(*str++)) != 0) {
		if (c >= 0x80) c = 0x7f;
		if (cx >= width_) break;

		if (font_ == WatchFont::Spleen8x16) {
			const uint8_t * q = spleen_8x16_font + c * glyph_h_;
			for (int row = 0; row < glyph_h_; ++row) {
				uint8_t v = q[row];
				uint32_t * dest =
				    pixels_.data() + size_t(y + row) * size_t(width_) + size_t(cx);
				for (int col = 0; col < glyph_w_; ++col) {
					if ((v & 0x80) && cx + col >= 0 && cx + col < width_) {
						dest[col] = argb;
					}
					v <<= 1;
				}
			}
		} else {
			const uint16_t * q = spleen_12x24_font + c * glyph_h_;
			for (int row = 0; row < glyph_h_; ++row) {
				uint16_t v = q[row];
				uint32_t * dest =
				    pixels_.data() + size_t(y + row) * size_t(width_) + size_t(cx);
				for (int col = 0; col < glyph_w_; ++col) {
					if ((v & 0x8000) && cx + col >= 0 && cx + col < width_) {
						dest[col] = argb;
					}
					v <<= 1;
				}
			}
		}
		cx += glyph_w_;
	}
}

void KbWatchWindow::paint(const KbWatch & watch, uint32_t frame) {
	fill_rect(0, 0, width_, height_, kBg);

	constexpr int kNameCol = 0;
	constexpr int kAddrCol = 17;
	constexpr int kValCol = 30;

	int y = kPad;
	draw_string(kPad + kNameCol * glyph_w_, y, "NAME", kHeader);
	draw_string(kPad + kAddrCol * glyph_w_, y, "ADDR", kHeader);
	draw_string(kPad + kValCol * glyph_w_, y, "VALUE", kHeader);
	y += row_h_ + 2;

	const auto & views = watch.Views();
	const int usable = height_ - y - kPad - row_h_;
	const int max_rows = std::max(0, usable / row_h_);
	const size_t show = std::min(views.size(), size_t(max_rows > 0 ? max_rows : 0));
	const bool truncated = views.size() > show;
	const size_t row_budget = truncated && show > 0 ? show - 1 : show;
	displayed_addresses_.clear();
	displayed_addresses_.reserve(row_budget);

	for (size_t i = 0; i < row_budget; ++i) {
		const auto & v = views[i];
		displayed_addresses_.push_back(v.modifiable
		                                 ? std::optional<uint16_t>(v.addr)
		                                 : std::nullopt);
		const bool flash =
		    v.changed_at_frame != 0 && frame >= v.changed_at_frame &&
		    (frame - v.changed_at_frame) < kFlashFrames;
		const bool selected =
		    v.modifiable && selected_address_ && *selected_address_ == v.addr;
		if (selected) {
			fill_rect(0, y - 1, width_, row_h_, kSelectedBg);
		} else if (flash) {
			fill_rect(0, y - 1, width_, row_h_, kFlashBg);
		}
		const uint32_t fg = flash ? kFlashFg : kFg;

		char namebuf[64];
		std::snprintf(namebuf, sizeof(namebuf), "%-16.16s", v.name);

		char addrbuf[24];
		if (v.len <= 1) {
			std::snprintf(addrbuf, sizeof(addrbuf), "$%04X", v.addr);
		} else {
			std::snprintf(addrbuf, sizeof(addrbuf), "$%04X-$%04X", v.addr,
			              uint16_t(v.addr + v.len - 1));
		}

		char valbuf[80];
		if (v.len == 1 && v.bytes) {
			std::snprintf(valbuf, sizeof(valbuf), "$%02X  %u", v.bytes[0],
			              unsigned(v.bytes[0]));
		} else if (v.bytes) {
			const std::string hex = KbWatch::FormatBytes(v.bytes, v.len);
			std::snprintf(valbuf, sizeof(valbuf), "%s", hex.c_str());
		} else {
			valbuf[0] = '\0';
		}

		draw_string(kPad + kNameCol * glyph_w_, y, namebuf, fg);
		draw_string(kPad + kAddrCol * glyph_w_, y, addrbuf, fg);
		draw_string(kPad + kValCol * glyph_w_, y, valbuf, fg);
		y += row_h_;
	}

	if (truncated) {
		char more[40];
		std::snprintf(more, sizeof(more), "... +%zu more", views.size() - row_budget);
		draw_string(kPad, y, more, kDim);
		y += row_h_;
	}

	char foot[48];
	std::snprintf(foot, sizeof(foot), "frame=%u  slots=%zu", frame, views.size());
	draw_string(kPad, height_ - kPad - glyph_h_, foot, kDim);

	SDL_UpdateTexture(texture_, nullptr, pixels_.data(), width_ * int(sizeof(uint32_t)));
	SDL_SetRenderDrawColor(renderer_, 0x1A, 0x1A, 0x1A, 255);
	SDL_RenderClear(renderer_);
	SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
	SDL_RenderPresent(renderer_);
}

void KbWatchWindow::OnFrame(KbWatch & watch, uint32_t frame) {
	if (close_requested_) {
		Close();
		return;
	}
	if (!IsOpen()) return;

	if (size_dirty_) {
		int w = 0, h = 0;
		SDL_GetWindowSize(window_, &w, &h);
		std::string err;
		if (!resize_buffers(w, h, err)) {
			REVM_LOG(REVM_ERROR, "resize failed: %s", err.c_str());
			Close();
			return;
		}
		size_dirty_ = false;
		watch.MarkDirty();
	}

	if (watch.Dirty() || paint_dirty_) {
		paint(watch, frame);
		watch.ClearDirty();
		paint_dirty_ = false;
		uint32_t flash_end = 0;
		for (const auto & v : watch.Views()) {
			if (v.changed_at_frame == 0) continue;
			const uint32_t end = v.changed_at_frame + kFlashFrames;
			if (end > flash_end) flash_end = end;
		}
		highlight_until_frame_ = flash_end;
		return;
	}

	if (highlight_until_frame_ != 0 && frame >= highlight_until_frame_) {
		paint(watch, frame);
		highlight_until_frame_ = 0;
	}
}

} // namespace revm
