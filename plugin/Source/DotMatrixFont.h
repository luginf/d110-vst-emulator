#pragma once

#include <juce_graphics/juce_graphics.h>

// A small hand-authored 5x7 dot-matrix bitmap font - not a licensed typeface,
// just our own table, same idea as the real HD44780/MSM6222B character-
// generator ROM a genuine alphanumeric LCD uses (see D110Panel's own
// rebuildLcdImage() for the D-110's real, firmware-rendered LCD - unlike that
// one, this is a purely cosmetic lookalike for a display with no real
// firmware behind it to render an authentic one). First built for
// D110SequencerRetroPanel's D-20-style screen and extracted here so any
// other cosmetic LCD readout (the D-50 editor's, see D50Editor.cpp) can
// reuse the same glyphs instead of a second copy of the table.
namespace dotmatrix {

// Draws one line of glyphs, left edge at (x, y) with y the TOP of the
// character cell - not justification-aware itself, see drawDotText()/
// drawDotFitted() below for that.
void drawDotGlyphs(juce::Graphics &g, const juce::String &text, float x, float y, float charPx);

float dotTextWidth(const juce::String &text, float charPx);

// Drop-in dot-matrix replacement for juce::Graphics::drawText() - same left/
// right/centred justification, current g colour, clipped to `area` (a too-
// long string is clipped rather than overflowing into a neighbouring
// column).
void drawDotText(juce::Graphics &g, const juce::String &text, juce::Rectangle<float> area, juce::Justification just,
                  float charPx);

// Drop-in dot-matrix replacement for drawFittedText() - greedy word-wrap
// into at most maxLines lines, truncates rather than shrinking further.
void drawDotFitted(juce::Graphics &g, const juce::String &text, juce::Rectangle<float> area,
                    juce::Justification just, int maxLines, float charPx);

}  // namespace dotmatrix
