// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "input/JoystickConfig.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace revm {

namespace {

std::string trim(std::string s) {
	auto not_space = [](unsigned char c) { return !std::isspace(c); };
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
	s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
	return s;
}

std::string lower(std::string s) {
	for (char & c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

const std::unordered_map<std::string, SDL_Scancode> & name_map() {
	static const std::unordered_map<std::string, SDL_Scancode> m = {
		{"a", SDL_SCANCODE_A}, {"b", SDL_SCANCODE_B}, {"c", SDL_SCANCODE_C},
		{"d", SDL_SCANCODE_D}, {"e", SDL_SCANCODE_E}, {"f", SDL_SCANCODE_F},
		{"g", SDL_SCANCODE_G}, {"h", SDL_SCANCODE_H}, {"i", SDL_SCANCODE_I},
		{"j", SDL_SCANCODE_J}, {"k", SDL_SCANCODE_K}, {"l", SDL_SCANCODE_L},
		{"m", SDL_SCANCODE_M}, {"n", SDL_SCANCODE_N}, {"o", SDL_SCANCODE_O},
		{"p", SDL_SCANCODE_P}, {"q", SDL_SCANCODE_Q}, {"r", SDL_SCANCODE_R},
		{"s", SDL_SCANCODE_S}, {"t", SDL_SCANCODE_T}, {"u", SDL_SCANCODE_U},
		{"v", SDL_SCANCODE_V}, {"w", SDL_SCANCODE_W}, {"x", SDL_SCANCODE_X},
		{"y", SDL_SCANCODE_Y}, {"z", SDL_SCANCODE_Z},
		{"1", SDL_SCANCODE_1}, {"2", SDL_SCANCODE_2}, {"3", SDL_SCANCODE_3},
		{"4", SDL_SCANCODE_4}, {"5", SDL_SCANCODE_5}, {"6", SDL_SCANCODE_6},
		{"7", SDL_SCANCODE_7}, {"8", SDL_SCANCODE_8}, {"9", SDL_SCANCODE_9},
		{"0", SDL_SCANCODE_0},
		{"space", SDL_SCANCODE_SPACE},
		{"return", SDL_SCANCODE_RETURN}, {"enter", SDL_SCANCODE_RETURN},
		{"tab", SDL_SCANCODE_TAB}, {"escape", SDL_SCANCODE_ESCAPE}, {"esc", SDL_SCANCODE_ESCAPE},
		{"backspace", SDL_SCANCODE_BACKSPACE}, {"delete", SDL_SCANCODE_DELETE}, {"del", SDL_SCANCODE_DELETE},
		{"home", SDL_SCANCODE_HOME}, {"end", SDL_SCANCODE_END},
		{"pageup", SDL_SCANCODE_PAGEUP}, {"pgup", SDL_SCANCODE_PAGEUP},
		{"pagedown", SDL_SCANCODE_PAGEDOWN}, {"pgdn", SDL_SCANCODE_PAGEDOWN}, {"pgdown", SDL_SCANCODE_PAGEDOWN},
		{"left", SDL_SCANCODE_LEFT}, {"right", SDL_SCANCODE_RIGHT},
		{"up", SDL_SCANCODE_UP}, {"down", SDL_SCANCODE_DOWN},
		{"lshift", SDL_SCANCODE_LSHIFT}, {"rshift", SDL_SCANCODE_RSHIFT},
		{"lctrl", SDL_SCANCODE_LCTRL}, {"rctrl", SDL_SCANCODE_RCTRL},
		{"leftctrl", SDL_SCANCODE_LCTRL}, {"rightctrl", SDL_SCANCODE_RCTRL},
		{"lalt", SDL_SCANCODE_LALT}, {"ralt", SDL_SCANCODE_RALT},
		{"comma", SDL_SCANCODE_COMMA}, {"period", SDL_SCANCODE_PERIOD}, {"dot", SDL_SCANCODE_PERIOD},
		{"slash", SDL_SCANCODE_SLASH}, {"backslash", SDL_SCANCODE_BACKSLASH},
		{"semicolon", SDL_SCANCODE_SEMICOLON}, {"quote", SDL_SCANCODE_APOSTROPHE},
		{"apostrophe", SDL_SCANCODE_APOSTROPHE}, {"grave", SDL_SCANCODE_GRAVE},
		{"minus", SDL_SCANCODE_MINUS}, {"equals", SDL_SCANCODE_EQUALS},
		{"leftbracket", SDL_SCANCODE_LEFTBRACKET}, {"rightbracket", SDL_SCANCODE_RIGHTBRACKET},
		{"kp_0", SDL_SCANCODE_KP_0}, {"kp_1", SDL_SCANCODE_KP_1}, {"kp_2", SDL_SCANCODE_KP_2},
		{"kp_3", SDL_SCANCODE_KP_3}, {"kp_4", SDL_SCANCODE_KP_4}, {"kp_5", SDL_SCANCODE_KP_5},
		{"kp_6", SDL_SCANCODE_KP_6}, {"kp_7", SDL_SCANCODE_KP_7}, {"kp_8", SDL_SCANCODE_KP_8},
		{"kp_9", SDL_SCANCODE_KP_9},
		{"kp_enter", SDL_SCANCODE_KP_ENTER}, {"kp_plus", SDL_SCANCODE_KP_PLUS},
		{"kp_minus", SDL_SCANCODE_KP_MINUS},
		{"f1", SDL_SCANCODE_F1}, {"f2", SDL_SCANCODE_F2}, {"f3", SDL_SCANCODE_F3},
		{"f4", SDL_SCANCODE_F4}, {"f5", SDL_SCANCODE_F5}, {"f6", SDL_SCANCODE_F6},
		{"f7", SDL_SCANCODE_F7}, {"f8", SDL_SCANCODE_F8}, {"f9", SDL_SCANCODE_F9},
		{"f10", SDL_SCANCODE_F10}, {"f11", SDL_SCANCODE_F11}, {"f12", SDL_SCANCODE_F12},
	};
	return m;
}

bool set_field(JoyBindings & b, const std::string & field, const std::string & value, std::string & error) {
	if (value.empty()) return true;
	SDL_Scancode sc;
	if (!ParseScancodeName(value, sc)) {
		error = "Unknown key name: '" + value + "'";
		return false;
	}
	if (sc == SDL_SCANCODE_F9 || sc == SDL_SCANCODE_F10 || sc == SDL_SCANCODE_F12) {
		error = "F9, F10, and F12 are reserved REVM keys";
		return false;
	}
	if (field == "up") b.up = sc;
	else if (field == "down") b.down = sc;
	else if (field == "left") b.left = sc;
	else if (field == "right") b.right = sc;
	else if (field == "fire") b.fire = sc;
	else {
		error = "Unknown joystick field: '" + field + "'";
		return false;
	}
	return true;
}

bool set_cheat_field(CheatBindings & b, const std::string & field, const std::string & value,
	                 std::string & error) {
	SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
	if (!value.empty() && !ParseScancodeName(value, sc)) {
		error = "Unknown key name: '" + value + "'";
		return false;
	}
	if (sc == SDL_SCANCODE_F9 || sc == SDL_SCANCODE_F10 || sc == SDL_SCANCODE_F12) {
		error = "F9, F10, and F12 are reserved REVM keys";
		return false;
	}
	if (field == "increment") b.increment = sc;
	else if (field == "decrement") b.decrement = sc;
	else {
		error = "Unknown cheats field: '" + field + "'";
		return false;
	}
	return true;
}

} // namespace

bool ParseScancodeName(const std::string & name, SDL_Scancode & out) {
	auto it = name_map().find(lower(trim(name)));
	if (it == name_map().end()) return false;
	out = it->second;
	return true;
}

JoystickConfig JoystickConfig::DefaultKeypadJoy2() {
	JoystickConfig c;
	c.joy2.up = SDL_SCANCODE_KP_8;
	c.joy2.down = SDL_SCANCODE_KP_2;
	c.joy2.left = SDL_SCANCODE_KP_4;
	c.joy2.right = SDL_SCANCODE_KP_6;
	c.joy2.fire = SDL_SCANCODE_KP_0;
	// Diagonals via kp_7/9/1/3 are handled by combining axes when both pressed —
	// individual diagonal keys also map:
	// (we only store cardinals+fire; diagonals = simultaneous cardinals)
	c.joy1 = JoyBindings{};
	return c;
}

const char * JoystickConfig::DefaultFileTemplate() {
	return R"REVM_CFG(# REVM configuration
#
# Key names are SDL scancode names without the SDL_SCANCODE_ prefix,
# lower-case, with underscores. Examples:
#   a  w  space  return  left  right  up  down  home  end  delete  pagedown  lctrl
#   kp_8  comma  period  slash  semicolon  quote  grave
#
# Leave a field empty (or omit the whole [joystickN] section) to leave
# that direction unbound.
#
# Keys bound to a joystick are suppressed from the C64 keyboard matrix so
# they don't also type CLR/HOME, DEL, etc.

# Cheat keys apply to the selected variable in the KB watch window. They are
# suppressed from the C64 keyboard while a variable is selected. F9 (timestamp),
# F10 (pause/resume), and F12 (quit) are fixed REVM keys and cannot be used here.
[cheats]
increment = pageup
decrement = pagedown

# MacBook / laptop: arrow keys + Left Shift fire → joy2
[joystick2]
up = up
down = down
left = left
right = right
fire = lshift

# Optional joy1 (unbound by default)
[joystick1]
up =
down =
left =
right =
fire =
)REVM_CFG";
}

bool JoystickConfig::WriteDefaultFile(const std::string & path,
                                      std::string & error) {
	std::ofstream f(path, std::ios::out | std::ios::trunc);
	if (!f) {
		error = "Cannot create default configuration " + path;
		return false;
	}
	f << DefaultFileTemplate();
	if (!f) {
		error = "Cannot write default configuration " + path;
		return false;
	}
	return true;
}

bool JoystickConfig::LoadFile(const std::string & path, std::string & error) {
	std::ifstream f(path);
	if (!f) {
		error = "Cannot open " + path;
		return false;
	}

	joy1 = JoyBindings{};
	joy2 = JoyBindings{};
	cheats = CheatBindings{};
	enum class Section { None, Joystick1, Joystick2, Cheats };
	Section section = Section::None;

	std::string line;
	int lineno = 0;
	while (std::getline(f, line)) {
		++lineno;
		auto hash = line.find('#');
		if (hash != std::string::npos) line = line.substr(0, hash);
		line = trim(line);
		if (line.empty()) continue;

		if (line.front() == '[' && line.back() == ']') {
			std::string sec = lower(line.substr(1, line.size() - 2));
			if (sec == "joystick1" || sec == "joy1") section = Section::Joystick1;
			else if (sec == "joystick2" || sec == "joy2") section = Section::Joystick2;
			else if (sec == "cheats") section = Section::Cheats;
			else {
				error = path + ":" + std::to_string(lineno) + ": unknown section [" + sec + "]";
				return false;
			}
			continue;
		}

		auto eq = line.find('=');
		if (eq == std::string::npos) {
			error = path + ":" + std::to_string(lineno) + ": expected key = value";
			return false;
		}
		if (section == Section::None) {
			error = path + ":" + std::to_string(lineno) + ": key outside a section";
			return false;
		}
		std::string field = lower(trim(line.substr(0, eq)));
		std::string value = trim(line.substr(eq + 1));
		std::string field_err;
		bool ok = false;
		if (section == Section::Joystick1) ok = set_field(joy1, field, value, field_err);
		else if (section == Section::Joystick2) ok = set_field(joy2, field, value, field_err);
		else ok = set_cheat_field(cheats, field, value, field_err);
		if (!ok) {
			error = path + ":" + std::to_string(lineno) + ": " + field_err;
			return false;
		}
	}
	if (cheats.increment != SDL_SCANCODE_UNKNOWN && cheats.increment == cheats.decrement) {
		error = path + ": cheat increment and decrement keys must differ";
		return false;
	}
	return true;
}

} // namespace revm
