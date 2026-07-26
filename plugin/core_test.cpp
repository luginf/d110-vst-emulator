// Console sanity check for D110Core: boots the real D-110 firmware in-process, prints
// the MSM6222B's dot matrix as ASCII art, then presses front-panel buttons and prints
// it again. If this shows the firmware's own menus changing, the wrap works and the
// plugin can be wired to it.
#include "Source/D110Core.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

const char *kRomPath =
	"C:\\Users\\bd260\\Downloads\\MAME 0.288 ROMs (non-merged);"
	"C:\\Users\\bd260\\Downloads\\MAME_0.288_ROMs_[merged]";

// Button indices follow INPUT_PORTS_START(d110): port 0 = SC0 (top row),
// port 1 = SC1 (bottom row), bit = the mask each button carries.
struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit",       0, 7}, {"Patch",    0, 6}, {"Timbre",   0, 5}, {"Part +",   0, 4},
	{"Group +",    0, 3}, {"Bank +",   0, 2}, {"Number +", 0, 1}, {"Write/Copy", 0, 0},
	{"Edit",       1, 7}, {"Part",     1, 6}, {"System",   1, 5}, {"Part -",   1, 4},
	{"Group -",    1, 3}, {"Bank -",   1, 2}, {"Number -", 1, 1}, {"Enter",    1, 0},
};

int indexOf(const char *name) {
	for (const auto &b : kButtons)
		if (std::strcmp(b.name, name) == 0) return D110Core::buttonIndex(b.port, b.bit);
	return -1;
}

// The dot matrix, as art. Each character is 5 dots wide; bit 4 is the leftmost.
void printLcd(D110Core &core, const char *label) {
	uint8_t buf[D110Core::kLcdBytes];
	std::printf("\n--- %s ---\n", label);
	if (!core.getLcd(buf)) { std::printf("   (no frame yet)\n"); return; }
	for (int line = 0; line < D110Core::kLines; ++line) {
		for (int row = 0; row < 7; ++row) {
			std::printf("   ");
			for (int col = 0; col < D110Core::kCols; ++col) {
				const uint8_t bits = buf[((size_t)line * D110Core::kCols + col) * D110Core::kRowsPerChar + row];
				for (int d = 4; d >= 0; --d) std::putchar((bits >> d) & 1 ? '#' : '.');
				std::putchar(' ');
			}
			std::putchar('\n');
		}
		std::putchar('\n');
	}
}

void press(D110Core &core, const char *name, int holdMs = 120) {
	const int idx = indexOf(name);
	if (idx < 0) { std::printf("!! unknown button %s\n", name); return; }
	core.setButton(idx, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
	core.setButton(idx, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(400));
}

} // namespace

int main(int argc, char **argv) {
	const std::string nvram = (argc > 1) ? argv[1] : "d110_nvram";

	D110Core core;
	std::printf("starting D-110 core (nvram dir: %s)\n", nvram.c_str());
	core.start(kRomPath, nvram);

	// Real hardware takes its time; the firmware needs several seconds before the
	// patch screen settles.
	for (int i = 0; i < 12 && !core.isRunning(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	std::printf("running: %s\n", core.isRunning() ? "yes" : "NO");
	std::this_thread::sleep_for(std::chrono::seconds(8));

	printLcd(core, "boot");

	press(core, "Part");    printLcd(core, "after Part");
	press(core, "Exit");
	press(core, "Timbre");  printLcd(core, "after Timbre");
	press(core, "Edit");    printLcd(core, "after Edit");
	press(core, "Exit");
	press(core, "System");  printLcd(core, "after System");

	uint8_t ram[D110Core::kRamSize];
	std::printf("\nram snapshot: %s, generation %llu\n",
	            core.getRam(ram) ? "ok" : "none",
	            (unsigned long long)core.ramGeneration());

	std::printf("\nstopping...\n");
	core.stop();
	std::printf("done\n");
	return 0;
}
