// Alan reported custom PCM samples playing back as "a high-pitched whistle." Root cause
// (confirmed by reading munt/mt32emu/src/TVP.cpp's calcBasePitch()): each PCM wave's
// control-ROM table entry carries a 16-bit pitch CALIBRATION value (pitchLSB/pitchMSB),
// added into the note's pitch computation - Roland tuned this per factory wave so it plays
// back in tune regardless of what pitch it was actually recorded at. loadCustomPcmWave()
// replaces the audio DATA (Synth::setPCMWaveSamples()) but was leaving the wave's ORIGINAL
// calibration untouched, so a replacement inherited whatever pitch offset the factory
// recording needed - almost certainly wrong for an unrelated new recording.
//
// This probe empirically finds the correct "neutral" calibration value - the one that makes
// a custom sample play back at its own recorded pitch when the note played matches the
// sample's own reference tuning - by:
//   1. writing a known-frequency (1000 Hz) synthetic sample into a PCM wave slot
//   2. isolating that wave on Part 1 Partial 1 (only audible partial, full TVA level)
//   3. trying a couple of different pitchOffset values via the new setPCMWavePitchOffset(),
//      measuring the ACTUAL playback frequency each time (zero-crossing counting)
//   4. since this whole pitch system is logarithmic (TVP.cpp's basePitch space, 4096 units
//      per semitone), two measurements let us solve linearly for the exact offset that
//      reproduces 1000 Hz - no need to guess or iterate further
#include "Source/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 32000.0; // render at the engine's own native rate - no host-side
                                        // resampling to blur the frequency measurement
constexpr int kBlock = 512;
constexpr int kChannel = 2; // Part 1, factory channel map
constexpr int kWaveIndex = 60; // arbitrary factory wave, unrelated to rhythm/reserved slots

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

// Estimates frequency from the average spacing between positive-going zero crossings (more
// stable than a plain crossings/duration count - one stray/missed crossing at the edges
// barely moves an averaged-period estimate, whereas it measurably skews a raw count over a
// short window). Captures `seconds` of audio, discarding `settle` seconds at the start so the
// TVA attack ramp and any note-on transient are gone before measuring.
double measureFrequency(D110AudioProcessor &proc, double seconds, double settle) {
	juce::AudioBuffer<float> audio(2, kBlock);
	std::vector<float> captured;
	juce::MidiBuffer empty;
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		proc.processBlock(audio, empty);
		const float *ch = audio.getReadPointer(0);
		for (int i = 0; i < kBlock; ++i) captured.push_back(ch[i]);
	}
	const size_t skip = size_t(settle * kSampleRate);
	std::vector<size_t> crossingIndex;
	for (size_t i = juce::jmax<size_t>(1, skip); i < captured.size(); ++i)
		if (captured[i - 1] < 0.0f && captured[i] >= 0.0f) crossingIndex.push_back(i);
	if (crossingIndex.size() < 2) return 0.0;
	const double totalSamples = double(crossingIndex.back() - crossingIndex.front());
	const double periods = double(crossingIndex.size() - 1);
	return periods * kSampleRate / totalSamples;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");
	proc.setForwardNotesToFirmware(true);

	// A precise 1000 Hz sine, one second long (well within any real wave slot's length),
	// encoded exactly the way loadCustomPcmWave() does internally - built here directly via
	// the same public API rather than round-tripping through a WAV file, since only the
	// resulting in-memory samples matter for this measurement.
	{
		constexpr int n = 32000;
		std::vector<float> tone(n);
		for (int i = 0; i < n; ++i) tone[i] = 0.8f * std::sin(2.0f * juce::MathConstants<float>::pi * 1000.0f * float(i) / float(kSampleRate));
		const auto tmpFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pcm_pitch_calibration_tone.wav");
		{
			juce::AudioBuffer<float> buf(1, n);
			std::copy(tone.begin(), tone.end(), buf.getWritePointer(0));
			juce::WavAudioFormat wav;
			std::unique_ptr<juce::AudioFormatWriter> writer(
				wav.createWriterFor(new juce::FileOutputStream(tmpFile), kSampleRate, 1, 16, {}, 0));
			writer->writeFromAudioSampleBuffer(buf, 0, n);
		}
		const bool loaded = proc.loadCustomPcmWave(kWaveIndex, tmpFile);
		std::printf("loadCustomPcmWave: %s\n", loaded ? "OK" : "FAILED");
		tmpFile.deleteFile();
	}

	// Isolate: Partial 1 = PCM wave kWaveIndex, full level; Partials 2-4 silenced via TVA
	// level 0 (sidesteps needing to know PARTIAL MUTE's exact bit semantics).
	proc.sendToneTempParam(0, 0, 36);           // Partial 1 WG PITCH CORS -> neutral (0 shift)
	proc.sendToneTempParam(0, 1, 50);           // Partial 1 WG PITCH FINE -> neutral (0 cents)
	proc.sendToneTempParam(0, 4, 2);            // Partial 1 WAVEFORM -> PCM bank 1
	proc.sendToneTempParam(0, 5, kWaveIndex);   // Partial 1 PCM wave number
	proc.sendToneTempParam(0, 41, 100);         // Partial 1 TVA LEVEL -> max
	proc.sendToneTempParam(0, 41 + 58, 0);      // Partial 2 TVA LEVEL -> 0
	proc.sendToneTempParam(0, 41 + 116, 0);     // Partial 3 TVA LEVEL -> 0
	proc.sendToneTempParam(0, 41 + 174, 0);     // Partial 4 TVA LEVEL -> 0
	// PCM partials don't go through TVF on real hardware, but reset it wide open anyway in
	// case this build's engine still applies it - the currently-loaded factory tone (a bass
	// patch) could otherwise be low-pass filtering a high test tone without our noticing.
	proc.sendToneTempParam(0, 23, 100);         // Partial 1 TVF FREQ -> fully open
	proc.sendToneTempParam(0, 24, 0);           // Partial 1 TVF RESO -> none
	// The currently-loaded factory tone (SlapBass 2) is exactly the kind of patch that leans
	// on a pronounced PITCH ENVELOPE (the classic slap-bass pitch-drop) - left alone, basePitch
	// keeps moving well past note-on, which reads back as wildly inconsistent frequency
	// measurements depending on how far into that sweep each capture happens to land. Kill it
	// at the source rather than trying to out-wait it.
	proc.sendToneTempParam(0, 8, 0);            // Partial 1 P-ENV DEPTH -> 0
	render(proc, 0.2);

	// Find a structure value that actually passes Partial 1 through (additive, not a
	// ring-modulating pairing that would zero it against a now-silent Partial 2).
	int workingStructure = -1;
	for (int structure = 0; structure <= 12; ++structure) {
		proc.sendToneTempParam(0, 10, juce::uint8(structure));
		render(proc, 0.1);
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, 60, (juce::uint8)100), 0);
		render(proc, 0.1, &on);
		const double freq = measureFrequency(proc, 0.2, 0.05);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kChannel, 60), 0);
		render(proc, 0.1, &off);
		render(proc, 0.1);
		std::printf("structure %d: measured ~%.1f Hz\n", structure, freq);
		if (freq > 50.0) { workingStructure = structure; break; }
	}
	if (workingStructure < 0) {
		std::printf("Could not isolate an audible PCM partial with any structure - aborting.\n");
		return 1;
	}
	std::printf("Using structure %d.\n", workingStructure);

	// Sweep several pitchOffset values, each measured from a fresh note (full note-off +
	// settle time between them so nothing carries over), and fit a log-linear regression
	// rather than solving from just two points - far less sensitive to per-measurement noise.
	auto measureAt = [&](juce::uint32 pitchOffset) -> double {
		proc.setCustomPcmWavePitchOffset(kWaveIndex, int(pitchOffset));
		render(proc, 0.1);
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, 60, (juce::uint8)100), 0);
		render(proc, 0.1, &on);
		const double freq = measureFrequency(proc, 0.3, 0.05);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kChannel, 60), 0);
		render(proc, 0.1, &off);
		render(proc, 0.1);
		return freq;
	};

	const juce::uint32 sweepPoints[] = { 54000, 55000, 56000, 57000, 58000, 59000 };
	std::vector<double> xs, ys; // xs = offset, ys = log2(freq)
	for (juce::uint32 offset : sweepPoints) {
		const double freq = measureAt(offset);
		std::printf("pitchOffset %u -> %.2f Hz\n", offset, freq);
		if (freq > 20.0) {
			xs.push_back(double(offset));
			ys.push_back(std::log2(freq));
		}
	}

	if (xs.size() < 2) {
		std::printf("Not enough valid sweep points to fit a calibration curve - aborting.\n");
		return 1;
	}

	// Ordinary least-squares fit: log2(freq) = a * offset + b
	const double n = double(xs.size());
	double sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
	for (size_t i = 0; i < xs.size(); ++i) {
		sumX += xs[i]; sumY += ys[i]; sumXY += xs[i] * ys[i]; sumXX += xs[i] * xs[i];
	}
	const double a = (n * sumXY - sumX * sumY) / (n * sumXX - sumX * sumX);
	const double b = (sumY - a * sumX) / n;
	std::printf("fit: log2(freq) = %.10f * offset + %.6f  (i.e. %.1f units/octave)\n", a, b, 1.0 / a);

	// Solve for the offset giving exactly 1000 Hz: log2(1000) = a*offset + b
	const double offsetFor1000 = (std::log2(1000.0) - b) / a;
	std::printf("computed pitchOffset for exactly 1000 Hz: %.1f\n", offsetFor1000);

	// Direct check at the computed value, plus how far off the naive "4096 units/semitone"
	// assumption from TVP.cpp's other pitch terms would have been, for comparison.
	const juce::uint32 candidate = juce::uint32(juce::jlimit(0.0, 65535.0, offsetFor1000));
	const double fCheck = measureAt(candidate);
	std::printf("verification: pitchOffset %u -> %.2f Hz (target 1000 Hz)\n", candidate, fCheck);

	// Two competing theoretical derivations, since it turns out this project uses munt's
	// DEFAULT renderer (RendererType_BIT16S, the fixed-point LA32WaveGenerator - confirmed by
	// grepping for any selectRendererType() override in plugin/Source, there is none), NOT
	// LA32FloatWaveGenerator, which is what the first derivation below was actually read from:
	//   - LA32FloatWaveGenerator.cpp: positionDelta = EXP2F(pitch/4096 - 16) * 2048 -> pitch
	//     20480 for positionDelta==1.
	//   - LA32WaveGenerator.cpp (the one actually active): pcmSampleStep (post >>9, 256 units
	//     = 1 sample) = EXP2F(pitch/4096 + 3) -> pitch 57344 for pcmSampleStep==256.
	// Testing both directly rather than trusting either derivation on paper alone.
	const double f20480 = measureAt(20480);
	std::printf("float-renderer-derived pitchOffset 20480 -> %.2f Hz (target 1000 Hz)\n", f20480);
	const double f57344 = measureAt(57344);
	std::printf("bit16s-renderer-derived pitchOffset 57344 -> %.2f Hz (target 1000 Hz)\n", f57344);

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
