/* Standalone C64 hardware aggregate.  Desktop media and preference APIs are
 * intentionally absent: Board owns startup, timing, input, and playback. */
#ifndef C64_H
#define C64_H

#include <cstdint>
#include <string>

constexpr unsigned C64_RAM_SIZE = 0x10000;
constexpr unsigned COLOR_RAM_SIZE = 0x400;
constexpr unsigned BASIC_ROM_SIZE = 0x2000;
constexpr unsigned KERNAL_ROM_SIZE = 0x2000;
constexpr unsigned CHAR_ROM_SIZE = 0x1000;

#ifdef NTSC
constexpr unsigned SCREEN_FREQ = 60;
constexpr unsigned CYCLES_PER_LINE = 65;
#else
constexpr unsigned SCREEN_FREQ = 50;
constexpr unsigned CYCLES_PER_LINE = 63;
#endif

extern bool IsFrodoSC;
void SetFrodoColorRamSeed(unsigned seed);

enum class PlayMode {
	Play,
	Rewind,
	Forward,
	RequestPause,
	Pause,
	RewindFrame,
	ForwardFrame,
};

class Display;
class MOS6510;
class MOS6569;
class MOS6581;
class MOS6526_1;
class MOS6526_2;

class C64 {
public:
	C64();
	~C64();

	void RequestQuit(int exit_code = 0);

	uint32_t CycleCounter() const { return cycle_counter; }
	uint32_t FrameCounter() const { return frame_counter; }
	void ResetCounters() { cycle_counter = 0; frame_counter = 0; }
	void SetCounters(uint32_t cycle, uint32_t frame) {
		cycle_counter = cycle;
		frame_counter = frame;
	}
	bool QuitRequested() const { return quit_requested; }

	bool EmulateCycle();

	void SetPlayMode(PlayMode mode) { play_mode = mode; }
	PlayMode GetPlayMode() const { return play_mode; }

	// Board applies live input directly.  Keep these event hooks harmless for
	// common Display builds that still mention them.
	void JoystickAdded(int32_t) {}
	void JoystickRemoved(int32_t) {}

	void ShowNotification(std::string s);

	uint8_t * RAM;
	uint8_t * Basic;
	uint8_t * Kernal;
	uint8_t * Char;
	uint8_t * Color;

	Display * TheDisplay;
	MOS6510 * TheCPU;
	MOS6569 * TheVIC;
	MOS6581 * TheSID;
	MOS6526_1 * TheCIA1;
	MOS6526_2 * TheCIA2;

private:
	void init_memory();
	unsigned EmulateCycleBeforeCpu();
	bool EmulateCycleAfterCpu(unsigned vic_flags);

	bool quit_requested = false;
	uint32_t cycle_counter = 0;
	uint32_t frame_counter = 0;
	PlayMode play_mode = PlayMode::Play;
};

int KeycodeFromString(const std::string & s);
const char * StringForKeycode(unsigned kc);

// Keyboard name conversion remains part of the shared input/preferences
// format even though the desktop C64 aggregate is gone.
enum {
	NUM_C64_KEYCODES = 65,
};


#endif // C64_H
