// Alan's screenshot (2026-08-07): patch I-11 ("VibeString") selected via the Patches tab,
// then a 3-note chord (B4/E4/G4) played repeatedly on the remapped channel - Monitor showed
// "0 of 128 partials sounding" and "PARTS THE SOUND ENGINE IS HOLDING" completely empty for
// all 9 parts, while the firmware's own LA32 VOICE SLOTS table showed 12/32 busy and the LCD
// showed Part 1 actively triggered. Every earlier probe (native_retrigger_silence_probe,
// native_long_session_stress_probe, native_editor_race_probe, native_patch_select_race_probe)
// tested single, sequential notes - never a CHORD (several near-simultaneous notes on the
// same part). This one does, using the exact enginePartStates()/engineActivePartials()
// readouts the Monitor tab itself reads, not just audio peak, so a "firmware says busy,
// engine says nothing" divergence is caught directly rather than inferred from silence.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 2; // Part 1, factory channel map (Alan's own is remapped to ch1)

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

juce::MidiBuffer chordOn(int ch, std::initializer_list<int> notes, int vel) {
	juce::MidiBuffer m;
	for (int n : notes) m.addEvent(juce::MidiMessage::noteOn(ch, n, (juce::uint8)vel), 0);
	return m;
}
juce::MidiBuffer chordOff(int ch, std::initializer_list<int> notes) {
	juce::MidiBuffer m;
	for (int n : notes) m.addEvent(juce::MidiMessage::noteOff(ch, n), 0);
	return m;
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

	// -- baseline: chord on the DEFAULT patch, never touching the Patches tab at all --
	{
		const std::initializer_list<int> notes = { 71, 64, 67 }; // B4, E4, G4
		int mismatches = 0;
		for (int round = 0; round < 50; ++round) {
			auto on = chordOn(kChannel, notes, 80 + (round % 10));
			render(proc, 0.08, &on);
			if (busySlots(proc) > 0 && proc.engineActivePartials() == 0) ++mismatches;
			auto off = chordOff(kChannel, notes);
			render(proc, 0.03, &off);
		}
		std::printf("baseline chord, default patch, no editor interaction: %d/50 mismatched\n",
		            mismatches);
	}

	// -- select patch I-11 (0-indexed 10) via the Patches tab's own mechanism, fully settled --
	proc.selectPatch(10);
	render(proc, 3.0);
	reportEngineState(proc, "after selecting patch I-11, before any notes");

	// Repeated 3-note chord (matching B4/E4/G4 in Alan's own MIDI log): on, hold briefly, off,
	// brief gap, retrigger - many rounds, checking engine state after every one.
	{
		const std::initializer_list<int> notes = { 71, 64, 67 };
		int mismatches = 0;
		for (int round = 0; round < 100; ++round) {
			auto on = chordOn(kChannel, notes, 80 + (round % 10));
			render(proc, 0.08, &on);
			const int busy = busySlots(proc);
			const int activePartials = proc.engineActivePartials();
			if (busy > 0 && activePartials == 0) {
				++mismatches;
				std::printf("MISMATCH at round %d: firmware busySlots=%d but engine "
				            "activePartials=0\n",
				            round, busy);
			}
			auto off = chordOff(kChannel, notes);
			render(proc, 0.03, &off);
		}
		std::printf("\nrepeated chord after selecting patch I-11: %d/100 rounds mismatched "
		            "(firmware busy, engine silent)\n",
		            mismatches);
		reportEngineState(proc, "final state");
	}

	return 0;
}
