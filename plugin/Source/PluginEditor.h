#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Two-row monochrome display mimicking the real D-110 LCD. Both rows are built entirely from
// stable getters (part activity, our own volume, current patch name) rather than mt32emu's own
// transient getDisplayState() text, so nothing here flickers, reverts, or shows stale leftovers.
// Row 1: which of the 8 Parts + Rhythm currently have an active voice, plus master volume.
// Row 2: the Patch/Timbre name for whichever Part is currently selected for patch browsing.
class LcdComponent : public juce::Component {
public:
	void setSnapshot(const D110AudioProcessor::LcdSnapshot &newSnapshot) {
		snapshot = newSnapshot;
		repaint();
	}

	void paint(juce::Graphics &g) override {
		g.setColour(juce::Colour(0xff0d1a0d));
		g.fillRect(getLocalBounds());
		g.setColour(juce::Colour(0xff8fd15a));
		auto lcdArea = getLocalBounds().reduced(5);
		g.fillRect(lcdArea);

		// Real D-110 LCD text is bold, chunky and dark blue - not a plain dark green monospace.
		g.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 22.0f, juce::Font::bold));
		const juce::Colour lcdText(0xff17225c);
		const juce::Colour lcdTextDim(0xff4a6b45);

		auto rowHeight = lcdArea.getHeight() / 2;
		auto row1 = lcdArea.removeFromTop(rowHeight);
		auto row2 = lcdArea;

		// Part numbers 1-8 and R are always visible (matching the real hardware), but the
		// currently-sounding ones are drawn brighter/bolder so you can see who's playing.
		static const char partLabels[9] = {'1', '2', '3', '4', '5', '6', '7', '8', 'R'};
		auto charWidth = row1.getWidth() / 16;
		auto charArea = row1;
		for (int i = 0; i < 9; ++i) {
			bool active = (snapshot.partStatesBitmask & (1u << i)) != 0;
			g.setColour(active ? lcdText : lcdTextDim);
			g.drawText(juce::String::charToString(partLabels[i]), charArea.removeFromLeft(charWidth),
					   juce::Justification::centred);
		}
		g.setColour(lcdText);
		g.drawText(" Part" + juce::String(snapshot.selectedPartNumber), charArea, juce::Justification::centredLeft);

		g.setColour(lcdText);
		g.drawText(snapshot.patchLine, row2, juce::Justification::centredLeft);
	}

private:
	D110AudioProcessor::LcdSnapshot snapshot;
};

// Small round MIDI activity indicator, styled after the real "MIDI MESSAGE" LED.
class LedComponent : public juce::Component {
public:
	void setOn(bool shouldBeOn) {
		if (on != shouldBeOn) {
			on = shouldBeOn;
			repaint();
		}
	}

	void paint(juce::Graphics &g) override {
		auto bounds = getLocalBounds().toFloat().reduced(1.0f);
		g.setColour(juce::Colour(0xff202020));
		g.fillEllipse(bounds);
		g.setColour(on ? juce::Colours::orange : juce::Colour(0xff3a2a10));
		g.fillEllipse(bounds.reduced(2.0f));
	}

private:
	bool on = false;
};

// A small flat, elongated pushbutton styled after the real D-110's front-panel buttons (they're
// flat rectangles, not round). Like the real hardware, it never draws a label on its own face -
// button names are silkscreened on the panel above/below instead, so the editor pairs each of
// these with a separate juce::Label positioned that way.
class PanelButton : public juce::Button {
public:
	explicit PanelButton(const juce::String &glyph = {}) : juce::Button({}), centerGlyph(glyph) {}

	void paintButton(juce::Graphics &g, bool isMouseOverButton, bool isButtonDown) override {
		auto bounds = getLocalBounds().toFloat().reduced(1.0f);
		auto base = getToggleState() ? juce::Colour(0xff35402f) : juce::Colour(0xff333338);
		if (isButtonDown) base = base.darker(0.4f);
		else if (isMouseOverButton) base = base.brighter(0.15f);

		juce::ColourGradient gradient(base.brighter(0.15f), bounds.getCentreX(), bounds.getY(),
									  base.darker(0.25f), bounds.getCentreX(), bounds.getBottom(), false);
		g.setGradientFill(gradient);
		g.fillRoundedRectangle(bounds, 3.0f);
		g.setColour(juce::Colours::black.withAlpha(0.7f));
		g.drawRoundedRectangle(bounds, 3.0f, 1.0f);

		if (centerGlyph.isNotEmpty()) {
			g.setColour(juce::Colours::whitesmoke);
			g.setFont(juce::Font(bounds.getHeight() * 0.5f, juce::Font::bold));
			g.drawText(centerGlyph, getLocalBounds(), juce::Justification::centred);
		}
	}

private:
	juce::String centerGlyph;
};

// A small caption meant to sit directly above or below a PanelButton, matching the silkscreened
// labels on the real hardware's panel.
class ButtonCaption : public juce::Label {
public:
	explicit ButtonCaption(const juce::String &text, juce::Colour colour = juce::Colours::whitesmoke)
		: juce::Label({}, text) {
		setJustificationType(juce::Justification::centred);
		setFont(juce::Font(10.5f));
		setColour(juce::Label::textColourId, colour);
	}
};

class D110AudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
	explicit D110AudioProcessorEditor(D110AudioProcessor &processor);
	~D110AudioProcessorEditor() override;

	void paint(juce::Graphics &g) override;
	void resized() override;

private:
	void timerCallback() override;
	void chooseSysexBank();
	void refreshLcdAndLed();
	void stubButtonPressed(const juce::String &buttonName);

	D110AudioProcessor &audioProcessor;

	// --- Authentic front-panel reproduction ---
	// "Roland" is white, but "D-110" and the "MULTI TIMBRAL SOUND MODULE" subtitle are blue.
	juce::Label brandLabel{"", "Roland"};
	juce::Label modelNumberLabel{"", "D-110"};
	juce::Label modelLabel{"", "MULTI TIMBRAL SOUND MODULE"};

	juce::Label phonesLabel{"", "PHONES"};
	juce::Label volumeLabel{"", "VOLUME"};
	juce::Label volumeMinLabel{"", "MIN"};
	juce::Label volumeMaxLabel{"", "MAX"};
	juce::Slider volumeKnob;

	LcdComponent lcd;

	static constexpr juce::uint32 kAccentBlue = 0xff5ab4e0;
	static constexpr int kRackHeight = 200;

	// Row 1 (top strip): EXIT PATCH TIMBRE [up-arrows x4] WRITE/COPY - captions sit ABOVE this row.
	// Row 2 (bottom strip): EDIT PART SYSTEM [down-arrows x4] ENTER - captions sit BELOW this row.
	// The 4 arrow columns instead share one caption row BETWEEN row 1 and row 2 (PART/GROUP/BANK/NUMBER).
	PanelButton exitButton, patchButton, timbreButton, writeCopyButton;
	ButtonCaption exitCaption{"EXIT"}, patchCaption{"PATCH"}, timbreCaption{"TIMBRE"},
		writeCopyCaption{"WRITE/COPY"};

	PanelButton editButton, partButton, systemButton, enterButton;
	ButtonCaption editCaption{"EDIT"}, partCaption{"PART"}, systemCaption{"SYSTEM"}, enterCaption{"ENTER"};

	// PART/PARAMETER/VALUE are white; GROUP/BANK/NUMBER (the second word) are blue on the real unit.
	// "PARAMETER" is a single label centred over BOTH the Group and Bank columns, not repeated.
	juce::Label partRockerLabel{"", "PART"};
	juce::Label parameterSharedLabel{"", "PARAMETER"};
	juce::Label paramGroupBottomLabel{"", "GROUP"};
	juce::Label paramBankBottomLabel{"", "BANK"};
	juce::Label valueTopLabel{"", "VALUE"}, valueBottomLabel{"", "NUMBER"};
	PanelButton partUp{juce::CharPointer_UTF8("\xe2\x96\xb2")}, partDown{juce::CharPointer_UTF8("\xe2\x96\xbc")};
	PanelButton paramGroupUp{juce::CharPointer_UTF8("\xe2\x96\xb2")}, paramGroupDown{juce::CharPointer_UTF8("\xe2\x96\xbc")};
	PanelButton paramBankUp{juce::CharPointer_UTF8("\xe2\x96\xb2")}, paramBankDown{juce::CharPointer_UTF8("\xe2\x96\xbc")};
	PanelButton valueUp{juce::CharPointer_UTF8("\xe2\x96\xb2")}, valueDown{juce::CharPointer_UTF8("\xe2\x96\xbc")};

	juce::Label memoryCardLabel{"", "MEMORY CARD"};
	juce::Rectangle<int> memoryCardSlotBounds;
	juce::Rectangle<int> phonesJackBounds;

	juce::Label midiMessageLabel{"", "MIDI MESSAGE"};
	LedComponent midiLed;
	juce::Label powerLabel{"", "POWER"};
	PanelButton powerSwitch;

	// --- Non-authentic emulator utilities (ROM loading etc.) - visually separated below ---
	juce::Label utilitiesHeader{"", "Emulator Settings (not on real hardware)"};
	juce::TextButton importBankButton{"Import SysEx/MIDI Bank..."};
	juce::Label importStatusLabel;
	juce::Label actionFeedbackLabel;
	juce::Label midiChannelHintLabel;
	juce::ToggleButton reverbToggle{"Reverb"};
	juce::ToggleButton superModeToggle{"Super Mode (unofficial, extra polyphony)"};

	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterVolumeAttachment;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> reverbAttachment;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> superModeAttachment;

	std::unique_ptr<juce::FileChooser> fileChooser;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110AudioProcessorEditor)
};
