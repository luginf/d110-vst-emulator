// Alan's decisive new clue (2026-08-07): the cutout only happens after interacting with the
// plugin's own Editor drawer (e.g. picking a tone/patch there) while playing - never if the
// editor is left alone, and it clears up if the sound is instead reselected from the
// photographed panel's own "native" buttons. That points squarely at a race already
// documented in PluginProcessor.cpp's processBlock() (the comment beside playSysexNow/
// playMsgOnPart): a note can be applied to the engine before a parameter it depends on has
// caught up, and the engine then silently drops it ("Attempted to play unmapped key"). That
// specific case (the built-in demo song's rhythm map) was fixed by ordering parameters before
// notes within the same block - this probe checks whether the EDITOR's own tone-selection
// write (sendTimbreTempParam, the same call the Parts tab's tone-picker makes - see the
// project's "self-corrected mistake" note: it writes TimbreTemp directly, unlike selectPatch()
// which replays real PATCH/BANK/NUMBER button presses) has the same race when it lands WHILE
// notes are actively playing, rather than testing it in isolation the way earlier probes did.
#include "Source/PluginProcessor.h"

#include <cstdio>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 2; // Part 1, factory channel map

float renderPeak(D110AudioProcessor &proc, double seconds, const juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	const int blocks = juce::jmax(1, int(seconds * kSampleRate / kBlock));
	float peak = 0.0f;
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		peak = juce::jmax(peak, audio.getMagnitude(0, kBlock));
	}
	return peak;
}

// Plays one note, reports whether it was audible.
bool playAndCheck(D110AudioProcessor &proc, int note, double hold, double gap) {
	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(kChannel, note, (juce::uint8)100), 0);
	const float peak = renderPeak(proc, hold, &on);
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(kChannel, note), 0);
	renderPeak(proc, gap, &off);
	return peak >= 0.001f;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	renderPeak(proc, 9.0);
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not start\n");
		return 1;
	}
	proc.setForwardNotesToFirmware(true);

	// -- baseline: 30 notes, no editor interaction at all --
	{
		int failures = 0;
		for (int i = 0; i < 30; ++i)
			if (!playAndCheck(proc, 55 + (i % 5) * 3, 0.08, 0.03)) ++failures;
		std::printf("baseline, no editor interaction               : %d/30 silent\n", failures);
	}

	// -- editor tone-selection landing BETWEEN notes (a clean edit, notes paused) --
	{
		proc.sendTimbreTempParam(0, 0, 0); // part 1: group -> PRESET a
		proc.sendTimbreTempParam(0, 1, 2); // number -> a different preset tone
		renderPeak(proc, 0.3);             // let it settle before playing again
		int failures = 0;
		for (int i = 0; i < 30; ++i)
			if (!playAndCheck(proc, 55 + (i % 5) * 3, 0.08, 0.03)) ++failures;
		std::printf("editor tone change BETWEEN notes (settled)    : %d/30 silent\n", failures);
	}

	// -- editor tone-selection landing WHILE notes are actively firing, no settle time at all
	// - the actual race: play continuously, and mid-stream, issue the SAME kind of write the
	// Parts tab's tone-picker makes, with no gap for it to land cleanly first. --
	{
		int failures = 0;
		int failuresRightAfterEdit = 0;
		for (int i = 0; i < 60; ++i) {
			if (i == 20) {
				// The edit lands with NO settle time - immediately followed by more notes,
				// the way a real player picking a new sound mid-performance would do it.
				proc.sendTimbreTempParam(0, 0, 0);
				proc.sendTimbreTempParam(0, 1, 1); // back to AcouPiano2
			}
			const bool ok = playAndCheck(proc, 55 + (i % 5) * 3, 0.06, 0.02);
			if (!ok) {
				++failures;
				if (i >= 20 && i < 30) ++failuresRightAfterEdit;
				std::printf("  silent at note %d (right after edit at note 20: %s)\n", i,
				            (i >= 20 && i < 25) ? "YES" : "no");
			}
		}
		std::printf("editor tone change WHILE notes are streaming  : %d/60 silent (%d of the first "
		            "10 after the edit)\n",
		            failures, failuresRightAfterEdit);
	}

	// -- repeat the same live-edit-mid-stream shape several times in a row, matching a real
	// session where the editor gets touched more than once while playing --
	{
		int failures = 0;
		for (int round = 0; round < 10; ++round) {
			proc.sendTimbreTempParam(0, 0, round % 2); // alternate PRESET a/b
			proc.sendTimbreTempParam(0, 1, round % 5);
			for (int i = 0; i < 10; ++i)
				if (!playAndCheck(proc, 55 + (i % 5) * 3, 0.06, 0.02)) ++failures;
		}
		std::printf("10 rounds of live edit + 10 notes each        : %d/100 silent\n", failures);
	}

	return 0;
}
