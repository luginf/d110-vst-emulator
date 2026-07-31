# D-110 SysEx address map, and how it maps onto both munt and the firmware's RAM

Source: the D-110 Owner's Manual, "5. PARAMETER ADDRESS MAP" (manual p.117),
scanned in [`manuals/D-110_SysEx_Scans.pdf`](manuals/D-110_SysEx_Scans.pdf).
Cross-checked against `munt/mt32emu/src/MemoryRegion.h` and against a measured
diff of the firmware's own battery RAM.

This is the reference for the bridge that carries panel edits from the emulated
firmware into munt's LA engine.

## The map as Roland documents it

Addresses are 7-bit hexadecimal (each byte carries 7 bits), which is what
mt32emu's `MT32EMU_MEMADDR()` macro converts.

| Start | Description | Entry stride |
| --- | --- | --- |
| `02 00 00` | Tone Temporary Area (synth part) | |
| `03 00 00` | **Timbre** Temporary Area, part 1 … part 8 at `03 00 70` | `0x10` |
| `03 01 00` | Timbre Temporary Area, rhythm part | |
| `03 01 10` | Rhythm Setup Temporary Area | |
| `04 00 00` | **Tone** Temporary Area, part 1 … part 8 at `04 0D 3A` | `0x176` (246) |
| `05 00 00` | Timbre Memory #1 … #128 at `05 07 78` | `0x08` |
| `06 00 00` | Patch Memory #1 … #64 at `06 3F 00` | `0x100` |
| `08 00 00` | Tone Memory #1 … #64 at `08 7E 00` | `0x200` |
| `10 00 00` | System Area | |
| `20 00 00` | Display | |
| `40 00 00` | Write Request | |

**Roland renamed things between the MT-32 and the D-110**, which is the only
reason the two look different at first glance:

| MT-32 term (what munt calls it) | D-110 term |
| --- | --- |
| Patch | **Timbre** |
| Timbre | **Tone** |

## It is the same map munt implements — exactly

`MemoryRegion.h` lines up address for address and stride for stride:

| munt region | munt base | D-110 area | agrees? |
| --- | --- | --- | --- |
| `PatchTemp`, 9 entries of 16 | `0x030000` | Timbre Temporary, 8 parts + rhythm at `03 01 00` | ✅ |
| `RhythmTemp`, 85 | `0x030110` | Rhythm Setup Temporary | ✅ exact |
| `TimbreTemp`, 8 entries of 246 | `0x040000` | Tone Temporary, part 2 at `04 01 76` = +246 | ✅ exact |
| `Patches`, 128 entries of 8 | `0x050000` | Timbre Memory #1–128, stride `0x08` | ✅ exact |
| `Timbres`, 256 entries of 256 | `0x080000` | Tone Memory, stride `0x200` | ✅ |
| `System` | `0x100000` | System Area | ✅ exact |
| `Display` | `0x200000` | Display | ✅ exact |

> **Reverb, 2026-07-31**: the D-110 documents its reverb as living in Patch Memory
> `06 00 00`, which munt has no region for — but the firmware also keeps the **live**
> Type/Time/Level in the System Area at `0x2D95`/`0x2D96`/`0x2D97`, i.e. system offsets
> 1/2/3, which are exactly munt's `reverbMode`/`reverbTime`/`reverbLevel`. Time and Level
> are mirrored (`0x2D96` → `0x100002`, 2 bytes) and follow the patch; Type is not, because
> eight D-110 types onto four engine modes has no honest mapping. Measured by
> `plugin/reverb_path_probe.cpp`; see [`factory_defaults.md`](factory_defaults.md).

**The one area munt has no region for is `06 00 00` Patch Memory** — the D-110's
64 patches, which assign a timbre and its settings to each of the 8 parts. The
MT-32 has no such concept. This does not block anything: when the firmware
*applies* a patch it writes the resulting per-part assignment into the Timbre
Temporary Area at `03 00 00`, which munt does model. So mirroring `0x030000`,
`0x040000`, `0x050000`, `0x080000` and `0x100000` covers everything munt can
actually render.

## And it is the same layout as the firmware's RAM

Measured, not assumed — see [[d110-bridge-design]] for the method. Editing
Fine Tune on the panel moves exactly one meaningful byte in the 32 KB battery RAM:

```
RAM offset 0x2003:  50 -> 56  after 6 presses of Number +
RAM offset 0x2003:  50 -> 54  after 4 presses          (re-run, same offset)
```

`fineTune` is byte 3 of Roland's per-part structure (0 timbreGroup, 1 timbreNumber,
2 keyShift, 3 fineTune), and 50 is the centre of its 0–100 range. So:

> **RAM `0x2000` == SysEx `0x030000`** (Timbre Temporary Area).

### Measured region bases

`plugin/bridge_probe.cpp` automates the method: it boots the firmware in-process,
walks its menus with the panel buttons and diffs the RAM around each edit, flagging
the byte whose delta equals the number of presses. Results so far:

| Edited on the panel | RAM offset | Delta | Meaning |
| --- | --- | --- | --- |
| Timbre Edit, page 1 | `0x2001` | +5 / 5 presses | per-part byte 1 = **timbreNumber** |
| Timbre Edit, page 2 | `0x2002` | +3 / 3 presses | byte 2 = **keyShift** |
| Timbre Edit, page 3 | `0x2003` | +4 / 4 presses | byte 3 = **fineTune** |
| System, Master Tune | `0x2D94` | +7 / 7 presses | first byte of the System Area |

So two bases are now pinned:

> **RAM `0x2000` == SysEx `0x030000`** — Timbre Temporary, 9 × 16 bytes
> **RAM `0x2D94` == SysEx `0x100000`** — System Area

The per-part structure agreeing on **three consecutive bytes in the order Roland
documents** is much stronger evidence than any single byte.

### Tone Temporary Area: `0x21E4`, measured by comparison

**Confirmed exactly**, and by a method that cannot give a false positive.
`plugin/tone_probe.cpp` reads the tone mt32emu holds for each part back out of the
engine — `Synth::readMemory()` is a real public API, and the Tone Temporary region
is backed by real memory — and slides those 246 bytes over the whole 32 KB of
firmware RAM looking for the best match. Both halves load their tone from the same
ROM, so where the bridge is right the two copies must be identical.

All eight parts match **246 bytes out of 246**, at exactly `base + part × 246`:

| Part | RAM | Tone | Part | RAM | Tone |
| --- | --- | --- | --- | --- | --- |
| 1 | `0x21E4` | AcouBass 1 | 5 | `0x25BC` | Trombone 1 |
| 2 | `0x22DA` | AcouPiano2 | 6 | `0x26B2` | Sax 1 |
| 3 | `0x23D0` | Guitar 1 | 7 | `0x27A8` | Sax 3 |
| 4 | `0x24C6` | Trumpet 2 | 8 | `0x289E` | Strings 3 |

> **RAM `0x21E4` == SysEx `0x040000`** — Tone Temporary, 8 × 246 bytes

An exact match is also the **null test**: mirroring a tone nobody edited provably
changes nothing. That is what the earlier span-difference method could never
establish — it only ever bounded the base from *below*, because the tail bytes of
two different tones need not differ. (The "+460 cents" that once seemed to condemn
`0x21E4` was not the base at all; it was the test harness's zero-crossing pitch
meter latching onto a different harmonic. The base was right all along.)

### System Area: correct, but only partly safe to mirror

The base is certain — read as Roland's structure, `0x2D94` gives
`reserveSettings` summing to exactly **32** and `chanAssign` reading
**1 2 3 4 5 6 7 8 9**, the D-110's documented "Part 1 answers on MIDI channel 2"
assignment. Neither could be coincidence.

But three fields must **not** be sent to the engine:

| Offset | Field | Reads | Why it is excluded |
| --- | --- | --- | --- |
| 0 | `masterTune` | `0x4A` | The panel shows 442, while Roland's documented 0–127 → 432.1–457.6 Hz mapping makes `0x4A` ≈ 447. The two scales disagree, so mirroring it would detune everything against the display. |
| 1–3 | reverb type/time/level | `04 04 04` | **The D-110 has more reverb types than the engine can represent.** Stepped through on the panel, its Reverb Type runs `1…8` plus `OFF`; mt32emu models the MT-32's four modes only (`reverbMode` 0–3: room, hall, plate, tap delay). The stored `04` is the D-110's Type 5 — a perfectly valid setting with no MT-32 equivalent. See below. |
| 22 | `masterVol` | `00` | **The D-110's volume is a physical knob**, so the firmware never fills this byte in. Sending it set the engine's master volume to zero and dropped the whole instrument by about 30 dB — which is how this was found. |

So only the verified middle is mirrored:

> **RAM `0x2D98` == SysEx `0x100004`** — partial reserve + MIDI channel map, 18 bytes

Not yet found: the Part Set page's first parameter did not move any byte by the
press count — most likely it was already at its maximum (Output Level was showing
100). Re-probe from a lower value.

## Host MIDI going the other way

The bridge above carries panel edits *out* to the engine. Traffic also has to go
*in*: on the real instrument one MIDI cable feeds both the CPU and the voice
circuitry, and here the CPU is MAME while the voice circuitry is mt32emu, so both
must be told. Without it the control board never learns a note was played, so the
top LCD row never lights the playing parts and the display drifts away from the
host's program changes.

The injection point is the CPU's own serial receiver, `m_maincpu->serial_w()` —
exactly what the driver uses for its built-in test note, so no MAME patch is
needed. Two details matter:

1. **It must be paced by a machine timer**, not driven from the OSD's `update()`.
   The emulated UART is a single register with no FIFO (`i8x9x_device::serial_w`
   overwrites `sbuf` and raises an interrupt), so two writes without emulated time
   between them leave only the second byte. A periodic `emu_timer` at MIDI's own
   3125 bytes/s spaces them exactly as a real cable would.
2. **That timer has to be allocated during OSD `init()`.** MAME closes save-state
   registration once the machine has finished starting, and allocating a timer
   after that is a fatal error.

Result: playing a channel replaces the matching part's digit on the top LCD row with
a solid block for as long as that part sounds — the hardware's own behaviour.
Confirmed in a live DAW.

**Do not use the offline harness to judge this.** `plugin/audio_test.cpp` renders
audio far faster than real time while the control board runs on its own thread *at*
real time, so its notes reach the firmware in bursts and its LCD reads race the
firmware's redraw. Identical code has produced a perfect 1:1 channel-to-part map,
every indicator lit at once, and nothing lit at all, on three successive runs. The
harness prints the check as advisory for that reason. Anything about **when** the
firmware reacts has to be judged where `processBlock` is called continuously in real
time, i.e. in a host.

The link underneath it *is* measurable offline and is sound: every byte queued is
delivered, none dropped, at MIDI's own 3125 bytes/s.

## Two facts that shape the bridge

1. **The firmware does not transmit edits.** A 40-second run that edited a
   parameter six times and changed the patch three times emitted exactly one byte
   from the CPU's serial TX. So the bridge reads RAM; it cannot just listen.
2. **But the firmware can dump on request.** The service notes' bug list refers to
   a "Data Transfer" mode with **"Dump One Way"** and **"Dump Hand Shake"**, which
   emit the whole memory as standard Roland SysEx. That is too slow and too modal
   to use live, but it is an excellent way to *validate* the offset map: trigger
   one dump, capture it, and check it against what mirroring the RAM produces.

## Firmware versions

The service notes document up to **v1.07** and note that v1.07 matches the 1M-type
mask ROM. MAME carries **v1.06** and **v1.10**. v1.13 exists (the project owner
flashed it into their own unit) but is later than this service notes edition and
is not in MAME's romset. Nothing depends on it.

Relevant v1.06 fixes, worth knowing because they describe real behaviour:
*"In MIDI Exclusive, each part parameters stored in Patch memory area cannot be
read/written properly"* and, as an improvement, *"when parameters are changed
using MIDI exclusive, the display will show `*` marks with Pitch, Timbre and Tone
numbers"* — so an edit pushed in over SysEx is expected to mark the display.

## Sources

- Owner's Manual MIDI section — [llamamusic.com/d110/D-110_SysEx_Scans.pdf](https://llamamusic.com/d110/D-110_SysEx_Scans.pdf)
- Service Notes — [archive.org/details/roland_D-110_SERVICE_NOTES](https://archive.org/details/roland_D-110_SERVICE_NOTES)
