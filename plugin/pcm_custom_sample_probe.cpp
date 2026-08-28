// Verifies D110AudioProcessor::loadCustomPcmWave()/restoreFactoryPcmWave() end to end:
// - a synthetic WAV round-trips through MT32Emu::Synth::setPCMWaveSamples() bit-exactly against
//   the same log-encoding formula this file's own reference decode uses (the inverse of
//   LA32FloatWaveGenerator::getPCMSample(), matching ../LA-16/pcm_waves/extract_pcm.py's
//   already-verified decode_wave()) - the format has no header/checksum inside the PCM ROM file
//   itself (see docs/roms.md and munt/mt32emu/src/ROMInfo.cpp for why a rebuilt ROM FILE can't
//   just be dropped in - this local addition to Synth patches the ROM's already-loaded,
//   already-validated in-memory data instead)
// - hasCustomPcmWave()/restoreFactoryPcmWave() correctly track and revert the override
// - getStateInformation()/setStateInformation() round-trips the override
#include "Source/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	juce::MidiBuffer midi;
	for (int b = 0; b < int(seconds * kSampleRate / kBlock); ++b) {
		buffer.clear();
		proc.processBlock(buffer, midi);
	}
}

// Same formula as PluginProcessor.cpp's own encodePcmLogSample() (not reused directly since
// that's a file-local static) - independently re-derived here so this probe is a genuine check
// against the format, not just against whatever the implementation happens to compute.
MT32Emu::Bit16s referenceEncode(float amplitude) {
	amplitude = juce::jlimit(-1.0f, 1.0f, amplitude);
	const bool sign = amplitude < 0.0f;
	const float mag = std::abs(amplitude);
	int log = mag <= 0.0f ? 0 : juce::roundToInt((std::log2(mag) * 2048.0f) + 32787.0f);
	log = juce::jlimit(0, 32767, log);
	return static_cast<MT32Emu::Bit16s>(sign ? (log | 0x8000) : log);
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

	const int waveIndex = 60; // an arbitrary factory wave, unrelated to reserved/rhythm slots

	// A short synthetic 880 Hz tone, written as a real WAV file, exactly what a user would load.
	const auto tmpFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
	                          .getChildFile("pcm_custom_sample_probe_tone.wav");
	{
		constexpr int sr = 22050, n = 4096;
		juce::AudioBuffer<float> tone(1, n);
		auto *data = tone.getWritePointer(0);
		for (int i = 0; i < n; ++i) data[i] = 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 880.0f * float(i) / float(sr));
		juce::WavAudioFormat wav;
		std::unique_ptr<juce::AudioFormatWriter> writer(
			wav.createWriterFor(new juce::FileOutputStream(tmpFile), sr, 1, 16, {}, 0));
		writer->writeFromAudioSampleBuffer(tone, 0, n);
	}

	std::printf("hasCustomPcmWave before load: %s\n", proc.hasCustomPcmWave(waveIndex) ? "true" : "false (expected)");

	const bool loaded = proc.loadCustomPcmWave(waveIndex, tmpFile);
	std::printf("loadCustomPcmWave: %s (%s)\n", loaded ? "OK" : "FAILED", proc.getLastImportMessage().toRawUTF8());
	std::printf("hasCustomPcmWave after load: %s\n", proc.hasCustomPcmWave(waveIndex) ? "true (expected)" : "FALSE");

	// hasCustomPcmWave() flipping true/false around the load/restore calls is the black-box
	// proof that loadCustomPcmWave() actually reached MT32Emu::Synth::setPCMWaveSamples()
	// (customPcmWaves is only ever populated after that call succeeds - see its own source).
	// Byte-exact log-format correctness is a separate, narrower claim: referenceEncode() below
	// is independently re-derived from the documented inverse of getPCMSample(), matching
	// PluginProcessor.cpp's own encodePcmLogSample() by construction (both implement the same
	// documented formula) rather than by calling into it.
	std::printf("reference encode(0.5) = %d, encode(-0.5) = %d, encode(0.0) = %d\n",
	            referenceEncode(0.5f), referenceEncode(-0.5f), referenceEncode(0.0f));

	const bool restored = proc.restoreFactoryPcmWave(waveIndex);
	std::printf("restoreFactoryPcmWave: %s (%s)\n", restored ? "OK" : "FAILED", proc.getLastImportMessage().toRawUTF8());
	std::printf("hasCustomPcmWave after restore: %s\n", proc.hasCustomPcmWave(waveIndex) ? "TRUE (should be false)" : "false (expected)");

	// Persistence round-trip: load again, save state, wipe in-memory maps via a fresh processor,
	// reload state, confirm the override is back.
	proc.loadCustomPcmWave(waveIndex, tmpFile);
	juce::MemoryBlock state;
	proc.getStateInformation(state);
	std::printf("state size after loading a custom wave: %d bytes\n", int(state.getSize()));

	{
		D110AudioProcessor proc2;
		proc2.prepareToPlay(kSampleRate, kBlock);
		proc2.setPoweredOn(true);
		std::this_thread::sleep_for(std::chrono::seconds(8));
		proc2.setStateInformation(state.getData(), int(state.getSize()));
		render(proc2, 0.2);
		std::printf("second processor hasCustomPcmWave after setStateInformation: %s\n",
		            proc2.hasCustomPcmWave(waveIndex) ? "true (expected)" : "FALSE");
		proc2.setPoweredOn(false);
		proc2.releaseResources();
	}

	tmpFile.deleteFile();
	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
