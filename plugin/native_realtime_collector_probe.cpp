// The one path NONE of the earlier probes in this investigation ever exercised: every one of
// them injected MIDI directly into processBlock()'s own buffer argument, the way a DAW host
// would - but Alan plays through a real MIDI keyboard into the Standalone build, and the
// on-screen test keyboard uses the exact same route (D110AudioProcessor::injectTestNote, see
// its own comment): osMidiCollector, a juce::MidiMessageCollector that timestamps incoming
// messages against REAL wall-clock time (Time::getMillisecondCounterHiRes()), not simulated
// audio-sample time. That only behaves correctly if processBlock() is actually being called
// at something close to real-time cadence - this probe uses real Thread::sleep between
// blocks (matching Alan's own reported JACK buffer of 512 samples @ 44100Hz, ~11.6ms/block),
// not the fast-forwarded rendering every earlier probe used, specifically to give this timing
// interaction a chance to show up.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 1;

void renderRealTime(D110AudioProcessor &proc, int numBlocks) {
	juce::AudioBuffer<float> audio(2, kBlock);
	juce::MidiBuffer empty;
	for (int b = 0; b < numBlocks; ++b) {
		audio.clear();
		proc.processBlock(audio, empty);
		std::this_thread::sleep_for(std::chrono::microseconds(11610)); // 512/44100s, real time
	}
}

int busySlots(D110AudioProcessor &proc) {
	std::vector<uint8_t> ram(static_cast<size_t>(D110CoreType::kRamSize));
	if (!proc.getCore().getRam(ram.data())) return -1;
	int busy = 0;
	for (int s = 0; s < D110CoreType::kNumHardwareVoices; ++s) {
		const size_t at = size_t(D110CoreType::kSlotStateTable) + size_t(s) * 2;
		const uint8_t st = ram[at];
		if (st == D110CoreType::kSlotBusyValue || st == D110CoreType::kSlotBusyValueAlt) ++busy;
	}
	return busy;
}

// Renders in real time WHILE measuring peak, for exactly as many blocks as `seconds` calls for.
float renderRealTimeMeasured(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> audio(2, kBlock);
	juce::MidiBuffer empty;
	const int blocks = juce::jmax(1, int(seconds * kSampleRate / kBlock));
	float peak = 0.0f;
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		proc.processBlock(audio, empty);
		peak = juce::jmax(peak, audio.getMagnitude(0, kBlock));
		std::this_thread::sleep_for(std::chrono::microseconds(11610));
	}
	return peak;
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const juce::String snapshotPath = argc > 1 ? argv[1] : "/tmp/01.d110snap";

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	renderRealTime(proc, int(9.0 * kSampleRate / kBlock));
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not start\n");
		return 1;
	}
	std::printf("importing snapshot: %s\n", snapshotPath.toRawUTF8());
	proc.importMemorySnapshot(juce::File(snapshotPath));
	renderRealTime(proc, int(9.0 * kSampleRate / kBlock));
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not come back up\n");
		return 1;
	}
	proc.setForwardNotesToFirmware(true);
	proc.selectPatch(0); // patch 01 "Eric"
	renderRealTime(proc, int(3.0 * kSampleRate / kBlock));
	std::printf("selected patch 01, real time so far - switching Part 1 to AcouPiano1 via the "
	            "editor mechanism\n");
	proc.sendTimbreTempParam(0, 0, 0);
	proc.sendTimbreTempParam(0, 1, 0);
	renderRealTime(proc, int(0.5 * kSampleRate / kBlock));

	// Real-time note injection through the SAME path the on-screen keyboard / real MIDI input
	// use - osMidiCollector - not direct processBlock() buffer injection.
	int audible = 0;
	int firstSilent = -1;
	constexpr int kNotes = 25;
	for (int i = 0; i < kNotes; ++i) {
		const int note = 55 + (i % 5) * 3;
		proc.injectTestNote(kChannel, note, 0.7f, true);
		const float peak = renderRealTimeMeasured(proc, 0.08);
		proc.injectTestNote(kChannel, note, 0.0f, false);
		renderRealTime(proc, int(0.03 * kSampleRate / kBlock));
		const bool ok = peak >= 0.001f;
		audible += ok ? 1 : 0;
		if (!ok && firstSilent < 0) firstSilent = i;
		std::printf("  real-time note %2d: %-8s peak=%.5f busySlots=%d activePartials=%d\n", i,
		            ok ? "audible" : "SILENT", peak, busySlots(proc), proc.engineActivePartials());
	}
	std::printf("\nreal-time injectTestNote through osMidiCollector: %d/%d audible, "
	            "firstSilent=%d\n",
	            audible, kNotes, firstSilent);
	return (audible < kNotes) ? 1 : 0;
}
