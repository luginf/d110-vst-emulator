// Same measurement as note_latency_probe.cpp, against the native core instead of the
// MAME-backed one: how long from "we handed the note to processBlock" to "the firmware
// itself registered it" (popNoteEvent), and - the actual point of this whole port - how much
// that latency VARIES run to run. note_latency_probe.cpp measured 0-18ms of variance on the
// MAME-backed core, caused by two independent clocks (host audio block clock vs MAME's own
// real-time thread) never being locked together. The native core has no second thread and no
// second clock - CPU stepping and MIDI delivery happen inline, in the same call, on whatever
// thread drives runForSeconds(). If that architectural difference is real, the standard
// deviation here should read close to zero, not just smaller.
//
// Measured the same way a real host would drive it: MIDI handed to the "block" immediately
// before advancing it by one block's worth of emulated time (matching processBlock's own
// block-then-render order), block size and sample rate identical to note_latency_probe.cpp
// so the two numbers are directly comparable.
#include "Source/native/D110CoreNative.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
} // namespace

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const char *nvramDir = argc > 1 ? argv[1] : "C:/temp/claude/mame_factory_reset_nvram";

	D110CoreNative core;
	if (!core.start("C:/Program Files/Common Files/VST3/D-110 Data", nvramDir)) {
		std::printf("failed to start\n");
		return 1;
	}
	core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Stub);

	double elapsed = 0.0;
	auto stepBlock = [&]() { core.runForSeconds(kBlockSeconds); elapsed += kBlockSeconds; };

	std::printf("warming up (9s)...\n");
	for (int i = 0; i < int(9.0 / kBlockSeconds); ++i) stepBlock();
	for (int i = 0; i < int(1.0 / kBlockSeconds); ++i) stepBlock(); // settle, same as the original

	constexpr int kRounds = 15;
	std::vector<double> offsets;
	D110CoreNative::NoteEvent ev;

	for (int i = 0; i < kRounds; ++i) {
		const double sendMs = elapsed * 1000.0;
		const uint8_t on[3] = { 0x91, uint8_t(60 + (i % 12)), 100 };
		core.pushMidi(on, 3);
		stepBlock(); // note handed to this block, exactly as processBlock would receive it

		double recvMs = -1.0;
		// Keep stepping (same 0.4s window as the original) until the matching ON event shows.
		for (int b = 0; b < int(0.4 / kBlockSeconds) && recvMs < 0.0; ++b) {
			while (core.popNoteEvent(ev))
				if (ev.on && ev.note == 60 + (i % 12) && recvMs < 0.0) recvMs = elapsed * 1000.0;
			if (recvMs < 0.0) stepBlock();
		}
		if (recvMs >= 0.0) offsets.push_back(recvMs - sendMs);

		const uint8_t off[3] = { 0x81, uint8_t(60 + (i % 12)), 0 };
		core.pushMidi(off, 3);
		for (int b = 0; b < int(0.3 / kBlockSeconds); ++b) { stepBlock(); while (core.popNoteEvent(ev)) {} }
	}

	std::printf("round | latency to firmware registration, ms\n");
	double sum = 0, minV = 1e9, maxV = -1e9;
	for (size_t i = 0; i < offsets.size(); ++i) {
		std::printf("  %2d | %8.2f\n", int(i) + 1, offsets[i]);
		sum += offsets[i];
		minV = std::min(minV, offsets[i]);
		maxV = std::max(maxV, offsets[i]);
	}
	const double mean = offsets.empty() ? 0.0 : sum / double(offsets.size());
	double variance = 0;
	for (double v : offsets) variance += (v - mean) * (v - mean);
	const double stddev = offsets.empty() ? 0.0 : std::sqrt(variance / double(offsets.size()));

	std::printf("\nmeasurements: %d, mean %.2fms, range [%.2f, %.2f], stddev %.2fms\n",
	            int(offsets.size()), mean, minV, maxV, stddev);
	std::printf("(MAME-backed core, same test: stddev up to a few ms across an 0-18ms range)\n");

	core.stop();
	return 0;
}
