# Timbre group 5, which made two demo-song parts play 21 dB quieter

Closed 2026-07-31. This records both what turned out to be a real bug and what wasn't a
bug - so that the second half doesn't get "fixed" again from scratch.

## What was measured

In the demo song "Macho Memory", solo rendering (`plugin/solo_level_test.cpp`, target
`d110_solo_level`: a fresh machine instance for each row, the same song from the start,
with only one part delivered to the engine) gave:

| part | notes | dB relative to part 1 |
| --- | --- | --- |
| 1 | 124 | 0.0 |
| 5 | 0 | silence |
| 6 | 50 | **-20.8** |
| 7 | 66 | **-23.7** |
| rhythm | 241 | -0.7 |

At the same time, the firmware held **timbre group 5** for parts 6 and 7, and 0 or 1 for
all the rest.

## The real bug: timbre substitution on a Timbre Temporary write

Three facts, each independently verifiable.

**1. Group 5 is illegal even by the D-110's own standards.** The maximum-value table in
the control ROM (`ControlROMMap::patchMaxTable`, for `ctrl_d110_1_10_*` this is offset
`0x4A49` in the combined "firmware + presets" image) reads as follows:

```
03 3F 30 64 18 03 07 7F 64 0E 7F 7F 00 00 00 00 ...
^^ timbre group: maximum 3
```

In other words, the firmware itself declares a limit of 3 for the timbre group - in its
internal RAM, the value 5 means something other than the field that goes out over SysEx.
On write, the engine clamps 5 down to 3 (`MemoryRegion::write` limits each byte using
this same table), and group 3 is the **rhythm bank**.

**2. A Timbre Temporary write reloads the part's timbre from the bank.**
`Synth::writeMemoryRegion` for `MR_PatchTemp` calls
`parts[i]->setTimbre(&mt32ram.timbres[getAbsTimbreNum()].timbre)`, and
`getAbsTimbreNum()` is `group * 64 + number`. This is correct for the MT-32. For the
D-110 it is not: Tone Temporary, which the firmware has already handed to the bridge,
gets overwritten in the process.

**3. The bridge sent Timbre Temporary more often than timbres.** Over 40 seconds of the
demo, the per-region send counter showed 8 Timbre Temporary messages against 1 for each
Tone Temporary. Even a single such write after a timbre is enough to make the
substitution permanent.

The result, captured by **name** (the first ten bytes of a timbre are its name in
ASCII), from a single run of `plugin/tone_clobber_probe.cpp` (target
`d110_tone_clobber`):

```
 part   | firmware       | engine
      1 | "SlapBass 1"   | "SlapBass 1"
      5 | "Guitar 2  "   | "Guitar 2  "
      6 | "Syn Lead 1"   | "ClsdHiHat1"   <-- SUBSTITUTED
      7 | "Syn Lead 1"   | "ClsdHiHat1"   <-- SUBSTITUTED
      8 | "Strings 3 "   | "Strings 3 "
```

The lead was playing a closed hi-hat. The match on the other six parts in the same run
is the proof that the method works, not an excuse.

Why the substitution isn't visible for the other parts in this run: for legal groups, a
bank lookup returns exactly the same timbre that the firmware put into Tone Temporary,
because both halves pull it from the same ROM. **But this only holds for an UNEDITED
timbre**, and the claim "the substitution is harmless for the other parts", which
originally stood here, turned out to be broader than what was actually measured. The
engine's bank holds the factory sound; an edit made from the panel is held by the
firmware only in Tone Temporary, and reloading from the bank loses it regardless of
group. Measured separately - see "Verified on an ordinary patch" below.

## Fix

`D110Core::MirrorRegion::reassertAfterTimbreTemp` - the Tone Temporary region is resent
every time a Timbre Temporary goes out, even if it hasn't itself changed. The order is
set by position in `kMirrorRegions`; timbres come after Timbre Temporary, so the
reassertion always arrives last. Not a single line in munt had to change: the firmware
remains the master, the engine the executor.

As a side effect, the SysEx ring had to be enlarged from 64 slots to 256 (`kSysexSlots`),
and a `D110Core::sysexDropped()` counter added: a timbre change now sends nine messages
instead of one, and at 64 slots the measurement lost 3 messages. A silently dropped
mirror message is an unapplied parameter - exactly the class of bug being fixed here.

### Verified by sound

`d110_solo_level`, same song, after the fix, with zero losses on both counters:

| part | before | after |
| --- | --- | --- |
| 6 | -20.8 dB | **+5.2 dB** |
| 7 | -23.7 dB | **+2.5 dB** |

A/B control with the same instrument (`d110_demo_wav`, full mix, two consecutive runs
with the flag off and on): peak 0.988 / RMS 0.0899 versus peak 1.150 / RMS 0.1555. The
mix got louder by about 4.8 dB, because the two leads came back.

### Verified on an ordinary patch

Everything above was measured on the demo song, and the demo is a special case: the
preset ROM is what put timbre group 5 there - it can't be set from the panel. So a
question remained that the demo can't answer: is an edit a person makes from the panel
on an ordinary patch lost as well.

`plugin/tone_edit_survives_probe.cpp` (target `d110_tone_edit_survives`) runs this
experiment TWICE in a single pass - with timbre reassertion on and off
(`D110Core::setToneReassert`, a switch that exists only for test rigs). A check that can
only print a single "survived" looks the same whether the fix is working or there was
nothing to lose in the first place.

The probe finds both edit pages by button presses, not by memory inspection: it presses
a value and watches which RAM byte shifted by exactly the number of presses. It found
`Tone Edit/Common/Name` -> `0x21E4` (the first letter of part 1's timbre name) and
`TIMB_E/Part1/Key Shift` -> `0x2002`. The probe rejects the `Tone =` page (`0x2001`) on
its own: it selects a DIFFERENT sound, and changing the timbre there is legitimate.

Factory reset, then Number+ x5 on the name (`A`=65 -> `F`=70), then Key Shift by +3:

| timbre reassertion | firmware | engine | discrepancy |
| --- | --- | --- | --- |
| off (control) | `FcouBass 1` | `AcouBass 1` | 0 -> **1 of 246 bytes** |
| on (as in the plugin) | `FcouBass 1` | `FcouBass 1` | 0 -> **0** |

The engine came back with the FACTORY name - i.e., it reloaded the timbre from the bank
and lost the edit. Before the Key Shift change, both halves matched byte-for-byte in
both runs, so the loss happens exactly there, not somewhere along the way.

The send counters show the whole mechanism: Timbre Temporary went out 3 times in both
runs, and part 1's timbre - **5 times without reassertion and 8 with it**, exactly one
extra for every Timbre Temporary send. Dropped mirror messages are zero in both cases.

Hence the correction above: the issue was never group 5. It made the loss AUDIBLE (lead
versus hi-hat), but any panel timbre edit followed by any part-parameter change would
have been lost just the same.

# Output was exceeding full scale

Found right after the previous one: at unity volume, the demo peaks around 1.1.

This is a discrepancy with the real unit, not its character. The D-110's DAC is
sixteen-bit and cannot output more than full scale, and the volume knob is analog and
sits AFTER it. The engine does model this saturation - `Synth::clipSampleEx(Bit32s)`
clamps the voice sum to +-32767 - but the same function has a float overload, and it's
deliberately empty:

```cpp
static inline float clipSampleEx(float sampleEx) {
    return sampleEx;
}
```

Our build goes through the float path, so the DAC model never reached the output.

Before adding a limiter, it was measured how much signal actually exceeds full scale at
all: on "Macho Memory" **69 samples out of 6,614,016, i.e. 0.001%** (`d110_demo_wav`
prints this on every run). Isolated peaks, not sustained clipping - the real DAC would
have shaved them off inaudibly.

The fix in `processBlock`: saturate to +-1 first, then multiply by volume - in the same
order as in the real unit. After it, the peak is exactly 1.000 and zero out-of-scale
samples. The knob runs 0..2 with unity gain at its midpoint, which is also its default
position, so at rest the output never exceeds full scale; past the midpoint, that's
requested gain, not the unit's behavior.

Not done, and needs a separate decision: the real DACs in this family doubled the volume
by permuting the input bits, and their overflow is the well-known "MT-32 digital
overload". The engine can reproduce it (`DACInputMode_GENERATION1` and `GENERATION2`),
but we run in the default `NICE` mode, which its own documentation describes as "higher
quality than the real units".

## This, however, was NOT a bug: part 5 with no notes

"Macho Memory" simply doesn't use parts 3, 5, and 8. Loss-free counters from a single
run: these three parts have **zero** note-on events and zero writes to the part byte
`f3a0[]` - meaning the bridge lost nothing, the firmware just never assigned them.

The complaint "the part doesn't sound even though its indicator is lit" came from
reading the indicator backwards. The top row of the LCD shows the **part's digit when
it's silent**, and replaces it with a solid 5x7 block **when the part is sounding** -
exactly as on the real hardware. The block is a custom character the character
generator can't name, and in text dumps it showed up as `?`. That's exactly why
`dumpIndicatorGlyphs()` in the probe prints these cells as dots:

```
   ##### ##### ##### ##### ##### ##### ##### .###. ####.
     ?     ?     3     ?     5     ?     7     8     R
```

Here parts 1, 2, 4, and 6 are sounding - while 3, 5, 8, and rhythm show their digit,
because they're silent. This matches the note counters from the same run, right down to
the exact set of parts.

## Tools

- `plugin/tone_clobber_probe.cpp` (`d110_tone_clobber`) - name comparison from a single
  run, with a read-path control and completeness counters. Start with this one.
- `plugin/tone_edit_survives_probe.cpp` (`d110_tone_edit_survives`) - the same question,
  but on an ordinary patch and with an edit made from the panel. It finds the menu pages
  itself, and runs the experiment twice - with and without timbre reassertion - so the
  result has a control capable of showing failure.
- `plugin/part_state_compare.cpp` (`d110_part_state`) - a byte-by-byte comparison of
  Timbre Temporary. Normally differs in exactly byte 0 for parts 6 and 7 (firmware 5,
  engine 3).
- `plugin/solo_level_test.cpp` (`d110_solo_level`) - per-part level, a fresh machine
  instance per row.

Two warnings about the measurements themselves, each of which cost this investigation a
wrong conclusion:

- `Synth::playSysex` queues the message; it's applied during sound rendering. There must
  be a `processBlock` between writing and reading it back.
- Sleeping through the firmware boot instead of rendering audio means starting the
  measurement with an unapplied mirror queue: the very first render plays the whole
  backlog on top of the state being measured.
- A target that's missing from `scripts/build_plugin.bat` silently stays stale. That's
  how "the tool reads zeros" turned out to be running a binary built before the address
  fix.

## What remains open

- What group 5 actually means in the firmware's internal RAM was never deciphered. Nor
  is it needed: the truth about a part's sound lives in Tone Temporary, and the bridge
  carries it across whole. Deciphering it would only be needed if a panel page appeared
  that displays or edits this group.
Nothing. The second defect found along the way is covered below, and it's closed too.

# Notes outran the parameters they depended on

`Attempted to play unmapped key 25/27` at the start of the demo - two drum hits that the
engine silently dropped.

The cause isn't a race with the emulator, but two different delivery disciplines within
a single `processBlock`:

| path | call | when it's applied |
| --- | --- | --- |
| mirror parameters | `Synth::playSysex` | queued in the engine with a timestamp, applied when audio rendering reaches it |
| firmware notes | `Synth::playMsgOnPart` | **immediately**, directly inside `Part::noteOn` |

The loop in `processBlock` processes parameters first, then notes - but the actual order
was reversed, because notes took effect immediately while parameters waited for
rendering. At the start of the song, the firmware loads the rhythm map and almost
immediately hits keys 25 and 27; the hits were applied before the map, found timbre 127
(OFF) there, and were dropped.

Fix: the bridge sends `playSysexNow`, and both halves take effect at the moment the
queues are processed - the loop order becomes real. The bank-import and menu
timbre-change queues are deliberately left on `playSysex`: their source is a user
action, seconds removed from any note.

Measured (`plugin/rhythm_map_probe.cpp`, target `d110_rhythm_map`, one run, zero losses
on both counters): the rhythm map goes out at 61 and 93 ms, the first hits start at 380
ms, keys 27 and 25 come at 579 and 747 ms. Firmware and engine hold identical records
both before the song (all OFF) and after (timbres 80, 86, 87, 81 ...). Zero warnings -
against a steady two per run before the fix, on the same unchanged `d110_tone_clobber`
and `d110_demo_wav`.

Another measurement trap that made the probe's first run inconclusive: the region send
counter grows on the machine's thread, but was only polled after button presses - by the
very first poll it had already accumulated, so all it read was "sometime up until now".
Polling has to run on the same loop that renders audio, and cover the button presses
too.
