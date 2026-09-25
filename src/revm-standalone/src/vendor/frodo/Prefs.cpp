/* Standalone preference defaults.  File-backed Frodo preferences are absent. */

#include "Prefs.h"

Prefs ThePrefs;

Prefs::Prefs()
{
	SIDType = SIDTYPE_DIGITAL_6581;
	DisplayType = DISPTYPE_WINDOW;
	Palette = PALETTE_PEPTO;
	ScalingNumerator = 4;
	ScalingDenominator = 1;

	SpriteCollisions = true;
	JoystickSwap = false;
	LimitSpeed = true;
	TestBench = false;
}
