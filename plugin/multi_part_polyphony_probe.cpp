// The user's real complaint isn't a single melodic line - it's simultaneous fast playing on
// BASS and SOLO parts together (two different parts sounding at once, both dropping notes).
// plugin/polyphony_test.cpp only ever exercises ONE part in isolation and (per this session's
// own TEMPDIAG instrumentation in munt/mt32emu/src/Part.cpp) never once triggers a refusal or
// single-assign abort at that tempo - so it can't be reproducing this. The D-110's 32 partials
// are a SYSTEM-WIDE budget shared across all 9 parts, not per-part, so two parts playing fast
// at once could genuinely exhaust it even where either part alone would not.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Part 1 -> channel 2 (bass), Part 2 -> channel 3 (solo), factory channel map.
constexpr int kBassChannel = 2;
constexpr int kSoloChannel = 3;

void renderBlocks(D110AudioProcessor &proc, int blocks, juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}
void render(D110AudioProcessor &proc, double seconds) {
	renderBlocks(proc, int(seconds * kSampleRate / kBlock));
}

void setPartTone(D110AudioProcessor &proc, int part, int group, int number) {
	proc.sendTimbreTempParam(part, 0, uint8_t(group));
	proc.sendTimbreTempParam(part, 1, uint8_t(number));
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not start\n");
		return 1;
	}
	proc.setForwardNotesToFirmware(true);

	// Bass on part 1: a02 Acou Piano 2 (2 partials) stands in for a plausible bass patch.
	// Solo on part 2: b01 Fantasy (4 partials) - the exact tone the user named.
	setPartTone(proc, 0, 0, 1);
	setPartTone(proc, 1, 1, 0);
	// PatchParam byte 5 = assignMode (Structures.h). Explicitly POLY3 (index 2) on both -
	// the user's own panel reading for the solo/Fantasy part - since the factory patch temp
	// otherwise defaults to POLY1 (single-assign, same-key retrigger), which is a DIFFERENT
	// mechanism from what the user described and not what's actually on their panel.
	proc.sendTimbreTempParam(0, 5, 2);
	proc.sendTimbreTempParam(1, 5, 2);
	render(proc, 1.5);
	std::printf("partials at engine: %u\n", proc.enginePartialCount());

	// Both parts play fast, overlapping 32nd-note-ish lines SIMULTANEOUSLY for several
	// seconds, exactly the "bass and solo both dropping notes at once" shape - not one part
	// tested in isolation. ~80ms per note (roughly 32nds at ~190bpm), next note-on fires
	// before the previous one's release has necessarily finished.
	constexpr int kSteps = 60;
	int peakPartials = 0;
	uint64_t onsBefore = proc.getCore().firmwareNoteOns();

	for (int i = 0; i < kSteps; ++i) {
		juce::MidiBuffer on;
		const int bassNote = 36 + (i % 2) * 2;       // do-re alternation, low octave
		const int soloNote = 72 + (i % 2) * 2;       // do-re alternation, high octave
		on.addEvent(juce::MidiMessage::noteOn(kBassChannel, bassNote, 0.9f), 0);
		on.addEvent(juce::MidiMessage::noteOn(kSoloChannel, soloNote, 0.9f), 1);
		renderBlocks(proc, 3, &on); // ~35ms
		peakPartials = std::max(peakPartials, proc.engineActivePartials());

		juce::MidiBuffer off;
		const int prevBassNote = 36 + ((i + 1) % 2) * 2;
		const int prevSoloNote = 72 + ((i + 1) % 2) * 2;
		if (i > 0) {
			off.addEvent(juce::MidiMessage::noteOff(kBassChannel, prevBassNote), 0);
			off.addEvent(juce::MidiMessage::noteOff(kSoloChannel, prevSoloNote), 1);
		}
		renderBlocks(proc, 3, &off); // ~35ms - roughly 70ms/note pair overall, ~14 notes/sec/part

		if (i % 10 == 0)
			std::printf("step %2d: engine partials=%2d firmwareNoteOns=%llu\n", i,
			            proc.engineActivePartials(), (unsigned long long)proc.getCore().firmwareNoteOns());
	}
	{
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kBassChannel, 36 + ((kSteps + 1) % 2) * 2), 0);
		off.addEvent(juce::MidiMessage::noteOff(kSoloChannel, 72 + ((kSteps + 1) % 2) * 2), 1);
		renderBlocks(proc, 3, &off);
	}
	render(proc, 2.0);

	const uint64_t sentTotal = kSteps * 2;
	const uint64_t takenTotal = proc.getCore().firmwareNoteOns() - onsBefore;
	std::printf("\nsent=%llu firmwareTook=%llu%s peakPartials=%d (of %u)\n",
	            (unsigned long long)sentTotal, (unsigned long long)takenTotal,
	            takenTotal < sentTotal ? "  <-- FIRMWARE LOST NOTES" : "", peakPartials,
	            proc.enginePartialCount());

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
