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

The plugin opens **powered off**, as a rack unit does. Click POWER and the firmware boots live,
in real time.

## What reaches the sound

Every parameter block the firmware keeps is mirrored into the sound engine, so the deep editor
pages are real and not decoration:

- **Timbre Temporary** - which timbre each part plays, key shift, fine tune, bender range,
  assign mode, output level, pan.
- **Tone Temporary** - the tone itself: structure, partial mute, waveforms, and the pitch, TVF
  and TVA envelopes, for all eight parts. Editing `Tone Edit / Structure 1&2` moves the sound
  immediately.
- **System** - partial reserve and the per-part MIDI channel map, so the SYSTEM page's channel
  assignment actually takes effect.

Host MIDI is delivered to **both** halves, as one cable feeds both on the real instrument. The
firmware therefore sees what you play: the top LCD row replaces a part's digit with a solid
block while that part is sounding, exactly as the hardware does, and the display follows
program changes sent by the DAW.

> The offline harness in `plugin/audio_test.cpp` reports this check unreliably, and its verdict
> should not be trusted: it renders audio far faster than real time while the control board runs
> on its own thread at real time, so its MIDI arrives in bursts and its LCD reads race the
> firmware. Confirmed working in a live DAW, which is the environment that matters.

Each instance keeps its **own** firmware memory, and your project saves it. Reload a session and
the patches and edits come back with it, rather than whatever a shared folder last held.

### Known limits

- **Only one instance can be switched on at a time.** MAME reaches its machine through a
  process-wide singleton, so a second running machine corrupts the host's heap - measured, not
  assumed. A second instance therefore refuses to power on and says so, instead of crashing your
  DAW. Loading several is fine; only one may be on.
- **The reverb is not the D-110's.** A real D-110 reverberates in a dedicated BOSS DSP with its
  own 32 KB ROM (`r15179879.ic6.bin`, the romset's `boss` region), and the firmware picks its
  program through bits 1-2 of the SO register. Nothing emulates that chip, here or in MAME, so
  what you hear is the sound engine's MT-32 reverb instead - a different unit with four modes
  where the D-110's panel offers eight types plus OFF. The panel's Reverb Type, Time and Level
  are therefore deliberately not mirrored; see
  [`docs/sysex_address_map.md`](docs/sysex_address_map.md). The per-part Reverb Switch does
  reach the engine.
- The real D-110 has eight **individual outputs** as well as the stereo mix, and a per-part
  assignment for them. This plugin is stereo only: the sound engine models the MT-32, which had
  no individual outputs at all, so there is nothing to route them from.
- **Master Tune** is not mirrored either: the firmware's scale and the engine's disagree, so
  passing the byte across would detune everything against what the display says.
- Dense material can peak slightly above full scale. The panel's VOLUME knob is the remedy, as
  it is on the hardware.
- Master tune, reverb and master volume are deliberately not mirrored - see
  [`docs/sysex_address_map.md`](docs/sysex_address_map.md) for exactly why each one is excluded.

## Requirements

You need your own **MAME `d110` ROM set** - copyrighted Roland firmware, **not included** here.
Put the files loose into:

```
C:\Program Files\Common Files\VST3\D-110 Data\
```

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
4. Click **ENTER**.
5. Ctrl+click WRITE/COPY again to release it.

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
- `plugin/state_test.cpp` - edits a parameter, saves the plugin state, restores it into a fresh
  instance, and checks the edit came back; also checks two instances get separate memory and that
  the second refuses to power on.
- `plugin/two_instance_test.cpp` - records what really happens when two machines run in one
  process. Diagnostic, not a fix.
- `plugin/bridge_probe.cpp`, `core_test.cpp` - the harnesses used to map the firmware's RAM and
  to exercise the control board on its own.

## Building

Requires CMake and a C++ compiler (Visual Studio Build Tools on Windows). JUCE is fetched
automatically by CMake on first configure.

**MAME is not vendored here and must be built first**, because its libraries are what run the
firmware. From an unmodified [MAME 0.288](https://github.com/mamedev/mame/releases/tag/mame0288)
tree - no source patches are needed:

```
make vs2022 MSBUILD=1 PTR64=1 MINGW64=C:/msys64/mingw64 MINGW32=C:/msys64/mingw32 \
     NOWERROR=1 PYTHON_EXECUTABLE=python SUBTARGET=d110 \
     SOURCES=src/mame/roland/roland_d10.cpp USE_BGFX=0
```

Then point `MAME_DIR` in [`plugin/mame.cmake`](plugin/mame.cmake) at that tree and build:

```
cd plugin
cmake -B build -S .
cmake --build build --config Release
```

Everything is built with the static runtime (`/MT`) to match MAME's release libraries. The
built `.vst3` is copied automatically to `C:\Program Files\Common Files\VST3`.

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
