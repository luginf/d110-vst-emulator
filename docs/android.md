# Android port (paused, unreleased)

An Android build of the emulator, added 2026-08-21 and developed through 2026-08-23, then
paused (Alan's call) to focus on other work. It runs, is feature-complete for a first release,
but has not been packaged or shipped - treat everything below as "works on one test device",
not "shipped and supported".

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

## Not done / not verified

- No release build, no signing config beyond the debug default, no store listing prep.
- The Retro sequencer view's D-pad has known layout issues (non-square hit targets, ENTER
  crammed in the middle) - see [`docs/sequencer.md`](sequencer.md) for the intended cross-only
  layout, being reworked independent of Android specifically.
- Not tested on a range of devices/Android versions - only one physical phone (a PocoF3) this
  whole port was developed and tested against.

## Possible future direction (not scheduled)

Alan raised the idea (2026-08-23) of eventually porting this to a Raspberry Pi as a dedicated
hardware sequencer, separate from the phone/tablet Android build above - not started, no
scoping done yet.
