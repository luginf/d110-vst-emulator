// The demo song is silent: the firmware plays it internally from its own ROM, the panel's
// part indicators move, but mt32emu never hears a thing - the firmware does not transmit
// its own notes (measured: one 0x00 byte in 40s out of its serial TX), and the RAM bridge
// only mirrors PARAMETER regions, not note events.
//
// The fix hinges on whether the firmware writes the note itself somewhere readable. Earlier
// traces suggest it does: playing note 60 at velocity 114 produced `f400[0] = 3C` and
// `f420[0] = 72` - exactly that note and velocity. This probe tests that properly, with a
// controlled experiment: play KNOWN notes on a KNOWN part and see whether f400/f420/f3a0
// carry note/velocity/part, then run the demo song and confirm the same arrays move.
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

const char *arrayName(uint16_t addr, int &index) {
	if (addr >= 0x33A0 && addr < 0x33C0) { index = addr - 0x33A0; return "f3a0"; }
	if (addr >= 0x33C0 && addr < 0x33E0) { index = addr - 0x33C0; return "f3c0"; }
	if (addr >= 0x3400 && addr < 0x3420) { index = addr - 0x3400; return "f400"; }
	if (addr >= 0x3420 && addr < 0x3440) { index = addr - 0x3420; return "f420"; }
	if (addr >= 0x3440 && addr < 0x3460) { index = addr - 0x3440; return "f440"; }
	if (addr >= 0x3460 && addr < 0x3480) { index = addr - 0x3460; return "f460"; }
	if (addr >= 0x3480 && addr < 0x34a0) { index = addr - 0x3480; return "f480"; }
	index = -1;
	return nullptr;
}

void dumpRelevant(D110AudioProcessor &proc, const char *label, int limit) {
	const auto events = proc.getCore().takeCtxEvents();
	std::printf("\n--- %s (%d events) ---\n", label, int(events.size()));
	int shown = 0;
	for (const auto &e : events) {
		int idx = -1;
		const char *name = arrayName(e.addr, idx);
		if (name == nullptr) continue;
		// f400/f420/f3a0 are the interesting ones for note reconstruction.
		const bool interesting = (std::string(name) == "f400" || std::string(name) == "f420" ||
		                          std::string(name) == "f3a0");
		if (!interesting) continue;
		std::printf("  PC %04X  %s[%d] = %02X (%d)\n", e.pc, name, idx, e.value, e.value);
		if (++shown >= limit) { std::printf("  ...\n"); break; }
	}
	if (shown == 0) std::printf("  (nothing written to f3a0/f400/f420)\n");
}

void playNote(D110AudioProcessor &proc, int channel, int note, float vel, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	bool sent = false, released = false;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer midi;
		const double t = std::chrono::duration<double>(Clock::now() - begin).count();
		if (!sent) { midi.addEvent(juce::MidiMessage::noteOn(channel, note, vel), 0); sent = true; }
		else if (!released && t > seconds * 0.6) {
			midi.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
			released = true;
		}
		buffer.clear();
		proc.processBlock(buffer, midi);
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

void idle(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	juce::MidiBuffer empty;
	const auto begin = Clock::now();
	auto next = begin;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		buffer.clear();
		proc.processBlock(buffer, empty);
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

void press(D110AudioProcessor &proc, std::initializer_list<int> idx, int holdMs, int settleMs) {
	for (int i : idx) proc.getCore().setButton(i, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
	for (int i : idx) proc.getCore().setButton(i, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");
	proc.getCore().setVoiceCtxTap(true);

	// Controlled experiment 1: a note whose number and velocity are unmistakable.
	// note 0x3C = 60, velocity 100/127 -> 0x64. On channel 2 = Part 1 (factory map).
	proc.getCore().takeCtxEvents();
	std::printf("\n=== host MIDI: note 60 (0x3C), velocity 100 (0x64), channel 2 = Part 1 ===\n");
	playNote(proc, 2, 60, 100.0f / 127.0f, 2.5);
	dumpRelevant(proc, "expect f400=3C, f420=64", 20);

	// Controlled experiment 2: change note AND part, so both mappings are pinned down.
	// note 0x24 = 36, velocity 40 (0x28), channel 5 = Part 4.
	proc.getCore().takeCtxEvents();
	std::printf("\n=== host MIDI: note 36 (0x24), velocity 40 (0x28), channel 5 = Part 4 ===\n");
	playNote(proc, 5, 36, 40.0f / 127.0f, 2.5);
	dumpRelevant(proc, "expect f400=24, f420=28, f3a0 different from before", 20);

	// Now the real question: does the DEMO SONG write the same arrays?
	std::printf("\n=== demo song (EDIT+ENTER, then ENTER) ===\n");
	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 400);
	press(proc, {D110Core::buttonIndex(1, 0)}, 200, 400);
	proc.getCore().takeCtxEvents();
	idle(proc, 4.0);
	dumpRelevant(proc, "demo song note writes - if these appear, notes are recoverable", 40);

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
