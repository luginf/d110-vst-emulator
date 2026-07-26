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

The panel itself is a photograph of the hardware, with invisible hit-regions at its own pixel
coordinates; only the LCD and the MIDI MESSAGE lamp are drawn, and the LCD's glyphs come from
the emulated controller's own mask character ROM. See
[`docs/panel_reference_notes.md`](docs/panel_reference_notes.md).

The plugin opens **powered off**, as a rack unit does. Click POWER and the firmware boots live,
in real time.

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

> **Note:** by factory default Part 1 listens on **MIDI channel 2**, not 1 - that is how the
> hardware behaves, not a bug.

On a fresh install the firmware's memory is blank and the display shows an empty patch. Use
**Factory Reset** on the right-click menu to have the firmware rebuild its patch and timbre
memory from the preset ROM - it is the documented cold start (hold Write/Copy across a reset,
confirm with Enter), performed for you.

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
- `plugin/audio_test.cpp` - offline check with no DAW: powers the plugin on, plays a note, and
  proves a panel edit changes the sound by measuring the pitch before and after.
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
