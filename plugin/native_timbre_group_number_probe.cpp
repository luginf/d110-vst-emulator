// Alan's report (2026-08-06): picking a tone from the Parts tab's right-click list updates
// the sound and the display, but a subsequent increment/decrement on the TONE cell continues
// from the REPLACED tone's own number, not the one just picked - as if the write never really
// landed. Checks the raw firmware RAM directly (bypassing the editor's own optimistic cache
// entirely) after sending the exact two-message group-then-number sequence
// D110EditorPane::showToneListMenu sends, to see whether the firmware genuinely accepts it.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> audio(2, kBlock);
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer none;
		proc.processBlock(audio, none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}
} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("did not start\n");
		return 1;
	}

	constexpr int part = 0;
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);

	// Set part 0 to a known starting tone: group 3 (r/CARD), number 53 (0-indexed) - stands
	// in for Alan's "xylophone at B54" (an arbitrary but definite starting point).
	proc.sendTimbreTempParam(part, 0, 3);
	proc.sendTimbreTempParam(part, 1, 53);
	render(proc, 1.0);
	proc.getCore().getRam(ram.data());
	std::printf("after setting starting tone: group=%d number=%d\n",
	            ram[D110CoreType::kRamTimbreTemp + part * D110CoreType::kTimbreTempRecord + 0],
	            ram[D110CoreType::kRamTimbreTemp + part * D110CoreType::kTimbreTempRecord + 1]);

	// Now the exact sequence showToneListMenu sends for picking "1 fantasy" from preset B:
	// group=1, number=0, sent as two separate messages back to back.
	proc.sendTimbreTempParam(part, 0, 1);
	proc.sendTimbreTempParam(part, 1, 0);

	// Check at several points in time - immediately, and after well past any plausible
	// mirror/round-trip delay - to see whether it sticks or reverts.
	for (double waited = 0.0; waited < 2.0; waited += 0.2) {
		render(proc, 0.2);
		proc.getCore().getRam(ram.data());
		const int g = ram[D110CoreType::kRamTimbreTemp + part * D110CoreType::kTimbreTempRecord + 0];
		const int n = ram[D110CoreType::kRamTimbreTemp + part * D110CoreType::kTimbreTempRecord + 1];
		std::printf("t=%.1fs: group=%d number=%d %s\n", waited + 0.2, g, n,
		            (g == 1 && n == 0) ? "(matches Fantasy - OK)" : "(MISMATCH)");
	}

	// Checking a second, unrelated hypothesis while this probe is already booted and factory-
	// reset: does each 128-byte Patch Memory record hold a per-part MIDI channel block at
	// offset 22-30 (as a terse existing code comment in layoutPatches() claims: "name 10,
	// reverb 3, reserve 9, channels 9" before the part sub-records start at 31), or is that
	// actually reserved/unused padding (as docs/factory_defaults.md's "the per-part MIDI
	// channel is not here [Patch Edit] - it lives under PART SET" line suggests)? If patch 0's
	// own bytes 22-30 read the same "1 2 3 4 5 6 7 8 9" factory_defaults.md already confirmed
	// for the LIVE System-area channel map (0x2DA1), that's strong corroboration the offset
	// guess is right.
	proc.getCore().getRam(ram.data());
	std::printf("patch 0, bytes 22-30 (hypothesised per-part channel block): ");
	for (int i = 22; i <= 30; ++i)
		std::printf("%d ", ram[D110CoreType::kRamPatches + i]);
	std::printf("\n");
	std::printf("patch 0, bytes 13-21 (hypothesised reserve block, expect 4 4 4 4 3 3 3 2 5): ");
	for (int i = 13; i <= 21; ++i)
		std::printf("%d ", ram[D110CoreType::kRamPatches + i]);
	std::printf("\n");

	return 0;
}
