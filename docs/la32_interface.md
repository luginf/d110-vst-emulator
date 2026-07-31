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

---

## 2026-07-31 — the CPU core underneath was fixed, and it touches this window

Two bugs were found in MAME's **shared** MCS-96 CPU core while working on the
D-70 (same `src/devices/cpu/mcs96`, same shared tree). Both are now fixed, and
this project has been rebuilt against them. What that did and did not change
here, all measured:

**Rebuilt and re-verified**: `d110_core_test` boots the firmware in-process, the
MSM6222B dot matrix decodes to real screens, the buttons still drive the menus,
`Average speed: 99.96%`. VST3 and Standalone rebuilt and installed.

**The panel freeze is UNCHANGED, and that is the expected result.** `d110_longrun_test`
still reports exactly what it did before: silent = survived, notes forwarded =
**DIED at 4s**, notes withheld = survived over 45 s. The D-70's freeze was the
HSO CAM bug; **this one is not** — it is the LA32 wait documented above, and the
2×2 in this file still stands. Do not conflate the two.

**But the second bug does land in this driver's code path, exactly once.**
`fe7f mulb indexed_2b` read its byte operand through `any_r16`, which masks the
address down to even, so a byte at an odd address was fetched from its even
neighbour. A scan of the whole firmware (`D70-VST/tools/mcs96_mulb_scan.py`)
finds **one** real instance, and it is real code, not a data coincidence:

```
3990: ldbze 76, 1b[56]        ; index = [struct+0x1B]
3994: mulb  74, 1709[76]      ; reg74 *= table1709[index]     <-- the defect
399A: add   70, 74
...
39CE: add   70, #0808 ; shr 70, #04     ; clamp and scale
39E2: stb   70, 0c41[54]      ; <<< WRITES INTO THE LA32 REGISTER WINDOW
```

The surrounding routine builds a value out of key follow (`ldb 75,45` then
`subb 75,#3c` — the key minus middle C), a bias-point difference from the table
at `0x16F8`, and a depth from the signed curve at `0x1709`
(`55 2A 15 10 0A 05 02 00 FE FB F6 F0 EB …`, i.e. +85 … −21 centred on zero).
The base `0x1709` is **odd**, so whenever the index came out even the firmware
multiplied by the neighbouring table entry — one step off, roughly half the time.

**Audible effect today: none, and here is why.** The value goes to `0x0C41+`,
inside the `0x0C00–0x0DFF` sound-board window — the LA32, which MAME does not
emulate and which this plugin routes around via mt32emu. The other store,
`39DD: stb 70, f1c0[54]`, is RAM `0x31C0`, above the parameter regions the SysEx
bridge mirrors, so it is not forwarded either.

**Why it still matters:** the emulated firmware now computes this correctly, and
it is a *pitch-shaped* quantity (key follow + bias + fine offset, clamped) headed
for the LA32. Anyone resuming the LA32 interface work above would otherwise have
been reading half-wrong values out of the very window they are trying to
characterise.

**D-10 checked too, and it is clean**: both firmware versions show candidate byte
pairs only inside a `scall` dispatch table, and disassembling around them
produces garbage — data, not instructions.

---

## 2026-07-31, later the same day — the real dispatch routine, the real busy value, and a SECOND stall behind the first

Resuming exactly where the note above left off: "trace a complete note lifecycle." This
is what that trace found, all measured against the running firmware or read straight out
of the ROM with `disasm_tool`, not inferred.

**`r52` was never a hardware voice number — it never should have been fed to the status
byte at all.** It is the mainline's own logical wait-context id, used only to address
`f440[]`. The status byte's bits 0-4 name a **hardware voice slot**, a completely
different number space, and every encoding mode tried against r52 in the prior session
was doomed regardless of the bit-7/±1 arithmetic, because the input was wrong before the
arithmetic ever ran.

**The dispatch routine, ROM `0x3615`-`0x3657`, disassembled for the first time this
session:**

```
3615  stb  50, ee00[54]     ; ee00[slot] = r50
361A  stb  52, ee01[54]     ; ee01[slot] = r52's LOW BYTE  <-- the mapping
361F  st   58, f240[54]     ; f240[slot] = r58 (word)
3624  st   56, ee80[54]     ; ee80[slot] = r56 (word)
3629  stb  b7, f201[54]
...
3646  ldb  70, #20
3649  stb  70, edc0[54]     ; edc0[slot] = 0x20  (see correction below)
364E  ld   70, #ffff
3652  st   70, 0cc0[54]     ; 0cc0[slot] cleared
```

**`r54` (word, already slot*2) is the true hardware voice slot** for the whole
dispatch — the same register indexes `ee00`/`ee01`/`f240`/`ee80`/`f201`/`edc0`/`0cc0`
throughout. Critically, **dispatch itself records the reverse mapping**: `ee01[slot] =
r52`. The completion handler never needs to be told r52 at all — given a slot, it can
look up which context that slot belongs to, or (the direction the stub needs) given the
context currently being waited on, it can find which slot to report by searching for
the entry that matches.

**Measured busy value is `0x40`, not the `0x20` the listing above shows** —
`d110_la32_lifecycle_probe` (new tool, `plugin/la32_lifecycle_probe.cpp`) played one
real note (channel 2, program default) with `StuckPolicy::Off` and dumped `rams`
0x2D00-0x3000 before and after. Exactly two slots changed, both `80 -> 40` (one D-110
note uses two LA32 partials) and both stayed at `40` — something between `0x364E` and
wherever this routine actually ends must overwrite the `0x20` the partial listing shows;
trust the runtime measurement over an incomplete disassembly. `kSlotBusyValue` is now
`0x40`.

**Fix applied to `StuckPolicy::La32Stub`** (`D110Core.cpp`): read `r52` as before (it is
still needed — just not as the answer), then scan `rams 0x2DC0+2n` (busy == `0x40`)
**cross-referenced against `rams 0x2E01+2n == r52`** to find the one slot that is both
busy and actually backs the context the CPU is parked waiting for. Answering ANY busy
slot (tried first, before this was understood) was not enough once more than one note is
pending, i.e. any chord: the handler ran and genuinely consumed status bytes, but if it
wasn't the right slot, the specific loop the CPU sits in never got its flag set.

**Result, measured with `d110_hang_probe`'s policy comparison, before → after this
session's fixes:**

| | before (r52 fed directly as status) | after (r52 → slot cross-reference) |
| --- | --- | --- |
| handler 3138-3195 entered | 0 | 12,967 |
| reads of 0x0C00 seen | 0 | 192,028 |
| statuses actually handed over | 0 | 255 |

So the handshake mechanism itself is now demonstrably correct and doing real work — a
categorical improvement over every previous attempt, which never got the handler to run
at all. **The panel still does not recover.** This is not the same finding as before;
read on.

### The second stall: a completely different loop, gated by a completely different flag

Every policy that manages to release the *first* wait (`PokeRam`, `PulseExtInt`, and now
the corrected `La32Stub`) converges on the exact same new PC histogram once it does:

```
PC 2677   15.1%     <- new stall, dominant
PC 269A   13.9%
PC 267C   11.9%
PC 2681   11.9%
PC 269E    8.6%
```

Disassembled (`disasm_tool ... 2648 26b0`):

```
2673  ld    78, #0070          ; outer loop: 78 = 0x70, 0x60, ..., 0x00 (8 parts)
2677  ldb   70, f28c[78]       ; <-- the new stall sits here
267C  cmpb  70, f284[78]
2681  jc    269a                ; nothing to reclaim for this part -> next part
2683  ldbze 52, f285[78]       ; r52 = head of this part's active-voice chain
2688  sjmp  2697
268A  ldb   70, f460[52]       ; <-- f460, NOT f440 - a different array entirely
268F  jbs   70, 6, 2710        ; bit 6 set -> found a reclaimable voice, done
2692  ldbze 52, f3c0[52]       ; follow the chain to the next context
2697  jbc   52, 7, 268a        ; loop while not at the chain's end sentinel
269A  sub   78, #0010
269E  jc    2677                ; more parts to check -> loop
```

This is a **voice-reclamation scan**: for each of the 8 parts, walk the linked list of
voice-contexts it currently has allocated (via `f3c0[]`, the chain the D-70/D-110 project
notes already named without disassembling) looking for one whose `f460[]` entry has **bit
6** set. `f460[]` sits immediately after `f440[]` in RAM (`docs` already knew this much,
from `D110Core.h`'s comment on why `kVoiceFlagSpan` is 32 and not 64) but **nothing found
so far writes to it** — not the completion handler at `0x32DC`-`0x3614`, which only
touches `f440[]` (`0x35DF`) and the `edc0`/`ee00`/`ee40`/`eec0`/`ef80` tables already
described above.

**Working hypothesis, not yet confirmed**: `f460[]` bit 6 is set by **note-off**
processing, not by the sound board — i.e. it means "this voice's key has been released
and it may be reclaimed the next time a part needs one," which is a mainline concern
independent of LA32 completion. If that is right, this second stall may not need a sound
board answer at all — it may just need note-off to reach the firmware fast enough
relative to how many voices `hang_probe`'s chords consume (2 slots/note, 32 slots total,
16 notes before starvation). Unconfirmed: the chord pattern already releases notes ~230ms
after pressing them, which should be ample, so either the reclaim scan itself is not
being reached (something upstream of `0x2673` still stuck) or `f460` needs something this
session has not yet found.

### Resume point

1. Find what writes `f460[]` bit 6 — the same way dispatch was found this session:
   search around note-off handling first, since the working hypothesis is that this is
   a mainline consequence of releasing a key, not another sound-board answer.
2. Once found, check whether it depends on anything the LA32 stub should also be
   supplying, or whether it is purely a mainline consequence of note-off that should
   "just work" once note-off reaches the firmware — in which case the second stall may
   be a separate, unrelated bug (or simply not yet being exercised correctly by the test
   harness) rather than another piece of the sound-board interface.
3. `kSlotContextTable`/`kSlotStateTable`/`kSlotBusyValue` and the corrected `La32Stub` in
   `D110Core.cpp` should be kept — they are a real, measured fix for the *first* stall,
   independently of whether the second one turns out to be LA32-related at all.

Tools added this session: `plugin/la32_lifecycle_probe.cpp` (`d110_la32_lifecycle` target)
— boots, plays exactly one note, dumps the raw voice tables before/after. Reuse it with
`StuckPolicy::La32Stub` engaged and a chord instead of one note to catch `f460[]` in the
act, rather than disassembling further in the blind.

### Update, same session: `f460[]` bit 6 DOES get set — the fix partially works, but is fragile

The resume point above was followed. `plugin/la32_ctx_probe.cpp` (target `d110_la32_ctx`)
adds a live write tap over the whole `f3c0`-`f480` window (`D110Core::kVoiceCtxTapBase` —
**note this constant is a CPU program-space address, `0xF3C0`, not the rams array offset
`0x33C0`; the first version of this tap used the rams offset directly and silently taped
ROM instead, giving zero events every time — a reminder that this window has two valid
numbers and code must be explicit about which one it's using**). With `StuckPolicy::La32Stub`
engaged and 12 seconds of chords:

```
163 write events captured
f3c0: 32   f400: 16   f420: 16   f440: 18   f460: 32   f480: 16
f460[] writes with bit 6 SET: 16
```

So the full chain — dispatch → LA32 completion → `f440[]` chain walk (`0x24DA`, entered
via `ldbze 54,f440[52]` / `lcall 0x303E` / `ldbze 54,ee40[54]`, looping to `0xFF`) →
`f460[]` bit 6 set at `0x24FB` — **genuinely completes, 16 times in 12 seconds of chords**.
This is real, not a coincidence: `PC 24FB` (right after the `stb 70,f460[52]` at `0x24F6`)
is exactly the block found earlier this session, and the addresses climb sequentially
(`3460, 3461, 3462, …`) as successive logical contexts get serviced.

**But the panel is still dead by the end of every test, chord or single-note, and the
new `plugin/la32_realistic_test.cpp` (target `d110_la32_realistic`) shows it is not a
question of stress alone**: single notes with realistic ~500ms gaps (nothing like a
chord-stress pattern) died at **10 seconds** after only **2** successful LA32 services —
worse than the chord run's 16, despite far lower voice demand per second. That
inconsistency (16 successes under harder load, 2 under lighter load) says the remaining
bug is timing/ordering-sensitive, not simply "too many notes, too few voices" — something
about which context happens to be at the wait loop when the stub fires, or a corrupted
chain state after the first failure, likely still explains the eventual stall.

**Where this leaves the fix**: `StuckPolicy::La32Stub` with the `r52`→slot
cross-reference is a real, measured improvement over every prior attempt — the handshake
mechanism is provably correct at the byte level and completes real voice lifecycles some
of the time. It is not yet reliable enough to flip on by default; `forwardNotesToFirmware`
should stay off in shipped defaults until a session traces WHY only 2 of presumably many
more attempted contexts complete in the realistic-timing case, e.g. by adding sequence
numbers/timestamps to the `f460` write log and comparing against dispatch events in the
same run, rather than assuming duration or note count is the deciding factor.

### Update, same session: the mechanism is PROVEN correct — the remaining bug is thread-timing-sensitive, not load-sensitive

Two more tools settled the question the section above left open. `plugin/la32_ctx_probe.cpp`
was extended with a second write tap over the DISPATCH tables too
(`D110Core::kDispatchTapBase = 0xEDC0` — again a CPU address, not the `0x2DC0` rams
offset) so dispatch and completion land in one merged, naturally time-ordered log (both
taps fire on the single CPU thread, so append order already IS execution order — no
sequence numbers needed).

**Run with the same gentle, realistic single-note pattern `la32_realistic_test.cpp` uses
(not chords)**: all 32 hardware slots got dispatched, and **all 16 logical contexts
completed cleanly** — `f440[]` written, `f460[]` bit 6 set, in order, context 0 through
15 — then the trace ends mid-way through context 0 being reused for a 17th note. **Run
twice, byte-for-byte identical both times.** This rules out "only some fraction of
dispatched voices ever complete" and rules out flaky/random hardware-style races: the
completion chain traced in the section above is not just mechanically correct, it
reproducibly completes note after note under realistic play.

**Yet `plugin/la32_realistic_test.cpp` — the SAME note pattern, same `StuckPolicy::La32Stub`
— reproducibly dies after only 2 completions**, both with an 8s boot-settle and with a 12s
one (ruling out a race against the firmware's own boot-time test note, the first
hypothesis tried). `ramGeneration()` stops climbing at the same point notes stop
completing, confirming the CPU is genuinely wedged, not merely ignoring the panel.

**The one remaining structural difference between the two binaries**: `la32_ctx_probe`
calls `setVoiceCtxTap(true)`, which does not change any emulated CPU state (the tap never
writes to `data`) but does make every write in the `0x2DC0-0x2FFF`/`0x33C0-0x34FF` windows
take a mutex-and-vector-push instead of an early-return — real wall-clock overhead on a
hot path. **Both outcomes are internally deterministic (reproduce exactly), but differ
from each other** — which points at the relative real-time interleaving between the host
thread feeding MIDI via `processBlock` and MAME's own real-time-throttled CPU thread as
the actual variable, not note density, not boot timing, and not randomness. Extra
overhead on the CPU thread shifts that interleaving enough to avoid whatever exact
ordering corrupts things after context 1 or 2 in the unmodified case.

**This is a real, characterized finding, not a dead end, but it is a different KIND of bug
than everything above** — a cross-thread timing sensitivity in exactly when EXTINT/the
status byte get delivered relative to what the firmware's mainline code is doing at that
instant, not a wrong address or wrong value. Chasing it further needs deterministic
single-stepping (comparing the exact instruction stream around the second and third note
between a run that lives and one that dies) rather than more real-time wall-clock
experiments, which by nature can only observe outcomes, not force a specific interleaving.
That is a bigger investment than this session's remaining budget — a clean resume point
for later, not a wall.

**Practical takeaway for now**: keep `forwardNotesToFirmware` off by default (unchanged
from v0.9.3). The LA32 handshake is proven correct in isolation; what remains is a
timing-interleaving bug in how reliably that correct mechanism gets exercised under real
threading, which is a meaningfully narrower problem than where this session started
("nothing about the sound board is emulated at all").

### Update, same session, later: found a genuine MAME core bug — the "timing race" above was actually this

The user asked whether the freeze reproduces on the exact button combo they use (EDIT+ENTER
into ROM Play, ENTER to play the demo song "Macho Memory") rather than only the synthetic
MIDI tests. It does, identically — `plugin/demo_song_repro.cpp` (new tool this session)
confirmed the PC sits at `0x29E9`/`0x29EE` the same way. But running `StuckPolicy::La32Stub`
against it (self-contained - the demo song is the firmware playing itself, no host MIDI
thread involved at all) surfaced something the earlier cross-thread-timing theory didn't
predict: the handler entered the `0x3138` prologue **~22,000 times** for only 2 real
completions, almost always bailing at `0x313D`.

A live tap on the actual EXTINT line state (`D110Core::Port2Sample`, via the public
`device_execute_interface::input_line_state()` - `i8x9x_device::port2_r()` itself is
protected) settled it: **`m_stuckIntHigh` read false on essentially all 22,951 samples**
taken while the handler was mid-bail. We were not holding the line. Something else was
re-triggering the interrupt continuously regardless of our own bookkeeping.

**The cause is in MAME's own MCS-96 core, not this plugin.** `cpu/mcs96/mcs96ops.lst`,
the source the interpreter is generated from, dispatches an interrupt like this:

```
for(level = 7; level >= 0 && !(PSW & pending_irq & (1<<level)); level--);
if(level >= 0) {
    if(level != 7)
        pending_irq &= ~(1<<level);
    ...
```

**Level 7 is `IRQ_EXTINT` (`i8x9x.h`, `IRQ_EXTINT = 0x80`) — the one level explicitly
excluded from having its pending bit cleared when taken.** `i8x9x_device::execute_set_input`
only ever *sets* that bit on the rising edge (`pending_irq |= IRQ_EXTINT`); nothing clears
it on the falling edge either. So once the first edge is raised, that bit stays latched
forever, and the CPU re-takes the "same" stale interrupt on every opportunity, completely
independent of what the external line is actually doing - which is exactly what the
23,000-entry bail storm was.

**Fixed without touching MAME**, same way every other intervention in this project works
- through public APIs on a running machine: `mcs96_device::MCS96_INT_PENDING` is a public
state slot (the same debug-state mechanism `IOC1` is already read through). Clearing bit
`0x80` there in the same place the line itself is dropped:

```cpp
const uint64_t pending = m_cpu->state_int(mcs96_device::MCS96_INT_PENDING);
m_cpu->set_state_int(mcs96_device::MCS96_INT_PENDING, pending & ~uint64_t(0x80));
```

**Result, the same demo song, before → after:**

| | before | after |
| --- | --- | --- |
| handler prologue (`0x3138`) entries in 10s | ~22,000 (bail storm) | 3 |
| LA32 services completed | 2 | **20**, holding steady over a 45s run |
| screen | frozen from the start | shows the active-part indicator (a real voice sounding) for the whole run |

This retroactively reframes the "cross-thread timing" finding from earlier in this
document: what looked like sensitivity to relative thread interleaving was very likely
this same pending-bit bug being coincidentally masked or exposed by how much wall-clock
overhead shifted exactly when a stray bail happened to occur, not a genuine race in this
plugin's own code. The `r52`→slot cross-reference fix and the busy-value/context-table
corrections earlier in this document are unaffected and still correct - they are what
made these 20 real completions possible in the first place.

**SOLVED, same session, final update**: broadened the slot scan to accept EITHER busy
value (`kSlotBusyValue = 0x40` or `kSlotBusyValueAlt = 0x20`) matched against the owning
context. Reasoning: comparing all 32 slots' `edc0` values during the demo song showed most
actively-sounding voices sit at `0x20` ("dispatched and playing, not currently awaiting
anything") while only a voice genuinely mid-handshake shows `0x40` - a wait reached via the
reset/reclaim loop at ROM `0x29BB` (walks `ee40[]`, zeroes `0x0C00`/`0x0C80`/`eec0`/`ef00`
per slot but never touches `edc0`) leaves the slot at its stale prior value, which could be
either. Accepting both fixed it completely:

- **Demo song**: plays continuously for a full 60s run, `la32Services` climbing without
  ever plateauing (60 → 3944), progressing through three songs back to back ("Macho
  Memory" → "Jah May Kah!" → "Sugar Plum" - real D-110 demo song names), panel responsive
  at the end.
- **Stress-chord test** (`d110_hang_probe`'s policy comparison, the harness built
  specifically to kill the panel fast): survived the full 30s test window, was previously
  dying in 0-3s.
- **Realistic single-note test** (`d110_la32_realistic`): 120 notes over 60s, `la32Services`
  climbing steadily throughout, panel responsive at the end.

**Shipped as the new default**: `D110AudioProcessor::setPoweredOn()` now calls
`core.setStuckPolicy(D110Core::StuckPolicy::La32Stub)` unconditionally, and
`forwardNotes` (`PluginProcessor.h`) defaults to `true` instead of `false` - the tradeoff
that flag used to represent (part indicators vs. a live panel) no longer exists. VST3 and
Standalone rebuilt and reinstalled with this change.

**Old, now superseded — kept for the record**: "after 20 clean completions the demo song wedges again, deterministically,
`PC` back at `0x29E9`/`0x29EE` with no further EXTINT activity at all (3 line-state samples in
a 45-second run, i.e. genuinely no further servicing is being attempted - the slot scan is
presumably no longer finding a match for whatever context is now being awaited). This is
very likely the SAME "second stall" characterised earlier in this document (the `f460[]`
voice-reclaim chain) or a related bookkeeping desync, now visible on its own without the
pending-bit noise obscuring it. Next step: rerun the `f460`/dispatch merged trace
(`plugin/la32_ctx_probe.cpp`) against the demo song specifically (it was only ever run
against synthetic MIDI so far) to see what state the tables are in right at completion #20."

### Update, same session, one layer further: it's a THIRD stall, inside dispatch itself

Added `D110Core::osdRecordUnresolvedContext`/`lastUnresolvedContext_` (a tiny diagnostic:
whenever the slot scan runs and finds nothing, remember which `r52` it was looking for)
and dumped `edc0[]`/`ee01[]` live at the moment of the stall. Ground truth:

```
unresolved wait context: r52 = 1, stuck for 27321 consecutive ticks
  slot  2: edc0=20  ee01(owner)=01      <- the context DOES have an owning slot...
```

**Slot 2 does belong to context 1 - but its `edc0` is `0x20`, not the `0x40` the scan
requires.** This is not a bug in the scan. Re-reading the last dispatch events captured
before the stall:

```
PC 29D7  eec0[2] = 06
PC 29DF  ef00[2] = 06
                                          <- and then NOTHING for slot 2, ever again
```

`0x20` and `0x40` are two DIFFERENT states, not "not yet 0x40 vs 0x40" as first assumed:
comparing against the demo song's other 31 slots, MOST active, sounding voices sit at
`0x20` - only the ones currently, momentarily awaiting an LA32 answer show `0x40`. So
`0x20` = "dispatched and playing, not currently waiting on anything" and `0x40` =
"between dispatch and being answered." Context 1 is being **reused** for a new note (its
slot was previously assigned and had already settled at `0x20` from an earlier note), and
the new dispatch for this reuse got as far as `eec0[2]=06; ef00[2]=06` and then simply
never continued - never reaching its own `edc0=0x40` transition, let alone completing.

**This means the true blocker is one layer deeper than anything answered so far**:
dispatching a REUSED context depends on something - almost certainly reclaiming the
hardware slot(s) the previous occupant of context 1 held, i.e. the exact "second stall"
`f460[]`/`f3c0` chain machinery documented earlier in this file - and that reclaim itself
seems to be what's actually stuck, with the OUTER symptom (parked at `0x29E9` for r52=1)
just being where the CPU happens to sit while the deeper dependency is unmet. The `0x3061`-
`0x30E3` dispatch subroutine (the one that successfully carried slots 13/24/25/31 through
to `edc0=0x40` earlier in the same run) evidently branches into something that can itself
block, and that inner block is what needs disassembling next - not another slot-scan tweak.

### Where this leaves things — SOLVED

Superseded by the section above ("SOLVED, same session, final update"): the third layer
was the busy-value scan being too narrow (`0x40` only), not another stall. With both busy
values accepted, the demo song, the stress-chord test, and the realistic single-note test
all play indefinitely with the panel staying responsive. `StuckPolicy::La32Stub` and
`forwardNotesToFirmware` are both on by default as of this session - this is a shipped
feature now, not a workaround toggle. Kept the whole investigation trail above rather than
cleaning it up, because the wrong turns (r52-as-voice-number, premature EXTINT clearing
that turned out to be a MAME core bug, the 0x40-only scan) are exactly what the next
LA32-shaped problem on a sibling machine (D-10, D-20, MT-32) will need to avoid repeating.
