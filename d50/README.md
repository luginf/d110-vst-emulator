# d50/

The D-50 emulator's engine, ported from Michi71's PicoFaceD5
(`UPSTREAM_README.md` has the original's own description, license and ROM
provenance). Unlike the D-110 backends elsewhere in this repo, there is no
real firmware here: PicoFaceD5 is a from-scratch reimplementation of the
D-50's LA synthesis (structures, WG/TVF/TVA, the three LFOs, the pitch
envelope, chorus/reverb), running against real Roland ROM-derived PCM data
and real factory SysEx patches, not an emulated CPU.

See `CLAUDE.md` at the repo root for how this fits into the wider project
(branch `d50`, not yet merged).

## Layout

- `d5_engine/` - the LA engine itself, vendored from PicoFaceD5's
  `include/d5_engine/` with only additive changes (never touch pitch/keyfollow
  math without a clear reason - see `d5_patch_map.h`'s own comments citing
  ROM disassembly addresses). **Eight of the eleven headers are byte-for-byte
  identical to upstream** - `d5_effects.h`, `d5_env.h`, `d5_fastmath.h`,
  `d5_hot.h`, `d5_lfo.h`, `d5_mod.h`, `d5_patch_map.h` and `d5_pcm_voice.h`.
  Only `d5_voice.h`, `d5_synth_voice.h` and `d5_patch.h` differ, and they
  carry nothing but this project's own additions (per-partial audition mute,
  the TVF keyfollow direction switch below) on top of the unmodified
  original. Re-verify with `cmp` against a fresh upstream checkout before
  concluding that a sound problem originates here.
- `D5_Bridge.h/.cpp` - the port's own integration layer between the engine
  and the rest of the app (MIDI, SysEx read/write, patch bank
  selection/loading). Diverges from upstream mainly in loading ROM/PCM/patch
  data from disk at runtime instead of linking it in at build time - see the
  file's own header comment.
- `D5RomLoader.h/.cpp`, `D5SyxLoader.h/.cpp` - runtime decoders for the raw
  PCM ROM pair and for SysEx bulk dumps, so end users never need Python or a
  rebuild to supply their own ROMs/patches. Each is checked byte-identical
  against the corresponding Python tool below.
- `tools/` - vendored from PicoFaceD5's own `tools/d5_extract/`: the Python
  reference converters (`d5_rom.py`, `d5_syx_to_patches.py`,
  `d5_make_blob.py`) that `plugin/CMakeLists.txt` still uses at configure
  time to bake a compiled-in fallback bank/PCM table, and that the runtime
  loaders above are validated against. These, and `d5_sample_table.json`
  (the 100-entry PCM start/length/loop/root table every PCM partial's pitch
  depends on), are also byte-for-byte identical to upstream.
- `roms/` - gitignored; see `roms/README.md` for what to put there.
- `d5_presets.h`, `d5_settings.h` - upstream's own hand-built 8-preset
  fallback bank and default settings, used only when no real ROM/SysEx bank
  is available.
- `fantasia.md` - the factory patch "Fantasia" (patch 1), decoded parameter
  by parameter from its SysEx bytes, in the same layout as the D-50's own
  editor pages - kept for cross-checking our SysEx decode against a real
  D-50's panel display, one parameter at a time. See that file's own header
  for status.

## TVF ENV DEPTH keyfollow: two directions, switchable at runtime

The engine is disassembled from `roms/D50-v1.06.bin` (confirmed by that
file's own embedded boot string, `D-50    Ver 1.06`), and it reproduces that
revision's arithmetic verbatim - including one documented firmware bug. The
D-50 Service Notes' CHANGE INFORMATION table states that ROM 1.04-1.06 had
"the effect of KEYFOLLOW on TVF ENV DEPTH... opposite to what designed",
corrected in ROM 1.07. Every factory patch was voiced for the corrected
direction, so a faithful 1.06 disassembly reproduces a bug most players never
heard.

Which one a real D-50 sounds like is a question for ears, not documents, so
both are available: `d5::g_tvf_depth_kf_fixed` (declared in
`d5_synth_voice.h`, read once per note by the two copies of the formula, in
that file and in `d5_voice.h` - they must stay in sync), reachable through
`D5_Bridge::setTvfKeyfollowFixed()` and surfaced as a **`KF v1.07` /
`KF v1.06` toggle** in the editor's tone header, beside the four partial-mute
buttons. Persisted with the rest of the state. Default is v1.07, the
corrected direction. It takes effect on the next key struck, so a chord can
be held, the button clicked, and the same chord struck again to compare.

**Where the switch does anything.** In the factory bank `PN-D50-00`, only 9
of 256 partials are affected: 234 have the keyfollow byte at 0, and another
13 set it on a PCM partial, which has no TVF for it to reach. The rest, best
candidates first (the strength column is the difference in the chip's own
depth unit at C6, for ranking only):

| Patch | Partial | KF | Env Depth | Strength |
|-------|---------|----|-----------|----------|
| 45 JX Horns-Strings | Lo P1 | 2 | 100 | 18.8 |
| 53 Velo-Brass | Up P2 | 2 | 90 | 16.9 |
| 19 Slap Brass | Up P2 | 2 | 90 | 16.9 |
| 19 Slap Brass | Lo P2 | 2 | 90 | 16.9 |
| 45 JX Horns-Strings | Lo P2 | 2 | 53 | 9.9 |
| 4 Arco Strings | Up P2 | 2 | 52 | 9.8 |
| 35 Ethnic Session | Lo P2 | 2 | 41 | 7.7 |
| 56 Pianissimo | Up P2 | 1 | 36 | 3.4 |
| 56 Pianissimo | Lo P2 | 1 | 36 | 3.4 |

The term is `key >> (4 - KF)` with `key` measured in semitones from C4, so
the two directions are **identical at C4** and diverge with distance from it.
Any comparison has to be played well away from the middle of the keyboard.

Patch 45 is the clearest test, and the difference there is audible: it shows
up mainly in the treble, as a pronounced phasing/LFO character. That is
consistent with the mechanism - the two directions open the filter by
different amounts as the pitch rises, and what the wider-open one lets
through is then worked on by the patch's own chorus. Which of the two matches
a real D-50 is still unsettled; the Patch menu's "Send to real D-50" puts the
same bytes on real hardware for a direct A/B.

## Working here

- The JUCE-side plugin code (`D50AudioProcessor`, `D50Editor`, the D-50
  editor UI) lives in `plugin/Source/d50/`, not here - this directory is the
  engine only, deliberately kept free of any JUCE dependency so it stays
  portable to the Pico target upstream still builds it for.
- Anything that looks like a bug in patch decode or synthesis should be
  checked against upstream first (`cmp` against a fresh clone of
  `Michi71/PicoVintageSynthCollection` is the fastest way) before assuming
  this port introduced it - most of `d5_engine/` never needed touching at
  all, and probably still doesn't. A full comparison has been run once, over
  every engine header and every extraction tool, and found no drift: what
  still does not match a real D-50 is upstream's own approximation, not
  something this port broke.
- Beware the harmonic-vs-fundamental trap when judging PCM pitch from a
  spectrum. Picking the loudest peak as the fundamental flags dozens of false
  octave errors in the sample table, because formant-carrying waves (vowels,
  sax, violin) legitimately peak well above their first harmonic. The sound
  test that holds is whether the energy falls **only** on multiples of some
  harmonic *k* - by that test the looped regions really are the single cycles
  the table declares.
- Parameter-by-parameter cross-checks like `fantasia.md` are the most
  reliable way to catch a decode bug when a patch sounds subtly wrong on a
  real D-50 - the panel shows the *interpreted* value (note names, LFO
  select as +1/-1/+2..., bias points as `<A1`/`>C7`, etc.), which is a much
  more direct comparison than trying to diagnose from audio alone.
