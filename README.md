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

```
C:\Program Files\Common Files\VST3\D-110 Data\
```

and the plugin will detect and load them automatically (by content, not filename) the next time
a new instance is created.

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

The built `.vst3` is copied automatically to `C:\Program Files\Common Files\VST3`.

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
