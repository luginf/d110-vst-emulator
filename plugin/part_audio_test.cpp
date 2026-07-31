// Reported by ear: on the demo song parts 6 and 7 light their indicators but make no sound.
// The note bridge demonstrably delivers notes to them, so the question is what the SOUND
// ENGINE does with those notes - which this answers by sounding each part on its own and
// measuring the result, instead of listening to nine parts at once.
//
// Two passes, because they fail differently:
//   1. straight into the engine (playMsgOnPart), bypassing the firmware entirely - isolates
//      the engine's own per-part state: timbre, output level, partial reserve;
//   2. through the firmware over its MIDI IN on that part's own channel - the path a player
//      actually uses.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

// Renders `seconds` in real time and returns peak and RMS.
struct Level { float peak = 0; double rms = 0; };

Level renderFor(D110AudioProcessor &proc, double seconds, juce::MidiBuffer first) {
	juce::AudioBuffer<float> block(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	double sumSq = 0;
	int64_t n = 0;
	Level out;
	bool sent = false;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer midi;
		if (!sent) { midi = first; sent = true; }
		block.clear();
		proc.processBlock(block, midi);
		for (int ch = 0; ch < 2; ++ch)
			for (int i = 0; i < kBlock; ++i) {
				const float s = block.getSample(ch, i);
				out.peak = juce::jmax(out.peak, std::abs(s));
				sumSq += double(s) * s;
				++n;
			}
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
	out.rms = n ? std::sqrt(sumSq / double(n)) : 0.0;
	return out;
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

	// What the firmware believes about each part, straight out of its own RAM: the Timbre
	// Temporary block is 16 bytes per part at 0x2000, and Roland's field order puts the
	// timbre group and number first, with output level and panpot at offsets 8 and 9.
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());
	std::printf("\nwhat the FIRMWARE holds per part (Timbre Temporary, RAM 0x2000):\n");
	for (int p = 0; p < 9; ++p) {
		const uint8_t *t = &ram[0x2000 + 16 * p];
		std::printf("  part %d: group %2d  timbre %3d  keyShift %3d  outLevel %3d  pan %2d\n",
		            p + 1, t[0], t[1], t[2], t[8], t[9]);
	}

	// Pass 1: the engine on its own. No firmware involvement at all, so anything silent
	// here is silent because of the engine's per-part state.
	std::printf("\n=== pass 1: straight into the sound engine (playMsgOnPart) ===\n");
	for (int part = 0; part < 9; ++part) {
		const int note = (part == 8) ? 36 : 60; // rhythm keys live low
		proc.playNoteOnPartForTest(uint8_t(part), uint8_t(note), 100);
		const Level lvl = renderFor(proc, 1.6, {});
		proc.playNoteOffOnPartForTest(uint8_t(part), uint8_t(note));
		renderFor(proc, 0.5, {});
		std::printf("  part %d: peak %.5f  rms %.6f  -> %s\n", part + 1, lvl.peak, lvl.rms,
		            lvl.peak > 0.002f ? "SOUNDS" : "*** SILENT ***");
	}

	// Pass 2: through the firmware, on each part's own factory channel (part N answers on
	// channel N+1; the rhythm part on channel 10).
	std::printf("\n=== pass 2: through the firmware, on each part's own MIDI channel ===\n");
	for (int part = 0; part < 9; ++part) {
		const int channel = part + 2 > 10 ? 10 : part + 2;
		const int note = (part == 8) ? 36 : 60;
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(channel, note, 0.8f), 0);
		const Level lvl = renderFor(proc, 1.6, on);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
		renderFor(proc, 0.6, off);
		std::printf("  part %d (ch %2d): peak %.5f  rms %.6f  -> %s\n", part + 1, channel,
		            lvl.peak, lvl.rms, lvl.peak > 0.002f ? "SOUNDS" : "*** SILENT ***");
	}

	// Pass 3: the one that matters. Alone, every part sounds; the report is that during the
	// demo song parts 6 and 7 light their indicators and stay silent. So run the song and
	// sample BOTH sides at once - which parts the engine has sounding, and how many of its
	// partials are busy. A part the engine never sounds while the partial count sits at the
	// ceiling is being starved, not lost.
	std::printf("\n=== pass 3: during the demo song, engine vs. its own partial budget ===\n");
	{
		auto press = [&proc](std::initializer_list<int> idx, int hold, int settle) {
			for (int i : idx) proc.getCore().setButton(i, true);
			std::this_thread::sleep_for(std::chrono::milliseconds(hold));
			for (int i : idx) proc.getCore().setButton(i, false);
			std::this_thread::sleep_for(std::chrono::milliseconds(settle));
		};
		press({D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 500);
		press({D110Core::buttonIndex(1, 0)}, 200, 500);

		int soundedByEngine[9] = {};
		int samples = 0, partialSum = 0, partialPeak = 0;
		const int total = int(proc.enginePartialCount());
		// Both sides, SAME run. Everything before this compared note counts from one run
		// against engine activity from another, which cannot distinguish "the engine
		// refused the note" from "the note never arrived".
		proc.getCore().startNoteLog();
		const auto begin = Clock::now();
		auto next = begin;
		juce::AudioBuffer<float> block(2, kBlock);
		while (std::chrono::duration<double>(Clock::now() - begin).count() < 60.0) {
			juce::MidiBuffer none;
			block.clear();
			proc.processBlock(block, none);
			const uint32_t states = proc.enginePartStates();
			for (int p = 0; p < 9; ++p)
				if (states & (1u << p)) ++soundedByEngine[p];
			const int busy = proc.engineActivePartials();
			partialSum += busy;
			partialPeak = juce::jmax(partialPeak, busy);
			++samples;
			next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
			std::this_thread::sleep_until(next);
		}
		// The decisive pairing: notes DELIVERED to each part against time SOUNDED by each
		// part, from the one run. Delivered-but-never-sounded is the engine refusing;
		// never-delivered is the firmware or the bridge, and the engine is innocent.
		int delivered[9] = {};
		for (const auto &e : proc.getCore().takeNoteLog())
			if (e.on && e.part < 9) ++delivered[e.part];

		std::printf("  engine partials: %d total, peak %d busy, mean %.1f\n", total, partialPeak,
		            samples ? double(partialSum) / samples : 0.0);
		std::printf("  part | note-ons delivered | %% of song sounding | verdict\n");
		for (int p = 0; p < 9; ++p) {
			const double pct = samples ? 100.0 * soundedByEngine[p] / samples : 0.0;
			const char *verdict = (delivered[p] == 0)   ? "never sent a note"
			                    : (soundedByEngine[p] == 0) ? "*** SENT BUT NEVER SOUNDED ***"
			                                                : "ok";
			std::printf("   %4d | %18d | %17.1f%% | %s\n", p + 1, delivered[p], pct, verdict);
		}
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
