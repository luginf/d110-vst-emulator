#pragma once

// D-20-style multitrack step sequencer engine. Deliberately D-110-agnostic: it knows
// about MIDI channels, beats and sample counts, not firmware RAM, so it could be lifted
// into a different project without a rewrite. The owning D110AudioProcessor supplies
// which live MIDI channel each track maps to (see setChannelSource).
//
// Time base: a "beat" is always a quarter note, matching standard MIDI file semantics
// (tempo in BPM = quarter notes per minute) - this is what lets tracks round-trip
// through juce::MidiFile without any unit conversion beyond ticks-per-quarter-note.
// The time signature only affects bar-length/metronome-grid math, not the beat unit
// events are stored in.

#include <array>
#include <functional>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace d110seq {

enum class QuantizeGrid { off, quarter, eighth, sixteenth, eighthTriplet, sixteenthTriplet };

// How a take is folded into the armed track's existing content when it stops - see
// stopRecording()'s own comment for exactly where each one erases from/to.
enum class RecordMode {
	overdub,      // adds the new notes; nothing already there is ever removed
	replaceRange, // erases only the span actually recorded (take start to take stop)
	replaceToEnd  // erases everything from the take's start onward, wherever it stops
};

class D110SequencerEngine {
public:
	static constexpr int kNumTracks = 9;      // 0-7 = D-110 parts 1-8, 8 = rhythm
	static constexpr int kRhythmTrack = kNumTracks - 1;

	D110SequencerEngine();

	// Which live MIDI channel (1-16) a track plays on. The engine asks rather than
	// stores this itself, so it stays decoupled from the D-110's own System-area
	// channel map; the caller's function typically reads that map for tracks 0-7 and
	// returns 10 (fixed) for track 8.
	void setChannelSource(std::function<int(int trackIndex)> channelForTrack);
	int channelForTrack(int trackIndex) const;

	// Transport
	void setTempo(double bpm);
	double getTempo() const { return tempoBpm; }
	void setTimeSignature(int numerator, int denominator);
	int getTimeSigNumerator() const { return timeSigNum; }
	int getTimeSigDenominator() const { return timeSigDen; }
	double barLengthBeats() const;

	void play();
	void stop();
	bool isPlaying() const { return playing; }
	bool isRecording() const { return recording; }
	// True while a precount is rolling but capture hasn't started yet.
	bool isPrecounting() const;

	// 0 = off, 1 or 2 = that many bars of metronome-only precount before capture starts -
	// see startRecording()'s use of this.
	void setPrecountBars(int bars) { precountBars = juce::jlimit(0, 2, bars); }
	int getPrecountBars() const { return precountBars; }
	void setMetronomeEnabled(bool enabled) { metronomeEnabled = enabled; }
	bool getMetronomeEnabled() const { return metronomeEnabled; }

	// Relocates the playhead to the start of a (1-indexed) bar without changing
	// play/record state - "démarrer sur la mesure qu'on veut".
	void gotoBar(int bar);
	int getCurrentBar() const;
	int getBarCount() const;
	double getPositionBeats() const { return positionBeats; }

	// Recording. Arming selects the one track that will capture; -1 disarms all. Changing
	// which track is armed while a take is in progress commits that take first (as if
	// stopRecording() had been called), rather than discarding it.
	void armTrack(int index);
	int getArmedTrack() const { return armedTrack; }

	void setRecordMode(RecordMode mode) { recordMode = mode; }
	RecordMode getRecordMode() const { return recordMode; }

	// Starts the transport (if not already) and recording on the armed track, from the
	// currently navigated bar. If precount is on, capture begins one bar later. Captured
	// notes are held in a separate buffer, not written into the track, until stopRecording()
	// folds them in according to getRecordMode() - so the track's own already-committed
	// content keeps playing back normally for the whole take, exactly as any other track's
	// does (this is what makes overdub actually audible while you're doing it).
	void startRecording();
	// Folds the take just finished into the armed track: see RecordMode's own comments for
	// what each mode does. No-op if nothing was being recorded.
	void stopRecording();

	void setTrackMuted(int index, bool muted);
	bool isTrackMuted(int index) const;
	void setTrackSoloed(int index, bool soloed);
	bool isTrackSoloed(int index) const;
	bool trackHasEvents(int index) const;

	void quantizeTrack(int index, QuantizeGrid grid);
	QuantizeGrid getTrackQuantize(int index) const;

	struct MetronomeClick {
		int samplePosition;
		bool downbeat;
	};

	// Called once per audio block, from the audio thread. Advances the transport by
	// numSamples worth of beats at the current tempo, and emits any due note events
	// from unmuted (or, if any track is soloed, only soloed) tracks into midiMessages
	// at the correct sample offset. If clicksOut is non-null and the metronome is
	// enabled, appends any metronome clicks due in this block.
	void renderInto(juce::MidiBuffer &midiMessages, int numSamples, double sampleRate,
	                 std::vector<MetronomeClick> *clicksOut = nullptr);

	// Feeds one already-merged MIDI message into the take currently in progress, at the
	// given beat position (typically positionBeats + the message's sample offset converted
	// to beats). No-op unless currently recording and past the precount. Goes into a
	// staging buffer, not the track itself - see startRecording()'s comment.
	void captureEvent(const juce::MidiMessage &message, double atBeats);

	bool loadMidiFile(const juce::File &file);
	bool saveMidiFile(const juce::File &file) const;

	// Raw (unpacked) serialisation of one track, for the caller to fold into its own
	// state blob the same way the firmware NVRAM already is (see packBlock in
	// PluginProcessor.cpp) - this class deliberately doesn't own XML/base64 itself.
	juce::MemoryBlock trackToBytes(int index) const;
	void trackFromBytes(int index, const void *data, size_t size);

private:
	struct Track {
		juce::MidiMessageSequence events;
		bool muted = false;
		bool soloed = false;
		QuantizeGrid quantize = QuantizeGrid::off;
	};

	double gridBeats(QuantizeGrid grid) const;
	bool anySoloed() const;
	static bool isNoteEvent(const juce::MidiMessage &m) { return m.isNoteOnOrOff(); }
	Track &trackAt(int index) { return tracks[static_cast<size_t>(index)]; }
	const Track &trackAt(int index) const { return tracks[static_cast<size_t>(index)]; }

	std::array<Track, kNumTracks> tracks;
	std::function<int(int)> channelSource;

	double tempoBpm = 120.0;
	int timeSigNum = 4;
	int timeSigDen = 4;
	double positionBeats = 0.0;

	bool playing = false;
	bool recording = false;
	int precountBars = 1;
	bool metronomeEnabled = true;
	int armedTrack = -1;
	RecordMode recordMode = RecordMode::replaceRange;

	// Beat position at which capture actually starts - positionBeats at the moment
	// startRecording() was called, plus however many bars of precount.
	double recordStartBeats = 0.0;
	// Notes captured during the take in progress, in the same beat/channel shape as a
	// Track's own events - merged into the armed track by stopRecording(), per recordMode.
	juce::MidiMessageSequence recordBuffer;
};

} // namespace d110seq
