// Headless check for D110SequencerEngine, with no plugin/firmware dependency at all:
// feeds synthetic note events in at known beat positions, steps the transport through
// renderInto() in fixed-size blocks the way processBlock will, and checks the emitted
// MIDI lands at the exact sample offset the tempo/blocksize predicts - rather than
// assuming the beats-to-samples math is right. Also round-trips a track through
// saveMidiFile/loadMidiFile and through trackToBytes/trackFromBytes (the state-save
// path), and checks quantize snaps note-on/off pairs together without changing note
// length.

#include "Source/sequencer/D110SequencerEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using d110seq::D110SequencerEngine;
using d110seq::LoopMode;
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

// ---- 2c. isPrecounting() correctly drops back to false, promptly, once the count-in has
// consumed exactly precountBars worth of beats - and positionBeats (frozen throughout the
// count-in, see startRecording()'s comment) starts advancing normally again right away.
// D110SequencerPanel's LED strip relies on exactly this transition: a static downbeat-only
// LED while isPrecounting(), the normal scrolling currentClickInBar() the instant it isn't.
void testPrecountEndsPromptly() {
	std::printf("-- precount ends promptly --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4);
	engine.armTrack(0);
	engine.setPrecountBars(1); // 1 bar = 4 beats = 2s at 120bpm
	engine.startRecording();
	engine.gotoBar(1);

	// Deliberately NOT beat-aligned (real host buffer size), unlike testMetronome()'s neat
	// exact-beat blocks - the kind of block size a real transition bug would hide from.
	constexpr double sr = 48000.0;
	constexpr int blockSamples = 512;
	const double startingPosition = engine.getPositionBeats();
	bool sawPrecountEnd = false;
	bool stayedFrozenWhilePrecounting = true;
	for (int i = 0; i < 500 && !sawPrecountEnd; ++i) {
		const bool wasPrecountingBefore = engine.isPrecounting();
		renderBlock(engine, blockSamples, sr);
		// The block where precount actually ends legitimately advances positionBeats partway
		// through - it falls through to normal rendering for whatever's left of that same
		// block once the count-in is used up (see renderInto()'s own comment) - so only blocks
		// that were STILL precounting before AND after this call need to have stayed frozen.
		if (wasPrecountingBefore && engine.isPrecounting() && engine.getPositionBeats() != startingPosition)
			stayedFrozenWhilePrecounting = false;
		if (!engine.isPrecounting()) sawPrecountEnd = true;
	}
	check(sawPrecountEnd, "isPrecounting() drops to false within a couple of seconds, not stuck");
	check(stayedFrozenWhilePrecounting, "positionBeats was frozen for every block still fully inside precount");
	renderBlock(engine, blockSamples, sr);
	check(engine.getPositionBeats() > startingPosition, "positionBeats resumes advancing right after precount");
}

// ---- 2d. precountBeatsElapsed() - what D110SequencerPanel edge-detects to flash the
// downbeat LED once per precount beat (2026-08-07, so the count-in can be followed visually
// without audio) - ticks 0,1,2,3 in order, once per beat, over a 1-bar/4-beat count-in at
// 120bpm, under the same realistic non-beat-aligned block size as the test above.
void testPrecountBeatsElapsed() {
	std::printf("-- precount beats elapsed --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4);
	engine.armTrack(0);
	engine.setPrecountBars(1);
	engine.startRecording();
	engine.gotoBar(1);

	constexpr double sr = 48000.0;
	constexpr int blockSamples = 512;
	std::vector<int> seen;
	for (int i = 0; i < 500 && engine.isPrecounting(); ++i) {
		const int elapsed = engine.precountBeatsElapsed();
		if (seen.empty() || seen.back() != elapsed) seen.push_back(elapsed);
		renderBlock(engine, blockSamples, sr);
	}
	check(seen.size() == 4 && seen[0] == 0 && seen[1] == 1 && seen[2] == 2 && seen[3] == 3,
	      "precountBeatsElapsed() ticks 0,1,2,3 in order, once per beat");
}

// ---- 2e. stop() resets the beat-within-bar counter to zero (Alan, 2026-08-07): recording
// used to leave positionBeats wherever STOP happened, so resuming without a precount picked
// the metronome back up mid-bar (e.g. beat 4, if the previous take ended on beat 3) instead
// of a clean beat 1. Records into bar 2, stops partway through beat 3, and checks the
// metronome reads beat 0 again immediately - while still reporting the SAME bar, since only
// the beat-within-bar remainder should reset, not the whole timeline position.
void testStopResetsBeatCounter() {
	std::printf("-- stop resets beat counter --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4);
	engine.setPrecountBars(0);
	engine.armTrack(0);
	engine.gotoBar(2);
	engine.startRecording();

	constexpr double sr = 48000.0;
	// 2.25 beats in - partway through beat 3 (0-indexed beat 2), well short of beat 4.
	renderBlock(engine, static_cast<int>(2.25 * 0.5 * sr), sr);
	check(engine.currentClickInBar() == 2, "sanity: stopped partway through beat 3 (index 2)");
	const int barBeforeStop = engine.getCurrentBar();

	engine.stop();
	check(engine.currentClickInBar() == 0, "beat counter reads 0 again immediately after stop");
	check(engine.getCurrentBar() == barBeforeStop, "still the same bar - only the beat remainder reset");

	engine.armTrack(0);
	engine.startRecording(); // no precount
	check(engine.currentClickInBar() == 0, "resuming without a precount starts the metronome on beat 1");
}

// ---- 2b. metronome clicks under REAL-SIZED, tempo-misaligned blocks (Alan reported the
// downbeat sound sometimes landing on beat 2 as well as beat 1) - testMetronome() above uses
// a block size that lands exactly on a beat boundary every time, which is nothing like a real
// processBlock() callback (512 samples or so, essentially never beat-aligned) and would hide
// any off-by-one at a block boundary. This renders several bars in small blocks across a few
// tempo/time-signature combinations and checks each click against the beat grid directly:
// exactly one click per beat, no two clicks closer together than half a beat, and the
// downbeat flag true on exactly the bar's first beat and nowhere else.
void testMetronomeRealBlocks() {
	std::printf("-- metronome (real block size) --\n");
	struct Config { double bpm; int num, den; int blockSamples; };
	const Config configs[] = {
		{ 120.0, 4, 4, 512 }, { 96.5, 3, 4, 256 }, { 173.0, 6, 8, 128 }, { 140.0, 4, 4, 480 },
	};
	constexpr double sr = 48000.0;
	constexpr int kBars = 8;

	for (const auto &cfg : configs) {
		D110SequencerEngine engine;
		engine.setChannelSource(defaultChannelForTrack);
		engine.setTempo(cfg.bpm);
		engine.setTimeSignature(cfg.num, cfg.den);
		engine.armTrack(0);
		engine.setPrecountBars(0);
		engine.startRecording();
		engine.gotoBar(1);

		const double clickGrid = 4.0 / double(cfg.den);
		const double barBeats = double(cfg.num) * (4.0 / double(cfg.den));
		const int clicksPerBar = juce::roundToInt(barBeats / clickGrid);
		const double beatsPerSample = (cfg.bpm / 60.0) / sr;
		const double totalBeats = barBeats * double(kBars);
		const int totalSamples = int(totalBeats / beatsPerSample) + cfg.blockSamples;

		double lastClickBeat = -1.0;
		int clickCount = 0;
		bool anyDoubled = false, anyMisplacedDownbeat = false, anyTooClose = false;
		double elapsedBeats = 0.0;
		for (int rendered = 0; rendered < totalSamples; rendered += cfg.blockSamples) {
			std::vector<D110SequencerEngine::MetronomeClick> clicks;
			const int n = std::min(cfg.blockSamples, totalSamples - rendered);
			renderBlock(engine, n, sr, &clicks);
			for (const auto &c : clicks) {
				const double clickBeat = elapsedBeats + double(c.samplePosition) * beatsPerSample;
				if (lastClickBeat >= 0.0) {
					const double delta = clickBeat - lastClickBeat;
					if (delta < clickGrid * 0.5) anyTooClose = true;
					if (std::abs(delta - clickGrid) > clickGrid * 0.25) anyDoubled = true;
				}
				const int idxInBar = juce::roundToInt(std::fmod(clickBeat, barBeats) / clickGrid) % clicksPerBar;
				const bool shouldBeDownbeat = idxInBar == 0;
				if (c.downbeat != shouldBeDownbeat) anyMisplacedDownbeat = true;
				lastClickBeat = clickBeat;
				++clickCount;
			}
			elapsedBeats += double(n) * beatsPerSample;
		}

		juce::String label = juce::String(cfg.bpm, 1) + " bpm " + juce::String(cfg.num) + "/"
		                    + juce::String(cfg.den) + " block=" + juce::String(cfg.blockSamples);
		check(clickCount > 0, (label + ": clicks were emitted at all").toRawUTF8());
		check(!anyTooClose, (label + ": no two clicks closer than half a beat").toRawUTF8());
		check(!anyDoubled, (label + ": every click is one grid step after the last").toRawUTF8());
		check(!anyMisplacedDownbeat, (label + ": downbeat flag matches beat 0 of the bar, and only beat 0").toRawUTF8());
	}
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

// ---- 4b. clearTrack erases one track's events and leaves the rest alone ---------
void testClearTrack() {
	std::printf("-- clear track --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setPrecountBars(0);

	for (int t : { 1, 2 }) {
		engine.armTrack(t);
		engine.startRecording();
		engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
		engine.captureEvent(juce::MidiMessage::noteOff(1, 60), 0.5);
		engine.stopRecording();
	}
	engine.setTrackSoloed(2, true);
	engine.quantizeTrack(2, QuantizeGrid::quarter);

	check(engine.trackHasEvents(1) && engine.trackHasEvents(2), "both tracks have events before clear");
	engine.clearTrack(2);
	check(!engine.trackHasEvents(2), "clearTrack removed track 2's events");
	check(engine.trackHasEvents(1), "track 1 untouched by clearing track 2");
	check(engine.isTrackSoloed(2), "clearTrack left track 2's solo state alone");
	check(engine.getTrackQuantize(2) == QuantizeGrid::quarter,
	      "clearTrack left track 2's quantize setting alone");
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

// ---- 5b. saveMidiFile embeds a Program Change from the program source -----------
void testProgramChangeExport() {
	std::printf("-- Program Change export --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setPrecountBars(0);
	// Track 2 (part 3, channel 4) is "playing" tone 41; the rhythm track has no such
	// concept, matching how PluginProcessor leaves sequencerLivePrograms[kRhythmTrack] at -1.
	engine.setProgramSource([](int track) {
		return track == 2 ? 41 : (track == D110SequencerEngine::kRhythmTrack ? -1 : -1);
	});

	engine.armTrack(2);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 60), 0.5);
	engine.stopRecording();

	const auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
	                           .getChildFile("d110_sequencer_probe_pc.mid");
	check(engine.saveMidiFile(tempFile), "saveMidiFile succeeds");

	juce::FileInputStream in(tempFile);
	juce::MidiFile mf;
	check(in.openedOk() && mf.readFrom(in), "exported file reads back as a MIDI file");
	tempFile.deleteFile();

	// Track layout: track 0 is the leading tempo/time-sig meta track (see saveMidiFile),
	// so track index t maps to mf track (t + 1).
	bool foundProgramChange = false;
	if (mf.getNumTracks() > 3) {
		const auto *seq = mf.getTrack(3);
		for (int i = 0; i < seq->getNumEvents(); ++i) {
			const auto &msg = seq->getEventPointer(i)->message;
			if (msg.isProgramChange() && msg.getTimeStamp() == 0.0
			    && msg.getProgramChangeNumber() == 41 && msg.getChannel() == 4) {
				foundProgramChange = true;
				break;
			}
		}
	}
	check(foundProgramChange, "track 2's exported track carries PC 41 on channel 4 at time 0");

	// A track the program source has no opinion on gets no Program Change at all - not a
	// wrong/placeholder one.
	bool rhythmHasProgramChange = false;
	if (mf.getNumTracks() > D110SequencerEngine::kRhythmTrack + 1) {
		const auto *seq = mf.getTrack(D110SequencerEngine::kRhythmTrack + 1);
		for (int i = 0; i < seq->getNumEvents(); ++i)
			if (seq->getEventPointer(i)->message.isProgramChange()) rhythmHasProgramChange = true;
	}
	check(!rhythmHasProgramChange, "rhythm track (no program source opinion) has no Program Change");
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

// ---- 7b. a replace mode goes silent on the armed track's own existing content mid-take -----
// Alan's report: overdub hearing the track underneath makes sense, but a replace take
// shouldn't play back what it's about to erase.
void testReplaceModeSilentDuringTake() {
	std::printf("-- replace mode is silent on the armed track while recording --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4);
	engine.setPrecountBars(0);

	// Track 0: existing note, about to be replaced. Track 1: existing note, untouched.
	engine.armTrack(0);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 60), 1.0);
	engine.stopRecording();
	engine.armTrack(1);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 61, (juce::uint8)100), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 61), 1.0);
	engine.stopRecording();

	engine.setRecordMode(RecordMode::replaceRange);
	engine.gotoBar(1);
	engine.armTrack(0);
	engine.startRecording();
	auto midTake = renderBlock(engine, 24000, 48000.0); // beat [0,1)
	bool saw60 = false, saw61 = false;
	for (const auto &e : midTake) {
		if (e.message.isNoteOn() && e.message.getNoteNumber() == 60) saw60 = true;
		if (e.message.isNoteOn() && e.message.getNoteNumber() == 61) saw61 = true;
	}
	check(!saw60, "replace mode: the armed track's own existing note is NOT heard mid-take");
	check(saw61, "replace mode: an untouched track still plays normally");
	engine.stopRecording();
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

// ---- 9. LoopMode::bar repeats the anchored bar instead of moving past it ---------
void testLoopBar() {
	std::printf("-- loop: bar mode --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4); // 1 bar = 4 beats
	engine.setPrecountBars(0);

	engine.armTrack(0);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0); // bar 1
	engine.captureEvent(juce::MidiMessage::noteOff(1, 60), 1.0);
	engine.captureEvent(juce::MidiMessage::noteOn(1, 61, (juce::uint8)100), 4.0); // bar 2
	engine.captureEvent(juce::MidiMessage::noteOff(1, 61), 5.0);
	engine.stopRecording();

	engine.gotoBar(1);
	engine.setLoopMode(LoopMode::bar);
	engine.play();

	constexpr double sr = 48000.0;
	// One bar = 4 beats = 2s at 120bpm. Render 3 bars' worth in one call to force at least
	// two wraps within a single renderInto() block, the case the sub-block splitting exists
	// for.
	auto all = renderBlock(engine, juce::roundToInt(sr * 2.0 * 3.0), sr);
	int noteOnCount60 = 0, noteOnCount61 = 0;
	for (const auto &e : all) {
		if (!e.message.isNoteOn()) continue;
		if (e.message.getNoteNumber() == 60) ++noteOnCount60;
		if (e.message.getNoteNumber() == 61) ++noteOnCount61;
	}
	check(noteOnCount60 == 3 && noteOnCount61 == 0,
	      "bar loop replays bar 1's note 3 times and never reaches bar 2's note");
	check(engine.getCurrentBar() == 1, "playhead stays anchored to bar 1 after looping");
}

// ---- 10. LoopMode::punch both loops the range and restricts capture to it --------
void testLoopPunchRestrictsRecording() {
	std::printf("-- loop: punch mode --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4);
	engine.setPrecountBars(0);
	engine.setRecordMode(RecordMode::overdub);
	engine.setPunchRange(2, 3); // bars 2-3 only

	engine.armTrack(0);
	engine.setLoopMode(LoopMode::punch); // should snap positionBeats to bar 2's start
	check(engine.getCurrentBar() == 2, "engaging punch loop snaps the playhead into the punch range");

	engine.startRecording();
	// One capture inside the punch range (bar 2, beat 4) and one outside it (bar 4, beat
	// 12) - only the first should survive; the second is the kind of accidental note a
	// punch workflow exists to protect against.
	engine.captureEvent(juce::MidiMessage::noteOn(1, 65, (juce::uint8)100), 4.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 65), 4.5);
	engine.captureEvent(juce::MidiMessage::noteOn(1, 99, (juce::uint8)100), 12.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 99), 12.5);
	engine.stopRecording();

	check(engine.trackHasEvents(0), "punch-recorded track has events");

	// Turn looping off to inspect the whole track, not just the punch range played back on
	// a loop.
	engine.setLoopMode(LoopMode::off);
	engine.gotoBar(1);
	engine.stop();
	engine.play();
	constexpr double sr = 48000.0;
	auto all = renderBlock(engine, juce::roundToInt(sr * 8.0), sr); // whole 4-bar span, no loop now
	bool saw65 = false, saw99 = false;
	for (const auto &e : all) {
		if (!e.message.isNoteOn()) continue;
		if (e.message.getNoteNumber() == 65) saw65 = true;
		if (e.message.getNoteNumber() == 99) saw99 = true;
	}
	check(saw65 && !saw99, "only the note captured inside the punch range was kept");
}

// ---- 11. newSong() clears the current slot and only the current slot -------------
void testNewSong() {
	std::printf("-- new song --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setPrecountBars(0);
	engine.setTempo(140.0);

	engine.armTrack(0);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 60), 1.0);
	engine.stopRecording();
	engine.setTrackMuted(1, true);
	engine.quantizeTrack(0, QuantizeGrid::eighth);
	check(engine.trackHasEvents(0), "track has events before New");

	engine.newSong();
	check(!engine.trackHasEvents(0), "New cleared the recorded track");
	check(!engine.isTrackMuted(1), "New also reset mute state");
	check(engine.getTrackQuantize(0) == QuantizeGrid::off, "New also reset quantize state");
	check(std::abs(engine.getTempo() - 140.0) < 0.01, "New left tempo (a workspace setting) untouched");
}

// ---- 12. song slots are independent and persist across switches ------------------
void testSongSlots() {
	std::printf("-- song slots --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setPrecountBars(0);
	check(engine.getCurrentSongSlot() == 0, "starts on slot 0");
	check(!engine.songSlotHasContent(0) && !engine.songSlotHasContent(1), "both slots start empty");

	engine.setTempo(90.0);
	engine.setTimeSignature(3, 4);
	engine.armTrack(0);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 60), 1.0);
	engine.stopRecording();

	engine.selectSongSlot(1);
	check(engine.getCurrentSongSlot() == 1, "switched to slot 1");
	check(!engine.trackHasEvents(0), "slot 1 starts with its own empty track 0");
	check(std::abs(engine.getTempo() - 120.0) < 0.01, "slot 1 starts with default tempo, not slot 0's");

	engine.setTempo(160.0);
	engine.armTrack(2);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 77, (juce::uint8)100), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 77), 1.0);
	engine.stopRecording();

	engine.selectSongSlot(0);
	check(engine.trackHasEvents(0) && std::abs(engine.getTempo() - 90.0) < 0.01 &&
	          engine.getTimeSigNumerator() == 3,
	      "switching back to slot 0 restores its own tempo/time-sig/track");

	engine.selectSongSlot(1);
	check(engine.trackHasEvents(2) && std::abs(engine.getTempo() - 160.0) < 0.01,
	      "switching back to slot 1 restores what was recorded there");

	check(engine.songSlotHasContent(0) && engine.songSlotHasContent(1), "both slots report having content");
	check(!engine.songSlotHasContent(2), "an untouched slot reports no content");

	// The slot* accessors must see the same data as the plain ones, for whichever slot is
	// live, and the stored copy for slots that aren't - this is what state persistence
	// relies on to save/restore all four slots without disturbing which one is live.
	check(std::abs(engine.slotTempo(1) - 160.0) < 0.01, "slotTempo(current) matches getTempo()");
	check(std::abs(engine.slotTempo(0) - 90.0) < 0.01, "slotTempo(other) reads slot 0's stored tempo");
	engine.setSlotTempo(0, 100.0);
	engine.selectSongSlot(0);
	check(std::abs(engine.getTempo() - 100.0) < 0.01, "setSlotTempo(other) is visible after switching to it");
}

// ---- 12b. copyCurrentSongTo() duplicates the current slot's content into another slot ----
void testCopySongSlot() {
	std::printf("-- copyCurrentSongTo --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setPrecountBars(0);
	engine.setTempo(100.0);
	engine.setTimeSignature(6, 8);
	engine.armTrack(0);
	engine.startRecording();
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
	engine.captureEvent(juce::MidiMessage::noteOff(1, 60), 1.0);
	engine.stopRecording();
	engine.quantizeTrack(0, QuantizeGrid::eighth);

	check(!engine.songSlotHasContent(2), "slot 2 starts empty");
	engine.copyCurrentSongTo(2);
	check(engine.songSlotHasContent(2), "slot 2 has content after copy");
	check(std::abs(engine.slotTempo(2) - 100.0) < 0.01, "copy carried the tempo over");
	check(engine.slotTimeSigNumerator(2) == 6 && engine.slotTimeSigDenominator(2) == 8,
	      "copy carried the time signature over");
	check(engine.slotTrackQuantize(2, 0) == QuantizeGrid::eighth, "copy carried per-track quantize over");

	// Switching to slot 2 should show the same recorded note as slot 0.
	engine.selectSongSlot(2);
	check(engine.trackHasEvents(0), "slot 2's copied track actually has the note");
	check(engine.getCurrentSongSlot() == 2, "still on slot 2 after switching");

	// The source slot itself is untouched by the copy.
	engine.selectSongSlot(0);
	check(engine.trackHasEvents(0), "slot 0 (the source) still has its own note");
	check(std::abs(engine.getTempo() - 100.0) < 0.01, "slot 0's tempo unaffected");

	// Copying onto the current slot itself is a no-op, not an accidental self-wipe.
	engine.copyCurrentSongTo(0);
	check(engine.trackHasEvents(0), "copying a slot onto itself is a no-op");
}

// ---- 13. precount is fictitious: it doesn't move the bar the take starts on ------
// Alan's report (2026-08-06): with precount on, the take started 1-2 bars later than the bar
// he was actually on when he pressed record - precount was advancing positionBeats instead of
// just clicking in place before capture began.
void testPrecountIsFictitious() {
	std::printf("-- precount is fictitious --\n");
	D110SequencerEngine engine;
	engine.setChannelSource(defaultChannelForTrack);
	engine.setTempo(120.0);
	engine.setTimeSignature(4, 4); // 1 bar = 4 beats = 2s at 120bpm
	engine.setPrecountBars(1);

	engine.gotoBar(5);
	engine.armTrack(0);
	engine.startRecording();
	check(engine.isPrecounting(), "precounting right after startRecording()");
	check(engine.getCurrentBar() == 5, "bar reads 5 (where record was pressed) at precount start");

	// A note played DURING the count-in must not be captured - the count-in hasn't finished.
	engine.captureEvent(juce::MidiMessage::noteOn(1, 40, (juce::uint8)100), engine.getPositionBeats());

	constexpr double sr = 48000.0;
	const int oneBarSamples = juce::roundToInt(sr * 2.0); // exactly 1 bar at 120bpm/4/4
	std::vector<D110SequencerEngine::MetronomeClick> clicks;
	renderBlock(engine, oneBarSamples, sr, &clicks);

	check(!engine.isPrecounting(), "precount finished after exactly 1 bar's worth of samples");
	check(engine.getCurrentBar() == 5, "bar STILL reads 5 - precount never advanced it");
	check(clicks.size() == 4 && clicks[0].downbeat, "4 clicks for the precount bar, first one a downbeat");

	// Now the real take starts - a note here must land right at bar 5's own start beat.
	engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), engine.getPositionBeats());
	engine.captureEvent(juce::MidiMessage::noteOff(1, 60), engine.getPositionBeats() + 1.0);
	engine.stopRecording();

	engine.gotoBar(5);
	engine.play();
	auto onBar5 = renderBlock(engine, oneBarSamples, sr);
	bool saw60 = false, saw40 = false;
	for (const auto &e : onBar5) {
		if (!e.message.isNoteOn()) continue;
		if (e.message.getNoteNumber() == 60) saw60 = true;
		if (e.message.getNoteNumber() == 40) saw40 = true;
	}
	check(saw60, "the real take's note lands right on bar 5, not bar 6 or 7");
	check(!saw40, "the note played during the count-in was never captured");
}

// ---- 14. deleteBars() ripples per track, closes the gap on just the one deleted from -----
void testDeleteBars() {
	std::printf("-- deleteBars --\n");
	constexpr double sr = 48000.0;
	constexpr int barSamples = 96000; // 2s at 48kHz = 1 bar at 120bpm/4-4

	auto seed = [&](D110SequencerEngine &engine, int track, std::initializer_list<int> bars) {
		engine.armTrack(track);
		engine.startRecording();
		for (int bar : bars) {
			const double b = double(bar - 1) * 4.0;
			engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), b);
			engine.captureEvent(juce::MidiMessage::noteOff(1, 60), b + 0.5);
		}
		engine.stopRecording();
	};
	auto onOffsets = [&](std::vector<CapturedEvent> &notes, int channel) {
		std::vector<int> offs;
		for (const auto &e : notes)
			if (e.message.isNoteOn() && e.message.getChannel() == channel) offs.push_back(e.sampleOffset);
		return offs;
	};

	{
		D110SequencerEngine engine;
		engine.setChannelSource(defaultChannelForTrack);
		engine.setTempo(120.0);
		engine.setTimeSignature(4, 4);
		engine.setPrecountBars(0);
		seed(engine, 0, {1, 2, 3, 4});
		seed(engine, 1, {1, 2, 3, 4});

		engine.deleteBars(0, 2, 2); // delete just bar 2, track 0 only

		engine.gotoBar(1);
		engine.play();
		auto notes = renderBlock(engine, barSamples * 4, sr);
		auto t0 = onOffsets(notes, 2);
		auto t1 = onOffsets(notes, 3);
		check(t0.size() == 3, "single-track delete: track 0 has 3 notes left (was 4)");
		if (t0.size() == 3)
			check(t0[0] == 0 && t0[1] == barSamples && t0[2] == barSamples * 2,
			      "single-track delete: bar 1 kept, old bar 3/4 close the gap into bar 2/3");
		check(t1.size() == 4, "single-track delete: track 1 untouched, still all 4 bars");
	}

	{
		D110SequencerEngine engine;
		engine.setChannelSource(defaultChannelForTrack);
		engine.setTempo(120.0);
		engine.setTimeSignature(4, 4);
		engine.setPrecountBars(0);
		seed(engine, 0, {1, 2, 3});
		seed(engine, 1, {1, 2, 3});

		engine.deleteBars(-1, 1, 1); // delete bar 1 from every track

		engine.gotoBar(1);
		engine.play();
		auto notes = renderBlock(engine, barSamples * 3, sr);
		auto t0 = onOffsets(notes, 2);
		auto t1 = onOffsets(notes, 3);
		check(t0.size() == 2 && t1.size() == 2, "all-track delete: both tracks lost bar 1");
		if (t0.size() == 2 && t1.size() == 2)
			check(t0[0] == 0 && t0[1] == barSamples && t1[0] == 0 && t1[1] == barSamples,
			      "all-track delete: both tracks' old bar 2/3 shift to bar 1/2, staying aligned");
	}
}

// ---- 15. copyBars() inserts, and only ever carries notes across tracks, not the channel --
void testCopyBars() {
	std::printf("-- copyBars --\n");
	constexpr double sr = 48000.0;
	constexpr int barSamples = 96000; // 2s at 48kHz = 1 bar at 120bpm/4-4

	auto seed = [&](D110SequencerEngine &engine, int track, std::initializer_list<int> bars) {
		engine.armTrack(track);
		engine.startRecording();
		for (int bar : bars) {
			const double b = double(bar - 1) * 4.0;
			engine.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), b);
			engine.captureEvent(juce::MidiMessage::noteOff(1, 60), b + 0.5);
		}
		engine.stopRecording();
	};
	auto onOffsets = [&](std::vector<CapturedEvent> &notes, int channel) {
		std::vector<int> offs;
		for (const auto &e : notes)
			if (e.message.isNoteOn() && e.message.getChannel() == channel) offs.push_back(e.sampleOffset);
		return offs;
	};

	// Same-track insert: copying bar 1 to bar 2 pushes the existing bar 2/3 later.
	{
		D110SequencerEngine engine;
		engine.setChannelSource(defaultChannelForTrack);
		engine.setTempo(120.0);
		engine.setTimeSignature(4, 4);
		engine.setPrecountBars(0);
		seed(engine, 0, {1, 2, 3});

		engine.copyBars(0, 0, 1, 1, 2);

		engine.gotoBar(1);
		engine.play();
		auto notes = renderBlock(engine, barSamples * 4, sr);
		auto offs = onOffsets(notes, 2);
		check(offs.size() == 4, "same-track insert: 4 notes now (was 3, +1 inserted copy)");
		if (offs.size() == 4)
			check(offs[0] == 0 && offs[1] == barSamples && offs[2] == barSamples * 2 &&
			          offs[3] == barSamples * 3,
			      "same-track insert: bar 1 kept, copy inserted as new bar 2, old bar 2/3 pushed to bar 3/4");
	}

	// Cross-track copy: only the notes travel - the copy plays on the destination track's own
	// live channel, never the source track's.
	{
		D110SequencerEngine engine;
		engine.setChannelSource(defaultChannelForTrack);
		engine.setTempo(120.0);
		engine.setTimeSignature(4, 4);
		engine.setPrecountBars(0);
		seed(engine, 0, {1});

		engine.copyBars(0, 1, 1, 1, 1); // track 0 bar 1 -> track 1 bar 1

		engine.gotoBar(1);
		engine.play();
		auto notes = renderBlock(engine, barSamples, sr);
		bool onDestChannel = false;
		for (const auto &e : notes)
			if (e.message.isNoteOn() && e.message.getChannel() == 3) onDestChannel = true;
		check(onDestChannel, "cross-track copy: the note plays on track 1's own channel (3), not track 0's (2)");
	}

	// "All tracks" copy: applied independently per track, so they stay aligned with each other.
	{
		D110SequencerEngine engine;
		engine.setChannelSource(defaultChannelForTrack);
		engine.setTempo(120.0);
		engine.setTimeSignature(4, 4);
		engine.setPrecountBars(0);
		seed(engine, 0, {1});
		seed(engine, 1, {1});

		engine.copyBars(-1, -1, 1, 1, 3); // bar 1 -> bar 3, every track

		engine.gotoBar(1);
		engine.play();
		auto notes = renderBlock(engine, barSamples * 3, sr);
		auto offs0 = onOffsets(notes, 2);
		auto offs1 = onOffsets(notes, 3);
		check(offs0.size() == 2 && offs1.size() == 2, "all-tracks copy: both tracks got the inserted copy");
		if (offs0.size() == 2 && offs1.size() == 2)
			check(offs0[0] == 0 && offs0[1] == barSamples * 2 && offs1[0] == 0 && offs1[1] == barSamples * 2,
			      "all-tracks copy: both tracks land the copy at bar 3, staying aligned with each other");
	}
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	testPlaybackTiming();
	testMetronome();
	testMetronomeRealBlocks();
	testPrecountEndsPromptly();
	testPrecountBeatsElapsed();
	testStopResetsBeatCounter();
	testMuteSolo();
	testQuantize();
	testClearTrack();
	testMidiFileRoundTrip();
	testProgramChangeExport();
	testByteRoundTrip();
	testPunchReplacePreservesSurroundings();
	testReplaceModeSilentDuringTake();
	testOverdubMode();
	testLoopBar();
	testLoopPunchRestrictsRecording();
	testNewSong();
	testSongSlots();
	testCopySongSlot();
	testPrecountIsFictitious();
	testDeleteBars();
	testCopyBars();

	if (failures == 0) {
		std::printf("\nALL PASSED\n");
		return 0;
	}
	std::printf("\n%d CHECK(S) FAILED\n", failures);
	return 1;
}
