// Alan confirmed (2026-08-07) that loading his own saved snapshot (/tmp/01.d110snap, saved
// under a colleague's account "eric" on this same shared machine) reproduces the cutout HERE
// too, not just on his JACK/Focusrite machine - the first time this bug has been reproducible
// outside his own setup. This probe loads that exact snapshot (same importMemorySnapshot()
// path the Utility tab's LOAD SNAPSHOT button uses - a full power cycle, matching what
// actually happens when a user does this), reads back the live channel map it left the
// instrument in, then hammers notes/chords on whichever channels are actually mapped,
// watching for the same "firmware busy, engine holds nothing" divergence
// native_chord_patch_probe.cpp already knows how to detect.
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

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const juce::String snapshotPath = argc > 1 ? argv[1] : "/tmp/01.d110snap";

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not start\n");
		return 1;
	}

	std::printf("importing snapshot: %s\n", snapshotPath.toRawUTF8());
	proc.importMemorySnapshot(juce::File(snapshotPath));
	// importMemorySnapshot() power-cycles internally (off then on) - give the firmware the
	// same boot time the very first power-on above needed.
	render(proc, 9.0);
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not come back up after loading the snapshot\n");
		return 1;
	}
	proc.setForwardNotesToFirmware(true);

	// Alan: "cela le fait sur le patch 01 intitulé Eric" - patch slot 1 (0-indexed 0) is
	// specifically what triggers it, selected the same way the Patches tab does (real
	// PATCH/BANK/NUMBER button-press simulation, fully settled before playing).
	proc.selectPatch(0);
	render(proc, 3.0);
	std::printf("selected patch 01 (\"Eric\")\n");

	// Read back the live per-part channel map (same bytes Monitor/Parts reads) so this probe
	// tests whichever channels the snapshot actually left mapped, not a guess.
	{
		std::vector<uint8_t> ram(static_cast<size_t>(D110CoreType::kRamSize));
		proc.getCore().getRam(ram.data());
		std::printf("live channel map after loading snapshot:\n");
		for (int part = 0; part < 8; ++part) {
			const size_t addr = size_t(D110CoreType::kRamSystem) + 13 + size_t(part);
			const int raw = addr < ram.size() ? int(ram[addr]) : -1;
			std::printf("  part %d -> %s\n", part + 1,
			            (raw >= 0 && raw <= 15) ? ("channel " + juce::String(raw + 1)).toRawUTF8()
			                                    : "OFF/unknown");
		}
	}

	reportEngineState(proc, "right after snapshot loaded, before any notes");

	// Hammer every MIDI channel 1-16 with both single notes and a 3-note chord, watching for
	// the firmware-busy/engine-silent divergence - broad on purpose, since we don't yet know
	// exactly which channel(s)/part(s) in Alan's own saved patch trigger it.
	int totalMismatches = 0;
	for (int ch = 1; ch <= 16; ++ch) {
		int chMismatches = 0;
		for (int round = 0; round < 15; ++round) {
			const int note = 55 + (round % 7) * 3;
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(ch, note, (juce::uint8)90), 0);
			render(proc, 0.07, &on);
			if (busySlots(proc) > 0 && proc.engineActivePartials() == 0) ++chMismatches;
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(ch, note), 0);
			render(proc, 0.03, &off);
		}
		// A chord too, on the same channel.
		for (int round = 0; round < 15; ++round) {
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(ch, 64, (juce::uint8)90), 0);
			on.addEvent(juce::MidiMessage::noteOn(ch, 67, (juce::uint8)90), 0);
			on.addEvent(juce::MidiMessage::noteOn(ch, 71, (juce::uint8)90), 0);
			render(proc, 0.07, &on);
			if (busySlots(proc) > 0 && proc.engineActivePartials() == 0) ++chMismatches;
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(ch, 64), 0);
			off.addEvent(juce::MidiMessage::noteOff(ch, 67), 0);
			off.addEvent(juce::MidiMessage::noteOff(ch, 71), 0);
			render(proc, 0.03, &off);
		}
		if (chMismatches > 0) {
			std::printf("channel %2d: %d/30 mismatched (firmware busy, engine silent)\n", ch,
			            chMismatches);
			totalMismatches += chMismatches;
		}
	}

	std::printf("\ntotal mismatches across all 16 channels: %d\n", totalMismatches);
	reportEngineState(proc, "final state");
	return totalMismatches > 0 ? 1 : 0;
}
