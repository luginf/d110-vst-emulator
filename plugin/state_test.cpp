// Does a project actually recall its own instrument?
//
// Three things are checked, all of which used to be broken:
//   1. The firmware's patch and timbre memory travels in the plugin's state, so a saved
//      project comes back with the sounds it was saved with rather than whatever the
//      shared file happens to hold now.
//   2. A plugin loaded with NO saved state finds the memory exactly as the last instance
//      left it. That is why the memory is ONE shared file: a folder per instance reset the
//      instrument every time the host was reopened, which is how the fault was reported.
//   3. The second instance refuses to switch on rather than starting a second MAME machine -
//      which, measured in two_instance_test.cpp, kills the host process outright.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

void press(D110AudioProcessor &proc, int port, int bit, int times) {
	for (int i = 0; i < times; ++i) {
		const int idx = D110Core::buttonIndex(port, bit);
		proc.getCore().setButton(idx, true);
		std::this_thread::sleep_for(std::chrono::milliseconds(130));
		proc.getCore().setButton(idx, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(320));
	}
}

// The firmware's Fine Tune byte for part 1 - a single byte that is easy to set from the
// panel and unmistakable when it comes back. See docs/sysex_address_map.md.
int readFineTune(D110AudioProcessor &proc) {
	std::vector<juce::uint8> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return -1;
	return ram[0x2003];
}

void bootAndSettle(D110AudioProcessor &proc) {
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(7));
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	juce::MemoryBlock savedState;
	int editedValue = -1;

	// ---- 1. edit something, then save the project ---------------------------
	{
		D110AudioProcessor proc;
		proc.prepareToPlay(kSampleRate, kBlock);
		std::printf("instance A nvram: %s\n",
		            proc.getNvramFolder().getFileName().toRawUTF8());
		bootAndSettle(proc);

		proc.getCore().factoryReset();
		while (proc.getCore().isResetting())
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		std::this_thread::sleep_for(std::chrono::seconds(2));
		std::printf("Fine Tune after factory reset : %d\n", readFineTune(proc));

		// Walk to Timbre Edit / Fine Tune and move it somewhere distinctive.
		press(proc, 0, 7, 2); // Exit, Exit
		press(proc, 0, 5, 1); // Timbre
		press(proc, 1, 7, 1); // Edit
		press(proc, 0, 3, 2); // Group + x2 -> Fine Tune
		press(proc, 0, 1, 17); // Number + x17
		std::this_thread::sleep_for(std::chrono::seconds(1));

		editedValue = readFineTune(proc);
		std::printf("Fine Tune after the edit      : %d\n", editedValue);

		proc.getStateInformation(savedState);
		std::printf("saved project state           : %d bytes\n", int(savedState.getSize()));
		proc.setPoweredOn(false);
		proc.releaseResources();
	}

	// ---- 2. a fresh instance restoring that project -------------------------
	{
		D110AudioProcessor proc;
		proc.prepareToPlay(kSampleRate, kBlock);
		proc.setStateInformation(savedState.getData(), int(savedState.getSize()));
		std::printf("\ninstance B nvram: %s (should match A)\n",
		            proc.getNvramFolder().getFileName().toRawUTF8());

		bootAndSettle(proc);
		const int restored = readFineTune(proc);
		std::printf("Fine Tune after reload        : %d\n", restored);
		std::printf("%s\n", restored == editedValue
		                        ? "*** THE PROJECT RECALLED ITS OWN FIRMWARE MEMORY ***"
		                        : "NOT RESTORED - the saved memory did not come back");
		proc.setPoweredOn(false);
		proc.releaseResources();
	}

	// ---- 3. the host was closed and reopened --------------------------------
	// The reported fault: quit Ableton, load the plugin again, everything back to
	// defaults. A plugin loaded fresh gets NO saved state, so it must find the memory
	// exactly as the last instance left it - which means one shared file, not a folder
	// per instance.
	{
		int edited = -1;
		{
			D110AudioProcessor first;
			first.prepareToPlay(kSampleRate, kBlock);
			bootAndSettle(first);
			press(first, 0, 7, 2); // Exit, Exit
			press(first, 0, 5, 1); // Timbre
			press(first, 1, 7, 1); // Edit
			press(first, 0, 3, 2); // Group + x2 -> Fine Tune
			press(first, 0, 1, 9); // Number + x9
			std::this_thread::sleep_for(std::chrono::seconds(1));
			edited = readFineTune(first);
			std::printf("\nbefore closing the host : Fine Tune %d\n", edited);
			first.setPoweredOn(false); // this is what flushes MAME's NVRAM to disk
			first.releaseResources();
		}

		// A completely new processor, with no state restored into it at all.
		{
			D110AudioProcessor second;
			second.prepareToPlay(kSampleRate, kBlock);
			std::printf("nvram folder            : %s\n",
			            second.getNvramFolder().getFullPathName().toRawUTF8());
			bootAndSettle(second);
			const int recalled = readFineTune(second);
			std::printf("after reopening it      : Fine Tune %d\n", recalled);
			std::printf("%s\n", recalled == edited
			                        ? "*** THE MEMORY SURVIVED THE HOST CLOSING ***"
			                        : "LOST - a freshly loaded plugin is not finding it");
			second.setPoweredOn(false);
			second.releaseResources();
		}
	}

	// ---- 4. two instances at once -------------------------------------------
	{
		D110AudioProcessor first, second;
		first.prepareToPlay(kSampleRate, kBlock);
		second.prepareToPlay(kSampleRate, kBlock);

		// Shared on purpose now: only one may be switched on at a time, so there is
		// nothing to collide, and one memory is what makes the state survive at all.
		std::printf("\nfirmware memory is shared: %s\n",
		            first.getNvramFolder() == second.getNvramFolder() ? "yes, as intended"
		                                                             : "NO - unexpected");

		first.setPoweredOn(true);
		std::this_thread::sleep_for(std::chrono::seconds(6));
		std::printf("\nfirst powered on : %s\n", first.isPoweredOn() ? "yes" : "NO");

		second.setPoweredOn(true);
		std::this_thread::sleep_for(std::chrono::seconds(2));
		std::printf("second powered on: %s  blocked: %s\n",
		            second.isPoweredOn() ? "YES - DANGEROUS" : "no",
		            second.isPowerBlocked() ? "yes" : "no");
		if (second.isPowerBlocked())
			std::printf("  message: %s\n", second.getLastError().toRawUTF8());
		std::printf("%s\n", (!second.isPoweredOn() && second.isPowerBlocked())
		                        ? "*** THE SECOND INSTANCE REFUSED, AS IT MUST ***"
		                        : "GUARD FAILED - a second machine would crash the host");

		// And once the first is off, the second may have it.
		first.setPoweredOn(false);
		second.setPoweredOn(true);
		std::this_thread::sleep_for(std::chrono::seconds(6));
		std::printf("after the first switched off, second powers on: %s\n",
		            second.isPoweredOn() ? "yes - the slot is handed over correctly"
		                                 : "NO - the slot was never released");
		second.setPoweredOn(false);
		first.releaseResources();
		second.releaseResources();
	}

	std::printf("\ndone\n");
	return 0;
}
