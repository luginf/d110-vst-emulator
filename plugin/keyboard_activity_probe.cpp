// Verifies D110KeyboardHost::isNoteActive() end to end against NonetSeqHost - the on-screen
// keyboard's "remote activity" LEDs (D110Keyboard.cpp's isLit()): a note reaching the app
// through ANY path (here: the same midiCollector injectTestNote()/a real MIDI In port both
// feed, and the sequencer's own renderInto() output) must flip isNoteActive() true while
// sounding and false once it stops. No GUI - NonetSeqHost::advance() runs on its own
// timer/audio-device thread exactly as it does in the real app; this just pumps the message
// loop (needed for the juce::Timer fallback path when no audio device opens) and waits in
// real time for it to catch up, rather than calling any of NonetSeqHost's private plumbing
// directly.

#include <cstdio>

#include <juce_events/juce_events.h>

#include "Source/sequencer/NonetSeqHost.h"

namespace {

int failures = 0;

void check(bool condition, const char *what) {
	std::printf("  %s   %s\n", condition ? "ok" : "FAIL", what);
	if (!condition) ++failures;
}

// Pumps the message loop (drives NonetSeqHost's juce::Timer fallback, a no-op otherwise if
// a real audio device callback is what's actually driving advance()) while waiting for
// `predicate` to become true, up to a generous timeout - real time, since advance()'s own
// clock is real time too (audio device or ~100Hz timer, see NonetSeqHost.h).
template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs) {
	const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
	while (juce::Time::getMillisecondCounter() < deadline) {
		if (predicate()) return true;
		juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
	}
	return predicate();
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;

	// A fresh install with no prior Nonet Sequencer session reads/writes
	// ~/.config/Nonet Sequencer.settings like the real app does - there is no override point,
	// so this probe accepts that like NonetSeqHost's other users do.
	{
		NonetSeqHost host;

		std::printf("-- isNoteActive: direct injection (mouse/PC-keyboard/MIDI-In path) --\n");
		check(!host.isNoteActive(60), "note 60 inactive before anything happens");
		host.injectTestNote(1, 60, 1.0f, true);
		check(waitUntil([&] { return host.isNoteActive(60); }, 2000),
		      "note 60 active once advance() has drained the note-on");
		host.injectTestNote(1, 60, 0.0f, false);
		check(waitUntil([&] { return !host.isNoteActive(60); }, 2000),
		      "note 60 inactive again once advance() has drained the note-off");

		std::printf("-- isNoteActive: sequencer playback (renderInto path) --\n");
		auto &engine = host.getSequencer();
		engine.setPrecountBars(0);
		engine.armTrack(0);
		engine.startRecording();
		engine.captureEvent(juce::MidiMessage::noteOn(engine.channelForTrack(0), 64, (juce::uint8)100), 0.0);
		engine.captureEvent(juce::MidiMessage::noteOff(engine.channelForTrack(0), 64), 1.9);
		engine.stopRecording();
		engine.armTrack(-1);
		check(engine.trackHasEvents(0), "track 0 has the captured note");

		engine.gotoBar(1);
		engine.play();
		check(!host.isNoteActive(64), "note 64 inactive before playback reaches it");
		check(waitUntil([&] { return host.isNoteActive(64); }, 5000),
		      "note 64 went active during playback");
		// The STOP button's real handler calls both of these (D110SequencerPanel.cpp) - engine
		// stopping playback doesn't by itself imply any note-off for whatever was still
		// sounding, midiPanic() is what actually guarantees that, for the LEDs same as for the
		// sound.
		engine.stop();
		host.midiPanic();
		check(waitUntil([&] { return !host.isNoteActive(64); }, 2000),
		      "note 64 inactive again after STOP");
	}

	std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d CHECK(S) FAILED\n", failures);
	return failures == 0 ? 0 : 1;
}
