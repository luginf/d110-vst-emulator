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

9 tracks: **Part 1-8**, then **Rhythm** (16 in Nonet Sequencer, with extra tracks on - see
below). A track is not free-standing - it always plays on
whatever MIDI channel the corresponding D-110 part is *currently* set to on the SYSTEM page
(Rhythm is fixed on channel 10). This is read live at playback time, not stored per event, so
re-routing a part's channel on the Parts/System tab immediately changes which channel that
track's already-recorded notes come out on - and it's what makes copying notes from one track
to another (see **Copy bar(s)**, below) carry only the pitch/timing across, never the channel.
**In Nonet Sequencer** (see below), which has no SYSTEM page to read a channel map from,
the channel readout is clickable instead - a 1-16 picker per track, shown in a different
colour from the plugin's own read-only readout so it reads as a control there.

Each row: **MUTE**, **SOLO** (soloing any track silences every non-soloed one), **ARM** (record-
enable, drawn as a small record-style dot - filled red while armed, a hollow ring otherwise;
arming one track disarms whatever else was armed; arming mid-take commits that take first
rather than discarding it), a channel readout, and a filled/empty bar showing whether the track
has any recorded events. The far right of each row carries a bare digit ("1".."8", or "R" for
rhythm) that always identifies the part, independent of whatever the row's own label is showing.

That label is "PART N"/"RHYTHM" by default, but **right-click anywhere on a row for Rename
track...** to give it a name of your own - shown here in place of the default label, and
written into the track as a Track Name meta-event when exporting to `.mid` (falling back to
the default label if never renamed, so an exported file's tracks are never left anonymous).
Names round-trip: importing a `.mid` picks up whatever track names it already carries, and
they're saved/restored with the rest of a song's state as usual. The same right-click menu
also carries quantize and the destructive per-track operations (clear/delete/copy/transpose
bars, below).

## Transport

**STOP** / **PLAY** / **REC**, a draggable **TEMPO** field (20-300 BPM; drag vertically, mouse
wheel, or the field shows the live value), a **time signature** field (click cycles six presets
- 4/4, 3/4, 6/8, 2/4, 5/4, 7/8 - right-click picks one directly), and bar navigation
(prev/next buttons, a draggable **BAR n/total** readout).

**STOP** always sends a MIDI panic (all notes off on every channel) as well as halting the
transport, so a note whose off was scheduled past the stop point (renderInto() stops walking the
sequence the instant playback halts) never gets left stuck sounding, or stuck showing as an
active voice on the Monitor tab.

**Right-click STOP sends that same MIDI panic** without stopping the transport - a quick way out
of a stuck note without waiting for the take to end.

**Right-click PLAY starts from the beginning** (bar 1) instead of resuming from wherever the
transport currently sits.

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

## MIDI Out (driving external gear)

Whatever the sequencer plays back - the tracks themselves, plus the metronome click when it's
set to play through the rhythm channel instead of its own click sound - also reaches the direct
**MIDI Out** port, if one is selected (right-click the panel, *MIDI Out* submenu; the same menu
also has *MIDI In*, for an external controller). This is a real OS MIDI port, independent of
whatever the host routes in and out, so it works identically in the Standalone app and inside a
DAW - plug in a MIDI interface and the sequencer can drive a real D-110, or any other synth,
alongside (or instead of) the emulation. Each track still goes out on whatever channel its D-110
part is live on (`channelForTrack`), same as internally. **Right-click STOP**'s MIDI panic also
reaches this port, since a stuck note on real hardware has no "stop the plugin" to fall back on.

Only the sequencer's own output takes this path in the plugin - host MIDI, the on-screen
keyboard, and the *MIDI In* port itself are not echoed to *MIDI Out* there, since the plugin's
own D-110 emulation is always available to hear what you're playing regardless. The independent
sequencer app below has no such emulation, and does thru MIDI In to MIDI Out for that reason. A
VST3 MIDI output bus a host could route on its own, without a physical cable, remains a possible
follow-up for the *plugin* specifically, distinct from the standalone app right below, which
already needs no firmware loaded at all.

## Nonet Sequencer - the independent app

**Nonet Sequencer** (binary `Nonet-Seq`, CMake target `Nonet-Seq`, source in
`NonetSeqMain.cpp`/`NonetSeqHost.h/.cpp`) is the sequencer on its own - no firmware, no
ROMs, no plugin wrapper, not even a sound engine. Named apart from the D-110 on purpose:
it's a plain 9-track MIDI sequencer (a nonet), not tied to any one instrument. It's the
same `D110SequencerPanel` and `D110SequencerEngine` as inside the plugin, in a bare window
with a three-field toolbar (a small LED, lit while a message is actually arriving on
**MIDI In**; **MIDI In**, **MIDI Out** - click either to pick a system MIDI port, same
device lists as the plugin's own Options menu - and **THEME**, this app's mini utility
field, click to flip the window's light/dark palette; its label always shows the theme
actually in effect, never a fixed word). Every track defaults to the factory D-110
channel map (Part 1-8 -> MIDI channels 2-9, Rhythm -> 10) so a real D-110 on its own
factory defaults just works from this app's MIDI Out without reconfiguring either side;
driving something else, point its own parts/tracks at the same channels instead.
Unlike the plugin's own MIDI Out (sequencer playback only, see above), this app also
**thru's MIDI In straight to MIDI Out** - there's no internal synth here to hear what
you're playing while you record, so without this a live controller would be silent.
Whatever arrives on MIDI In is rechannelized onto the on-screen keyboard's own selected
channel first (unless it's set to Omni), exactly like the plugin's Standalone MIDI In port
does - so a physical USB controller follows the keyboard's CH picker the same way the
virtual/PC keyboard already did, instead of always keeping whatever channel it sends on.

Underneath the transport is the same on-screen test keyboard the plugin has
(`D110Keyboard`, see `D110Keyboard.h`, extracted out of `PluginEditor.*` behind a small
`D110KeyboardHost` interface so both this app and the plugin can own one) - a two-octave
mouse piano plus optional tracker-style PC keyboard input, right-click for its own MIDI
routing menu (channel 1-16 or omni, i.e. which of the app's own MIDI Out channels a struck
key targets - independent of which channel a track records on). Notes played on it go
through the same MIDI In collector a real port's notes do, so they thru to MIDI Out and get
captured while a track is armed, exactly like a real controller plugged into MIDI In.
Its keys light up for two independent reasons: struck directly here (mouse or PC-tracker
key - instant, no polling) or any note reaching the app another way - external MIDI In or
sequencer playback - polled from the same activity a small lock-free array records at ~30Hz,
so a note played by a remote controller or by the sequencer itself shows on the keyboard the
same as one played by hand. `midiPanic()` (STOP's all-notes-off) clears it along with
everything else.

Its own state (all 4 song slots, MIDI port choice, theme, keyboard routing, and the same
transport preferences the plugin persists) lives in its own settings file, separate from
the plugin's:

- Linux: `~/.config/Nonet Sequencer.settings`
- macOS: `~/Library/Application Support/Nonet Sequencer.settings`
- Windows: `%APPDATA%\Nonet Sequencer.settings`

Everything else - recording, step recording, quantize, bar editing, undo, song slots,
Load/Save - works exactly as described above, since it's the same panel and engine code.
Deliberately Standalone only: there's no VST3/plugin-format build of this one.

### Extra tracks (10-16)

Nonet Sequencer only - the D-110 plugin's own sequencer stays fixed at 9 tracks (Parts 1-8 +
Rhythm), since that's all a D-110 part-wise structure has any use for. **Right-click the blank
strip right of BACK, above the track rows,** for **Activate extra tracks (16 total)** - once
on, two buttons appear there, **1-9** and **10-16**, switching the track list between the
original 9 and 7 more plain MIDI tracks (**TRACK 10** .. **TRACK 16**, defaulting to whatever
channels the first 9 don't already use: 1, then 11-16). Both buttons stay hidden - and the
track list stays the ordinary 9 rows - until extra tracks are turned on; turning them back off
jumps the view back to the 1-9 page automatically. Extra tracks work exactly like the other 9
- MUTE/SOLO/ARM, per-track channel (click the CH readout), rename, quantize, bar editing,
recording, step recording, MIDI Out, `.mid`/`.midiseq` export-import - undo, song slots and
`.midiseq` save/load always cover all 16 regardless of whether the toggle is on, so a track's
own content is never lost by switching it off and back on, only hidden from view/playback
meanwhile.

Timing comes from a real (silent) audio device callback rather than a GUI timer, the same
reasoning as the native CPU core's own move off MAME's non-audio-thread stepping (see
`CLAUDE.md`) - this app's whole reason to exist is MIDI timing accuracy for external gear,
and a hardware-clocked callback has far less jitter than the message thread does. If no
output device is available at all, it falls back to a plain timer instead (degraded, but
still usable).

### Per-track Program Change

Nonet Sequencer only, same reasoning as the per-track channel edit above: the plugin's
tracks feed the live firmware directly, which already has its own patch per part, so
there's nothing for a Program Change to usefully select there. Click a track's **CH**
readout (same entry point as changing its channel) for a **Program Change...** item at the
bottom of that menu - pick a program 1-128, or leave it blank for none (a trailing `*` on
the CH readout marks a track that has one set). Whichever tracks have a program set get it
sent, once, over MIDI Out the moment **PLAY** or **REC** starts (precount included, so an
external synth has already switched patch before any notes arrive) - not re-sent mid-song,
since a track only ever holds one fixed program for the whole song slot.

## Load / Save

A plain click on **LOAD**/**SAVE** loads/saves just the *current* song as a standard `.mid`
file (a Program Change is written ahead of each track's notes, from whatever sound is live on
that part at export time). **Right-click** LOAD/SAVE for all 4 song slots at once, as a single
portable `.midiseq` file - an XML wrapper around a gzip-compressed standard MIDI file per
track, plus the transport preferences below; not a raw memory snapshot, hence the name
(renamed from `.d110songs` - a file saved under the old name still loads, it's the same
format either way).

Every file dialog in the app - these two, plus SysEx bank import/export and the memory
snapshot save/load on the panel's own Options menu - shares one "last used folder", updated
on every successful pick and offered as the starting point for the next dialog, in any of
them, instead of each one independently resetting to the OS default. Persisted between runs
the same way the rest of this state is.

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
