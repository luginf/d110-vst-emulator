// One-shot helper: boots the real MAME-backed D110Core against a given nvram directory,
// drives a genuine factory reset (the real hardware procedure - hold Write/Copy across a
// reset, confirm with Enter, see D110Core::factoryReset()), then stops so MAME writes the
// resulting rams/memcs files to disk. Exists so native_note_probe.cpp (which has no factory
// reset of its own yet - that's Phase 6) can load a properly-initialized battery RAM via the
// NVRAM cross-compatibility already proven in Phase 3, instead of testing against the "virgin,
// never reset" RAM every other probe so far has used incidentally.
#include "Source/D110Core.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

const char *kRomPath =
	"C:\\Users\\bd260\\Downloads\\MAME 0.288 ROMs (non-merged);"
	"C:\\Users\\bd260\\Downloads\\MAME_0.288_ROMs_[merged]";

int main(int argc, char **argv) {
	const std::string nvram = (argc > 1) ? argv[1] : "d110_factory_nvram";

	D110Core core;
	std::printf("starting (nvram dir: %s)\n", nvram.c_str());
	core.start(kRomPath, nvram);
	for (int i = 0; i < 12 && !core.isRunning(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	std::printf("running: %s\n", core.isRunning() ? "yes" : "NO");
	std::this_thread::sleep_for(std::chrono::seconds(3));

	std::printf("factory reset...\n");
	core.factoryReset();
	std::this_thread::sleep_for(std::chrono::seconds(3));
	while (core.isResetting()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
	std::this_thread::sleep_for(std::chrono::seconds(2));
	std::printf("reset done\n");

	core.stop();
	std::printf("stopped, nvram written\n");
	return 0;
}
