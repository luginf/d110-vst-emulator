// User's live-DAW report on the MAME-backed D110Emulator (native core is fine): playing SHORT,
// not-necessarily-overlapping notes on Fantasy (b01) or Steel Drum (b48) makes the note sustain
// forever (release never happens) and the top LCD row's part-sounding indicator freezes - i.e.
// the firmware itself appears to stop running, not just "this one note didn't release". Tests
// single, well-spaced notes first (the simplest case that could show it), then a short fast
// burst, on both named tones, watching RAM staleness (is the firmware's mainline code - LCD
// scan, panel poll - still running at all) and note-off completion, not just note counts.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 2; // Part 1

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
std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

// Plays one note, holds it briefly, releases it, then watches for a real amount of real time
// (per this project's own probe-must-wait-on-the-clock discipline) whether the note-off ever
// actually completes AND whether the firmware's own RAM keeps changing at all (a genuinely
// stuck CPU freezes ALL of it, not just this one voice's bookkeeping).
bool testOneNote(D110AudioProcessor &proc, int note, const char *label) {
	const uint64_t onsBefore = proc.getCore().firmwareNoteOns();
	const uint64_t offsBefore = proc.getCore().firmwareNoteOffs();

	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(kChannel, note, 0.9f), 0);
	renderBlocks(proc, 9, &on); // ~100ms held - "short note"

	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(kChannel, note), 0);
	renderBlocks(proc, 2, &off);

	auto ramBefore = snapshot(proc);
	render(proc, 2.0); // real time to let release finish AND to catch a frozen firmware
	auto ramAfter = snapshot(proc);
	const bool ramStatic = ramBefore == ramAfter;

	const bool onOk = proc.getCore().firmwareNoteOns() == onsBefore + 1;
	const bool offOk = proc.getCore().firmwareNoteOffs() == offsBefore + 1;
	std::printf("%-28s onOk=%d offOk=%d ramStatic-after-2s=%d%s\n", label, onOk, offOk, ramStatic,
	            (!offOk || ramStatic) ? "  <-- SUSPECT" : "");
	return offOk && !ramStatic;
}

} // namespace

// The owner's exact shape: a FRESH plugin instance each time (cold power-on from scratch, not
// a tone switch inside an already-warmed-up session), tone selected, then played almost right
// away - "on the first notes", not after settling in. `settleSeconds` is deliberately short.
bool runColdRepro(int group, int number, const char *toneLabel, double settleSeconds,
                   D110Core::StuckPolicy forcePolicy) {
	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0); // boot time - unavoidable, not part of what's being varied
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("%s: did not start\n", toneLabel);
		return true;
	}
	proc.setForwardNotesToFirmware(true);
	// Control: force the policy AFTER setPoweredOn() already configured its own default, to
	// isolate whether this session's La32Ramps changes caused this or it predates them.
	proc.getCore().setStuckPolicy(forcePolicy);
	if (forcePolicy != D110Core::StuckPolicy::La32Ramps)
		proc.getCore().setLa32StatusMode(0);

	std::printf("=== %s, fresh instance, %.1fs settle, policy=%s ===\n", toneLabel, settleSeconds,
	            forcePolicy == D110Core::StuckPolicy::La32Ramps ? "La32Ramps" : "La32Stub");
	{
		auto ram = snapshot(proc);
		std::printf("  before any note - slot table (busy/context):");
		for (int n = 0; n < D110Core::kNumHardwareVoices; ++n) {
			const uint8_t busy = ram[D110Core::kSlotStateTable + 2 * n];
			if (busy == D110Core::kSlotBusyValue || busy == D110Core::kSlotBusyValueAlt)
				std::printf(" [slot=%d busy=%02x ctx=%02x]", n, busy, ram[D110Core::kSlotContextTable + 2 * n]);
		}
		std::printf("\n");
	}
	setPartTone(proc, 0, group, number);
	render(proc, settleSeconds);

	bool bad = false;
	for (int i = 0; i < 12; ++i) {
		char label[64];
		std::snprintf(label, sizeof(label), "note %d (#%d)", 60 + (i % 5), i);
		if (!testOneNote(proc, 60 + (i % 5), label)) bad = true;
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return bad;
}

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// Control: does forcing the OLD default (La32Stub) on a fresh cold-started instance also
	// fail the very first note's release, or is this specific to today's La32Ramps changes?
	bool anyBad = false;
	if (runColdRepro(1, 0, "b01 Fantasy", 0.3, D110Core::StuckPolicy::La32Stub)) anyBad = true;
	std::printf("\n");
	if (runColdRepro(1, 0, "b01 Fantasy", 0.3, D110Core::StuckPolicy::La32Ramps)) anyBad = true;
	std::printf("\n");

	std::printf("%s\n", anyBad ? "SUSPECT STUCK/FROZEN" : "no stuck-note/freeze signature");
	return anyBad ? 1 : 0;
}
