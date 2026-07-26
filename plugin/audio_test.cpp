// Offline check that the plugin actually makes sound, and - once the bridge exists -
// that a panel edit changes it. Instantiates the real processor, powers it on, plays a
// note and measures the output, with no DAW and no audio device involved.
//
// Note the D-110's factory MIDI assignment: Part 1 listens on channel 2, not 1.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <thread>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Renders `seconds` of audio and returns the peak absolute sample.
float render(D110AudioProcessor &proc, juce::MidiBuffer &midi, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const int blocks = int(seconds * kSampleRate / kBlock);
	float peak = 0.0f;
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		proc.processBlock(buffer, midi);
		midi.clear(); // events only belong in the first block
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));
	}
	return peak;
}

// Fundamental frequency of a sustained note, by counting zero crossings of the
// low-passed signal. Crude, but more than precise enough to see a fine-tune shift.
float measurePitch(D110AudioProcessor &proc) {
	juce::MidiBuffer midi;
	midi.addEvent(juce::MidiMessage::noteOn(2, 60, 0.9f), 0);

	juce::AudioBuffer<float> buffer(2, kBlock);
	std::vector<float> tail;
	const int blocks = int(1.5 * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		proc.processBlock(buffer, midi);
		midi.clear();
		// Skip the attack; measure the steady part.
		if (b > blocks / 3)
			for (int i = 0; i < buffer.getNumSamples(); ++i)
				tail.push_back(buffer.getSample(0, i));
	}

	// Release the note so the next measurement starts clean.
	{
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
		juce::AudioBuffer<float> flush(2, kBlock);
		for (int b = 0; b < int(1.0 * kSampleRate / kBlock); ++b) {
			flush.clear();
			proc.processBlock(flush, off);
			off.clear();
		}
	}

	if (tail.size() < 4096) return 0.0f;

	// Autocorrelation, not zero crossings. Counting crossings is easy to write but it
	// latches onto whichever harmonic dominates, so a change in timbre reads as an
	// octave jump that never happened - which is exactly how an earlier version of this
	// test produced "+460 cents" and "+1180 cents" from edits that could not have done
	// that. Autocorrelation over a window that covers several periods, with the peak
	// picked among true local maxima and then refined by a parabolic fit, is stable
	// enough to trust to a couple of cents.
	const int n = int(std::min<size_t>(tail.size(), 32768));
	double mean = 0.0;
	for (int i = 0; i < n; ++i) mean += tail[(size_t)i];
	mean /= n;

	const int minLag = int(kSampleRate / 1000.0); // 1000 Hz ceiling
	const int maxLag = int(kSampleRate / 50.0);   // 50 Hz floor
	std::vector<double> corr((size_t)maxLag + 1, 0.0);
	for (int lag = minLag; lag <= maxLag; ++lag) {
		double sum = 0.0;
		for (int i = 0; i + lag < n; ++i)
			sum += (tail[(size_t)i] - mean) * (tail[(size_t)(i + lag)] - mean);
		corr[(size_t)lag] = sum / (n - lag);
	}

	int best = -1;
	for (int lag = minLag + 1; lag < maxLag; ++lag) {
		if (corr[(size_t)lag] <= corr[(size_t)lag - 1]) continue;
		if (corr[(size_t)lag] < corr[(size_t)lag + 1]) continue;
		if (best < 0 || corr[(size_t)lag] > corr[(size_t)best]) best = lag;
	}
	if (best <= 0) return 0.0f;

	// Sub-sample refinement, so a few cents of shift is actually resolvable.
	const double y0 = corr[(size_t)best - 1], y1 = corr[(size_t)best], y2 = corr[(size_t)best + 1];
	const double denom = 2.0 * (2.0 * y1 - y0 - y2);
	const double offset = (denom != 0.0) ? (y2 - y0) / denom : 0.0;
	return float(kSampleRate / (best + offset));
}

// The display comes off the emulated controller as dots, which is right for drawing but
// hopeless for diagnosis - reading screens by eye out of ASCII art is slow and I have
// already misread one. So load the same character ROM the machine uses and run the
// lookup backwards, turning dots back into text. Diagnostics only; the plugin itself
// never needs this.
std::vector<uint8_t> g_cgrom;

// 'A' identifies the character ROM by content, the same way the synth ROMs are
// recognised - the file's name does not matter.
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
		const auto f = entry.getFile();

		// Normally the only thing in the data folder is the d110 romset archive, and the
		// character generator lives inside it.
		if (f.hasFileExtension("zip")) {
			juce::ZipFile zip(f);
			for (int i = 0; i < zip.getNumEntries(); ++i) {
				const auto *e = zip.getEntry(i);
				if (e == nullptr || e->uncompressedSize != 4096) continue;
				std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(i));
				if (stream == nullptr) continue;
				juce::MemoryBlock data;
				stream->readIntoMemoryBlock(data);
				if (isCgrom(data)) {
					g_cgrom.assign(static_cast<const uint8_t *>(data.getData()),
					               static_cast<const uint8_t *>(data.getData()) + 4096);
					return;
				}
			}
			continue;
		}

		juce::MemoryBlock data;
		if (f.loadFileAsData(data) && isCgrom(data)) {
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

void showLcd(D110AudioProcessor &proc, const char *label) {
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return;
	std::printf("\n%s:\n", label);
	for (int line = 0; line < D110Core::kLines; ++line) {
		std::printf("  [");
		for (int col = 0; col < D110Core::kCols; ++col)
			std::putchar(decodeCell(rows + ((size_t)line * D110Core::kCols + col) * D110Core::kRowsPerChar));
		std::printf("]\n");
	}
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;

	D110AudioProcessor proc;
	std::printf("ROMs loaded : %s\n", proc.isSynthReady() ? "yes" : "NO");
	std::printf("control ROM : %s\n", proc.getControlRomDescription().toRawUTF8());
	std::printf("PCM ROM     : %s\n", proc.getPcmRomDescription().toRawUTF8());
	if (!proc.isSynthReady())
		std::printf("last error  : %s\n", proc.getLastError().toRawUTF8());

	proc.prepareToPlay(kSampleRate, kBlock);

	// Powered off, the unit must be silent.
	{
		juce::MidiBuffer midi;
		midi.addEvent(juce::MidiMessage::noteOn(2, 60, 0.8f), 0);
		const float peak = render(proc, midi, 0.5);
		std::printf("\npowered OFF, note played -> peak %.5f  %s\n", peak,
		            peak < 1.0e-6f ? "(silent, correct)" : "(SHOULD BE SILENT)");
	}

	std::printf("\nswitching POWER on (this also boots the D-110 firmware)...\n");
	proc.setPoweredOn(true);
	// The control board takes a few seconds to come up; the sound engine is ready at once.
	std::this_thread::sleep_for(std::chrono::seconds(6));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	// Start from the factory state every time. Without this the test inherits whatever
	// the previous run left in NVRAM - which is how an earlier version ended up editing
	// a parameter already parked at its limit and reporting "no change".
	std::printf("factory reset (Write/Copy held across a reset, then Enter)...\n");
	proc.getCore().factoryReset();
	while (proc.getCore().isResetting())
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	std::this_thread::sleep_for(std::chrono::seconds(2));
	loadCgrom();
	showLcd(proc, "after factory reset");

	{
		juce::MidiBuffer midi;
		midi.addEvent(juce::MidiMessage::noteOn(2, 60, 0.9f), 0);
		midi.addEvent(juce::MidiMessage::noteOn(2, 64, 0.9f), 8);
		midi.addEvent(juce::MidiMessage::noteOn(2, 67, 0.9f), 16);
		const float peak = render(proc, midi, 2.0);
		std::printf("\npowered ON, chord on ch.2 -> peak %.5f  %s\n", peak,
		            peak > 0.01f ? "*** SOUND ***" : "(silent - something is wrong)");
	}

	// ---- MIDI MESSAGE lamp -------------------------------------------------
	// It indicates MIDI reception, not power: the service notes' test procedure says
	// "check that the MIDI MESSAGE LED is lit" while MIDI is being sent in. So it must
	// be dark when idle and light while notes are arriving/sounding.
	{
		int litWhilePlaying = 0, litWhenIdle = 0;
		juce::AudioBuffer<float> buffer(2, kBlock);
		juce::MidiBuffer midi;
		midi.addEvent(juce::MidiMessage::noteOn(2, 60, 0.9f), 0);
		for (int b = 0; b < 120; ++b) {
			buffer.clear();
			proc.processBlock(buffer, midi);
			midi.clear();
			if (proc.getLcdSnapshot().midiLedOn) ++litWhilePlaying;
		}
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
		for (int b = 0; b < 200; ++b) {
			buffer.clear();
			proc.processBlock(buffer, off);
			off.clear();
			if (b > 120 && proc.getLcdSnapshot().midiLedOn) ++litWhenIdle;
		}
		std::printf("\nMIDI MESSAGE lamp: lit on %d/120 blocks while a note sounds, "
		            "%d/80 once idle  %s\n", litWhilePlaying, litWhenIdle,
		            (litWhilePlaying > 0 && litWhenIdle == 0) ? "*** correct ***"
		                                                      : "(check the wiring)");
	}

	showLcd(proc, "firmware display");

	// ---- the bridge: does an edit made on the PANEL change what we hear? ----
	// Walk the firmware to Timbre Edit / Fine Tune and turn it up, then re-measure the
	// pitch of the same note. Nothing here touches the sound engine directly - the only
	// path is panel button -> firmware -> its parameter RAM -> Roland exclusive -> LA engine.
	{
		auto press = [&proc](int port, int bit, int times) {
			for (int i = 0; i < times; ++i) {
				const int idx = D110Core::buttonIndex(port, bit);
				proc.getCore().setButton(idx, true);
				std::this_thread::sleep_for(std::chrono::milliseconds(130));
				proc.getCore().setButton(idx, false);
				std::this_thread::sleep_for(std::chrono::milliseconds(320));
			}
		};

		// Walk to a known page from a known place: Exit twice guarantees we start at
		// Patch Play whatever the firmware was left showing, since its display state
		// survives in NVRAM between runs.
		press(0, 7, 2); // Exit, Exit
		press(0, 5, 1); // Timbre
		press(1, 7, 1); // Edit
		press(0, 3, 2); // Group + twice -> the Fine Tune page
		showLcd(proc, "display before the edit");

		const uint64_t sysexBefore = proc.getCore().sysexEmitted();
		const float before = measurePitch(proc);
		std::printf("pitch before the edit : %.2f Hz\n", before);

		// Sweep the parameter well away from wherever it was parked. Going up then down
		// would cancel out, so this only goes one way; if it is already at a limit the
		// message count comes out zero and says so.
		std::printf("pressing Number+ x30 on the panel...\n");
		press(0, 1, 30);
		std::this_thread::sleep_for(std::chrono::seconds(1));

		showLcd(proc, "display after the edit");
		std::printf("mirror messages sent  : %llu\n",
		            (unsigned long long)(proc.getCore().sysexEmitted() - sysexBefore));
		const float after = measurePitch(proc);
		std::printf("pitch after the edit  : %.2f Hz\n", after);
		const float cents = (before > 0 && after > 0) ? 1200.0f * std::log2(after / before) : 0.0f;
		std::printf("shift                 : %+.1f cents  %s\n", cents,
		            std::abs(cents) > 5.0f ? "*** THE PANEL CHANGED THE SOUND ***"
		                                   : "(no change - bridge not working)");
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("done\n");
	return 0;
}
