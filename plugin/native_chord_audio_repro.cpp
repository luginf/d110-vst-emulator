// The honest reproduction of the user's report: real audio through real mt32emu, a longer
// and denser sequence of fast notes and real overlapping chords than the earlier probes
// tried, checking whether the output goes silent and STAYS silent (matching "sound froze")
// rather than trusting a raw PC-sample comparison, which turned out to give false positives
// on a legitimately busy dispatch loop that revisits some addresses very often.
#include "Source/native/D110CoreNative.h"

#include <mt32emu/mt32emu.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
} // namespace

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const char *dataDir = "C:/Program Files/Common Files/VST3/D-110 Data";
	const char *nvramDir = argc > 1 ? argv[1] : "C:/temp/claude/mame_factory_reset_nvram";

	auto firmware = loadFile(std::string(dataDir) + "/d-110.v1.10.ic19.bin");
	auto presets = loadFile(std::string(dataDir) + "/r15179873-lh5310-97.ic12.bin");
	auto waveIc7 = loadFile(std::string(dataDir) + "/r15179878.ic7.bin");
	auto waveIc8 = loadFile(std::string(dataDir) + "/r15179880.ic8.bin");
	auto boss = loadFile(std::string(dataDir) + "/r15179879.ic6.bin");

	std::vector<uint8_t> controlRom(firmware);
	controlRom.insert(controlRom.end(), presets.begin(), presets.end());
	std::vector<uint8_t> pcmRom(waveIc8);
	pcmRom.insert(pcmRom.end(), waveIc7.begin(), waveIc7.end());

	MT32Emu::ArrayFile controlFile(controlRom.data(), controlRom.size());
	MT32Emu::ArrayFile pcmFile(pcmRom.data(), pcmRom.size());
	const MT32Emu::ROMImage *controlImage = MT32Emu::ROMImage::makeROMImage(&controlFile);
	const MT32Emu::ROMImage *pcmImage = MT32Emu::ROMImage::makeROMImage(&pcmFile);

	auto synth = std::make_unique<MT32Emu::Synth>();
	if (!boss.empty()) synth->setBossReverbROM(boss.data(), MT32Emu::Bit32u(boss.size()));
	if (!synth->open(*controlImage, *pcmImage, MT32Emu::DEFAULT_MAX_PARTIALS,
	                  MT32Emu::AnalogOutputMode_COARSE, false)) {
		std::fprintf(stderr, "Synth::open failed\n");
		return 1;
	}
	const double sampleRate = 44100.0;
	MT32Emu::SampleRateConverter src(*synth, sampleRate, MT32Emu::SamplerateConversionQuality_GOOD);

	D110CoreNative core;
	if (!core.start(dataDir, nvramDir)) return 1;
	core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Ramps);

	std::vector<float> audio;
	std::vector<double> rmsPerSecond;
	auto renderSeconds = [&](double seconds) {
		const int totalFrames = int(seconds * sampleRate);
		const int block = 512;
		std::vector<float> scratch(size_t(block) * 2);
		int done = 0;
		double sumSq = 0.0;
		int sumCount = 0;
		int nextSecondBoundary = int(sampleRate);
		while (done < totalFrames) {
			const int n = std::min(block, totalFrames - done);
			core.runForSeconds(double(n) / sampleRate);

			MT32Emu::Bit8u sysex[256];
			while (const int len = core.popSysex(sysex))
				synth->playSysexNow(sysex, MT32Emu::Bit32u(len));
			D110CoreNative::NoteEvent ev;
			while (core.popNoteEvent(ev)) {
				if (ev.part > 8) continue;
				synth->playMsgOnPart(MT32Emu::Bit8u(ev.part), ev.on ? 0x9 : 0x8,
				                     MT32Emu::Bit8u(ev.note), MT32Emu::Bit8u(ev.velocity));
			}

			src.getOutputSamples(scratch.data(), (unsigned int)n);
			for (int i = 0; i < n; ++i) {
				const float l = scratch[size_t(i) * 2], r = scratch[size_t(i) * 2 + 1];
				sumSq += double(l) * l + double(r) * r;
				++sumCount;
			}
			done += n;
			if (done >= nextSecondBoundary || done >= totalFrames) {
				rmsPerSecond.push_back(std::sqrt(sumSq / std::max(1, sumCount * 2)));
				sumSq = 0.0; sumCount = 0;
				nextSecondBoundary += int(sampleRate);
			}
		}
	};

	std::printf("warming up (9s)...\n");
	renderSeconds(9.0);
	rmsPerSecond.clear();

	std::printf("dense mixed notes + real chords for 20s...\n");
	// Fast individual notes, densely packed (matches "playing fast").
	for (int i = 0; i < 40; ++i) {
		const uint8_t n = uint8_t(48 + (i % 24));
		const uint8_t on[3] = { 0x91, n, 100 };
		core.pushMidi(on, 3);
		renderSeconds(0.05);
		const uint8_t off[3] = { 0x81, n, 0 };
		core.pushMidi(off, 3);
		renderSeconds(0.03);
	}
	// Real overlapping chords, several in a row, held and released (matches "several chords").
	const uint8_t chordNotes[4] = { 48, 52, 55, 60 };
	for (int chord = 0; chord < 15; ++chord) {
		uint8_t bytes[12];
		for (int i = 0; i < 4; ++i) { bytes[i*3]=0x91; bytes[i*3+1]=uint8_t(chordNotes[i]+(chord%12)); bytes[i*3+2]=100; }
		core.pushMidi(bytes, 12);
		renderSeconds(0.4);
		for (int i = 0; i < 4; ++i) { bytes[i*3]=0x81; bytes[i*3+1]=uint8_t(chordNotes[i]+(chord%12)); bytes[i*3+2]=0; }
		core.pushMidi(bytes, 12);
		renderSeconds(0.3);
	}

	std::printf("\n--- RMS per second (0.0 for several seconds in a row = frozen) ---\n");
	int consecutiveSilent = 0, maxConsecutiveSilent = 0;
	for (size_t i = 0; i < rmsPerSecond.size(); ++i) {
		std::printf("  s%2zu: %.5f\n", i, rmsPerSecond[i]);
		if (rmsPerSecond[i] < 1e-5) { ++consecutiveSilent; maxConsecutiveSilent = std::max(maxConsecutiveSilent, consecutiveSilent); }
		else consecutiveSilent = 0;
	}

	// After all that activity, play one final, isolated note and confirm the instrument
	// still responds - the direct test of "did it actually freeze for good".
	std::printf("\nfinal recovery check: one more note after all that...\n");
	const uint8_t finalOn[3] = { 0x91, 67, 100 };
	core.pushMidi(finalOn, 3);
	const size_t before = audio.size();
	(void)before;
	double sumSq = 0.0; int cnt = 0;
	{
		std::vector<float> scratch(512 * 2);
		for (int b = 0; b < int(1.0 * sampleRate / 512); ++b) {
			core.runForSeconds(512.0 / sampleRate);
			MT32Emu::Bit8u sysex[256];
			while (const int len = core.popSysex(sysex)) synth->playSysexNow(sysex, MT32Emu::Bit32u(len));
			D110CoreNative::NoteEvent ev;
			while (core.popNoteEvent(ev)) {
				if (ev.part > 8) continue;
				synth->playMsgOnPart(MT32Emu::Bit8u(ev.part), ev.on ? 0x9 : 0x8, MT32Emu::Bit8u(ev.note), MT32Emu::Bit8u(ev.velocity));
			}
			src.getOutputSamples(scratch.data(), 512);
			for (int i = 0; i < 512; ++i) { const float l=scratch[size_t(i)*2]; sumSq += double(l)*l; ++cnt; }
		}
	}
	const double finalRms = std::sqrt(sumSq / std::max(1, cnt));
	std::printf("final note RMS: %.5f  %s\n", finalRms, finalRms > 1e-4 ? "*** still alive ***" : "SILENT - frozen for good");

	std::printf("\nmax consecutive silent seconds during the stress sequence: %d\n", maxConsecutiveSilent);
	std::printf("%s\n", (maxConsecutiveSilent >= 3 || finalRms <= 1e-4) ? "PROBLEM CONFIRMED" : "no freeze reproduced here");

	core.stop();
	return 0;
}
