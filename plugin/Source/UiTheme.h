#pragma once

#include <juce_graphics/juce_graphics.h>

// Single colour palette for the hand-drawn parts of the interface - the extended editor,
// the test keyboard and the sequencer. NOT for the photographed panel itself: its
// indicators (LCD backlight, LEDs) reflect the instrument's real colours and shouldn't
// change with the theme, so D110Panel/D110MemoryCard don't reach in here.
namespace d110ui {

enum class Theme { Dark, Light };

struct Palette {
	juce::Colour panelBg;   // drawer background (kEdBack, keyboard/sequencer background)
	juce::Colour box;       // field/button rectangle
	juce::Colour boxBorder; // its border
	juce::Colour label;     // labels - blue, like Roland's own silkscreen
	juce::Colour value;     // values - green, like the D-110's own LCD backlight
	juce::Colour dim;       // secondary text

	juce::Colour handleBg;
	juce::Colour handleBar;
	juce::Colour handleBarHover;
	juce::Colour handleChevron;
	juce::Colour handleChevronHover;
	juce::Colour handleLabel;

	juce::Colour keyWhite;
	juce::Colour keyWhiteHeld;
	juce::Colour keyWhiteBorder;
	juce::Colour keyBlack;
	juce::Colour keyBlackHeld;
	juce::Colour keyCaption;
	juce::Colour keyButtonFill;
	juce::Colour keyButtonText;

	juce::Colour seqActiveFill;
	juce::Colour seqActiveText;
	juce::Colour seqInactiveFill;
	juce::Colour seqInactiveText;
	juce::Colour seqMetroDownbeat;
	juce::Colour seqMetroBeat;
	juce::Colour seqTrackFilled;
	juce::Colour seqTrackEmpty;
};

Theme getTheme();
void setTheme(Theme theme);
const Palette &palette();

} // namespace d110ui
