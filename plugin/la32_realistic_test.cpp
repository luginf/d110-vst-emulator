// hang_probe/longrun_test both deliberately hammer the firmware with a 4-note chord
// every ~0.28s to find the breaking point fast - that is what they are for. This test
// asks the practically relevant question instead: with StuckPolicy::La32Stub engaged,
// does the panel survive NORMAL playing - single notes, realistic gaps - for a long time?
// If chord-stress still starves it but this survives, the fix is real progress for actual
// use even though the stress test still finds a limit.
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;
using Lcd = std::vector<juce::uint8>;

Lcd readLcd(D110AudioProcessor &proc) {
	Lcd v(D110Core::kLcdBytes, 0);
	proc.getCore().getLcd(v.data());
	return v;
}

bool buttonStillWorks(D110AudioProcessor &proc) {
	const Lcd before = readLcd(proc);
	auto tap = [&proc](int port, int bit) {
		const int idx = D110Core::buttonIndex(port, bit);
		proc.getCore().setButton(idx, true);
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		proc.getCore().setButton(idx, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
	};
	tap(0, 5); // Timbre
	const Lcd afterTimbre = readLcd(proc);
	tap(0, 7); // Exit
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	return afterTimbre != before;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.getCore().setStuckPolicy(D110Core::StuckPolicy::La32Stub);
	proc.setPoweredOn(true);
	// 12s, not 8s: this session found the only difference between a run that completed 16
	// clean note-cycles and one that died after 2 was a 1s difference in boot-settle time
	// before notes started - testing whether the firmware's OWN boot-time test note (it
	// sends one down its own serial port regardless of host MIDI) racing against the first
	// user note is the actual trigger, not "light load" as first suspected.
	std::this_thread::sleep_for(std::chrono::seconds(12));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");
	proc.setForwardNotesToFirmware(true);

	// One note at a time, held ~350ms, ~150ms gap - a slow melodic line, not a stress test.
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	int block = 0, step = 0;
	double lastCheck = 0.0;
	int died = -1;
	constexpr double kTotalSeconds = 60.0;
	constexpr int kOnBlock = 0, kOffBlock = 30; // ~350ms on at 44100/512

	while (std::chrono::duration<double>(Clock::now() - begin).count() < kTotalSeconds) {
		juce::MidiBuffer midi;
		const int phase = block % 43; // ~500ms period
		if (phase == kOnBlock)
			midi.addEvent(juce::MidiMessage::noteOn(2, 60 + (step % 12), 0.9f), 0);
		else if (phase == kOffBlock) {
			midi.addEvent(juce::MidiMessage::noteOff(2, 60 + (step % 12)), 0);
			++step;
		}
		buffer.clear();
		proc.processBlock(buffer, midi);
		++block;
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);

		const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
		// Non-invasive: just the counters, no button presses - a button check pauses note
		// feeding for over a second (150+400+150+400+250ms) and this session found that
		// interference alone might be why an earlier run of this exact test died fast (2
		// services) while an uninterrupted run of the same note pattern completed 16.
		if (elapsed - lastCheck >= 5.0) {
			lastCheck = elapsed;
			std::printf("  %4.0fs  notes played %d   la32 services %llu   ramGen %llu\n", elapsed,
			            step, (unsigned long long)proc.getCore().la32Services(),
			            (unsigned long long)proc.getCore().ramGeneration());
		}
	}
	// Exactly ONE button check, after all note-feeding has stopped - the real question.
	{
		const bool alive = buttonStillWorks(proc);
		std::printf("  final panel check (notes stopped): %s\n", alive ? "responds" : "*** DEAD ***");
		if (!alive) died = int(kTotalSeconds);
	}

	std::printf("\n=== verdict ===\n");
	if (died < 0)
		std::printf("  survived %d notes over %.0fs of realistic playing with La32Stub engaged\n",
		            step, kTotalSeconds);
	else
		std::printf("  DIED at %ds after %d notes\n", died, step);

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
