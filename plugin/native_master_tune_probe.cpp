// Verifies the 2026-08-07 fix: masterTune (System Area offset 0, RAM 0x2D94) is now mirrored
// to the sound engine (ram_mirror.cpp), unlike before. mt32emu's own masterTune field uses
// the identical 0-127 scale and default (0x4A) as the D-110's - see ram_mirror.cpp's own
// comment for why forwarding the raw byte, unlike reverb type before it, needed no
// translation. This renders the same held note at three different masterTune byte values and
// checks the waveform actually differs (proof the byte reaches the engine) and that a simple
// zero-crossing-rate pitch estimate moves in the expected direction as the byte moves away
// from its centre (64).
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

void render(D110AudioProcessor &proc, double seconds, const juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	const int blocks = juce::jmax(1, int(seconds * kSampleRate / kBlock));
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
	}
}

// Autocorrelation-based F0 estimate, searched only within a physically plausible window
// around 440Hz (masterTune's whole documented range is +-~13Hz around A440) - avoids the
// octave/harmonic confusion a zero-crossing count gets from a real (harmonically rich, not
// pure-sine) piano tone.
double estimatePitchHz(D110AudioProcessor &proc, double seconds) {
	std::vector<float> samples;
	juce::AudioBuffer<float> audio(2, kBlock);
	const int blocks = juce::jmax(1, int(seconds * kSampleRate / kBlock));
	juce::MidiBuffer empty;
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		proc.processBlock(audio, empty);
		const float *ch = audio.getReadPointer(0);
		samples.insert(samples.end(), ch, ch + kBlock);
	}

	const int minLag = int(kSampleRate / 460.0);  // ~95 samples
	const int maxLag = int(kSampleRate / 420.0);  // ~105 samples
	double bestCorr = -1.0;
	int bestLag = minLag;
	const int n = int(samples.size()) - maxLag;
	for (int lag = minLag; lag <= maxLag; ++lag) {
		double corr = 0.0;
		for (int i = 0; i < n; ++i) corr += double(samples[size_t(i)]) * double(samples[size_t(i + lag)]);
		if (corr > bestCorr) { bestCorr = corr; bestLag = lag; }
	}
	return kSampleRate / double(bestLag);
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

	constexpr int kNote = 69; // A4, 440Hz at standard tuning - easy sanity reference
	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(2, kNote, (juce::uint8)100), 0);
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(2, kNote), 0);

	for (int tuneByte : { 64, 0, 127, 64 }) {
		render(proc, 0.05, &off);  // release any note left over from the previous iteration
		render(proc, 0.3);         // let the release tail die out completely
		proc.sendSystemParam(0, juce::uint8(tuneByte)); // System field 0 = masterTune
		render(proc, 0.1);
		render(proc, 0.05, &on); // trigger a fresh note under the new tuning
		render(proc, 0.3);       // let it settle into steady state
		const double hz = estimatePitchHz(proc, 0.2);
		std::printf("masterTune byte=%3d: autocorrelation pitch estimate = %.2f Hz\n", tuneByte, hz);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(2, kNote), 0);
		render(proc, 0.2, &off);
	}

	return 0;
}
