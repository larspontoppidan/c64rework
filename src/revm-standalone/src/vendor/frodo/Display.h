/*
 *  Display.h - C64 graphics display, emulator window handling
 *
 *  Frodo Copyright (C) Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include "Prefs.h"

#include <SDL.h>

#include <chrono>
#include <string>


// Display dimensions
constexpr unsigned DISPLAY_X = 0x180;
constexpr unsigned DISPLAY_Y = 0x110;

// On-screen notifications
constexpr unsigned NUM_NOTIFICATIONS = 3;
constexpr unsigned NOTIFICATION_LENGTH = 46;


class C64;


// Class for C64 graphics display
class Display {
public:
	Display(C64 * c64);
	~Display();

	void Update();

	void ShowNotification(std::string s);

	uint8_t * BitmapBase();
	int BitmapXMod();

	void PollKeyboard(uint8_t *key_matrix, uint8_t *rev_matrix, uint8_t *joystick);

private:
	void init_colors(int palette_prefs);

	void error_and_quit(const std::string & msg) const;

	void draw_overlays();
	void draw_string(unsigned x, unsigned y, const char *str, uint8_t front_color) const;

	void toggle_fullscreen(bool full);

	C64 * the_c64;						// Pointer to C64 object

	SDL_Window * the_window = nullptr;
	SDL_Renderer * the_renderer = nullptr;
	SDL_Texture * the_texture = nullptr;
	bool window_shown_ = false;

	uint8_t * vic_pixels = nullptr;		// Buffer for VIC to draw into
	uint32_t palette[256];				// Mapping of VIC color values to native ARGB

	struct Notification {
		char text[NOTIFICATION_LENGTH];	// Notification text in C64 screen code
		std::chrono::time_point<std::chrono::steady_clock> time;	// Time of notification
		bool active;
	};

	Notification notes[NUM_NOTIFICATIONS];	// On-screen notifications
	unsigned next_note;					// Index of next free notification

	ButtonMapping button_mapping;		// Controller button mapping
	bool num_locked = false;			// For keyboard joystick swap
};


#endif // ndef DISPLAY_H
