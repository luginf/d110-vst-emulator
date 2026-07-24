// Standalone smoke test: load D-110 ROM files, play a short sequence of notes,
// render the result to a 16-bit stereo WAV file for listening.
#include <cstdio>
#include <cstdint>
#include <cstring>
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

// Scans raw bytes for concatenated F0...F7 SysEx messages, e.g. the contents of a bare .syx file.
// Mirrors plugin/Source/PluginProcessor.cpp's extractSysexMessagesFromRawBytes - kept separate
// here since rom_test doesn't link against JUCE.
static std::vector<std::vector<uint8_t>> extractSysexMessagesFromRawBytes(const std::vector<uint8_t> &data) {
	std::vector<std::vector<uint8_t>> messages;
	size_t i = 0;
	while (i < data.size()) {
		if (data[i] == 0xF0) {
			size_t j = i + 1;
			while (j < data.size() && data[j] != 0xF7) ++j;
			if (j < data.size()) {
				messages.emplace_back(data.begin() + static_cast<long>(i), data.begin() + static_cast<long>(j) + 1);
				i = j + 1;
				continue;
			}
			break;
		}
		++i;
	}
	return messages;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		std::fprintf(stderr, "Usage: %s <control_rom> <pcm_rom> <output.wav> [bank.syx]\n", argv[0]);
		return 1;
	}
	const char *controlRomPath = argv[1];
	const char *pcmRomPath = argv[2];
	const char *outWavPath = argv[3];
	const char *syxBankPath = argc >= 5 ? argv[4] : nullptr;

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

	// D-110 factory default chanAssign maps Part 1 to MIDI channel 1 (0-based channel index 0)
	// directly - unlike the MT-32, whose Part 1 defaults to channel 2, a well-known quirk.
	const uint8_t channel = 0;
	synth.playMsg(makeMsg(0xC0 | channel, 0)); // Program change, program 0
	renderSeconds(0.1);

	std::printf("Part 1, Patch 1 instrument before import: %s\n", synth.getPatchName(0));

	if (syxBankPath != nullptr) {
		FILE *syxFile = std::fopen(syxBankPath, "rb");
		if (!syxFile) {
			std::fprintf(stderr, "Failed to open SysEx bank: %s\n", syxBankPath);
			return 1;
		}
		std::fseek(syxFile, 0, SEEK_END);
		long syxSize = std::ftell(syxFile);
		std::fseek(syxFile, 0, SEEK_SET);
		std::vector<uint8_t> syxRaw(static_cast<size_t>(syxSize));
		size_t bytesRead = std::fread(syxRaw.data(), 1, syxRaw.size(), syxFile);
		std::fclose(syxFile);
		syxRaw.resize(bytesRead);

		auto messages = extractSysexMessagesFromRawBytes(syxRaw);
		std::printf("Found %zu SysEx message(s) in %s\n", messages.size(), syxBankPath);

		for (auto &message : messages) synth.playSysex(message.data(), static_cast<uint32_t>(message.size()));
		renderSeconds(0.5); // let the synth's MIDI queue actually drain and apply the writes

		// Read back the raw persistent Timbre memory directly (Group A, timbre slot 0, flat
		// address 131072) bypassing any Patch->Timbre indirection, to check whether the write
		// itself actually landed regardless of which patch (if any) references that slot.
		char timbreNameRaw[11] = {};
		synth.readMemory(131072, 10, reinterpret_cast<Bit8u *>(timbreNameRaw));
		std::printf("Raw Timbre memory at Group A slot 0 after import: \"%s\"\n", timbreNameRaw);

		// Hypothesis: writeMemoryRegion's MR_Timbres case adds a spurious +128 entry offset that
		// readMemoryRegion doesn't apply, so a write meant for Group A slot 0 actually lands 128
		// slots later - at "Memory" group slot 0 (flat addr 131072 + 128*256 = 163840).
		char timbreNameShifted[11] = {};
		synth.readMemory(163840, 10, reinterpret_cast<Bit8u *>(timbreNameShifted));
		std::printf("Raw Timbre memory at Memory-group slot 0 (Group A + 128) after import: \"%s\"\n", timbreNameShifted);

		// Re-select Patch 1 on Part 1 so any change to the timbre memory it references is picked up.
		synth.playMsg(makeMsg(0xC0 | channel, 0));
		renderSeconds(0.1);

		std::printf("Part 1, Patch 1 instrument after import:  %s\n", synth.getPatchName(0));

		// Patch 1 might just not be the patch that references the timbre slot(s) we overwrote -
		// scan all 128 patches on Part 1 to see if any imported name shows up anywhere.
		std::printf("Scanning all 128 patches for changed instrument names...\n");
		for (int program = 0; program < 128; program++) {
			synth.playMsg(makeMsg(0xC0 | channel, static_cast<uint8_t>(program)));
			renderSeconds(0.02);
			const char *instr = synth.getPatchName(0);
			bool looksImported = instr != nullptr && std::strstr(instr, "KURT") != nullptr;
			if (looksImported) std::printf("  Patch %3d: %s  <-- matches imported bank!\n", program + 1, instr);
		}
		std::printf("Scan done.\n");
	}

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
