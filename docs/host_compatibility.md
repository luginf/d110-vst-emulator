# DAW/host compatibility

## Direct MIDI ports

Both the Standalone app and the VST3 plugin can open a system MIDI input/output pair of their
own, independent of whatever the host routes (Options menu -> "MIDI In"/"MIDI Out"). Useful for
pointing the app directly at a hardware controller or an external editor.

### JACK MIDI input (Linux Standalone only)

If JACK is available, the Linux Standalone build also exposes a real JACK MIDI input port,
`D-110 Emulator:midi_in`, visible in any patchbay (qjackctl, Catia, qpwgraph...) - so a DAW's
MIDI Out can be wired in directly. Detected automatically at build time; silently absent if
JACK's dev headers weren't found, or on Windows/macOS. Standalone only, not the VST3 plugin.

## Known VST3 host issues

The same VST3 build behaves differently across hosts:

| Host | Status |
| --- | --- |
| Ardour | Works correctly. |
| Carla | Sound is pitched up; hardware pitch bend has no effect. Also: Carla doesn't forward live SysEx to hosted VST3 plugins, so a live external editor (e.g. Edisyn) won't reach the plugin there - use file-based import, or a host that does forward it. |
| Qtractor | Plugin loads and shows a patch on the LCD, but produces no audio. |

The Standalone build is unaffected by any of this. These look like host-side bugs rather than
something in this project's VST3 handling (the same code works correctly in Ardour), but the
root cause hasn't been pinned down. If you hit either issue, Ardour or the Standalone app are
the reliable options today.
