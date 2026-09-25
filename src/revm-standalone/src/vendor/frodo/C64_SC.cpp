/*
 *  C64_SC.cpp - Standalone Frodo SC hardware wiring
 *
 *  The desktop Frodo C64 aggregate also owns drives, IEC, tape, cartridges,
 *  snapshots, preferences, and its application loop.  None of those can be
 *  reached by the standalone Board, so this translation unit wires only the
 *  C64 memory and chips that remain in the runtime.
 */

#include "sysdeps.h"

#include "C64.h"
#include "CIA.h"
#include "CPUC64.h"
#include "Display.h"
#include "SID.h"
#include "VIC.h"

#include <utility>

namespace {

// Color RAM power-on noise.  Keep this private to the standalone runtime;
// libc rand()/srand() is also used by SDL and must not be perturbed.
unsigned color_ram_seed = 42;

uint8_t color_ram_nibble(uint32_t &state) {
	state = state * 1664525u + 1013904223u;
	return uint8_t((state >> 16) & 0x0f);
}

} // namespace

#ifdef FRODO_SC
bool IsFrodoSC = true;
#else
bool IsFrodoSC = false;
#endif

void SetFrodoColorRamSeed(unsigned seed) {
	color_ram_seed = seed;
}

C64::C64()
    : RAM(new uint8_t[C64_RAM_SIZE]()),
      Basic(new uint8_t[BASIC_ROM_SIZE]()),
      Kernal(new uint8_t[KERNAL_ROM_SIZE]()),
      Char(new uint8_t[CHAR_ROM_SIZE]()),
      Color(new uint8_t[COLOR_RAM_SIZE]()),
      TheDisplay(nullptr),
      TheCPU(nullptr),
      TheVIC(nullptr),
      TheSID(nullptr),
      TheCIA1(nullptr),
      TheCIA2(nullptr) {
	init_memory();

	TheDisplay = new Display(this);
	TheCPU = new MOS6510(this, RAM, Basic, Kernal, Char, Color);
	TheVIC = new MOS6569(this, TheDisplay, TheCPU, RAM, Char, Color);
	TheSID = new MOS6581;
	TheSID->SetC64(this);
	TheCIA1 = new MOS6526_1(TheCPU, TheVIC);

	// CIA2 still drives VIC bank selection.  IEC/1541 wiring is gone.
	TheCIA2 = new MOS6526_2(TheCPU, TheVIC);
	TheCPU->SetChips(TheVIC, TheSID, TheCIA1, TheCIA2);
}

C64::~C64() {
	delete TheCIA2;
	delete TheCIA1;
	delete TheSID;
	delete TheVIC;
	delete TheCPU;
	delete TheDisplay;

	delete[] RAM;
	delete[] Basic;
	delete[] Kernal;
	delete[] Char;
	delete[] Color;
}

void C64::init_memory() {
	// PAL C64 power-up RAM pattern (Assy 250425 / Fujitsu MB8264A-15).
	uint8_t *p = RAM;
	for (unsigned i = 0; i < 512; ++i) {
		for (unsigned j = 0; j < 64; ++j) {
			if (j == 4 || j == 5)
				*p++ = (i & 1) ? 0x03 : 0x01;
			else if (j == 7)
				*p++ = 0x07;
			else if (j == 32 || j == 57 || j == 58)
				*p++ = 0xff;
			else if (j == 55)
				*p++ = (i & 1) ? 0x07 : 0x05;
			else if (j == 56)
				*p++ = (i & 1) ? 0x2f : 0x27;
			else if (j == 59)
				*p++ = 0x10;
			else if (j == 60)
				*p++ = 0x05;
			else
				*p++ = 0x00;
		}
		for (unsigned j = 0; j < 64; ++j) {
			if (j == 36)
				*p++ = 0xfb;
			else if (j == 63)
				*p++ = (i & 1) ? 0xff : 0x7c;
			else
				*p++ = 0xff;
		}
	}

	uint32_t rng = color_ram_seed ? color_ram_seed : 1u;
	for (unsigned i = 0; i < COLOR_RAM_SIZE; ++i)
		Color[i] = color_ram_nibble(rng);
}

void C64::RequestQuit(int exit_code) {
	(void)exit_code;
	quit_requested = true;
}

unsigned C64::EmulateCycleBeforeCpu() {
	const unsigned flags = TheVIC->EmulateCycle();
	if (flags & VIC_HBLANK)
		TheSID->EmulateLine();
	TheCIA1->EmulateCycle();
	TheCIA2->EmulateCycle();
	return flags;
}

bool C64::EmulateCycleAfterCpu(unsigned vic_flags) {
	++cycle_counter;
	if (vic_flags & VIC_VBLANK) {
		++frame_counter;
		return true;
	}
	return false;
}

bool C64::EmulateCycle() {
	const unsigned flags = EmulateCycleBeforeCpu();
	TheCPU->EmulateCycle();
	return EmulateCycleAfterCpu(flags);
}

void C64::ShowNotification(std::string s) {
	if (TheDisplay)
		TheDisplay->ShowNotification(std::move(s));
}

namespace {

const char *const c64_key_names[NUM_C64_KEYCODES] = {
	"INS/DEL", "RETURN", "CRSR ←→", "F7", "F1", "F3", "F5", "CRSR ↑↓",
	"3", "W", "A", "4", "Z", "S", "E", "SHIFT (Left)",
	"5", "R", "D", "6", "C", "F", "T", "X",
	"7", "Y", "G", "8", "B", "H", "U", "V",
	"9", "I", "J", "0", "M", "K", "O", "N",
	"+", "P", "L", "-", ".", ":", "@", ",",
	"£", "*", ";", "CLR/HOME", "SHIFT (Right)", "=", "↑", "/",
	"1", "←", "CONTROL", "2", "SPACE", "C=", "Q", "RUN/STOP",
	"PLAY",
};

} // namespace

int KeycodeFromString(const std::string &s) {
	for (int i = 0; i < NUM_C64_KEYCODES; ++i)
		if (s == c64_key_names[i])
			return i;
	return -1;
}

const char * StringForKeycode(unsigned kc) {
	if (kc < NUM_C64_KEYCODES)
		return c64_key_names[kc];
	return "";
}
