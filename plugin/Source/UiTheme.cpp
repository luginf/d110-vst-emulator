#include "UiTheme.h"

namespace d110ui {

namespace {

Theme currentTheme = Theme::Dark;

// Dark palette - the same numbers that used to be scattered across PluginEditor.cpp and
// D110SequencerPanel.cpp as local kEdXxx constants/literals; the values didn't change, just
// gathered into one place.
const Palette kDark {
	juce::Colour(0xff141416), // panelBg
	juce::Colour(0xff1d1d20), // box
	juce::Colour(0xff34343a), // boxBorder
	juce::Colour(0xff6fa8dc), // label
	juce::Colour(0xff8ede4a), // value
	juce::Colour(0xff787880), // dim

	juce::Colour(0xff141416), // handleBg
	juce::Colour(0xff26262c), // handleBar
	juce::Colour(0xff3a3a42), // handleBarHover
	juce::Colour(0xff8a8a94), // handleChevron
	juce::Colour(0xffd0d0d8), // handleChevronHover
	juce::Colour(0xff6a6a74), // handleLabel

	juce::Colour(0xffe8e8ec), // keyWhite
	juce::Colour(0xff6ab81f), // keyWhiteHeld
	juce::Colour(0xff0a0a0c), // keyWhiteBorder
	juce::Colour(0xff1a1a1e), // keyBlack
	juce::Colour(0xff3f7a10), // keyBlackHeld
	juce::Colour(0xff6a6a74), // keyCaption
	juce::Colour(0xff26262c), // keyButtonFill
	juce::Colour(0xff8a8a94), // keyButtonText

	juce::Colour(0xff6ab81f), // seqActiveFill
	juce::Colour(0xff0a0a0c), // seqActiveText
	juce::Colour(0xff26262c), // seqInactiveFill
	juce::Colour(0xffb8b8c0), // seqInactiveText
	juce::Colour(0xffd98a1f), // seqMetroDownbeat
	juce::Colour(0xff4fb81f), // seqMetroBeat
	juce::Colour(0xff3f7a10), // seqTrackFilled
	juce::Colour(0xff26262c), // seqTrackEmpty
};

// Light palette - the same meaning for every field (labels blue, values green, like the
// instrument's own LCD backlight), carried over onto a light background instead of a dark
// one. The accent green for a pressed key/active sequencer button is deliberately the same
// in both palettes - it's the "on" colour, not a theme colour.
const Palette kLight {
	juce::Colour(0xfff2f3f5), // panelBg
	juce::Colour(0xffffffff), // box
	juce::Colour(0xffc7cbd1), // boxBorder
	juce::Colour(0xff1f5fa8), // label
	juce::Colour(0xff2f8a1f), // value
	juce::Colour(0xff8a8f98), // dim

	juce::Colour(0xffe6e8eb), // handleBg
	juce::Colour(0xffd6d9de), // handleBar
	juce::Colour(0xffc3c7ce), // handleBarHover
	juce::Colour(0xff5a5f68), // handleChevron
	juce::Colour(0xff23262c), // handleChevronHover
	juce::Colour(0xff6f7480), // handleLabel

	juce::Colour(0xfffbfbfc), // keyWhite
	juce::Colour(0xff6ab81f), // keyWhiteHeld
	juce::Colour(0xff9a9ea6), // keyWhiteBorder
	juce::Colour(0xff2c2f36), // keyBlack
	juce::Colour(0xff3f7a10), // keyBlackHeld
	juce::Colour(0xff6f7480), // keyCaption
	juce::Colour(0xffd6d9de), // keyButtonFill
	juce::Colour(0xff3d4148), // keyButtonText

	juce::Colour(0xff6ab81f), // seqActiveFill
	juce::Colour(0xff0a0a0c), // seqActiveText
	juce::Colour(0xffd6d9de), // seqInactiveFill
	juce::Colour(0xff3d4148), // seqInactiveText
	juce::Colour(0xffb8701a), // seqMetroDownbeat
	juce::Colour(0xff3f9a1a), // seqMetroBeat
	juce::Colour(0xff3f7a10), // seqTrackFilled
	juce::Colour(0xffd6d9de), // seqTrackEmpty
};

} // namespace

Theme getTheme() { return currentTheme; }
void setTheme(Theme theme) { currentTheme = theme; }
const Palette &palette() { return currentTheme == Theme::Light ? kLight : kDark; }

} // namespace d110ui
