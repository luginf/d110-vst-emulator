// Standalone smoke test: load D-110 ROM files, play a short sequence of notes,
// render the result to a 16-bit stereo WAV file for listening.
#include <cstdio>
#include <cstdint>
#include <vector>
#include <mt32emu/mt32emu.h>

using namespace MT32Emu;

static void writeWavHeader(FILE *f, uint32_t dataBytes, uint32_t sampleRate, uint16_t channels) {
	uint32_t byteRate = sampleRate * channels * 2;
	uint16_t blockAlign = channels * 2;
	uint32_t chunkSize = 36 + dataBytes;

	fwrite("RIFF", 1, 4, f);
	fwrite(&chunkSize, 4, 1, f);
	fwrite("WAVE", 1, 4, f);

	fwrite("fmt ", 1, 4, f);
	uint32_t fmtSize = 16;
	fwrite(&fmtSize, 4, 1, f);
	uint16_t audioFormat = 1; // PCM
	fwrite(&audioFormat, 2, 1, f);
	fwrite(&channels, 2, 1, f);
	fwrite(&sampleRate, 4, 1, f);
	fwrite(&byteRate, 4, 1, f);
	fwrite(&blockAlign, 2, 1, f);
	uint16_t bitsPerSample = 16;
	fwrite(&bitsPerSample, 2, 1, f);

	fwrite("data", 1, 4, f);
	fwrite(&dataBytes, 4, 1, f);
}

// mt32emu packs a short MIDI message as: bits 0-7 = status byte (code<<4 | channel),
// bits 8-15 = data1, bits 16-23 = data2. Building it via shifts avoids getting the byte order backwards.
static uint32_t makeMsg(uint8_t status, uint8_t data1 = 0, uint8_t data2 = 0) {
	return static_cast<uint32_t>(status) | (static_cast<uint32_t>(data1) << 8) | (static_cast<uint32_t>(data2) << 16);
}

int main(int argc, char **argv) {
	if (argc < 4) {
		std::fprintf(stderr, "Usage: %s <control_rom> <pcm_rom> <output.wav>\n", argv[0]);
		return 1;
	}
	const char *controlRomPath = argv[1];
	const char *pcmRomPath = argv[2];
	const char *outWavPath = argv[3];

	FileStream controlRomFile;
	if (!controlRomFile.open(controlRomPath)) {
		std::fprintf(stderr, "Failed to open control ROM: %s\n", controlRomPath);
		return 1;
	}
	FileStream pcmRomFile;
	if (!pcmRomFile.open(pcmRomPath)) {
		std::fprintf(stderr, "Failed to open PCM ROM: %s\n", pcmRomPath);
		return 1;
	}

	const ROMImage *controlROMImage = ROMImage::makeROMImage(&controlRomFile);
	const ROMImage *pcmROMImage = ROMImage::makeROMImage(&pcmRomFile);

	if (controlROMImage->getROMInfo() == NULL) {
		std::fprintf(stderr, "Control ROM not recognised (unknown size/checksum)\n");
		return 1;
	}
	if (pcmROMImage->getROMInfo() == NULL) {
		std::fprintf(stderr, "PCM ROM not recognised (unknown size/checksum)\n");
		return 1;
	}
	std::printf("Control ROM recognised as: %s\n", controlROMImage->getROMInfo()->description);
	std::printf("PCM ROM recognised as: %s\n", pcmROMImage->getROMInfo()->description);

	Synth synth;
	if (!synth.open(*controlROMImage, *pcmROMImage)) {
		std::fprintf(stderr, "Synth failed to open (ROMs recognised but incompatible/broken?)\n");
		return 1;
	}

	uint32_t sampleRate = synth.getStereoOutputSampleRate();
	std::printf("Synth opened OK. Output sample rate: %u Hz\n", sampleRate);

	std::vector<int16_t> buffer;
	const uint32_t framesPerChunk = 1024;
	std::vector<int16_t> chunk(framesPerChunk * 2);

	auto renderSeconds = [&](double seconds) {
		uint64_t totalFrames = static_cast<uint64_t>(seconds * sampleRate);
		uint64_t rendered = 0;
		while (rendered < totalFrames) {
			uint32_t framesNow = static_cast<uint32_t>(
				(totalFrames - rendered) < framesPerChunk ? (totalFrames - rendered) : framesPerChunk);
			synth.render(chunk.data(), framesNow);
			buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + framesNow * 2);
			rendered += framesNow;
		}
	};

	// Default factory chanAssign maps Part 1 to MIDI channel 2 (0-based channel index 1),
	// not channel 1 - a well-known MT-32/D-110 quirk. Channel nibble 1 below = "MIDI channel 2".
	const uint8_t channel = 1;
	synth.playMsg(makeMsg(0xC0 | channel, 0)); // Program change, program 0
	renderSeconds(0.1);

	synth.playMsg(makeMsg(0x90 | channel, 60, 64)); // Note on, C4
	renderSeconds(0.5);
	synth.playMsg(makeMsg(0x90 | channel, 64, 64)); // Note on, E4
	renderSeconds(0.5);
	synth.playMsg(makeMsg(0x90 | channel, 67, 64)); // Note on, G4
	renderSeconds(1.0);

	synth.playMsg(makeMsg(0x80 | channel, 60, 64)); // Note off, C4
	synth.playMsg(makeMsg(0x80 | channel, 64, 64)); // Note off, E4
	synth.playMsg(makeMsg(0x80 | channel, 67, 64)); // Note off, G4
	renderSeconds(1.5); // let release/reverb tail ring out

	synth.close();
	ROMImage::freeROMImage(controlROMImage);
	ROMImage::freeROMImage(pcmROMImage);

	FILE *out = std::fopen(outWavPath, "wb");
	if (!out) {
		std::fprintf(stderr, "Failed to open output file: %s\n", outWavPath);
		return 1;
	}
	uint32_t dataBytes = static_cast<uint32_t>(buffer.size() * sizeof(int16_t));
	writeWavHeader(out, dataBytes, sampleRate, 2);
	std::fwrite(buffer.data(), sizeof(int16_t), buffer.size(), out);
	std::fclose(out);

	std::printf("Wrote %.2f seconds of audio to %s\n", buffer.size() / 2.0 / sampleRate, outWavPath);
	return 0;
}
