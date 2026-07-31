// Renders the D-110's own demo song to a WAV file, so "is there any sound at all" is
// settled by a file that can be listened to rather than by inference. Also renders a
// control: a plain host-MIDI note, which is known to work. If the control has audio and
// the demo song does not, that isolates the fault to the note path, not the audio output.
//
// Usage: d110_demo_wav [output_dir]
#include "Source/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

void press(D110AudioProcessor &proc, std::initializer_list<int> idx, int holdMs, int settleMs) {
	for (int i : idx) proc.getCore().setButton(i, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
	for (int i : idx) proc.getCore().setButton(i, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
}

// Renders in real time (the firmware only runs at real time) into a growing buffer,
// optionally injecting a host MIDI note at the start.
struct RenderResult {
	juce::AudioBuffer<float> audio;
	float peak = 0;
	double rms = 0;
};

RenderResult renderRealTime(D110AudioProcessor &proc, double seconds, bool withHostNote) {
	const int totalBlocks = int(seconds * kSampleRate / kBlock);
	RenderResult out;
	out.audio.setSize(2, totalBlocks * kBlock);
	out.audio.clear();

	juce::AudioBuffer<float> block(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	double sumSq = 0;
	int64_t n = 0;

	// Note-on RATE is what tells "the song is running too fast" apart from "each note is
	// being triggered several times". A D-110 demo song at a normal tempo across a few
	// parts is a few tens of note-ons a second at most; a multiple of that means something
	// is multiplying them, not that the sequencer is racing.
	uint64_t lastCount = proc.getCore().firmwareNoteOns();
	double lastReport = 0.0;

	for (int b = 0; b < totalBlocks; ++b) {
		const double elapsed = double(b) * kBlockSeconds;
		if (elapsed - lastReport >= 1.0) {
			const uint64_t now = proc.getCore().firmwareNoteOns();
			std::printf("    t=%4.0fs   note-ons this second: %llu\n", elapsed,
			            (unsigned long long)(now - lastCount));
			lastCount = now;
			lastReport = elapsed;
		}
		juce::MidiBuffer midi;
		if (withHostNote) {
			if (b == 4) midi.addEvent(juce::MidiMessage::noteOn(2, 60, 0.9f), 0);
			else if (b == 60) midi.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
			else if (b == 90) midi.addEvent(juce::MidiMessage::noteOn(2, 67, 0.9f), 0);
			else if (b == 150) midi.addEvent(juce::MidiMessage::noteOff(2, 67), 0);
		}
		block.clear();
		proc.processBlock(block, midi);
		for (int ch = 0; ch < 2; ++ch) {
			out.audio.copyFrom(ch, b * kBlock, block, ch, 0, kBlock);
			for (int i = 0; i < kBlock; ++i) {
				const float s = block.getSample(ch, i);
				out.peak = juce::jmax(out.peak, std::abs(s));
				sumSq += double(s) * s;
				++n;
			}
		}
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
	out.rms = n ? std::sqrt(sumSq / double(n)) : 0.0;
	return out;
}

bool writeWav(const juce::File &file, const juce::AudioBuffer<float> &buffer) {
	file.deleteFile();
	juce::WavAudioFormat fmt;
	std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
	if (stream == nullptr) return false;
	std::unique_ptr<juce::AudioFormatWriter> writer(
		fmt.createWriterFor(stream.get(), kSampleRate, 2, 24, {}, 0));
	if (writer == nullptr) return false;
	stream.release(); // writer owns it now
	return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const juce::File outDir = (argc > 1)
		? juce::File(juce::String(argv[1]))
		: juce::File::getSpecialLocation(juce::File::userDesktopDirectory);
	outDir.createDirectory();
	std::printf("writing WAVs to: %s\n", outDir.getFullPathName().toRawUTF8());

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	// The demo song plays, but far too fast. Is its tempo coupled to how quickly the LA32
	// stub answers the firmware's voice-allocation wait? The real chip took real time; the
	// stub answers within a fraction of a millisecond. Sweep the delay and watch the
	// note-on rate: if the rate falls as the delay rises, the tempo is paced by that wait
	// and the stub has to model the chip's real timing rather than answer as fast as it can.
	std::printf("\n=== does the LA32 response delay set the tempo? ===\n");
	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 500);
	press(proc, {D110Core::buttonIndex(1, 0)}, 200, 500);

	proc.getCore().startNoteLog();
	{
		const uint64_t onBefore = proc.getCore().firmwareNoteOns();
		const uint64_t offBefore = proc.getCore().firmwareNoteOffs();
		auto seg = renderRealTime(proc, 20.0, false);
		const uint64_t ons = proc.getCore().firmwareNoteOns() - onBefore;
		const uint64_t offs = proc.getCore().firmwareNoteOffs() - offBefore;
		std::printf("  note-ons %llu (%.1f/s), note-offs %llu\n",
		            (unsigned long long)ons, double(ons) / 20.0, (unsigned long long)offs);
		std::printf("  MEAN NOTE LENGTH: %.1f ms  <- a few ms means every note is a click,\n"
		            "                                which sounds like a runaway tempo\n",
		            proc.getCore().meanNoteMs());
		std::printf("  peak %.3f  rms %.5f\n", seg.peak, seg.rms);
		writeWav(outDir.getChildFile("d110_demo_song.wav"), seg.audio);

		std::printf("\n  first note events (ms, part, note, vel, on/off):\n");
		const auto log = proc.getCore().takeNoteLog();
		for (size_t i = 0; i < log.size() && i < 60; ++i)
			std::printf("    %8.1f  part %d  note %3d  vel %3d  %s\n", log[i].ms, log[i].part,
			            log[i].note, log[i].velocity, log[i].on ? "ON" : "off");
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
