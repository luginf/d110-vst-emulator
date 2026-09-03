// Offline audio check for D50AudioProcessor - same reasoning as
// d110_audio_test.cpp: instantiates the real processor, feeds it a note
// directly, and measures the output. No DAW, no audio device - this sandbox's
// real audio device path is documented as unreliable for headless checks
// (see project memory project_headless_audio_render_silent), so this is the
// way to verify the JUCE wiring (resampling, MIDI mapping) actually produces
// sound rather than trusting a Standalone window that opened but may not
// have a working audio callback under Xvfb.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <juce_audio_formats/juce_audio_formats.h>

#include "Source/d50/D50AudioProcessor.h"
#include "D5SyxLoader.h"

namespace {
void writeWav(const char *path, const juce::AudioBuffer<float> &buf, double sampleRate) {
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream(juce::File(path).createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.get(), sampleRate, static_cast<unsigned>(buf.getNumChannels()), 16, {}, 0));
    if (writer) {
        stream.release();  // the writer now owns it
        writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
    }
}
}  // namespace

// Usage: d50_processor_probe [sampleRate] [out.wav]
int main(int argc, char **argv) {
    const double sampleRate = argc > 1 ? std::atof(argv[1]) : 44100.0;
    const char *wavPath = argc > 2 ? argv[2] : nullptr;
    const int blockSize = 512;

    D50AudioProcessor proc;
    juce::AudioProcessorEditor *unusedEditor = nullptr;  // never shown, never touched
    juce::ignoreUnused(unusedEditor);

    proc.setPlayConfigDetails(0, 2, sampleRate, blockSize);
    proc.prepareToPlay(sampleRate, blockSize);

    if (!proc.isReady()) {
        std::fprintf(stderr, "NOT READY: %s\n", proc.statusMessage().toRawUTF8());
        return 1;
    }
    std::printf("ready: %s\n", proc.statusMessage().toRawUTF8());
    std::printf("patch %d/%d: %s\n", proc.currentPatch() + 1, proc.patchCount(),
                proc.patchNameAt(proc.currentPatch()).toRawUTF8());

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);

    double sumSquares = 0.0;
    float peak = 0.0f;
    int64_t totalSamples = 0;
    const int numBlocks = static_cast<int>(sampleRate * 2.0 / blockSize);  // ~2 seconds
    juce::AudioBuffer<float> captured(2, wavPath ? numBlocks * blockSize : 0);
    for (int b = 0; b < numBlocks; ++b) {
        buffer.clear();
        proc.processBlock(buffer, midi);
        midi.clear();  // note-on only fires once, on the very first block
        if (b == numBlocks / 2) {
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            proc.processBlock(buffer, off);  // extra block folds the note-off in too
        }
        if (wavPath) {
            captured.copyFrom(0, b * blockSize, buffer, 0, 0, blockSize);
            captured.copyFrom(1, b * blockSize, buffer, 1, 0, blockSize);
        }
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
            const float *data = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i) {
                sumSquares += static_cast<double>(data[i]) * data[i];
                peak = std::max(peak, std::abs(data[i]));
                ++totalSamples;
            }
        }
    }
    if (wavPath) {
        writeWav(wavPath, captured, sampleRate);
        std::printf("wrote %s\n", wavPath);
    }
    const double rms = std::sqrt(sumSquares / static_cast<double>(totalSamples));
    std::printf("rendered %d blocks at %.0f Hz: rms=%.5f peak=%.5f\n", numBlocks, sampleRate, rms, peak);
    if (rms < 1e-6) {
        std::fprintf(stderr, "SILENT: rms is ~zero, something is wrong\n");
        return 1;
    }
    std::printf("OK: engine is producing real audio through the JUCE processor\n");

    // The on-screen keyboard never touches processBlock's own MidiBuffer - it queues
    // through D110KeyboardHost::injectTestNote() (a juce::MidiMessageCollector, drained
    // inside processBlock itself), the same path a mouse/PC-keyboard click takes in the
    // real editor. Checked separately since it is a different code path from the MIDI
    // buffer test above and a GUI click under Xvfb isn't a reliable way to confirm it.
    D110KeyboardHost &keyboardHost = proc;
    keyboardHost.injectTestNote(1, 67, 1.0f, true);
    double kbSumSquares = 0.0;
    int64_t kbSamples = 0;
    for (int b = 0; b < 20; ++b) {
        buffer.clear();
        juce::MidiBuffer empty;
        proc.processBlock(buffer, empty);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
            const float *data = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i) {
                kbSumSquares += static_cast<double>(data[i]) * data[i];
                ++kbSamples;
            }
        }
    }
    keyboardHost.injectTestNote(1, 67, 1.0f, false);
    const double kbRms = std::sqrt(kbSumSquares / static_cast<double>(kbSamples));
    std::printf("on-screen keyboard path (injectTestNote): rms=%.5f\n", kbRms);
    if (kbRms < 1e-6) {
        std::fprintf(stderr, "SILENT: the keyboard's injectTestNote() path produced no audio\n");
        return 1;
    }
    std::printf("OK: on-screen keyboard reaches the engine too\n");

    // Regression check for the "PC keyboard chord plays only one note" report
    // (Alan, 2026-09-01): D110Keyboard::sendNote() broadcasts every key across
    // all 16 MIDI channels whenever MIDI Remap is off (its default) - fine for
    // the multitimbral D-110, which needs SOME part to hear it, but this
    // bridge is channel-blind, so each broadcast key press used to mean 16
    // redundant noteOn() calls stealing 16 real engine voice slots for one
    // note alone. Reproduces that exact broadcast pattern directly (not
    // through D110Keyboard, which needs a live X11 key event this sandbox
    // cannot send reliably) and checks a chord is actually louder than one
    // note, not the same.
    auto renderRms = [&](int numBlocksToRender) {
        double sq = 0.0;
        int64_t n = 0;
        for (int b = 0; b < numBlocksToRender; ++b) {
            buffer.clear();
            juce::MidiBuffer empty;
            proc.processBlock(buffer, empty);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
                const float *data = buffer.getReadPointer(ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i) {
                    sq += static_cast<double>(data[i]) * data[i];
                    ++n;
                }
            }
        }
        return std::sqrt(sq / static_cast<double>(n));
    };
    auto broadcastNote = [&](int note, bool on) {
        for (int ch = 1; ch <= 16; ++ch) keyboardHost.injectTestNote(ch, note, 1.0f, on);
    };

    broadcastNote(60, true);
    renderRms(4);  // let the attack settle before measuring
    const double oneNoteRms = renderRms(16);
    broadcastNote(60, false);
    renderRms(8);  // let it fully release before the next chord

    broadcastNote(60, true);
    broadcastNote(64, true);
    broadcastNote(67, true);
    renderRms(4);
    const double chordRms = renderRms(16);

    if (wavPath) {
        // A literal recording of the exact broadcast-chord scenario above (still
        // sounding at this point), so this can be judged by ear too, not just by
        // the RMS ratio - "<out.wav>_chord.wav" next to whatever WAV was asked for.
        juce::AudioBuffer<float> chordCapture(2, static_cast<int>(sampleRate * 2.0));
        int written = 0;
        while (written < chordCapture.getNumSamples()) {
            buffer.clear();
            juce::MidiBuffer empty;
            proc.processBlock(buffer, empty);
            const int n = std::min(blockSize, chordCapture.getNumSamples() - written);
            chordCapture.copyFrom(0, written, buffer, 0, 0, n);
            chordCapture.copyFrom(1, written, buffer, 1, 0, n);
            written += n;
        }
        writeWav((std::string(wavPath) + "_chord.wav").c_str(), chordCapture, sampleRate);
        std::printf("wrote %s_chord.wav\n", wavPath);
    }

    broadcastNote(60, false);
    broadcastNote(64, false);
    broadcastNote(67, false);

    std::printf("broadcast one note rms=%.5f, broadcast 3-note chord rms=%.5f (ratio %.2f)\n", oneNoteRms, chordRms,
                chordRms / oneNoteRms);
    // A generous margin above 1.0, not a precise acoustic prediction: real
    // chords can partly cancel depending on phase/patch, so this only checks
    // the qualitative thing that actually failed - measured directly (see
    // this file's own history) at 0.54 (chord QUIETER than one note) before
    // the fix in D5_Bridge::noteOn(), 1.23 after it.
    if (chordRms < oneNoteRms * 1.1) {
        std::fprintf(stderr,
                      "REGRESSION: a 3-note chord broadcast across 16 channels each isn't "
                      "meaningfully louder than one note - the chord-eats-the-voice-pool bug is back\n");
        return 1;
    }
    std::printf("OK: a broadcast chord is louder than one note - the voice pool isn't being "
                "eaten by channel-broadcast duplicates\n");

    // Editor parameter round trip (D50Editor's Tone section, 2026-09-01):
    // getPatchByte()/setPatchByte() go through D5_Bridge::sysexWriteTemp(),
    // the same call a real SysEx parameter edit would make - checks the byte
    // written is the byte read back, and that changing it actually changes
    // the sound (upper partial 1's cutoff, offset 13).
    constexpr int kCutoffOffset = 13;  // kBlkUpperP1 (0) + p[13], see d5_patch_map.h
    const int originalCutoff = proc.getPatchByte(kCutoffOffset);
    proc.setPatchByte(kCutoffOffset, 10);
    if (proc.getPatchByte(kCutoffOffset) != 10) {
        std::fprintf(stderr, "PARAM EDIT BROKEN: wrote cutoff=10, read back %d\n", proc.getPatchByte(kCutoffOffset));
        return 1;
    }
    keyboardHost.injectTestNote(1, 60, 1.0f, true);
    const double darkRms = renderRms(20);
    keyboardHost.injectTestNote(1, 60, 1.0f, false);
    renderRms(8);

    proc.setPatchByte(kCutoffOffset, 100);
    keyboardHost.injectTestNote(1, 60, 1.0f, true);
    const double brightRms = renderRms(20);
    keyboardHost.injectTestNote(1, 60, 1.0f, false);
    proc.setPatchByte(kCutoffOffset, originalCutoff);  // leave the sounding patch as found

    std::printf("cutoff=10 rms=%.5f, cutoff=100 rms=%.5f\n", darkRms, brightRms);
    if (std::abs(brightRms - darkRms) < std::min(darkRms, brightRms) * 0.05) {
        std::fprintf(stderr, "PARAM EDIT INERT: changing cutoff barely changed the sound\n");
        return 1;
    }
    std::printf("OK: editing a patch byte through the same path the editor uses audibly changes the sound\n");

    // Export/import round trip (D50Editor's "Patch..." menu, 2026-09-01):
    // export the sounding patch (with an edit, so the round trip actually
    // carries something), overwrite that same byte, then reimport the
    // exported file and check the edit came back.
    proc.setPatchByte(kCutoffOffset, 42);
    const juce::File tmp =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("d50_export_roundtrip_test.syx");
    if (!proc.exportCurrentPatch(tmp)) {
        std::fprintf(stderr, "EXPORT FAILED\n");
        return 1;
    }
    proc.setPatchByte(kCutoffOffset, 5);  // move it away from what was exported
    const juce::String importMsg = proc.importSyxBank(tmp);
    std::printf("export/import round trip: %s, cutoff after reimport=%d (expect 42)\n", importMsg.toRawUTF8(),
                proc.getPatchByte(kCutoffOffset));
    tmp.deleteFile();
    if (proc.getPatchByte(kCutoffOffset) != 42) {
        std::fprintf(stderr, "EXPORT/IMPORT ROUND TRIP BROKEN\n");
        return 1;
    }
    std::printf("OK: a patch exported through the editor's menu re-imports with edits intact\n");

    // Whole-bank export (Alan, 2026-09-01: "export n'export qu'un seul
    // instrument ? pas toute la banque ?"): every patch should come out, and
    // the currently-sounding one should carry its live edit even though that
    // edit only ever lands in tempPatch(), never back into the bank/overlay -
    // exportBank() has to substitute it in itself (see its own comment). A
    // fresh processor, not `proc`: the import round trip just above replaced
    // proc's own bank with a one-patch one - exactly correct behaviour for
    // that test, but not what a "does a real multi-patch bank export whole"
    // check needs.
    D50AudioProcessor proc2;
    proc2.setPlayConfigDetails(0, 2, sampleRate, blockSize);
    proc2.prepareToPlay(sampleRate, blockSize);
    if (!proc2.isReady()) {
        std::fprintf(stderr, "second processor instance NOT READY: %s\n", proc2.statusMessage().toRawUTF8());
        return 1;
    }
    const int bankCountBefore = proc2.patchCount();
    if (bankCountBefore < 4) {
        std::fprintf(stderr, "bank too small to test index 3 (%d patches)\n", bankCountBefore);
        return 1;
    }
    proc2.setPatch(3);                      // sounding patch is now index 3
    proc2.setPatchByte(kCutoffOffset, 77);  // live-edit it
    const juce::File bankFile =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("d50_export_bank_test.syx");
    if (!proc2.exportBank(bankFile)) {
        std::fprintf(stderr, "BANK EXPORT FAILED\n");
        return 1;
    }
    auto bankResult = d5::loadBankFromSyx(bankFile.getFullPathName().toStdString());
    bankFile.deleteFile();
    const bool bankSizeOk = bankResult.ok && static_cast<int>(bankResult.patches.size()) == bankCountBefore;
    std::printf("bank export: ok=%d patches=%zu (expect %d), patch 3 name='%s' cutoff byte=%d (expect 77)\n",
                bankResult.ok, bankResult.patches.size(), bankCountBefore,
                bankSizeOk && bankResult.names.size() > 3 ? bankResult.names[3].c_str() : "?",
                bankSizeOk ? bankResult.patches[3][kCutoffOffset] : -1);
    if (!bankSizeOk || bankResult.patches[3][kCutoffOffset] != 77) {
        std::fprintf(stderr, "BANK EXPORT BROKEN: %s\n", bankResult.message.c_str());
        return 1;
    }
    std::printf("OK: exportBank() writes every patch, with the sounding one's live edit included\n");

    // PCM wave names (Alan, 2026-09-02: "il faudrait indiquer le nom en plus
    // du numero") - checked against the same ROM survey this session already
    // confirmed by ear (d5_engine_probe's own PCM listing): wave 1 is Marmba,
    // wave 2 Vibes.
    const juce::String name0 = proc.pcmWaveName(0);
    const juce::String name1 = proc.pcmWaveName(1);
    std::printf("PCM wave names: 0='%s' (expect Marmba), 1='%s' (expect Vibes)\n", name0.toRawUTF8(),
                name1.toRawUTF8());
    if (name0 != "Marmba" || name1 != "Vibes") {
        std::fprintf(stderr, "PCM WAVE NAMES WRONG\n");
        return 1;
    }
    std::printf("OK: PCM wave names resolve correctly\n");

    // Upper/Lower tone scope (Alan, 2026-09-02: "rajouter un tone lower ?").
    // D50Editor::setToneScope() re-targets partial1/partial2 and the tone-
    // scoped ParamColumns at kLowerP1Base/kLowerP2Base/kLowerCommonBase
    // instead of the upper tone's - trivial one-line re-targeting
    // (PartialPanel::setBase()/ParamColumn::setBase(), both in D50Editor.h),
    // so what actually needs checking here is the block arithmetic itself:
    // that the lower tone's blocks are genuinely separate bytes from the
    // upper tone's, not an aliasing bug in the block-index constants.
    // (A full D50Editor is not instantiated here - a real GUI Component
    // outside a rendered window is exactly what plugin/*_editor_shot.cpp's
    // dedicated off-screen setup exists for, not a plain headless probe.)
    constexpr int kUpperP1Base = 0, kLowerP1Base = 3 * 64;
    constexpr int kCutoffKfOffset = 15;  // kTvfFull's "Cutoff Keyfollow", see D50Editor.cpp's own table
    proc2.setPatchByte(kUpperP1Base + kCutoffKfOffset, 3);
    proc2.setPatchByte(kLowerP1Base + kCutoffKfOffset, 9);
    const int upperBefore = proc2.getPatchByte(kUpperP1Base + kCutoffKfOffset);
    const int lowerBefore = proc2.getPatchByte(kLowerP1Base + kCutoffKfOffset);
    std::printf("tone scope: upper=%d (expect 3), lower=%d (expect 9), independent bytes confirmed\n", upperBefore,
                lowerBefore);
    if (upperBefore != 3 || lowerBefore != 9) {
        std::fprintf(stderr, "TONE SCOPE BROKEN: upper/lower blocks are not independent\n");
        return 1;
    }
    std::printf("OK: upper and lower tone blocks are independently addressable\n");

    // Upper/Lower solo-mute, one button per PARTIAL (Alan, 2026-09-02: asked
    // what audition tools were still missing while debugging Arco Strings'
    // sound, then split the two tone-wide buttons into four per-partial ones
    // to localize a per-note artifact the coarser tool couldn't pin down -
    // D5_Bridge::setUpperPartial1Mute()'s own comment has the design). proc2
    // is still on patch 3 (Arco Strings) from the bank-export test above.
    // Muting a whole tone (both its partials) should measurably quiet the
    // mix without silencing it outright (the other tone still plays); muting
    // just one partial of a tone should differ audibly from muting the other
    // (otherwise the two buttons aren't actually independent).
    auto renderMuteRms = [&](D50AudioProcessor &p) {
        juce::MidiBuffer m;
        m.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
        double sumSq = 0.0;
        int64_t n = 0;
        const int blocks = static_cast<int>(sampleRate * 1.0 / blockSize);
        for (int b = 0; b < blocks; ++b) {
            buffer.clear();
            p.processBlock(buffer, m);
            m.clear();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
                const float *data = buffer.getReadPointer(ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i) {
                    sumSq += static_cast<double>(data[i]) * data[i];
                    ++n;
                }
            }
        }
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        buffer.clear();
        p.processBlock(buffer, off);
        // Let the release tail fully die before the next call measures a
        // fresh note - otherwise the previous (differently-muted) note's
        // tail bleeds into the next measurement's RMS.
        for (int b = 0; b < 40; ++b) {
            buffer.clear();
            juce::MidiBuffer empty;
            p.processBlock(buffer, empty);
        }
        return std::sqrt(sumSq / static_cast<double>(n));
    };
    auto allOn = [&] {
        proc2.setUpperPartial1Muted(false);
        proc2.setUpperPartial2Muted(false);
        proc2.setLowerPartial1Muted(false);
        proc2.setLowerPartial2Muted(false);
    };
    allOn();
    const double rmsBoth = renderMuteRms(proc2);
    proc2.setUpperPartial1Muted(true);
    proc2.setUpperPartial2Muted(true);
    const double rmsLowerOnly = renderMuteRms(proc2);
    allOn();
    proc2.setLowerPartial1Muted(true);
    proc2.setLowerPartial2Muted(true);
    const double rmsUpperOnly = renderMuteRms(proc2);
    allOn();
    std::printf("solo/mute: both=%.5f upper-muted(lower only)=%.5f lower-muted(upper only)=%.5f\n", rmsBoth,
                rmsLowerOnly, rmsUpperOnly);
    if (!(rmsLowerOnly < rmsBoth) || !(rmsUpperOnly < rmsBoth) || rmsLowerOnly < 1e-6 || rmsUpperOnly < 1e-6) {
        std::fprintf(stderr, "SOLO/MUTE BROKEN: muting a tone should quiet the mix without silencing it\n");
        return 1;
    }
    std::printf("OK: muting either tone (both its partials) quiets the mix, the other tone keeps sounding\n");

    // Per-partial granularity: muting just Upper P1 vs just Upper P2 should
    // give genuinely different results (not the same number twice), or the
    // two buttons aren't actually independent controls.
    proc2.setUpperPartial1Muted(true);
    const double rmsUpperP2Only = renderMuteRms(proc2);  // upper P1 muted, P2 + lower sound
    allOn();
    proc2.setUpperPartial2Muted(true);
    const double rmsUpperP1Only = renderMuteRms(proc2);  // upper P2 muted, P1 + lower sound
    allOn();
    std::printf("per-partial: upper-P1-muted=%.5f upper-P2-muted=%.5f (expect different)\n", rmsUpperP2Only,
                rmsUpperP1Only);
    if (std::fabs(rmsUpperP2Only - rmsUpperP1Only) < 1e-6) {
        std::fprintf(stderr, "PARTIAL MUTE NOT INDEPENDENT: muting P1 and muting P2 sound identical\n");
        return 1;
    }
    std::printf("OK: each partial's mute button is independently audible\n");

    // Send-to-real-D-50 (Alan, 2026-09-02: A/B the current patch bytes -
    // including live Tone-tab edits - against his real unit over external
    // MIDI Out). No real MIDI Out device exists in this sandbox, so this only
    // checks the guard: no device selected means hasExternalMidiOutput() is
    // false and sendPatchToExternalMidi() is a no-op, not a crash.
    if (proc2.hasExternalMidiOutput()) {
        std::fprintf(stderr, "MIDI OUT GUARD BROKEN: hasExternalMidiOutput() true with none selected\n");
        return 1;
    }
    if (proc2.sendPatchToExternalMidi()) {
        std::fprintf(stderr, "MIDI OUT GUARD BROKEN: sendPatchToExternalMidi() succeeded with no device\n");
        return 1;
    }
    proc2.setMidiOutputDevice("this-identifier-does-not-exist-on-this-machine");
    if (proc2.hasExternalMidiOutput()) {
        std::fprintf(stderr, "MIDI OUT GUARD BROKEN: an unknown device identifier should fail to open\n");
        return 1;
    }
    std::printf("OK: send-to-real-D-50 correctly no-ops with no MIDI Out device selected\n");

    // Retrigger under a held sustain pedal (regression, 2026-09-02). This is
    // ordinary playing - strike a key, hold the pedal, strike the same key
    // again - and it used to produce complete silence on the second strike.
    // D5_Bridge::noteOff() deliberately leaves held_[note] set while the
    // pedal is down (the original's key array does the same), and noteOn()
    // had grown a `if (held_[note]) return;` guard against the on-screen
    // keyboard's 16-channel broadcast: together, every repeat strike over a
    // held pedal was dropped before it reached the engine. The broadcast is
    // now collapsed in D50AudioProcessor::injectTestNote() instead, and
    // noteOn() is back to upstream's "only a genuinely new note may steal".
    //
    // A plain "is the second strike loud enough" check would be worthless
    // here: the first note is still sustaining under the pedal, so its own
    // level alone clears any absolute threshold. The two runs below are
    // therefore differential - identical message streams except that the
    // second one strikes the key again - and a dropped note-on makes them
    // come out bit-identical, which is exactly what the old guard did.
    {
        auto sendCc = [&](int cc, int value) {
            juce::MidiBuffer m;
            m.addEvent(juce::MidiMessage::controllerEvent(1, cc, value), 0);
            buffer.clear();
            proc2.processBlock(buffer, m);
        };
        auto settle = [&]() {
            sendCc(64, 0);    // pedal up
            sendCc(123, 0);   // all notes off
            for (int b = 0; b < 200; ++b) {
                buffer.clear();
                juce::MidiBuffer empty;
                proc2.processBlock(buffer, empty);
            }
        };
        auto runPedalSequence = [&](bool strikeAgain) {
            settle();
            sendCc(64, 127);  // pedal down
            juce::MidiBuffer on;
            on.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
            buffer.clear();
            proc2.processBlock(buffer, on);
            for (int b = 0; b < 40; ++b) {  // let the attack pass
                buffer.clear();
                juce::MidiBuffer empty;
                proc2.processBlock(buffer, empty);
            }
            juce::MidiBuffer keyUp;  // swallowed by the pedal, the note keeps sounding
            keyUp.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            buffer.clear();
            proc2.processBlock(buffer, keyUp);

            juce::MidiBuffer again;
            if (strikeAgain) again.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
            double sumSq = 0.0;
            int64_t n = 0;
            for (int b = 0; b < 40; ++b) {
                buffer.clear();
                proc2.processBlock(buffer, again);
                again.clear();
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
                    const float *d = buffer.getReadPointer(ch);
                    for (int i = 0; i < buffer.getNumSamples(); ++i) {
                        sumSq += static_cast<double>(d[i]) * d[i];
                        ++n;
                    }
                }
            }
            return std::sqrt(sumSq / static_cast<double>(n));
        };

        const double withoutRestrike = runPedalSequence(false);
        const double withRestrike = runPedalSequence(true);
        settle();
        std::printf("sustain retrigger: pedal-held note alone rms=%.6f, same plus a repeat strike rms=%.6f\n",
                    withoutRestrike, withRestrike);
        // 10%: a working retrigger measures about +25% here, and a dropped
        // one about +3% - that residue is the previous run's reverb tail, not
        // the strike, since settle() cannot empty the hall completely in the
        // blocks it is given. Both figures were measured, in this probe, with
        // and without the old guard in place.
        if (std::fabs(withRestrike - withoutRestrike) < withoutRestrike * 0.10) {
            std::fprintf(stderr,
                         "SUSTAIN RETRIGGER BROKEN: striking a held key again over the pedal changed nothing "
                         "(%.6f vs %.6f) - the repeat note-on is being dropped before it reaches the engine\n",
                         withRestrike, withoutRestrike);
            return 1;
        }
        std::printf("OK: a key struck again over a held sustain pedal retriggers instead of falling silent\n");
    }

    // TVF ENV DEPTH keyfollow direction switch (Alan, 2026-09-02: wants to
    // A/B the two firmware revisions by ear in one click). The factory patch
    // this probe is sitting on may well have the keyfollow byte at 0 - most
    // do, Fantasia's is 0 on all four partials - so the test writes a real
    // one first, through the same SysEx path the editor uses, and plays high
    // up the keyboard where the key-distance term is largest.
    {
        constexpr int kUpperP1 = 0;  // block 0, 64 bytes per block
        proc2.setPatchByte(kUpperP1 * 64 + 20, 4);   // TVF ENV DEPTH KEYFOLLOW, max
        proc2.setPatchByte(kUpperP1 * 64 + 18, 80);  // TVF ENV DEPTH, so the term has something to scale
        auto renderNote = [&](int note) {
            juce::MidiBuffer m;
            m.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8)100), 0);
            double sumSq = 0.0;
            int64_t n = 0;
            for (int b = 0; b < 60; ++b) {
                buffer.clear();
                proc2.processBlock(buffer, m);
                m.clear();
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
                    const float *d = buffer.getReadPointer(ch);
                    for (int i = 0; i < buffer.getNumSamples(); ++i) {
                        sumSq += static_cast<double>(d[i]) * d[i];
                        ++n;
                    }
                }
            }
            juce::MidiBuffer off;
            off.addEvent(juce::MidiMessage::noteOff(1, note), 0);
            buffer.clear();
            proc2.processBlock(buffer, off);
            for (int b = 0; b < 120; ++b) {
                buffer.clear();
                juce::MidiBuffer empty;
                proc2.processBlock(buffer, empty);
            }
            return std::sqrt(sumSq / static_cast<double>(n));
        };
        if (!proc2.getTvfKeyfollowFixed()) {
            std::fprintf(stderr, "KEYFOLLOW SWITCH: expected the corrected (1.07) direction by default\n");
            return 1;
        }
        const double fixedRms = renderNote(84);
        proc2.setTvfKeyfollowFixed(false);
        const double rawRms = renderNote(84);
        proc2.setTvfKeyfollowFixed(true);
        std::printf("TVF keyfollow direction: v1.07=%.6f v1.06=%.6f\n", fixedRms, rawRms);
        if (std::fabs(fixedRms - rawRms) < fixedRms * 0.02) {
            std::fprintf(stderr,
                         "KEYFOLLOW SWITCH DEAD: both directions render the same (%.6f vs %.6f) - the "
                         "toggle is not reaching the engine\n",
                         fixedRms, rawRms);
            return 1;
        }
        if (!proc2.getTvfKeyfollowFixed()) {
            std::fprintf(stderr, "KEYFOLLOW SWITCH: the setter did not stick\n");
            return 1;
        }
        std::printf("OK: the TVF keyfollow direction switch audibly changes the sound and reads back\n");
    }

    // Sequencer, added 2026-09-03 (Alan's request, mono-timbral first cut - see
    // D50AudioProcessor's own D110SequencerHost overrides). The engine's own record/playback
    // timing is already proven correct in isolation by sequencer_probe.cpp - what's specific
    // to THIS class and worth checking here is that processBlock() actually wires it into the
    // real audio path: a note captured while armed/recording, then played back with no live
    // MIDI input at all, has to reach the bridge and come out as real audio.
    {
        D50AudioProcessor procSeq;
        procSeq.setPlayConfigDetails(0, 2, sampleRate, blockSize);
        procSeq.prepareToPlay(sampleRate, blockSize);
        if (!procSeq.isReady()) {
            std::fprintf(stderr, "SEQUENCER: processor not ready\n");
            return 1;
        }
        auto &eng = procSeq.getSequencer();
        eng.setTempo(120.0);
        eng.armTrack(0);
        eng.startRecording();
        eng.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
        eng.captureEvent(juce::MidiMessage::noteOff(1, 60), 2.0);  // 1 second at 120 BPM
        eng.stopRecording();
        eng.gotoBar(1);
        eng.play();

        juce::AudioBuffer<float> seqBuffer(2, blockSize);
        juce::MidiBuffer noInput;
        double sumSq = 0.0;
        int64_t n = 0;
        const int seqBlocks = static_cast<int>(sampleRate * 1.5 / blockSize);  // covers the 1s note
        for (int b = 0; b < seqBlocks; ++b) {
            seqBuffer.clear();
            procSeq.processBlock(seqBuffer, noInput);
            for (int ch = 0; ch < seqBuffer.getNumChannels(); ++ch) {
                const float *d = seqBuffer.getReadPointer(ch);
                for (int i = 0; i < seqBuffer.getNumSamples(); ++i) {
                    sumSq += static_cast<double>(d[i]) * d[i];
                    ++n;
                }
            }
        }
        const double seqRms = std::sqrt(sumSq / static_cast<double>(n));
        std::printf("sequencer playback (no live MIDI input): rms=%.5f\n", seqRms);
        if (seqRms < 1e-6) {
            std::fprintf(stderr,
                         "SEQUENCER SILENT: a captured note played back through processBlock() "
                         "produced no audio - the record/playback wiring is broken\n");
            return 1;
        }
        std::printf("OK: a note captured via the sequencer plays back through the real audio path, "
                    "with no live MIDI input\n");
    }
    return 0;
}
