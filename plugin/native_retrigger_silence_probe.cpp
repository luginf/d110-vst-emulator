// Alan's precise report (2026-08-06, JACK + Focusrite machine, buffer 512, no xruns, no
// problem on other VSTs): retriggering a note - same pitch OR a different one - while the
// PREVIOUS note's firmware voice slot is still lit on the Monitor tab produces no audible
// sound for the retrigger, even with plenty of free voices (12 of 32 busy). Waiting for the
// slot to go dark first plays normally. This rules out real polyphony exhaustion.
//
// native_single_part_stuck_probe.cpp already stress-tested this shape of retriggering and
// found the FIRMWARE side clean (every note-on accepted, nothing stuck) - but that probe only
// ever checked firmware bookkeeping (firmwareNoteOns()/engineActivePartials() at rest), never
// whether the ENGINE actually rendered audio for a given retrigger while a slot was still
// busy. This probe drives the exact same real native core + real mt32emu synth through
// processBlock() (kBlock matches Alan's own reported JACK buffer size) and measures the
// actual rendered peak per note - the direct, literal "did sound come out" check.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512; // Alan's own reported JACK buffer size
constexpr int kChannel = 2; // Part 1, factory channel map

float renderAndMeasurePeak(D110AudioProcessor &proc, double seconds, const juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	const int blocks = juce::jmax(1, int(seconds * kSampleRate / kBlock));
	float peak = 0.0f;
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		peak = juce::jmax(peak, audio.getMagnitude(0, kBlock));
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
	return peak;
}

int busySlots(D110AudioProcessor &proc) {
	// The exact table Monitor's own "LA32 VOICE SLOTS" grid reads.
	std::vector<uint8_t> ram(static_cast<size_t>(D110CoreType::kRamSize));
	if (!proc.getCore().getRam(ram.data())) return -1;
	int busy = 0;
	for (int s = 0; s < D110CoreType::kNumHardwareVoices; ++s) {
		const size_t at = size_t(D110CoreType::kSlotStateTable) + size_t(s) * 2;
		const uint8_t state = ram[at];
		if (state == D110CoreType::kSlotBusyValue || state == D110CoreType::kSlotBusyValueAlt) ++busy;
	}
	return busy;
}

juce::MidiBuffer noteOnMsg(int note, int vel) {
	juce::MidiBuffer m;
	m.addEvent(juce::MidiMessage::noteOn(kChannel, note, (juce::uint8)vel), 0);
	return m;
}
juce::MidiBuffer noteOffMsg(int note) {
	juce::MidiBuffer m;
	m.addEvent(juce::MidiMessage::noteOff(kChannel, note), 0);
	return m;
}

void report(const char *label, int busyBefore, float peak) {
	std::printf("%-55s: busySlotsBefore=%2d peak=%.4f  %s\n", label, busyBefore, peak,
	            peak < 0.001f ? "SILENT" : "audible");
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	renderAndMeasurePeak(proc, 9.0);
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not start\n");
		return 1;
	}
	proc.setForwardNotesToFirmware(true);

	// -- can part 8 (channel 9) play a SINGLE, isolated, well-spaced note at all? Reserve is
	// ruled out (raising it changed nothing, in both a fresh and a late-session test) - before
	// chasing a retrigger-timing theory further, check the much simpler possibility that part
	// 8's factory patch/tone is just broken/muted outright, independent of any retriggering. --
	{
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(9, 60, (juce::uint8)100), 0);
		const float peak = renderAndMeasurePeak(proc, 0.3, &on);
		std::printf("SINGLE isolated note on channel 9/part 8            : peak=%.4f  %s\n", peak,
		            peak < 0.001f ? "SILENT" : "audible");
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(9, 60), 0);
		renderAndMeasurePeak(proc, 2.0, &off);
	}

	// -- part 8's factory tone is "Strings 3" (docs/factory_defaults.md) - a pad-type sound
	// that plausibly has a much slower attack than part 1's fast, percussive "AcouPiano2".
	// The single-note peak above (0.0144) was already an order of magnitude quieter than
	// channel 2's baseline (0.1547-0.1572) - if Strings 3 just needs longer than a 25ms hammer
	// hold to swell up to an audible level, that alone could explain the whole "silent"
	// pattern with NO note-handling bug at all. Force part 8 onto the exact same tone as part
	// 1 (group 0/PRESET, number 1 = AcouPiano2) and repeat the identical hammer - if THIS
	// comes back mostly audible, the bug isn't part-8-specific at all, it's tone-specific.
	{
		proc.sendTimbreTempParam(7, 0, 0); // part 8 (0-indexed 7): tone group -> PRESET a
		proc.sendTimbreTempParam(7, 1, 1); // tone number -> AcouPiano2, same as part 1
		renderAndMeasurePeak(proc, 0.5);
		int silent = 0;
		constexpr int kNotes = 200;
		for (int i = 0; i < kNotes; ++i) {
			const int note = 55 + (i % 5) * 3;
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(9, note, (juce::uint8)100), 0);
			const float peak = renderAndMeasurePeak(proc, 0.025, &on);
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(9, note), 0);
			renderAndMeasurePeak(proc, 0.010, &off);
			if (peak < 0.001f) ++silent;
		}
		std::printf("SAME hammer, part 8 given part 1's AcouPiano2 tone  : %d/%d silent (%.2f%%)\n", silent,
		            kNotes, 100.0 * silent / kNotes);
	}

	// -- immediate hammer, channel 9 (part 8), zero prior notes played this session - is the
	// near-total failure rate seen in native_long_session_stress_probe an IMMEDIATE property
	// of part 8, or does it only appear after a long session accumulates something? --
	{
		int silent = 0;
		constexpr int kNotes = 200;
		for (int i = 0; i < kNotes; ++i) {
			const int note = 55 + (i % 5) * 3;
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(9, note, (juce::uint8)100), 0);
			const float peak = renderAndMeasurePeak(proc, 0.025, &on);
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(9, note), 0);
			renderAndMeasurePeak(proc, 0.010, &off);
			if (peak < 0.001f) ++silent;
		}
		std::printf("FRESH BOOT hammer on channel 9/part 8 (zero prior notes): %d/%d silent (%.2f%%)\n", silent,
		            kNotes, 100.0 * silent / kNotes);
	}

	// -- same, but with part 8's Partial Reserve raised BEFORE any note is played, so there is
	// no accumulated-session confound this time (the earlier long-session probe's reserve
	// test came back inconclusive, but it ran very late in a long session where OTHER things
	// had already gone wrong - see its own final "resting activePartials" note). Clean test:
	// does raising the reserve fix it, on a fresh session, or not? --
	{
		proc.sendSystemParam(11, 16); // part 8 (field 4+7=11): 2 -> 16
		renderAndMeasurePeak(proc, 1.0);
		int silent = 0;
		constexpr int kNotes = 200;
		for (int i = 0; i < kNotes; ++i) {
			const int note = 55 + (i % 5) * 3;
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(9, note, (juce::uint8)100), 0);
			const float peak = renderAndMeasurePeak(proc, 0.025, &on);
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(9, note), 0);
			renderAndMeasurePeak(proc, 0.010, &off);
			if (peak < 0.001f) ++silent;
		}
		std::printf("SAME, part 8 reserve raised to 16 first             : %d/%d silent (%.2f%%)\n", silent,
		            kNotes, 100.0 * silent / kNotes);
	}

	// -- baseline: single note, held, released, waited out fully --
	{
		auto on = noteOnMsg(60, 100);
		float peak = renderAndMeasurePeak(proc, 0.3, &on);
		std::printf("baseline single note                                  : peak=%.4f  %s\n", peak,
		            peak < 0.001f ? "SILENT (unexpected)" : "audible");
		auto off = noteOffMsg(60);
		renderAndMeasurePeak(proc, 0.05, &off);
		renderAndMeasurePeak(proc, 2.0); // let the slot fully go dark
		std::printf("busySlots after 2s idle (should be 0)                 : %d\n", busySlots(proc));
	}

	// -- same pitch, note-off then immediately retriggered before the slot goes dark --
	{
		auto on1 = noteOnMsg(60, 100);
		renderAndMeasurePeak(proc, 0.1, &on1);
		const int busyBefore = busySlots(proc);
		auto off1 = noteOffMsg(60);
		renderAndMeasurePeak(proc, kBlock / kSampleRate, &off1); // exactly one block
		auto on2 = noteOnMsg(60, 100);
		const float peak = renderAndMeasurePeak(proc, 0.3, &on2);
		report("same pitch retriggered right after note-off", busyBefore, peak);
		auto off2 = noteOffMsg(60);
		renderAndMeasurePeak(proc, 2.0, &off2);
	}

	// -- different pitch, note-off for the first then immediately the second --
	{
		auto on1 = noteOnMsg(60, 100);
		renderAndMeasurePeak(proc, 0.1, &on1);
		const int busyBefore = busySlots(proc);
		auto off1 = noteOffMsg(60);
		renderAndMeasurePeak(proc, kBlock / kSampleRate, &off1);
		auto on2 = noteOnMsg(64, 100);
		const float peak = renderAndMeasurePeak(proc, 0.3, &on2);
		report("different pitch played right after the first's note-off", busyBefore, peak);
		auto off2 = noteOffMsg(64);
		renderAndMeasurePeak(proc, 2.0, &off2);
	}

	// -- different pitch overlapping the first, no note-off sent at all (a chord/legato shape) --
	{
		auto on1 = noteOnMsg(60, 100);
		renderAndMeasurePeak(proc, 0.1, &on1);
		const int busyBefore = busySlots(proc);
		auto on2 = noteOnMsg(64, 100);
		const float peak = renderAndMeasurePeak(proc, 0.3, &on2);
		report("different pitch overlapping the first (no note-off yet)", busyBefore, peak);
		juce::MidiBuffer offBoth = noteOffMsg(60);
		offBoth.addEvent(juce::MidiMessage::noteOff(kChannel, 64), 0);
		renderAndMeasurePeak(proc, 2.0, &offBoth);
	}

	// -- same pitch, note-on/note-off/note-on ALL in the SAME block (zero real-time gap) --
	{
		auto on1 = noteOnMsg(60, 100);
		renderAndMeasurePeak(proc, 0.1, &on1);
		const int busyBefore = busySlots(proc);
		juce::MidiBuffer allThree = noteOffMsg(60);
		allThree.addEvent(juce::MidiMessage::noteOn(kChannel, 60, (juce::uint8)100), 1);
		const float peak = renderAndMeasurePeak(proc, 0.3, &allThree);
		report("same pitch: off+on in the SAME block (zero gap)", busyBefore, peak);
		auto off2 = noteOffMsg(60);
		renderAndMeasurePeak(proc, 2.0, &off2);
	}

	// -- same pitch, retriggered with NO note-off at all - a second note-on while the first
	// is still sounding and was never released (true double-press, no release in between) --
	{
		auto on1 = noteOnMsg(60, 100);
		renderAndMeasurePeak(proc, 0.1, &on1);
		const int busyBefore = busySlots(proc);
		auto on2 = noteOnMsg(60, 100);
		const float peak = renderAndMeasurePeak(proc, 0.3, &on2);
		report("same pitch retriggered with NO note-off first", busyBefore, peak);
		auto off2 = noteOffMsg(60);
		renderAndMeasurePeak(proc, 2.0, &off2);
	}

	return 0;
}
