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
// what gets persisted (state XML, .midiseq), so inserting anywhere else would silently
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

// Whether quantizeTrack() actually rewrites the recorded events (hard, the original and
// still-default behaviour) or leaves them exactly as played and instead snaps them to the
// grid live, every time they're read for playback, without ever touching the track's own
// stored data (soft) - Alan's question "est-ce que ça garde l'enregistrement de départ ?"
// 2026-08-21. See renderInto()'s own comment for how soft mode is actually computed. A
// workspace-wide setting, not per-track - it changes what quantizeTrack()/the existing
// per-track grid picker DO, not a separate control of its own.
enum class QuantizeMode { hard, soft };

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
	// 7 more generic MIDI tracks (9-15, "TRACK 10".."TRACK 16") beyond the base 9 - Nonet
	// Sequencer only (see D110SequencerHost::supportsExtraTracks()), never the D-110 plugin,
	// which never calls setExtraTracksEnabled(true) and so never sees activeTrackCount() rise
	// above kNumTracks. The tracks array is always physically sized to kMaxTracks regardless
	// of host - the extra 7 empty Track slots cost nothing meaningful in the plugin, and
	// keeping one array size (rather than a second, smaller one for "extra" tracks) means
	// every existing per-track method just works unmodified once its own bound check is
	// widened to kMaxTracks.
	static constexpr int kMaxTracks = 16;
	static constexpr int kNumSongSlots = 4;   // 4 independent songs, switchable in the UI

	D110SequencerEngine();

	// Nonet Sequencer only - see kMaxTracks above. Off by default and only ever turned on by
	// a host with supportsExtraTracks() true, so activeTrackCount() stays kNumTracks (9) for
	// the plugin forever. Disabling while a track >= kNumTracks is armed/recording disarms it
	// first (same as armTrack(-1) would), so a hidden track can't keep silently capturing.
	void setExtraTracksEnabled(bool enabled);
	bool getExtraTracksEnabled() const { return extraTracksEnabled; }
	// How many of the kMaxTracks physical track slots are currently "in play" - rendered by
	// renderInto(), exported by saveMidiFile(), reachable by deleteBars()/copyBars()/
	// transposeBars()'s trackIndex<0 "every track" form. kNumTracks normally, kMaxTracks once
	// extra tracks are enabled. Content on a track beyond this is never lost (undo snapshots,
	// song-slot switches and D110SequencerSongsFile's own persistence always cover the full
	// kMaxTracks regardless of this flag) - just not currently playing/exported.
	int activeTrackCount() const { return extraTracksEnabled ? kMaxTracks : kNumTracks; }

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

	// Same idea as setProgramSource(), for Channel Volume (0-100) / Pan (0-14, 7=centre) -
	// written into the exported file as CC7/CC10 (scaled to the wire's 0-127) right after the
	// Program Change, same time-0 position. < 0 = no opinion, same meaning as setProgramSource()
	// (saveMidiFile() then just leaves that track without the corresponding CC). Alan asked for
	// this 2026-08-19 after noticing the exported file carried the Program Change but not
	// Volume/Pan.
	void setVolumeSource(std::function<int(int trackIndex)> volumeForTrack);
	void setPanSource(std::function<int(int trackIndex)> panForTrack);

	// Same idea again, Bank Select MSB/LSB (1-128 musician-facing, same numbering as
	// D110SequencerHost::getTrackBank()/getTrackBankLsb()) - written as CC0/CC32 right before
	// the Program Change, only consulted when the Program Change itself is also being written
	// (a bank with no program is meaningless). The D-110 firmware itself has no bank concept
	// (it predates Bank Select and folds A/B straight into the Program Change number itself,
	// already reflected in whatever programSource returns), but the exported file is read by
	// other software too: the D-110 plugin always writes a constant bank 1/1 (raw wire byte
	// 0/0), matching Roland-D110.idf's own hbank="0" lbank="0", so a receiver going off that
	// file resolves the patch name; Nonet Sequencer instead reads the stored per-track
	// Bank/Bank LSB (2026-08-19, Alan's request - export from the stored settings, not a
	// "live" value there being no synth to read one from).
	void setBankSource(std::function<int(int trackIndex)> bankForTrack);
	void setBankLsbSource(std::function<int(int trackIndex)> bankLsbForTrack);

	// Same idea once more: a track's exported SysEx "preamble", any custom-sound data that has
	// to reach the receiving instrument's own memory before its Program Change can select it
	// (the D-110 plugin uses this for tracks whose live Timbre is an Internal tone - Program
	// Change alone can never reach one, on a real unit either - see
	// D110AudioProcessor::buildInternalToneSysEx()). Each element of the returned vector is one
	// complete SysEx message's own payload bytes, NOT including the F0/F7 wrapper -
	// juce::MidiMessage::createSysExMessage() adds those - written in order, all at time 0,
	// ahead of the Bank Select/Program Change/Volume/Pan events above (a Program Change with no
	// data behind it yet would be meaningless). Empty vector or no callback set = nothing
	// written, the common case. D-110-agnostic like every other Source callback here - what the
	// bytes actually mean is entirely up to whatever sets this.
	void setSysExPreambleSource(std::function<std::vector<std::vector<juce::uint8>>(int trackIndex)> sysExForTrack);

	// The load-side mirror of setSysExPreambleSource() - broadened 2026-08-21 to also cover
	// Program Change/Volume/Pan, after Alan found reimporting a track changed how it sounded:
	// loadMidiFile()'s track model is note-only (see captureEvent()'s own comment), so every
	// non-note event it finds in the source file - the SysEx preamble above, but also whatever
	// Program Change/CC7 (Volume)/CC10 (Pan) saveMidiFile() wrote out, see its own comment - was
	// simply dropped on the floor, silently leaving the live instrument on whatever sound/level
	// it already happened to be on rather than what the file actually says. Not applied to
	// anything by the engine itself - D-110-agnostic like every Source/Sink here, it's purely a
	// courier, already rechannelled onto channelForTrack(trackIndex) (the CURRENT live mapping,
	// not whatever channel happened to be baked into the file - same convention renderInto()
	// uses for notes). The D-110 plugin replays these straight back into the firmware exactly
	// as if they'd just arrived over real MIDI - see D110AudioProcessor::applyLoadedTrackSetup().
	// Bank Select (CC0/CC32) is deliberately NOT included: the D-110 firmware has no Bank Select
	// concept at all (see setBankSource()'s own comment), so replaying it would be a pure no-op
	// for this instrument - only Program Change/Volume/Pan actually change anything it does.
	// Called once per track, only when at least one such event was found; not called otherwise,
	// and no callback set = everything is just dropped, same as before this existed.
	void setLoadedTrackSetupSink(std::function<void(int trackIndex, std::vector<juce::MidiMessage> setup)> setupSink);

	// Transport
	void setTempo(double bpm);
	double getTempo() const { return tempoBpm; }
	// Tap tempo: call once per tap (a button press, in either sequencer view). Two or more
	// taps within kTapResetMs of each other average into a live tempo via setTempo(); a
	// longer gap starts a fresh sequence instead of corrupting the average with a stray
	// tap, so leaving it alone for a couple of seconds and tapping again always starts
	// clean. A lone first tap doesn't change the tempo yet - there's no interval to measure.
	void registerTapTempo();
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

	// See QuantizeMode's own comment. Switching this does not, by itself, change anything
	// already on a track - hard mode already baked in stays baked in, and a track's
	// getTrackQuantize() grid (if any) just changes what it now MEANS: ignored in hard mode
	// (the events are already where they need to be), applied live in soft mode.
	void setQuantizeMode(QuantizeMode mode) { quantizeMode = mode; }
	QuantizeMode getQuantizeMode() const { return quantizeMode; }

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
	// How many steps the current step grid (getStepDuration(), x1.5 if getStepDotted()) divides
	// the current bar into - e.g. 4 for quarter-note steps in 4/4, 8 for eighth-note steps in
	// the same bar. Together with getStepIndexInBar(), lets the UI show "step M of N"/how many
	// are left, and re-subdivide a beat-based visual (metronome LEDs) to the step grid instead
	// while step recording is active.
	int getStepsPerBar() const;

	void setTrackMuted(int index, bool muted);
	bool isTrackMuted(int index) const;
	void setTrackSoloed(int index, bool soloed);
	bool isTrackSoloed(int index) const;
	bool trackHasEvents(int index) const;

	// A user-given label, empty by default - shown in the panel in place of "PART N"/
	// "RHYTHM" once set, and written into the track as a Track Name meta-event by
	// saveMidiFile() (falling back to "PART N"/"RHYTHM" there if still empty, so an
	// exported file is never left with anonymous tracks). Per-slot, like mute/solo/
	// quantize above - a name is part of what makes a song's track what it is.
	void setTrackName(int index, const juce::String &name);
	juce::String getTrackName(int index) const;

	// The fixed per-track Program Change/Bank/Bank LSB/Volume/Pan override (set by clicking a
	// track's own CH/PC readout in the sequencer panel; sent once at the PLAY/REC edge by
	// whichever host implements D110SequencerHost::supportsProgramChange()). Per-slot, like
	// mute/solo/quantize/name above (2026-08-21, Alan's explicit correction - this used to be
	// one workspace-wide value shared by all 4 songs, which he pointed out makes no sense: a
	// song's own instrumentation is part of what makes it THAT song). -1 = no override (send
	// nothing) for program/volume/pan, matching D110SequencerHost.h's own "no useful unset
	// value" reasoning for bank/bankLsb (always a real 1-128 value there).
	int getTrackProgram(int index) const;
	void setTrackProgram(int index, int program);
	int getTrackBank(int index) const;
	void setTrackBank(int index, int bank);
	int getTrackBankLsb(int index) const;
	void setTrackBankLsb(int index, int bankLsb);
	int getTrackVolume(int index) const;
	void setTrackVolume(int index, int volume);
	int getTrackPan(int index) const;
	void setTrackPan(int index, int pan);

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
	// rather than growing without bound over a long editing session. `description` is a short,
	// human-readable label for what's ABOUT TO happen (e.g. "Clear track PART 2"), surfaced by
	// getUndoDescription()/getRedoDescription() so the UI can show what a right-click on UNDO/
	// REDO would actually do. Also called internally by startRecording()/startStepRecording(),
	// which is why real-time and step takes are undoable too, not just the editing operations
	// below. Clears redoStack: a fresh edit invalidates whatever was previously undone, same as
	// undo/redo in any ordinary editor.
	void pushUndoSnapshot(const juce::String &description);
	// Restores the most recently pushed snapshot, if any; a no-op with nothing to undo. Stashes
	// what's being overwritten onto redoStack first, under the same description, so redo() can
	// bring it straight back.
	void undo();
	bool canUndo() const { return !undoStack.empty(); }
	// Re-applies the most recently undone snapshot, if any; a no-op with nothing to redo.
	void redo();
	bool canRedo() const { return !redoStack.empty(); }
	// What undo()/redo() would do right now, e.g. "Clear track PART 2" - empty string if
	// canUndo()/canRedo() is false. For a right-click tooltip/label on the UNDO/REDO control.
	juce::String getUndoDescription() const { return undoStack.empty() ? juce::String() : undoStack.back().description; }
	juce::String getRedoDescription() const { return redoStack.empty() ? juce::String() : redoStack.back().description; }

	// Read-only peek into the rest of the stack, for a "pick how many steps to undo/redo"
	// list (D110SequencerRetroPanel's OPTIONS > UNDO/REDO, Alan's request, 2026-08-23 - a
	// single "UNDO (whatever getUndoDescription() says)" row used to visually collide label
	// against value for anything longer than a few characters). stepsBack == 0 is the same
	// entry getUndoDescription()/getRedoDescription() already report (what a single
	// undo()/redo() call would do); higher stepsBack looks further back in the same stack -
	// undo()/redo() itself is still called one step at a time in a loop to actually perform a
	// multi-step jump, this is purely for listing what's there.
	int getUndoStackSize() const { return (int) undoStack.size(); }
	int getRedoStackSize() const { return (int) redoStack.size(); }
	juce::String getUndoDescriptionAt(int stepsBack) const {
		const int idx = (int) undoStack.size() - 1 - stepsBack;
		return (idx >= 0 && idx < (int) undoStack.size()) ? undoStack[(size_t) idx].description : juce::String();
	}
	juce::String getRedoDescriptionAt(int stepsBack) const {
		const int idx = (int) redoStack.size() - 1 - stepsBack;
		return (idx >= 0 && idx < (int) redoStack.size()) ? redoStack[(size_t) idx].description : juce::String();
	}

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

	// One note event, for D110SequencerPanel's event-list edit dialog - not used by
	// playback/rendering, which reads a track's own MidiMessageSequence directly.
	struct NoteEventInfo {
		int index;            // this track's own MidiMessageSequence index - pass to deleteNoteEvent()
		double beatInBar;     // 0-indexed offset from the start of whichever bar this note is in
		int note;
		int velocity;
		double durationBeats; // 0 if no matching note-off was found (shouldn't normally happen)
	};

	// Every note-on landing in [fromBar, toBarInclusive] (1-indexed, inclusive) on one track -
	// for a graphical, list-based way to remove a single wrong note without touching the rest
	// of the bar (Alan's own request: "pas un piano roll", scoped to the currently navigated
	// bar). `index` in each entry is only valid until the next edit to this track - re-fetch
	// after calling deleteNoteEvent(). Read-only: doesn't need pushUndoSnapshot() itself.
	std::vector<NoteEventInfo> eventsInBarRange(int trackIndex, int fromBar, int toBarInclusive) const;

	// Deletes one note event (and its matching note-off, if any) - index as returned by
	// eventsInBarRange(). No-op if out of range. See pushUndoSnapshot() above - the UI
	// checkpoints before calling this, same as every other destructive edit.
	void deleteNoteEvent(int trackIndex, int index);

	// Retunes one note event (and its matching note-off, if any) to newNote, clamped to
	// [0, 127] - index as returned by eventsInBarRange(). Its beat position and velocity are
	// untouched. No-op if out of range. See pushUndoSnapshot() above - the UI checkpoints
	// before calling this, same as every other destructive edit.
	void setNoteEventPitch(int trackIndex, int index, int newNote);

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
	juce::String slotTrackName(int slot, int track) const;
	void setSlotTrackName(int slot, int track, const juce::String &name);
	int slotTrackProgram(int slot, int track) const;
	void setSlotTrackProgram(int slot, int track, int program);
	int slotTrackBank(int slot, int track) const;
	void setSlotTrackBank(int slot, int track, int bank);
	int slotTrackBankLsb(int slot, int track) const;
	void setSlotTrackBankLsb(int slot, int track, int bankLsb);
	int slotTrackVolume(int slot, int track) const;
	void setSlotTrackVolume(int slot, int track, int volume);
	int slotTrackPan(int slot, int track) const;
	void setSlotTrackPan(int slot, int track, int pan);

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
		juce::String name;
		// The fixed Program Change/Bank/BankLsb/Volume/Pan override - see getTrackProgram()'s
		// own comment. Plain fields on Track, same as the others above, so every existing
		// per-slot mechanism (selectSongSlot's swap, copyCurrentSongTo, undo snapshots) already
		// handles them correctly with no extra code anywhere else.
		int program = -1;
		int bank = 1;
		int bankLsb = 1;
		int volume = -1;
		int pan = -1;
	};

	double gridBeats(QuantizeGrid grid) const;
	// The actual round-to-grid math, shared by snapTrackToGrid() (hard, writes it back) and
	// renderInto() (soft, applied on the fly to a local copy of a timestamp, never written
	// anywhere) - one definition of "what grid X does to a beat position" for both.
	double snapBeat(double beat, QuantizeGrid grid) const;
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

	std::array<Track, kMaxTracks> tracks;
	std::function<int(int)> channelSource;
	std::function<int(int)> programSource;
	std::function<int(int)> volumeSource;
	std::function<int(int)> panSource;
	std::function<int(int)> bankSource;
	std::function<int(int)> bankLsbSource;
	std::function<std::vector<std::vector<juce::uint8>>(int)> sysExPreambleSource;
	std::function<void(int, std::vector<juce::MidiMessage>)> loadedTrackSetupSink;
	bool extraTracksEnabled = false;

	// Storage for slots other than currentSlot - see selectSongSlot()'s own comment for why
	// songs[currentSlot] itself is stale/unused (the live members above are authoritative
	// for whichever slot is current).
	struct Song {
		double tempoBpm = 120.0;
		int timeSigNum = 4;
		int timeSigDen = 4;
		std::array<Track, kMaxTracks> tracks;
	};
	std::array<Song, kNumSongSlots> songs;
	int currentSlot = 0;

	// See pushUndoSnapshot()/undo()/redo() above.
	struct UndoSnapshot {
		std::array<Track, kMaxTracks> tracks;
		std::array<Song, kNumSongSlots> songs;
		int currentSlot;
		juce::String description; // e.g. "Clear track PART 2" - see getUndoDescription()
	};
	static constexpr size_t kMaxUndoDepth = 20;
	std::vector<UndoSnapshot> undoStack;
	std::vector<UndoSnapshot> redoStack;

	// "PART N"/"RHYTHM"/"TRACK N" - the user's own name if they set one, otherwise the same
	// fallback saveMidiFile() and the undo/redo descriptions both use, so a track never shows
	// up unnamed in either place.
	juce::String trackLabel(int t) const;

	double tempoBpm = 120.0;
	int timeSigNum = 4;
	int timeSigDen = 4;
	double positionBeats = 0.0;

	// Tap tempo scratch state - see registerTapTempo(). Not persisted (getStateInformation/
	// slot save-load never touch it): it's a live gesture, not a setting.
	std::vector<double> tapTimesMs;
	static constexpr double kTapResetMs = 2000.0;
	static constexpr int kTapMaxSamples = 8;

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
	QuantizeMode quantizeMode = QuantizeMode::hard;

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
