# D-110 VST Emulator - notes for Claude

A VST3/standalone emulator of the Roland D-110 multi-timbral sound module. It runs the
**real Roland firmware** (not a reimplementation of the menus/editor logic) against an
emulated LA32 sound engine (`munt`/mt32emu, D-110 fork). See `README.md` for the full
user-facing picture - architecture, ROM requirements, known limits, panel/editor
behaviour. Don't duplicate that here; this file is about how to work in the repo.

## Two CPU backends - know which one you're touching

- `plugin/Source/native/` - **`D110EmulatorNative`, the default and only actively
  developed backend.** The D-110's i8x9x/MCS-96 CPU reimplemented from scratch, zero
  MAME dependency, stepped inline on the audio thread (fixes 0-18ms MIDI jitter the
  MAME backend has). This is what CI builds (Windows/macOS) and what ships in releases.
- `plugin/Source/D110Core.*` - the original MAME-backed backend (`D110Emulator`), runs
  the firmware inside an embedded MAME `roland_d10` driver instance. Opt-in only
  (`-DD110_BUILD_MAME_BACKEND=ON`), needs a separately built MAME 0.288 tree with a
  local patch (`patches/mame_mcs96_stale_irq_level.patch`). Kept in the tree as a
  dormant fallback, not deleted, but **not to be shipped in packaging/releases
  anymore** (Alan's call, 2026-08-05) - see the memory note `feedback_native_only_releases`.
  Don't propose reviving it in `.deb`/CI unless explicitly asked.
- Both share the same JUCE plugin shell (`PluginProcessor.*`, `PluginEditor.*`) and the
  same firmware ROMs; they build as two separate plugin targets that can sit side by
  side in a DAW.

## Layout quick-reference

- `plugin/Source/PluginProcessor.*` - JUCE `AudioProcessor`: MIDI in/out routing to the
  firmware, `osMidiCollector` (`juce::MidiMessageCollector`) is the queue real MIDI
  input and UI-injected notes (`injectTestNote`) both feed, drained in `processBlock`.
  Also owns `sequencerEngine` (see below) and drives it from `processBlock`.
- `plugin/Source/PluginEditor.*` - the whole UI. `D110Panel` = the photographed panel
  with invisible hit-regions + the offscreen-rendered dot-matrix LCD. `D110EditorPane` =
  the nine-tab extended editor drawer (`Tab` enum: Parts, Tone, Rhythm, Patches,
  Timbres, Tones, System, Monitor, Utility), opens downward, independently foldable.
  `D110Keyboard` = the on-screen test keyboard drawer (mouse piano + tracker-style PC
  keyboard input, MIDI channel/omni via right-click), also independently foldable, open
  by default. `D110SequencerPanel` (see below) is a third such drawer, closed by
  default. `D110MemoryCard` = the memory card slot widget.
- `plugin/Source/sequencer/` - a D-20-style multitrack MIDI sequencer, added 2026-08-05.
  `D110SequencerEngine` is the transport/data model - deliberately D-110-agnostic (own
  internal clock, note-only `juce::MidiMessageSequence` per track, MIDI-file and
  quantize logic), talking to the rest of the plugin only through a `channelForTrack`
  callback the processor supplies. `D110SequencerPanel` is the JUCE UI drawer. 9 tracks
  (D-110 Parts 1-8 by their live SYSTEM-area channel, plus a rhythm track fixed on
  channel 10); state persists in `getStateInformation`/`setStateInformation` the same
  way the firmware NVRAM does. `plugin/sequencer_probe.cpp` and
  `plugin/sequencer_state_probe.cpp` are its headless tests (engine timing/quantize/
  file-I/O, and the state-save round trip, respectively) - both native-core-only, no
  MAME dependency.
- `plugin/CMakeLists.txt` - two plugin targets (native always, MAME opt-in) plus a long
  list of headless test/probe executables (`plugin/*.cpp` at the top level, ~60 of
  them) used to measure firmware RAM layout and verify behaviour empirically rather
  than by reading Roland docs (there aren't any that go this deep). `d110_editor_shot` /
  `d110_native_editor_shot` render the whole editor UI to a PNG headlessly - the fast
  way to visually check UI changes without opening a DAW.
- `munt/` - vendored sound engine with a couple of local fixes for D-110 (LCD
  buffer/part-count, see `munt/mt32emu/src/Display.cpp`).
- `docs/` - measured ground truth (panel pixel geometry, SysEx address map, factory
  defaults, memory card protocol). Code comments reference these; check here before
  guessing at firmware behaviour.
- `.github/workflows/build-{windows,macos}.yml` - CI, native-core only. Deliberately
  **not** wired to `main`/`linux-port` push - triggered by `workflow_dispatch` or by
  pushing to the `ci/windows-macos-builds` branch specifically (fast-forward `main`
  onto it to run CI: `git push origin main:ci/windows-macos-builds`).

## Conventions

- **Code comments are written in Russian** (see the header of `plugin/CMakeLists.txt`
  for why - MSVC codepage issue was the proximate trigger, but it's the established
  convention project-wide, not just that file). Match it when editing existing
  Russian-commented code; new files/comments in areas you author don't have to follow
  this unless editing alongside existing Russian comments.
- Everything in the extended editor writes to the **firmware** via real Roland SysEx
  messages (DT1), never directly to the sound engine - the mirror to the sound engine
  is a separate, one-directional path. If you add a new editor field, follow this
  pattern: find/verify the firmware RAM address empirically (a `*_probe.cpp` tool), add
  a `Cell`, send a real SysEx write.
- Verification bias in this codebase: claims about firmware behaviour are backed by a
  purpose-built probe (`plugin/*_probe.cpp`, `*_test.cpp`) that measures RAM/output
  directly, not by inference. Follow that pattern rather than guessing when touching
  firmware-adjacent code. `plugin/audio_test.cpp`'s pass/fail is explicitly documented
  as unreliable (timing races) - don't trust it as a sole signal.
- Linux packaging: `.deb` built locally (not by CI), staged at repo root (gitignored),
  named `d110-emulator_<version>-fixesNN_amd64.deb`, `Depends` computed via
  `dpkg-shlibdeps`, built with `fakeroot dpkg-deb` for correct ownership. Maintainer
  field is `luginf`, not an email address.

## Build

Default (native only, no MAME needed):
```
cd plugin && cmake -B build -S . && cmake --build build --config Release
```
Full requirements, MAME-backend opt-in build, and ROM setup: see `README.md`.
