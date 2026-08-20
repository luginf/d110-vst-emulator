# Building: full details, and the optional MAME-backed backend

See the [README](../README.md#build) for the short version (the default native-core build).
This is the detail, plus how to build the opt-in MAME-backed `D110Emulator`.

Requires CMake and a C++ compiler (Visual Studio Build Tools on Windows; GCC and the SDL OSD's
dependencies on Linux - `libsdl2-dev libsdl2-ttf-dev libfontconfig1-dev libpulse-dev` cover it on
Debian/Ubuntu). JUCE is fetched automatically by CMake on first configure.

There are two interchangeable CPU backends - see [`plugin/Source/native/`](../plugin/Source/native)
vs. `plugin/Source/D110Core.*` - built as two separate plugins that can sit side by side in a
DAW's plugin list. **`D110EmulatorNative` is the default build and needs nothing beyond CMake
and a compiler**: the D-110's own i8x9x/MCS-96 CPU, hand-ported out of MAME's device/scheduler
framework into a standalone interpreter with zero MAME dependency, stepped inline on the audio
thread. The original `D110Emulator` runs the same firmware inside an embedded
[MAME](https://github.com/mamedev/mame) instance instead; it is kept in the tree as a dormant,
opt-in fallback (`-DD110_BUILD_MAME_BACKEND=ON`) rather than deleted, since it is the
longer-proven of the two.

Default build (native core only, no MAME):

```
cd plugin
cmake -B build -S .
cmake --build build --config Release
```

The built `.vst3` is copied automatically to the platform's shared VST3 folder
(`C:\Program Files\Common Files\VST3` on Windows, `~/Library/Audio/Plug-Ins/VST3` on macOS,
`~/.vst3` on Linux).

### Optional: JACK MIDI input port (Linux Standalone only)

If libjack's dev headers are found at configure time (`libjack-dev`/`libjack-jackd2-dev`, or
PipeWire's own JACK-compatible package), the Linux Standalone build also gets a real JACK MIDI
input port (`D-110 Emulator:midi_in`) so a DAW's MIDI Out can be wired into it directly through
any JACK patchbay. Nothing to turn on - detected automatically, silently absent everywhere else
(Windows/macOS, or a Linux box without those headers). See
[`docs/host_compatibility.md`](host_compatibility.md) for the details.

## Optional: also building the MAME-backed `D110Emulator`

**MAME is not vendored here and must be built first**, because its libraries are what run the
firmware. From a [MAME 0.288](https://github.com/mamedev/mame/releases/tag/mame0288) tree,
apply the one required patch below - it fixes a real crash, an out-of-bounds array access
reachable from this project's own EXTINT workaround, not just a cosmetic difference - then
build:

Windows:
```
cd <mame-tree>
git apply <this-repo>/patches/mame_mcs96_stale_irq_level.patch
make vs2022 MSBUILD=1 PTR64=1 MINGW64=C:/msys64/mingw64 MINGW32=C:/msys64/mingw32 \
     NOWERROR=1 PYTHON_EXECUTABLE=python SUBTARGET=d110 \
     SOURCES=src/mame/roland/roland_d10.cpp USE_BGFX=0
```

Linux (uses MAME's own default SDL OSD, no extra flags needed):
```
cd <mame-tree>
git apply <this-repo>/patches/mame_mcs96_stale_irq_level.patch
make SUBTARGET=d110 SOURCES=src/mame/roland/roland_d10.cpp -j$(nproc)
```

See [`../patches/README.md`](../patches/README.md) for what the patch fixes and why it's required.

Then point `MAME_DIR` in [`../plugin/mame.cmake`](../plugin/mame.cmake) at that tree (or pass
`-DMAME_DIR=...` on the CMake command line), and turn the backend on:

```
cd plugin
cmake -B build -S . -DD110_BUILD_MAME_BACKEND=ON -DMAME_DIR=<mame-tree>
cmake --build build --config Release
```

On Windows, everything is built with the static runtime (`/MT`) to match MAME's release
libraries.
