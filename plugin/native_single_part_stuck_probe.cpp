// Alan's exact report (2026-08-06): playing several notes quickly in a row on ONE part,
// only a few get through; toggling Super Mode (which reopens the sound engine - see
// superModeReopenPending in PluginProcessor.cpp's processBlock()) makes them all play again,
// and so does changing that part's patch away and back. Looking at Monitor's MIDI IN log
// while notes are stuck still shows them received, so raw MIDI isn't the loss point - Monitor
// only shows the first of the three tiers (sent / firmware took / engine sounding) this
// project's other polyphony probes already use to localise this kind of complaint (see
// polyphony_test.cpp, multi_part_polyphony_probe.cpp) - so this checks tiers 2 and 3
// specifically, for a single part, across repeated runs and across a Super Mode toggle.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 2; // Part 1, factory channel map

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

// `count` distinct notes back to back, `stepMs` apart, each held `noteMs` then released -
// "several notes in a row", not a held chord.
void playRun(D110AudioProcessor &proc, int count, int stepMs, int noteMs) {
	for (int i = 0; i < count; ++i) {
		const int note = 48 + (i % 13);
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, note, (juce::uint8)100), 0);
		renderBlocks(proc, juce::jmax(1, int(noteMs * kSampleRate / 1000.0 / kBlock)), &on);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kChannel, note), 0);
		const int gapMs = juce::jmax(1, stepMs - noteMs);
		renderBlocks(proc, juce::jmax(1, int(gapMs * kSampleRate / 1000.0 / kBlock)), &off);
	}
}

// SAME pitch repeated as fast as this stepping can go (one block between on and off, one
// block gap) - the scenario noteWatch()'s own code raised as a live concern while reading
// it: releaseContext() only fires on context reuse when the NEW note differs from the one
// the context already held (`ctxNote_[ctx] != value`), so a context reused for the identical
// pitch before its prior instance's release has been detected leaves that prior instance
// with no note-off ever emitted - a partial leaked per retrigger.
void playSameNoteRepeatedly(D110AudioProcessor &proc, int count, int note) {
	for (int i = 0; i < count; ++i) {
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, note, (juce::uint8)100), 0);
		renderBlocks(proc, 1, &on);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kChannel, note), 0);
		renderBlocks(proc, 1, &off);
	}
}

juce::AudioProcessorParameter *findParam(D110AudioProcessor &proc, const juce::String &nameContains) {
	for (auto *p : proc.getParameters())
		if (p->getName(64).containsIgnoreCase(nameContains)) return p;
	return nullptr;
}

void reportRun(const char *label, D110AudioProcessor &proc, uint64_t firmwareBefore, int sent) {
	const uint64_t firmwareAfter = proc.getCore().firmwareNoteOns();
	const int active = proc.engineActivePartials();
	std::printf("%-28s: sent=%d firmwareTook=%llu activePartialsAtRest=%d (of %u)\n", label, sent,
	            (unsigned long long)(firmwareAfter - firmwareBefore), active, proc.enginePartialCount());
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

	constexpr int kNotesPerRun = 60;
	constexpr int kStepMs = 50, kNoteMs = 30; // fast, successive - "plusieurs notes d'affilee"

	uint64_t before = proc.getCore().firmwareNoteOns();
	playRun(proc, kNotesPerRun, kStepMs, kNoteMs);
	render(proc, 1.0); // let releases land before reading "at rest"
	reportRun("run 1", proc, before, kNotesPerRun);

	// Same run again, nothing reset in between - if partials leak (never freed even once
	// notes have released and gone silent), the "at rest" count should creep up rather than
	// return to ~0.
	before = proc.getCore().firmwareNoteOns();
	playRun(proc, kNotesPerRun, kStepMs, kNoteMs);
	render(proc, 1.0);
	reportRun("run 2 (no reset)", proc, before, kNotesPerRun);

	before = proc.getCore().firmwareNoteOns();
	playRun(proc, kNotesPerRun, kStepMs, kNoteMs);
	render(proc, 1.0);
	reportRun("run 3 (no reset)", proc, before, kNotesPerRun);

	// Toggle Super Mode on then off - each toggle reopens the sound engine (see
	// superModeReopenPending), which the owner reports "unsticks" the notes.
	if (auto *p = findParam(proc, "Super Mode")) {
		p->setValueNotifyingHost(1.0f);
		render(proc, 1.0); // let the async reopen land
		p->setValueNotifyingHost(0.0f);
		render(proc, 1.0);
		std::printf("Super Mode toggled on then off; activePartials now=%d\n", proc.engineActivePartials());
	} else {
		std::printf("could not find the Super Mode parameter by name - skipped\n");
	}

	before = proc.getCore().firmwareNoteOns();
	playRun(proc, kNotesPerRun, kStepMs, kNoteMs);
	render(proc, 1.0);
	reportRun("run 4 (after SuperMode toggle)", proc, before, kNotesPerRun);

	// Change the part's patch away and back - the owner reports this also unsticks it.
	proc.selectPatch(1);
	render(proc, 0.3);
	proc.selectPatch(0);
	render(proc, 0.3);
	std::printf("patch changed away and back; activePartials now=%d\n", proc.engineActivePartials());

	before = proc.getCore().firmwareNoteOns();
	playRun(proc, kNotesPerRun, kStepMs, kNoteMs);
	render(proc, 1.0);
	reportRun("run 5 (after patch change)", proc, before, kNotesPerRun);

	// Same pitch, retriggered as fast as possible, many times - the specific edge case in
	// noteWatch()'s own context-reuse logic (see playSameNoteRepeatedly's comment).
	before = proc.getCore().firmwareNoteOns();
	playSameNoteRepeatedly(proc, 80, 60);
	render(proc, 1.0);
	reportRun("run 6 (same pitch retriggered)", proc, before, 80);

	return 0;
}
