// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Michi71

// D5_Bridge.h -- everything the rest of the firmware needs from the LA engine,
// and nothing of the engine's own vocabulary. The controller and the MIDI
// front end talk to this; only this file knows what a partial is.

#ifndef D5_BRIDGE_H
#define D5_BRIDGE_H

#include <cstdint>
#include <string>
#include <vector>

#include "d5_engine/d5_patch.h"

namespace d5 { constexpr int kMaxVoicesPerTone = 8; }

class D5_Bridge {
public:
    // d110-vst-emulator port: upstream reaches the PCM sample data through a
    // global `extern const int16_t d5_pcm_blob[]` linked in from d5_blob.S
    // (a Pico .incbin of ROM-derived data, baked into flash at build time).
    // We never bake ROM-derived data into a shipped binary - same reasoning
    // as the D-110 firmware ROMs, loaded from disk at runtime instead - so
    // init() takes the blob as a pointer the caller loaded from d5_pcm.bin.
    // pcmBlob must outlive this D5_Bridge.
    void init(const int16_t* pcmBlob);

    uint32_t sampleRate() const { return 32000; }

    // Renders into the core's buffer layout: two int32 words per frame, left
    // then right, each carrying its 16-bit sample in the upper half.
    void fillBufferI32(int32_t* out, int frames);

    void noteOn(uint8_t note, uint8_t velocity);
    void noteOff(uint8_t note);
    void allNotesOff();

    // d110-vst-emulator port addition: loads a real patch bank at runtime
    // (typically parsed from a user's own SysEx dump by D5SyxLoader), taking
    // priority over whatever D5_HAVE_BANK compiled in and over the 8 built-in
    // presets - the same "prefer what the user actually supplied" precedence
    // pcmBlob_/init() uses. `names` may be shorter than `patches`; a missing
    // entry falls back to "Patch N". Selects patch 0 of the new bank.
    void loadBank(std::vector<std::vector<uint8_t>> patches, std::vector<std::string> names);
    bool hasRuntimeBank() const { return !bank_.empty(); }

    // d110-vst-emulator port addition: the PCM wave name for the panel's own
    // "PCM" parameter (0..99, see d5_engine/d5_patch_map.h's map_partial()) -
    // the frozen sample table (tools/d5_extract/d5_sample_table.json, baked
    // into d5_pcm_table.h at configure time) names each one regardless of
    // whether a ROM ever decoded successfully at runtime, so this needs no
    // instance state - kept here rather than in the editor so nothing above
    // D5_Bridge has to know d5_pcm_table.h exists at all.
    static const char* pcmWaveName(int waveNumber0to99);

    void selectPatch(int index);
    int patch() const { return patchIndex_; }
    // The rest of the firmware counts patches through here rather than
    // reaching into the preset table: that table is the engine's business.
    int patchCount() const;
    const char* patchName() const;
    // d110-vst-emulator port addition: patchName() only ever answers for the
    // currently sounding patch (a received bank's names live in the overlay,
    // not a plain table) - a patch-picker UI needs every name up front. Bank
    // names only; an overlay entry received live over MIDI is not reachable
    // by index here, same as on real hardware there is no way to read a
    // patch's name without first selecting it.
    const char* patchNameAt(int index) const;
    const char* structureName() const;

    void setVolume(int percent);           // 0..100
    void setMasterTune(int cents);         // -50..+50
    void setReverb(int percent);           // scales the patch's reverb balance
    void setChorus(int percent);
    void setVoiceLimit(int voices);        // per tone, 1..8
    void setPitchBendSemis(float semis);   // wheel position x bender range
    int bendRangeSemis() const { return bendRange_; }
    // RPN 0 data entry (EPROM 0x4E72): overwrites the patch's bender range
    // until the next patch change, exactly like the firmware's FE04/FE0C.
    void setBendRange(int semis);

    void setModWheel(float w);             // CC1, the D-50 lever's near kin   // reaches sounding notes
    void setAftertouch(float a);           // channel pressure, 0..1; survives patch changes like the wheel
    // CC64, the hold pedal. The D-50 receives it (its CC table lists 1, 5,
    // 6, 7, 38, 64, 65, 98, 99, 100, 101 and nothing else) and gates it on
    // a system switch at 0xC5C7; a held pedal defers every key-up until it
    // lifts.
    void setSustain(bool on);
    bool sustain() const { return sustain_; }
    void setPortamentoSwitch(bool on);     // CC65: overrides the patch's switch
    void setPortamentoTime(int percent);   // CC5, 0..100
    int voiceLimit() const { return voiceLimit_; }
    // The reverb and chorus balance in force, in the D-50's own 0..100.
    // They follow the patch on every change, so the panel always starts
    // from what the patch itself asks for.
    int reverbBalance() const { return reverb_; }
    // Reverb Type, the D-50's 32 rooms/halls/delays/gates (patch data,
    // panel numbering 1..32).
    void setReverbType(int t);
    int reverbType() const;
    // The rest of the D-50's own effect and mix parameters, in its own
    // units, with the firmware's own maxima (EPROM max table 0x7E10/0x7E50):
    // chorus type 0..7, rate/depth 0..100; EQ low freq 0..15, gain 0..24
    // (-12..+12 dB), high freq 0..21, Q 0..8, gain 0..24; tone balance
    // 0..100. All follow the patch on a change.
    void setChorusType(int v);      int chorusType() const  { return choType_; }
    void setChorusRate(int v);      int chorusRate() const  { return choRate_; }
    void setChorusDepth(int v);     int chorusDepth() const { return choDepth_; }
    void setEqLowFreq(int v);       int eqLowFreq() const   { return eqLoF_; }
    void setEqLowGain(int v);       int eqLowGain() const   { return eqLoG_; }
    void setEqHighFreq(int v);      int eqHighFreq() const  { return eqHiF_; }
    void setEqHighQ(int v);         int eqHighQ() const     { return eqHiQ_; }
    void setEqHighGain(int v);      int eqHighGain() const  { return eqHiG_; }
    void setToneBalance(int v);     int toneBalance() const { return toneBal_; }
    // d110-vst-emulator addition, no real-panel equivalent: audition any one
    // of the four partials alone (Alan, 2026-09-02, tracking down a
    // per-note artifact - one mute button per tone wasn't precise enough to
    // tell which of a tone's two partials was responsible). Voices/
    // envelopes on a muted partial keep running (see d5_patch.h's
    // Voice::next()) - this only silences its contribution to the mix, so
    // unmuting mid-note doesn't restart anything.
    // d110-vst-emulator addition: the TVF ENV DEPTH keyfollow direction, as a
    // live A/B switch (Alan, 2026-09-02 - the question is which one a real
    // D-50 sounds like, and only listening can answer it). Engine-global, not
    // per-instance, because it lives in the header-only engine; read once per
    // note, so it takes effect on the next key struck. See
    // d5_synth_voice.h's g_tvf_depth_kf_fixed for what the two directions are.
    static void setTvfKeyfollowFixed(bool fixed) { d5::g_tvf_depth_kf_fixed = fixed; }
    static bool tvfKeyfollowFixed() { return d5::g_tvf_depth_kf_fixed; }

    void setUpperPartial1Mute(bool m) { patch_.set_upper_partial1_mute(m); }
    void setUpperPartial2Mute(bool m) { patch_.set_upper_partial2_mute(m); }
    void setLowerPartial1Mute(bool m) { patch_.set_lower_partial1_mute(m); }
    void setLowerPartial2Mute(bool m) { patch_.set_lower_partial2_mute(m); }
    bool upperPartial1Muted() const { return patch_.upper_partial1_muted(); }
    bool upperPartial2Muted() const { return patch_.upper_partial2_muted(); }
    bool lowerPartial1Muted() const { return patch_.lower_partial1_muted(); }
    bool lowerPartial2Muted() const { return patch_.lower_partial2_muted(); }
    // Hz and dB behind those indices, for the display.
    float eqLowHz() const;
    float eqHighHz() const;
    int chorusBalance() const { return chorus_; }
    // What the governor actually allows in notes. A whole-mode patch runs
    // one tone, so a note costs one voice instead of two and the same
    // silicon carries twice as many -- which is exactly the D-50's own
    // 16-against-8 polyphony (bank driver 0x8003: all sixteen slots go to
    // the upper tone when the key mode is whole).
    int noteLimit() const { return wholeMode_ ? 2 * voiceLimit_ : voiceLimit_; }
    // Tails the CPU governor retired in the last second. A RATE, not a
    // total: a running total only ever climbs, so it says nothing about
    // whether the machine is coping right now. Zero means the render has
    // room; a handful means dense passages are being trimmed at their
    // quietest end; tens mean the patch is living at the limit.
    uint32_t shedRate() const { return shedRate_; }
    uint32_t shedTotal() const { return shedTotal_; }

    int activeVoices() const { return activeVoices_; }
    // Every note-on that reached this bridge since boot -- the footer shows
    // it so a stuck voice display can be told apart from notes never
    // arriving (chord of three: +3 here means delivery works, +1 means the
    // transport or parser dropped the rest).
    uint32_t noteOnTotal() const { return noteOnTotal_; }
    int cpuLoadPeakPercent() const { return cpuPeak_; }
    int outputPeakPercent() const { return outPeak_; }
    int bootBenchPercent();

    // ---- SysEx, the D-50's own address space -------------------------
    // The machine keeps the patch it is playing in a "temporary area" that
    // MIDI can read and write; the 448 bytes there are the same seven
    // 64-byte blocks a bulk dump carries. Writing them is how an editor
    // programs the instrument, and it is what stage one of this does.
    static constexpr int kPatchBytes = 448;
    const uint8_t* tempPatch() const { return temp_; }
    // Write into the temporary area and rebuild the sounding patch without
    // clearing effects or restarting LFOs -- an editor sends these in
    // streams, and configure() would click on every one.
    void sysexWriteTemp(int offset, const uint8_t* data, int len);
    // Read any stored patch, for answering a request. Returns null outside
    // the bank. Prefers a patch received over MIDI to the one in flash.
    const uint8_t* storedPatch(int index) const;
    // Write into internal memory. `off` counts bytes from the start of the
    // area (02-00-00), so a bulk message that straddles two slots is split
    // here rather than by the caller. The patches live in RAM: the D-50
    // keeps its sixty-four in battery-backed memory, ours would be flash,
    // and a flash write per DT1 would stall the render and wear the part.
    // What is received plays at once and survives patch changes; it does
    // not survive a power cycle.
    void sysexWriteStored(uint32_t off, const uint8_t* data, int len);
    int receivedPatchCount() const { return overlayUsed_; }

private:
    void applyPatch();
    void applyLevels();

    std::vector<std::vector<uint8_t>> bank_;   // runtime-loaded bank, see loadBank()
    std::vector<std::string> bankNames_;
    const int16_t* pcmBlob_ = nullptr;
    d5::Patch patch_{};
    int patchIndex_ = 0;
    int volume_ = 80;
    int tune_ = 0;
    int reverb_ = 100;
    int chorus_ = 100;
    int voiceLimit_ = d5::kMaxVoicesPerTone;
    bool wholeMode_ = false;        // set from the patch's key mode
    int shedHoldoff_ = 0;           // blocks since the last governor shed
    uint32_t shedTotal_ = 0;        // tails retired for the CPU, since boot
    uint32_t shedWindow_ = 0;       // ... in the second being counted
    uint32_t shedRate_ = 0;         // ... in the second before that
    uint32_t blockCount_ = 0;       // blocks into the current second
    // CC65/CC5 state, kept so a patch change restores its own setting and a
    // CC5 arriving before CC65 still lands when the switch does.
    bool portaSwitch_ = false;
    int portaTime_ = 0;
    int bendRange_ = 2;             // bender range in semitones, pb[26]
    int activeVoices_ = 0;
    uint32_t noteOnTotal_ = 0;
    int cpuPeak_ = 0;
    int outPeak_ = 0;
    float baseVolume_ = 1.0f;
    int patchReverbBal_ = 30;      // what the patch itself asks for, 0..100
    int patchChorusBal_ = 50;
    int choType_ = 0, choRate_ = 35, choDepth_ = 50;
    int eqLoF_ = 8, eqLoG_ = 12, eqHiF_ = 12, eqHiQ_ = 3, eqHiG_ = 12;
    int toneBal_ = 50;
    void applyEq();
    uint8_t temp_[448] = {};        // the D-50's temporary area
    // Patches received over MIDI, shadowing the flash bank. Sixty-four of
    // them, the size of a D-50's internal memory, addressed by absolute
    // patch index so they can come from any of the six banks; oldest is
    // reused when a sixty-fifth arrives.
    static constexpr int kOverlaySlots = 64;
    uint8_t overlay_[kOverlaySlots][448] = {};
    // Absolute patch index PLUS ONE, so that zero -- what the array starts
    // as -- means free. The D-50's own free list uses exactly this trick on
    // its voice slots; here it stops an all-zero table from claiming that
    // patch 1 has been overwritten, which silenced it outright.
    uint16_t overlayFor_[kOverlaySlots] = {};
    int overlayUsed_ = 0;
    int overlayNext_ = 0;                      // round-robin replacement
    char nameBuf_[20] = {};
    int overlayFind(int index) const;
    void applyStored(int index, int off, const uint8_t* data, int len);
    uint8_t held_[128] = {};
    bool sustain_ = false;
    uint8_t sustained_[128] = {};   // keys released under a held pedal
};

#endif // D5_BRIDGE_H
