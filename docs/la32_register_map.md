# LA32 registers, recovered from what the firmware writes into them

First pass, 2026-08-01. Tool: `plugin/la32_regmap_probe.cpp`, target
`d110_la32_regmap`.

There is no register map for the MB87136APF anywhere: the chip isn't emulated by MAME or
by anyone else. The service notes give only conclusions (`service_notes_findings.md`):
nine address lines `A0-A8`, i.e. **512 registers**, an eight-bit data bus, a `WR` input,
an interrupt output. The `0x0C00`-`0x0DFF` window, found earlier from unmapped accesses,
is exactly that size - meaning this is the entire synthesis control interface, in full.

A register cannot be read - it is write-only. So its value is worked out by forcing it to
change: three stimuli are used, differing in exactly one property.

| | note | velocity |
| --- | --- | --- |
| A | 60 | 100 |
| B | 72 (an octave higher) | 100 |
| C | 60 | 40 |

A cell that differs between A and B but not between A and C carries pitch; the other way
round, it carries velocity.

## Control

A window of the same length **with no note: zero writes**. Everything that lands in the
table is caused by the note, not by background firmware activity. No run overflowed the
capture.

## Structure: five banks, four bytes per voice

| Bank | what is known about it |
| --- | --- |
| `0x0C00` | four bytes per voice; one of them changes from run to run |
| `0x0C40` | three bytes per voice, constant for a given timbre |
| `0x0C80` | four bytes; **+3 carries velocity** |
| `0x0CC0` | not a one-shot setting but a **continuous stream** of updates for as long as the note sounds |
| `0x0D00` | four bytes, constant for a given timbre |

Notes get **sequential 4-byte slots**: the first note took offsets 0-3 in each bank, the
second 4-7, the third 8-11.

**This is the main trap in this measurement, and the first version of the probe fell
right into it.** Runs cannot be compared by ABSOLUTE address: each note gets its own
slot, so any cell looks "changed" simply because it wasn't touched in the other run.
This is how the probe confidently declared half the window "pitch" and half "velocity" -
and all of it was an artifact. Addresses are normalized to an offset within the slot, and
only then compared.

## Second pass: the slot is read from the firmware, a fourth stimulus is added

The slot is no longer guessed. The firmware maintains a state table (RAM `0x2DC0 + 2n`,
`D110Core::kSlotStateTable`): a free slot holds `0x80`. A snapshot of this table, taken
while the note is still sounding, directly names the slots allocated to it. Result:

| run | slots |
| --- | --- |
| A note 60, velocity 100 | 0, 1 |
| B note 72, velocity 100 | 2, 3 |
| C note 60, velocity 40 | 4, 5 |
| D note 60, velocity 100, different timbre | 6, 7 |

**This confirms the structure: two bytes per slot, two slots per note.** A bank spans
`0x40` = 64 bytes, the chip has 32 slots, so each slot gets two bytes; a note takes four
bytes because it uses two partials. This matches the earlier measurement, where exactly
two slots per note transitioned from `0x80` to `0x40`.

The fourth stimulus is **a different timbre** at the same note and velocity (from the
panel: Timbre → Edit → the "Tone =" page → Number+; the part's timbre became 1/31
instead of 0/17). It turned out to be decisive: without it, thirteen cells looked
"meaningless".

### Result, zero capture losses

| bank | partial | byte | A | B pitch | C velocity | D timbre | what it moves |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `0x0C00` | 0 | 0 | `FF C4` | `FF C4` | `FF C4` | `FF D2` | timbre only |
| `0x0C00` | 0 | 1 | `F1` | `E5` | `D4` | `95` | everything |
| `0x0C00` | 1 | 0 | `FF D0` | `FF D0` | `FF D0` | `39 9C` | timbre only |
| `0x0C00` | 1 | 1 | `00` | `00` | `00` | `00` | nothing |
| `0x0C40` | 0 | 1 | `BA` | `BA` | `BA` | `90` | timbre only |
| `0x0C40` | 1 | 0 | `B0` | `B0` | `B0` | `D1` | timbre only |
| `0x0C40` | 1 | 1 | `76` | `76` | `76` | `8E` | timbre only |
| `0x0C80` | 1 | 0 | `FF CF` | `FF CF` | `FF CF` | `FF BE` | timbre only |
| `0x0C80` | 1 | 1 | `7E` | `7E` | **`6E`** | `7A` | **velocity and timbre** |
| `0x0CC0` | both | both | stream | stream | stream | stream | everything |
| `0x0D00` | 0 | 1 | `18` | `18` | `18` | `28` | timbre only |
| `0x0D00` | 1 | 0 | `12` | `12` | `12` | `52` | timbre only |
| `0x0D00` | 1 | 1 | `D9` | `D9` | `D9` | `6E` | timbre only |

Summary: nine registers are moved by **timbre only**, one by **velocity and timbre**,
five by everything at once (this is the `0x0CC0` stream), four are moved by nothing.

### Two findings that stand apart

**`0x0C80`, partial 1, byte 1 - level.** The only register that separated out velocity:
`7E` at velocity 100 for BOTH notes, `6E` at velocity 40 for the same note. Pitch doesn't
touch it at all.

**`0x0C00`, partial 0, byte 1 - keyboard tracking, exactly one unit per semitone.** `F1`
at note 60 and `E5` at note 72: a difference of **12 across 12 semitones**, decreasing.
But it also moves with velocity (`D4` at 40), so this isn't pitch as such but a parameter
with keyboard tracking - in LA synthesis that's how filter cutoff frequency behaves.
There is no basis yet to claim exactly which parameter this is; only the coefficient
itself has been established.

**A leading `FF` is not a value.** In many cells the first write is `FF`, with the
meaningful number coming second; the same pattern shows up in the write order per note.
It looks like a register reset before loading, and `FF` should not be read as data.

## What the first pass established

**`0x0C80` +3 - velocity.** `7E` at velocity 100 for both notes, `6E` at velocity 40 for
the same note 60. This is the only register that cleanly separated the stimuli.

**Bank `0x0CC0` is a stream, not a setting.** It's written from ROM `0x2C0D` over and
over while the note is held; on a two-second window it alone overflowed the capture ring
with thousands of writes. Looks like envelope servicing.

**The `0x0CC0` stream keeps servicing a voice that has ALREADY been released.** In run B,
writes go both to the new note's slot and to the slot of the previous note, which is
still decaying. So for this bank the "slot = lowest used offset" normalization doesn't
work, and its rows in the table cannot be read - unlike the other four banks, where only
the new voice is touched.

## What could NOT be established

**No pitch register was found.** No cell gave "changes between A and B, but not between
A and C". Cell `0x0C00` +1 changes across all three runs (`F1`, `E5`, `D4`), i.e. it
doesn't separate pitch from velocity, and there are no other candidates.

The negative result is honest but not final: pitch could well live in the `0x0CC0`
stream, whose analysis broke down for the reason above, or it could be written as a
multi-byte value that a "single address at a time" view cuts apart. There is no basis to
claim it isn't here.

## Third pass: PITCH FOUND - it's a sixteen-bit number in bank `0x0CC0`

The previous verdict that "bank `0x0CC0` depends on everything" was a comparison
artifact: streams were compared whole, tails of different lengths included, and the
vectors didn't match even when their BEGINNINGS matched element by element. And the
beginnings do match - for note 60 at velocity 100 and at velocity 40 the first values are
identical, while for note 72 they differ by exactly 17.

A set of values doesn't answer this question; a LAW does. Chromatic run, the first
meaningful value of each register (leading `FF` skipped):

| note | `0CC0.p.1` (high byte) | `0CC0.p.0` (low byte) | as a number | step |
| --- | --- | --- | --- | --- |
| 60 | `81` | `0D` | `0x810D` | - |
| 61 | `82` | `63` | `0x8263` | **+342** |
| 62 | `83` | `BA` | `0x83BA` | **+343** |
| 63 | `85` | `11` | `0x8511` | **+343** |

**The two slot bytes in this bank are a single sixteen-bit value**, high byte `.1`, low
byte `.0`: the low byte overflows (`0D → 63 → BA → 11`) exactly when the high byte
increments by one. Step **342-343 per semitone**, i.e. about **4112 per octave**.

This matches the independent measurement from the second pass, where across an octave
the high byte gave `+16` and the low byte `+17`: `16 × 256 + 17 = 4113`. Two different
experiments, two different timbres, the same scale.

Both partials carry their own such number (`0CC0.0.*` and `0CC0.1.*`), and both move the
same way - `81/91` for one versus the other differ by a constant offset, meaning the
partials are detuned relative to each other by a fixed amount, as is expected in LA
synthesis.

### The limit the method ran into

The slot is identified by the transition from `0x80` (free) to an occupied state. **But
the `edc0` table does not return to `0x80` when a voice is released** - this was already
measured earlier (`la32_interface.md`: the reset pass at ROM `0x29BB` doesn't touch it,
and voices that have finished playing stay at `0x20`). So the marker only works for
slots that have NEVER been used yet, and there are only 32 of those.

That's why the chromatic run only resolved notes 60-63, and after that the slots ran
out: a four-partial timbre eats four per note, and the four main stimuli had already
used up 16 of the 32 before it. A pause of one and a half seconds between notes doesn't
help and can't help - it isn't a matter of time.

**Fixed completely, and the fix turned out simpler than planned.** The chromatic run is
taken FIRST, before all other experiments - and that's enough. The problem wasn't the
slot-identification method but that slots were running out: the firmware hands out
never-used slots first, there are 32 of them, and four stimuli ate sixteen before the
chromatic run even started. Neither a pause between notes nor filtering by subroutine
address helped, because that wasn't the cause - **the order of the experiments was the
cause**.

Eight clean notes instead of four, slots 0-3, 4-7, ... 28-31 in sequence, and the pitch
series is complete:

| note | value | step | note | value | step |
| --- | --- | --- | --- | --- | --- |
| 60 | `0x810D` | - | 64 | `0x8667` | +342 |
| 61 | `0x8263` | +342 | 65 | `0x87BF` | +344 |
| 62 | `0x83BA` | +343 | 66 | `0x8915` | +342 |
| 63 | `0x8511` | +343 | 67 | `0x8A6B` | +342 |

Seven steps in a row, 2398 total across seven semitones: **342.57 per semitone, i.e.
≈4111 per octave**. The ±1 spread in individual steps is rounding of a constant
quantity, not different step sizes.

Let's be honest about this: 4111 is close to 4096 (`0x1000`), which would suggest
itself as "twelve bits per octave", but **is not equal to it** - a discrepancy of 15
units, i.e. about 6 cents, too large to be byte rounding. What exactly 4111 means has
not been established.

### The remainder that never gave in

From the ninth note on, the columns fill with zeros again - the slots ran out and reuse
began. Verified that this is NOT solved by a pause (it isn't a matter of time) or by
filtering on subroutine address: the writes come from the same note-on addresses as the
real ones, just with zeros. It looks like slot reuse begins with the same subroutine
zeroing it out. For eight notes this doesn't get in the way; a full octave will need
either a two-partial timbre (enough for sixteen notes) or working out how note-on
differs from zeroing within a single subroutine.

### History of the method, so as not to redo it Slot identification is now taken FROM
THE WRITES THEMSELVES, in bank `0x0C00`: it's written when the voice is issued, unlike
the envelope bank, which concurrently continues playing out previous notes. The state
table was kept as a cross-check, and **both methods agreed on all four stimuli** - slots
0-3, 4-7, 8-11, 12-15.

But past the fourth chromatic note this still doesn't hold up, for a different reason:
once unused slots run out, the firmware starts reusing them, and bank `0x0C00` receives
not only note-on writes but also **note-off** writes for previous voices. The "slot
touched in `0x0C00`" marker doesn't distinguish between them, and from note 64 onward the
table fills with zeros from other voices' note-offs - which shows up in the difference
series as a break.

**What will distinguish them:** the address of the subroutine that made the write. It's
present in the capture (`SoWrite::pc`); note-on comes from `0x3615`-`0x3657` (analyzed in
`la32_interface.md`), note-off from elsewhere. Filtering by this address will separate
one from the other.

## Targeted timbre edits: registers split apart by partial

The "different timbre" stimulus is too crude - it moves nine registers at once and
doesn't say which one is responsible for what. The `d110_la32_regmap tone` mode changes
NOT the whole timbre but a single parameter within it, and compares against a
measurement taken with the same note right before the edit.

**One experiment per run, and this is forced.** There are 32 slots, the firmware hands
out never-used ones first, and two experiments don't fit in one run - combining them was
tried three times, spoiling the measurement each time. The mode is chosen via an
argument.

| edit page | what shifted in the timbre (RAM `0x21E4`+) | which registers shifted |
| --- | --- | --- |
| 0 (entry) | byte +0 - **name** | **none** |
| 1 | byte +10 | exactly **one partial**, across all five banks at once |
| 2 | byte +11 | exactly **the other partial**, also across all five |

The number of the affected partial differs from run to run (in one it was 0 and 2, in
another 1 and 2) - it depends on whatever structure value ended up set, not on the page.
What stays constant: **the edit touches the registers of exactly one partial and no
others**.

What this looks like in values (page 1, byte +10 from 10 to 12):

```
0C00.1.0 : 2C D3   ->  FF B5        0C80.1.0 : FF B5  ->  00
0C00.1.1 : 00 00   ->  87 00        0C80.1.1 : 87 00  ->  00
0C40.1.1 : 80      ->  60           0D00.1.0 : 32     ->  B2
0CC0.1.0 : FF 0D 27 31 ... -> FF 00 1A 24 ...
```

The values don't just change - they **move between banks**: what used to sit in `0C80`
ends up in `0C00`, and `0C80` is zeroed.

**Page 0 is a built-in control, and it passed.** Editing the name doesn't change the
sound, and no register shifted because of it. If one had, all the other rows of the
table would be noise.

Bytes +10 and +11 are the first bytes after the ten-character name, i.e. **the structure
of partial pairs 1&2 and 3&4**. Each moves the registers of only its own pair, and
radically so: values move between banks (`0C00` gets what used to be in `0C80`, and vice
versa). It looks like structure switches not a parameter within the partial but the very
bank assignment for it - which is natural for LA synthesis: structure is exactly what
determines what a partial is, a generator or a filter.

**A caveat that travels with this:** on pages 1 and 2 the capture overflowed (602/1747
and 2209/2964 writes lost), so the lists of changed registers are a **lower bound**. That
what's listed changed is established; that nothing beyond it changed is not.

### Part+ leads to the partial parameters - found by exploration

The `d110_la32_regmap find` mode sweeps a grid of button presses and, at each cell,
checks which timbre byte shifted. It assumes nothing about what the buttons do - the
byte names the parameter itself. **No notes are played during this, so the exploration
doesn't spend voice slots**, and the whole grid can be swept in one run, whereas
measuring registers is bottlenecked at 32 slots.

Answer: **`Part+`**.

**`Part+` selects the partial, `Group+` selects the parameter within it.** The whole grid
has been captured, and it agrees without a single exception:

| `Part+` | partial base in the timbre | bytes shifted at `Group+` 0..6 |
| --- | --- | --- |
| 1 | **14** | 14, 22, 34, 37, 42, 55, 61 |
| 2 | **72** | 72, 80, 92, 95, 100, 113, 119 |
| 3 | **130** | 130, 138, 150, 153, 158, 171, 177 |
| 4 | **188** | 188, 196, ... |

The bases are spaced exactly **58 bytes** apart - this is the size of a partial within a
D-110 timbre (10 bytes of name, 4 bytes of the common part, then four partials of 58
bytes each; 14 + 4×58 = 246, the full length of the timbre). The offsets that `Group+`
produces are the same for all four partials: **+0, +8, +20, +23, +28, +41, +47**.

**A trap that had to be worked around:** values in firmware memory persist across runs,
and previous runs only ever incremented them - by this point many were sitting at the
upper limit, and "nothing shifted" would mean not "the page is empty" but "there's
nowhere left to add to". The probe now, on seeing no upward movement, tries downward.

### Starting from a clean timbre: the partial's first parameter is pitch, and it confirms bank `0x0CC0`

With a factory reset at the start of the run (timbre 1/24, structures 2 and 0, the note
takes exactly two slots), editing **byte 14** - the first parameter of partial 1 - from
36 to 39 shifted **exactly one register and no others**:

```
0CC0.0.1 :  FF 4E 4E 4E ...  ->  FF 52 52 52 ...
```

The high byte of pitch, `4E` → `52`, i.e. **+4 in the high byte = +1024** in the
sixteen-bit value. Three semitones on the previously measured scale is 1028. Matches.

**This is an independent confirmation of bank `0x0CC0`, and it's valuable precisely
because the stimulus is completely different.** Earlier, pitch was identified by NOTE
NUMBER; now, by a TIMBRE PARAMETER, and both point to the same cell. This also names
what partial byte +0 is: its coarse pitch.

**The remaining partial parameters don't reach the registers.** All seven reachable
offsets were tried (+0, +8, +20, +23, +28, +41, +47), each time from a factory reset,
and **only +0 shifted a register**. Bytes 22, 34, 37, 42 changed in the timbre, but
nothing was reflected in the chip.

From this a picture of the interface itself emerges, and it explains bank `0x0CC0`:
**what goes to the chip is not a description of the sound but its current state**. Pitch
is written once at voice note-on, while everything else - envelopes, filter, level - the
firmware, it seems, computes itself and sends as a result into the `0x0CC0` stream,
which is continuously updated from ROM `0x2C0D` for as long as the note sounds. For a
1988 chip this makes sense: there's nothing in it to compute envelopes with.

**A caveat that must travel with this conclusion:** the note is held for only 0.5 s. A
parameter affecting the slow part of the envelope might simply not have shown up within
this window. The claim that a parameter "doesn't reach the registers" has been checked
on a short window, and needs re-checking for long envelopes.

### An earlier attempt to measure per-partial, and why it gave nothing

With `Part+` found, targeted edits on partial 1 (pages 0, 1, 2 - timbre bytes 14, 22, 34)
shifted the byte but **did not shift a single register**. At the same time the slot
lists became irregular: `0 2 3` instead of four in a row.

The most likely explanation is that partial 1 simply isn't active in the current timbre
structure, in which case its parameters aren't supposed to reach the chip anyway. But
this can't be distinguished from a measurement error on this data, so there's no
conclusion here.

**The cause is accumulated state.** The timbre lives in firmware memory across runs, and
there were many runs with `Number+` presses: structures ran up to their limits, partials
were switched on and off, and by this point the timbre is in a state nobody chose. **The
next run must start from a factory reset** - otherwise the starting point is unknown, and
without it "the register didn't shift" means nothing.

### What does NOT lead anywhere, although it looked plausible

`Group+` hits a wall past the second page: on pages 3, 4, and 5 nothing moves in timbre
RAM **at all**, and consequently no register does either. So the Common group has
exactly three parameters - the name and the two structures - and this matches pages 0-2
turning out to be exactly those.

`Bank+` doesn't lead to the partials either: with it, timbre byte +1 shifted, i.e. **the
second letter of the name**, while the registers didn't move at all. Within the timbre
edit it moves the cursor along the name - exactly as already measured for the Patch Edit
page (`reverb_path_probe`). I carried over someone else's assumption that "Bank+ selects
the group" instead of measuring it, and that cost a run.

**How to keep searching - don't guess what the buttons do, measure**: press the value
key on every reachable page and see which timbre byte shifted. The byte names the
parameter itself, and the menu layout isn't needed at all. This is how pages were
already found in `tone_edit_survives_probe` and `reverb_reg_probe`.

## 2026-08-02: CORRECTION of an earlier conclusion, and the actual register map

### What the mistake cost: "seven partial parameters" - there aren't seven

The earlier statement "all seven reachable offsets were tried (+0, +8, +20, +23, +28,
+41, +47), and only +0 moves a register" was **misinterpreted**. These aren't seven
partial parameters but the FIRST parameters of seven GROUPS. This is obvious at once if
you check them against the timbre layout:

| offset | what's actually there | group |
| --- | --- | --- |
| +0 | WG Pitch Coarse | **WG** |
| +8 | P-ENV Depth | **P-ENV** |
| +20 | P-LFO Rate | **P-LFO** |
| +23 | TVF Cutoff | **TVF** |
| +28 | TVF ENV Depth | **TVF-ENV** |
| +41 | TVA Level | **TVA** |
| +47 | TVA ENV Time KF | **TVA-ENV** |

And exactly these seven names sit as strings in the firmware ROM (`P-ENV`, `P-LFO`,
`TVF-ENV`, `TVA-ENV`). That is, **`Group+` selects the group, and `Bank+` selects the
parameter within it**, and only seven of the partial's thirty parameters had been
measured. Waveform, PCM wave number, pulse width, and resonance - the ones that MUST
reach the chip, because it's the chip itself that generates the waveform - were never
checked. The conclusion that "only pitch goes to the chip, and the firmware computes
everything else itself" didn't hold up on this data.

Before measuring, `Bank+` had been checked: earlier it moved the cursor along the
timbre NAME, and I carried that observation over from the name page to all the others.
On a parameter page it does something different, and that needed to be measured, not
inherited.

### The grid captured in full (`d110_la32_regmap grid`)

No notes are played, no slots are spent, so the whole grid is captured in one run. The
result **matched the MT-32 timbre layout (munt, `Structures.h`) byte for byte**: group 0
gives offsets +0..+7, group 1 +8..+16, group 2 +20..+22, group 3 +23..+27, group 4
+28..+36, group 5 +41..+46, group 6 +47..+55. This is an independent confirmation, taken
from live firmware rather than borrowed from someone else's header, that the D-110
timbre is the MT-32 timbre.

### Registers captured by targeted edits

Each parameter is edited on its own, from a factory reset, and two identical notes are
compared - before the edit and after. Timbre 1/24, structures 2 and 0.

| timbre parameter | register | law | confirmed by |
| --- | --- | --- | --- |
| **TVF Cutoff** (+23) | `0C40 .x.1` | **one-to-one** | a step of +5 gave +5, a step of +7 gave +7 |
| **WG Pulse Width** (+6) | `0C40 .x.0` | **value × 2.55** | 69→`B0`, 89→`E3`; 69×2.55=176, 89×2.55=227 |
| **TVF Resonance** (+24) | `0D00 .x.1`, low 5 bits | **value + 1** | 24→25, 29→30, 30→31 (ceiling) |
| **WG Waveform** SQU→SAW (+4) | `0D00 .x.0`, bit 6 | flag | `12`→`52` |
| **WG PCM Bank / PCM Wave** (+4, +5) | `0C40 .x.1`, `0D00 .x.1` high bits | ROM wave selection | PCM partial only |
| TVA velocity, Resonance | `0C80 .x.1` | amplitude | resonance moves it too |
| note number, WG Pitch Coarse | `0CC0 .x.{1,0}` | 16 bit, 342.57 per semitone | captured earlier |

**Two built-in controls passed, and without them the table would be worth nothing.** On
the SYNTHETIC partial, editing the ROM wave number and the PCM bank didn't shift a
SINGLE register - as it should be, it has no ROM wave. On the PCM partial the opposite:
SQU→SAW shifted nothing, while the wave number shifted three registers at once.

**Which partial is synthetic and which is PCM is decided by the structure, and this has
to be known in advance.** For a timbre with structures 2 and 0, the first partial is
PCM, the second synthetic (`PartialStruct` from munt: structure 2 gives mask `0b10`).
The first attempt to measure waveform landed on the PCM partial and gave "not a single
register" - a correct result, but meaning something other than what one might think.

### Why this is the path to emulation

What's been found maps onto the `LA32WaveGenerator` interface from munt **without a
single gap**:

```
initSynth(sawtoothWaveform, pulseWidth, resonance)   <- 0D00.x.0 bit 6, 0C40.x.0, 0D00.x.1
initPCM(pcmWaveAddress, pcmWaveLength, looped, ...)  <- 0C40.x.1, 0D00.x.1 (wave selection)
generateNextSample(amp, pitch, cutoff)               <- 0C80.x.1, 0CC0.x.*, 0C40.x.1
```

In other words munt already contains a model of the chip itself; what it lacks is
feeding it state from the REAL firmware. The map above is exactly that missing link,
which is why it was worth capturing to completion.

**What remains to work out before this can be turned into code:**

1. **What the `0x0CC0` stream carries over time.** Two bytes per slot, but they're
   rewritten continuously while the note sounds (from ROM `0x2C0D`). The first
   meaningful value is pitch. But the TVA and TVF envelopes must also reach the chip
   somehow, and `generateNextSample` needs three values per sample. Either the stream is
   multiplexed, or amplitude and cutoff have their own streams that the capture hasn't
   separated out yet. **This is the next experiment, and it decides the shape of the
   whole implementation.**
2. **What `0C00 .x.1` is.** Tracks the key exactly one unit per semitone, but also moves
   with velocity. Doesn't look like cutoff - cutoff was found in `0C40 .x.1`,
   one-to-one.
3. **The leading `FF`** - a reset before loading, or addressing.

## 2026-08-02, continued: the stream is PITCH, and the chip drives envelopes ITSELF

The probe `plugin/la32_stream_probe.cpp` (`d110_la32_stream`) prints the stream as a time
series instead of comparing sets of values - it was exactly this kind of comparison that
sank the previous three passes. One note, held 1.5 s, then 1.2 s after release, capture
over the whole `0x0C00`-`0x0DFF` window, nothing lost. The control - a window of the same
length with no note - gave **zero writes**.

### What streams, and what is set once

| bank | writes per note | who writes |
| --- | --- | --- |
| `0C00` | **8** | ROM 3B1E, 39DB, 30B5, 3080 |
| `0C40` | **3** | ROM 393E, 38B0, 3704 |
| `0C80` | **6** | ROM 3B25, 394B, 308C, 3087 |
| `0CC0` | **16468** | ROM **2C0D** x16460 |
| `0D00` | **4** | ROM 379D, 3781, 36FA, 36C7 |

Exactly one bank streams. But - and this turned out to be unexpected - **it keeps
rewriting the same value**: for slot 0, across 4117 writes there are exactly two
distinct successive values (a leading `FF`, then a value). The stream doesn't carry an
envelope; it **updates pitch**, and the value only changes where pitch is actually
supposed to move: the first 40 ms (the pitch envelope's attack) and at the moment the
note is released.

### Subroutine 0x2C08 read directly

```
2BEA  add 70, ef40[40]      this slot's base pitch (RAM 0x2F40 + slot*2)
2BEF  addc 72, 0
2BF2  jbs 73, 7, 2c06       went negative - clamp to zero
2BFA  cmp 70, #e800         otherwise clamp above to 0xE800
2C08  st  70, 0cc0[40]      SIXTEEN-BIT WRITE
```

`st`, not `stb`: the slot's two bytes are **one 16-bit number**, which earlier was
inferred from carries between the bytes, and is now visible in the instruction itself.
This also establishes the ceiling: **pitch is capped above at `0xE800`**.

### Envelopes do NOT reach the registers - and here's why

Edits to the TVA envelope - level `envLevel[0]` from 100 to 40, and attack time from 12
to 72, both confirmed by a shift in the timbre bytes - **did not shift anything in the
registers at all**. Bank `0x0C80` kept being written twice per note (`7E` at note-on,
`00` at note-off), exactly as before.

The cause was found by the same run: **the count of chip responses over the whole note
is ZERO.**

From this a model emerges, and it explains both this and the old story of the panel
freezing:

- **Amplitude and cutoff are HARDWARE RAMPS inside the chip.** The firmware puts two
  bytes into the bank - target and increment - the chip moves toward the target on its
  own and, on reaching it, raises the `INT` pin. On this interrupt the firmware loads
  the next envelope stage.
- That's why there are exactly two writes to `0x0C80` per note: the first stage at
  note-on and the first release stage at note-off. **The remaining stages aren't missing
  because they don't exist, but because there's nobody to say "the ramp arrived".**
- For the same reason, editing envelope times and levels moves nothing: those stages are
  never reached.

**This is exactly what munt models**: the `LA32Ramp` class with methods
`startRamp(Bit8u target, Bit8u increment)` and `checkInterrupt()`. The match isn't
approximate - two bytes per slot and an interrupt on completion.

**And this overturns the earlier conclusion**, recorded above as "what goes to the chip
isn't a description of the sound but its current state, and the firmware computes the
envelopes itself." For PITCH this is true - the firmware really does compute it and pour
it out as a stream. For amplitude and cutoff, the opposite: the chip drives them, and the
firmware only supplies the stages.

**And finally, this explains the `INT` pin.** The status byte at `0x0C00`, which
`la32_interface.md` analyzed as "which voice was freed", is, it seems, actually **which
ramp reached its target**: the handler reads it, and bits 0-4 name the voice. The
`La32Stub` stub answered on the same register, but with a different meaning.

### What follows from this for the implementation

The full picture of what needs to be written is now known:

| into the chip | from where | what munt does with it |
| --- | --- | --- |
| waveform, pulse width, resonance, ROM wave | `0C40`, `0D00`, once | `initSynth` / `initPCM` |
| pitch, 16 bit, streamed | `0CC0`, from ROM 2C0D | `pitch` in `generateNextSample` |
| amplitude: target and increment | `0C80`, by stages | `LA32Ramp` -> `amp` |
| cutoff: target and increment | `0C00`, by stages | `LA32Ramp` -> `cutoff` |
| "ramp arrived" back to the processor | `INT` + status byte at `0C00` | `LA32Ramp::checkInterrupt` |

**Caveats that travel with this.** That bank `0x0C00` is the CUTOFF ramp is not yet
proven: in its favor are the two bytes, a write at note-on and at note-off, and the fact
that the high byte tracks the key exactly per semitone and depends on velocity - but it
didn't move under a direct cutoff edit (`0C40 .x.1` moved instead, one-to-one). Perhaps
`0C40` carries the base and `0C00` the ramp's target; perhaps not. This needs checking
once the ramps are working and stages are flowing.

## 2026-08-02, further still: ramps are written and working, the handler read in full

### The ramp engine: `StuckPolicy::La32Ramps`

Written after the model of `LA32Ramp` from munt (a model of the same chip, reconstructed
there by analyzing recordings from a live unit): the high bit of the increment is
direction, the low seven bits are exponential speed, a zero increment neither moves nor
interrupts, a target already passed gives an instant set and interrupt. It's computed
analytically rather than sample by sample: the increment is constant, so it can be
serviced at MIDI clock rate without losing precision on the arrival moment.

**Byte order within the slot was taken from measurement, not chosen**: the odd byte is
the TARGET (at note-on, `7E` at velocity 100 and `6E` at velocity 40, i.e. the level),
the even byte is the INCREMENT (at note-off, `CF`, high bit meaning "down"). The cutoff
bank `0x0C00` behaves exactly the same way, and this is itself an argument that it's a
ramp too.

**The mechanism worked right away**: 5 starts, 5 arrivals, 5 responses to the processor
per note - against ZERO responses before it. And writes appeared in the banks from
subroutines that had never shown up there before (`0x32AF`, `0x32B4`, `0x3180`,
`0x3185`, `0x32DA`) - meaning the firmware genuinely started responding to a ramp's
arrival by loading the next stage.

### The handler, read instead of guessed

Trying four encodings of the status byte by brute force gave nothing, and brute force
was dropped in favor of disassembly. ROM `0x3138`:

```
3139  jbs port2, 2, 313e     is the INT pin still raised? if not, exit
313E  ldbze 64, 0c00         READ THE STATUS BYTE
3147  jbs 64, 7, 3190        bit 7 SET - go to 3190
314A  shlb 64, #01           bit 7 clear: value * 2
314D  andb 64, #3e
3150  subb 64, #02           minus two
3156  ldb 80, edc0[64]       slot state table, indexed
315B  cmpb 80, #80           is the slot free?
3160  ldb 81, #ff            yes - write 0xFF80 into pitch, i.e. MUTE the slot
318D  ljmp 32dc              no - the long path, voice servicing
3190  jbc 64, 5, 31a1        bit 7 set: branch on bit 5 to 31a1 or 32c2
```

From this, directly: **the number in the status byte is `slot + 1`**, because
`(v*2 - 2) & 0x3E` gives `slot*2`. No guessing needed.

**And this corrects yet another old entry.** `la32_interface.md` stated "bit 7 set =
nothing to service." Nothing of the sort: with bit 7 set the handler doesn't exit but
jumps to `0x3190` and there branches on bit 5 into two DIFFERENT live paths. So the
`0xFF` response, which here was taken to depict "nothing to service," actually sends the
firmware down one of them.

### Where the mechanism gets stuck right now

With the correct encoding (`slot + 1`), the note goes silent immediately: in the trace,
the second register write arrives at millisecond zero, not at note-off. The reason reads
out of the same handler - it checks `edc0[slot*2]`, and if the slot is still listed as
free, it **mutes it** by writing `0xFF80` into the pitch register. We're reporting the
ramp's arrival EARLIER than the firmware manages to mark the slot occupied.

From this, two candidates, and they must be told apart by measurement, not by choice:

1. **The leading `0xFF` isn't an increment but "set and don't interrupt".** This fits
   the long-standing observation that `FF` in these cells looks like a reset before
   loading. In that case, on note-on the firmware simply sets the level without an
   interrupt, and a ramp with an interrupt only happens where the envelope is actually
   moving.
2. **We're reporting too early** - on a real chip there's a delay between arrival and
   the interrupt (in munt this is `INTERRUPT_TIME`, seven samples), and the "write
   registers - issue voice" sequence has time to complete on real hardware.

The first candidate was tested with the `D110Core::setLa32PresetFf` switch (a
hypothesis, hence a switch rather than a decision baked into the code):

| | responses | arrivals | amplitude bank trace, slot 1 |
| --- | --- | --- | --- |
| `0xFF` as speed | 3 | 3 | `0:FF` `0:00` - the note goes silent at millisecond zero |
| `0xFF` as a set | 2 | 2 | `0:FF` `1520:CF` **`1520:00`** - survived to note-off |

**Half of it was confirmed**: the note survives to note-off, and a THIRD write appears,
one that had never been there before - the firmware loaded the next stage after the
release ramp arrived. The "arrival -> next stage" mechanism works.

**Half of it wasn't confirmed**: editing the TVA envelope (level 100→40, attack time
12→72) still changes nothing in the registers. So the attack doesn't ramp: the firmware
sets the level immediately and holds it. If `0xFF` is a set without an interrupt, then
something else must be the write that kicks off the attack, and it isn't in the capture.
So either `0xFF` really is a speed after all and the second candidate is at play (we
report before the firmware marks the slot occupied), or the attack stage doesn't get
loaded for some third reason.

### The order of events measured, and it rules out the second candidate

Both intercepts are put on a single timeline (`d110_la32_stream order`): the intercept
of firmware tables now also writes into the time-ordered capture, and the address filter
has been widened to let both sides through. Both intercepts live on the same processor
thread, so the order in the combined log is the real order of events, with no sorting.

```
0.02 | 364E | EDC0 = 20 | SLOT MARKED OCCUPIED
0.02 | 36C7 | 0D00      | setup (waveform, resonance)
0.03 | 3704 | 0C41      | setup (pulse width, cutoff)
0.06 | 394B | 0C80      | AMPLITUDE RAMP
0.06 | 3B1E | 0C00      | CUTOFF RAMP
```

**The firmware marks the slot occupied EARLIER than it writes the ramp registers.** So
reporting arrival right after the write is legitimate: the handler's `edc0 != 0x80`
check already passes by that point. **The second candidate is ruled out by
measurement**, not discarded for convenience.

From this the first candidate remains the only one: since with `0xFF`-as-speed the
firmware receives an interrupt it isn't expecting and mutes the voice, - **`0xFF` is a
set without an interrupt**. This is a conclusion by elimination, not fitting the desired
outcome, and `setLa32PresetFf` now describes the hardware rather than working around an
inconvenience.

### The ramp bank is chosen by the VOICE FLAG, not by function - and this fixed the rest

Ramp writes for the two slots of one note came from DIFFERENT subroutines and went to
DIFFERENT banks. Disassembly names the reason directly (ROM `0x3B0E`, and `0x393E` the
same way):

```
3B0E  ldb 78, #ff
3B11  ldb 70, ef80[54]     voice flag
3B16  jbc 70, 7, 3b20      BIT 7 DECIDES WHICH BANK TO WRITE
3B19  st  78, 0c00[54]     set    -> bank 0x0C00
3B20  st  78, 0c80[54]     clear  -> bank 0x0C80
```

`ef80[slot]` is the same byte that goes into register `0x0D00` (the order log shows the
pair: `EF80=D2` then `0D00=D2`). For a PCM partial it's `D2` (bit 7 set), for a synthetic
one `12` (clear). **So `0x0C00` and `0x0C80` aren't "cutoff ramp" and "amplitude ramp"
but one and the same ramp register for two KINDS of partial**; the voice uses one bank,
and the firmware puts its own thing into the other.

As long as the engine drove ramps in both banks at once, every voice got an extra ramp,
its arrival produced an interrupt the firmware wasn't expecting, and the chain of stages
fell apart. With bank selection by flag, everything fell into place.

### The envelope DID REACH the chip - measured, with controls in both directions

Editing the TVA release time (partial 2, `TVA-ENV`, parameter `envTime[4]`), three
points:

| release time | increment at note-off | when the next stage arrived |
| --- | --- | --- |
| 49 (factory) | `CF` | after 141 ms |
| 89 (slower) | `A7` | didn't make it within the capture window |
| 9 (faster) | `F7` | immediately |

The increment follows the parameter, ramp duration follows the increment. **Before the
ramps, this edit didn't move anything in the registers at all.** Controls run in both
directions from the starting point, so "coincidence" doesn't fly here.

### Why the attack doesn't ramp, and this too turned out to be correct

Editing `envLevel[0]` from 100 to 40 doesn't move the registers, while editing the **TVA
partial level** (+41) from 85 to 45 moves the note-on target from `7E` to `5B`. So at
note-on the firmware sets the PARTIAL LEVEL by an interrupt-free set, rather than
playing back the envelope's first stage. In the factory timbre the envelope starts at
its maximum, so there simply is no attack stage - there's nothing to ramp. The mapping
between level and target isn't linear (85→126, 45→91), which is expected for amplitude
in LA32's logarithmic domain.

### Summary: the ramp interface works end to end

The firmware loads a stage -> the chip drives it on its own -> on arrival it raises
`INT` and names the voice in the status byte -> the firmware loads the next one. Per
note: 4 starts, 2 arrivals, 2 responses, against ZERO responses before all this.

## 2026-08-02, bridge to sound: captured registers run through a model of the chip

`plugin/la32_render.cpp` (`d110_la32_render`) takes what the firmware put into the
registers at voice note-on and feeds it into `LA32FloatWaveGenerator` from munt - a
model of the SAME chip. The library is built statically, so its internal headers are
accessible; the wave generator has no public interface and was never meant to.

The breakdown of a single note looks like this:

```
slot | kind    | saw | width | cutoff | resonance | level | pitch
   0 | PCM     | yes |     0 |    186 |        24 |   241 | 4E3E
   1 | synth   | no  |   176 |    118 |        25 |   126 | 810D
```

### A check not by ear: the fundamental is measured

| note | pitch register | fundamental |
| --- | --- | --- |
| 60 | `0x810D` | 130.82 Hz |
| 67 | `0x8A6B` | 196.30 Hz |
| 72 | `0x911E` | 262.40 Hz |

**Note 60 sounds exactly an octave below C4 at A440, with a discrepancy of 0.1 cent.**
The octave is most likely the partial's own tuning (coarse pitch), not a property of the
interface: the timbre used was the factory one, and nobody normalized it.

**The intervals match the chip's law to within hundredths of a cent:**

| interval | register difference | predicted by the 4096-per-octave law | measured | discrepancy |
| --- | --- | --- | --- | --- |
| 7 semitones | +2398 | 702.54 cents | 702.57 | **+0.03** |
| 12 semitones | +4113 | 1204.98 cents | 1205.02 | **+0.04** |

The register analysis and the chip model agree completely. This is the first time
something captured from real D-110 firmware has been run through a model of its
synthesis chip and given a predictable result.

### And this resolves the question of 4111 versus 4096

The earlier note that "4111 is close to 4096 but not equal to it, and what this means is
unknown" is now clarified. The chip's own law is **4096 per octave**
(`sampleStep = 2^(pitch/4096 + 4)`, `LA32FloatWaveGenerator::getSampleStep`), and the
render confirms this to within hundredths of a cent. But **the firmware steps by 342.6
units per semitone** - both across seven semitones and across twelve, two independent
measurements. At a scale of 4096 per octave the step would be 341.33.

So **the D-110's octave is stretched by roughly 5 cents**. Three explanations, and
there's nothing yet to choose between them: the real chip's exponential table might
differ slightly from a pure power of two; munt's constant might be slightly imprecise;
or the unit genuinely is stretched. Recorded as a measured fact with an open
interpretation, not as a verdict on anyone's model.

### Volume: the register carries a LEVEL, and the generator expects its complement

In munt (`Partial.cpp`) there's `ampRampVal = 67117056 - ampRamp.nextValue()`, and then
`amp = 2^(-ampVal / 2^22)`. That is, **a bigger number means quieter**, and the register
does carry the level. Feeding it in directly would turn volume inside out: a full level
of 255 would give silence. One unit of level works out to `2^(1/16)`, i.e. **0.376 dB**.

Checked by SLOPE, not by a single peak - the absolute peak also depends on waveform:

| velocity | level in register | peak | computed ceiling | peak below ceiling |
| --- | --- | --- | --- | --- |
| 100 | 126 | 0.0015 | 0.0036 | **7.5 dB** |
| 40 | 110 | 0.0008 | 0.0018 | **7.5 dB** |

A level difference of 16 units changes both the peak and the ceiling by exactly 6 dB,
and the gap between them stays the same to within a tenth of a dB. The constant 7.5 dB
is the crest factor of a filtered square wave, not a parsing error.

### The firmware's envelope drives the sound

The render is now fed ramp stages as they arrive, using the same law as in `D110Core`.
The note is held for a second and released. The firmware issued three stages:

```
   0 ms: target 126, increment FF   set at note-on
1000 ms: target 0,   increment CF   release ramp at note-off
1121 ms: target 0,   increment 00   NEXT STAGE, loaded on ramp arrival
```

The envelope of the sound itself (peak per 25 ms, dB from maximum):

```
0..1000 ms:  0    holding
1100 ms:   -35    decaying
1200 ms:   -42    and flat from here
```

The decay ends at exactly the point where the ramp reaches its target - 1121 ms. The
residual -42 dB isn't silence but the chip's floor: at level 0 the generator gives
`2^-16`, which relative to this note's peak is exactly -42 dB. The voice is muted not by
a zero level but by a separate firmware command.

**The third stage is the working chain itself**: the ramp arrived, the chip raised the
interrupt, the firmware loaded the next stage. Visible not in a counter but in the
sound.

## 2026-08-02, PCM: wave address and length read straight from the registers, no table needed

Editing the `pcmWave` parameter (timbre +19) with two different step sizes gave an exact
byte-for-byte match with the wave table from the preset ROM
(`r15179873-lh5310-97.ic12.bin`, offset `0x0900`, the same address as
`ControlROMMaps["ctrl_d110_1_10_2"].pcmTable` in munt):

| pcmWave | register `0C40.x.1` | `pos` in the wave table | register `0D00.x.1` | `len` in the table |
| --- | --- | --- | --- | --- |
| 61 | `BA` | `BA` | `18` | `10` (plus resonance `08` in the low bits) |
| 64 | `C0` | `C0` | - (untouched) | `10` |
| 81 | `E3` | `E3` | `88` | `80` (plus the same `08`) |

The match is exact, digit for digit, not approximate. From this:

- **`0x0C40 .partial.1` (odd byte), for a PCM partial, is the `pos` byte from the wave
  table DIRECTLY, i.e. the wave ROM address divided by `0x800`.** Not an index for the
  chip - the address itself, already computed by the firmware. This agrees with the
  service notes: the LA32 has its own address bus into the wave ROM (`RA0-19`, 20
  lines), and the table is only needed by the firmware - the chip knows nothing about
  any tables.
- **`0x0D00 .partial.1` (odd byte), for a PCM partial, carries the `len` byte in its
  high bits** (bit 7 is loop, bits 6-4 are the length exponent), and the same resonance
  as the synthetic partial in its low bits. One register serves both purposes not
  because they got mixed up, but because the resonant filter in the LA32 is shared
  between PCM and synthesis.

**So the wave table isn't needed for sound at all** - only raw samples from the wave ROM
and the two registers already analyzed. Exactly as the chip itself sees it.

`plugin/la32_render.cpp` now renders the PCM partial too: it decodes the same wave ROM
the plugin itself loads (`waveIc8`+`waveIc7`, the same assembly order and the same
interleaving as `Synth::loadPCMROM` in munt), takes address and length from the
registers, and feeds them into `LA32FloatWaveGenerator::initPCM`. Note 60: address
`0x05D000`, length 4096, no loop, peak 0.2081 - sound, neither silence nor an
out-of-bounds read from the ROM.

## 2026-08-02, pair structure: bit 5 of the voice flag, matching munt at 22 points

Timbre structure decides two things at once: which partial is synthetic and which is
PCM (already found - bit 7), and whether the partials add or get multiplied by a ring
modulator. No need to search for the second one at random: `d110_la32_stream struct`
sweeps ALL reachable structures, plays a note on each, and prints the `0x0D00` flag byte
for both slots next to what munt's tables (`PartialStruct` and `PartialMixStruct` from
`Part.cpp`) say about that structure.

| structure | per munt | leading flag | trailing flag | bit 5 |
| --- | --- | --- | --- | --- |
| 2 | PCM + synth, mix 0 | `D2` | `12` | 0 0 |
| 3 | PCM + synth, mix 1 | `D2` | `32` | 0 **1** |
| 4 | synth + PCM, mix 1 | `12` | `B2` | 0 **1** |
| 5 | PCM + PCM, mix 0 | `D2` | `D2` | 0 0 |
| 6 | PCM + PCM, mix 1 | `D2` | `B2` | 0 **1** |
| 7 | synth + synth, mix 3 | `10` | `14` | 0 0 |
| 8 | PCM + PCM, mix 3 | `D0` | `D4` | 0 0 |
| 9 | synth + synth, mix 2 | `32` | `32` | **1 1** |
| 10 | PCM + synth, mix 2 | `F2` | `32` | **1 1** |
| 11 | synth + PCM, mix 2 | `32` | `B2` | **1 1** |
| 12 | PCM + PCM, mix 2 | `F2` | `B2` | **1 1** |

**Bit 5 exactly reproduces `Partial::isRingModulatingNoMix()` from munt** -
`(position == 1 && mix == 1) || mix == 2` - across all eleven structures and both slots,
i.e. at 22 points without a single exception. Zero at mix 0 and mix 3, only for the
trailing partial at mix 1, for both at mix 2.

**Bit 7 is confirmed along the way**: across all eleven structures it's set exactly
where munt expects a PCM partial, and clear where it expects a synthetic one.

**Bit 6 for a PCM partial goes off exactly for the trailing partial under ring
modulation** (`B2` versus `D2`, `F2`). This is exactly munt's `pcmWaveInterpolated`
condition, which states outright that for such a partial the interpolation multiplier is
busy with the ring modulator. For a synthetic partial the same bit carries the sawtooth
flag (measured earlier via the SQU→SAW edit). There's nothing in the current data to
separate these two meanings - in the factory timbre all synthetic partials are square
waves - and neither interpretation has been ruled out.

### The voice is assembled from a pair

`la32_render` no longer renders partials separately: they go into
`LA32FloatPartialPair`, whose mode is taken from the measured bit 5. On the factory
structure 2 (PCM + synth, addition) the pair's peak is 0.0517; on structure 9 (synth +
synth, ring modulation) it's 0.00000258.

**The second number nearly got read as "not working".** At four decimal places it
printed as zero, and that isn't silence: ring modulation MULTIPLIES two signals, and the
product of two quiet partials is bound to be tiny. Printing is now to eight decimal
places.

**A caveat that travels with the whole section**: we don't have a recording from a real
D-110, so "correct" here means "agrees with the munt model and is internally
consistent," not "matches the hardware." The pitch scale matching to hundredths of a
cent, and the structure bits matching at 22 points, are strong arguments, but not a
comparison against a real unit.

### What the bridge still can't do

- **Cutoff is held constant**: the cutoff ramp (if it exists - see the caveat above) is
  not fed into the render.
- **The envelope is only fed to the synthetic partial**, in a separate run; the pair is
  rendered at a constant level.
- This is a test rig, not the plugin's sound: the plugin still plays through its own
  mt32emu synthesizer.

## Where to go next

The first two items of the earlier list are done - the slot is taken from the firmware,
the fourth stimulus has been added. What remains, plus what's been added:

1. **Pitch still isn't visible on its own.** No register moves with the note while
   staying put across a velocity change. Most likely it's inside the `0x0CC0` stream,
   which moves in response to everything at once and needs to be analyzed as a stream,
   not as a value. Next experiment: record not a set of values but their SEQUENCE over
   time, and compare the shape of the curves, not sets.
2. **Break "timbre only" down into its components.** Changing timbre moves nine
   registers - too crude a stimulus. Targeted changes are needed: change ONLY the
   waveform, only the structure, only the TVA envelope, for a single timbre. The route
   to these pages is already known (Timbre → Edit → Edit, then Group+/Bank+, see
   `service_notes_findings.md`).
3. **Check byte pairs as a single number.** The bytes within a slot might be halves of a
   sixteen-bit value; currently they're compared separately.
4. **Understand the leading `FF`** - is it a reset or addressing.
