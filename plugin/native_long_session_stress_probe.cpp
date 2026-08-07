// Alan's report (2026-08-06/07, Standalone + real MIDI keyboard over JACK): notes going
// silent while the Monitor tab still shows them received and the firmware's voice slots
// lighting up - NOT reproduced by native_retrigger_silence_probe.cpp's single-shot retrigger
// scenarios (every one of those came back audible, even the tightest zero-gap case). Two new
// facts narrow this down: it "doesn't happen all the time" (needs some volume/duration of
// real play first), and toggling Super Mode - which reopens the sound engine, see
// superModeReopenPending in PluginProcessor.cpp's processBlock() - "unblocks" it afterward.
// That points at STATE THAT DRIFTS OVER A REAL SESSION (something in this project's own
// note-context bookkeeping, or in mt32emu's own Part/Poly state, getting out of sync after
// enough notes) rather than a single deterministic event ordering - so this probe plays a
// long, randomised session (thousands of notes, varied channels/pitches/hold times/gaps,
// including plenty of fast retriggers) and watches for ANY note that the firmware accepts
// but produces no audible peak for, rather than testing one fixed scenario.
#include "Source/PluginProcessor.h"

#include <array>
#include <cstdio>
#include <random>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512; // Alan's own reported JACK buffer size

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

juce::AudioProcessorParameter *findParam(D110AudioProcessor &proc, const juce::String &nameContains) {
	for (auto *p : proc.getParameters())
		if (p->getName(64).containsIgnoreCase(nameContains)) return p;
	return nullptr;
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
	auto *superMode = findParam(proc, "Super Mode");

	std::mt19937 rng(12345);
	std::uniform_int_distribution<int> channelDist(2, 9); // parts 1-8, factory channel map
	std::uniform_int_distribution<int> noteDist(36, 84);
	std::uniform_real_distribution<double> holdDist(0.02, 0.4);   // 20-400ms held
	std::uniform_real_distribution<double> gapDist(0.0, 0.15);    // 0-150ms gap - plenty of fast retriggers

	constexpr int kTotalNotes = 3000;
	constexpr int kToggleEvery = 500; // matches Alan's "toggling Super Mode unblocks it"
	int silentCount = 0;
	int firstSilentIndex = -1;
	int silentSinceLastToggle = 0;
	std::array<int, 16> attemptsByChannel{}, silentByChannel{};

	for (int i = 0; i < kTotalNotes; ++i) {
		const int channel = channelDist(rng);
		const int note = noteDist(rng);
		const double hold = holdDist(rng);
		const double gap = gapDist(rng);
		++attemptsByChannel[static_cast<size_t>(channel)];

		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(channel, note, (juce::uint8)100), 0);
		const float peak = renderPeak(proc, hold, &on);

		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
		renderPeak(proc, gap, &off);

		const bool silent = peak < 0.001f;
		if (silent) {
			++silentCount;
			++silentSinceLastToggle;
			++silentByChannel[static_cast<size_t>(channel)];
			if (firstSilentIndex < 0) firstSilentIndex = i;
			if (silentCount <= 30)
				std::printf("SILENT #%-3d at note %4d: ch=%d note=%3d hold=%.3f gap=%.3f peak=%.5f "
				            "busy=%d activePartials=%d\n",
				            silentCount, i, channel, note, hold, gap, peak,
				            [&] {
					            std::vector<uint8_t> ram(static_cast<size_t>(D110CoreType::kRamSize));
					            if (!proc.getCore().getRam(ram.data())) return -1;
					            int busy = 0;
					            for (int s = 0; s < D110CoreType::kNumHardwareVoices; ++s) {
						            const size_t at =
						                size_t(D110CoreType::kSlotStateTable) + size_t(s) * 2;
						            const uint8_t st = ram[at];
						            if (st == D110CoreType::kSlotBusyValue
						                || st == D110CoreType::kSlotBusyValueAlt)
							            ++busy;
					            }
					            return busy;
				            }(),
				            proc.engineActivePartials());
		}

		if (i > 0 && i % kToggleEvery == 0 && superMode != nullptr) {
			std::printf("--- note %4d: toggling Super Mode (silent since last toggle: %d) ---\n", i,
			            silentSinceLastToggle);
			superMode->setValueNotifyingHost(1.0f);
			renderPeak(proc, 0.3);
			superMode->setValueNotifyingHost(0.0f);
			renderPeak(proc, 0.3);
			silentSinceLastToggle = 0;
		}
	}

	std::printf("\ntotal notes=%d silent=%d (%.2f%%) firstSilentIndex=%d\n", kTotalNotes, silentCount,
	            100.0 * silentCount / kTotalNotes, firstSilentIndex);
	std::printf("\nper-channel breakdown:\n");
	for (int ch = 2; ch <= 9; ++ch) {
		const int attempts = attemptsByChannel[static_cast<size_t>(ch)];
		const int sil = silentByChannel[static_cast<size_t>(ch)];
		std::printf("  channel %d (part %d): %d/%d silent (%.2f%%)\n", ch, ch - 1, sil, attempts,
		            attempts > 0 ? 100.0 * sil / attempts : 0.0);
	}

	// Controlled A/B: channel 2 (part 1) vs channel 9 (part 8) under IDENTICAL, fixed,
	// aggressive stimulus - no per-note randomness, so any difference is about the part, not
	// which notes/timings a given random seed happened to draw for it. Alan's own real report
	// is on a part he's remapped to MIDI channel 1 (not tested here - the factory default
	// leaves channel 1 unassigned, see README) - part 1 is the closest reasonable stand-in
	// unless he's remapped a different part there.
	std::printf("\ncontrolled hammer: channel 2 (part 1) vs channel 9 (part 8), identical stimulus\n");
	for (int channel : { 2, 9 }) {
		int hammerSilent = 0;
		constexpr int kHammerNotes = 800;
		for (int i = 0; i < kHammerNotes; ++i) {
			const int note = 55 + (i % 5) * 3; // a small rotating set of pitches, not one fixed note
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(channel, note, (juce::uint8)100), 0);
			const float peak = renderPeak(proc, 0.025, &on); // 25ms hold
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
			renderPeak(proc, 0.010, &off); // 10ms gap before the next retrigger
			if (peak < 0.001f) ++hammerSilent;
		}
		std::printf("  channel %d (part %d): %d/%d silent (%.2f%%)\n", channel, channel - 1, hammerSilent,
		            kHammerNotes, 100.0 * hammerSilent / kHammerNotes);
	}

	// Hypothesis: this tracks PARTIAL RESERVE (System tab), not "being part 8" as such - the
	// factory table is 4 4 4 4 3 3 3 2 5 (parts 1-8 then rhythm; see docs/factory_defaults.md),
	// so part 8 has the smallest melodic reserve (2) of the lot. Field numbering matches
	// D110EditorPane::layoutSystem's own Area::System cells: field 4+i is part i's reserve.
	// Swap the two parts' reserves and repeat the identical hammer - if the failure follows
	// the RESERVE VALUE rather than the part, part 1 (now reserve 2) should start failing and
	// part 8 (now reserve 4) should mostly stop.
	std::printf("\nswapping reserves - part 1 (field 4) <-> part 8 (field 11) - and repeating\n");
	proc.sendSystemParam(4, 2);  // part 1: 4 -> 2
	proc.sendSystemParam(11, 4); // part 8: 2 -> 4
	renderPeak(proc, 0.2);
	for (int channel : { 2, 9 }) {
		int hammerSilent = 0;
		constexpr int kHammerNotes = 800;
		for (int i = 0; i < kHammerNotes; ++i) {
			const int note = 55 + (i % 5) * 3;
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(channel, note, (juce::uint8)100), 0);
			const float peak = renderPeak(proc, 0.025, &on);
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
			renderPeak(proc, 0.010, &off);
			if (peak < 0.001f) ++hammerSilent;
		}
		std::printf("  channel %d (part %d, reserve now %s): %d/%d silent (%.2f%%)\n", channel, channel - 1,
		            channel == 2 ? "2" : "4", hammerSilent, kHammerNotes,
		            100.0 * hammerSilent / kHammerNotes);
	}

	// The swap above changed the mirrored RESERVE VALUE without changing the mirror TIMING or
	// margin - if the previous result held only because the mirror hadn't caught up yet (a full
	// second should rule that out) or because a difference of 2 vs 4 is too close to show
	// through the noise, an extreme, maximally-settled version should make it unambiguous:
	// part 8's reserve pushed way up (20) with a full second to settle, part 1's pushed way
	// down (1) the same way.
	std::printf("\nextreme reserves (part 1 -> 1, part 8 -> 20), 1s to settle, repeating\n");
	proc.sendSystemParam(4, 1);
	proc.sendSystemParam(11, 20);
	renderPeak(proc, 1.0);
	for (int channel : { 2, 9 }) {
		int hammerSilent = 0;
		constexpr int kHammerNotes = 800;
		for (int i = 0; i < kHammerNotes; ++i) {
			const int note = 55 + (i % 5) * 3;
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(channel, note, (juce::uint8)100), 0);
			const float peak = renderPeak(proc, 0.025, &on);
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
			renderPeak(proc, 0.010, &off);
			if (peak < 0.001f) ++hammerSilent;
		}
		std::printf("  channel %d (part %d, reserve now %s): %d/%d silent (%.2f%%)\n", channel, channel - 1,
		            channel == 2 ? "1" : "20", hammerSilent, kHammerNotes,
		            100.0 * hammerSilent / kHammerNotes);
	}

	// Part 1 just went from perfectly clean (every earlier test in this run) to 100% silent,
	// with nothing about IT changed except a reserve tweak that should have made it MORE
	// tolerant, not less - that smells like a session-wide leak accumulating across everything
	// played so far (~6000+ notes by this point), not a per-part reserve effect at all. If
	// engine partials are genuinely leaking, activePartials should sit well above 0 even after
	// every note has been off for seconds, with nothing currently held.
	juce::MidiBuffer allOff;
	for (int ch = 2; ch <= 9; ++ch)
		for (int n = 30; n <= 90; ++n) allOff.addEvent(juce::MidiMessage::noteOff(ch, n), 0);
	renderPeak(proc, 0.05, &allOff);
	renderPeak(proc, 5.0);
	std::printf("\nresting activePartials after 5s with every note off: %d (of %u) - %s\n",
	            proc.engineActivePartials(), proc.enginePartialCount(),
	            proc.engineActivePartials() > 4 ? "LEAK SUSPECTED" : "looks clean");

	return silentCount > 0 ? 1 : 0;
}
