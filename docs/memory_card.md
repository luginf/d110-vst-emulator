# The M-256D memory card: how the firmware recognizes it, and what had to be reconstructed to find out

The `roland_d10.cpp` driver in MAME hands the card 32 KB at addresses `0xC0000-0xC7FFF` of
the banked address space, tagged with the comment "Shall become a proper memcard device
someday" - meaning there is no slot, no card, and no representation of its absence there at
all. So the first thing to figure out was **what, for this machine, actually distinguishes
an inserted card from an empty slot** - and the answer turned out not to be what you'd expect.

## There is no dedicated "card present" line

Connector CN8 (main board schematic, service notes p. 7) carries 34 pins: power, `A0-A14`
(fifteen address lines - exactly 32 KB), `D0-D7`, `OE`, `CE`, `WE`, `MR`, ground, and two
special lines - **`VBB`** (pin 33) and **`CST`** (pin 34). Both go to gate array IC21
(µPD65005G-062), which is the card controller:

- `CST` -> array input **`SENS`** (pin 61), pulled up to power through R11 100K;
- `VBB` -> comparator 22a (divider R17 22K / R18 27K) -> array input **`BATT`** (pin 60).

`SENS` is the write-protect mechanism on the card itself, `BATT` is its battery status.
**There is no "card inserted" line among them.**

## The firmware detects the card by writing to it

The recognition routine, ROM `0x770A`:

```
770A  lcall 7d35             bank 0x30 - first 16 KB of the card into window 0x8000-0xBFFF
770D  ld   78, #7804         twelve-byte signature in ROM
7711  ld   76, #8000         start of the card
7715  ldb  75, #0c           twelve bytes
7718  ldb  70, [76]          byte from the card
771B  cmpb 70, [78]          compare against signature; mismatch -> 7746
7724  djnz 75, 7718
7727  scall 7785             X / ~X pairs at offsets 0x0C and 0x0E
772E  ldb  70, 800c          card type: 'D' - code 0, 'N' - code 'O'

7746  ldb  75, 70            signature mismatch:
7749  negb 75                  complement of the byte read
774B  stb  75, [76]            WRITE it to the card
774E  cmpb 70, [76]            read the same location back
7751  jne  7759                changed - the write succeeded, a card IS present (code 'C')
7753  cmpb 70, #ff             unchanged. Was the original byte 0xFF?
7756  jne  7759                no - still treat it as a card
7758  ret                      0xFF and not writable - NO CARD, code 0xFF
```

Message decoding at `0x76E2` turns these codes into text: `0xFF` becomes "Card Not Ready",
and `'C'`/`'D'`/`'I'` become "Illegal Card".

**To the firmware, an empty slot is a bus that reads as `0xFF` and does not accept writes.**
The signature in ROM at `0x7804` is `"Roland D-10 "`, followed by `'D'`/`0xBB` and `'X'`/`0xA7`
(value/complement pairs).

## A status port at the last address of the window

The firmware checks write protection exactly once per boot, at `0x7778`: it sets bank `0x31`
and reads `0xBFFF`, i.e. **offset `0x7FFF` from the start of the card**. A zero in bit 0
produces "Memory Card Write Protected". The battery check is at the same location: `0x584C`
reads that same byte and, when `bit1 = 1, bit2 = 0`, shows "Check Card's Battery".

This byte is **not card memory but the status port of the IC21 array**, and here's why:

- `SENS` and `BATT` are array inputs, and there is nowhere else for the processor to read them
  from: the card has no lines other than the bus;
- measured behavior confirms it - formatting fills the entire card, including writing `0x00`
  to that address. As long as that address was ordinary memory, **a freshly formatted card
  immediately came out write-protected**, and writing to it became impossible. Once it became
  a port, formatting and writing proceeded back-to-back with no error message at all.

## What was implemented in the plugin from this

None of this required patching MAME.

- Extraction: the card's contents are moved into the core's buffer, the shared memory `memcs`
  is filled with `0xFF`, **and a read intercept is installed on it**, returning `0xFF`
  regardless of what was written there. A single fill is not enough, and this was verified by
  measurement: filled memory still accepts writes, reads return what was written, and the
  firmware says "Illegal Card" - i.e. it sees a card.
- The last address of the window is served by the same intercept, acting as a status port:
  bit 0 is the position of the write-protect switch (`D110Core::setCardWriteProtect`), the
  remaining bits are all ones, as a pulled-up, unconnected line would return them.
- The card's contents live in their own file, `nvram/d110/memcs` (renamed from `mame_nvram/`
  2026-08-06, with a one-time migration - see `D110AudioProcessor::getNvramRoot()`), and are
  stored in the project state separately from the unit's battery-backed RAM.
- Clicking the slot on the panel moves the card: the contacts disconnect as soon as it starts
  moving out of the slot, and reconnect once it has seated fully.
- The slot position and the write-protect switch position are stored in the project state
  along with the card's contents themselves, so a project saved with the card removed reopens
  with it removed.

## What it looks like

An inserted card **is visible edge-on in the slot opening**: eighteen dots of its edge, which
at the panel's scale (4.4 dots per millimeter) is about four millimeters - the thickness of
the card body at the grip end. The opening is taller, thirty dots, and the darkness remaining
above the card reads as the depth of the slot the card emerges from. An empty slot is
immediately distinguishable from an occupied one, with no labels needed.

The shadow in the opening is drawn as a gradient ON TOP of the card, not baked into the
card's own image: the card travels through the opening, so it is the location that should
darken, not the card.

The travel takes about nine tenths of a second. Speed depends on how far the card is from
EITHER stop, so it starts gently, accelerates through the middle, and eases into the stop
gently. The previous motion law only accounted for the remaining distance: the card would
shoot off at full speed and be indistinguishable for the first third of its travel, then slow
down where there was nothing left to see.

This isn't something you can judge from the constants, so there is `plugin/panel_render.cpp`
(`d110_panel_render`): it captures the ACTUAL plugin panel to PNG - the very `D110Panel` the
user sees - as a filmstrip every 100 ms, separately for extraction and insertion. It moves the
card not via a variable but by clicking the slot, so the entire path from the mouse hit to the
rendered frame is exercised. The last frame is compared against the very first: the card, once
returned, must land exactly where it started from.

## How this was verified

`plugin/card_probe.cpp`, target `d110_card`. Four modes, each designed so that the result has
a check capable of showing a failure.

**`cards` - four cards, each differing in exactly one property:**

| input | firmware response |
| --- | --- |
| empty slot (reads `0xFF`, write does not go through) | `Card Not Ready` |
| card full of zeros | `Illegal Card` -> `Card Formatting OK?` |
| formatted | no error, `No Space` (empty, but recognized as its own) |
| same, with write-protect switch engaged | `Memory Card Write Protected` |

Four different responses to four different cards is the proof itself: a single "Card Not
Ready" on an empty slot alone would mean the same thing as a menu that simply doesn't work.

**`roundtrip` - the full owner's cycle**: blank card -> "Illegal Card" -> "Card Formatting
OK?" -> `Complete` (the signature on the card becomes `Roland D-10 `, type `44/BB 58/A7`) ->
`Save to Card` -> `Complete`, 22581 bytes of data on the card.

**`loadverify` - whether reading really RETURNS the memory.** "Complete" on the screen only
says that the operation ran to completion. So: write to the card -> factory reset -> read from
the card, and three snapshots of the battery-backed RAM are compared.

```
after reset vs. written:   total 8069, in patch memory 7040
after read vs. written:    total   19, in patch memory    0
after read vs. edit:       total 8082, in patch memory 7040
```

The nineteen differing bytes are firmware working areas (`0x36xx`, `0x39xx`), which change
between any two snapshots on their own. **Not a single byte differed in patch memory**:
reading from the card returned exactly what had been written, and that includes going through
a machine restart, which is what performs the factory reset.

The stimulus wasn't obvious at first: an edit made in Patch Edit doesn't work, because it
lands in a temporary copy - the snapshot showed zero changes in patch memory. A factory reset
rebuilds stored memory from the preset ROM, and that's visible as the 7040 bytes.

**`persist` - the card as a storage medium.** Write a tag to the card, power off, power on:
the signature and the tag are still there. Eject, power off, power on: the slot is still
empty, `Save to Card` responds with "Card Not Ready", and the card's contents remain intact
and come back with it.

## Menu layout, found by exhaustively pressing buttons

None of these buttons were guessed - all were found by the `explore` mode, which presses the
named buttons and prints the screen after each one.

- `WRITE/COPY` from the main screen opens `Save to Card`;
- `GROUP△` cycles through functions: `Save to Card` -> `Load from Card` -> `Dump One Way` ->
  `Dump Hand Shake`; `BANK▲` cycles through data types: `Sound` / `RhythmSetup`;
- `ENTER` asks `Sure?`, and **it's `WRITE/COPY` that executes**;
- formatting is confirmed the other way around - with `ENTER`;
- reading from the card writes into internal memory, so with `Mem Protect = ON` the firmware
  first prompts `MemProtected / Turn off once ?`.
