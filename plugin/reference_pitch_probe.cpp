// Renders a single reference note (Part 1, channel 2, note 60) straight through
// D110AudioProcessor::processBlock() - no host, no Carla, no JACK - and writes it to a WAV
// file. This is the known-good ground truth for comparing against a recording taken through
// Carla (2026-08-20: Alan reports notes arriving roughly a fifth-to-tritone higher when
// hosting the VST3 in Carla specifically). Sample rate/channel are passed on the command
// line so this can be rendered at the exact rate a comparison JACK setup is using.
//
// Usage: d110_reference_pitch_probe <output.wav> [sampleRate] [note] [channel1based]
#include "Source/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {
constexpr int kBlock = 512;
} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <output.wav> [sampleRate=48000] [note=60] [channel1based=2]\n",
		             argv[0]);
		return 1;
	}
	const juce::File outFile{ juce::String(argv[1]) };
	const double sampleRate = argc > 2 ? atof(argv[2]) : 48000.0;
	const int note = argc > 3 ? atoi(argv[3]) : 60;
	const int channel = argc > 4 ? atoi(argv[4]) : 2;

	D110AudioProcessor proc;
	proc.prepareToPlay(sampleRate, kBlock);
	proc.setPoweredOn(true);
	std::printf("booting firmware...\n");
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	const int totalSeconds = 3;
	const int totalBlocks = int(totalSeconds * sampleRate / kBlock);
	juce::AudioBuffer<float> full(2, totalBlocks * kBlock);
	full.clear();
	juce::AudioBuffer<float> block(2, kBlock);

	const auto begin = std::chrono::steady_clock::now();
	auto next = begin;
	for (int b = 0; b < totalBlocks; ++b) {
		juce::MidiBuffer midi;
		if (b == 4) midi.addEvent(juce::MidiMessage::noteOn(channel, note, (juce::uint8)100), 0);
		if (b == totalBlocks - 20) midi.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
		block.clear();
		proc.processBlock(block, midi);
		full.copyFrom(0, b * kBlock, block, 0, 0, kBlock);
		full.copyFrom(1, b * kBlock, block, 1, 0, kBlock);
		next += std::chrono::microseconds(int64_t(double(kBlock) / sampleRate * 1e6));
		std::this_thread::sleep_until(next);
	}

	outFile.deleteFile();
	juce::WavAudioFormat fmt;
	std::unique_ptr<juce::FileOutputStream> stream(outFile.createOutputStream());
	if (stream == nullptr) {
		std::fprintf(stderr, "could not open %s for writing\n", outFile.getFullPathName().toRawUTF8());
		return 1;
	}
	std::unique_ptr<juce::AudioFormatWriter> writer(
		fmt.createWriterFor(stream.get(), sampleRate, 2, 24, {}, 0));
	if (writer == nullptr) return 1;
	stream.release();
	writer->writeFromAudioSampleBuffer(full, 0, full.getNumSamples());
	std::printf("wrote %s (note %d, channel %d, %.0f Hz)\n", outFile.getFullPathName().toRawUTF8(), note,
	            channel, sampleRate);

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
