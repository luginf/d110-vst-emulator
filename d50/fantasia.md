# Fantasia (patch 1) - decoded parameters

Cross-check material: every value below is decoded straight from
`PND50-00.syx`'s bytes for patch 1 ("Fantasia") through this port's own
`d5_engine/d5_patch_map.h`, formatted the way the D-50's own editor pages
would show it (note names, `+1`/`-1`/`+2`... LFO selects, `<A1`/`>C7` bias
points). The point is a page-by-page comparison against a real D-50's LCD
for the same patch - if a value here doesn't match the real unit, that is a
SysEx decode bug; if every value matches but the patch still sounds wrong,
the bug is downstream in synthesis (TVF/oscillator/envelope), not decode.

Status: **Upper Partial 1 decoded and handed to Alan for comparison against
his real D-50, 2026-09-02 - not yet confirmed either way.** Upper Partial 2
and the Common block (Structure, P-ENV, LFOs, EQ, Chorus) are not decoded
here yet; add them the same way once Partial 1 is confirmed or found wrong.
Lower Partial 1/2 and Lower Common are out of scope for this file (see
`project_sound_engine_investigation` in the auto-memory for why Lower is
being tracked separately - it uses PCM partials, "Bells" and "Spect2", and
its own investigation).

## Upper, Partial 1

Raw bytes (`kBlkUpperP1`, offsets 0-53 - the rest of the 64-byte block is
unused padding):

```
24 45 11 1 0 1 1 0 0 7 2 0 7 50 0 9 127 7 52 52 0 0 17 55 44 34 88 90 62 70
44 0 2 0 8 100 60 127 12 37 58 45 78 56 64 90 100 88 0 4 0 0 0 9
```

**WG Pitch**
- Coarse: C3
- Fine: -5
- Key Follow: 1

**WG Modulation**
- LFO Mode: (+)
- P-ENV Mode: OFF
- Bender Mode: Key Follow

**WG Waveform**
- Waveform: Sawtooth
- PCM Wave No: n/a (Structure 1 - both partials are the Synthesizer
  generator, this byte is unused)

**WG Pulse Width**
- Pulse Width: 0
- Velocity Range: 0
- After Touch Range: 0
- LFO Select: +2
- LFO Depth: 0

**TVF**
- Cutoff Frequency: 50
- Resonance: 0
- Key Follow: 3/4
- Bias Point: >C7
- Bias Level: 0

**TVF ENV**
- Depth: 52
- Velocity Range: 52
- Key Follow (Depth): 0
- Key Follow (Time): 0
- T1-T5: 17, 55, 44, 34, 88
- L1/L2/L3/Sustain Level: 90, 62, 70, 44
- End Level: 0

**TVF Modulation**
- LFO Level: +2
- LFO Depth: 0
- After Touch Range: +1

**TVA**
- Level: 100
- Velocity Range: +10
- Bias Point: >C7
- Bias Level: -12 (uncertain - the raw byte/table mapping for TVA Bias
  Level wasn't fully confirmed while writing this, unlike TVF's which uses
  the documented `kBiasMag` table)

**TVA ENV**
- T1-T5: 37, 58, 45, 78, 56
- L1/L2/L3/Sustain Level: 64, 90, 100, 88
- End Level: 0
- Velocity Follow (Time1): 4
- Key Follow (Time): 0

**TVA Modulation**
- LFO Select: +1
- LFO Depth: 0
- After Touch Range: +2
