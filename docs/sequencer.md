# The sequencer

A D-20-style multitrack MIDI sequencer, the third foldable drawer under the panel (alongside
the extended editor and the on-screen test keyboard), closed by default. It has its own
internal clock - it is **not** synced to the host's tempo or transport, the same way the
original hardware's own sequencer wasn't - so it behaves identically in the VST3 and the
Standalone build.

The engine (`plugin/Source/sequencer/D110SequencerEngine.h/.cpp`) is deliberately D-110-agnostic:
it only knows about MIDI channels, beats and note events, not firmware RAM, so none of this
depends on the emulator underneath it. The UI (`D110SequencerPanel.h/.cpp`) is the only piece
that talks to the rest of the plugin, through a `channelForTrack` callback.

## Tracks and channels

9 tracks: **Part 1-8**, then **Rhythm**. A track is not free-standing - it always plays on
whatever MIDI channel the corresponding D-110 part is *currently* set to on the SYSTEM page
(Rhythm is fixed on channel 10). This is read live at playback time, not stored per event, so
re-routing a part's channel on the Parts/System tab immediately changes which channel that
track's already-recorded notes come out on - and it's what makes copying notes from one track
to another (see **Copy bar(s)**, below) carry only the pitch/timing across, never the channel.

Each row: **MUTE**, **SOLO** (soloing any track silences every non-soloed one), **ARM** (record-
enable - arming one track disarms whatever else was armed; arming mid-take commits that take
first rather than discarding it), a channel readout, and a filled/empty bar showing whether the
track has any recorded events.

## Transport

**STOP** / **PLAY** / **REC**, a draggable **TEMPO** field (20-300 BPM; drag vertically, mouse
wheel, or the field shows the live value), a **time signature** field (click cycles six presets
- 4/4, 3/4, 6/8, 2/4, 5/4, 7/8 - right-click picks one directly), and bar navigation
(prev/next buttons, a draggable **BAR n/total** readout).

**Right-click STOP sends a MIDI panic** (all notes off on every channel) instead of stopping the
transport - a quick way out of a stuck note without waiting for the take to end.

Right-click the **BAR** readout for: *Go to bar...*, *Set punch in/out here* or *...*, and the
all-tracks forms of **Delete bar(s)**, **Copy bar(s) to...** and **Transpose bar(s)** (see
below).

### Metronome, precount, loop and punch

**METRO** toggles the metronome; right-click it for *Visual only* (an LED strip under the
transport) / *Audio only* / *Both*, whether it also fires real notes on the rhythm channel
instead of (or alongside) its own click sound, whether it's silent during plain playback and
only sounds while recording, and a volume submenu (25-150%).

**PRECOUNT** cycles 0 -> 1 -> 2 bars of click-only lead-in before a take actually starts
capturing. The count-in is *fictitious*: it never advances the bar counter, so a take always
starts on the bar you navigated to, not however many bars later.

**LOOP** cycles OFF -> BAR (repeats whatever bar you're on) -> PUNCH (loops the
[punch-in, punch-out] range set from the BAR readout's right-click menu). While **LOOP: PUNCH**
is active, recording is additionally *restricted* to that same range - notes played outside it
are not captured.

## Recording

### Real-time

Arm a track, press **REC** (or click it while stopped - REC starts the transport too). The
**REC: mode** field (click to cycle, right-click for descriptions) picks how the take is folded
into the track once you stop:

- **Overdub** - adds the new notes; nothing already on the track is removed. The track's
  existing content keeps playing back audibly while you record over it.
- **Replace** - erases only the span you actually recorded (from where the take started to
  wherever you stopped it).
- **Replace to end** - erases everything on the track from the take's start onward, however far
  that turns out to be - a deliberate "wipe the rest of the track" tool.

In either replace mode, the track goes **silent for the whole take**, not just the part that
will end up erased - hearing the very material you're about to overwrite would be confusing, and
`stopRecording()` doesn't know the actual erased span until the take ends.

### Step recording

A second way to enter notes, with no regard for real time: play a note or chord and let go of
it (or press **REST** for silence), and the write cursor advances by one step. Useful for
patterns that are awkward to play in tempo, or built one note at a time.

- **STEP** arms/leaves step mode on the currently armed track (same ARM as real-time
  recording). Starting step recording stops a real-time take in progress, and vice versa - the
  two are mutually exclusive.
- The **step duration** field sets how far one step advances: whole, half, quarter, eighth,
  sixteenth, eighth triplet, sixteenth triplet, or 1/32 (click cycles, right-click picks
  directly).
- **DOT** multiplies the current step's length by 1.5 (a dotted half = 3 beats), the same way it
  would in notation, without needing a separate dotted entry for every duration. It's a toggle,
  like MUTE/SOLO/ARM - it stays on across steps (and affects **REST** too - a dotted rest is a
  real notation concept) until switched off again.
- **Chords**: hold several notes down together - they all land on the *same* step. The step only
  commits, and the cursor only advances, once *every* note that was part of it has been
  released, not just whichever one happens to lift first. **REST** is a no-op while any note
  from the current chord is still held - finish releasing it first.
- **BACK** undoes the most recently committed step (a note, a chord, or a rest) and moves the
  cursor back by one, so a wrong note can be fixed without restarting the take. It always
  rewinds exactly what was applied, even if the step duration or DOT has since changed.
- A live **"Bar n step m"** readout shows where the cursor currently is. Step recording never
  touches the transport's own playhead - leaving step mode always drops you back exactly where
  the transport was, the same way a precount leaves it untouched.

Notes played while either recording mode is active are still audible through the firmware as
they're entered - capture is a side channel, not a detour.

## Quantize

Right-click any track row for **Quantize**: off, 1/4, 1/8, 1/16, 1/8 triplet, 1/16 triplet, or
1/32. Snaps every note-on to the nearest grid line and carries its note-off along by the same
offset, so note length survives the snap.

## Editing bars

From the same right-click menu (per-track) or the BAR readout's menu (every track at once):

- **Clear track** - erases every recorded event on one track, leaving mute/solo/quantize and
  every other track untouched.
- **Delete bar(s)** - removes a bar range and closes the gap by shifting everything after it
  earlier. Scoped to one track, it ripples *only that track* - it can end up reading a different
  bar number than the others from that point on, by design, rather than trying to keep every
  track's bar numbering in lockstep.
- **Copy bar(s) to...** - copies a bar range and **inserts** it at a destination bar, pushing
  whatever's already there later to make room (never overwrites). Can target a different track
  than the source - only the notes travel, never the source track's channel, since a track's
  channel is always read live from `channelForTrack` regardless of which track the notes end up
  on. Copying "every track" applies the same range/destination independently per track, which is
  what keeps them aligned with each other for a whole-song copy.
- **Transpose bar(s)** - shifts every note's pitch in a bar range by a number of semitones
  (negative to go down), in place - no destination to choose, since the notes stay on whatever
  track and bar they were already on. Clamped to the valid MIDI note range rather than wrapping.

**NEW** clears every track in the current song slot (tempo/time signature/other transport
settings are left alone) - "new song" within the slot you're on.

## Undo

**UNDO** (dimmed when there's nothing to undo) reverts the most recent of: quantize, clear
track, delete/copy/transpose bars, new song, or copy-song-to-slot. Up to 20 levels deep. It does
not touch playback/transport state (position, playing, armed track) - only track/song content,
so undoing an edit never disturbs what's currently rolling.

## Song slots

4 independent songs (tempo, time signature and all 9 tracks each), switched with the 4 numbered
buttons - a small dot marks which slots have content. **Right-click any slot button** to copy
the *current* song into one of the other three (flagged "(overwrites it)" if that slot already
has content) - a shortcut for starting the next song from a copy of this one.

## Load / Save

A plain click on **LOAD**/**SAVE** loads/saves just the *current* song as a standard `.mid`
file (a Program Change is written ahead of each track's notes, from whatever sound is live on
that part at export time). **Right-click** LOAD/SAVE for all 4 song slots at once, as a single
portable `.d110songs` file.

The sequencer's state (all 4 slots, plus transport preferences like tempo, metronome, loop mode)
persists in the plugin's own project save the same way the firmware's NVRAM does - see
[`architecture.md`](architecture.md#firmware-memory-and-plugin-settings) for exactly where that
data lives (and why it's a *separate* file from the firmware ROM/NVRAM one, in the Standalone
build in particular).

## Verification

`plugin/sequencer_probe.cpp` (target `d110_sequencer_probe`) is the headless test suite: it
drives the engine directly - no plugin, no firmware - feeding synthetic MIDI in at known beat
positions and checking played-back events land at the exact sample offset the tempo/block size
predicts, for every feature above (timing, quantize, both recording modes, loop/punch, delete/
copy/transpose bars, step recording including chords/rest/back/dotted durations, undo, song
slots). `plugin/sequencer_state_probe.cpp` (target `d110_sequencer_state_probe`) round-trips the
whole engine through `getStateInformation`/`setStateInformation` and checks it comes back
identical.
