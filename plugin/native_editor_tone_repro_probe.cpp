// Alan's fully deterministic repro (2026-08-07, reproducible every time): load his snapshot
// (/tmp/01.d110snap), on channel 1 (Part 1). Changing that part's instrument to AcouPiano1
// via OUR Editor's Parts tab (a direct TimbreTemp write - sendTimbreTempParam, same call the
// tone-picker makes) leaves notes silent afterward. Choosing the exact same AcouPiano1 from
// the photographed panel's own "native" TIMBRE/PART buttons instead plays correctly. This
// probe reproduces both paths on the actual snapshot and compares them directly.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 1; // Alan's own remap: Part 1 -> channel 1 in this snapshot

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
	std::printf("%-55s: activePartials=%d  partsHolding=[", label, proc.engineActivePartials());
	for (int p = 0; p < 9; ++p) std::printf("%d", (states >> p) & 1u);
	std::printf("]  firmwareBusySlots=%d\n", busySlots(proc));
}

int playAndPeak(D110AudioProcessor &proc, int note, double hold, double gap) {
	juce::AudioBuffer<float> audio(2, kBlock);
	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(kChannel, note, (juce::uint8)90), 0);
	float peak = 0.0f;
	const int holdBlocks = juce::jmax(1, int(hold * kSampleRate / kBlock));
	juce::MidiBuffer empty;
	for (int b = 0; b < holdBlocks; ++b) {
		audio.clear();
		proc.processBlock(audio, b == 0 ? on : empty);
		peak = juce::jmax(peak, audio.getMagnitude(0, kBlock));
		juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
	}
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(kChannel, note), 0);
	render(proc, gap, &off);
	return peak >= 0.001f ? 1 : 0;
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
	render(proc, 9.0);
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not come back up\n");
		return 1;
	}
	proc.setForwardNotesToFirmware(true);
	{
		std::vector<uint8_t> ram(static_cast<size_t>(D110CoreType::kRamSize));
		proc.getCore().getRam(ram.data());
		std::printf("current patch BEFORE selectPatch(0): %d\n",
		            int(ram[(size_t)D110CoreType::kRamPatchNumber]));
	}
	// Patch 01 was ALREADY the live patch right after boot - so selectPatch(0) alone would be
	// a no-op (zero queued bank/number button presses), never exercising real navigation.
	// Alan clicks a DIFFERENT Patches-tab row before landing on "Eric" in real use - move away
	// first (a real multi-step queued button-press sequence), THEN select patch 01 the same
	// way, so THIS press sequence is real too, matching what he actually does.
	proc.selectPatch(30);
	render(proc, 3.0);
	std::printf("moved away to patch 31 first (real button-press sequence)\n");
	{
		std::vector<uint8_t> ram(static_cast<size_t>(D110CoreType::kRamSize));
		proc.getCore().getRam(ram.data());
		std::printf("live channel map on patch 31:\n");
		for (int part = 0; part < 8; ++part) {
			const size_t addr = size_t(D110CoreType::kRamSystem) + 13 + size_t(part);
			std::printf("  part %d -> channel %d\n", part + 1, int(ram[addr]) + 1);
		}
	}
	proc.selectPatch(0); // patch 01 "Eric" - now a REAL navigation, not a no-op
	render(proc, 1.0);
	{
		std::vector<uint8_t> ram(static_cast<size_t>(D110CoreType::kRamSize));
		proc.getCore().getRam(ram.data());
		std::printf("live channel map back on patch 01:\n");
		for (int part = 0; part < 8; ++part) {
			const size_t addr = size_t(D110CoreType::kRamSystem) + 13 + size_t(part);
			std::printf("  part %d -> channel %d\n", part + 1, int(ram[addr]) + 1);
		}
	}
	render(proc, 3.0);
	reportEngineState(proc, "after loading snapshot + navigating to patch 01");

	// -- baseline: play on channel 1 right now, before touching the tone at all - tracking
	// busySlots growth per note, not just audibility, since the leak (if any) may not cause
	// silence immediately. --
	{
		int audible = 0;
		for (int i = 0; i < 30; ++i) {
			audible += playAndPeak(proc, 55 + (i % 5) * 3, 0.08, 0.03);
			if (i % 10 == 9)
				std::printf("  baseline note %2d: busySlots=%d\n", i, busySlots(proc));
		}
		std::printf("baseline, channel 1, tone untouched                    : %d/30 audible\n", audible);
	}

	// -- change Part 1's tone to AcouPiano1 via the EDITOR's own mechanism (a direct
	// TimbreTemp write - sendTimbreTempParam, exactly what the Parts tab's tone-picker calls).
	// AcouPiano1 is preset group 0 (PRESET a), tone number 0 (the very first preset tone -
	// AcouPiano2, the one used throughout earlier probes, is number 1, the second). Many more
	// notes than before (60, not 10) and busySlots printed after every one, to see whether it
	// keeps climbing indefinitely (a real leak) and whether/when that turns into real silence. --
	{
		proc.sendTimbreTempParam(0, 0, 0); // part 1 (0-indexed): tone group -> PRESET a
		proc.sendTimbreTempParam(0, 1, 0); // tone number -> AcouPiano1
		render(proc, 0.5);
		reportEngineState(proc, "after EDITOR-style tone change to AcouPiano1");
		int audible = 0;
		int firstSilent = -1;
		for (int i = 0; i < 60; ++i) {
			const int ok = playAndPeak(proc, 55 + (i % 5) * 3, 0.08, 0.03);
			audible += ok;
			if (!ok && firstSilent < 0) firstSilent = i;
			std::printf("  post-edit note %2d: %-8s busySlots=%d activePartials=%d\n", i,
			            ok ? "audible" : "SILENT", busySlots(proc), proc.engineActivePartials());
		}
		std::printf("AFTER EDITOR tone change to AcouPiano1, channel 1      : %d/60 audible, "
		            "firstSilent=%d\n",
		            audible, firstSilent);
		reportEngineState(proc, "final state");
	}

	// -- one more shape: the tone change lands WHILE a note is actively held (not with a clean
	// gap before/after) - "même notes qui ne sonnent pas" (Alan) may mean the note that was
	// already sounding when he picked the new tone. Hold a note, edit mid-hold, keep going. --
	{
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, 60, (juce::uint8)90), 0);
		render(proc, 0.05, &on); // brief hold, note is actively sounding

		proc.sendTimbreTempParam(0, 0, 1); // switch to PRESET b this time, for a clean edit
		proc.sendTimbreTempParam(0, 1, 0);
		render(proc, 0.05); // no gap - straight back into more notes on the SAME still-held note

		int audible = 0;
		int firstSilent = -1;
		for (int i = 0; i < 30; ++i) {
			const int ok = playAndPeak(proc, 60, 0.06, 0.02); // same note repeatedly, matching
			                                                   // "même notes"
			audible += ok;
			if (!ok && firstSilent < 0) firstSilent = i;
			if (!ok)
				std::printf("  mid-hold-edit note %2d: SILENT busySlots=%d activePartials=%d\n", i,
				            busySlots(proc), proc.engineActivePartials());
		}
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kChannel, 60), 0);
		render(proc, 0.5, &off);
		std::printf("tone changed WHILE note 60 was still held, then repeated: %d/30 audible, "
		            "firstSilent=%d\n",
		            audible, firstSilent);
		reportEngineState(proc, "final state (mid-hold edit)");
	}

	return 0;
}
