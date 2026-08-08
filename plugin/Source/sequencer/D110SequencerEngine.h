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

// Appended after thirtySecond, not alphabetised/reordered - the enum's integer value is
// what gets persisted (state XML, .d110songs), so inserting anywhere else would silently
// reinterpret every existing save's quantize/step-duration setting as the wrong grid. half and
// whole exist for step recording (see setStepDuration()) but, since this enum is shared with
// quantizeTrack(), also become quantize-to-a-half/whole-note-grid options - unusual, but
// harmless to leave available rather than splitting the enum in two.
enum class QuantizeGrid {
	off,
	quarter,
	eighth,
	sixteenth,
	eighthTriplet,
	sixteenthTriplet,
	thirtySecond,
	half,
	whole
};

// How a take is folded into the armed track's existing content when it stops - see
// stopRecording()'s own comment for exactly where each one erases from/to.
enum class RecordMode {
	overdub,      // adds the new notes; nothing already there is ever removed
	replaceRange, // erases only the span actually recorded (take start to take stop)
	replaceToEnd  // erases everything from the take's start onward, wherever it stops
};

// The LOOP button's 3-position state. "bar" loops whatever bar navigation last landed on
// (see gotoBar()); "punch" loops the [punchIn, punchOut] range and, while active, also
// confines captureEvent() to that same range - one range, shared by playback looping and
// punch-in/out recording, per Alan's own framing of the request.
enum class LoopMode { off, bar, punch };

// What an enabled metronome actually shows - the LED strip, the audible click, or both. See
// D110SequencerEngine::setMetronomeMode().
enum class MetronomeMode { visualOnly, audioOnly, both };

class D110SequencerEngine {
public:
	static constexpr int kNumTracks = 9;      // 0-7 = D-110 parts 1-8, 8 = rhythm
	static constexpr int kRhythmTrack = kNumTracks - 1;
	static constexpr int kNumSongSlots = 4;   // 4 independent songs, switchable in the UI

	D110SequencerEngine();

	// Which live MIDI channel (1-16) a track plays on. The engine asks rather than
	// stores this itself, so it stays decoupled from the D-110's own System-area
	// channel map; the caller's function typically reads that map for tracks 0-7 and
	// returns 10 (fixed) for track 8.
	void setChannelSource(std::function<int(int trackIndex)> channelForTrack);
	int channelForTrack(int trackIndex) const;

	// Which Program Change value (0-127), if any, represents the sound a track is
	// currently set to - the caller typically reads the D-110's own live tone number for
	// that part. Unlike channelForTrack, this is allowed to have no opinion (return < 0,
	// e.g. for the rhythm track, which has no single "current program" the way a melodic
	// part does) - saveMidiFile() then just leaves that track without one. Read only at
	// save time, not stored in the track's own events, so it always reflects whatever's
	// live on the instrument at the moment of exporting, not whatever it was when notes
	// were recorded - see saveMidiFile()'s own comment for why that's the intended
	// behaviour ("modifiable by changing the first bar" means changing the instrument's
	// live sound before the next export, there being no piano-roll here to edit a
	// specific inserted event by hand).
	void setProgramSource(std::function<int(int trackIndex)> programForTrack);

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

	// Whether an enabled metronome shows as the LED strip, the audible click, or both -
	// independent of the METRO button's own on/off (that stays a single click enabling
	// whichever of these is currently selected). Right-click on the METRO button to change
	// this - see D110SequencerPanel::showMetronomeModeMenu().
	void setMetronomeMode(MetronomeMode mode) { metronomeMode = mode; }
	MetronomeMode getMetronomeMode() const { return metronomeMode; }

	// Most DAWs' "click only when recording": when on, an enabled metronome still shows/
	// sounds normally while recording, but stays silent and its LED strip stays dark during
	// plain playback. Right-click the METRO button to change this.
	void setMetronomeRecordOnly(bool enabled) { metronomeRecordOnly = enabled; }
	bool getMetronomeRecordOnly() const { return metronomeRecordOnly; }

	// GM2's own dedicated percussion-channel notes for exactly this purpose (regular beat /
	// downbeat), rather than anything D-110-specific - so a metronome routed through
	// channelForTrack(kRhythmTrack) still makes sense on a GM2-compatible synth later, not
	// just this plugin's own rhythm map.
	static constexpr int kMetronomeClickNote = 33; // GM2: Metronome Click
	static constexpr int kMetronomeBellNote = 34;   // GM2: Metronome Bell (downbeat)

	// When on, metronome clicks are ALSO (in place of, not in addition to - see
	// PluginProcessor's own click mixing) sent as real note on/off pairs on the rhythm
	// channel, instead of only the internal synthesized click - useful for hearing the
	// metronome through an actual percussion patch, D-110 or otherwise.
	void setMetronomeUseChannel10(bool enabled) { metronomeUseChannel10 = enabled; }
	bool getMetronomeUseChannel10() const { return metronomeUseChannel10; }

	// Scales both the internal click's amplitude and the channel-10 notes' velocity. 1.0 is
	// the level the click always used before this existed.
	void setMetronomeVolume(float volume) { metronomeVolume = juce::jlimit(0.0f, 1.5f, volume); }
	float getMetronomeVolume() const { return metronomeVolume; }

	// Click-grid geometry shared with renderInto()'s own metronome-audio math (a quarter in
	// 4/4, an eighth in 6/8, ...) - exposed so a visual metronome (an LED-per-click strip)
	// can stay in lockstep with the audible clicks without duplicating the grid logic.
	int clicksPerBar() const;
	int currentClickInBar() const;

	// How many click-grid units have elapsed since the precount started (0, 1, 2, ... - NOT
	// wrapped to the bar, unlike currentClickInBar(), since a precount visual only needs to
	// detect "a new beat just happened" to flash on it, not which beat of the bar it is).
	// positionBeats itself is frozen throughout precount (see startRecording()'s own comment),
	// so this is what a caller like D110SequencerPanel polls instead, watching for it to
	// increase, to flash the downbeat LED once per precount beat even without audio.
	int precountBeatsElapsed() const;

	// Relocates the playhead to the start of a (1-indexed) bar without changing
	// play/record state - "démarrer sur la mesure qu'on veut". Also updates the bar-loop
	// anchor (see LoopMode::bar) - loop-on-current-bar always follows the last navigated bar.
	void gotoBar(int bar);
	int getCurrentBar() const;
	int getBarCount() const;
	double getPositionBeats() const { return positionBeats; }

	// Snaps the playhead into range if it's currently outside the mode being switched to,
	// so engaging LOOP always starts looping immediately rather than waiting to be reached.
	void setLoopMode(LoopMode mode);
	LoopMode getLoopMode() const { return loopMode; }

	// 1-indexed bar range used by LoopMode::punch, for both playback looping and (while that
	// mode is active) restricting captureEvent() to the same span. Setting punch-in past the
	// current punch-out drags punch-out along with it, and vice versa, so the range never
	// inverts.
	void setPunchIn(int bar);
	int getPunchIn() const { return punchInBar; }
	void setPunchOut(int bar);
	int getPunchOut() const { return punchOutBar; }
	// Sets both ends at once - unlike the single setters above, doesn't drag one end along
	// with the other, so a dialog submitting both values together can't fight itself.
	void setPunchRange(int inBar, int outBar);

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

	// Step (non-real-time) recording: instead of playing in tempo, one step's worth of notes is
	// entered at a time - play a note or chord and let go of it (or call stepRest() for
	// silence), and the write cursor advances by getStepDuration() worth of beats. Reuses
	// QuantizeGrid to express a step's length, since "one step = a quarter/eighth/sixteenth/..."
	// is exactly what that enum already models - no separate grid type needed (QuantizeGrid::off
	// is not a valid step duration; setStepDuration() substitutes quarter for it). Works on the
	// same armed track as real-time recording (armTrack()) and is mutually exclusive with it -
	// starting one stops the other, same as armTrack() already does mid-take.
	void setStepDuration(QuantizeGrid grid);
	QuantizeGrid getStepDuration() const { return stepGrid; }

	// Multiplies the current step's length by 1.5 (a dotted note - e.g. dotted half = 3 beats)
	// - independent of getStepDuration() rather than a separate set of "dotted" QuantizeGrid
	// values, so the grid list doesn't have to double to cover every base duration's dotted
	// form. A plain toggle like MUTE/SOLO/ARM, not a one-shot modifier: it stays on across
	// steps (and applies to stepRest() too - a dotted rest is a real notation concept) until
	// switched off again.
	void setStepDotted(bool dotted) { stepDotted = dotted; }
	bool getStepDotted() const { return stepDotted; }

	// Starts step recording on the currently armed track, writing from the current bar's start
	// (see gotoBar()). No-op if no track is armed. Does not touch play/pause state - the
	// transport is typically left stopped during step entry, the same way real hardware step
	// sequencers work, but nothing here enforces that.
	void startStepRecording();
	// Commits whatever notes are still held (if any - see stepNoteOn()) rather than discarding
	// them, then leaves step mode. No-op if not currently step recording.
	void stopStepRecording();
	bool isStepRecording() const { return stepRecording; }

	// Feeds one note on/off into the step currently being entered - same event shape as
	// captureEvent(), but with no beat position: step recording doesn't care when in real time a
	// note was played, only that it was held. Chords: hold several notes down together, they all
	// land on the SAME step - the step commits and the cursor advances automatically once every
	// note that was part of it has been released (not just the one currently reported).
	void stepNoteOn(int noteNumber, int velocity);
	void stepNoteOff(int noteNumber);
	// Advances the cursor by one step without recording anything - a rest. No-op while any note
	// from the current step is still held (finish the chord first).
	void stepRest();
	// Undoes the most recently committed step (whatever notes were on it, or a rest) and moves
	// the cursor back by one step, so a wrong note can be fixed without restarting step entry
	// from the top. No-op at the very start of the take.
	void stepBack();

	// 1-indexed bar, and 1-indexed step within that bar, the write cursor currently sits on -
	// for the UI to show "where you are" during step entry the same way getCurrentBar() does for
	// the transport. Not the same position: step recording never touches positionBeats, so
	// leaving step mode always drops you back exactly wherever the transport was, the same way a
	// precount leaves it untouched (see startRecording()'s own comment).
	int getStepBar() const;
	int getStepIndexInBar() const;

	void setTrackMuted(int index, bool muted);
	bool isTrackMuted(int index) const;
	void setTrackSoloed(int index, bool soloed);
	bool isTrackSoloed(int index) const;
	bool trackHasEvents(int index) const;

	// Undo for the editing operations below (quantizeTrack, clearTrack, deleteBars, copyBars,
	// transposeBars, newSong, copyCurrentSongTo) - none of them checkpoints on its own; the
	// caller (the UI) calls pushUndoSnapshot() right before applying one, exactly where it
	// already shows a confirmation dialog for the destructive ones, so undo() always reverts
	// whichever of those the user actually did most recently. A snapshot captures every song
	// slot's tracks plus which slot is current - copyCurrentSongTo() is the one operation that
	// touches a slot that isn't live, so a snapshot has to cover all of them, not just the
	// current one. Playback/transport state (position, playing, armed track, ...) is
	// deliberately NOT captured, so undoing an edit never disturbs what's currently rolling.
	// Capped at kMaxUndoDepth entries - the oldest snapshot is dropped once the stack is full,
	// rather than growing without bound over a long editing session.
	void pushUndoSnapshot();
	// Restores the most recently pushed snapshot, if any; a no-op with nothing to undo.
	void undo();
	bool canUndo() const { return !undoStack.empty(); }

	void quantizeTrack(int index, QuantizeGrid grid);
	QuantizeGrid getTrackQuantize(int index) const;
	// Erases every recorded event on one track only, leaving its mute/solo/quantize state and
	// every other track untouched - unlike newSong(), which wipes the whole current slot. See
	// pushUndoSnapshot() above - the UI checkpoints before calling this.
	void clearTrack(int index);

	// Removes [fromBar, toBarInclusive] (1-indexed, inclusive) from one track (trackIndex >= 0)
	// or every track at once (trackIndex < 0), closing the gap by shifting everything after the
	// range earlier by its length. Ripples independently per track: deleting on a single track
	// only shifts that track's own later content - it will then read a different bar number
	// than the other tracks from that point on, by design (Alan's own call). See
	// pushUndoSnapshot() above - the UI checkpoints before calling this.
	void deleteBars(int trackIndex, int fromBar, int toBarInclusive);

	// Copies [fromBar, toBarInclusive] (1-indexed, inclusive) from srcTrack, inserting it at
	// destBar on destTrack: destBar and everything already at/after it on destTrack is pushed
	// later first, by the copied range's length, so nothing already there is overwritten - the
	// destination track (or every track, see below) grows by that many bars. srcTrack ==
	// destTrack == -1 copies every track's own [fromBar, toBarInclusive] to the same destBar,
	// applied independently per track, which is what keeps them aligned with each other for a
	// whole-song copy. Copying onto a different track only ever carries the notes across, never
	// the source track's channel - see channelForTrack(), always re-applied at render time from
	// whichever track index the notes end up living on, regardless of what they were recorded
	// with. See pushUndoSnapshot() above - the UI checkpoints before calling this.
	void copyBars(int srcTrack, int destTrack, int fromBar, int toBarInclusive, int destBar);

	// Transposes every note in [fromBar, toBarInclusive] (1-indexed, inclusive) on one track
	// (trackIndex >= 0) or every track at once, independently (trackIndex < 0), by semitones.
	// Applied in place - a note's own track and position never change, unlike copyBars(), so
	// there is no destination track/bar to choose. Clamps the resulting pitch to [0, 127]
	// rather than wrapping or dropping the note. See pushUndoSnapshot() above - the UI
	// checkpoints before calling this.
	void transposeBars(int trackIndex, int fromBar, int toBarInclusive, int semitones);

	// Clears every track in the CURRENT slot (events, mute/solo/quantize all reset) and
	// stops/rewinds/disarms - "new song" within the currently selected slot. Transport
	// preferences (tempo, time signature, loop/punch, precount, metronome) are left alone,
	// since those read as workspace settings rather than song content. See pushUndoSnapshot()
	// above - the UI checkpoints before calling this.
	void newSong();

	// Switches which of the kNumSongSlots slots is live: the outgoing slot's tempo/time
	// signature/tracks are written back to storage, the target slot's become live, and the
	// transport stops/rewinds/disarms, the same as loading a different pattern usually
	// should. A no-op if slot is already current.
	void selectSongSlot(int slot);
	int getCurrentSongSlot() const { return currentSlot; }
	bool songSlotHasContent(int slot) const;

	// Copies the CURRENT slot's tempo, time signature and every track into destSlot,
	// overwriting whatever was stored there - a shortcut for starting the next song from a
	// copy of this one instead of rebuilding matching tracks by hand or round-tripping through
	// a .mid export/import. The current slot itself, and what's currently playing, are left
	// untouched either way - only destSlot's stored data changes. No-op if destSlot is already
	// the current slot. See pushUndoSnapshot() above - the UI checkpoints before calling this.
	void copyCurrentSongTo(int destSlot);

	// Per-slot accessors that read/write ANY slot's data without switching which one is
	// live - used only by the plugin's own state persistence (see PluginProcessor's
	// get/setStateInformation) to save and restore all four slots, including the ones not
	// currently selected. UI code should use the plain, current-slot accessors above
	// instead (setTempo(), quantizeTrack(), trackToBytes(), ...).
	double slotTempo(int slot) const;
	void setSlotTempo(int slot, double bpm);
	int slotTimeSigNumerator(int slot) const;
	int slotTimeSigDenominator(int slot) const;
	void setSlotTimeSignature(int slot, int numerator, int denominator);
	juce::MemoryBlock slotTrackToBytes(int slot, int track) const;
	void slotTrackFromBytes(int slot, int track, const void *data, size_t size);
	bool slotTrackMuted(int slot, int track) const;
	void setSlotTrackMuted(int slot, int track, bool muted);
	bool slotTrackSoloed(int slot, int track) const;
	void setSlotTrackSoloed(int slot, int track, bool soloed);
	QuantizeGrid slotTrackQuantize(int slot, int track) const;
	void setSlotTrackQuantize(int slot, int track, QuantizeGrid grid);

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
	// gridBeats(stepGrid), x1.5 if stepDotted - the actual length of the step about to be
	// committed, shared by commitStepInternal(), stepRest() and stepBack().
	double currentStepBeats() const { return gridBeats(stepGrid) * (stepDotted ? 1.5 : 1.0); }
	// Writes whatever's in stepHeldNotes into the armed track at stepPositionBeats, advances the
	// cursor by one step, and clears the held set - shared by the auto-commit in stepNoteOff()
	// and the commit-on-stop in stopStepRecording().
	void commitStepInternal();
	static bool isNoteEvent(const juce::MidiMessage &m) { return m.isNoteOnOrOff(); }
	Track &trackAt(int index) { return tracks[static_cast<size_t>(index)]; }
	const Track &trackAt(int index) const { return tracks[static_cast<size_t>(index)]; }
	// Any slot's track, without switching what's live - trackAt() itself (the live tracks
	// array) for slot == currentSlot, the stored copy in songs[] otherwise. Shared by the
	// plain per-track accessors (via trackAt) and the slot* ones above.
	Track &songTrackAt(int slot, int track);
	const Track &songTrackAt(int slot, int track) const;
	// Shared by quantizeTrack()/setSlotTrackQuantize() and by trackToBytes()/
	// trackFromBytes()/slotTrackToBytes()/slotTrackFromBytes() - the actual grid-snap and
	// MidiFile (de)serialisation logic, decoupled from which Track it's operating on.
	void snapTrackToGrid(Track &track, QuantizeGrid grid) const;
	juce::MemoryBlock serializeTrack(const juce::MidiMessageSequence &events) const;
	void deserializeTrack(juce::MidiMessageSequence &events, const void *data, size_t size) const;

	std::array<Track, kNumTracks> tracks;
	std::function<int(int)> channelSource;
	std::function<int(int)> programSource;

	// Storage for slots other than currentSlot - see selectSongSlot()'s own comment for why
	// songs[currentSlot] itself is stale/unused (the live members above are authoritative
	// for whichever slot is current).
	struct Song {
		double tempoBpm = 120.0;
		int timeSigNum = 4;
		int timeSigDen = 4;
		std::array<Track, kNumTracks> tracks;
	};
	std::array<Song, kNumSongSlots> songs;
	int currentSlot = 0;

	// See pushUndoSnapshot()/undo() above.
	struct UndoSnapshot {
		std::array<Track, kNumTracks> tracks;
		std::array<Song, kNumSongSlots> songs;
		int currentSlot;
	};
	static constexpr size_t kMaxUndoDepth = 20;
	std::vector<UndoSnapshot> undoStack;

	double tempoBpm = 120.0;
	int timeSigNum = 4;
	int timeSigDen = 4;
	double positionBeats = 0.0;

	bool playing = false;
	bool recording = false;
	int precountBars = 1;
	bool metronomeEnabled = true;
	MetronomeMode metronomeMode = MetronomeMode::both;
	bool metronomeRecordOnly = false;
	bool metronomeUseChannel10 = false;
	float metronomeVolume = 1.0f;
	int armedTrack = -1;
	RecordMode recordMode = RecordMode::replaceRange;

	LoopMode loopMode = LoopMode::off;
	int loopBar = 1;      // bar anchor for LoopMode::bar, kept in sync by gotoBar()
	int punchInBar = 1;
	int punchOutBar = 1;

	// Beat position at which capture actually starts - positionBeats at the moment
	// startRecording() was called, UNCHANGED by precount (see precountRemainingBeats: the
	// count-in is fictitious, so the take starts on the bar the transport was already on, not
	// however many bars later).
	double recordStartBeats = 0.0;
	// > 0 while a fictitious count-in is playing: renderInto() spends samples ticking this
	// down (metronome only, positionBeats frozen, nothing captured or played back) before
	// falling through to normal playback/recording for whatever's left of the block. Zero
	// means "not precounting" - this, not a positionBeats comparison, is what isPrecounting()
	// reads.
	double precountRemainingBeats = 0.0;
	// Notes captured during the take in progress, in the same beat/channel shape as a
	// Track's own events - merged into the armed track by stopRecording(), per recordMode.
	juce::MidiMessageSequence recordBuffer;

	// See setStepDuration()/startStepRecording() and friends above.
	bool stepRecording = false;
	QuantizeGrid stepGrid = QuantizeGrid::eighth;
	bool stepDotted = false;
	// Where the NEXT committed step will land, in beats - advanced by commitStepInternal()/
	// stepRest(), rewound by stepBack(). Independent of positionBeats (see startStepRecording()'s
	// own comment).
	double stepPositionBeats = 0.0;
	struct StepHeldNote {
		int note;
		int velocity;
		bool stillDown;
	};
	// Every note played as part of the step currently being entered, whether or not it's still
	// physically held - stepNoteOff() only flips stillDown, it never removes an entry, so a note
	// released early is still part of the chord once the rest are released too. Cleared by
	// commitStepInternal().
	std::vector<StepHeldNote> stepHeldNotes;
	// Beat length of each step committed so far in this take, oldest first - a stack stepBack()
	// pops from, so it always rewinds exactly what was applied even if getStepDuration() has
	// since changed mid-take.
	std::vector<double> stepLengths;
};

} // namespace d110seq
