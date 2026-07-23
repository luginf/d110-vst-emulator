# D-110 VST Emulator

A VST3 plugin that emulates the Roland D-110 multi-timbral sound module, built on top of
[munt](https://github.com/munt/munt) (specifically
[davidhsilaban's D-110 fork](https://github.com/davidhsilaban/munt/tree/davidhsilaban-d110-changes-with-kode54-super-mode))
and [JUCE](https://juce.com/).

The front panel is a from-scratch recreation of the real D-110's layout (buttons, LCD, labels),
driven live by the underlying `mt32emu` synthesis engine - not a static image.

## Requirements

You need your own dump of the D-110's **Control ROM** and **PCM Wave ROM** - these are copyrighted
Roland firmware and are **not included** in this repository. Drop them into:

- Windows: `C:\Program Files\Common Files\VST3\D-110 Data\`
- macOS: `~/Library/Audio/Plug-Ins/VST3/D-110 Data/`
- Linux: `~/.vst3/D-110_Data/`

and the plugin will detect and load them automatically (by content, not filename) the next time
a new instance is created.

### Building the ROM files from individual chip dumps

If you only have the raw per-chip dumps from the D-110 mainboard (as found in MAME's `d110`
romset, `roland_d10.cpp`), they need to be merged into two single files first - the plugin
(and `rom_test`) expect one combined Control ROM and one combined PCM ROM, matched by exact
size and checksum, not by filename:

- **Control ROM** (163840 bytes) = `d-110.v1.10.ic19.bin` + `r15179873-lh5310-97.ic12.bin`,
  concatenated in that order. Use the **v1.10** `ic19` dump - v1.06 isn't recognised by this
  fork.
- **PCM ROM** (1048576 bytes) = `r15179880.ic8.bin` + `r15179878.ic7.bin`, concatenated in
  **that order** (note: this is the reverse of the `ic7`/`ic8` byte order used in MAME's own
  `la32` memory region).

`r15179879.ic6.bin` (the "BOSS" reverb processor chip) isn't used by this emulator.

```
cat d-110.v1.10.ic19.bin r15179873-lh5310-97.ic12.bin > D-110_Control.bin
cat r15179880.ic8.bin r15179878.ic7.bin > D-110_PCM.bin
```

Verify the result with `rom_test` before dropping the files into the folder above - it prints
a clear recognition message or error, whereas the plugin just silently shows "no ROMs loaded"
on a mismatch:

```
./rom_test D-110_Control.bin D-110_PCM.bin test.wav
```

## Project layout

- `munt/` - the emulation engine (`mt32emu`), vendored from the fork above with a couple of local
  fixes (see `munt/mt32emu/src/Display.cpp` for the LCD buffer/part-count corrections made for D-110).
- `plugin/` - the JUCE/CMake VST3 plugin itself (`Source/PluginProcessor.*`, `Source/PluginEditor.*`).
- `rom_test/` - a small standalone console tool that loads a Control ROM + PCM ROM and renders a
  test chord to a `.wav` file, useful for verifying a ROM dump works before building the full plugin.

## Building

Requires CMake and a C++ compiler (Visual Studio Build Tools on Windows). JUCE is fetched
automatically by CMake on first configure.

```
cd plugin
cmake -B build -S .
cmake --build build --config Release
```

The built `.vst3` is copied automatically to the platform's shared VST3 folder
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
