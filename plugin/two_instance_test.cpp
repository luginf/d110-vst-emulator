// What actually happens when two D110Cores run in one process - which is what a DAW does
// the moment a second instance of the plugin is loaded.
//
// The suspicion is specific and structural, not vague: MAME reaches its machine through
// mame_machine_manager::instance(), which caches a process-wide `static
// mame_machine_manager *s_manager` and hands the SAME one to every later caller. A second
// cli_frontend::execute would therefore attach to the first instance's OSD and options,
// and the first machine's destructor nulls the pointer out from under the second.
//
// This harness does not try to fix that. It records what really happens, so the guard the
// plugin ships is based on measurement rather than on reading the source and guessing.
#include "Source/D110Core.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

const char *kRomPath =
	"C:\\Users\\bd260\\Downloads\\MAME 0.288 ROMs (non-merged);"
	"C:\\Users\\bd260\\Downloads\\MAME_0.288_ROMs_[merged]";

void report(const char *label, D110Core &core) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	std::vector<uint8_t> lcd(D110Core::kLcdBytes, 0);
	const bool haveRam = core.getRam(ram.data());
	const bool haveLcd = core.getLcd(lcd.data());

	int nonZero = 0;
	if (haveRam)
		for (uint8_t b : ram)
			if (b) ++nonZero;

	int litDots = 0;
	if (haveLcd)
		for (uint8_t b : lcd)
			if (b) ++litDots;

	std::printf("  %-10s running=%-3s  ram=%-3s (%5d non-zero)  lcd=%-3s (%4d dots)  gen=%llu\n",
	            label, core.isRunning() ? "yes" : "NO", haveRam ? "yes" : "no", nonZero,
	            haveLcd ? "yes" : "no", litDots,
	            (unsigned long long)core.ramGeneration());
}

} // namespace

int main() {
	// Unbuffered: this harness is expected to be able to crash, and a buffered stdout
	// would throw away the very lines that say how far it got.
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("=== one instance ===\n");
	{
		D110Core a;
		a.start(kRomPath, "d110_nvram_a");
		std::this_thread::sleep_for(std::chrono::seconds(8));
		report("single", a);
		a.stop();
	}

	std::printf("\n=== two instances at once ===\n");
	{
		D110Core a, b;
		std::printf("starting A...\n");
		a.start(kRomPath, "d110_nvram_a");
		std::this_thread::sleep_for(std::chrono::seconds(6));
		report("A alone", a);

		std::printf("starting B while A is still running...\n");
		b.start(kRomPath, "d110_nvram_b");
		std::this_thread::sleep_for(std::chrono::seconds(8));
		report("A", a);
		report("B", b);

		// If both survive this far, do they stay independent? Drive only A's panel and
		// see whether B's display moves with it - shared global state would show up here.
		std::printf("pressing Timbre on A only...\n");
		a.setButton(D110Core::buttonIndex(0, 5), true);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		a.setButton(D110Core::buttonIndex(0, 5), false);
		std::this_thread::sleep_for(std::chrono::seconds(2));
		report("A", a);
		report("B", b);

		std::printf("stopping B...\n");
		b.stop();
		std::this_thread::sleep_for(std::chrono::seconds(2));
		report("A after B stopped", a);

		std::printf("stopping A...\n");
		a.stop();
	}

	std::printf("\ndone - reaching this line at all is itself part of the result\n");
	return 0;
}
