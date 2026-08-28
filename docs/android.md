# Android port

A Standalone Android build of the emulator - no VST3 (Android has no meaningful VST3 host
ecosystem). Same shape as the desktop Standalone app / Nonet Sequencer: one app, no plugin
wrapper. Works on real hardware; not yet packaged for a store release.

CMake target `D110Android`, Gradle module `android/app`, app id `com.d110emulator.android`,
launcher label "d110".

## What's in it

- The real firmware/native core, the real photographed panel, the on-screen keyboard (1 or 2
  octaves), and the full D-20-style sequencer (grid and retro D-pad views) - all shared with the
  desktop plugin. No extended editor drawer and no memory card slot.
- A single hamburger menu (☰) for everything the desktop build splits across Load/Options.
- Loading a MIDI file to play, and importing a SysEx/MIDI bank (`.syx` or SysEx events embedded
  in a `.mid`) to populate the internal Tone Memory (Bank I).
- USB MIDI keyboard input, and MIDI Output to drive an external hardware synth.
- Long-press (~500ms) as a right-click substitute for context menus.
- Manual screen-margin override (Options) for devices where automatic nav-bar/status-bar
  avoidance guesses wrong.

## Build

```
cd android
./gradlew assembleDebug
```

Needs the Android SDK/NDK (see `android/local.properties`). Install with
`adb install -r app/build/outputs/apk/debug/app-debug.apk`.

## ROM placement

Hamburger menu -> **Choose ROM files...** - select all your D-110 ROM files at once (multi-select:
long-press the first one to enter selection mode, then tap the rest). Works even before the
synth has ever started. The selected files are copied into the app's own storage and the
emulator powers on immediately. **Use this** rather than placing files manually - see why below.

Alternatively, `adb push` the same files (see [`docs/roms.md`](roms.md)) directly into:

```
/storage/emulated/0/Android/data/com.d110emulator.android/files/roms/
```

Both routes land in the same place and persist across restarts.

**Don't try to place files there with a file manager app.** On Android 11 and later, the OS
blocks every app except the owner from browsing or writing into another app's
`Android/data/<package>/` folder - this applies to file manager apps too, even with "all files
access" granted. The in-app picker above works around this: it only needs the user to pick
*source* files from an ordinary accessible folder (e.g. Downloads), then the app itself writes
them into its own storage, which is always allowed regardless of Android version.

## Known limitations

- Not distributed via any app store; debug build only.
- Only verified on one physical device so far.
