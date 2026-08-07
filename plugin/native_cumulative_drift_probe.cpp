// Follow-up to native_retrigger_silence_probe.cpp and native_long_session_stress_probe.cpp.
// The tone-swap test in the former showed part 8's failures were fully explained by its
// factory tone (Strings 3, slow attack) not having time to swell within a 25ms hammer hold -
// giving it part 1's fast-attack AcouPiano2 made the failures vanish completely (0/200).
// But Alan confirmed the REAL problem also hits fast, percussive tones directly (AcouPiano1),
// so attack time isn't the (whole) story for his actual case. That leaves the other pattern
// already seen in native_long_session_stress_probe.cpp: a late-session run made even part 1
// (clean in every earlier test) fail 100% of the time, with nothing about part 1 itself
// changed - consistent with something drifting over a REAL, cumulative session rather than
// being tied to a specific part or tone.
//
// This probe isolates that cleanly: ONE part, ONE fast-attack tone (the default AcouPiano2,
// never changed), played for a long simulated session, holds long enough that a working piano
// note is unambiguously audible (attack is near-instant) - watching for exactly when, if ever,
// failures start, and what engine/firmware state looks like right before/after the first one.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <random>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 2; // Part 1, factory channel map, default tone AcouPiano2

float renderPeak(D110AudioProcessor &proc, double seconds, const juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	const int blocks = juce::jmax(1, int(seconds * kSampleRate / kBlock));
	float peak = 0.0f;
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		peak = juce::jmax(peak, audio.getMagnitude(0, kBlock));
	}
	return peak;
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

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	renderPeak(proc, 9.0);
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) {
		std::printf("did not start\n");
		return 1;
	}
	proc.setForwardNotesToFirmware(true);

	std::mt19937 rng(777);
	// 60-150ms hold: comfortably enough for AcouPiano2's near-instant attack to be audible,
	// nowhere near the ~25ms window that made Strings 3 look "silent" for an unrelated reason.
	std::uniform_real_distribution<double> holdDist(0.06, 0.15);
	std::uniform_real_distribution<double> gapDist(0.0, 0.08);
	std::uniform_int_distribution<int> noteDist(48, 72);

	constexpr int kTotalNotes = 8000;
	int silentCount = 0;
	int firstSilentIndex = -1;
	int consecutiveSilent = 0, maxConsecutiveSilent = 0;

	for (int i = 0; i < kTotalNotes; ++i) {
		const int note = noteDist(rng);
		const double hold = holdDist(rng);
		const double gap = gapDist(rng);

		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, note, (juce::uint8)100), 0);
		const float peak = renderPeak(proc, hold, &on);

		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kChannel, note), 0);
		renderPeak(proc, gap, &off);

		const bool silent = peak < 0.001f;
		if (silent) {
			++silentCount;
			++consecutiveSilent;
			maxConsecutiveSilent = juce::jmax(maxConsecutiveSilent, consecutiveSilent);
			if (firstSilentIndex < 0) {
				firstSilentIndex = i;
				std::printf("FIRST SILENT at note %d: note=%d hold=%.3f peak=%.5f busy=%d "
				            "activePartials=%d/%u\n",
				            i, note, hold, peak, busySlots(proc), proc.engineActivePartials(),
				            proc.enginePartialCount());
			}
		} else {
			consecutiveSilent = 0;
		}

		if (i > 0 && i % 1000 == 0)
			std::printf("... note %5d: silentSoFar=%d (%.2f%%) busy=%d activePartials=%d/%u\n", i,
			            silentCount, 100.0 * silentCount / (i + 1), busySlots(proc),
			            proc.engineActivePartials(), proc.enginePartialCount());
	}

	std::printf("\ntotal=%d silent=%d (%.2f%%) firstSilentIndex=%d maxConsecutiveSilent=%d\n", kTotalNotes,
	            silentCount, 100.0 * silentCount / kTotalNotes, firstSilentIndex, maxConsecutiveSilent);
	return silentCount > 0 ? 1 : 0;
}
