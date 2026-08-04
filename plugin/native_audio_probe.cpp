// First real sound out of the native core: boots D110CoreNative, opens a real mt32emu Synth
// exactly the way PluginProcessor.cpp does (same ROM assembly, same playSysexNow()/
// playMsgOnPart() drain loop off popSysex()/popNoteEvent()), plays a few notes, and writes the
// result to a WAV file. Deliberately links neither MAME nor JUCE - only MT32Emu and this
// port's own native/ sources - proving the whole chain (CPU -> RAM mirror -> SysEx -> mt32emu)
// works without the MAME machine anywhere in the loop.
#include "Source/native/D110CoreNative.h"

#include <mt32emu/mt32emu.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> loadFile(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return {};
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Raw 16-bit PCM stereo WAV, no library needed.
void writeWav(const std::string &path, const std::vector<float> &interleavedStereo, double sampleRate) {
	std::ofstream f(path, std::ios::binary);
	const uint32_t numFrames = uint32_t(interleavedStereo.size() / 2);
	const uint32_t dataBytes = numFrames * 2 /*ch*/ * 2 /*bytes/sample*/;
	const uint32_t byteRate = uint32_t(sampleRate) * 2 * 2;
	auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
	auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
	f.write("RIFF", 4); w32(36 + dataBytes); f.write("WAVE", 4);
	f.write("fmt ", 4); w32(16); w16(1); w16(2); w32(uint32_t(sampleRate)); w32(byteRate); w16(4); w16(16);
	f.write("data", 4); w32(dataBytes);
	for (float s : interleavedStereo) {
		int32_t v = int32_t(std::lround(std::max(-1.0f, std::min(1.0f, s)) * 32767.0f));
		w16(int16_t(v));
	}
}

} // namespace

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const char *dataDir = "C:/Program Files/Common Files/VST3/D-110 Data";
	const char *nvramDir = argc > 1 ? argv[1] : "C:/temp/claude/mame_factory_reset_nvram";
	const char *outPath = argc > 2 ? argv[2] : "C:/temp/claude/native_first_sound.wav";

	// Same chip-dump assembly PluginProcessor.cpp's tryAssembleRomsFromChipDumps() does -
	// see that function's own comment for the exact SHA1s/order this was verified against.
	auto firmware = loadFile(std::string(dataDir) + "/d-110.v1.10.ic19.bin");
	auto presets = loadFile(std::string(dataDir) + "/r15179873-lh5310-97.ic12.bin");
	auto waveIc7 = loadFile(std::string(dataDir) + "/r15179878.ic7.bin");
	auto waveIc8 = loadFile(std::string(dataDir) + "/r15179880.ic8.bin");
	auto boss = loadFile(std::string(dataDir) + "/r15179879.ic6.bin");
	if (firmware.empty() || presets.empty() || waveIc7.empty() || waveIc8.empty()) {
		std::fprintf(stderr, "missing ROM file(s) in %s\n", dataDir);
		return 1;
	}

	std::vector<uint8_t> controlRom(firmware);
	controlRom.insert(controlRom.end(), presets.begin(), presets.end());
	std::vector<uint8_t> pcmRom(waveIc8);   // IC8 first, then IC7 - see PluginProcessor.cpp's comment
	pcmRom.insert(pcmRom.end(), waveIc7.begin(), waveIc7.end());

	MT32Emu::ArrayFile controlFile(controlRom.data(), controlRom.size());
	MT32Emu::ArrayFile pcmFile(pcmRom.data(), pcmRom.size());
	const MT32Emu::ROMImage *controlImage = MT32Emu::ROMImage::makeROMImage(&controlFile);
	const MT32Emu::ROMImage *pcmImage = MT32Emu::ROMImage::makeROMImage(&pcmFile);
	if (!controlImage->getROMInfo() || !pcmImage->getROMInfo()) {
		std::fprintf(stderr, "mt32emu did not recognise the assembled control/PCM images\n");
		return 1;
	}
	std::printf("control ROM: %s\n", controlImage->getROMInfo()->description);
	std::printf("PCM ROM:     %s\n", pcmImage->getROMInfo()->description);

	auto synth = std::make_unique<MT32Emu::Synth>();
	if (!boss.empty())
		synth->setBossReverbROM(boss.data(), MT32Emu::Bit32u(boss.size()));
	if (!synth->open(*controlImage, *pcmImage, MT32Emu::DEFAULT_MAX_PARTIALS,
	                  MT32Emu::AnalogOutputMode_COARSE, false)) {
		std::fprintf(stderr, "Synth::open failed\n");
		return 1;
	}

	const double sampleRate = 44100.0;
	MT32Emu::SampleRateConverter src(*synth, sampleRate, MT32Emu::SamplerateConversionQuality_GOOD);

	D110CoreNative core;
	if (!core.start(dataDir, nvramDir)) {
		std::fprintf(stderr, "D110CoreNative::start failed\n");
		return 1;
	}
	core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Stub);

	std::vector<float> audio;
	auto renderSeconds = [&](double seconds) {
		const int totalFrames = int(seconds * sampleRate);
		const int block = 512;
		std::vector<float> scratch(size_t(block) * 2);
		int done = 0;
		while (done < totalFrames) {
			const int n = std::min(block, totalFrames - done);
			// Advance the firmware by exactly as much emulated time as audio we are about
			// to render, same shape as PluginProcessor::processBlock() driving D110Core off
			// the block size - so the RAM mirror and note events stay in step with the
			// audio clock instead of running ahead or behind it.
			core.runForSeconds(double(n) / sampleRate);

			MT32Emu::Bit8u sysex[D110CoreNative::kRamSize < 256 ? 256 : 256];
			while (const int len = core.popSysex(sysex))
				synth->playSysexNow(sysex, MT32Emu::Bit32u(len));

			D110CoreNative::NoteEvent ev;
			while (core.popNoteEvent(ev)) {
				if (ev.part > 8) continue;
				synth->playMsgOnPart(MT32Emu::Bit8u(ev.part), ev.on ? 0x9 : 0x8,
				                     MT32Emu::Bit8u(ev.note), MT32Emu::Bit8u(ev.velocity));
			}

			src.getOutputSamples(scratch.data(), (unsigned int)n);
			audio.insert(audio.end(), scratch.begin(), scratch.begin() + size_t(n) * 2);
			done += n;
		}
	};

	std::printf("warming up (9s)...\n");
	renderSeconds(9.0);

	std::printf("playing a short phrase...\n");
	const uint8_t notes[3] = { 60, 64, 67 };
	for (uint8_t n : notes) {
		const uint8_t on[3] = { 0x91, n, 100 };
		core.pushMidi(on, 3);
		renderSeconds(0.6);
		const uint8_t off[3] = { 0x81, n, 0 };
		core.pushMidi(off, 3);
		renderSeconds(0.3);
	}
	renderSeconds(1.0);

	double sumSquares = 0.0, peak = 0.0;
	for (float s : audio) { sumSquares += double(s) * s; peak = std::max(peak, double(std::fabs(s))); }
	const double rms = std::sqrt(sumSquares / std::max<size_t>(1, audio.size()));

	writeWav(outPath, audio, sampleRate);
	std::printf("\nwrote %s (%.1fs, %zu frames)\n", outPath, audio.size() / 2.0 / sampleRate, audio.size() / 2);
	std::printf("RMS=%.5f peak=%.5f\n", rms, peak);
	std::printf("%s\n", (rms > 1e-4 && peak > 1e-3) ? "PASS (non-silent audio produced)" : "FAIL (silent)");
	return (rms > 1e-4 && peak > 1e-3) ? 0 : 1;
}
