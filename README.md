# D-110 VST Emulator

A VST3 plugin that emulates the Roland D-110 multi-timbral sound module.

**It runs the D-110's real Roland firmware.** The menus, the display, the patch and timbre
editors and all sixteen front-panel buttons are the hardware's own - nothing about them is a
reimplementation. That half comes from [MAME](https://github.com/mamedev/mame)'s `roland_d10`
driver, which emulates the i8x9x CPU, the MSM6222B display controller and the panel's scan
matrix. MAME has no LA32 emulation for any Roland LA machine, so the **sound** comes from
[munt](https://github.com/munt/munt) (specifically
[davidhsilaban's D-110 fork](https://github.com/davidhsilaban/munt/tree/davidhsilaban-d110-changes-with-kode54-super-mode)),
whose LA engine is mature and accurate. The two halves are joined by mirroring the firmware's
own parameter memory into the sound engine as Roland exclusive messages, so an edit made on the
panel is audible - see [`docs/sysex_address_map.md`](docs/sysex_address_map.md).

**The firmware also decides every note you hear.** Notes are not handed to the sound engine
directly: they go into the firmware, which applies its own key ranges, part assignment and
voice allocation, and the plugin reads back which note it started on which part. That is what
lets the instrument's **own ROM demo songs play** - hold EDIT and ENTER for ROM Play, then
ENTER - since the firmware generates those internally and never transmits them.

The panel itself is a photograph of the hardware, with invisible hit-regions at its own pixel
coordinates; only the LCD and the MIDI MESSAGE lamp are drawn, and the LCD's glyphs come from
the emulated controller's own mask character ROM. See
[`docs/panel_reference_notes.md`](docs/panel_reference_notes.md).

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
  21 dB down. See [`docs/timbre_group_5.md`](docs/timbre_group_5.md).
- **System** - partial reserve and the per-part MIDI channel map, so the SYSTEM page's channel
  assignment actually takes effect.

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
- **PATCHES** — the 64 stored patches. **Clicking a number selects that patch on the
  instrument**, by pressing its own PATCH / BANK / NUMBER buttons — so the display, the parts
  and the sound follow exactly as they do by hand. Below the list are that patch's own eight
  part assignments.
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
See [`docs/sysex_address_map.md`](docs/sysex_address_map.md).

Host MIDI is delivered to **both** halves, as one cable feeds both on the real instrument. The
firmware therefore sees what you play: the top LCD row replaces a part's digit with a solid
block while that part is sounding, exactly as the hardware does, and the display follows
program changes sent by the DAW.

> The offline harness in `plugin/audio_test.cpp` reports this check unreliably, and its verdict
> should not be trusted: it renders audio far faster than real time while the control board runs
> on its own thread at real time, so its MIDI arrives in bursts and its LCD reads race the
> firmware. Confirmed working in a live DAW, which is the environment that matters.

The firmware's memory is **one file beside the ROMs**, as the instrument has one battery RAM,
and it is written when the plugin is switched off - so close the host and reopen it and your
patches are where you left them. Your project saves a copy too, so reloading a session brings
back the sounds it was saved with rather than whatever the file has since become.

**The memory card slot works.** A seated card shows its edge in the slot, so an occupied socket
looks different from an empty one at a glance. Click the slot and the M-256D slides down and
out over about a second, its label passing as it goes, and the firmware notices: `Save to Card`
then answers `Card Not Ready`. Click again and it seats itself, and the card's `Save` / `Load` /
format functions behave as the hardware's do. There is
no card-detect line on a real D-110 - the firmware recognises a card by writing a byte and
reading it back, and an empty socket is a bus that reads `0xFF` and takes no writes - so that is
exactly what an ejected card is here. The card keeps its own 32 KB file, separate from the
instrument's battery RAM, and travels with your project the same way. See
[`docs/memory_card.md`](docs/memory_card.md).

### Known limits

- **Only one instance can be switched on at a time.** MAME reaches its machine through a
  process-wide singleton, so a second running machine corrupts the host's heap - measured, not
  assumed. A second instance therefore refuses to power on and says so, instead of crashing your
  DAW. Loading several is fine; only one may be on.
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
  [`docs/sysex_address_map.md`](docs/sysex_address_map.md).
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
- **Master Tune** is not mirrored either: the firmware's scale and the engine's disagree, so
  passing the byte across would detune everything against what the display says.
- The output saturates at full scale rather than exceeding it, because a 16-bit DAC cannot do
  otherwise and the VOLUME knob is analogue and sits after it. The sound engine models that
  clamp for 16-bit output and deliberately skips it on its floating-point path, so the plugin
  applies it before the knob. Past the knob's midpoint you are asking for gain above the
  instrument's own maximum, as with a mixer fader.
- Master tune, reverb and master volume are deliberately not mirrored - see
  [`docs/sysex_address_map.md`](docs/sysex_address_map.md) for exactly why each one is excluded.

## Requirements

You need your own **MAME `d110` ROM set** - copyrighted Roland firmware, **not included** here.
Put the files loose into:

- Windows: `C:\Program Files\Common Files\VST3\D-110 Data\`
- macOS: `~/Library/Audio/Plug-Ins/VST3/D-110 Data/`
- Linux: `~/.vst3/D-110_Data/`

That one set serves both halves: the control board takes the firmware, the presets and the
character generator from it, and the sound engine's Control and PCM images are assembled from
the same chip dumps in memory. Files are recognised by **content, not by name**.

Already-combined `mt32emu`-style Control and PCM images are accepted too, as is the romset
still in its `.zip`. Whatever you have, drop it in.

Right-click the panel to see what was recognised.

> **Note:** by factory default **nothing plays on MIDI channel 1**. Part 1 listens on channel
> **2**, part 2 on 3, and so on to part 8 on channel 9, with rhythm on 10. That is how the
> hardware behaves, not a bug - see
> [`docs/factory_defaults.md`](docs/factory_defaults.md) for the full factory state (channels,
> pan, key ranges, partial reserve) read straight off the firmware's own display.

The very first time the plugin is ever used it performs the documented cold start once, so it
comes up at factory settings instead of showing the empty patch a D-110 with blank battery RAM
really does show. The result is kept and every instance created afterwards is seeded from it,
so **loading the plugin never makes you sit through initialisation**.

To initialise it again later, do what you would do on the hardware - **hold WRITE/COPY while
switching on, then confirm with ENTER**:

1. Switch **POWER** off.
2. **Ctrl+click** (or Alt+click) **WRITE/COPY**. The cap latches down, exactly as if you were
   holding it.
3. Switch **POWER** on. The firmware comes up seeing the button held and asks to confirm.
4. Ctrl+click **WRITE/COPY** again to release it.
5. Click **ENTER** to confirm.

The order of the last two steps matters and is easy to get backwards - release WRITE/COPY
**before** confirming with ENTER, not after. Confirming while the cap is still latched down
does nothing, because the firmware is waiting to see WRITE/COPY come back up before it will
read ENTER as the answer to its own prompt.

Ctrl+click latches any of the sixteen caps, which is what makes "hold this button while
switching on" possible with a single mouse. It is on a modifier rather than on a long press on
purpose: **the firmware repeats a held button**, so a long press is how you scroll a value, and
that has to keep working.

## Project layout

- `munt/` - the sound engine (`mt32emu`), vendored from the fork above with a couple of local
  fixes (see `munt/mt32emu/src/Display.cpp` for the LCD buffer/part-count corrections made for D-110).
- `plugin/Source/D110Core.*` - the control board: runs MAME's `d110` machine on its own thread
  with a headless OSD, and exposes the display, the sixteen buttons and the firmware's parameter
  memory. Needs no patched MAME.
- `plugin/Source/PluginProcessor.*`, `PluginEditor.*` - the JUCE plugin and the photo-composite panel.
- `plugin/mame.cmake` - the MAME library/include/define lists and how the subset was built.
- `docs/` - the measured panel geometry and the SysEx address map, both derived by profiling
  rather than by eye. Every number in the code is justified there.
- `rom_test/` - a console tool that renders a test chord from a Control + PCM ROM pair.
- `plugin/audio_test.cpp` - offline check with no DAW: powers the plugin on, plays a note, proves
  a panel edit changes the sound, checks that re-sending an *unedited* state changes nothing, and
  sweeps all sixteen MIDI channels reading the part indicators off the firmware's own display.
- `plugin/tone_probe.cpp` - locates the Tone Temporary Area by reading the engine's copy of each
  tone back out and matching it against the firmware's RAM. An exact match is simultaneously the
  measurement and the null test.
- `plugin/tone_edit_survives_probe.cpp` - edits a tone from the panel on an ordinary patch, then
  changes a part parameter, and checks the sound engine still holds the edited tone. Finds both
  menu pages by pressing buttons and watching which RAM byte moves, and runs the experiment twice
  - with the tone re-assert on and off - so the result has a control that can show the failure.
- `plugin/state_test.cpp` - edits a parameter, saves the plugin state, restores it into a fresh
  instance, and checks the edit came back; also checks a plugin loaded with no saved state finds
  the memory the last one left, and that a second instance refuses to power on.
- `plugin/two_instance_test.cpp` - records what really happens when two machines run in one
  process. Diagnostic, not a fix.
- `plugin/panel_render.cpp` - snapshots the real panel to PNG, as a storyboard every 100 ms,
  so the card's travel is judged by looking at it rather than from constants. It drives the
  card by clicking the slot, so the whole path from the mouse to the frame is what gets checked.
- `plugin/card_probe.cpp` - the memory card. Puts four different cards in the slot, each
  differing from the next by one property, and prints what the firmware said about each; then
  formats a blank card, saves to it, wipes the instrument with a factory reset and loads it back,
  comparing three snapshots of the battery RAM. See [`docs/memory_card.md`](docs/memory_card.md).
- `plugin/bridge_probe.cpp`, `core_test.cpp` - the harnesses used to map the firmware's RAM and
  to exercise the control board on its own.

## Building

Requires CMake and a C++ compiler (Visual Studio Build Tools on Windows; GCC and the SDL OSD's
dependencies on Linux - `libsdl2-dev libsdl2-ttf-dev libfontconfig1-dev libpulse-dev` cover it on
Debian/Ubuntu). JUCE is fetched automatically by CMake on first configure.

**MAME is not vendored here and must be built first**, because its libraries are what run the
firmware. From a [MAME 0.288](https://github.com/mamedev/mame/releases/tag/mame0288) tree,
apply the one required patch below - it fixes a real crash, an out-of-bounds array access
reachable from this project's own EXTINT workaround, not just a cosmetic difference - then
build:

Windows:
```
cd <mame-tree>
git apply <this-repo>/patches/mame_mcs96_stale_irq_level.patch
make vs2022 MSBUILD=1 PTR64=1 MINGW64=C:/msys64/mingw64 MINGW32=C:/msys64/mingw32 \
     NOWERROR=1 PYTHON_EXECUTABLE=python SUBTARGET=d110 \
     SOURCES=src/mame/roland/roland_d10.cpp USE_BGFX=0
```

Linux (uses MAME's own default SDL OSD, no extra flags needed):
```
cd <mame-tree>
git apply <this-repo>/patches/mame_mcs96_stale_irq_level.patch
make SUBTARGET=d110 SOURCES=src/mame/roland/roland_d10.cpp -j$(nproc)
```

See [`patches/README.md`](patches/README.md) for what the patch fixes and why it's required.

Then point `MAME_DIR` in [`plugin/mame.cmake`](plugin/mame.cmake) at that tree (or pass
`-DMAME_DIR=...` on the CMake command line) and build:

```
cd plugin
cmake -B build -S .
cmake --build build --config Release
```

On Windows, everything is built with the static runtime (`/MT`) to match MAME's release
libraries. The built `.vst3` is copied automatically to the platform's shared VST3 folder
(`C:\Program Files\Common Files\VST3` on Windows, `~/Library/Audio/Plug-Ins/VST3` on macOS,
`~/.vst3` on Linux).

## Legal Notice

This is an independent open-source software project. It is not affiliated
with, endorsed by, sponsored by, or approved by Yamaha Corporation, Roland
Corporation, Ensoniq Corporation, or any other trademark owner. All
trademarks remain the property of their respective owners.

No copyrighted Roland firmware, ROM images, or other proprietary binary
files are included in or distributed with this repository - you must
obtain and supply your own legally acquired Control ROM and PCM Wave ROM
dumps (see Requirements above).

This project incorporates third-party open-source code (munt/mt32emu,
JUCE); all original copyright notices and license headers have been
preserved. See:

- [LICENSE](LICENSE) - this project's own license (AGPLv3)
- [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) - full third-party license details
- [CREDITS.md](CREDITS.md) - acknowledgements
- [DISCLAIMER.md](DISCLAIMER.md) - the full disclaimer text
