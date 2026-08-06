// Headless check for D110SequencerEngine, with no plugin/firmware dependency at all:
// feeds synthetic note events in at known beat positions, steps the transport through
// renderInto() in fixed-size blocks the way processBlock will, and checks the emitted
// MIDI lands at the exact sample offset the tempo/blocksize predicts - rather than
// assuming the beats-to-samples math is right. Also round-trips a track through
// saveMidiFile/loadMidiFile and through trackToBytes/trackFromBytes (the state-save
// path), and checks quantize snaps note-on/off pairs together without changing note
// length.

#include "Source/sequencer/D110SequencerEngine.h"

#include <cstdio>
#include <vector>

using d110seq::D110SequencerEngine;
using d110seq::QuantizeGrid;
using d110seq::RecordMode;

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

// Default D-110 factory channel map: part N -> channel N+1 (part 1 = ch2, ... part 8 =
// ch9), rhythm fixed on ch10 - see README's factory-defaults note.
int defaultChannelForTrack(int track) {
	return track == D110SequencerEngine::kRhythmTrack ? 10 : track + 2;
}

struct CapturedEvent {
	int sampleOffset;
	juce::MidiMessage message;
};

std::vector<CapturedEvent> renderBlock(D110SequencerEngine &engine, int numSamples, double sampleRate,
                                        std::vector<D110SequencerEngine::MetronomeClick> *clicks = nullptr) {
	juce::MidiBuffer buf;
	engine.renderInto(buf, numSamples, sampleRate, clicks);
	std::vector<CapturedEvent> out;
	for (const auto meta : buf) out.push_back({meta.samplePosition, meta.getMessage()});
	return out;
}

// ---- 1. record via captureEvent, then play back and check exact sample offsets ----
void testPlaybackTiming() {
	std::printf("-- playback timing --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0); // exactly 0.5s/beat
	engine.setTimeSignature(4, 4);

	engine.armTrack(0);
	engine.setPrecountBars(0);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 60), 1.0);
	engine.captureEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8)100), 2.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 64), 3.0);
	engine.stopRecording();
	check(engine.trackHasEvents(0), "track 0 captured events");

	constexpr double sr = 48000.0;
	constexpr int blockSamples = 24000; // exactly 0.5s = 1 beat at 120bpm
	engine.gotoBar(1);
	engine.play();

	auto b0 = renderBlock(engine, blockSamples, sr);
	check(b0.size() == 1 && b0[0].message.isNoteOn() && b0[0].message.getNoteNumber() == 60 &&
	          b0[0].message.getChannel() == 2 && b0[0].sampleOffset == 0,
	      "block 0: note-on 60 ch2 at sample 0");

	auto b1 = renderBlock(engine, blockSamples, sr);
	check(b1.size() == 1 && b1[0].message.isNoteOff() && b1[0].message.getNoteNumber() == 60 &&
	          b1[0].sampleOffset == 0,
	      "block 1: note-off 60 at sample 0");

	auto b2 = renderBlock(engine, blockSamples, sr);
	check(b2.size() == 1 && b2[0].message.isNoteOn() && b2[0].message.getNoteNumber() == 64 &&
	          b2[0].sampleOffset == 0,
	      "block 2: note-on 64 at sample 0");

	auto b3 = renderBlock(engine, blockSamples, sr);
	check(b3.size() == 1 && b3[0].message.isNoteOff() && b3[0].message.getNoteNumber() == 64 &&
	          b3[0].sampleOffset == 0,
	      "block 3: note-off 64 at sample 0");
}

// ---- 2. metronome clicks: one per beat in 4/4, only beat 0 of the bar is a downbeat --
void testMetronome() {
	std::printf("-- metronome --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4);
	engine.armTrack(0);
	engine.setPrecountBars(0);
	engine.startRecording(); // sets playing = true
	engine.gotoBar(1);

	constexpr double sr = 48000.0;
	constexpr int blockSamples = 24000;
	bool downbeats[4] = {};
	int counts[4] = {};
	for (int i = 0; i < 4; ++i) {
		std::vector<D110SequencerEngine::MetronomeClick> clicks;
		renderBlock(engine, blockSamples, sr, &clicks);
		counts[i] = (int)clicks.size();
		if (!clicks.empty()) downbeats[i] = clicks[0].downbeat;
	}
	check(counts[0] == 1 && counts[1] == 1 && counts[2] == 1 && counts[3] == 1, "one click per beat");
	check(downbeats[0] && !downbeats[1] && !downbeats[2] && !downbeats[3], "only beat 0 is a downbeat");
}

// ---- 3. mute / solo gating -------------------------------------------------------
void testMuteSolo() {
	std::printf("-- mute/solo --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4);
	engine.setPrecountBars(0);

	engine.armTrack(0);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
	engine.stopRecording();

	engine.armTrack(1);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 61, (juce::uint8)100), 0.0);
	engine.stopRecording();

	constexpr double sr = 48000.0;
	constexpr int blockSamples = 24000;

	engine.setTrackMuted(0, true);
	engine.gotoBar(1);
	engine.play();
	auto muted = renderBlock(engine, blockSamples, sr);
	check(muted.size() == 1 && muted[0].message.getNoteNumber() == 61, "muted track 0 does not sound");
	engine.setTrackMuted(0, false);

	engine.setTrackSoloed(1, true);
	engine.gotoBar(1);
	auto soloed = renderBlock(engine, blockSamples, sr);
	check(soloed.size() == 1 && soloed[0].message.getNoteNumber() == 61,
	      "soloing track 1 silences unsoloed track 0");
	engine.setTrackSoloed(1, false);
}

// ---- 4. quantize snaps note-on and keeps note-off's original duration -----------
void testQuantize() {
	std::printf("-- quantize --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4);
	engine.setPrecountBars(0);

	engine.armTrack(2);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.93);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 60), 1.87); // 0.94 beats long
	engine.stopRecording();

	engine.quantizeTrack(2, QuantizeGrid::quarter); // 1.0-beat grid: 0.93 -> 1.0

	constexpr double sr = 48000.0;
	constexpr double beatsPerSample = (120.0 / 60.0) / sr;
	constexpr int blockSamples = 24000; // [0,1) beats

	engine.gotoBar(1);
	engine.play();
	auto b0 = renderBlock(engine, blockSamples, sr);
	check(b0.empty(), "quantized note-on no longer sounds before beat 1");

	auto b1 = renderBlock(engine, blockSamples, sr); // [1,2) beats
	bool sawOn = false, sawOff = false;
	int onOffset = -1, offOffset = -1;
	for (const auto &e : b1) {
		if (e.message.isNoteOn()) {
			sawOn = true;
			onOffset = e.sampleOffset;
		}
		if (e.message.isNoteOff()) {
			sawOff = true;
			offOffset = e.sampleOffset;
		}
	}
	check(sawOn && onOffset == 0, "note-on snapped to beat 1.0 (sample 0 of block 1)");
	// Duration preserved: original was 0.94 beats, so note-off should land at
	// beat 1.94, i.e. sample round(0.94 / beatsPerSample) into block 1.
	const int expectedOffOffset = juce::roundToInt(0.94 / beatsPerSample);
	check(sawOff && std::abs(offOffset - expectedOffOffset) <= 1,
	      "note-off kept original note length after quantize");
}

// ---- 5. MIDI file round-trip -----------------------------------------------------
void testMidiFileRoundTrip() {
	std::printf("-- .mid file round-trip --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(97.0);
	engine.setTimeSignature(3, 4);
	engine.setPrecountBars(0);

	engine.armTrack(D110SequencerEngine::kRhythmTrack);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)110), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 36), 0.25);
	engine.captureEvent(juce::MidiMessage::noteOn(1, 38, (juce::uint8)90), 1.5);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 38), 1.75);
	engine.stopRecording();

	const auto tempFile =
	    juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("d110_sequencer_probe.mid");
	check(engine.saveMidiFile(tempFile), "saveMidiFile succeeds");

	D110SequencerEngine reloaded;
	reloaded.setChannelSource(defaultChannelForTrack);
	check(reloaded.loadMidiFile(tempFile), "loadMidiFile succeeds");
	tempFile.deleteFile();

	check(std::abs(reloaded.getTempo() - 97.0) < 0.01, "tempo round-tripped");
	check(reloaded.getTimeSigNumerator() == 3 && reloaded.getTimeSigDenominator() == 4,
	      "time signature round-tripped");
	check(reloaded.trackHasEvents(D110SequencerEngine::kRhythmTrack), "rhythm track events round-tripped");

	reloaded.gotoBar(1);
	reloaded.play();
	constexpr double sr = 48000.0;
	// 97bpm: seconds per beat = 60/97. A block covering exactly one beat:
	const int oneBeatSamples = juce::roundToInt(sr * 60.0 / 97.0);
	auto b0 = renderBlock(reloaded, oneBeatSamples, sr);
	check(!b0.empty() && b0[0].message.isNoteOn() && b0[0].message.getNoteNumber() == 36 &&
	          b0[0].message.getChannel() == 10,
	      "reloaded rhythm note-on at expected position/channel");
}

// ---- 6. trackToBytes/trackFromBytes round-trip (the state-save path) ------------
void testByteRoundTrip() {
	std::printf("-- state byte round-trip --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setPrecountBars(0);
	engine.armTrack(3);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8)80), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 72), 0.5);
	engine.stopRecording();

	const auto bytes = engine.trackToBytes(3);
	check(bytes.getSize() > 0, "trackToBytes produced data");

	D110SequencerEngine other;
	other.setChannelSource(defaultChannelForTrack);
	other.trackFromBytes(3, bytes.getData(), bytes.getSize());
	check(other.trackHasEvents(3), "trackFromBytes restored events");

	// Empty round-trip should clear, not leave stale data.
	other.trackFromBytes(3, nullptr, 0);
	check(!other.trackHasEvents(3), "trackFromBytes(size 0) clears the track");
}

// ---- 7. replaceRange punch erases only the take's own span --------------------
// The bug this guards against: recording bars 1-8, then punching in again from bar 4
// to bar 6, used to erase bars 6-8 as well - stopRecording() cleared from the take's
// start to the END of the track, not just to where the take actually stopped.
void testPunchReplacePreservesSurroundings() {
	std::printf("-- replaceRange punch preserves the bars outside the take --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4);
	engine.setPrecountBars(0);
	engine.setRecordMode(RecordMode::replaceRange);

	engine.armTrack(0);
	engine.startRecording();
	for (int bar = 0; bar < 8; ++bar) {
		const double beat = bar * 4.0;
		engine.captureEvent(juce::MidiMessage::noteOn(1, 60 + bar, (juce::uint8)100), beat);
		engine.captureEvent(juce::MidiMessage::noteOff(1, 60 + bar), beat + 1.0);
	}
	engine.stopRecording();

	// Punch in at bar 4 (beat 12), stop at bar 6 (beat 20) - gotoBar() stands in here for
	// what renderInto() would have done to positionBeats over a real, block-by-block take.
	engine.gotoBar(4);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 99, (juce::uint8)100), 12.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 99), 13.0);
	engine.gotoBar(6);
	engine.stopRecording();

	engine.gotoBar(1);
	engine.play();
	auto all = renderBlock(engine, 24000 * 40, 48000.0); // the whole 8-bar span in one go
	std::vector<int> onNotes;
	for (const auto &e : all)
		if (e.message.isNoteOn()) onNotes.push_back(e.message.getNoteNumber());
	const std::vector<int> expected = {60, 61, 62, 99, 65, 66, 67};
	check(onNotes == expected, "bars 1-3 and 6-8 survived; only bars 4-5 were replaced");
}

// ---- 8. overdub adds without erasing, and the earlier take is heard while recording ----
void testOverdubMode() {
	std::printf("-- overdub mode --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4);
	engine.setPrecountBars(0);

	engine.armTrack(0);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 60), 1.0);
	engine.stopRecording();

	// Second take, overdub: while it's in progress (before any new capture), the FIRST
	// take's note must still play back - this is the architecture fix that lets overdub's
	// existing material actually be heard while laying down the new one.
	engine.setRecordMode(RecordMode::overdub);
	engine.gotoBar(1);
	engine.armTrack(0);
	engine.startRecording();
	auto midTake = renderBlock(engine, 24000, 48000.0); // beat [0,1)
	bool sawOriginalDuringTake = false;
	for (const auto &e : midTake)
		if (e.message.isNoteOn() && e.message.getNoteNumber() == 60) sawOriginalDuringTake = true;
	check(sawOriginalDuringTake, "original note still audible mid-overdub-take");

	engine.captureEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8)100), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 72), 1.0);
	engine.stopRecording();

	engine.gotoBar(1);
	engine.play();
	auto b0 = renderBlock(engine, 24000, 48000.0);
	bool saw60 = false, saw72 = false;
	for (const auto &e : b0) {
		if (e.message.isNoteOn() && e.message.getNoteNumber() == 60) saw60 = true;
		if (e.message.isNoteOn() && e.message.getNoteNumber() == 72) saw72 = true;
	}
	check(saw60 && saw72, "overdub kept the original note and added the new one");
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	testPlaybackTiming();
	testMetronome();
	testMuteSolo();
	testQuantize();
	testMidiFileRoundTrip();
	testByteRoundTrip();
	testPunchReplacePreservesSurroundings();
	testOverdubMode();

	if (failures == 0) {
		std::printf("\nALL PASSED\n");
		return 0;
	}
	std::printf("\n%d CHECK(S) FAILED\n", failures);
	return 1;
}
