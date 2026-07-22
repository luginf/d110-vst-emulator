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
%LOCALAPPDATA%\Programs\Common\VST3\D-110 ROMs\
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

The built `.vst3` is copied automatically to `%LOCALAPPDATA%\Programs\Common\VST3`.

## License

`mt32emu` (under `munt/`) is LGPL - see `munt/mt32emu/COPYING.LESSER.txt`.
