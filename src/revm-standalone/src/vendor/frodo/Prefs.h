/* Standalone preferences shared by the surviving C64 chips and display. */
#ifndef PREFS_H
#define PREFS_H

#include <map>
#include <string>

// SID types.
enum {
	SIDTYPE_NONE,
	SIDTYPE_DIGITAL_6581,
	SIDTYPE_DIGITAL_8580,
	SIDTYPE_SIDCARD,
	SIDTYPE_RESID_6581
};

// Display types.
enum {
	DISPTYPE_WINDOW,
	DISPTYPE_SCREEN
};

// Color palettes.
enum {
	PALETTE_PEPTO,
	PALETTE_COLODORE
};

// Controller button mapping used by the SDL display.
using ButtonMapping = std::map<unsigned, unsigned>;

class Prefs {
public:
	Prefs();

	ButtonMapping SelectedButtonMapping() const { return {}; }

	int SIDType;
	int DisplayType;
	int Palette;
	int ScalingNumerator;
	int ScalingDenominator;

	bool SpriteCollisions;
	bool JoystickSwap;
	bool LimitSpeed;
	bool TestBench;

	// Retained for the display's optional screenshot suppression path.
	std::string TestScreenshotPath;
};

extern Prefs ThePrefs;

#endif // PREFS_H
