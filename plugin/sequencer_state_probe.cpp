// Does a project actually recall the D-20-style sequencer? getStateInformation packs each
// track's MidiMessageSequence, tempo/time-signature and per-track mute/solo/quantize
// alongside the firmware NVRAM already covered by state_test.cpp; this checks that round
// trip in isolation - none of it depends on the machine being powered on, so no boot wait
// is needed here.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>

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
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	juce::MemoryBlock savedState;
	{
		D110AudioProcessor proc;
		proc.prepareToPlay(44100.0, 512);
		auto &eng = proc.getSequencer();

		eng.setTempo(97.0);
		eng.setTimeSignature(3, 4);
		eng.setMetronomeEnabled(false);
		eng.setPrecountBars(0);

		eng.armTrack(2);
		eng.startRecording();
		eng.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
		eng.captureEvent(juce::MidiMessage::noteOff(1, 60), 0.5);
		eng.stopRecording();
		eng.setTrackMuted(2, true);
		eng.setTrackSoloed(4, true);
		eng.quantizeTrack(2, d110seq::QuantizeGrid::eighth);

		proc.getStateInformation(savedState);
	}
	check(savedState.getSize() > 0, "getStateInformation produced data");

	D110AudioProcessor proc2;
	proc2.prepareToPlay(44100.0, 512);
	proc2.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
	auto &eng2 = proc2.getSequencer();

	check(std::abs(eng2.getTempo() - 97.0) < 0.01, "tempo restored");
	check(eng2.getTimeSigNumerator() == 3 && eng2.getTimeSigDenominator() == 4, "time signature restored");
	check(!eng2.getMetronomeEnabled(), "metronome flag restored");
	check(eng2.getPrecountBars() == 0, "precount bars restored");
	check(eng2.trackHasEvents(2), "track 2 events restored");
	check(!eng2.trackHasEvents(0), "untouched track 0 stayed empty");
	check(eng2.isTrackMuted(2), "track 2 mute restored");
	check(eng2.isTrackSoloed(4), "track 4 solo restored");
	check(eng2.getTrackQuantize(2) == d110seq::QuantizeGrid::eighth, "track 2 quantize restored");

	if (failures == 0) {
		std::printf("\nALL PASSED\n");
		return 0;
	}
	std::printf("\n%d CHECK(S) FAILED\n", failures);
	return 1;
}
