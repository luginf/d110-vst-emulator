// Watches BOTH the dispatch tables (edc0/ee00/ee01/eec0/ef80, rams 0x2DC0-0x2FFF) and the
// completion tables (f3c0/f400/f420/f440/f460/f480, rams 0x33C0-0x34FF) live, merged into
// one chronological log, while StuckPolicy::La32Stub answers the first wait. The question
// this session's realistic-timing test raised: light single-note load died faster and
// completed FEWER voices (2) than heavy chord load (16) - the opposite of what a simple
// "too many notes, not enough voices" explanation predicts. This traces every dispatch and
// every completion event in order to see exactly which contexts get abandoned and where.
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

// The same gentle, realistic pattern as la32_realistic_test.cpp: one note at a time,
// ~350ms held, ~150ms gap - NOT a chord stress test. Being gentle is the whole point: the
// surprise this probe chases is that the LIGHT load fails worse than the heavy one, so a
// stress pattern here would answer a question nobody asked.
//
// The cycle is counted in blocks because that is the only clock the render loop has: 43
// blocks of 512 frames at 44100 is ~500ms, and note-off at block 30 puts the held part at
// ~350ms. Notes walk up a chromatic run (step % 12) so that no two consecutive notes are the
// same pitch - a repeated pitch can be re-triggered on the voice already holding it, and
// then the allocation being traced here never happens at all.
void playRealistic(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	int block = 0, step = 0;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer midi;
		const int phase = block % 43;
		if (phase == 0)
			midi.addEvent(juce::MidiMessage::noteOn(2, 60 + (step % 12), 0.9f), 0);
		else if (phase == 30) {
			midi.addEvent(juce::MidiMessage::noteOff(2, 60 + (step % 12)), 0);
			++step;
		}
		buffer.clear();
		proc.processBlock(buffer, midi);
		++block;
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

// A raw address is unreadable in a trace hundreds of lines long, and worse, the two families
// below are indexed differently - a dispatch table by voice SLOT at stride 2, a completion
// table by CONTEXT at stride 1. Printing "eec0[3]" instead of "2EC6" is what makes the two
// halves of the log comparable at a glance; doing that arithmetic by eye while reading is
// where the earlier confusion between slots and contexts came from.
struct Decoded { const char *array; int index; };

Decoded decode(uint16_t addr) {
	// Dispatch tables (rams 0x2DC0-0x2FFF), all word-per-voice-slot (stride 2, 32 slots),
	// except ee40 which is the doubly-linked free/queue list.
	if (addr >= 0x2DC0 && addr < 0x2E00) return {"edc0", (addr - 0x2DC0) / 2};
	if (addr >= 0x2E00 && addr < 0x2E40) return {"ee00", (addr - 0x2E00) / 2};
	if (addr >= 0x2E40 && addr < 0x2E80) return {"ee40", (addr - 0x2E40)};
	if (addr >= 0x2E80 && addr < 0x2EC0) return {"ee80", (addr - 0x2E80) / 2};
	if (addr >= 0x2EC0 && addr < 0x2F00) return {"eec0", (addr - 0x2EC0) / 2};
	if (addr >= 0x2F00 && addr < 0x2F40) return {"ef00", (addr - 0x2F00) / 2};
	if (addr >= 0x2F80 && addr < 0x2FC0) return {"ef80", (addr - 0x2F80) / 2};
	// Completion tables (rams 0x33C0-0x34FF), all byte-per-CONTEXT (stride 1).
	if (addr >= 0x33C0 && addr < 0x33E0) return {"f3c0", addr - 0x33C0};
	if (addr >= 0x3400 && addr < 0x3420) return {"f400", addr - 0x3400};
	if (addr >= 0x3420 && addr < 0x3440) return {"f420", addr - 0x3420};
	if (addr >= 0x3440 && addr < 0x3460) return {"f440", addr - 0x3440};
	if (addr >= 0x3460 && addr < 0x3480) return {"f460", addr - 0x3460};
	if (addr >= 0x3480 && addr < 0x34a0) return {"f480", addr - 0x3480};
	return {"?", -1};
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.getCore().setStuckPolicy(D110Core::StuckPolicy::La32Stub);
	proc.getCore().setVoiceCtxTap(true);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	proc.getCore().takeCtxEvents(); // drop boot-time noise
	proc.setForwardNotesToFirmware(true);
	std::printf("playing REALISTIC single notes for 20s with La32Stub engaged...\n");
	playRealistic(proc, 20.0);

	const auto events = proc.getCore().takeCtxEvents();
	std::printf("\n%d write events captured\n\n", int(events.size()));

	// Filtered, and the count of what was dropped is printed with it. The firmware writes all
	// over this window for reasons that have nothing to do with voices, and an unfiltered
	// trace buries the events being looked for - but a filter whose losses are not reported
	// is how you end up reading "nothing happened" off a screen that was simply not shown it.
	uint64_t unnamed = 0;
	std::printf("chronological trace, NAMED tables only (background noise elsewhere filtered):\n");
	for (const auto &e : events) {
		const Decoded d = decode(e.addr);
		if (d.index < 0) { ++unnamed; continue; }
		std::printf("  PC %04X  %s[%d]  = %02X\n", e.pc, d.array, d.index, e.value);
	}
	std::printf("\n(%llu unnamed/background events filtered out of %d)\n",
	            (unsigned long long)unnamed, int(events.size()));

	// Summary: which hardware slots (edc0[]) went busy (0x40) and which ever got a
	// completion write (f440[] or f460[] touched for the corresponding context).
	//
	// Read as three columns of the same story: handed out, handler ran, given back. A slot in
	// the first list and not the third is one the firmware still believes it is using, and
	// enough of those is exactly how the instrument runs out of voices while sounding almost
	// nothing. The lists are printed as raw appearances rather than a tally because an index
	// showing up twice is itself informative - it means the slot was re-issued.
	std::printf("\nslots marked busy (edc0[n] = 0x40): ");
	std::map<int, bool> busySlot;
	for (const auto &e : events) {
		const Decoded d = decode(e.addr);
		if (std::string(d.array) == "edc0" && e.value == 0x40) { busySlot[d.index] = true; std::printf("%d ", d.index); }
	}
	std::printf("\ncontexts that got an f440[] write (LA32 handler ran for them): ");
	for (const auto &e : events) {
		const Decoded d = decode(e.addr);
		if (std::string(d.array) == "f440") std::printf("%d(=%02X) ", d.index, e.value);
	}
	std::printf("\ncontexts that got f460[] bit6 set (fully reclaimed): ");
	for (const auto &e : events) {
		const Decoded d = decode(e.addr);
		if (std::string(d.array) == "f460" && (e.value & 0x40)) std::printf("%d ", d.index);
	}
	std::printf("\n");

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
