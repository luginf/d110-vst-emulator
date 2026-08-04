// How much CPU does raising mt32emu's usePartialCount actually cost? Renders a fixed number
// of real audio blocks back-to-back with NO artificial real-time pacing (unlike every other
// probe in this project, which sleeps between blocks to feed the firmware at real MIDI speed -
// here that would only hide the number we want), under a worst-case load (every part playing,
// as many notes overlapping as the test can keep fed) at several partial counts, and reports
// wall-clock render time per block vs. the real-time budget (512 samples @ 44100Hz = 11.6ms).
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockBudgetMs = 1000.0 * kBlock / kSampleRate;

void renderBlocksPaced(D110AudioProcessor &proc, int blocks, juce::MidiBuffer *first = nullptr) {
	// Only the WARM-UP (boot, tone setup, note dispatch) needs real pacing so the firmware's
	// own real-time thread keeps up; the timed section below deliberately has none.
	juce::AudioBuffer<float> audio(2, kBlock);
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}
void render(D110AudioProcessor &proc, double seconds) {
	renderBlocksPaced(proc, int(seconds * kSampleRate / kBlock));
}

void setPartTone(D110AudioProcessor &proc, int part, int group, int number) {
	proc.sendTimbreTempParam(part, 0, uint8_t(group));
	proc.sendTimbreTempParam(part, 1, uint8_t(number));
	proc.sendTimbreTempParam(part, 5, 2); // POLY3, matches the earlier measurement
}
} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not start\n");
		return 1;
	}
	proc.setForwardNotesToFirmware(true);
	std::printf("partials at engine: %u\n", proc.enginePartialCount());

	// Fill every one of the 8 voice parts with the 4-partial Fantasy tone (worst case per
	// note) and hammer all of them with fast, overlapping notes at once - about as dense as
	// this instrument's own 9-part structure allows, so the DSP cost measured is a genuine
	// ceiling, not a lucky-case average.
	for (int p = 0; p < 8; ++p) setPartTone(proc, p, 1, 0);
	render(proc, 1.0);

	juce::AudioBuffer<float> audio(2, kBlock);
	int step = 0;
	auto feedNotes = [&](juce::MidiBuffer &midi) {
		for (int p = 0; p < 8; ++p) {
			const int note = 48 + p * 3 + (step % 2) * 2;
			midi.addEvent(juce::MidiMessage::noteOn(2 + p, note, 0.9f), p * 2);
			if (step > 0) {
				const int prevNote = 48 + p * 3 + ((step + 1) % 2) * 2;
				midi.addEvent(juce::MidiMessage::noteOff(2 + p, prevNote), p * 2 + 1);
			}
		}
		++step;
	};

	// Warm-up under real pacing so the firmware actually dispatches these before the timed
	// section starts measuring steady-state render cost, not cold-start allocation.
	for (int i = 0; i < 20; ++i) {
		audio.clear();
		juce::MidiBuffer midi;
		feedNotes(midi);
		proc.processBlock(audio, midi);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
	std::printf("active partials going into timed section: %d\n", proc.engineActivePartials());

	// Timed section: NO sleeping, back-to-back processBlock calls, pure DSP wall-clock cost.
	constexpr int kTimedBlocks = 2000; // ~46s of audio at this sample rate/block size
	const auto start = std::chrono::steady_clock::now();
	int peakPartials = 0;
	for (int i = 0; i < kTimedBlocks; ++i) {
		audio.clear();
		juce::MidiBuffer midi;
		if (i % 2 == 0) feedNotes(midi); // keep refreshing the overlap every other block
		proc.processBlock(audio, midi);
		peakPartials = std::max(peakPartials, proc.engineActivePartials());
	}
	const auto end = std::chrono::steady_clock::now();
	const double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
	const double perBlockMs = totalMs / kTimedBlocks;
	const double realtimeBudgetMs = kTimedBlocks * kBlockBudgetMs;

	std::printf("\npeak active partials during timed section: %d (of %u)\n", peakPartials,
	            proc.enginePartialCount());
	std::printf("%d blocks rendered in %.1fms (%.4fms/block, budget %.4fms/block)\n",
	            kTimedBlocks, totalMs, perBlockMs, kBlockBudgetMs);
	std::printf("CPU load: %.2f%% of one real-time audio thread (%.1fms of DSP work per %.1fms of real time)\n",
	            100.0 * totalMs / realtimeBudgetMs, totalMs, realtimeBudgetMs);

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
