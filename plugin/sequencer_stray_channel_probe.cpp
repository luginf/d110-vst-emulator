// Reproduces Alan's "parasitic notes, not every time" report (2026-08-18): sometimes,
// playing a song from a .midiseq file (his example: /tmp/songs.midiseq, song slot 2) makes
// notes sound that weren't programmed - as if drawn from whatever instrument happens to be
// live on some other part.
//
// Root cause, found by reading rather than guessing (D110SequencerEngine::channelForTrack()):
// the plugin's channel source (PluginProcessor.cpp's sequencerLiveChannels) starts every
// melodic track at -1 ("unknown yet") and only gets filled in from the firmware's own SYSTEM
// RAM once core.isRunning() is true AND a getRam() call actually succeeds - which, per
// native_snapshot_repro_probe.cpp's own 9-second wait after setPoweredOn(true), can take
// several seconds of real boot time. Until then (or for any Part explicitly turned OFF,
// which also reads back as -1), channelForTrack()'s OLD fallback collapsed EVERY melodic
// track onto the SAME hardcoded channel 1 - so two tracks with simultaneous content would
// both land on channel 1 and audibly collide, sounding like one part's notes are "leaking"
// into another's, for exactly as long as the real channel map hadn't loaded yet. That
// matches "not every time" precisely: it only bites if playback starts before boot finishes
// (or while some Part is OFF), not always.
//
// This probe doesn't need real audio or firmware boot to demonstrate the bug: channelForTrack
// itself is the whole story. It captures the failure with the channel source unset (the
// simplest way to force the -1 fallback for every track, same effect as "not booted yet"),
// checks every melodic track gets its OWN channel (the same factory map
// NonetSeqHost/sequencer_probe.cpp already use: Part N -> channel N+2, Rhythm -> 10) instead
// of every one of them colliding on channel 1.

#include "Source/sequencer/D110SequencerEngine.h"

#include <cstdio>
#include <set>

using d110seq::D110SequencerEngine;

namespace {
int failures = 0;

void check(bool condition, const char *what) {
	if (condition) {
		std::printf("  ok   %s\n", what);
	} else {
		std::printf("  FAIL %s\n", what);
		++failures;
	}
}
} // namespace

int main() {
	D110SequencerEngine engine;
	// No setChannelSource() call at all - channelForTrack() must fall all the way back to
	// its own hardcoded default for every track, exactly like every melodic Part does during
	// the plugin's boot window (sequencerLiveChannels starts filled with -1 - see
	// PluginProcessor.cpp's constructor).

	std::printf("-- fallback channel map (no channel source, no live RAM data yet) --\n");
	std::set<int> seen;
	bool anyCollision = false;
	for (int t = 0; t < D110SequencerEngine::kRhythmTrack; ++t) { // the 8 melodic parts
		const int ch = engine.channelForTrack(t);
		std::printf("  track %d -> channel %d\n", t, ch);
		if (!seen.insert(ch).second) anyCollision = true;
	}
	check(!anyCollision, "no two melodic tracks share a fallback channel");
	check(engine.channelForTrack(D110SequencerEngine::kRhythmTrack) == 10, "rhythm still falls back to channel 10");
	// The exact factory map (Part N -> channel N+2) other tools in this codebase already rely
	// on - sequencer_probe.cpp's defaultChannelForTrack(), NonetSeqHost's real per-track
	// default - so the fallback now matches what the firmware itself will show once booted,
	// not just "not colliding".
	for (int t = 0; t < D110SequencerEngine::kRhythmTrack; ++t)
		check(engine.channelForTrack(t) == t + 2, "matches the real D-110 factory default for this part");

	return failures == 0 ? 0 : 1;
}
