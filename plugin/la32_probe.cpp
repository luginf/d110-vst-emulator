// Discovers the LA32's register interface AS THE FIRMWARE SEES IT.
//
// The D-110's address map claims only a handful of the low I/O page - the bank register,
// the SO register, the two panel scan ports and the LCD. Everything the sound board would
// answer is simply not there, so every access to it lands on MAME's unmapped handler. Turn
// on unmapped logging, play a note, and the addresses that appear are the interface.
//
// The point is NOT to synthesise anything - munt already does the sound. It is to learn
// what the firmware writes and, more importantly, what it reads back, because a read that
// never returns what it expects is what leaves it spinning and the front panel dead.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

// Chords rather than single notes, because the interface is what is being looked for and
// three voices at once make the firmware exercise more of it per cycle. The cycle is 24
// blocks (~280ms) with note-off at block 16 (~185ms held): short and repeated many times,
// so that whatever the sound board is talked to about appears often enough to rise above the
// housekeeping traffic in the log. `chords == false` gives the idle baseline instead.
void play(D110AudioProcessor &proc, double seconds, bool chords) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	int block = 0, step = 0;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer midi;
		if (chords) {
			if (block % 24 == 0)
				for (int n : {0, 4, 7})
					midi.addEvent(juce::MidiMessage::noteOn(2, 48 + (step % 12) + n, 0.85f), 0);
			else if (block % 24 == 16) {
				for (int n : {0, 4, 7})
					midi.addEvent(juce::MidiMessage::noteOff(2, 48 + (step % 12) + n), 0);
				++step;
			}
		}
		buffer.clear();
		proc.processBlock(buffer, midi);
		++block;
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

// Reads and writes are counted apart because they answer different questions. A write says
// the firmware is telling the sound board something; a READ says it is waiting for an answer,
// and an answer that never comes the way it expects is the thing that leaves it spinning. The
// PCs are kept per address so a register can be traced back to the routine that touches it.
struct Access {
	uint64_t reads = 0, writes = 0;
	std::map<std::string, uint64_t> pcs;
};

// MAME writes lines like:
//   'maincpu' (29E9): unmapped program memory read from 0400 & FF
void summarise(const std::vector<std::string> &lines) {
	static const std::regex re(
		R"(\((\w+)\):\s*unmapped\s+\w+\s+memory\s+(read|write)\s+(?:from|to)\s+([0-9A-Fa-f]+))");

	std::map<unsigned, Access> byAddress;
	uint64_t unparsed = 0;
	for (const auto &line : lines) {
		std::smatch m;
		if (!std::regex_search(line, m, re)) {
			++unparsed;
			continue;
		}
		const unsigned addr = std::stoul(m[3].str(), nullptr, 16);
		auto &a = byAddress[addr];
		if (m[2] == "read") ++a.reads; else ++a.writes;
		a.pcs[m[1].str()] += 1;
	}

	std::printf("  %d distinct unmapped addresses (%llu log lines, %llu not parsed)\n",
	            int(byAddress.size()), (unsigned long long)lines.size(),
	            (unsigned long long)unparsed);
	// "Zero addresses found" and "the regex stopped matching MAME's wording" look identical
	// from the outside, and the second one silently reads as the first - as proof that the
	// firmware touches nothing. So when nothing parsed, print the raw lines instead of a
	// clean empty table, and the next reader can see at once which of the two happened.
	if (byAddress.empty() && unparsed) {
		std::printf("\n  first few raw lines, to fix the pattern:\n");
		for (size_t i = 0; i < std::min<size_t>(6, lines.size()); ++i)
			std::printf("    %s", lines[i].c_str());
		return;
	}

	std::printf("\n  %-8s %10s %10s   read from PCs\n", "address", "reads", "writes");
	for (const auto &[addr, a] : byAddress) {
		std::string pcs;
		int shown = 0;
		for (const auto &[pc, n] : a.pcs) {
			if (shown++ >= 4) { pcs += "..."; break; }
			if (!pcs.empty()) pcs += ",";
			pcs += pc;
		}
		std::printf("  %04X     %10llu %10llu   %s\n", addr, (unsigned long long)a.reads,
		            (unsigned long long)a.writes, pcs.c_str());
	}
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	// Logging on BEFORE power, so the boot sequence is captured too - if the firmware probes
	// the sound board while starting up, that is part of the interface and it happens once.
	proc.getCore().setLogUnmapped(true);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	// Baseline: what does an idle machine touch? Anything here is housekeeping and not
	// part of playing a note.
	proc.getCore().takeLogLines();
	play(proc, 6.0, false);
	std::printf("\n=== IDLE ===\n");
	summarise(proc.getCore().takeLogLines());

	// Now play, which is what sends it to the sound board. Notes go THROUGH the firmware -
	// handed straight to the sound engine they would never reach the code under study, and
	// the log would come back looking exactly like the idle baseline above.
	proc.setForwardNotesToFirmware(true);
	play(proc, 8.0, true);
	std::printf("\n=== WHILE PLAYING ===\n");
	summarise(proc.getCore().takeLogLines());

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
