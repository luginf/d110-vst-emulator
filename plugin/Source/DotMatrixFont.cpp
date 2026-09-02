#include "DotMatrixFont.h"

#include <array>

namespace {

// A small hand-authored 5x7 bitmap font - not a licensed typeface, just our own table,
// same idea as the real HD44780/MSM6222B character-generator ROM the genuine D-110 LCD
// uses (see D110Panel::rebuildLcdImage()): each set bit is one lit square dot with a gap
// to its neighbour, drawn directly rather than through juce::Font. Covers space, A-Z
// (case-folded), 0-9, and the handful of symbols the two screens that use this display.
using DotGlyph = std::array<juce::uint8, 7>;

constexpr juce::uint8 dotRow(int b4, int b3, int b2, int b1, int b0) {
	return (juce::uint8) ((b4 << 4) | (b3 << 3) | (b2 << 2) | (b1 << 1) | b0);
}

DotGlyph glyphFor(juce::juce_wchar c) {
	if (c >= 'a' && c <= 'z') c -= 32;
	switch (c) {
		case '0': return { dotRow(0,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,1,1), dotRow(1,0,1,0,1),
		                    dotRow(1,1,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,1,1,0) };
		case '1': return { dotRow(0,0,1,0,0), dotRow(0,1,1,0,0), dotRow(0,0,1,0,0), dotRow(0,0,1,0,0),
		                    dotRow(0,0,1,0,0), dotRow(0,0,1,0,0), dotRow(0,1,1,1,0) };
		case '2': return { dotRow(0,1,1,1,0), dotRow(1,0,0,0,1), dotRow(0,0,0,0,1), dotRow(0,0,0,1,0),
		                    dotRow(0,0,1,0,0), dotRow(0,1,0,0,0), dotRow(1,1,1,1,1) };
		case '3': return { dotRow(0,1,1,1,0), dotRow(1,0,0,0,1), dotRow(0,0,0,0,1), dotRow(0,0,1,1,0),
		                    dotRow(0,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,1,1,0) };
		case '4': return { dotRow(0,0,0,1,0), dotRow(0,0,1,1,0), dotRow(0,1,0,1,0), dotRow(1,0,0,1,0),
		                    dotRow(1,1,1,1,1), dotRow(0,0,0,1,0), dotRow(0,0,0,1,0) };
		case '5': return { dotRow(1,1,1,1,1), dotRow(1,0,0,0,0), dotRow(1,1,1,1,0), dotRow(0,0,0,0,1),
		                    dotRow(0,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,1,1,0) };
		case '6': return { dotRow(0,0,1,1,0), dotRow(0,1,0,0,0), dotRow(1,0,0,0,0), dotRow(1,1,1,1,0),
		                    dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,1,1,0) };
		case '7': return { dotRow(1,1,1,1,1), dotRow(0,0,0,0,1), dotRow(0,0,0,1,0), dotRow(0,0,1,0,0),
		                    dotRow(0,1,0,0,0), dotRow(0,1,0,0,0), dotRow(0,1,0,0,0) };
		case '8': return { dotRow(0,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,1,1,0),
		                    dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,1,1,0) };
		case '9': return { dotRow(0,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,1,1,1),
		                    dotRow(0,0,0,0,1), dotRow(0,0,0,1,0), dotRow(0,1,1,0,0) };
		case 'A': return { dotRow(0,0,1,0,0), dotRow(0,1,0,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1),
		                    dotRow(1,1,1,1,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1) };
		case 'B': return { dotRow(1,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,1,1,1,0),
		                    dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,1,1,1,0) };
		case 'C': return { dotRow(0,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,0), dotRow(1,0,0,0,0),
		                    dotRow(1,0,0,0,0), dotRow(1,0,0,0,1), dotRow(0,1,1,1,0) };
		case 'D': return { dotRow(1,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1),
		                    dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,1,1,1,0) };
		case 'E': return { dotRow(1,1,1,1,1), dotRow(1,0,0,0,0), dotRow(1,0,0,0,0), dotRow(1,1,1,1,0),
		                    dotRow(1,0,0,0,0), dotRow(1,0,0,0,0), dotRow(1,1,1,1,1) };
		case 'F': return { dotRow(1,1,1,1,1), dotRow(1,0,0,0,0), dotRow(1,0,0,0,0), dotRow(1,1,1,1,0),
		                    dotRow(1,0,0,0,0), dotRow(1,0,0,0,0), dotRow(1,0,0,0,0) };
		case 'G': return { dotRow(0,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,0), dotRow(1,0,1,1,1),
		                    dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,1,1,1) };
		case 'H': return { dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,1,1,1,1),
		                    dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1) };
		case 'I': return { dotRow(0,1,1,1,0), dotRow(0,0,1,0,0), dotRow(0,0,1,0,0), dotRow(0,0,1,0,0),
		                    dotRow(0,0,1,0,0), dotRow(0,0,1,0,0), dotRow(0,1,1,1,0) };
		case 'J': return { dotRow(0,0,1,1,1), dotRow(0,0,0,1,0), dotRow(0,0,0,1,0), dotRow(0,0,0,1,0),
		                    dotRow(0,0,0,1,0), dotRow(1,0,0,1,0), dotRow(0,1,1,0,0) };
		case 'K': return { dotRow(1,0,0,0,1), dotRow(1,0,0,1,0), dotRow(1,0,1,0,0), dotRow(1,1,0,0,0),
		                    dotRow(1,0,1,0,0), dotRow(1,0,0,1,0), dotRow(1,0,0,0,1) };
		case 'L': return { dotRow(1,0,0,0,0), dotRow(1,0,0,0,0), dotRow(1,0,0,0,0), dotRow(1,0,0,0,0),
		                    dotRow(1,0,0,0,0), dotRow(1,0,0,0,0), dotRow(1,1,1,1,1) };
		case 'M': return { dotRow(1,0,0,0,1), dotRow(1,1,0,1,1), dotRow(1,0,1,0,1), dotRow(1,0,0,0,1),
		                    dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1) };
		case 'N': return { dotRow(1,0,0,0,1), dotRow(1,1,0,0,1), dotRow(1,0,1,0,1), dotRow(1,0,0,1,1),
		                    dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1) };
		case 'O': return { dotRow(0,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1),
		                    dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,1,1,0) };
		case 'P': return { dotRow(1,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,1,1,1,0),
		                    dotRow(1,0,0,0,0), dotRow(1,0,0,0,0), dotRow(1,0,0,0,0) };
		case 'Q': return { dotRow(0,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1),
		                    dotRow(1,0,1,0,1), dotRow(1,0,0,1,0), dotRow(0,1,1,0,1) };
		case 'R': return { dotRow(1,1,1,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,1,1,1,0),
		                    dotRow(1,0,1,0,0), dotRow(1,0,0,1,0), dotRow(1,0,0,0,1) };
		case 'S': return { dotRow(0,1,1,1,1), dotRow(1,0,0,0,0), dotRow(1,0,0,0,0), dotRow(0,1,1,1,0),
		                    dotRow(0,0,0,0,1), dotRow(0,0,0,0,1), dotRow(1,1,1,1,0) };
		case 'T': return { dotRow(1,1,1,1,1), dotRow(0,0,1,0,0), dotRow(0,0,1,0,0), dotRow(0,0,1,0,0),
		                    dotRow(0,0,1,0,0), dotRow(0,0,1,0,0), dotRow(0,0,1,0,0) };
		case 'U': return { dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1),
		                    dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,1,1,0) };
		case 'V': return { dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1),
		                    dotRow(1,0,0,0,1), dotRow(0,1,0,1,0), dotRow(0,0,1,0,0) };
		case 'W': return { dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(1,0,1,0,1),
		                    dotRow(1,0,1,0,1), dotRow(1,1,0,1,1), dotRow(1,0,0,0,1) };
		case 'X': return { dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,0,1,0), dotRow(0,0,1,0,0),
		                    dotRow(0,1,0,1,0), dotRow(1,0,0,0,1), dotRow(1,0,0,0,1) };
		case 'Y': return { dotRow(1,0,0,0,1), dotRow(1,0,0,0,1), dotRow(0,1,0,1,0), dotRow(0,0,1,0,0),
		                    dotRow(0,0,1,0,0), dotRow(0,0,1,0,0), dotRow(0,0,1,0,0) };
		case 'Z': return { dotRow(1,1,1,1,1), dotRow(0,0,0,0,1), dotRow(0,0,0,1,0), dotRow(0,0,1,0,0),
		                    dotRow(0,1,0,0,0), dotRow(1,0,0,0,0), dotRow(1,1,1,1,1) };
		case '+': return { dotRow(0,0,0,0,0), dotRow(0,0,1,0,0), dotRow(0,0,1,0,0), dotRow(1,1,1,1,1),
		                    dotRow(0,0,1,0,0), dotRow(0,0,1,0,0), dotRow(0,0,0,0,0) };
		case '-': return { dotRow(0,0,0,0,0), dotRow(0,0,0,0,0), dotRow(0,0,0,0,0), dotRow(1,1,1,1,1),
		                    dotRow(0,0,0,0,0), dotRow(0,0,0,0,0), dotRow(0,0,0,0,0) };
		case '(': return { dotRow(0,0,0,1,0), dotRow(0,0,1,0,0), dotRow(0,1,0,0,0), dotRow(0,1,0,0,0),
		                    dotRow(0,1,0,0,0), dotRow(0,0,1,0,0), dotRow(0,0,0,1,0) };
		case ')': return { dotRow(0,1,0,0,0), dotRow(0,0,1,0,0), dotRow(0,0,0,1,0), dotRow(0,0,0,1,0),
		                    dotRow(0,0,0,1,0), dotRow(0,0,1,0,0), dotRow(0,1,0,0,0) };
		case '?': return { dotRow(0,1,1,1,0), dotRow(1,0,0,0,1), dotRow(0,0,0,0,1), dotRow(0,0,0,1,0),
		                    dotRow(0,0,1,0,0), dotRow(0,0,0,0,0), dotRow(0,0,1,0,0) };
		case '/': return { dotRow(0,0,0,0,1), dotRow(0,0,0,1,0), dotRow(0,0,0,1,0), dotRow(0,0,1,0,0),
		                    dotRow(0,1,0,0,0), dotRow(0,1,0,0,0), dotRow(1,0,0,0,0) };
		case '*': return { dotRow(0,0,0,0,0), dotRow(0,1,0,1,0), dotRow(0,0,1,0,0), dotRow(1,1,1,1,1),
		                    dotRow(0,0,1,0,0), dotRow(0,1,0,1,0), dotRow(0,0,0,0,0) };
		case '.': return { dotRow(0,0,0,0,0), dotRow(0,0,0,0,0), dotRow(0,0,0,0,0), dotRow(0,0,0,0,0),
		                    dotRow(0,0,0,0,0), dotRow(0,0,0,0,0), dotRow(0,1,1,0,0) };
		case ',': return { dotRow(0,0,0,0,0), dotRow(0,0,0,0,0), dotRow(0,0,0,0,0), dotRow(0,0,0,0,0),
		                    dotRow(0,0,0,0,0), dotRow(0,0,1,0,0), dotRow(0,1,0,0,0) };
		case '<': return { dotRow(0,0,0,0,1), dotRow(0,0,0,1,0), dotRow(0,0,1,0,0), dotRow(0,1,0,0,0),
		                    dotRow(0,0,1,0,0), dotRow(0,0,0,1,0), dotRow(0,0,0,0,1) };
		case '>': return { dotRow(1,0,0,0,0), dotRow(0,1,0,0,0), dotRow(0,0,1,0,0), dotRow(0,0,0,1,0),
		                    dotRow(0,0,1,0,0), dotRow(0,1,0,0,0), dotRow(1,0,0,0,0) };
		case '%': return { dotRow(1,0,0,0,1), dotRow(0,0,0,0,1), dotRow(0,0,0,1,0), dotRow(0,0,1,0,0),
		                    dotRow(0,1,0,0,0), dotRow(1,0,0,0,0), dotRow(1,0,0,0,1) };
		default: return {};
	}
}

}  // namespace

namespace dotmatrix {

void drawDotGlyphs(juce::Graphics &g, const juce::String &text, float x, float y, float charPx) {
	const float dot = charPx / 7.0f;
	for (auto c : text) {
		const auto glyph = glyphFor(c);
		for (int row = 0; row < 7; ++row)
			for (int col = 0; col < 5; ++col)
				if ((glyph[(size_t) row] >> (4 - col)) & 1)
					g.fillRect(x + (float) col * dot, y + (float) row * dot, dot * 0.85f, dot * 0.85f);
		x += dot * 6.0f; // 5 dot columns + 1 dot gap to the next character
	}
}

float dotTextWidth(const juce::String &text, float charPx) {
	return charPx / 7.0f * 6.0f * (float) text.length();
}

void drawDotText(juce::Graphics &g, const juce::String &text, juce::Rectangle<float> area,
                  juce::Justification just, float charPx) {
	const float w = dotTextWidth(text, charPx);
	float x = area.getX();
	if (just.testFlags(juce::Justification::horizontallyCentred)) x = area.getCentreX() - w * 0.5f;
	else if (just.testFlags(juce::Justification::right)) x = area.getRight() - w;
	// Clip to the caller's own area - a too-long string is clipped rather
	// than overflowing into a neighbouring column and turning into noise
	// (see D110SequencerRetroPanel's own history of this exact bug).
	juce::Graphics::ScopedSaveState clipState(g);
	g.reduceClipRegion(area.getSmallestIntegerContainer());
	drawDotGlyphs(g, text, x, area.getCentreY() - charPx * 0.5f, charPx);
}

void drawDotFitted(juce::Graphics &g, const juce::String &text, juce::Rectangle<float> area,
                    juce::Justification just, int maxLines, float charPx) {
	juce::StringArray words;
	words.addTokens(text, " ", "");
	juce::StringArray lines;
	juce::String cur;
	for (auto &w : words) {
		const juce::String trial = cur.isEmpty() ? w : cur + " " + w;
		if (dotTextWidth(trial, charPx) > area.getWidth() && !cur.isEmpty()) {
			lines.add(cur);
			cur = w;
		} else {
			cur = trial;
		}
	}
	if (cur.isNotEmpty()) lines.add(cur);
	while (lines.size() > maxLines) lines.remove(lines.size() - 1);

	auto a = area;
	const float lineH = charPx * 1.3f;
	for (auto &line : lines) drawDotText(g, line, a.removeFromTop(lineH), just, charPx);
}

}  // namespace dotmatrix
