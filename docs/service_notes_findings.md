# What the Service Notes Say About the Sound Section

Source: `docs/manuals/D-110_Service_Notes.pdf`, Roland, June 1988, first edition, 16
A3 pages. This is a scan with no text layer, so the pages were parsed as images
(`pdftoppm -r 300` with cropping of the needed area; the text layer is empty - 129
bytes for the entire document).

All 16 pages were read. What matters for emulation is on the block diagram (p. 5), the
main board schematic (p. 7), the button board (p. 10), the IC DATA section (pp. 11-12),
the test mode (pp. 12-13), and the CHANGE INFORMATION and RECOVERING FACTORY DATA
sections (pp. 14-15). Pages 2-4 (assembly drawing, parts list, wiring diagram), 8-9
(analog board and power supply), and 16 (a typo in the lithium battery part number)
give nothing for emulation.

This is DOCUMENTATION, not measurement: it explains what was measured earlier and
suggests where to measure, but it doesn't remove the obligation to measure. The
reverb-type path below, for instance, was read from the schematic and then still
confirmed by an experiment with a control.

## What the sound section consists of

| Position | Chip | What it is |
| --- | --- | --- |
| IC9 | **MB87136APF**, labeled right in the pin table as "LA Chip … (LA32)" | synthesis |
| IC7, IC8 | HN62304BPD10, HN62304BPC99 | wave data ROM |
| IC5 | **HG61H20R36F**, "REVERB CUSTOM IC" | reverb |
| IC6 | HN623257PZ20 | reverb microprogram ROM, 32 KB |
| IC1-IC4 | MN4264-12 / MN4256-12 | reverb dynamic memory - its delay line |
| IC16 | HG61H15B-72F | gate array: address decoder, LCD, panel, EXIO |
| IC18 | 8097BH | processor |
| IC116 | PCM54 | DAC, 16-bit |
| IC113 | HD14051BP | eight-channel analog demultiplexer |

The signal path per the block diagram: LA32 -> reverb -> `SD0-SD15` -> DAC ->
demultiplexer -> eight sample-and-holds -> eight low-pass filters -> MIX OUT L/R and
MULTI OUT 1-6. In other words, the individual outputs are obtained by time-division of
a SINGLE DAC.

## LA32: the interface that has so far been reconstructed from the firmware

Pin table of the MB87136APF from p. 11 (transcribed in full, because this is exactly
the interface emulated in `docs/la32_interface.md`):

| Pins | Name | Dir. | Function |
| --- | --- | --- | --- |
| 1 | **INT** | out. | **interrupt output** |
| 2 | OE | in. | output enable |
| 5 | CS | in. | chip select |
| 6-14 | **A0-A8** | in. | processor address bus |
| 17-24 | **D0-D7** | bidir. | processor data bus |
| 25, 26, 29-33, 36 | RD0-7 | in. | wave ROM data bus |
| 34, 35, 37, 38, 43-50, 55-60 | RA0-19 | out. | wave ROM address bus |
| 61-64, 67-76, 79, 80 | O0-15 | out. | digital audio output |
| 81-84 | SH0-3 | out. | **not used** |
| 86, 88 | X1, X2 | — | 32.768 MHz crystal |
| 92, 93 | 16M, 32M | out. | half-rate clock and full clock |
| 94 | CKIN | in. | clock input |
| 96 | SYI | in. | sync input |
| 98, 100 | WR, RD | in. | write and read strobes |

What this confirms and clarifies:

- **The interrupt the firmware waits for at `0x29E9` is pin 1 of the LA32 itself.**
  Not "something on the sound board", but specifically the output of the synthesis
  chip. The whole `StuckPolicy::La32Stub` construction relied on this as a hypothesis;
  now it's written in the documentation.
- **A0-A8 are nine address lines, i.e. exactly 512 registers.** The window
  `0x0C00`-`0x0DFF`, found by the `la32_probe` probe from unmapped accesses, is exactly
  512 addresses. The size match is not a coincidence: this is the entire LA32 register
  file.
- The crystal is 32.768 MHz, not 16, and the chip itself outputs divided clocks
  externally.
- The wave ROM address bus has 20 lines = 1 MB of wave-data address space.
- The LA32's SH0-3 pins are not used - the `SH1-SH3` signals to the analog board don't
  come from here, but from the IC16 gate array (see below).

## Reverb: it DOES have a register interface, and it's not on the sound board bus

This changes the conclusion recorded in [[roland-boss-reverb-feasibility]] as "the type
must reach the chip via the sound board bus `0x0C00`-`0x0D02`". The main board
schematic (p. 7) shows a separate input from the processor at IC5:

- **`D1`-`D5`** (pins 98, 99, 100, 1, 2) go straight to the processor's DATA bus. Five
  bits, and `D0` is NOT connected.
- **`STB0`** (pin 11) and **`STB1`** (pin 12) - two strobes.
- `RES` (13) - power-on reset, `OE` (5), `SYNC` (9), `INSTB` (10), `SH0` (19), `SH1`
  (18), `SAW0` (97), `NL-C` (96), `NL-D` (95).
- `ROMA0-12` -> IC6, `ROMD0-7` <- IC6; `RAMA0-7`, `RAMD0-15`, `RAS`, `CAS`, `WE` ->
  IC1-IC4; `SD0-SD15` -> analog board; `X1` (16) - 8 MHz clock.

The strobes are formed by two NAND gates from the outputs of the IC16 gate array:

```
STB0 = NOT(EXIO1 · WL)          IC16 pin 40 = EXIO1, pin 65 = WL
STB1 = NOT(EXIO2 · WL)          IC16 pin 41 = EXIO2
```

`WL` is "write low", the strobe for writing the low byte. This means **the processor
writes five-bit words to the reverb at two addresses decoded by IC16**. The array also
has a third such output, `EXIO3` (pin 42).

The addresses are inside the array, and the schematic doesn't name them. But the D-110
memory map in MAME (`roland_d10.cpp`) only occupies `0x0100` (bank), `0x0200` (SO
latch), `0x021A`-`0x021D` (panel scan), `0x0300`/`0x0380` (LCD), while the
`so_trace_probe` probe saw unmapped accesses at exactly three addresses: `0x0280`
(already established to be an alias of the SO latch), **`0x0400` and `0x0800`**. Two
unexplained addresses for two strobes is a hypothesis that can be checked directly, and
it is checked by a probe, not by reasoning.

Once again, because this changes the plan: the LA32 itself has `A0-A8` and `CS` pins -
its own register file - while the reverb has a separate input `D1-D5` + `STB0/STB1`.
**These are two different interfaces**, and previously they had been lumped into one.

## IC16 gate array, pins

From p. 11, the relevant part: `SI0-SI7` (1-8), `AUXB2` (9), `AUXB3` (10), `CLK` (11),
`SC0`/`SC1` (13, 14), `AD0-AD15` (21-40, interleaved), **`EXIO1` (40), `EXIO2` (41),
`EXIO3` (42)**, `A0-A16` (43-61), `BANK0` (62), `BANK1` (63), **`WR H` (64), `WR L`
(65)**, `SO0-SO7` (66-71, 74), `LCD0-LCD3` (75-78), `LCDE` (79), `LCDRS` (80). The same
page gives the array's internal block diagram: address latch, **address decoder**,
eight-bit latches and buffers, serial buffer, LCD control, interrupt control, a
programmable divider and clock generator.

This also shows why the firmware never writes to processor ports 1 and 2: everything
that ports do on other synthesizers is done here by the gate array - the panel, the
LCD, and the SO latch alike.

## Panel matrix - confirmed by the button board schematic (p. 10)

The button board (ASSY 79454430) is wired exactly the way `INPUT_PORTS_START(d110)` in
MAME describes it, and that's worth recording because the layout was taken from there
without independent verification. Connector CN7 carries **ten** lines: two column
strobes `SC0`, `SC1` and eight scan lines `SI0`-`SI7`. One strobe serves the column
EXIT / PATCH / TIMBRE / PART▲ / GROUP△ / BANK▲ / NUMBER▲ / WRITE-COPY, the other -
EDIT / PART / SYSTEM / PART▼ / GROUP▽ / BANK▼ / NUMBER▼ / ENTER, and `SI7`..`SI0` run
top to bottom, i.e. EXIT and EDIT are the most significant bit, WRITE/COPY and ENTER
the least significant. This matches exactly the masks in
`docs/panel_reference_notes.md`.

Each button has a series diode (all 1SS-133, all buttons EVQ-QVT 05G) - so pressing
several buttons at once doesn't produce false triggers, and experiments like
"EDIT+ENTER together" are legitimate on this hardware, not just in the emulator.

## Factory test mode (pp. 12-13)

Entirely in the firmware, so it's available in the emulator too. Entry: hold the two
rightmost bottom buttons at power-on. Then, step by step: `Acou Piano Right analog out
test`, a check of all 16 buttons (`all switch ok` when all are pressed), a timbre
check (the eight bottom buttons play notes), a reverb check, MULTI OUT, DAC
adjustment, a MIDI self-test, and a memory card and internal RAM test.

**And right there - independent confirmation of how the MIDI MESSAGE LED works.** The
test setup (p. 12) requires **connecting MIDI OUT to MIDI IN with a cable**, and the
"reverb check" step says: press a key, listen to the volume with reverb, and *"check
that the MIDI MESSAGE LED is lit"*. The firmware doesn't write anything to the LED
itself here - it writes to MIDI OUT, and what lights the LED is what came back through
the loop into the input. This is exactly the scheme we derived by measurement and
implemented in `D110Core::midiLampOn()`, only stated by Roland itself.

Separately useful: **the firmware version is shown by the firmware itself** - three
buttons at power-on, and the screen shows `D-110 ver 1.06 / Apr. 5, 1988`. A
ready-made independent check of which ROM image is actually loaded.

## What the factory reset does, and what it does NOT do (p. 15)

The procedure is written down verbatim the same way `D110Core::factoryReset()`
replicates it: hold WRITE/COPY, power on, and press ENTER. But according to Roland's
text, it restores **only Timbre memory and Rhythm Setup**.

**The factory reset doesn't touch Tone memory or Patch memory.** Roland's suggested
way to restore those is from the official "D-110 FACTORY PRESET CARD" memory card via
Load from Card / All. We don't have the card, which explains why after a reset the
tones and patches stay however the previous session left them, rather than becoming
"factory".

## Firmware version history (p. 14), and one long-standing puzzle

The table starts at 1.01; our image is 1.10. From it, what's relevant:

- **After playing the demo, the firmware FORCIBLY sets `Master Tune = 442`,
  `Control ch = OFF`, `Exclu Unit# = 17`** (listed as a bug fixed in version 1.01).
  This resolves the question that stood open in `sysex_address_map.md`: the panel
  shows 442, while Roland's 0-127 table gives about 447 for byte `0x4A`. 442 is not a
  scale recalculation, but a constant the firmware writes itself.
- In the same place, about the same version: "Turning the power off in ROM play mode
  might result in that the former tone color remains keeping" and "The setting (Patch
  + Rhythm Setup) at power-off might not be kept holding" - i.e. ROM Play has
  documented side effects on the saved state.
- The preset chip IC15 was changed from `LH5310-97` to `LH5310-DJ` for "improvement on
  ROM play data", and the note requires: **if DJ is fitted, the firmware must be 1.07
  or newer**.

  It's easy to get confused here, because this concerns TWO DIFFERENT chips. The
  firmware is IC19 (EPROM `µPD27C256AD-20`), while the presets and demo data are a
  separate mask ROM, IC15 (labeled IC12 on the D-110 board). They were changed
  independently.

  **Our 1.10 firmware satisfies this requirement** - 1.10 is newer than 1.07, not
  older. The restriction is one-directional and doesn't concern our combination.
  What's earlier for us is specifically the preset ROM: in the set,
  `r15179873-lh5310-97.ic12.bin`, i.e. "-97". There's no choice here anyway - **a
  "-DJ" dump doesn't exist in MAME at all**, the `d110` romset only has "-97", and of
  the firmware versions offered, 1.06 and 1.10, 1.10 is the default
  (`ROM_DEFAULT_BIOS("110")`).

  The only practical consequence: the demo songs play using the earlier revision of
  the data, since it was precisely the demo data that was improved in the move to
  "-DJ".

## Where the reverb TYPE actually goes - found and measured

The schematic said where to look; the disassembler and the
`plugin/reverb_reg_probe.cpp` probe (target `d110_reverb_reg`) gave the answer.

The entire reverb setup is a single continuous ROM subroutine `0x4C7B`-`0x4CC5`, and
it distributes four parameters across three ports:

```
4C7B: ld   70, ed94        ; RAM 0x2D94 - Master Tune, recomputed for a different consumer
4C80: mulub 70, #ab        ;   ×171
4C83: sub  70, #2ac0
4C87: shra 70, #06         ;   >>6
4C8A: st   70, f4a0        ;   -> RAM 0x34A0
4C8F: scall 4d04           ; TIME: 0x0400 bits 0-2 <- RAM 0x2D96 & 7
4C91: scall 4cc6           ; LEVEL: 0x0800 bits 0-1 <- (RAM 0x2D97 & 6) >> 1
4C93: ldb  70, ed95        ; RAM 0x2D95 - TYPE
4C98: andb 75, 70, #0e     ;   type bits 1-3
4C9C: di
4C9D: andb c8, #f1         ;   clear these bits in the shadow byte
4CA0: orb  c8, 75          ;   insert
4CA3: ei
4CA4: stb  c8, 021a        ;   -> PORT 0x021A
4CA9: ...                  ; TYPE, bit 0 -> 0x0800 bit 2
4CC0: stb  75, 0800
4CC5: ret
```

There is also a second entry point, `0x4C93` (`lcall` from `0x2E54`): it skips time
and level and updates only the type - this is the path for editing from the panel.

**Final layout:**

| Parameter | RAM | Where it goes | Width |
| --- | --- | --- | --- |
| Reverb Type | `0x2D95` (0-8, where 8 = OFF) | **bits 1-3 in port `0x021A`**, **bit 0 in bit 2 of port `0x0800`** | 4 bits, split across two ports |
| Reverb Time | `0x2D96` (0-7) | port `0x0400`, bits 0-2 | 3 bits |
| Reverb Level | `0x2D97` (0-7) | port `0x0800`, bits 0-1 | **2 bits**: taken as `(level & 6) >> 1` |

Both ports also have one more one-bit field, set by the same technique from register
`r70` (`0x0800` bit 2 is the low bit of the type; `0x0400` bit 3 - subroutine
`0x4CE7`, what calls it hasn't been determined yet).

A note worth keeping in mind: **the level only reaches the chip as the two
most-significant bits out of three.** On screen it's 0-7, but what goes to the reverb
chip is 0-3.

### Measurement

A sweep of the eight types from the panel, intercepting writes to `0x021A` and to
`0x0400`-`0x0BFF`, with the values separated by the calling subroutine's address (the
panel scan from `0x5B35` writes to `0x021A` in parallel - one thousand to four
thousand writes per window; without separating them nothing can be made out, and the
first version of this probe lumped them into a single row). Each type change produces
**exactly one** write from `0x4CA9` and **exactly one** from `0x4CC5`:

| RAM `0x2D95` | type on screen | `0x021A` from `0x4CA9` | `0x0800` from `0x4CC5` |
| --- | --- | --- | --- |
| 1 | 2 | `00` | `06` |
| 2 | 3 | `02` | `02` |
| 3 | 4 | `02` | `06` |
| 4 | 5 | `04` | `02` |
| 5 | 6 | `04` | `06` |
| 6 | 7 | `06` | `02` |
| 7 | 8 | `06` | `06` |
| 8 | OFF | `08` | `02` |

In other words, `0x021A` carries exactly `type & 0x0E`, and bit 2 in `0x0800` is
exactly `type & 1`.

**Confirmation from an independent spot in the same run.** The panel scan (ROM
`0x5B35`) writes to `0x021A` in parallel, and its values in the same windows come in
pairs: `00 01` for type 1, `02 03` for type 3, `04 05`, `06 07`, `08 09` for OFF. It's
the same byte: **bit 0 is the panel column strobe, bits 1-3 are the reverb type**, one
shared shadow register `c8`, which is exactly what explains the mask `andb c8, #f1`
(keep bit 0 and bits 4-7, overwrite 1-3). Two completely different subroutines place
their own fields into the same byte, and both show the same dependency on type -
that's the kind of confirmation a single subroutine cannot provide.

**Control**: the same button presses and the same note, but without changing the type
- **not a single write** to `0x0800` from `0x4CC5` or to `0x021A` from `0x4CA9`. So
it's specifically the type change that triggers the writes, not the presses, the note,
or the time.

### And this is the SECOND real gap in the MAME driver - fixed, the patch is in the repository

`roland_d10.cpp` declares `map(0x021a, 0x021b).portr("SC0").nopw()` - the address is
claimed read-only, and **writes to it are silently dropped**. But the firmware writes
real data there: bits 1-3 of the reverb type. Exactly the same kind of issue as the
dropped writes at `0x0280`, and both are upstream candidates, like the MCS-96 timer
divider fix and the TMP68301 ARELR fix.

Both fixes have been made and are in [`../patches/`](../patches/) -
`mame_roland_d10_dropped_writes.patch`, with notes on what in them is measured versus
inferred. The plugin doesn't need them: it still builds against the unmodified tree
and sees the data through its own intercepts. Verified after applying it - the unit
boots, the eight-type sweep gives the same values down to the last row, and the demo
song plays for 75 seconds with a live panel at 100% real time.

### What this changes, and what it doesn't

Changes: the question of "where the type goes" is closed, and the earlier conclusion
"the type must travel over the sound board bus `0x0C00`-`0x0D02`" is **disproved** -
the sound board bus isn't connected to the reverb at all, those are LA32 registers.

Doesn't change: the D-110's reverb still doesn't produce sound because of this. Nobody
emulates the chip yet, and the eight types honestly don't map onto four engine modes -
see [[no-proxy-emulation-rule]]. But now it's known exactly WHAT would have to be fed
into a model's input, if one is ever built: 4 bits of type, 3 bits of time, 2 bits of
level, plus two bits selecting the microprogram bank and R.SW from the SO latch.

## What's NOT in the notes

A pin table for the HG61H20R36F reverb chip - only the package outline on p. 11 and
the connections on the schematic. The delay, coefficient, and mode values, even less
so: they're in the IC6 microprogram and, as already measured, are not present in the
ROM as plain numbers (see [[roland-boss-reverb-feasibility]]: all 21 published munt
lengths were searched for, zero were found).
