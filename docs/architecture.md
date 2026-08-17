# How it works

> **Note (2026-08-05):** the original upstream repository this project is forked from
> ([PatriotBY/d110-vst-emulator](https://github.com/PatriotBY/d110-vst-emulator)) is currently
> unavailable - removed or made private by its author. This fork's `main` branch is up to date
> with the last commit we pulled from it before that happened, and all Linux-port work continues
> here. If the upstream project reappears, we intend to reconcile with it as before.

**It runs the D-110's real Roland firmware.** The menus, the display, the patch and timbre
editors and all sixteen front-panel buttons are the hardware's own - nothing about them is a
reimplementation. That half comes from [MAME](https://github.com/mamedev/mame)'s `roland_d10`
driver, which emulates the i8x9x CPU, the MSM6222B display controller and the panel's scan
matrix (or, in the default native backend, a from-scratch port of that same CPU with zero MAME
dependency - see the Building doc). MAME itself has no LA32 emulation for any Roland LA machine,
so the **sound** comes from [munt](https://github.com/munt/munt) (specifically
[davidhsilaban's D-110 fork](https://github.com/davidhsilaban/munt/tree/davidhsilaban-d110-changes-with-kode54-super-mode)),
whose LA engine is mature and accurate. The two halves are joined by mirroring the firmware's
own parameter memory into the sound engine as Roland exclusive messages, so an edit made on the
panel is audible - see [`sysex_address_map.md`](sysex_address_map.md).

**The firmware also decides every note you hear.** Notes are not handed to the sound engine
directly: they go into the firmware, which applies its own key ranges, part assignment and
voice allocation, and the plugin reads back which note it started on which part. That is what
lets the instrument's **own ROM demo songs play** - hold EDIT and ENTER for ROM Play, then
ENTER - since the firmware generates those internally and never transmits them.

The panel itself is a photograph of the hardware, with invisible hit-regions at its own pixel
coordinates; only the LCD and the MIDI MESSAGE lamp are drawn, and the LCD's glyphs come from
the emulated controller's own mask character ROM. See
[`panel_reference_notes.md`](panel_reference_notes.md).

The plugin opens **already switched on**: the firmware boots live, in real time, as soon as
the project loads - not fast-forwarded, and not waiting for a click. Click POWER to switch
it off, exactly as the hardware's own switch does, and click it again to boot it back up.

## What reaches the sound

Every parameter block the firmware keeps is mirrored into the sound engine, so the deep editor
pages are real and not decoration:

- **Timbre Temporary** - which timbre each part plays, key shift, fine tune, bender range,
  assign mode, output level, pan.
- **Tone Temporary** - the tone itself: structure, partial mute, waveforms, and the pitch, TVF
  and TVA envelopes, for all eight parts. Editing `Tone Edit / Structure 1&2` moves the sound
  immediately. It is re-sent after every Timbre Temporary change, because writing a timbre
  makes the sound engine reload that part's tone from one of its four MT-32 banks - and a bank
  holds the factory tone, not the one you just edited. Without it **any** tone edit is lost the
  moment you touch a part parameter: measured on an ordinary patch, renaming a tone and then
  moving Key Shift left the engine playing the factory sound again. On the first demo song the
  same fault was audible rather than subtle, because that song's tone group is one the four
  banks cannot name at all, so two parts played a closed hi-hat instead of a lead and measured
  21 dB down. See [`timbre_group_5.md`](timbre_group_5.md).
- **System** - partial reserve, the per-part MIDI channel map and Master Tune, so the SYSTEM
  page's channel assignment and tuning actually take effect.

## The extended editor

Under the instrument is a **drawer that opens downwards** — click the handle strip beneath
the panel. It is drawn in code rather than photographed, and deliberately so: the panel is a
photo-composite because it depicts a thing that exists, while a D-110 has no editor at all.
What it has is a two-line, sixteen-character display, and reaching one of a partial's
fifty-eight values through it takes dozens of button presses.

Nine tabs, and everything on them is the instrument's own memory:

- **PARTS** — what it is playing right now: tone group and tone (with the tone's name), level,
  pan, key shift, fine tune, bender range, assign mode, reverb switch and key range, for all
  eight parts and rhythm.
- **TONE** — the 246-byte tone a part is playing: structures, partial mute, envelope mode, a
  row per partial, and the chosen partial in full — pitch, waveform, PCM sample, the pitch,
  TVF and TVA envelopes and the LFO. The name can be typed straight in.
- **RHYTHM** — the rhythm setup, one row per drum key, all 85 of them.
- **PATCHES** — two sub-tabs. **ALL PATCHES** lists the 64 stored patches — **clicking a
  number selects that patch on the instrument**, by pressing its own PATCH / BANK / NUMBER
  buttons, so the display, the parts and the sound follow exactly as they do by hand.
  Selecting a patch this way does **not** switch sub-tabs on its own (it used to; Alan found
  the auto-jump disorienting while browsing patches by ear) — switch to **PARTS OF PATCH**
  yourself to see or edit that patch's own eight part assignments. Split into two full-height
  sub-tabs (rather than a fixed vertical split of both) so neither is clipped when the drawer
  is resized short.
- **TIMBRES** — the 128 stored timbres; clicking one sends that program change on the chosen
  part's own MIDI channel, as an external keyboard would.
- **TONES** — the 64 internal tone slots, with STORE and RECALL against the part's tone.
- **SYSTEM** — master tune, reverb, partial reserve and the MIDI channel map.
- **MONITOR** — the firmware's own LA32 voice-slot table, which parts the engine is holding,
  the bridge's message counters and a MIDI-in tape.
- **UTILITY** — a message for the instrument's display, and SysEx bank import.

**Nothing in the drawer writes to the sound engine.** Every field sends the *instrument* a
Roland exclusive message through its own MIDI IN, exactly as an external editor would; the
firmware changes its memory and the mirror carries that to the engine. An edit made here and
an edit made on the panel are therefore the same event, and each shows up in both places.

That this works is measured rather than assumed — `plugin/editor_write_probe.cpp` sends one
write into each area and reports which byte of the battery RAM moved, which is also how the
two areas nobody had located were found (Timbre Memory at `0x2994`, Tone Memory at `0x4000`);
`plugin/editor_test.cpp` then checks each editor field end to end, proves an edit is audible
against a control measurement, and checks that a patch click lands on the patch asked for.
See [`sysex_address_map.md`](sysex_address_map.md).

Host MIDI is delivered to **both** halves, as one cable feeds both on the real instrument. The
firmware therefore sees what you play: the top LCD row replaces a part's digit with a solid
block while that part is sounding, exactly as the hardware does, and the display follows
program changes sent by the DAW.

> The offline harness in `plugin/audio_test.cpp` reports this check unreliably, and its verdict
> should not be trusted: it renders audio far faster than real time while the control board runs
> on its own thread at real time, so its MIDI arrives in bursts and its LCD reads race the
> firmware. Confirmed working in a live DAW, which is the environment that matters.

## Firmware memory and plugin settings

The firmware's memory is **one file beside the ROMs**, as the instrument has one battery RAM.
It is written on an explicit POWER OFF, and (Standalone only, since 2026-08-12) also on plain
quit even if you never powered off - so close the app or the host and reopen it and your
patches are where you left them either way. In a DAW host, your project saves a copy of that
same memory too, so reloading a session brings back the sounds it was saved with rather than
whatever the shared file has since become.

**The Standalone app deliberately does *not* do the same project-style round trip.** Its own
settings file used to also embed a firmware-RAM copy, restored unconditionally at every
launch - which meant a settings file that hadn't been re-saved in a while (any quit path that
skipped JUCE's own save-state hook) could silently overwrite fresher on-disk memory the moment
the app next started, before you touched anything. Real data loss, reported and fixed
2026-08-12: the Standalone now relies solely on the battery-RAM file above (kept current by
the POWER OFF/quit flush described above), and never round-trips memory through its own
settings file at all. A DAW project, by contrast, genuinely should carry the instrument's
exact state with it, so this round trip is unchanged there.

**Moving to a different machine also means copying a second, separate file.** The battery RAM
above only holds the firmware's own memory - patches, timbres, system settings, the memory
card. Everything belonging to the *plugin itself* instead - the light/dark theme, the editor
drawer's height, and the sequencer's 4 song slots (see [`sequencer.md`](sequencer.md)) - lives
in the Standalone build's own settings file, which the NVRAM folder does not include:

- Linux: `~/.config/D-110 Emulator.settings`
- macOS: `~/Library/Application Support/D-110 Emulator.settings`
- Windows: `%APPDATA%\D-110 Emulator.settings`

If a song written on one machine isn't showing up on another after copying the NVRAM folder
over, check that this settings file (theme/song slots, not the instrument's own memory - see
above) made the trip too, or use the sequencer's own right-click **LOAD**/**SAVE** on the
transport strip to export/import all 4 song slots as a single portable `.midiseq` file instead
(a plain click there still saves/loads just the current song as a standard `.mid`).

**The battery RAM above can also be captured as a standalone file directly**, independent of
either the NVRAM folder or a DAW project: Utility tab -> **SAVE SNAPSHOT...**/**LOAD
SNAPSHOT...** writes/reads the exact same image (every Patch, Timbre, Tone, System byte, plus
the memory card) to a portable file you can keep or hand to someone else, loading one powers
the instrument off and back on with that memory in place so the change is felt immediately.
The sequencer's own **per-song sound snapshot** (see
[`sequencer.md`](sequencer.md#per-song-sound-snapshot-d-110-plugin-only)) captures the exact
same image, just keyed to one of the 4 song slots and stored inside the project instead of a
separate file - so each song can carry its own instrument state, recalled with the same
power-cycle-and-replace approach, without you needing a snapshot file per song on disk.

## The memory card slot

**The memory card slot works.** A seated card shows its edge in the slot, so an occupied socket
looks different from an empty one at a glance. Click the slot and the M-256D slides down and
out over about a second, its label passing as it goes, and the firmware notices: `Save to Card`
then answers `Card Not Ready`. Click again and it seats itself, and the card's `Save` / `Load` /
format functions behave as the hardware's do. There is
no card-detect line on a real D-110 - the firmware recognises a card by writing a byte and
reading it back, and an empty socket is a bus that reads `0xFF` and takes no writes - so that is
exactly what an ejected card is here. The card keeps its own 32 KB file, separate from the
instrument's battery RAM, and travels with your project the same way. See
[`memory_card.md`](memory_card.md).

## Known limits

- **Only one instance of the MAME-backed `D110Emulator` can be switched on at a time.** MAME
  reaches its machine through a process-wide singleton, so a second running machine corrupts
  the host's heap - measured, not assumed. A second instance therefore refuses to power on and
  says so, instead of crashing your DAW. Loading several is fine; only one may be on. This does
  **not** apply to the default `D110EmulatorNative` backend, which has no such singleton and
  runs any number of instances independently.
- **The reverb is not the D-110's.** A real D-110 reverberates in a dedicated BOSS DSP with its
  own 32 KB ROM (`r15179879.ic6.bin`, the romset's `boss` region), and the firmware picks its
  program through bits 1-2 of the SO register. Nothing emulates that chip, here or in MAME, so
  what you hear is the sound engine's MT-32 reverb instead - a different unit with four modes
  where the D-110's panel offers eight types plus OFF. **Reverb Time and Level do reach it**,
  and so does the per-part Reverb Switch: the firmware keeps the live values at the same
  System Area offsets the engine models, with the same 0-7 ranges, and they follow the patch
  as they do on the hardware (measured: setting Level from 0 to 7 on the panel moves the tail
  after a released note by 25 dB). **Reverb Type is deliberately not mirrored** - eight types
  onto four modes has no honest mapping - so the character of the room is the engine's choice,
  while its length and amount are the instrument's. See
  [`sysex_address_map.md`](sysex_address_map.md).
- The real D-110 has six **individual outputs** as well as the stereo mix, and a per-part
  assignment for them - `MIX OUT L/R` and `MULTI OUT 1-6` on the service notes' block diagram,
  fed by time-slicing one DAC. This plugin is stereo only: the sound engine models the MT-32,
  which had no individual outputs at all, so there is nothing to route them from. The
  assignment itself is real and the drawer edits it, because it is the instrument's own byte -
  it simply has nowhere to go here. (Six, not eight: stepped through on the panel, Output
  Assign reads `MIX` then `1`…`6` and stops, which the block diagram agrees with.)
- **A D-110 has no per-part reverb switch.** The byte an MT-32 uses for one is Output Assign
  here - measured on the panel's own Timbre Edit page, where three presses moved it from `MIX`
  to `3`. The sound engine reads that byte as its reverb switch, and every value the D-110 can
  put there is non-zero, so the engine's reverb stays on; nothing is lost, because there was
  never a switch to lose. Reverb on a D-110 is per patch, not per part.
- The output saturates at full scale rather than exceeding it, because a 16-bit DAC cannot do
  otherwise and the VOLUME knob is analogue and sits after it. The sound engine models that
  clamp for 16-bit output and deliberately skips it on its floating-point path, so the plugin
  applies it before the knob. Past the knob's midpoint you are asking for gain above the
  instrument's own maximum, as with a mixer fader.
- Reverb type and master volume are deliberately not mirrored - see
  [`sysex_address_map.md`](sysex_address_map.md) for exactly why each one is excluded. (Master
  Tune used to be on this list too; it is now mirrored, verified directly against the sound
  engine's own pitch computation.)
