// Native-core counterpart to core_test.cpp: same LCD ASCII-art dump, same button walk (Part,
// Exit, Timbre, Edit, Exit, System), but against D110CoreNative instead of the MAME-backed
// D110Core, and links NONE of MAME_LIBS. Run both and diff the printed screens by eye - if the
// firmware's own menus change the same way on both, the native CPU/bus/LCD port is faithful
// enough to read the front panel through, not just to boot silently.
//
// No real-time sleeps needed: D110CoreNative::runForSeconds() advances deterministic emulated
// time directly, so a "120ms hold" is exactly 120ms of CPU time, not a race against a
// background thread's own wall clock.
#include "Source/native/D110CoreNative.h"

#include <cstdio>
#include <cstring>

namespace {

struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit",       0, 7}, {"Patch",    0, 6}, {"Timbre",   0, 5}, {"Part +",   0, 4},
	{"Group +",    0, 3}, {"Bank +",   0, 2}, {"Number +", 0, 1}, {"Write/Copy", 0, 0},
	{"Edit",       1, 7}, {"Part",     1, 6}, {"System",   1, 5}, {"Part -",   1, 4},
	{"Group -",    1, 3}, {"Bank -",   1, 2}, {"Number -", 1, 1}, {"Enter",    1, 0},
};

int indexOf(const char *name) {
	for (const auto &b : kButtons)
		if (std::strcmp(b.name, name) == 0) return D110CoreNative::buttonIndex(b.port, b.bit);
	return -1;
}

void printLcd(D110CoreNative &core, const char *label) {
	uint8_t buf[D110CoreNative::kLcdBytes];
	std::printf("\n--- %s ---\n", label);
	if (!core.getLcd(buf)) { std::printf("   (not running)\n"); return; }
	for (int line = 0; line < D110CoreNative::kLines; ++line) {
		for (int row = 0; row < 7; ++row) {
			std::printf("   ");
			for (int col = 0; col < D110CoreNative::kCols; ++col) {
				const uint8_t bits = buf[((size_t)line * D110CoreNative::kCols + col) * D110CoreNative::kRowsPerChar + row];
				for (int d = 4; d >= 0; --d) std::putchar((bits >> d) & 1 ? '#' : '.');
				std::putchar(' ');
			}
			std::putchar('\n');
		}
		std::putchar('\n');
	}
}

void press(D110CoreNative &core, const char *name, double holdSeconds = 0.12) {
	const int idx = indexOf(name);
	if (idx < 0) { std::printf("!! unknown button %s\n", name); return; }
	core.setButton(idx, true);
	core.runForSeconds(holdSeconds);
	core.setButton(idx, false);
	core.runForSeconds(0.4);
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110CoreNative core;
	const char *romFolder = "C:/Program Files/Common Files/VST3/D-110 Data";
	std::printf("starting native D-110 core (roms: %s)\n", romFolder);
	if (!core.start(romFolder)) {
		std::printf("failed to load ROMs\n");
		return 1;
	}
	std::printf("running: %s\n", core.isRunning() ? "yes" : "NO");

	// Real hardware settles over several seconds; give the native core the emulated-time
	// equivalent before trusting the first LCD frame.
	core.runForSeconds(8.0);
	printLcd(core, "boot");

	press(core, "Part");    printLcd(core, "after Part");
	press(core, "Exit");
	press(core, "Timbre");  printLcd(core, "after Timbre");
	press(core, "Edit");    printLcd(core, "after Edit");
	press(core, "Exit");
	press(core, "System");  printLcd(core, "after System");

	std::printf("\ndone\n");
	return 0;
}
