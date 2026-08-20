// Dumps the D-110's own factory Patch names for every one of the 128 raw Program Change
// slots (0-127), straight from the real firmware: send a real Program Change on Part 1's
// channel, let the firmware and the sound engine's mirror catch up in real time, then read
// the name back via D110AudioProcessor::getEnginePatchName() - the same call the fallback
// LCD snapshot uses. Written for generating a MusE .idf instrument definition from measured
// data rather than copying a patch list from somewhere unverified (Alan's request,
// 2026-08-20) - matches this project's own "measured, not copied" bias (see docs/).
//
// Usage: d110_patch_name_dump_probe [output.tsv]
// Output columns: pc<TAB>bankLetter<TAB>bankPosition<TAB>name
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;
} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const juce::String outPath = argc > 1 ? juce::String(argv[1]) : juce::String();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	std::printf("booting firmware...\n");
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	juce::AudioBuffer<float> block(2, kBlock);
	auto pump = [&](double seconds) {
		const int blocks = int(seconds * kSampleRate / kBlock);
		auto next = std::chrono::steady_clock::now();
		for (int b = 0; b < blocks; ++b) {
			juce::MidiBuffer midi;
			block.clear();
			proc.processBlock(block, midi);
			next += std::chrono::microseconds(int64_t(double(kBlock) / kSampleRate * 1e6));
			std::this_thread::sleep_until(next);
		}
	};

	std::vector<std::string> names(128);
	for (int pc = 0; pc < 128; ++pc) {
		juce::AudioBuffer<float> firstBlock(2, kBlock);
		firstBlock.clear();
		juce::MidiBuffer midi;
		midi.addEvent(juce::MidiMessage::programChange(2, pc), 0);
		proc.processBlock(firstBlock, midi);
		pump(0.35);
		const char *name = proc.getEnginePatchName(0);
		names[static_cast<size_t>(pc)] = name != nullptr ? juce::String(name).trim().toStdString() : "";
		std::printf("  pc=%3d  \"%s\"\n", pc, names[static_cast<size_t>(pc)].c_str());
	}

	if (outPath.isNotEmpty()) {
		std::ofstream out(outPath.toStdString());
		for (int pc = 0; pc < 128; ++pc) {
			const int bankIndex = pc / 64;      // 0 = Bank A, 1 = Bank B
			const int posInBank = pc % 64;      // 0..63
			const char bankLetter = static_cast<char>('A' + bankIndex);
			const int groupDigit = posInBank / 8 + 1; // 1..8
			const int slotDigit = posInBank % 8 + 1;  // 1..8
			out << pc << '\t' << bankLetter << groupDigit << slotDigit << '\t'
			    << names[static_cast<size_t>(pc)] << '\n';
		}
		std::printf("wrote %s\n", outPath.toRawUTF8());
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
