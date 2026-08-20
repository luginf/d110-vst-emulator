# DAW/host compatibility - direct MIDI ports and VST3 hosting quirks

Two separate topics: how the Standalone app gets MIDI into it without going through a host at
all, and what happens when the VST3 build is hosted by different real DAWs. Both come from the
same 2026-08-20 investigation.

## Direct MIDI ports, beside whatever the host routes in

Both the Standalone app and the VST3 plugin can open a system MIDI input/output pair of their
own, independent of the host's own MIDI routing (`PluginEditor.cpp`'s Options menu, "MIDI In"/
"MIDI Out" - see `PluginProcessor.cpp`'s own "MIDI ports opened directly" comment). This exists so
an external editor, or a hardware controller, can reach the module the way it would reach the
real hardware. It uses JUCE's own `MidiInput`/`MidiOutput`, which on Linux is ALSA-sequencer-based
(`snd_seq_*` under the hood, confirmed by reading JUCE 8.0.15's own source) - it does **not**
create a port of its own; it subscribes to an *existing* device's port.

That's fine for "point this app at my keyboard" but doesn't give a manageable, patchbay-visible
port for something like "route my DAW's own MIDI Out into this app" - there's nothing to connect
*to*.

### JACK MIDI input port (Standalone only)

Added 2026-08-20 (`plugin/Source/JackMidiInput.h/.cpp`) for exactly that: a real JACK MIDI input
port, `D-110 Emulator:midi_in`, that shows up in any JACK patchbay (qjackctl, Catia, qpwgraph...)
so a DAW's MIDI Out can be wired into it directly, leaving a hardware keyboard connected only to
the DAW - Keyboard -> DAW -> D-110, instead of a keyboard feeding both the DAW and the D-110 at
once via the ALSA picker above.

- Linux only, and optional even there: only built when libjack's dev headers are found at
  configure time (`plugin/CMakeLists.txt`'s `D110_HAVE_JACK_MIDI` detection block, `pkg_check_modules`
  then a `find_library`/`find_path` fallback). Genuinely absent - not just disabled - everywhere
  else, so this is a no-op on Windows/macOS CI and on any Linux box without libjack-dev/
  libjack-jackd2-dev/PipeWire's own JACK-compatible package.
- Opens with `JackNoStartServer`: never launches a JACK server on the user's behalf. If none is
  reachable, the port silently never appears - no popup, no log spam, for the many users who don't
  run JACK/PipeWire-JACK at all.
- **Standalone only.** Gated at runtime on `wrapperType == wrapperType_Standalone` inside
  `prepareToPlay()`. A VST3 instance loaded in a host never opens this port - a host-hosted plugin
  should get its MIDI from the host's own routing, and a persistently-named JACK client per plugin
  instance would be awkward (name collisions across instances, unclear lifecycle when a host
  removes/re-adds the plugin).
- Feeds the exact same `handleIncomingMidiMessage()` -> `osMidiCollector` path the ALSA picker
  already uses, so omni/rechannel behaves identically regardless of which of the two a message
  came through.
- Verified end to end by `plugin/jack_midi_input_probe.cpp` (target `d110_jack_midi_input_probe`):
  constructs the real `D110AudioProcessor` with `wrapperType` forced to Standalone the same way
  JUCE's own wrapper does it, calls `prepareToPlay()`, confirms the port appears via
  `jack_get_ports`, connects a companion JACK MIDI client, sends a real note-on/off, and checks it
  reaches `D110KeyboardHost::isNoteActive()`.

## VST3 hosting: known-good and known-bad DAWs

The exact same VST3 build behaves differently across hosts. Measured 2026-08-20, same patch/NVRAM
throughout:

| Host | Sound | Pitch | Notes |
| --- | --- | --- | --- |
| Ardour | correct | correct | works perfectly |
| Carla | plays | **pitched up ~6-7 semitones** (roughly a fifth to a tritone) | a physical controller's pitch bend wheel has no effect at all; nothing shows in the Monitor tab's MIDI log either |
| Qtractor | **silent** | - | firmware powers on and shows a patch name on the LCD; MIDI channel, volume, and the 2 connected output ports all look correct, same shape as a plugin (Surge) that does play |

The Standalone build (not VST3 at all) is also correct. Ardour behaving correctly with the exact
same code is the reason this is tracked as (probably, not proven) two separate host-side bugs
rather than something in this project's own VST3 handling - see the theories ruled out below.

Also known and **confirmed to be a Carla bug, not this project's** (a different investigation, not
about pitch): Carla does not forward live-streamed SysEx MIDI to a hosted VST3 plugin at all (Note
On/CC work fine over the same route) - regular MIDI notes/CCs are unaffected, only a live external
editor talking SysEx (e.g. Edisyn) is. Confirmed by testing the same setup in Ardour, where it
works. Root-caused all the way to the exact line in Carla's own source and patched in a private
fork - see the project's own memory system (`feedback_carla_vst3_sysex`) for the full account if
this needs picking back up; not repeated here since it's a separate bug from the pitch/silence
issue below.

### Pitch shift + silence: what was ruled out (2026-08-20)

Investigated in this order, each with a concrete negative result:

1. **Sample rate mismatch.** munt's native rate is a fixed 32000 Hz (`MT32EMU_SAMPLE_RATE`); 48000
   / 32000 = 1.5, suspiciously close to a fifth, which was the leading theory at first. The Monitor
   tab (extended editor) now shows "host sample rate: NNNN Hz" - `processor.getSampleRate()`,
   whatever the host last told `prepareToPlay()` - specifically so this can be checked live inside
   any host without guesswork. **Confirmed 44100 in Carla, matching the JACK server's own rate.**
   Ruled out - though note this only proves what the host told the `AudioProcessor`, not that the
   sound engine's own `sampleRateConverter` was rebuilt with that value at the right moment; not
   independently re-checked.
2. **JUCE's own VST3 MIDI-CC/pitch-bend-as-parameters emulation**
   (`JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS`, on by default for any plugin that wants MIDI
   input). Auto-generates one automatable VST3 parameter per real MIDI CC per channel, plus two
   pseudo-indexed extras for After Touch (Steinberg's `Vst::kAfterTouch = 128`) and Pitch Bend
   (`Vst::kPitchBend = 129`) - **129 is not a real MIDI CC number**, it's just where Steinberg's own
   `ControllerNumbers` enum puts pitch bend so every controller fits in one parameter-ID array.
   Alan had found, on the pre-fix build, that nudging this shadow "Pitch Bend" parameter (which
   defaults to normalized 0 = raw pitch-bend value 0, fully bent down) to roughly 0.30 fixed the
   pitch by ear in Carla. Disabled the mechanism entirely
   (`JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0` in `plugin/CMakeLists.txt`) - the shadow
   parameters are confirmed gone from Carla's parameter list, but **the pitch shift is still
   there**. So the manual fix was compensating for something else by ear, not correcting a bad
   default - a red herring, though the flag is still worth keeping disabled (removes ~2050 unused
   automatable parameters from every host's parameter/MIDI-Learn list, real MIDI input is
   unaffected either way).
3. **A genuine incoming MIDI Pitch Bend message from Carla itself**, independent of the parameter
   mechanism above. The Monitor tab's raw MIDI log (fed by `logIncomingMidi()`, unconditional, not
   gated by debug mode - live for every message reaching `processBlock` regardless of host) showed
   **nothing** right after loading in Carla. Separately, a physical controller's pitch bend wheel
   has **zero effect** in Carla (works correctly in Standalone). Ruled out.
4. **MIDI channel mismatch** (checked for the Qtractor silence specifically) - already on channel
   2 (Part 1's factory channel), not the cause.
5. **Master Volume parameter reset to its minimum** (checked for the Qtractor silence) - the
   front-panel VOLUME knob (the same control as the `masterVolume` VST3 parameter) sits at its
   normal position, not at MIN. Not the cause.
6. **Output bus/port count.** The D-110 declares 7 output buses in total (the default-on stereo
   "Output"/MIX bus, plus 6 default-off mono buses for MULTI OUT 1-6 - see `createBuses()` in
   `PluginProcessor.cpp`), an unusual shape for a VST3 instrument that a leaner host could in
   principle mishandle. Checked in Qtractor: exactly 2 output ports listed for the D-110, same
   shape as Surge (which plays correctly), routing looked the same as Surge's too. Weakens this
   lead without eliminating it outright - the *content* written to those two ports at the sample
   level was never independently checked, only that the connections exist.

**Root cause not found for either symptom.** An attempt to reproduce the Carla behaviour on the
development machine (a scripted `carla-single` + a throwaway dummy-backend JACK server) hit
unrelated environment problems specific to that sandbox (no realtime scheduling permission, heavy
JACK xruns, `carla-single` producing total silence even for a plain note that should have worked)
and was abandoned rather than trusted - nothing about the actual bug was confirmed by directly
recording and analysing Carla's real audio output.

If this comes up again: don't re-check the six ruled-out theories above without new evidence. Two
tools already exist and need no further setup - the Monitor tab's "host sample rate" line and its
always-on raw MIDI log - open the extended editor's MONITOR tab in whichever host is misbehaving
and look. If a concrete mechanism in Carla's own source is ever identified, Alan has an existing,
documented workflow for patching Carla directly in a private fork (build process, packaging,
branch-per-bug convention already worked out for the SysEx bug above) rather than working around
it from this project's side.
