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

// Paced against the real clock, not against a block count. The emulated machine runs on its
// own thread and takes MIDI at the wire's rate, so processBlock returning is no evidence
// that the firmware got anywhere: tens of thousands of iterations pass in seconds while the
// instrument has seen a fraction of what was sent. Counting iterations has already produced
// both a false "broken" and a false "works" in this project, so the loop sleeps until the
// wall-clock deadline of each block instead.
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
		// Block 20 is roughly 230ms in: long enough that the firmware has certainly taken
		// the note-on and allocated its slots, early enough that the rest of the window is
		// all aftermath - which is the part this probe is here to photograph.
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
	// The control board boots on its own timer. isRunning() would go true well before the
	// tables below mean anything, so the wait is a flat nine seconds of real time rather
	// than a poll - a snapshot taken mid-boot reads as garbage that looks like data.
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	// The "before" copy is what makes the diff at the end trustworthy: it is the only way to
	// tell a byte the note changed from a byte that was already sitting there after boot.
	std::vector<uint8_t> before(D110Core::kRamSize, 0);
	proc.getCore().getRam(before.data());
	dump(before.data(), 0x2DC0, 64, "BEFORE: edc0[] busy/idle table");

	proc.getCore().setPcSampling(true);
	// Routed THROUGH the firmware, not handed straight to the sound engine. What the control
	// board does with the note IS the measurement here; bypassing it would photograph
	// nothing.
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
	// Not from the writeup - dumped on the chance that ee80[] runs further than assumed.
	// Labelled a guess in the output on purpose, so a later reader does not quote this row
	// back as an established address the way the documented ones can be quoted.
	dump(after.data(), 0x2F00, 64, "AFTER: ee80[] (word span guess)");

	// The named dumps only show tables somebody already suspected. This sweep is the check
	// against that: any byte the note moved anywhere in the window shows up here, including
	// in an array nobody has identified yet.
	std::printf("\n=== changed bytes 0x2D00-0x3000 ===\n");
	for (int i = 0x2D00; i < 0x3000; ++i)
		if (before[i] != after[i])
			std::printf("  %04X: %02X -> %02X\n", i, before[i], after[i]);

	// A histogram, not a trace: the freeze is the point, and a frozen firmware collapses onto
	// a few addresses. Which addresses says which loop it is spinning in - and therefore what
	// it is waiting for from the LA32 that MAME does not emulate. Anything other than the
	// known 29E9/29EE pair means it stalled somewhere new and the reading needs redoing.
	const auto top = proc.getCore().topPcs(5);
	std::printf("\ntop PCs after the note (should show the 29E9/29EE stall):\n");
	for (const auto &h : top) std::printf("  PC %04X  %llu\n", h.pc, (unsigned long long)h.hits);

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
