#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
// MidiMessageCollector lives here, not in juce_audio_processors - see
// PluginProcessor.h's own comment on the same include.
#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <vector>

#include "../../../d50/D5_Bridge.h"
#include "../D110KeyboardHost.h"

// The D-50 plugin's AudioProcessor. Unlike D110AudioProcessor this owns no
// firmware/CPU emulation at all - d50/D5_Bridge.h is Michi71's reimplemented
// LA engine (see CLAUDE.md's D-50 section and d50/UPSTREAM_README.md), and
// this class is just the JUCE-side wiring around it: MIDI in -> D5_Bridge,
// D5_Bridge's fixed 32 kHz stereo output -> resampled to the host rate.
//
// The 512 KiB PCM sample blob (d5_pcm.bin, decoded from Alan's own D-50 ROM
// dump) is never compiled into this binary - loaded from disk at runtime,
// same reasoning as the D-110 firmware ROMs never being embedded either. See
// loadPcmBlob()'s own comment for the search path.
class D50AudioProcessor : public juce::AudioProcessor, public D110KeyboardHost {
public:
    D50AudioProcessor();
    ~D50AudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) override;

    bool isBusesLayoutSupported(const BusesLayout &layouts) const override;

    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "D-50 Emulator"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String &) override {}

    void getStateInformation(juce::MemoryBlock &destData) override;
    void setStateInformation(const void *data, int sizeInBytes) override;

    // --- D-50 specific, for the editor -------------------------------------
    bool isReady() const { return bridgeReady; }
    juce::String statusMessage() const { return status; }
    int patchCount() const { return bridgeReady ? bridge.patchCount() : 0; }
    juce::String patchNameAt(int index) const {
        return bridgeReady ? juce::String(bridge.patchNameAt(index)) : juce::String();
    }
    int currentPatch() const { return bridgeReady ? bridge.patch() : 0; }
    void setPatch(int index) {
        if (bridgeReady) bridge.selectPatch(index);
    }
    int getVolumePercent() const { return volumePercent; }
    void setVolumePercent(int v);
    int getReverbPercent() const { return bridgeReady ? bridge.reverbBalance() : 0; }
    void setReverbPercent(int v) {
        if (bridgeReady) bridge.setReverb(v);
    }
    int getChorusPercent() const { return bridgeReady ? bridge.chorusBalance() : 0; }
    void setChorusPercent(int v) {
        if (bridgeReady) bridge.setChorus(v);
    }

    // --- D110KeyboardHost ----------------------------------------------------
    // The bridge is monotimbral and does not look at MIDI channel at all (unlike
    // the D-110's 9 parts on 9 channels), so channel/remap only matter for what
    // the on-screen keyboard itself displays - they never change what sounds.
    void injectTestNote(int channel, int note, float velocity, bool on) override;
    int getKeyboardMidiChannel() const override { return keyboardMidiChannel; }
    void setKeyboardMidiChannel(int channel) override { keyboardMidiChannel = channel; }
    bool getMidiRemap() const override { return midiRemap; }
    void setMidiRemap(bool remap) override { midiRemap = remap; }
    bool getKeyboardPcInputEnabled() const override { return keyboardPcInputEnabled; }
    void setKeyboardPcInputEnabled(bool enabled) override { keyboardPcInputEnabled = enabled; }
    int getKeyboardPcLayout() const override { return keyboardPcLayout; }
    void setKeyboardPcLayout(int layout) override { keyboardPcLayout = layout; }
    bool isNoteActive(int note) const override {
        return note >= 0 && note < 128 && noteActiveFlags[static_cast<size_t>(note)].load(std::memory_order_relaxed);
    }

private:
    bool loadPcmBlob();
    void loadPatchBank();
    void handleMidiMessage(const juce::MidiMessage &m);
    // Renders exactly numOutSamples stereo samples at hostSampleRate into
    // outL/outR, pulling as much 32 kHz audio from the bridge as the
    // resampler needs and keeping whatever it doesn't consume for next time
    // (pendingL/R) - dropping that remainder instead would throw away real,
    // already-rendered audio and glitch the pitch every block.
    void renderInternal(int numOutSamples, float *outL, float *outR);

    D5_Bridge bridge;
    std::vector<int16_t> pcmBlob;
    bool bridgeReady = false;
    juce::String status{"D-50: not loaded yet"};

    double hostSampleRate = 44100.0;
    juce::LagrangeInterpolator resamplerL, resamplerR;
    std::vector<float> pendingL, pendingR;
    std::vector<int32_t> renderScratch;

    int volumePercent = 80;

    // RPN 0 (bender range): CC101/CC100 select the parameter, CC6 supplies
    // the value; 127/127 is MIDI's "no RPN selected" null.
    int rpnMsb = 127, rpnLsb = 127;

    // On-screen keyboard support: mouse/PC-keyboard clicks arrive on the message
    // thread via injectTestNote(), queued here exactly like the D-110 plugin's own
    // osMidiCollector and drained into the real MIDI stream from processBlock().
    juce::MidiMessageCollector injectedMidi;
    int keyboardMidiChannel = 1;
    bool midiRemap = false;
    bool keyboardPcInputEnabled = false;
    int keyboardPcLayout = 0;
    std::array<std::atomic<bool>, 128> noteActiveFlags{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D50AudioProcessor)
};
