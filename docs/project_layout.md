# Project layout

- `munt/` - the sound engine (`mt32emu`), vendored from the fork above with a couple of local
  fixes (see `munt/mt32emu/src/Display.cpp` for the LCD buffer/part-count corrections made for D-110).
- `plugin/Source/native/` - the default backend: the D-110's own CPU reimplemented from scratch,
  zero MAME dependency (`D110EmulatorNative`).
- `plugin/Source/D110Core.*` - the original, MAME-backed control board: runs MAME's `d110`
  machine on its own thread with a headless OSD, and exposes the display, the sixteen buttons
  and the firmware's parameter memory. Opt-in (`D110_BUILD_MAME_BACKEND`), not built by default.
- `plugin/Source/PluginProcessor.*`, `PluginEditor.*` - the JUCE plugin and the photo-composite panel.
- `plugin/mame.cmake` - the MAME library/include/define lists and how the subset was built.
- `docs/` - the measured panel geometry and the SysEx address map, both derived by profiling
  rather than by eye. Every number in the code is justified there.
- `rom_test/` - a console tool that renders a test chord from a Control + PCM ROM pair.
- `plugin/audio_test.cpp` - offline check with no DAW: powers the plugin on, plays a note, proves
  a panel edit changes the sound, checks that re-sending an *unedited* state changes nothing, and
  sweeps all sixteen MIDI channels reading the part indicators off the firmware's own display.
- `plugin/tone_probe.cpp` - locates the Tone Temporary Area by reading the engine's copy of each
  tone back out and matching it against the firmware's RAM. An exact match is simultaneously the
  measurement and the null test.
- `plugin/tone_edit_survives_probe.cpp` - edits a tone from the panel on an ordinary patch, then
  changes a part parameter, and checks the sound engine still holds the edited tone. Finds both
  menu pages by pressing buttons and watching which RAM byte moves, and runs the experiment twice
  - with the tone re-assert on and off - so the result has a control that can show the failure.
- `plugin/state_test.cpp` - edits a parameter, saves the plugin state, restores it into a fresh
  instance, and checks the edit came back; also checks a plugin loaded with no saved state finds
  the memory the last one left, and that a second instance refuses to power on.
- `plugin/two_instance_test.cpp` - records what really happens when two machines run in one
  process. Diagnostic, not a fix.
- `plugin/panel_render.cpp` - snapshots the real panel to PNG, as a storyboard every 100 ms,
  so the card's travel is judged by looking at it rather than from constants. It drives the
  card by clicking the slot, so the whole path from the mouse to the frame is what gets checked.
- `plugin/card_probe.cpp` - the memory card. Puts four different cards in the slot, each
  differing from the next by one property, and prints what the firmware said about each; then
  formats a blank card, saves to it, wipes the instrument with a factory reset and loads it back,
  comparing three snapshots of the battery RAM. See [`memory_card.md`](memory_card.md).
- `plugin/bridge_probe.cpp`, `core_test.cpp` - the harnesses used to map the firmware's RAM and
  to exercise the control board on its own.
