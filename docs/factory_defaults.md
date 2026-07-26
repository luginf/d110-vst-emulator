# The D-110's factory state

What a D-110 looks like with nothing changed. Every value below was read **off the
firmware's own display**, by `plugin/audio_test.cpp`, immediately after performing the
documented factory initialisation — not copied from a manual and not inferred from RAM.

## How to get there

Roland's procedure is to hold **WRITE/COPY** while switching the unit on and confirm with
**ENTER**. The plugin does exactly that:

- **Automatically**, the first time a new instance is switched on. A D-110 with blank
  battery RAM boots to an empty patch and reads as broken rather than as new, and since
  each plugin instance keeps its own firmware memory, "new" is the normal case.
- **On demand**, from **Factory Reset** on the panel's right-click menu.

Either way the firmware rebuilds its own patch memory, timbre memory and settings from the
preset ROM. There is no table of defaults written out in the plugin — the values are the
ROM's, restored by the firmware itself, which is why they are the real ones.

## SYSTEM page

| Parameter | Factory value |
| --- | --- |
| Master Tune | `442` |
| Mem Protect | `ON` |
| Control Ch. | `OFF` |
| Exclu Unit# | `17` |
| Overflow | `OFF` |

This page is global only. **The per-part MIDI channel is not here** — it lives under
PART SET, reached with the **PART** button.

## PATCH EDIT page — and the D-110's entire effects section

The whole of it. A D-110 has **one effect: a built-in digital reverb**. No chorus, no EQ,
no separate delay — delay is one of the reverb types. Patch Edit has exactly four
parameters, and three of them are the reverb:

| Parameter | Factory value | Range |
| --- | --- | --- |
| Name | `Patch   01` | 10 characters |
| Reverb Type | `5` | `1`–`8`, then `OFF` |
| Reverb Time | `5` | `1`–`8` (delay time when the type is a delay) |
| Reverb Level | `4` | `0`–`7` (0 = no reverb) |

The range was not taken from a manual — it was stepped through on the panel, one press at
a time, and read back off the display.

**Reverb is stored per patch**, which is where the D-110 differs structurally from the
MT-32: on the MT-32 reverb lives in the System area, and that is the area the sound engine
models. The D-110 keeps it in Patch Memory (`06 00 00`), a region munt has no concept of.

There is also a **Reverb Switch per part**, in the Timbre Temporary block. That one *is*
mirrored, so switching reverb off for an individual part does reach the sound engine — it
is the global type/time/level that cannot cross. See
[`sysex_address_map.md`](sysex_address_map.md).

## PART SET page, per part

| Part | Output Level | Pan | Key Range L | Key Range U | **MIDI Channel** | Partial Reserve |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 100 | `3>` | C-1 | G9 | **2** | 4 |
| 2 | 100 | `<3` | C-1 | G9 | **3** | 4 |
| 3 | 100 | `1>` | C-1 | G9 | **4** | 4 |
| 4 | 100 | `<1` | C-1 | G9 | **5** | 4 |
| 5 | 100 | `5>` | C-1 | G9 | **6** | 3 |
| 6 | 100 | `<5` | C-1 | G9 | **7** | 3 |
| 7 | 100 | `7>` | C-1 | G9 | **8** | 3 |
| 8 | 100 | `<7` | C-1 | G9 | **9** | 2 |

Pan is written as a distance and a direction: `3>` is three steps right of centre, `<3`
three steps left. The factory patch fans the parts outwards in pairs — 1 and 2 to either
side at 3, then 3 and 4 at 1, 5 and 6 at 5, 7 and 8 at 7.

Partial reserve sums to **32**, the D-110's whole polyphony: `4+4+4+4+3+3+3+2 = 27` for the
eight voice parts, with the remaining **5** reserved for the rhythm part.

## Nothing plays on MIDI channel 1

This surprises people, and it is genuine hardware behaviour rather than an emulation fault:
**Part 1 listens on channel 2**, part 2 on channel 3, and so on up to part 8 on channel 9,
with the rhythm part on channel 10. Channel 1 is assigned to no part at all, so a factory
D-110 is silent on it. Roland's own support note says the same: after a factory reset the
first patch has part 1 on MIDI channel 2.

Confirmed independently three ways: the PART SET display above; the firmware's own RAM,
where the channel map reads `1 2 3 4 5 6 7 8 9` (stored zero-based, so 1 means channel 2);
and by playing every channel in turn and watching which part indicator lights on the top
LCD row.

To put a part on channel 1, press **PART**, page to **MIDI Channel** with PARAMETER GROUP,
choose the part with PART, and set it with VALUE. That assignment is mirrored into the sound
engine, so it takes effect on what you hear as well as on the display.

## Cross-check against the firmware's RAM

The partial reserve values are also what the plugin reads out of battery RAM at `0x2D98`
when mirroring the System area into the sound engine:

```
04 04 04 04 03 03 03 02 05      <- reserve, 8 parts + rhythm, sums to 32
01 02 03 04 05 06 07 08 09      <- channel map, zero-based
```

The displayed page and the raw bytes agreeing field for field is what confirms that region
is mapped correctly — see [`sysex_address_map.md`](sysex_address_map.md).

## Sources

- [D-110: Initializing — Restoring the Factory Settings](https://support.roland.com/hc/en-us/articles/201965229-D-110-Initializing-Restoring-the-Factory-Settings)
- [D-110: Assigning MIDI Receive Channels to each Part](https://support.roland.com/hc/en-us/articles/201924709-D-110-Assigning-MIDI-Receive-Channels-to-each-Part)
- [D-110 Owner's Manual](https://synthmanuals.com/manuals/roland/d-110/owners_manual/)
