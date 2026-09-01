# d50/roms/

Gitignored, like the D-110's own ROM folder - nothing here is ever committed.

Put here:

- The two D-50 PCM ROMs (IC30/IC29), or a combined 512 KB dump - any dump
  works, including a 512 KB read-out of a 256 KB chip (folded automatically).
- The D-50 program EPROM (IC22, 64 KB) - required, not optional; the sample
  name table and parameter ranges come from it.
- Any D-50 SysEx bulk dump(s) (`*.syx`) for real factory/user patches -
  optional; several files concatenate into several banks, in filename order.

Files are identified by content (CRC32), not by name - see `d50/tools/d5_rom.py`.

`plugin/CMakeLists.txt` converts whatever is here into `d5_pcm_table.h`/
`d5_patch_data.h` at configure time (into the build tree, never into this
folder) - those still need the CMake+Python step and are what the plugin
target itself is built against.

The PCM ROM pair alone (A+B, no EPROM needed) has a second, independent
consumer: `D50Emulator` decodes them itself at startup via
`d50/D5RomLoader.cpp` (byte-identical to the Python conversion above, checked
against it directly) if it finds them loose beside its own binary or in a
`d50-roms/` folder next to it - no build tools needed for that part. Same
story for a `.syx` bank: `d50/D5SyxLoader.cpp` parses and validates it at
startup (byte-identical to `d5_syx_to_patches.py`'s own output, checked
against it directly) and it takes priority over whatever bank got compiled
in - which means a plugin build no longer needs to have anyone's patch data
baked in at all to still play real patches; see `D5_Bridge::loadBank()`'s own
comment. This is the path an actual end user (not just a from-source build)
would use; the plugin's own POST_BUILD step copies Alan's raw ROM+`.syx`
files there instead of pre-converted/compiled forms specifically to exercise
that same path locally. No ROM images or patch dumps are part of this
repository - see `d50/UPSTREAM_README.md`.
