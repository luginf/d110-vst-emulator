#include "D50Editor.h"

namespace {
juce::Slider &configureKnob(juce::Slider &s) {
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 18);
    s.setRange(0.0, 100.0, 1.0);
    return s;
}
}  // namespace

D50Editor::D50Editor(D50AudioProcessor &p) : juce::AudioProcessorEditor(p), processor(p), keyboard(p) {
    titleLabel.setText("D-50 Emulator (early port)", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    addAndMakeVisible(statusLabel);

    addAndMakeVisible(patchBox);
    patchBox.onChange = [this] {
        const int id = patchBox.getSelectedId();
        if (id > 0) processor.setPatch(id - 1);
    };

    for (auto *l : {&volumeLabel, &reverbLabel, &chorusLabel}) {
        l->setJustificationType(juce::Justification::centred);
        l->setFont(juce::Font(juce::FontOptions(13.0f)));
        addAndMakeVisible(l);
    }
    volumeLabel.setText("Volume", juce::dontSendNotification);
    reverbLabel.setText("Reverb", juce::dontSendNotification);
    chorusLabel.setText("Chorus", juce::dontSendNotification);

    configureKnob(volumeSlider).setValue(processor.getVolumePercent(), juce::dontSendNotification);
    volumeSlider.onValueChange = [this] { processor.setVolumePercent(static_cast<int>(volumeSlider.getValue())); };
    addAndMakeVisible(volumeSlider);

    configureKnob(reverbSlider).setValue(processor.getReverbPercent(), juce::dontSendNotification);
    reverbSlider.onValueChange = [this] { processor.setReverbPercent(static_cast<int>(reverbSlider.getValue())); };
    addAndMakeVisible(reverbSlider);

    configureKnob(chorusSlider).setValue(processor.getChorusPercent(), juce::dontSendNotification);
    chorusSlider.onValueChange = [this] { processor.setChorusPercent(static_cast<int>(chorusSlider.getValue())); };
    addAndMakeVisible(chorusSlider);

    addAndMakeVisible(keyboard);

    setSize(420, 220 + static_cast<int>(D110Keyboard::kRefH));
    startTimerHz(4);
    timerCallback();
}

D50Editor::~D50Editor() { stopTimer(); }

void D50Editor::refreshPatchList() {
    patchBox.clear(juce::dontSendNotification);
    if (!processor.isReady()) {
        patchBox.setTextWhenNothingSelected("(no patches)");
        return;
    }
    const int n = processor.patchCount();
    for (int i = 0; i < n; ++i) patchBox.addItem(juce::String(i + 1) + ". " + processor.patchNameAt(i), i + 1);
    patchBox.setSelectedId(processor.currentPatch() + 1, juce::dontSendNotification);
}

void D50Editor::timerCallback() {
    statusLabel.setText(processor.statusMessage(), juce::dontSendNotification);
    if (processor.isReady() != wasReady) {
        wasReady = processor.isReady();
        refreshPatchList();
    }
    // The bridge advances patch() on a bare Program Change too (no round trip
    // through setPatch()), so the combo box has to be kept in sync from here
    // rather than only from its own onChange.
    if (processor.isReady() && patchBox.getSelectedId() != processor.currentPatch() + 1)
        patchBox.setSelectedId(processor.currentPatch() + 1, juce::dontSendNotification);
}

void D50Editor::paint(juce::Graphics &g) { g.fillAll(juce::Colour(0xff2a2a2e)); }

void D50Editor::resized() {
    auto area = getLocalBounds().reduced(12);
    titleLabel.setBounds(area.removeFromTop(26));
    statusLabel.setBounds(area.removeFromTop(20));
    area.removeFromTop(8);
    patchBox.setBounds(area.removeFromTop(28));
    area.removeFromTop(16);

    auto knobs = area.removeFromTop(120);
    const int w = knobs.getWidth() / 3;
    auto layout = [&](juce::Rectangle<int> slot, juce::Label &label, juce::Slider &slider) {
        label.setBounds(slot.removeFromTop(18));
        slider.setBounds(slot);
    };
    layout(knobs.removeFromLeft(w), volumeLabel, volumeSlider);
    layout(knobs.removeFromLeft(w), reverbLabel, reverbSlider);
    layout(knobs, chorusLabel, chorusSlider);

    area.removeFromTop(12);
    keyboard.setBounds(area.removeFromTop(static_cast<int>(D110Keyboard::kRefH)));
}
