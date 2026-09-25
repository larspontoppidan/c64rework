/*
 *  frodo_globals.cpp - Global objects required by vendored Frodo cores
 */

#include "main.h"
#include "C64.h"

C64 * TheC64 = nullptr;

bool Frodo::RunPrefsEditor() {
	// No prefs GUI in REVM — keep running.
	return true;
}

void Frodo::ProcessArgs(int, char **) {}
int Frodo::ReadyToRun() { return 0; }

namespace {

class RevmFrodoStub : public Frodo {
public:
	bool RunPrefsEditor() override { return true; }
};

RevmFrodoStub g_app_stub;

} // namespace

Frodo * TheApp = &g_app_stub;
