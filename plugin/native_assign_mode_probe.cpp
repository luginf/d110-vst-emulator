// ROOT CAUSE CONFIRMED (2026-08-07) via a clean A/B RAM diff of Alan's own "broken" and
// "fixed" snapshots: the only meaningful difference was TimbreTemp field 5 (ASSIGN) for
// Part 1 - 0 ("POLY 1", single-assign) in the broken snapshot, 2 ("POLY 3", normal poly) in
// the fixed one. D110EditorPane's Parts-tab tone-picker (sendTimbreTempParam, fields 0/1
// only - group and number) never touches ASSIGN, so switching tones through the Editor
// silently inherits whatever ASSIGN the PREVIOUS tone happened to leave behind. Patch
// "Eric"'s original tone (VibeString) apparently used POLY 1 - so picking AcouPiano1
// afterward through the Editor played it in single-assign mode by accident, which is what
// mt32emu's own Part::playPoly() does on repeated retriggers of the same key while in that
// mode (Part.cpp: `if ((assignMode & 2) == 0) { abortFirstPoly(key); if
// (synth->isAbortingPoly()) return; }` - an intermittent, timing-dependent early-return,
// matching the ~30-50% observed miss rate exactly). This probe confirms directly: force
// ASSIGN=0 (POLY 1) on an otherwise-fresh instance/tone, hammer a repeated chord the same
// way Alan's own MIDI log showed, and check the per-part hit/miss rate.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 2; // Part 1, factory channel map

void render(D110AudioProcessor &proc, double seconds, const juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	const int blocks = juce::jmax(1, int(seconds * kSampleRate / kBlock));
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
	}
}

float renderMeasured(D110AudioProcessor &proc, double seconds, const juce::MidiBuffer *first = nullptr) {
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

	// -- baseline: default ASSIGN (POLY 3, the factory default), zero-gap repeated chord,
	// checked the same per-part way as the ASSIGN=0 test below for a fair comparison. --
	{
		int misses = 0;
		for (int round = 0; round < 80; ++round) {
			juce::MidiBuffer msg;
			msg.addEvent(juce::MidiMessage::noteOff(kChannel, 48), 0);
			msg.addEvent(juce::MidiMessage::noteOff(kChannel, 67), 0);
			msg.addEvent(juce::MidiMessage::noteOff(kChannel, 64), 0);
			msg.addEvent(juce::MidiMessage::noteOn(kChannel, 48, (juce::uint8)108), 1);
			msg.addEvent(juce::MidiMessage::noteOn(kChannel, 67, (juce::uint8)108), 1);
			msg.addEvent(juce::MidiMessage::noteOn(kChannel, 64, (juce::uint8)108), 1);
			render(proc, 0.012, &msg);
			render(proc, 0.03);
			if ((proc.enginePartStates() & 1u) == 0) ++misses;
		}
		std::printf("baseline (ASSIGN untouched, factory default): %d/80 rounds Part 1 missed\n", misses);
	}

	// -- force ASSIGN=0 (POLY 1) on Part 1, exactly like an Editor tone-pick that inherits a
	// stale mono setting from a previous tone - same repeated chord, same timing. --
	proc.sendTimbreTempParam(0, 5, 0); // part 1 (0-indexed): ASSIGN -> 0 (POLY 1)
	render(proc, 0.3);
	{
		// Zero gap this time - note-off immediately followed by note-on for the SAME keys,
		// no waiting - the shape most likely to catch abortFirstPoly()'s Poly still mid-abort
		// when the retrigger lands, rather than long enough for it to have finished. Checks
		// Part 1's OWN bit in enginePartStates(), not overall audio peak - with a 3-note
		// chord, a peak check can't tell one dropped note from two others still sounding.
		int misses = 0;
		for (int round = 0; round < 80; ++round) {
			juce::MidiBuffer msg;
			msg.addEvent(juce::MidiMessage::noteOff(kChannel, 48), 0);
			msg.addEvent(juce::MidiMessage::noteOff(kChannel, 67), 0);
			msg.addEvent(juce::MidiMessage::noteOff(kChannel, 64), 0);
			msg.addEvent(juce::MidiMessage::noteOn(kChannel, 48, (juce::uint8)108), 1);
			msg.addEvent(juce::MidiMessage::noteOn(kChannel, 67, (juce::uint8)108), 1);
			msg.addEvent(juce::MidiMessage::noteOn(kChannel, 64, (juce::uint8)108), 1);
			render(proc, 0.012, &msg); // ~one block, no gap at all
			render(proc, 0.03);        // let the block latency resolve before checking
			const bool part1Sounding = (proc.enginePartStates() & 1u) != 0;
			if (!part1Sounding) {
				++misses;
				std::printf("  round %2d: Part 1 NOT sounding\n", round);
			}
		}
		std::printf("ASSIGN=0 (POLY 1), zero-gap repeated chord: %d/80 rounds Part 1 missed\n", misses);
	}

	// -- confirm the fix: restore ASSIGN=2 (POLY 3), same pattern should clear up --
	proc.sendTimbreTempParam(0, 5, 2); // ASSIGN -> 2 (POLY 3)
	render(proc, 0.3);
	{
		int misses = 0;
		for (int round = 0; round < 80; ++round) {
			juce::MidiBuffer msg;
			msg.addEvent(juce::MidiMessage::noteOff(kChannel, 48), 0);
			msg.addEvent(juce::MidiMessage::noteOff(kChannel, 67), 0);
			msg.addEvent(juce::MidiMessage::noteOff(kChannel, 64), 0);
			msg.addEvent(juce::MidiMessage::noteOn(kChannel, 48, (juce::uint8)108), 1);
			msg.addEvent(juce::MidiMessage::noteOn(kChannel, 67, (juce::uint8)108), 1);
			msg.addEvent(juce::MidiMessage::noteOn(kChannel, 64, (juce::uint8)108), 1);
			render(proc, 0.012, &msg);
			render(proc, 0.03);
			if ((proc.enginePartStates() & 1u) == 0) ++misses;
		}
		std::printf("ASSIGN restored to 2 (POLY 3), zero-gap repeated chord: %d/80 rounds Part 1 missed\n",
		            misses);
	}

	return 0;
}
