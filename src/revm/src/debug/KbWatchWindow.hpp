// Created  : 2026-07-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "debug/KbWatch.hpp"
#include "input/MemoryModification.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <SDL_scancode.h>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
union SDL_Event;

namespace revm {

enum class WatchFont {
	Spleen8x16,
	Spleen12x24,
};

// Second SDL window listing KbWatch slots. No scroll: fit or truncate.
// Redraws only when KbWatch is dirty (or on resize / highlight expiry).
// Fonts: embedded Spleen 8x16 / 12x24 (BSD-2-Clause; see LICENSE.spleen).
class KbWatchWindow {
public:
	KbWatchWindow();
	~KbWatchWindow();

	KbWatchWindow(const KbWatchWindow &) = delete;
	KbWatchWindow & operator=(const KbWatchWindow &) = delete;

	bool Open(size_t slot_count, WatchFont font, std::string & error);
	void Close();
	bool IsOpen() const { return window_ != nullptr; }

	void OnFrame(KbWatch & watch, uint32_t frame);
	// Poll host shortcut keys once after LiveInput has pumped SDL for this frame.
	void PollCheatKeys(SDL_Scancode increment_key, SDL_Scancode decrement_key);
	std::vector<MemoryModification> TakePendingModifications();
	bool ConsumesCheatKeys() const;

	static bool ParseFontName(const std::string & name, WatchFont & out, std::string & error);

private:
	static int EventWatchThunk(void * userdata, SDL_Event * event);
	void on_event(const SDL_Event & event);
	bool resize_buffers(int w, int h, std::string & error);
	void paint(const KbWatch & watch, uint32_t frame);
	void draw_string(int x, int y, const char * str, uint32_t argb);
	void fill_rect(int x, int y, int w, int h, uint32_t argb);

	SDL_Window * window_ = nullptr;
	SDL_Renderer * renderer_ = nullptr;
	SDL_Texture * texture_ = nullptr;
	uint32_t window_id_ = 0;

	WatchFont font_ = WatchFont::Spleen12x24;
	int glyph_w_ = 12;
	int glyph_h_ = 24;
	int row_h_ = 28;

	int width_ = 0;
	int height_ = 0;
	std::vector<uint32_t> pixels_;

	bool event_watch_installed_ = false;
	bool close_requested_ = false;
	bool size_dirty_ = false;
	bool paint_dirty_ = false;
	uint32_t highlight_until_frame_ = 0;
	std::optional<uint16_t> selected_address_;
	std::vector<std::optional<uint16_t>> displayed_addresses_;
	std::vector<MemoryModification> pending_modifications_;
	bool increment_was_down_ = false;
	bool decrement_was_down_ = false;

	static constexpr int kPad = 10;
	static constexpr int kMaxRowsFit = 40;
	static constexpr uint32_t kFlashFrames = 25;
};

} // namespace revm
