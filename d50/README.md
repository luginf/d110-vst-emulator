# d50/

The D-50 emulator's engine, ported from Michi71's PicoFaceD5
(`UPSTREAM_README.md` has the original's own description, license and ROM
provenance). Unlike the D-110 backends elsewhere in this repo, there is no
real firmware here: this is a from-scratch reimplementation of the D-50's LA
synthesis (structures, WG/TVF/TVA, the three LFOs, the pitch envelope,
chorus/reverb), running against real Roland ROM-derived PCM data and real
factory SysEx patches, not an emulated CPU.

The JUCE-side plugin/editor (`D50AudioProcessor`, `D50Editor`) lives in
`plugin/Source/d50/`, not here - this directory is the engine itself,
deliberately kept free of any JUCE dependency so it stays portable to the
Pico target upstream still builds it for.

## Get started

You need your own **D-50 ROM dumps** - copyrighted Roland firmware, **not
included** here. Put them in `roms/` (gitignored - see `roms/README.md`).

You can get them here: 
- [https://mdk.cab/game/d50](https://mdk.cab/game/d50)
- [https://mdk.cab/download/standalone/d50.7z](https://mdk.cab/download/standalone/d50.7z)

## Layout

- `d5_engine/` - the LA engine itself, vendored from upstream with only
  additive changes.
- `D5_Bridge.h/.cpp` - the port's own integration layer (MIDI, SysEx,
  patch bank selection/loading).
- `D5RomLoader.h/.cpp`, `D5SyxLoader.h/.cpp` - runtime decoders for raw PCM
  ROM dumps and SysEx bulk dumps, so end users never need a rebuild to
  supply their own.
- `tools/` - the Python reference converters this port's C++ loaders are
  validated against.
- `roms/` - gitignored; see `roms/README.md`.
- `d5_presets.h`, `d5_settings.h` - upstream's own hand-built fallback
  bank/settings, used only when no real ROM/SysEx bank is available.
- `fantasia.md` - the factory "Fantasia" patch decoded parameter by
  parameter, for cross-checking SysEx decode against a real D-50's panel.

See `CLAUDE.md` at the repo root for how this fits into the wider project,
and `.claude/dev-notes/d50.md` for engine-internals detail (the TVF
keyfollow firmware-bug investigation, PCM pitch pitfalls, verification
approach) trimmed out of this file to keep it short.
