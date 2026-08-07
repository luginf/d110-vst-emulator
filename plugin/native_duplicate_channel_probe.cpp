// Isolates one specific detail found in Alan's own reproducing snapshot
// (native_snapshot_repro_probe.cpp): parts 3 AND 4 are BOTH mapped to MIDI channel 3 - so a
// single note-on there triggers two parts from one MIDI event. Tests that shape alone, on an
// otherwise fresh/default instance (no snapshot, no custom patch content), to see whether the
// duplicate mapping by itself is enough to produce the same "firmware busy, engine silent"
// divergence, or whether it depends on the rest of that snapshot's content too.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

void render(D110AudioProcessor &proc, double seconds, const juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	const int blocks = juce::jmax(1, int(seconds * kSampleRate / kBlock));
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
	}
}

int busySlots(D110AudioProcessor &proc) {
	std::vector<uint8_t> ram(static_cast<size_t>(D110CoreType::kRamSize));
	if (!proc.getCore().getRam(ram.data())) return -1;
	int busy = 0;
	for (int s = 0; s < D110CoreType::kNumHardwareVoices; ++s) {
		const size_t at = size_t(D110CoreType::kSlotStateTable) + size_t(s) * 2;
		const uint8_t st = ram[at];
		if (st == D110CoreType::kSlotBusyValue || st == D110CoreType::kSlotBusyValueAlt) ++busy;
	}
	return busy;
}

void reportEngineState(D110AudioProcessor &proc, const char *label) {
	const uint32_t states = proc.enginePartStates();
	std::printf("%-45s: activePartials=%d  partsHolding=[", label, proc.engineActivePartials());
	for (int p = 0; p < 9; ++p) std::printf("%d", (states >> p) & 1u);
	std::printf("]  firmwareBusySlots=%d\n", busySlots(proc));
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

	// -- baseline: default channel map, no duplicate --
	{
		int mismatches = 0;
		for (int round = 0; round < 30; ++round) {
			const int note = 55 + (round % 7) * 3;
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(4, note, (juce::uint8)90), 0); // channel 4 -> part 3
			render(proc, 0.07, &on);
			if (busySlots(proc) > 0 && proc.engineActivePartials() == 0) ++mismatches;
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(4, note), 0);
			render(proc, 0.03, &off);
		}
		std::printf("baseline, default channel map (part 3 alone on ch4): %d/30 mismatched\n",
		            mismatches);
	}

	// -- force parts 3 AND 4 (0-indexed 2, 3) both onto channel 3, matching Alan's snapshot --
	proc.sendSystemParam(13 + 2, 2); // part 3 -> channel 3 (raw value = channel-1)
	proc.sendSystemParam(13 + 3, 2); // part 4 -> channel 3 too
	render(proc, 0.5);
	reportEngineState(proc, "after forcing parts 3+4 both onto channel 3");

	{
		int mismatches = 0;
		for (int round = 0; round < 60; ++round) {
			const int note = 55 + (round % 7) * 3;
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(3, note, (juce::uint8)90), 0);
			render(proc, 0.07, &on);
			if (busySlots(proc) > 0 && proc.engineActivePartials() == 0) {
				++mismatches;
				std::printf("  mismatch at round %d\n", round);
			}
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(3, note), 0);
			render(proc, 0.03, &off);
		}
		std::printf("parts 3+4 duplicated on channel 3, single notes    : %d/60 mismatched\n",
		            mismatches);
		reportEngineState(proc, "after the duplicate-channel single-note run");
	}

	// -- a chord on the duplicated channel, since Alan's own report involved chords too --
	{
		int mismatches = 0;
		for (int round = 0; round < 30; ++round) {
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(3, 64, (juce::uint8)90), 0);
			on.addEvent(juce::MidiMessage::noteOn(3, 67, (juce::uint8)90), 0);
			on.addEvent(juce::MidiMessage::noteOn(3, 71, (juce::uint8)90), 0);
			render(proc, 0.07, &on);
			if (busySlots(proc) > 0 && proc.engineActivePartials() == 0) {
				++mismatches;
				std::printf("  chord mismatch at round %d\n", round);
			}
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(3, 64), 0);
			off.addEvent(juce::MidiMessage::noteOff(3, 67), 0);
			off.addEvent(juce::MidiMessage::noteOff(3, 71), 0);
			render(proc, 0.03, &off);
		}
		std::printf("parts 3+4 duplicated on channel 3, chords          : %d/30 mismatched\n",
		            mismatches);
		reportEngineState(proc, "final state");
	}

	return 0;
}
