// The user suspects a factory-reset D-110 sounds like its panning is "stuck" or wrong.
// factory_defaults.md documents the REAL hardware's factory pan as fanned per part (Part 1
// 3 right, Part 2 3 left, Part 3 1 right, ...), measured off the real firmware's own
// display - not centered. This checks two things directly: does a real factory reset
// leave the RAM panpot byte at the expected value (PatchTemp.panpot, RAM 0x2000+16*n+9,
// munt's Structures.h: 0-14, 0=R 14=L, 7=center), and does that byte actually reach the
// sound engine as a real stereo shift (not stuck centered, not clipped hard to one side).
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// RMS of a channel over a rendered note, L and R separately.
struct Balance { float l = 0, r = 0; };

// `part` is deliberately unused: the part is addressed by the MIDI channel it listens on,
// never by index, so that this measures the same path a host would drive. It stays in the
// signature because every printed row is labelled by part number, and dropping it would put
// the caller in charge of remembering the channel-to-part offset twice over.
Balance measureBalance(D110AudioProcessor &proc, int part, int channel) {
	juce::MidiBuffer midi;
	// Whatever patch the part is already holding is exactly what we want to hear - this
	// checks the FACTORY state, so selecting a program here would overwrite the thing under
	// test. Part N listens on channel N+1 at factory defaults, which is what `channel` is.
	midi.addEvent(juce::MidiMessage::noteOn(channel, 60, 0.9f), 0);

	// Paced against the real clock, not a bare block count: the emulated machine runs on
	// its own thread at real time, so a tight loop with no sleep_until can blast through this
	// whole 1.2s measurement in milliseconds of wall time - not enough for the firmware to
	// have dispatched the note-on at all. Measured directly: without pacing, which parts read
	// as "silent" changes from run to run, purely on OS scheduling luck, and one run showed 6
	// of 8 parts silent. See probe-must-wait-on-the-clock in this project's own notes.
	juce::AudioBuffer<float> buffer(2, kBlock);
	double sumL = 0, sumR = 0;
	int n = 0;
	const int blocks = int(1.2 * kSampleRate / kBlock);
	auto next = std::chrono::steady_clock::now();
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		proc.processBlock(buffer, midi);
		midi.clear();
		next += std::chrono::microseconds(int64_t(double(kBlock) / kSampleRate * 1e6));
		std::this_thread::sleep_until(next);
		// The first quarter is skipped because the attack is the one part of the note whose
		// two channels are NOT in a fixed ratio - a transient rings up at its own rate per
		// side, and averaging it in moves the balance for reasons that are not panning.
		if (b > blocks / 4) {
			for (int i = 0; i < buffer.getNumSamples(); ++i) {
				sumL += double(buffer.getSample(0, i)) * buffer.getSample(0, i);
				sumR += double(buffer.getSample(1, i)) * buffer.getSample(1, i);
				++n;
			}
		}
	}
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(channel, 60), 0);
	for (int b = 0; b < int(0.5 * kSampleRate / kBlock); ++b) {
		buffer.clear();
		proc.processBlock(buffer, off);
		off.clear();
		next += std::chrono::microseconds(int64_t(double(kBlock) / kSampleRate * 1e6));
		std::this_thread::sleep_until(next);
	}
	(void)part;
	// RMS rather than peak: panning is a ratio held across the whole note, and a peak is one
	// sample that can land on either side by luck of where the waveform happened to be.
	return { float(std::sqrt(sumL / n)), float(std::sqrt(sumR / n)) };
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	// The firmware's OWN reset, driven through the panel exactly as a hand would do it, not a
	// block of defaults written into RAM from outside. Anything written from outside would
	// prove only that this file and factory_defaults.md agree with each other.
	std::printf("\nforcing a genuine factory reset (real hardware procedure)...\n");
	proc.getCore().factoryReset();
	// Three seconds BEFORE the poll, not just after: isResetting() does not go true the
	// instant factoryReset() returns, so polling straight away reads "already finished" off a
	// reset that has not started and the whole measurement lands on the pre-reset state.
	std::this_thread::sleep_for(std::chrono::seconds(3));
	while (proc.getCore().isResetting()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
	std::this_thread::sleep_for(std::chrono::seconds(2));
	// The reset changed the firmware's RAM; the sound engine still holds the old patch until
	// the mirror is pushed across. Without this the bytes below and the audio below would be
	// read out of two different machines' worth of state.
	proc.getCore().resyncMirror();
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());

	// Two independent readings per row on purpose. The RAM byte says what the firmware
	// believes; the L/R RMS says what actually came out. A row where the byte is right and
	// the audio is centred means the panpot never reached the engine - which is the failure
	// the user described, and it is invisible if only one of the two columns is printed.
	std::printf("\npart  RAM panpot byte  expected (factory_defaults.md)   measured L/R RMS\n");
	// As the instrument's own display writes it: "3>" is 3 to the right, "<3" 3 to the left.
	static const char *expected[8] = {"3>", "<3", "1>", "<1", "5>", "<5", "7>", "<7"};
	for (int part = 0; part < 8; ++part) {
		const uint8_t panByte = ram[0x2000 + 16 * part + 9];
		const Balance bal = measureBalance(proc, part, part + 2); // channel = part+2 (1-based ch, part1->ch2)
		const float total = bal.l + bal.r;
		// A silent part reports 50% - dead centre - and that reads exactly like a correct
		// centred pan. Check it against the L and R figures printed beside it, which are the
		// only thing that tells "centred" from "nothing came out at all".
		const float pct = total > 1e-6f ? 100.0f * bal.r / total : 50.0f;
		std::printf("  %d      %3d (0x%02X)      %-4s                          L=%.4f R=%.4f  (%.0f%% right)\n",
		            part + 1, panByte, panByte, expected[part], bal.l, bal.r, pct);
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
