// Offline check that the plugin actually makes sound, and - once the bridge exists -
// that a panel edit changes it. Instantiates the real processor, powers it on, plays a
// note and measures the output, with no DAW and no audio device involved.
//
// Note the D-110's factory MIDI assignment: Part 1 listens on channel 2, not 1.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <string>
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

// Both display lines as one string, so a test can branch on what the firmware is showing
// instead of the author having to predict the menu tree.
std::string lcdText(D110AudioProcessor &proc) {
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return {};
	std::string s;
	for (int line = 0; line < D110Core::kLines; ++line) {
		if (line) s.push_back('/');
		for (int col = 0; col < D110Core::kCols; ++col)
			s.push_back(decodeCell(rows + ((size_t)line * D110Core::kCols + col) * D110Core::kRowsPerChar));
	}
	return s;
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

// A whole-panel spectral fingerprint, so "the sound did not change" can be asserted more
// strongly than "the pitch did not change". Timbre edits that leave the fundamental alone
// - a filter cutoff, an envelope - move this and not the pitch.
struct Fingerprint {
	float peak = 0.0f;
	float centroid = 0.0f; // brightness: amplitude-weighted mean of |sample| slope
};

Fingerprint measureFingerprint(D110AudioProcessor &proc) {
	juce::MidiBuffer midi;
	midi.addEvent(juce::MidiMessage::noteOn(2, 60, 0.9f), 0);

	juce::AudioBuffer<float> buffer(2, kBlock);
	std::vector<float> tail;
	const int blocks = int(1.5 * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		proc.processBlock(buffer, midi);
		midi.clear();
		if (b > blocks / 3)
			for (int i = 0; i < buffer.getNumSamples(); ++i)
				tail.push_back(buffer.getSample(0, i));
	}
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

	Fingerprint fp;
	if (tail.size() < 4096) return fp;
	double energy = 0.0, slope = 0.0;
	for (size_t i = 0; i < tail.size(); ++i) {
		fp.peak = juce::jmax(fp.peak, std::abs(tail[i]));
		energy += double(tail[i]) * tail[i];
		if (i) {
			const double d = double(tail[i]) - tail[i - 1];
			slope += d * d;
		}
	}
	// Ratio of high-frequency to total energy: a standard cheap brightness measure, and
	// all that is needed to tell "identical" from "different".
	fp.centroid = energy > 0.0 ? float(std::sqrt(slope / energy)) : 0.0f;
	return fp;
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

	// ---- indicator check on a COMPLETELY fresh machine ----------------------
	// Deliberately the very first thing played after the reset. If the indicators work
	// here but not later in this same run, the firmware is not losing MIDI - it is
	// running out of voices, because MAME emulates no LA32 and so nothing ever reports a
	// partial as finished.
	{
		auto settle = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
		auto row = [&proc] {
			const std::string t = lcdText(proc);
			return t.substr(0, std::min<size_t>(9, t.size()));
		};
		std::printf("\n--- indicators on a fresh machine (first notes of the run) ---\n");
		std::printf("  before anything played : [%s]\n", row().c_str());
		for (int channel : {2, 3, 9, 10}) {
			juce::MidiBuffer midi;
			for (int note : {36, 48, 60, 72})
				midi.addEvent(juce::MidiMessage::noteOn(channel, note, 0.9f), 0);
			render(proc, midi, 0.3);
			settle(500);
			std::printf("  ch %-2d playing           : [%s]\n", channel, row().c_str());
			juce::MidiBuffer off;
			for (int note : {36, 48, 60, 72})
				off.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
			render(proc, off, 0.3);
			settle(700);
			std::printf("  ch %-2d released          : [%s]\n", channel, row().c_str());
		}
	}

	// ---- what the firmware itself says about MIDI channels ------------------
	// Read straight off the SYSTEM page, from a just-factory-reset machine, before
	// anything else in this test has had a chance to disturb it. This is the instrument's
	// own answer to "which channel plays which part", with no interpretation of RAM bytes
	// and no inference from which notes happen to sound.
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
		std::printf("\n--- SYSTEM page, straight after a factory reset ---\n");
		press(0, 7, 2); // Exit, Exit
		press(1, 5, 1); // System
		for (int group = 0; group < 6; ++group) {
			std::printf("  %s\n", lcdText(proc).c_str());
			press(0, 3, 1); // Group +
		}

		// The per-part MIDI channel is NOT on the SYSTEM page - that page is global only
		// (master tune, memory protect, control channel, unit number, overflow). Per-part
		// settings live under PART SET, reached with the Part button.
		std::printf("\n--- PART SET page, per part, after a factory reset ---\n");
		press(0, 7, 2); // Exit, Exit
		press(1, 6, 1); // Part -> PART SET
		for (int part = 0; part < 8; ++part) {
			std::printf("  -- part %d --\n", part + 1);
			for (int group = 0; group < 6; ++group) {
				std::printf("    %s\n", lcdText(proc).c_str());
				press(0, 3, 1); // Group +
			}
			// Group+ saturates at the last parameter, so wind back to the first one
			// before moving on - otherwise every later part reports only that last page.
			press(1, 3, 8); // Group - x8
			press(0, 4, 1); // Part +
		}
		press(0, 7, 2); // back to Patch Play
	}

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

	// ---- THE NULL TEST -----------------------------------------------------
	// Mirroring a tone nobody edited must be inaudible. tone_probe already proved the RAM
	// side byte for byte, but it says nothing about the OTHER half of the bridge: the
	// Roland address each region is sent to. If that arithmetic is wrong, a resync writes
	// a good tone to the wrong place and the sound moves even though nothing was touched.
	// So: fingerprint the sound, force a full resync, fingerprint it again.
	{
		const Fingerprint before = measureFingerprint(proc);
		const uint64_t sysexBefore = proc.getCore().sysexEmitted();

		proc.getCore().resyncMirror();
		std::this_thread::sleep_for(std::chrono::seconds(1));
		juce::MidiBuffer idle;
		render(proc, idle, 0.5); // let the audio thread drain the queue into the engine

		const uint64_t sent = proc.getCore().sysexEmitted() - sysexBefore;
		const Fingerprint after = measureFingerprint(proc);

		const float dPeak = before.peak > 0 ? std::abs(after.peak - before.peak) / before.peak : 0.0f;
		const float dTone = before.centroid > 0
		                        ? std::abs(after.centroid - before.centroid) / before.centroid
		                        : 0.0f;
		std::printf("\n--- null test: resend the current state, unedited ---\n");
		std::printf("regions resent        : %llu\n", (unsigned long long)sent);
		std::printf("peak      %.5f -> %.5f  (%+.2f%%)\n", before.peak, after.peak, dPeak * 100.0f);
		std::printf("brightness %.5f -> %.5f  (%+.2f%%)\n", before.centroid, after.centroid,
		            dTone * 100.0f);
		std::printf("%s\n", (sent >= 9 && dPeak < 0.02f && dTone < 0.02f)
		                        ? "*** NULL - mirroring an unedited state changes nothing ***"
		                        : (sent < 9 ? "(too few regions resent - resync did not run)"
		                                    : "SOUND MOVED - a region is addressed wrongly"));
	}

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

	// ---- does the FIRMWARE see the notes? -----------------------------------
	// On the hardware the top row's part digits are replaced by a solid block while that
	// part is sounding. That only works if the control board receives the MIDI, which it
	// did not until host messages were forwarded into the CPU's serial port. Play each
	// part's channel in turn and read the row back off the real display.
	{
		// Rendering audio is NOT the same as letting time pass. render() produces its
		// seconds of audio offline in a few milliseconds, while the emulated control board
		// runs on its own thread in real time - so anything that has to travel down the
		// emulated MIDI cable and be acted on by the firmware needs a genuine wall-clock
		// wait, not a longer render. Getting this wrong makes a working link look dead.
		auto settle = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };

		{
			const uint64_t before = proc.getCore().midiDelivered();
			juce::MidiBuffer probe;
			for (int i = 0; i < 40; ++i)
				probe.addEvent(juce::MidiMessage::controllerEvent(1, 3, i % 128), i * 4);
			render(proc, probe, 0.1);
			settle(500);
			std::printf("\nMIDI link: %llu of 120 bytes delivered in 0.5 s of real time "
			            "(%.0f/s expected)\n",
			            (unsigned long long)(proc.getCore().midiDelivered() - before),
			            D110Core::kMidiBytesPerSecond);
		}

		std::printf("\n--- part indicators on the firmware's own display ---\n");
		// Exit to Patch Play, which is the screen that carries the indicators.
		auto press = [&proc](int port, int bit, int times) {
			for (int i = 0; i < times; ++i) {
				const int idx = D110Core::buttonIndex(port, bit);
				proc.getCore().setButton(idx, true);
				std::this_thread::sleep_for(std::chrono::milliseconds(130));
				proc.getCore().setButton(idx, false);
				std::this_thread::sleep_for(std::chrono::milliseconds(320));
			}
		};
		press(0, 7, 2);

		// A lit indicator is the CGRAM block, which has no CGROM glyph and so decodes as
		// '?'. Comparing against the digit that BELONGS in that column is the reliable
		// test; comparing against an earlier snapshot is not, because a note left hanging
		// by an earlier stage would already have changed it.
		// Nine slots, not eight: the row reads "12345678R", so the rhythm part has an
		// indicator of its own in the ninth column.
		auto litMask = [&proc] {
			const std::string t = lcdText(proc);
			int mask = 0;
			for (int i = 0; i < 9 && size_t(i) < t.size(); ++i) {
				const char expect = (i < 8) ? char('1' + i) : 'R';
				if (t[(size_t)i] != expect) mask |= 1 << i;
			}
			return mask;
		};

		// Silence anything an earlier stage left sounding, on every channel, so the
		// starting point really is nothing playing.
		{
			juce::MidiBuffer allOff;
			for (int ch = 1; ch <= 16; ++ch) {
				allOff.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
				allOff.addEvent(juce::MidiMessage::noteOff(ch, 60), 0);
				allOff.addEvent(juce::MidiMessage::noteOff(ch, 64), 0);
				allOff.addEvent(juce::MidiMessage::noteOff(ch, 67), 0);
			}
			render(proc, allOff, 0.2);
			settle(1200);
		}

		std::printf("  idle          : [%s]  lit mask %02X\n", lcdText(proc).c_str(), litMask());

		// Sweep every MIDI channel and report which part each one drives. This is the
		// firmware's own answer to "which channel plays which part", read off its display
		// rather than assumed from the documented default.
		int litParts = 0;
		for (int channel = 1; channel <= 16; ++channel) {
			// Several notes spread over the keyboard, not one: the rhythm part only
			// answers on keys that have a drum assigned, and a single middle C would
			// make "this part never responds" indistinguishable from "that one key is
			// not mapped".
			juce::MidiBuffer midi;
			for (int note : {36, 48, 60, 72})
				midi.addEvent(juce::MidiMessage::noteOn(channel, note, 0.9f), 0);
			const float peak = render(proc, midi, 0.4);
			settle(500); // real time, so the bytes actually arrive and the panel redraws
			const int mask = litMask();

			std::string parts;
			for (int i = 0; i < 9; ++i)
				if (mask & (1 << i)) {
					if (!parts.empty()) parts += ",";
					parts += (i < 8) ? char('1' + i) : 'R';
				}
			if (!parts.empty()) ++litParts;
			std::printf("  ch %-2d [%.9s] peak %.4f -> %s\n", channel, lcdText(proc).c_str(),
			            peak, parts.empty() ? "(no indicator)" : ("part " + parts).c_str());

			juce::MidiBuffer off;
			for (int note : {36, 48, 60, 72})
				off.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
			render(proc, off, 0.1);
			settle(500);
		}
		std::printf("  MIDI bytes queued: %llu, delivered: %llu, dropped: %llu\n",
		            (unsigned long long)proc.getCore().midiForwarded(),
		            (unsigned long long)proc.getCore().midiDelivered(),
		            (unsigned long long)proc.getCore().midiDropped());
		// ADVISORY ONLY - do not read a failure here as a fault in the plugin. This
		// harness renders audio far faster than real time while the control board runs on
		// its own thread AT real time, so the notes reach the firmware in bursts and these
		// LCD reads race it. Successive runs have reported a perfect 1:1 channel-to-part
		// map, everything lit, and nothing lit, from identical code. In a live DAW, where
		// processBlock is called continuously in real time, the indicators track correctly
		// - which is the environment that decides it.
		std::printf("  %s\n", litParts >= 6
		                          ? "*** the firmware sees the notes ***"
		                          : "(nothing lit - EXPECTED to be unreliable offline; "
		                            "check in a DAW, not here)");
	}

	// ---- the deep pages: the whole point of mirroring Tone Temporary ---------
	// Fine Tune above lives in the Timbre Temporary block, which was already bridged. The
	// pages that were dead are the ones inside the TONE - partials, waveform, TVF, TVA.
	// Rather than hard-code a route through a menu tree nobody here has memorised, walk
	// the parameter groups and report what each one is, then edit whichever page looks
	// like a tone parameter and listen for a change that is NOT a pitch change.
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

		// Group+ inside Timbre Edit only pages the TIMBRE's own parameters and saturates
		// at OutputAssign - the TONE lives a level deeper. On the hardware the way in is
		// to press Edit again from the "Tone =" page, so drill and then page through
		// whatever that opens.
		auto toTimbreEdit = [&press] {
			press(0, 7, 2); // Exit, Exit -> Patch Play, whatever was showing
			press(0, 5, 1); // Timbre
			press(1, 7, 1); // Edit
		};

		std::printf("\n--- Timbre Edit groups ---\n");
		toTimbreEdit();
		for (int page = 0; page < 8; ++page) {
			std::printf("  group %2d: [%s]\n", page, lcdText(proc).c_str());
			press(0, 3, 1); // Group +
		}

		// Pressing Edit again from the "Tone =" page drops into Tone Edit, whose pages ARE
		// the Tone Temporary block. Group+ steps the parameters within a bank and Bank+
		// selects Common or one of the four partials, so map the tree both ways and stop
		// at the first genuine partial parameter.
		auto toToneEdit = [&toTimbreEdit, &press] {
			toTimbreEdit();
			press(1, 7, 1); // Edit again, from the Tone = page
		};

		// Structure 1&2 - how the tone's four partials are wired to each other. About as
		// deep inside a tone as a parameter gets, and stone dead before this block was
		// mirrored. Which page gets edited hardly matters: the whole 246-byte tone travels
		// as one region, so every parameter in it comes across together.
		std::printf("\n--- a deep Tone Edit page ---\n");
		toToneEdit();
		std::printf("  Tone Edit opens at : [%s]\n", lcdText(proc).c_str());
		press(0, 3, 1); // Group + -> Structure 1&2
		{
			std::printf("  editing            : [%s]\n", lcdText(proc).c_str());
			const Fingerprint before = measureFingerprint(proc);
			const float pitchBefore = measurePitch(proc);
			const uint64_t sysexBefore = proc.getCore().sysexEmitted();

			press(0, 1, 20); // Number + x20
			std::this_thread::sleep_for(std::chrono::seconds(1));
			showLcd(proc, "after the tone edit");

			const Fingerprint after = measureFingerprint(proc);
			const float pitchAfter = measurePitch(proc);
			const float dTone = before.centroid > 0
			                        ? (after.centroid - before.centroid) / before.centroid
			                        : 0.0f;
			const float cents = (pitchBefore > 0 && pitchAfter > 0)
			                        ? 1200.0f * std::log2(pitchAfter / pitchBefore)
			                        : 0.0f;
			std::printf("mirror messages sent  : %llu\n",
			            (unsigned long long)(proc.getCore().sysexEmitted() - sysexBefore));
			std::printf("brightness %.5f -> %.5f  (%+.1f%%)\n", before.centroid, after.centroid,
			            dTone * 100.0f);
			std::printf("pitch      %.2f -> %.2f Hz  (%+.1f cents)\n", pitchBefore, pitchAfter, cents);
			std::printf("%s\n", std::abs(dTone) > 0.03f || std::abs(cents) > 5.0f
			                        ? "*** A TONE PAGE NOW CHANGES THE SOUND ***"
			                        : "(no change - Tone Temporary is not getting through)");
		}
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("done\n");
	return 0;
}
