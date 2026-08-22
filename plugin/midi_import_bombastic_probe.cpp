// Alan's report (2026-08-22): importing 2026-08-19_linuxmao_1e.mid, track 7 ("bank I :
// bombastic") carries four SysEx messages at t=0 - three writing custom Tone data (Roland
// addresses 08 78 00 / 08 78 7b / 08 79 76, which map to kRamTones per docs/sysex_address_map.md)
// and one small write to TimbreTemp Part 6 (address 03 00 50 -> kRamTimbreTemp + 5*16,
// writing TimbreGroup=2, TimbreNumber=60) - but no Program Change at all on that channel.
// Played back exactly as our simple Android/desktop MIDI-file players inject them
// (D110AudioProcessor::injectMidiMessage, the same osMidiCollector path real MIDI input uses),
// Part 6 does not audibly become "Bombastic". This probe replays those four exact bytes
// against the real firmware+engine and inspects what actually happened, instead of guessing
// from the Roland spec by hand.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
// docs/factory_defaults.md: "Part 1 listens on channel 2, part 2 on channel 3, ... up to
// part 8 on channel 9" - factory default is Part N -> Channel N+1, so Part 6 (index 5) is
// channel 7, NOT channel 6. The MIDI file itself sends this track's notes on channel 6
// (matching its own sequential Part-N-on-channel-N authoring convention, visible in every
// other track too - "PART 1" on channel 1, "PART 2" on channel 2, etc.) - a convention that
// only lines up with a D-110 whose own SYSTEM channel map has already been reassigned to
// match, which a factory-default boot (what our simple player does) never does.
constexpr int kChannel = 7;

const uint8_t kSysex1[] = {0xf0, 0x41, 0x10, 0x16, 0x12, 0x08, 0x78, 0x00, 0x42, 0x6f, 0x6d, 0x62,
                           0x61, 0x73, 0x74, 0x69, 0x63, 0x20, 0x00, 0x00, 0x0f, 0x00, 0x18, 0x2e,
                           0x0b, 0x01, 0x00, 0x58, 0x43, 0x07, 0x05, 0x00, 0x00, 0x06, 0x00, 0x00,
                           0x25, 0x3a, 0x32, 0x32, 0x32, 0x34, 0x53, 0x00, 0x0c, 0x42, 0x00, 0x0e,
                           0x1b, 0x07, 0x64, 0x64, 0x00, 0x00, 0x00, 0x32, 0x32, 0x32, 0x32, 0x64,
                           0x64, 0x64, 0x64, 0x64, 0x3c, 0x1b, 0x0c, 0x00, 0x0c, 0x00, 0x00, 0x00,
                           0x32, 0x32, 0x32, 0x2b, 0x64, 0x64, 0x64, 0x64, 0x24, 0x36, 0x0b, 0x01,
                           0x01, 0x58, 0x4d, 0x07, 0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x25, 0x3a,
                           0x32, 0x32, 0x32, 0x34, 0x53, 0x00, 0x0c, 0x49, 0x00, 0x0e, 0x1b, 0x07,
                           0x64, 0x64, 0x00, 0x00, 0x00, 0x32, 0x32, 0x32, 0x32, 0x64, 0x64, 0x64,
                           0x64, 0x64, 0x3c, 0x1b, 0x0c, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x32, 0x3c,
                           0xf7};
const uint8_t kSysex2[] = {0xf0, 0x41, 0x10, 0x16, 0x12, 0x08, 0x78, 0x7b, 0x32, 0x32, 0x31, 0x64,
                           0x64, 0x64, 0x64, 0x18, 0x30, 0x0b, 0x01, 0x00, 0x58, 0x15, 0x07, 0x05,
                           0x00, 0x00, 0x02, 0x00, 0x00, 0x17, 0x2d, 0x32, 0x32, 0x32, 0x28, 0x53,
                           0x00, 0x0c, 0x4a, 0x00, 0x0e, 0x1b, 0x07, 0x64, 0x00, 0x00, 0x00, 0x00,
                           0x32, 0x32, 0x32, 0x32, 0x64, 0x64, 0x64, 0x64, 0x64, 0x32, 0x1b, 0x0c,
                           0x00, 0x0c, 0x00, 0x00, 0x00, 0x32, 0x32, 0x32, 0x2b, 0x64, 0x64, 0x64,
                           0x64, 0x24, 0x34, 0x0b, 0x01, 0x01, 0x58, 0x2e, 0x07, 0x05, 0x00, 0x00,
                           0x02, 0x00, 0x00, 0x17, 0x2d, 0x32, 0x32, 0x32, 0x28, 0x53, 0x00, 0x0c,
                           0x4b, 0x00, 0x0e, 0x1b, 0x07, 0x64, 0x64, 0x00, 0x00, 0x00, 0x32, 0x32,
                           0x32, 0x32, 0x64, 0x64, 0x64, 0x64, 0x64, 0x3c, 0x1b, 0x0c, 0x00, 0x0c,
                           0x00, 0x00, 0x00, 0x32, 0x32, 0x32, 0x2f, 0x64, 0x64, 0x64, 0x64, 0x43,
                           0xf7};
const uint8_t kSysex3[] = {0xf0, 0x41, 0x10, 0x16, 0x12, 0x08, 0x79, 0x76, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xf7};
const uint8_t kSysex4[] = {0xf0, 0x41, 0x10, 0x16, 0x12, 0x03, 0x00, 0x50, 0x02, 0x3c, 0x6f, 0xf7};

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

float playAndMeasure(D110AudioProcessor &proc, int note, double hold, double gap) {
	juce::AudioBuffer<float> audio(2, kBlock);
	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(kChannel, note, (juce::uint8)89), 0);
	juce::MidiBuffer empty;
	float peak = 0.0f;
	const int holdBlocks = juce::jmax(1, int(hold * kSampleRate / kBlock));
	for (int b = 0; b < holdBlocks; ++b) {
		audio.clear();
		proc.processBlock(audio, b == 0 ? on : empty);
		peak = juce::jmax(peak, audio.getMagnitude(0, kBlock));
		juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
	}
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(kChannel, note), 0);
	render(proc, gap, &off);
	return peak;
}

void dumpTimbreTemp(D110AudioProcessor &proc, int part, const char *label) {
	std::vector<uint8_t> ram(static_cast<size_t>(D110CoreType::kRamSize));
	proc.getCore().getRam(ram.data());
	const size_t base = size_t(D110CoreType::kRamTimbreTemp) + size_t(part) * 16;
	std::printf("%s: TimbreTemp part %d =", label, part + 1);
	for (int i = 0; i < 16; ++i) std::printf(" %02x", ram[base + i]);
	std::printf("  (group=%d number=%d)\n", ram[base], ram[base + 1]);
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
		std::printf("did not start - check ROMs are discoverable\n");
		return 1;
	}
	std::printf("booted OK\n");

	dumpTimbreTemp(proc, 5, "BEFORE any sysex");

	// Sanity control: channel 2 -> Part 1 is the combo other probes in this codebase already
	// rely on as known-working (native_assign_mode_probe.cpp etc.) - if THIS is silent too,
	// the bug is in this probe's own setup, not in anything about the bombastic sysex.
	{
		juce::AudioBuffer<float> audio(2, kBlock);
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(2, 60, (juce::uint8)90), 0);
		juce::MidiBuffer empty;
		float peak = 0.0f;
		for (int b = 0; b < 20; ++b) {
			audio.clear();
			proc.processBlock(audio, b == 0 ? on : empty);
			peak = juce::jmax(peak, audio.getMagnitude(0, kBlock));
			juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
		}
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
		render(proc, 0.3, &off);
		std::printf("SANITY channel 2 (Part 1, known-good combo): peak=%.4f %s\n", peak,
		            peak >= 0.001f ? "AUDIBLE" : "SILENT");
	}

	// Baseline: does the factory-default Part 6 sound at all, before touching anything?
	float baselinePeak = playAndMeasure(proc, 45, 0.4, 0.1);
	std::printf("baseline note (factory default Part 6): peak=%.4f %s\n", baselinePeak,
	            baselinePeak >= 0.001f ? "AUDIBLE" : "SILENT");

	// Inject the four sysex exactly as the file has them, via the same path
	// injectMidiMessage() uses (osMidiCollector -> processBlock -> handleIncomingMidiMessage).
	for (const auto *sx : {kSysex1, kSysex2, kSysex3, kSysex4}) {
		const int len = sx == kSysex1 ? (int)sizeof(kSysex1)
		                : sx == kSysex2 ? (int)sizeof(kSysex2)
		                : sx == kSysex3 ? (int)sizeof(kSysex3)
		                                : (int)sizeof(kSysex4);
		proc.injectMidiMessage(juce::MidiMessage(sx, len));
	}
	// CC10/CC7 from the file too, for completeness (pan/volume, shouldn't matter for timbre).
	proc.injectMidiMessage(juce::MidiMessage::controllerEvent(kChannel, 10, 72));
	proc.injectMidiMessage(juce::MidiMessage::controllerEvent(kChannel, 7, 43));
	render(proc, 0.5);

	dumpTimbreTemp(proc, 5, "AFTER the four sysex + CC10/CC7");

	float afterPeak = playAndMeasure(proc, 45, 0.4, 0.1);
	std::printf("note after sysex (should be \"Bombastic\"): peak=%.4f %s\n", afterPeak,
	            afterPeak >= 0.001f ? "AUDIBLE" : "SILENT");

	// Control: the SAME address+data (group=2, number=60 at Part 6 index 5), sent through our
	// OWN trusted call (sendTimbreTempParam - what the Parts tab's own tone-picker uses,
	// already verified working by editor_write_probe.cpp) instead of a hand-built raw sysex.
	// If THIS changes the RAM and the raw kSysex4 above didn't, the bug is in how the raw
	// bytes are being constructed/parsed, not in the firmware's DT1 handling itself.
	proc.sendTimbreTempParam(5, 0, 2);
	proc.sendTimbreTempParam(5, 1, 60);
	render(proc, 0.5);
	dumpTimbreTemp(proc, 5, "AFTER sendTimbreTempParam(5,0,2)+(5,1,60) control");

	float controlPeak = playAndMeasure(proc, 45, 0.4, 0.1);
	std::printf("note after control write: peak=%.4f %s\n", controlPeak,
	            controlPeak >= 0.001f ? "AUDIBLE" : "SILENT");

	return 0;
}
