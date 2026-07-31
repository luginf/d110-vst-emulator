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

Balance measureBalance(D110AudioProcessor &proc, int part, int channel) {
	juce::MidiBuffer midi;
	// Program Change 0 first, so we are definitely on this part's currently-loaded patch,
	// then a note on that MIDI channel (part N listens on channel N+1, factory default).
	midi.addEvent(juce::MidiMessage::noteOn(channel, 60, 0.9f), 0);

	juce::AudioBuffer<float> buffer(2, kBlock);
	double sumL = 0, sumR = 0;
	int n = 0;
	const int blocks = int(1.2 * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		proc.processBlock(buffer, midi);
		midi.clear();
		if (b > blocks / 4) { // skip the attack
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
	}
	(void)part;
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

	std::printf("\nforcing a genuine factory reset (real hardware procedure)...\n");
	proc.getCore().factoryReset();
	std::this_thread::sleep_for(std::chrono::seconds(3));
	while (proc.getCore().isResetting()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
	std::this_thread::sleep_for(std::chrono::seconds(2));
	proc.getCore().resyncMirror();
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());

	std::printf("\npart  RAM panpot byte  expected (factory_defaults.md)   measured L/R RMS\n");
	static const char *expected[8] = {"3>", "<3", "1>", "<1", "5>", "<5", "7>", "<7"};
	for (int part = 0; part < 8; ++part) {
		const uint8_t panByte = ram[0x2000 + 16 * part + 9];
		const Balance bal = measureBalance(proc, part, part + 2); // channel = part+2 (1-based ch, part1->ch2)
		const float total = bal.l + bal.r;
		const float pct = total > 1e-6f ? 100.0f * bal.r / total : 50.0f;
		std::printf("  %d      %3d (0x%02X)      %-4s                          L=%.4f R=%.4f  (%.0f%% right)\n",
		            part + 1, panByte, panByte, expected[part], bal.l, bal.r, pct);
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
