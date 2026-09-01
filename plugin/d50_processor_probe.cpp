// Offline audio check for D50AudioProcessor - same reasoning as
// d110_audio_test.cpp: instantiates the real processor, feeds it a note
// directly, and measures the output. No DAW, no audio device - this sandbox's
// real audio device path is documented as unreliable for headless checks
// (see project memory project_headless_audio_render_silent), so this is the
// way to verify the JUCE wiring (resampling, MIDI mapping) actually produces
// sound rather than trusting a Standalone window that opened but may not
// have a working audio callback under Xvfb.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <juce_audio_formats/juce_audio_formats.h>

#include "Source/d50/D50AudioProcessor.h"

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
    return 0;
}
