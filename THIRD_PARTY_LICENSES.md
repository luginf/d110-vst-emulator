# Third-Party Licenses

This project incorporates code from several third-party open-source
projects. This file lists each of them, their license, and where their
code is used in this repository. Original copyright notices and license
headers in the source files themselves have been preserved and are not
duplicated in full here except where noted.

This project as a whole (the combined work of the code below plus this
project's own original code) is distributed under the **GNU Affero General
Public License v3.0** - see [LICENSE](LICENSE) - because it links the JUCE
framework under JUCE's AGPLv3 option (see below). munt/mt32emu's LGPLv2.1+
license (see below) permits this: LGPL-licensed libraries may be combined
into a GPL/AGPL-licensed larger work.

---

## munt / mt32emu

- **License**: GNU Lesser General Public License, version 2.1 or (at your
  option) any later version - see the vendored copy at
  [`munt/mt32emu/COPYING.LESSER.txt`](munt/mt32emu/COPYING.LESSER.txt) for
  the full text.
- **Copyright**: Copyright (C) 2003-2009 Dean Beeler, Jerome Fisher;
  Copyright (C) 2011-2022 Dean Beeler, Jerome Fisher, Sergey V. Mikayev.
- **Repository**: https://github.com/munt/munt (this project vendors
  davidhsilaban's D-110 fork,
  https://github.com/davidhsilaban/munt/tree/davidhsilaban-d110-changes-with-kode54-super-mode)
- **Used for**: the actual LA-synthesis sound engine (Roland MT-32/D-110
  emulation core). Vendored in full, including its own license files, at
  `munt/`.
- **Modifications made in this project**: `munt/mt32emu/src/Display.cpp`
  was patched to change `DISPLAYED_VOICE_PARTS_COUNT` from 5 to 8 (the
  D-110 has 8 LCD part-status slots vs. the original MT-32's 5), to remove
  inter-digit spaces to fit the 20-byte display buffer, and to make
  `copyNullTerminatedString`/`Mode_MAIN` pad their tail with spaces instead
  of leaving stale bytes from a previous, longer message. The complete
  modified source is included in this repository, satisfying the LGPL's
  source-availability requirement for modified versions of the library.

## JUCE

- **License**: this project uses the JUCE Framework under its **AGPLv3**
  open-source licensing option (as opposed to a paid commercial JUCE
  license).
- **Author / owner**: Raw Material Software Limited
- **Website**: https://juce.com
- **Used for**: the VST3 plugin framework (`plugin/`), fetched automatically
  at build time via CMake `FetchContent` - not vendored in this repository.

---

All trademarks, service marks, and trade names referenced above are the
property of their respective owners.
