#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "D50AudioProcessor.h"
#include "../D110Keyboard.h"

// First-cut D-50 editor: a patch picker, the panel's own three balance knobs
// (volume/reverb/chorus), and the shared on-screen test keyboard so this is
// actually playable without a DAW or external controller. Not a front-panel
// recreation like D110Panel - there is no real firmware/panel behind this
// engine to photograph or measure (see CLAUDE.md's D-50 section), so this
// is plain JUCE widgets until a real design exists.
class D50Editor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit D50Editor(D50AudioProcessor &p);
    ~D50Editor() override;

    void paint(juce::Graphics &g) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshPatchList();

    D50AudioProcessor &processor;

    juce::Label titleLabel, statusLabel;
    juce::ComboBox patchBox;
    juce::Label volumeLabel, reverbLabel, chorusLabel;
    juce::Slider volumeSlider, reverbSlider, chorusSlider;
    D110Keyboard keyboard;

    bool wasReady = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D50Editor)
};
