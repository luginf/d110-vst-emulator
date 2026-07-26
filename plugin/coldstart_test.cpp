// Two things about start-up that used to be wrong, checked the way a user would hit them.
//
// 1. Loading the plugin must NOT re-initialise the instrument. The cold start happens once
//    ever; every instance afterwards is seeded from the kept image and comes up working.
// 2. The factory initialisation must be reachable from the panel by the hardware's own
//    procedure - latch WRITE/COPY down, switch on, confirm with ENTER - because the
//    plugin no longer offers a Factory Reset command of its own.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Write/Copy is port 0 bit 0, Enter is port 1 bit 0 - the same two the documented cold
// start uses. See docs/panel_reference_notes.md.
constexpr int kWriteCopy = 0, kWriteCopyBit = 0;
constexpr int kEnter = 1, kEnterBit = 0;

int fineTune(D110AudioProcessor &proc) {
	std::vector<juce::uint8> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return -1;
	return ram[0x2003];
}

void tap(D110AudioProcessor &proc, int port, int bit) {
	const int idx = D110Core::buttonIndex(port, bit);
	proc.getCore().setButton(idx, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	proc.getCore().setButton(idx, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(350));
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("firmware memory root: %s\n\n",
	            D110AudioProcessor::getNvramRoot().getFullPathName().toRawUTF8());

	// ---- 1. a newly loaded instance must come up ready, not initialising ----
	{
		D110AudioProcessor proc;
		proc.prepareToPlay(kSampleRate, kBlock);

		const auto started = std::chrono::steady_clock::now();
		proc.setPoweredOn(true);
		std::this_thread::sleep_for(std::chrono::seconds(7));
		const bool resetting = proc.getCore().isResetting();
		const double elapsed =
			std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

		std::vector<juce::uint8> ram(D110Core::kRamSize, 0);
		size_t nonZero = 0;
		if (proc.getCore().getRam(ram.data()))
			for (juce::uint8 b : ram)
				if (b) ++nonZero;

		std::printf("a freshly loaded instance after %.0fs:\n", elapsed);
		std::printf("  still initialising : %s\n", resetting ? "YES - it should not be" : "no");
		std::printf("  memory populated   : %d bytes non-zero\n", int(nonZero));
		std::printf("  %s\n\n", (!resetting && nonZero > D110Core::kRamSize / 8)
		                            ? "*** COMES UP READY, NO INITIALISATION ***"
		                            : "NOT READY - it is still initialising on load");
		proc.setPoweredOn(false);
		proc.releaseResources();
	}

	// ---- 2. the hardware's own factory init, driven from the panel ---------
	{
		D110AudioProcessor proc;
		proc.prepareToPlay(kSampleRate, kBlock);
		proc.setPoweredOn(true);
		std::this_thread::sleep_for(std::chrono::seconds(7));

		// Move a parameter well away from its factory value, so a real initialisation is
		// unmistakable when it puts it back.
		auto press = [&proc](int port, int bit, int times) {
			for (int i = 0; i < times; ++i) tap(proc, port, bit);
		};
		press(0, 7, 2); // Exit, Exit
		press(0, 5, 1); // Timbre
		press(1, 7, 1); // Edit
		press(0, 3, 2); // Group + x2 -> Fine Tune
		press(0, 1, 21); // Number + x21
		std::this_thread::sleep_for(std::chrono::seconds(1));
		std::printf("Fine Tune after editing it : %d\n", fineTune(proc));

		// Now the documented procedure, exactly as the panel offers it: POWER off, latch
		// WRITE/COPY down, POWER on, confirm with ENTER.
		std::printf("POWER off, latch WRITE/COPY, POWER on, ENTER...\n");
		proc.setPoweredOn(false);
		std::this_thread::sleep_for(std::chrono::seconds(1));

		proc.getCore().setButton(D110Core::buttonIndex(kWriteCopy, kWriteCopyBit), true);
		proc.setPoweredOn(true);
		std::this_thread::sleep_for(std::chrono::seconds(6));
		proc.getCore().setButton(D110Core::buttonIndex(kWriteCopy, kWriteCopyBit), false);
		std::this_thread::sleep_for(std::chrono::milliseconds(800));

		tap(proc, kEnter, kEnterBit);
		std::this_thread::sleep_for(std::chrono::seconds(5));

		const int after = fineTune(proc);
		std::printf("Fine Tune after the panel init : %d\n", after);
		std::printf("%s\n", after == 50
		                        ? "*** THE PANEL'S OWN FACTORY INIT WORKS ***"
		                        : "did NOT return to the factory value of 50");

		proc.setPoweredOn(false);
		proc.releaseResources();
	}

	std::printf("\ndone\n");
	return 0;
}
