# Third-Party Licenses

This project incorporates code from several third-party open-source
projects. This file lists each of them, their license, and where their
code is used in this repository. Original copyright notices and license
headers in the source files themselves have been preserved and are not
duplicated in full here except where noted.

This project as a whole (the combined work of the code below plus this
project's own original code) is distributed under the **GNU Affero General
Public License v3.0** - see [LICENSE](LICENSE) - because it links the JUCE
framework under JUCE's AGPLv3 option (see below). The other components are
compatible with that:

- **munt/mt32emu** is LGPLv2.1-or-later, and LGPL libraries may be combined
  into a GPL/AGPL-licensed larger work.
- **MAME** is GPLv2-or-later. The "or later" option allows it to be used
  under GPLv3, and GPLv3 and AGPLv3 are explicitly compatible (AGPLv3 §13),
  so the combined work may be distributed under AGPLv3.

Because the whole is AGPLv3, **the complete corresponding source of any
binary release is this repository**, plus the pinned upstream revisions
named below.

---

## munt / mt32emu

- **License**: GNU Lesser General Public License, version 2.1 or (at your
  option) any later version - see the vendored copy at
  [`munt/mt32emu/COPYING.LESSER.txt`](munt/mt32emu/COPYING.LESSER.txt) for
  the full text.
- **Copyright**: Copyright (C) 2003-2009 Dean Beeler, Jerome Fisher;
  Copyright (C) 2011-2022 Dean Beeler, Jerome Fisher, Sergey V. Mikayev.
- **Repository**: https://github.com/munt/munt
- **Used for**: the actual LA-synthesis sound engine (Roland MT-32/D-110
  emulation core). Vendored in full, including its own license files, at
  `munt/`.
- **Modifications made in this project**: `munt/mt32emu/src/Display.cpp`
  was patched to change `DISPLAYED_VOICE_PARTS_COUNT` from 5 to 8 (the
  D-110 has 8 LCD part-status slots vs. the original MT-32's 5), to remove
  inter-digit spaces to fit the 20-byte display buffer, and to make
  `copyNullTerminatedString`/`Mode_MAIN` pad their tail with spaces instead
  of leaving stale bytes from a previous, longer message.
- **2026-08-04, BOSS reverb chip emulation ported in**: `munt/mt32emu/src/BossEmu.cpp`
  and `.h` are Sergey V. Mikayev's cycle-accurate interpreter of the D-110's own
  reverb chip (Hitachi HG61H20R36F / BOS-007, board silkscreen IC6) — copied
  unmodified, copyright notice intact, from the public fork
  https://github.com/davidhsilaban/munt/tree/davidhsilaban-d110-changes-with-kode54-super-mode
  (LGPLv2.1-or-later, same as the rest of munt). `BReverbModel.cpp`/`.h` and
  `Synth.cpp`/`.h` carry a small, hand-picked subset of that same fork's changes -
  the reverb-selection plumbing (`setBossReverbROM()`, an 8-model array instead of
  4, `BReverbModel::createBossReverbModel()`) only. The fork's other changes
  (`resetHardware()`, `writeMemory()`, a Timbre-memory addressing fix) were
  deliberately left out of this pass and are not present here.
- **2026-08-04, six individual outputs ported in**: the same fork's
  `D110MultiOutputStreams` render path (`Synth.h`/`.cpp`: `renderD110MultiOutput()`,
  per-part `Analog` instances, Output Assign routing reading
  `PatchTemp.patch.reverbSwitch`) was ported the same way - hand-picked, not the
  whole fork. Routes each of the D-110's 9 parts (8 voice + rhythm) to the shared
  MIX bus or one of 6 mono buses per its own Output Assign setting; verified by
  measurement (`plugin/multi_output_probe.cpp`) that a part's signal lands in
  exactly the bus it is assigned to and nowhere else. The complete modified
  source is included in this repository, satisfying the LGPL's
  source-availability requirement for modified versions of the library.

## MAME

- **License**: GNU General Public License, version 2 or (at your option) any
  later version. Most individual files, including the ones this project
  depends on most directly, additionally carry the permissive
  **BSD-3-Clause** header - among them
  `src/mame/roland/roland_d10.cpp` (Olivier Galibert, Jonathan Gevaryahu)
  and `src/devices/video/msm6222b.cpp` (Olivier Galibert).
- **Copyright**: MAME is the work of Nicola Salmoria and the MAME team.
- **Repository**: https://github.com/mamedev/mame - built from the **0.288**
  release tree.
- **Used for**: the D-110's *control* half. MAME supplies the emulated i8x9x
  CPU that runs the real Roland firmware, the MSM6222B LCD controller, the
  battery-backed RAM and the front-panel scan matrix. It supplies **no
  sound** - MAME has no LA32 emulation for any Roland LA machine, which is
  why this project pairs it with mt32emu.
- **How it is used**: linked as static libraries built from an unmodified
  0.288 tree with
  `SUBTARGET=d110 SOURCES=src/mame/roland/roland_d10.cpp`. **No MAME source
  files were modified** - everything this plugin needs (the LCD's `render()`,
  the `rams` memory share and the `SC0`/`SC1` ioports) is reachable through
  public interfaces. The MAME source is therefore not vendored here; build it
  from the upstream 0.288 tag and see `plugin/mame.cmake` for the exact
  library list and build flags.

## JUCE

- **License**: this project uses the JUCE Framework under its **AGPLv3**
  open-source licensing option (as opposed to a paid commercial JUCE
  license).
- **Author / owner**: Raw Material Software Limited
- **Website**: https://juce.com
- **Used for**: the VST3 plugin framework (`plugin/`), fetched automatically
  at build time via CMake `FetchContent` - not vendored in this repository.

## Front panel artwork

- **Files**: [`docs/panel_reference.png`](docs/panel_reference.png) (embedded
  into the plugin as its entire front panel) and the untouched
  [`docs/panel_reference_original.png`](docs/panel_reference_original.png).
- **Provenance**: produced by this project's author, by reworking and
  upscaling a photograph of the hardware found on the internet with a
  generative image tool. It is therefore a **derivative of a source
  photograph whose author and licence are unknown**, and is not claimed as a
  wholly original work. The original is kept alongside the retouched asset so
  the two modifications made here - blacking out the rack-ear mounting slots
  and reshaping the LCD opening to a real module's proportions - remain
  auditable.
- **File**: [`docs/lcd_reference.png`](docs/lcd_reference.png), a photograph
  of a real, powered-on D-110 LCD, supplied by the author and of the same
  unknown-provenance character. It is **not** shipped in the plugin binary; it
  is kept here only as the measurement reference for the display renderer.
- **Note**: "Roland" and "D-110" are trademarks of Roland Corporation. This
  project is not affiliated with, endorsed by, or sponsored by Roland. The
  panel image is used to depict the emulated instrument, not to imply any
  such relationship - see [DISCLAIMER.md](DISCLAIMER.md).

---

All trademarks, service marks, and trade names referenced above are the
property of their respective owners.
