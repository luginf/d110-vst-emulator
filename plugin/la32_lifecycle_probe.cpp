// One-shot diagnostic: play EXACTLY ONE note, let the firmware freeze naturally (no
// interrupt answered at all), then dump the raw bytes of every table the LA32 interface
// writeup (docs/la32_interface.md) implicates - 0x2DC0 (edc0[], busy/idle), 0x2E00 (ee00[],
// word-per-voice), 0x2E40 (ee40[], linked list), 0x2EC0 (eec0[], state), 0x2F80 (ef80[],
// flags) - so the StuckPolicy::La32Stub slot scan can be checked against ground truth
// instead of guessed at again.
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
using Clock = std::chrono::steady_clock;

void play(D110AudioProcessor &proc, double seconds, bool noteOn, bool noteOff) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	int block = 0;
	bool sentOn = false, sentOff = false;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer midi;
		if (noteOn && !sentOn) {
			midi.addEvent(juce::MidiMessage::noteOn(2, 60, 0.9f), 0);
			sentOn = true;
		}
		if (noteOff && !sentOff && block == 20) {
			midi.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
			sentOff = true;
		}
		buffer.clear();
		proc.processBlock(buffer, midi);
		++block;
		next += std::chrono::microseconds(int64_t(double(kBlock) / kSampleRate * 1e6));
		std::this_thread::sleep_until(next);
	}
}

void dump(const uint8_t *ram, uint16_t base, int count, const char *label) {
	std::printf("\n%s  (rams 0x%04X, %d bytes)\n", label, base, count);
	for (int i = 0; i < count; ++i) {
		if (i % 16 == 0) std::printf("  %04X:", base + i);
		std::printf(" %02X", ram[base + i]);
		if (i % 16 == 15) std::printf("\n");
	}
	std::printf("\n");
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

	std::vector<uint8_t> before(D110Core::kRamSize, 0);
	proc.getCore().getRam(before.data());
	dump(before.data(), 0x2DC0, 64, "BEFORE: edc0[] busy/idle table");

	proc.getCore().setPcSampling(true);
	proc.setForwardNotesToFirmware(true);
	std::printf("\nplaying a single note...\n");
	play(proc, 3.0, true, true);

	std::vector<uint8_t> after(D110Core::kRamSize, 0);
	proc.getCore().getRam(after.data());
	dump(after.data(), 0x2DC0, 64, "AFTER: edc0[] busy/idle table");
	dump(after.data(), 0x2E00, 64, "AFTER: ee00[] word-per-voice table");
	dump(after.data(), 0x2E40, 64, "AFTER: ee40[] linked list table");
	dump(after.data(), 0x2EC0, 64, "AFTER: eec0[] state table");
	dump(after.data(), 0x2F80, 64, "AFTER: ef80[] flags table");
	dump(after.data(), 0x2F00, 64, "AFTER: ee80[] (word span guess)");

	std::printf("\n=== changed bytes 0x2D00-0x3000 ===\n");
	for (int i = 0x2D00; i < 0x3000; ++i)
		if (before[i] != after[i])
			std::printf("  %04X: %02X -> %02X\n", i, before[i], after[i]);

	const auto top = proc.getCore().topPcs(5);
	std::printf("\ntop PCs after the note (should show the 29E9/29EE stall):\n");
	for (const auto &h : top) std::printf("  PC %04X  %llu\n", h.pc, (unsigned long long)h.hits);

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
