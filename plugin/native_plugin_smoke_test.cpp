// The check the user asked for, done here first: does the REAL plugin - D110AudioProcessor,
// compiled against the native backend (D110_NATIVE_CORE), driven by processBlock() exactly
// as a DAW would drive it, real host MIDI in the buffer rather than core.pushMidi() called
// directly - actually boot (real LCD text, not a blank screen) and actually make sound.
// Every earlier probe this session drove D110CoreNative directly with its own manual
// stepping loop; this is the first one that goes through the real processBlock() path, which
// is exactly where the missing runForSeconds() call was found and fixed.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

float renderBlock(D110AudioProcessor &proc, juce::MidiBuffer &midi) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	buffer.clear();
	proc.processBlock(buffer, midi);
	midi.clear();
	float peak = 0.0f;
	for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));
	return peak;
}

float render(D110AudioProcessor &proc, juce::MidiBuffer &midi, double seconds) {
	const int blocks = int(seconds * kSampleRate / kBlock);
	float peak = 0.0f;
	for (int b = 0; b < blocks; ++b) {
		juce::MidiBuffer empty;
		peak = juce::jmax(peak, renderBlock(proc, b == 0 ? midi : empty));
	}
	return peak;
}

// Same character-ROM-backwards-lookup technique as audio_test.cpp, so the LCD can be read as
// text instead of eyeballing dot art.
std::vector<uint8_t> g_cgrom;

bool isCgrom(const juce::MemoryBlock &data) {
	if (data.getSize() != 4096) return false;
	const auto *p = static_cast<const uint8_t *>(data.getData());
	static const uint8_t kA[7] = { 0x0e, 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11 };
	for (int r = 0; r < 7; ++r)
		if ((p[16 * 0x41 + r] & 0x1f) != kA[r]) return false;
	return true;
}

void loadCgrom() {
	const auto dir = D110AudioProcessor::getAutoRomFolder();
	for (const auto &entry : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFiles)) {
		juce::MemoryBlock data;
		if (entry.getFile().loadFileAsData(data) && isCgrom(data)) {
			g_cgrom.assign(static_cast<const uint8_t *>(data.getData()),
			               static_cast<const uint8_t *>(data.getData()) + 4096);
			return;
		}
	}
}

char decodeCell(const uint8_t *rows) {
	if (g_cgrom.empty()) return '?';
	for (int code = 0x20; code < 0x80; ++code) {
		bool same = true;
		for (int r = 0; r < 7; ++r)
			if ((g_cgrom[(size_t)16 * code + r] & 0x1f) != (rows[r] & 0x1f)) { same = false; break; }
		if (same) return char(code);
	}
	bool blank = true;
	for (int r = 0; r < 7; ++r) if (rows[r] & 0x1f) blank = false;
	return blank ? ' ' : '?';
}

std::string lcdText(D110AudioProcessor &proc) {
	uint8_t rows[D110CoreType::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return {};
	std::string s;
	for (int line = 0; line < D110CoreType::kLines; ++line) {
		if (line) s.push_back('/');
		for (int col = 0; col < D110CoreType::kCols; ++col)
			s.push_back(decodeCell(rows + ((size_t)line * D110CoreType::kCols + col) * D110CoreType::kRowsPerChar));
	}
	return s;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	std::printf("ROMs loaded : %s\n", proc.isSynthReady() ? "yes" : "NO");
	proc.prepareToPlay(kSampleRate, kBlock);

	// prepareToPlay() itself calls setPoweredOn(true) (auto-power-on, this project's own
	// standing behaviour) - so by the time it returns, D110CoreNative::start() has already
	// run. What's in question is whether processBlock() actually advances it from there.
	std::printf("firmware running right after prepareToPlay: %s\n",
	            proc.getCore().isRunning() ? "yes" : "NO");

	// Drive it exactly like a host audio thread would: nothing but processBlock() calls,
	// real time NOT slept for - if the native core only advances inside processBlock(), this
	// loop is the only thing that can make 9 emulated seconds happen.
	std::printf("driving processBlock() for 9s of emulated boot time...\n");
	juce::MidiBuffer none;
	render(proc, none, 9.0);

	loadCgrom();
	const std::string boot = lcdText(proc);
	std::printf("LCD after boot: [%s]\n", boot.c_str());
	bool lcdBlank = true;
	for (char c : boot) if (c != ' ' && c != '/') { lcdBlank = false; break; }
	std::printf("%s\n", lcdBlank ? "LCD IS BLANK - still broken" : "LCD has real text - PASS");

	// Part 1 answers on MIDI channel 2 (1-based), same convention as every other probe.
	juce::MidiBuffer chord;
	chord.addEvent(juce::MidiMessage::noteOn(2, 60, 0.9f), 0);
	chord.addEvent(juce::MidiMessage::noteOn(2, 64, 0.9f), 8);
	chord.addEvent(juce::MidiMessage::noteOn(2, 67, 0.9f), 16);
	const float peak = render(proc, chord, 2.0);
	std::printf("\nchord on ch.2 -> peak %.5f  %s\n", peak,
	            peak > 0.01f ? "*** SOUND ***" : "(silent - still broken)");

	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
	off.addEvent(juce::MidiMessage::noteOff(2, 64), 0);
	off.addEvent(juce::MidiMessage::noteOff(2, 67), 0);
	render(proc, off, 1.0);

	std::printf("\nfirmwareNoteOns=%llu firmwareNoteOffs=%llu sysexEmitted=%llu midiDelivered=%llu\n",
	            (unsigned long long)proc.getCore().firmwareNoteOns(),
	            (unsigned long long)proc.getCore().firmwareNoteOffs(),
	            (unsigned long long)proc.getCore().sysexEmitted(),
	            (unsigned long long)proc.getCore().midiDelivered());

	const bool ok = !lcdBlank && peak > 0.01f;
	std::printf("\n%s\n", ok ? "OVERALL PASS" : "OVERALL FAIL");

	proc.setPoweredOn(false);
	proc.releaseResources();
	return ok ? 0 : 1;
}
