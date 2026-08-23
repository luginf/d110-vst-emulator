# Android port (paused, unreleased)

An Android build of the emulator, added 2026-08-21 and developed through 2026-08-23, then
paused (Alan's call) to focus on other work. Briefly picked back up on 2026-08-24, not to
resume the port generally but to carry three fixes over from desktop work that day: the
retro sequencer's UI improvements (see [`docs/sequencer.md`](sequencer.md)'s own "Retro mode"
section - all of it is shared code, so it reached Android just by rebuilding), a real
data-loss bug (see "State persistence" below), and the app's launcher icon. Still paused as
an overall effort otherwise - it runs, is feature-complete for a first release, but has not
been packaged or shipped - treat everything below as "works on one test device", not "shipped
and supported".

**Standalone only, no VST3** - Android has no meaningful VST3 host ecosystem, so this is the
same shape as the desktop Standalone / Nonet Sequencer: one app, no plugin wrapper. CMake
target `D110Android` (`android/app/src/main/cpp/CMakeLists.txt`), Gradle module `android/app`,
binary `libjuce_jni.so`, app id `com.d110emulator.android`, launcher label "d110" (the D-110's
own name was hard to type quickly on a phone keyboard - see `android/app/src/main/cpp/Main.cpp`'s
own header comment for the fuller feature list this file is a summary of).

## What's in it

- The real firmware/native core, the real photographed panel, the real on-screen keyboard
  (1 or 2 octaves, Options menu), and the full D-20-style sequencer - both grid (default) and
  retro D-pad views - all reused unchanged from the desktop plugin. No extended editor drawer
  and no memory card - deliberately out of scope for "as simple as possible".
- A single hamburger menu (☰) replaces the desktop's separate Load/Options buttons - screen
  space is tight, especially in landscape.
- USB MIDI keyboard input (confirmed working with real hardware) and MIDI Output to drive an
  external hardware synth off whatever the D-110 itself is playing.
- Right-click substitute: a long press (~500ms, roughly stationary) reaches the same context
  menus a real right-click does on desktop - see `D110SequencerPanel::mouseDown()`'s own
  comment. Doesn't apply to the piano keys themselves (holding a key is just a sustained note).
- Manual screen-margin override (Options -> four independent Top/Bottom/Left/Right checkboxes)
  for devices where the automatic nav-bar/status-bar avoidance guesses wrong (e.g. a tablet
  that keeps its nav bar bottom-anchored even in landscape).
- The real app icon (2026-08-24) - `android/app/src/main/res/drawable/icon.png`, replacing an
  earlier plain placeholder - is the front panel's own "D-110" wordmark, vectorised by hand
  from `docs/panel_reference.png` rather than upscaled from a screenshot; see
  `docs/app_icon.svg`'s own comment. The desktop Standalone/VST3 targets get the same mark via
  `ICON_BIG`/`ICON_SMALL` in `plugin/CMakeLists.txt`, though that only actually shows up on
  Windows/macOS - JUCE has no automatic window-icon path for it on Linux (confirmed by reading
  its own CMake logic: there's no Linux branch in `_juce_generate_icon`), so the Linux
  Standalone's own taskbar icon is unchanged for now (deferred, Alan's call, 2026-08-24 - see
  Nonet Sequencer, which sets its own icon directly at runtime instead, since that app is fully
  our own `Main.cpp` and Standalone's isn't).
- When the retro sequencer is showing, the app's own Play/Stop buttons and status line hide
  (Alan's request, 2026-08-24) - they're for the app's separate "Load MIDI file..." playback
  feature, not the sequencer transport, and sitting right above retro's own STOP/PLAY/REC read
  as confusing duplicates. Only the hamburger stays (it has no equivalent menu button of its
  own to relocate Load MIDI file/Panel switch/etc onto, unlike the grid sequencer - see
  `Main.cpp`'s own comment on both).

## Build

```
cd android
./gradlew assembleDebug
```

Needs the Android SDK/NDK (see `android/local.properties`) - same JUCE checkout the desktop
build uses, fetched into `android/juce-src` by `settings.gradle` before Gradle even reaches the
native side. Install with `adb install -r app/build/outputs/apk/debug/app-debug.apk`; ROMs go
wherever the in-app Utility tab's folder picker is pointed, or loose beside the APK's own data
dir - see the desktop [`docs/roms.md`](roms.md), same ROM files, no Android-specific format.

## JUCE-on-Android gotchas actually hit here (worth knowing before touching file I/O again)

Three separate, non-obvious bugs surfaced while wiring the sequencer's own Load/Save MIDI
buttons to Android's content:// file picker (2026-08-22/23) - none of them Android's fault
exactly, but all three are easy to reintroduce if this code is touched again without knowing
they're there:

1. **A long-press timer racing a `juce::FileChooser`.** Any button that both opens a
   `juce::FileChooser` *and* sits under the long-press-as-right-click mechanism (see above) has
   to bump the panel's long-press token before launching the chooser. On desktop the ordinary
   `mouseUp()` a moment later does this for free; on Android, opening the chooser hands off to a
   real system Activity that this window never sees a matching finger-lift for, so the deferred
   timer still fires later and opens a *second*, competing `FileChooser` - JUCE's Android
   backend only supports one in flight at a time (`juce::FileChooser::Native::currentFileChooser`
   in `juce_FileChooser_android.cpp`), so the second one breaks the first one's own result
   callback. See `D110SequencerPanel::mouseDown()`'s `loadBounds`/`saveBounds` handlers.
2. **`juce::URL::isLocalFile()` / `getLocalFile()` are not trustworthy on Android for a
   content:// picker result.** They reconstruct a raw filesystem path from the URI's document ID
   (e.g. `primary:Download/foo.mid` -> `/storage/emulated/0/Download/foo.mid`) without ever
   checking it's actually readable. That reconstructed path passes a `stat()` (so
   `File::getSize()` succeeds) but scoped storage still refuses to actually open it for reading -
   confirmed empirically. The fix is to attempt a real `FileInputStream`/`FileOutputStream` open
   on the reconstructed path and only trust it if that succeeds, falling through to
   `juce::AndroidDocument` (the actually-reliable answer) otherwise - see
   `D110SequencerPanel::withLocalFileForLoad()`/`withLocalFileForSave()`.
3. **Synthetic taps (`adb shell input tap`) are unreliable on JUCE popup menus and system
   pickers, especially anything nested or newly opened.** A single instantaneous tap often does
   nothing; a zero-distance `input touchscreen swipe x y x y <duration>` registers far more
   reliably. Screenshot-then-tap cycles also need the *current* screenshot's own coordinates
   measured fresh each time - reusing a previous screenshot's coordinates after any scroll,
   orientation change, or view-state change reliably misses.

## State persistence (fixed 2026-08-24)

Alan reported that quitting the Android app lost every song - "comme si on avait réinitialisé
la mémoire". Root cause, confirmed by reading `Main.cpp`: unlike the desktop Standalone target
(a real `juce::StandaloneFilterApp`, which autosaves/reloads
`getStateInformation()`/`setStateInformation()` through its own settings file) or Nonet
Sequencer (its own settings file via `NonetSeqHost::saveSettings()`/`loadSettings()`), the
Android app is a bare `JUCEApplication` with a raw `D110AudioProcessor processor;` member and
*nothing* wired up to either call - state was simply never saved anywhere, ever.

Fixed the same way NonetSeqHost persists its own settings: one flat file
(`d110-state.bin`, in the app's private files dir) holding the exact binary blob
`getStateInformation()`/`setStateInformation()` already produce/consume everywhere else -
every song slot/track, tempo, retro key bindings, LCD mode, keyboard config, theme. Loaded
once in `MainComponent`'s constructor, right after `reloadRomsAndPowerOn()` has already
booted the firmware fresh (restoring firmware NVRAM bytes into files on disk after boot only
takes visible effect on the *next* power-on, but the D-110 core already flushes its own NVRAM
to disk continuously during normal operation independent of this - see
`project_standalone_nvram_persistence_fix` in project memory - so the only thing this needed
to restore here is the higher-level state, which it does unconditionally).

Saving happens in two places, not just one, because Android can (and does) kill the whole
process without warning once it's backgrounded, well before an orderly C++ destructor chain
would ever run:

- `MainComponent::~MainComponent()` - covers a clean in-app quit (the rare case).
- `D110AndroidApp::suspended()` - called when the OS backgrounds the app; this is the save
  point that actually matters, since the process can die at any point afterwards with no
  further callback. `shutdown()` also saves, belt-and-suspenders, though it may never be
  reached on Android at all.

Verified on a real device: set a distinctive tempo and retro mode, backgrounded the app,
force-killed the process (`adb shell am kill`, confirmed gone via `pidof`), relaunched -
tempo and retro mode both came back correctly.

## Not done / not verified

- No release build, no signing config beyond the debug default, no store listing prep.
- Not tested on a range of devices/Android versions - only one physical phone (a PocoF3) this
  whole port was developed and tested against.

## Possible future direction (not scheduled)

Alan raised the idea (2026-08-23) of eventually porting this to a Raspberry Pi as a dedicated
hardware sequencer, separate from the phone/tablet Android build above - not started, no
scoping done yet.
