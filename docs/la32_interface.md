# The LA32 interface, as the D-110's firmware sees it

Everything here was measured against the running firmware, not taken from documentation
— there is none for this. The tools are in the repository: `plugin/hang_probe.cpp` samples
the program counter and diffs a healthy histogram against a stalled one,
`plugin/la32_probe.cpp` logs every access to an address the machine's map leaves unclaimed,
and `plugin/disasm_tool.cpp` reads the firmware back with MAME's own MCS-96 disassembler.

## Why this matters

MAME's `roland_d10` driver is `MACHINE_NOT_WORKING` / `NO_SOUND`, and this is what that
means in practice: **the control board works perfectly until a note is played.** Then the
firmware hands the note to a sound board that is not there, waits for it to answer, and
never returns to scanning the front panel. The display freezes and no button responds,
while the sound — which comes from munt, not from the firmware — carries on regardless.

It is not about MIDI. The instrument's own **demo song** freezes it identically, and that
uses no MIDI at all.

## Where the stall is

```
29E6  orb   int_mask, #80      enable EXTINT
29E9  ldbze 54, f440[52]       read a per-voice flag out of battery RAM
29EE  jbc   54, 7, 29e9        spin until bit 7 of it is set
29F1  andb  int_mask, #7f
29F4  ret
```

`0xF440` reaches battery RAM through `fixed_r`, landing at the `rams` share offset
`0x3440`. Nothing but an interrupt handler ever sets that bit.

Releasing it by writing the flag directly is not enough: the stall simply moves to the
voice-chain walk at `0x2673`–`0x269E`, which follows a linked list through `rams 0x33C0`
with flags at `rams 0x3460` and never reaches its end. The chain has to be maintained by
the handler, not faked from outside.

## The register window

The D-110 map claims almost nothing in the low I/O page — the bank register at `0x0100`,
the SO register at `0x0200`, the panel scan ports at `0x021A`/`0x021C`, and the LCD at
`0x0300`/`0x0380`. The sound board answers in the gap at **`0x0C00`–`0x0DFF`**, which on
the D-10 the driver gives to the key scanner and which the D-110 configuration removes
outright (`config.device_remove("keyscan")`).

Logging unmapped accesses while playing shows **160 distinct addresses, every one of them
written and none read** — the firmware pushes voice parameters out on a stride of 2 from
`0x0C00`, from routines at `0x307B`, `0x3080`, `0x30B5`, `0x3704`, `0x38B0`, `0x393E`,
`0x39DB`, `0x3B1E`. It polls nothing. The answer comes back as an interrupt.

## The interrupt, and the handshake

The MCS-96 vector table sits at `0x2000`:

| Vector | Interrupt | Handler |
| --- | --- | --- |
| 0 | Timer overflow | `0x22A3` |
| 1–4 | A/D, HSI, HSO, HSI.0 | unused (`FFFF`) |
| 5 | Software timer | `0x1A08` |
| 6 | Serial (MIDI in) | `0x1DAC` |
| **7** | **EXTINT** | **`0x3138`** |

`IRQ_EXTINT` is bit 7, which is exactly the bit the stalled routine enables, and `IOC1`
reads `0x75` at that moment — bit 1 clear, so the CPU does accept the line. The handler:

```
3138  pushf
3139  jbs   port2, 2, 313e     proceed only while the EXTINT pin is still high
313C  popf
313D  ret                      otherwise do nothing at all
313E  ldbze 64, 0c00           READ the status register
3143  ldb   int_mask, #40
3146  ei
3147  jbs   64, 7, 3190        bit 7 set -> nothing to service
314A  shlb  64, #01            otherwise bits 0..4 are a voice number
314D  andb  64, #3e            (index x 2, so 32 voices)
3150  subb  64, #02
3153  andb  64, #3e
3156  ldb   80, edc0[64]
315B  cmpb  80, #80
315E  jne   316a
3160  ldb   81, #ff
3163  st    80, 0cc0[64]       write back to the voice's registers
3168  popf / ret
316A  ld    80, #ffff
316E  st    80, 0cc0[64]
3173  ld    80, ef80[64]
317B  st    80, 0d00[64]
3185  ldb   80, #06
3188  stb   80, eec0[64]
318D  ljmp  32dc
```

So the contract is:

1. The board raises **EXTINT** and holds it high.
2. The handler checks the pin is still high (`port2` bit 2 — on this CPU the EXTINT pin
   *is* P2.2), then reads **`0x0C00`**.
3. **Bit 7 set** means "nothing to service"; the handler goes to `0x3190`, where bit 5
   selects again between two paths.
4. **Bit 7 clear** means bits 0–4 identify which of 32 voices has finished, and the handler
   services it, writing to `0x0CC0[i]` and `0x0D00[i]`.

An unmapped read returns `0xFF`. Bit 7 set — "nothing to service" — every single time,
which is precisely why the voice flag is never written and the wait never ends.

## What would have to be built

Not a synthesiser. munt already does the sound, and the firmware never reads a sample.
What is missing is only the **bookkeeping half** of the chip:

- A device (or a runtime `install_read_handler`, which needs no MAME patch) covering
  `0x0C00`–`0x0DFF`.
- A queue of voices that have finished. The firmware's own writes into `0x0C00+` say which
  voices were started, so completion can be scheduled from them.
- Read of `0x0C00`: return the next finished voice with bit 7 clear, or `0xFF` when the
  queue is empty.
- Raise EXTINT while the queue is non-empty and hold it high until the handler has read the
  status, because the handler checks the pin before doing anything.

Driving EXTINT on a free-running timer does **not** work and should not be tried again:
because the pin is also P2.2, holding or toggling it corrupts a port the firmware reads,
and the panel dies within seconds even with no notes playing at all. It has to be raised
only when there is genuinely something to report.

## What has been built, and what it proved

The handshake itself now runs end to end, and none of it needs MAME patched:

- The status register is supplied by a **runtime read tap** on `0x0C00`, which can be
  installed on a running machine and may replace the value a read returns.
- The voice the firmware is waiting on is read out of **CPU register 52**, through the
  `register_file` share — which must be looked up through the CPU, not the root device.
  Asking the root returns nothing, silently.
- EXTINT is raised only while the firmware is demonstrably in the wait loop, and held
  until the handler has collected the status, because the handler checks the pin first.

Measured: the handler is entered ~12900 times in a 30-second run, the read of `0x0C00` is
intercepted, and thousands of status bytes are handed over. The mechanism is not in doubt.

**It is still not enough.** The panel stops responding anyway. Four plausible encodings of
the status byte were enumerated and measured, and all four fail:

| Mode | Status byte | Statuses collected | Result |
| --- | --- | --- | --- |
| 0 | `(v+1) & 0x1F`, bit 7 clear | 7130 | panel dies |
| 1 | `v & 0x1F`, bit 7 clear | 7129 | panel dies |
| 2 | `0x80 \| (v & 0x1F)` | 8232 | panel dies |
| 3 | `0x80 \| ((v+1) & 0x1F)` | 255 | panel dies |

Mode 3 is the interesting one: the firmware asked far fewer times, so it did take a
different path — but it still did not recover.

## What that says about the remaining work

Answering the wait is not the same as satisfying the voice manager. The flag the wait loop
polls is written at `0x35DF` (`stb a2, f440[62]`), inside chain maintenance that the
handler only reaches on its *long* path — and which path it takes is decided by
`rams[0x2DC0 + 2v] == 0x80`. So the firmware expects the chip to be consistent across a
whole allocation cycle: which voices exist, in what order they complete, and what the
tables at `0x2DC0`, `0x2E00`, `0x2EC0`, `0x2F80` and the chain at `0x33C0` say about them.

The next step is therefore to trace a **complete note lifecycle** — the writes into
`0x0C00+` that start a voice, and what the firmware then expects back — rather than to
answer a single wait in isolation. The tooling for that is in place; the reverse
engineering is not.

## Why this is worth doing

Every Roland LA machine in MAME is in the same position — there is no LA32 anywhere in the
tree. A working control-side interface here is the piece that would let the D-10, D-20 and
MT-32 family be driven the same way.
