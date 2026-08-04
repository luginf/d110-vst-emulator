// MAME-backed counterpart to plugin/native_ramp_edge_stress_probe.cpp: same dense,
// genuinely overlapping/legato note pattern that reliably froze the native core under
// La32Ramps before its serviceStuckPolicy() fix (a landed ramp only ever answers a voice's
// OWN envelope completion, never the separate per-note DISPATCH wait every note-on parks at -
// see D110Core.h's "the missing external interrupt"/"releasing the stuck wait" comments), now
// ported to D110Core.cpp's midiTick() too. Drives the REAL D110AudioProcessor through
// processBlock() with real wall-clock pacing (D110Core runs on its own thread, unlike the
// native core - see probe-must-wait-on-the-clock discipline elsewhere in this project), and
// checks the same two things: does firmwareNoteOns()/Offs() keep advancing, and does the RAM
// snapshot keep changing, rather than trusting any single PC sample.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

void render(D110AudioProcessor &proc, double seconds, juce::MidiBuffer *midi = nullptr) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const int blocks = std::max(1, int(seconds * kSampleRate / kBlock));
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, (b == 0 && midi) ? *midi : none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
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
		std::printf("firmware did not start: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}
	proc.setForwardNotesToFirmware(true);
	std::printf("policy: %s\n", "template default (whatever setPoweredOn() configures)");

	constexpr int kRounds = 120;
	std::vector<uint8_t> prevRam = snapshot(proc);
	int staleStreak = 0, maxStaleStreak = 0;
	uint64_t lastOnCount = 0, lastOffCount = 0;
	int stalledRounds = 0;

	for (int round = 0; round < kRounds; ++round) {
		const int base = 36 + (round * 5) % 40;
		const int voices = 2 + (round % 3); // 2..4 overlapping voices per round

		// This round's ON, overlapping the PREVIOUS round's still-sounding notes.
		juce::MidiBuffer on;
		for (int v = 0; v < voices; ++v)
			on.addEvent(juce::MidiMessage::noteOn(2 + (v % 2), base + v * 3, 0.85f), 0);
		render(proc, 0.05, &on);

		if (round > 0) {
			const int prevBase = 36 + ((round - 1) * 5) % 40;
			const int prevVoices = 2 + ((round - 1) % 3);
			juce::MidiBuffer off;
			for (int v = 0; v < prevVoices; ++v)
				off.addEvent(juce::MidiMessage::noteOff(2 + (v % 2), prevBase + v * 3), 0);
			render(proc, 0.05, &off);
		}

		const auto ram = snapshot(proc);
		const bool same = ram == prevRam;
		if (same) { ++staleStreak; maxStaleStreak = std::max(maxStaleStreak, staleStreak); }
		else staleStreak = 0;
		prevRam = ram;

		const uint64_t on64 = proc.getCore().firmwareNoteOns(), off64 = proc.getCore().firmwareNoteOffs();
		if (on64 == lastOnCount && off64 == lastOffCount) ++stalledRounds;
		lastOnCount = on64;
		lastOffCount = off64;

		if (round % 20 == 0)
			std::printf("round %3d: onCount=%llu offCount=%llu staleStreak=%d\n", round,
			            (unsigned long long)on64, (unsigned long long)off64, staleStreak);
	}

	render(proc, 3.0); // let everything ring out and settle

	std::printf("\nmax RAM-static streak: %d consecutive rounds (~100ms each)\n", maxStaleStreak);
	std::printf("rounds with zero note-on/off progress: %d/%d\n", stalledRounds, kRounds);
	std::printf("final onCount=%llu offCount=%llu\n",
	            (unsigned long long)proc.getCore().firmwareNoteOns(),
	            (unsigned long long)proc.getCore().firmwareNoteOffs());

	const bool suspect = maxStaleStreak > 15 || stalledRounds > kRounds / 2;
	std::printf("\n%s\n", suspect ? "SUSPECT FREEZE" : "no freeze signature over this run");

	proc.setPoweredOn(false);
	proc.releaseResources();
	return suspect ? 1 : 0;
}
