// Follow-up to native_editor_race_probe.cpp, which tested the PARTS tab's tone-picker
// (sendTimbreTempParam, a direct write) racing against streaming notes and found nothing.
// Alan's report may instead mean the PATCHES tab, which works completely differently:
// D110AudioProcessor::selectPatch() queues a sequence of simulated PATCH/BANK/NUMBER button
// presses and executes them one per 60ms tick via a juce::Timer - NOT instantly. That timer
// needs JUCE's MessageManager dispatch loop actually running to fire at all; earlier probes
// that called selectPatch() (native_single_part_stuck_probe.cpp included) never pumped that
// loop, so the queued button presses likely never executed - this probe does it properly
// (runDispatchLoopUntil, the same pattern editor_test.cpp already established for verifying
// selectPatch() itself), and plays notes WHILE a multi-step patch change is still mid-flight,
// the way a player clicking a Patches row while actively playing would.
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
		// Lets D110AudioProcessor's own juce::Timer (driving selectPatch()'s queued button
		// presses) actually fire - real time, not simulated audio time, since JUCE timers are
		// wall-clock driven regardless of how fast processBlock is being called here.
		juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
	}
	return peak;
}

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

	// -- baseline: no patch selection at all --
	{
		int failures = 0;
		for (int i = 0; i < 30; ++i)
			if (!playAndCheck(proc, 55 + (i % 5) * 3, 0.08, 0.03)) ++failures;
		std::printf("baseline, no patch selection                       : %d/30 silent\n", failures);
	}

	// -- selectPatch(), fully settled (waited out) before playing again - repeated across many
	// DIFFERENT destination patches, since the single-patch version above showed one stray
	// silent note (1/30) where the big clean 8000-note run never saw one at all. Tracks
	// whether a failure is specifically the FIRST note played after a fresh selectPatch(). --
	{
		int failures = 0, firstNoteFailures = 0, totalNotes = 0;
		for (int patch = 0; patch < 20; ++patch) {
			proc.selectPatch(patch * 3 % 64);
			renderPeak(proc, 3.0); // plenty of time for every button-press step to settle
			for (int i = 0; i < 10; ++i) {
				++totalNotes;
				if (!playAndCheck(proc, 55 + (i % 5) * 3, 0.08, 0.03)) {
					++failures;
					if (i == 0) ++firstNoteFailures;
					std::printf("  silent: patch=%d note-index-since-select=%d\n", patch * 3 % 64, i);
				}
			}
		}
		std::printf("selectPatch() settled, 20 different patches x10 notes: %d/%d silent (%d were the "
		            "very first note after settling)\n",
		            failures, totalNotes, firstNoteFailures);
	}

	// -- the real race: selectPatch() issued, then notes played IMMEDIATELY while the queued
	// button-press sequence is still executing (patchSteps > 0) - exactly what "clicking a
	// Patches row while playing" looks like. --
	{
		proc.selectPatch(20); // a patch several banks away - a longer button-press sequence
		int failures = 0;
		int i = 0;
		// Keep playing until the patch-select sequence has actually finished, then a bit more.
		while (proc.isSelectingPatch() || i < 40) {
			if (!playAndCheck(proc, 55 + (i % 5) * 3, 0.05, 0.02)) {
				++failures;
				std::printf("  silent at note %d (still selecting patch: %s)\n", i,
				            proc.isSelectingPatch() ? "yes" : "no");
			}
			++i;
			if (i > 200) break; // safety
		}
		std::printf("notes played WHILE selectPatch() still mid-flight  : %d/%d silent\n", failures, i);
	}

	return 0;
}
