// Reported: after twenty or thirty seconds of playing, the control board dies - the part
// indicators stop reacting and not one of the sixteen buttons does anything.
//
// Reproducing that needs the real thing, not a convenient approximation: the whole
// processor, processBlock driven at REAL TIME (a DAW cannot render faster than the clock,
// and neither can the emulated control board), and chords rather than single notes. The
// earlier core-only version of this test fed one note at a time and never reproduced it.
//
// Two phases differing in one variable only - whether any MIDI is played - so whichever
// one dies says what is responsible.
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;

using Lcd = std::vector<juce::uint8>;
using Clock = std::chrono::steady_clock;

Lcd readLcd(D110AudioProcessor &proc) {
	Lcd v(D110Core::kLcdBytes, 0);
	proc.getCore().getLcd(v.data());
	return v;
}

// Runs the plugin for `seconds` of WALL CLOCK time, rendering blocks at the rate a host
// would and injecting whatever `midiFor` returns for each block.
template <typename MidiFn, typename Tick>
void runRealTime(D110AudioProcessor &proc, double seconds, MidiFn midiFor, Tick tick) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto begin = Clock::now();
	auto nextBlock = begin;
	int block = 0;

	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer midi = midiFor(block);
		buffer.clear();
		proc.processBlock(buffer, midi);
		++block;

		// Pace to real time, exactly as a host's audio callback does.
		nextBlock += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(nextBlock);

		tick(std::chrono::duration<double>(Clock::now() - begin).count());
	}
}

// Press a button and report whether the display moved. Timbre and Exit swap between two
// clearly different screens, so on a live firmware something always changes.
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

int runPhase(D110AudioProcessor &proc, const char *label, bool withMidi, double seconds) {
	std::printf("\n=== %s ===\n", label);

	int died = -1;
	double lastCheck = 0.0;
	int chordStep = 0;

	auto midiFor = [&](int block) {
		juce::MidiBuffer midi;
		if (!withMidi) return midi;
		// A chord every ~20 blocks, roughly four per second, held then released - denser
		// than one note at a time and much closer to actually playing.
		if (block % 20 == 0) {
			const int root = 48 + (chordStep % 12);
			for (int n : {0, 4, 7, 12})
				midi.addEvent(juce::MidiMessage::noteOn(2, root + n, 0.85f), 0);
		} else if (block % 20 == 14) {
			const int root = 48 + (chordStep % 12);
			for (int n : {0, 4, 7, 12})
				midi.addEvent(juce::MidiMessage::noteOff(2, root + n), 0);
			++chordStep;
		}
		return midi;
	};

	auto tick = [&](double elapsed) {
		if (elapsed - lastCheck < 4.0) return;
		lastCheck = elapsed;
		const bool alive = buttonStillWorks(proc);
		// ramGeneration is the decisive one: it counts snapshots in which the firmware's
		// battery RAM actually changed. If it keeps climbing while the panel is dead, the
		// CPU is alive and merely ignoring the panel; if it freezes, the CPU itself has
		// stopped - a hang, not a mode.
		std::printf("  %4.0fs  panel %s   ramGen %llu   midi delivered %llu dropped %llu\n",
		            elapsed, alive ? "responds" : "*** DEAD ***",
		            (unsigned long long)proc.getCore().ramGeneration(),
		            (unsigned long long)proc.getCore().midiDelivered(),
		            (unsigned long long)proc.getCore().midiDropped());
		if (!alive && died < 0) died = int(elapsed);
	};

	runRealTime(proc, seconds, midiFor, tick);
	return died;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(8));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	const int quietDied = runPhase(proc, "real-time audio, NO MIDI (control)", false, 30.0);

	// Forwarding notes is OFF by default now, precisely because of what this phase shows.
	// Turn it back on to demonstrate the fault it was turned off for.
	proc.setForwardNotesToFirmware(true);
	const int midiDied = runPhase(proc, "chords, notes forwarded to the firmware", true, 30.0);

	// Third phase: play exactly as hard, but keep NOTES away from the control board while
	// the sound engine still gets them. If the panel survives this, the voice path is the
	// culprit and holding notes back is a fix rather than a guess.
	std::printf("\n(restarting the control board, then playing with notes withheld)\n");
	proc.setPoweredOn(false);
	std::this_thread::sleep_for(std::chrono::seconds(2));
	proc.setForwardNotesToFirmware(false);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(8));
	const int filteredDied = runPhase(proc, "chords, notes WITHHELD from the firmware", true, 45.0);

	std::printf("\n=== verdict ===\n");
	auto say = [](int d) {
		return d < 0 ? std::string("survived") : ("DIED at " + std::to_string(d) + "s");
	};
	std::printf("  silent                     : %s\n", say(quietDied).c_str());
	std::printf("  playing, notes forwarded   : %s\n", say(midiDied).c_str());
	std::printf("  playing, notes withheld    : %s\n", say(filteredDied).c_str());
	if (quietDied < 0 && midiDied >= 0 && filteredDied < 0)
		std::printf("  *** NOTES ARE THE TRIGGER; WITHHOLDING THEM KEEPS THE PANEL ALIVE ***\n");
	else if (midiDied >= 0 && filteredDied >= 0)
		std::printf("  it dies even without notes - something else in the stream is to blame\n");

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
