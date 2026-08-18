#include "D110SequencerRetroPanel.h"

#include <cmath>

#include "../UiTheme.h"

using d110seq::D110SequencerEngine;

namespace {

juce::String recordModeShortLabel(d110seq::RecordMode m) {
	switch (m) {
		case d110seq::RecordMode::overdub: return "OVERDUB";
		case d110seq::RecordMode::replaceRange: return "REPLACE";
		case d110seq::RecordMode::replaceToEnd: return "REPL+END";
	}
	return {};
}

juce::String loopModeShortLabel(d110seq::LoopMode m) {
	switch (m) {
		case d110seq::LoopMode::off: return "OFF";
		case d110seq::LoopMode::bar: return "BAR";
		case d110seq::LoopMode::punch: return "PUNCH";
	}
	return {};
}

juce::String stepDurationShortLabel(d110seq::QuantizeGrid g) {
	using d110seq::QuantizeGrid;
	switch (g) {
		case QuantizeGrid::whole: return "1/1";
		case QuantizeGrid::half: return "1/2";
		case QuantizeGrid::quarter: return "1/4";
		case QuantizeGrid::eighth: return "1/8";
		case QuantizeGrid::sixteenth: return "1/16";
		case QuantizeGrid::eighthTriplet: return "1/8T";
		case QuantizeGrid::sixteenthTriplet: return "1/16T";
		case QuantizeGrid::thirtySecond: return "1/32";
		case QuantizeGrid::off:
		default: return "1/4";
	}
}

// Character set the NameEdit screen's UP/DOWN cycles through at the caret position -
// space first (so trimming back to the default label is one press away), then A-Z, 0-9.
const juce::String kNameEditCharset(" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");

// ---------------------------------------------------------------------------
// Dot-matrix LCD font
// ---------------------------------------------------------------------------
// A small hand-authored 5x7 bitmap font - not a licensed typeface, just our own table,
// same idea as the real HD44780/MSM6222B character-generator ROM the genuine D-110 LCD
// uses (see D110Panel::rebuildLcdImage()): each set bit is one lit square dot with a gap
// to its neighbour, drawn directly rather than through juce::Font. Covers exactly what
// this screen displays - space, A-Z (case-folded, this LCD is all-caps like the real
// hardware's own menus), 0-9, and the handful of symbols the menu strings use.
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

// Draws one line of glyphs, left edge at (x, y) with y the TOP of the character cell -
// not justification-aware itself, see drawDotText()/drawDotFitted() below for that.
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

// Drop-in dot-matrix replacement for the single-line juce::Graphics::drawText() calls
// this screen used to make - same left/right/centred justification, current g colour.
void drawDotText(juce::Graphics &g, const juce::String &text, juce::Rectangle<float> area,
                  juce::Justification just, float charPx) {
	const float w = dotTextWidth(text, charPx);
	float x = area.getX();
	if (just.testFlags(juce::Justification::horizontallyCentred)) x = area.getCentreX() - w * 0.5f;
	else if (just.testFlags(juce::Justification::right)) x = area.getRight() - w;
	drawDotGlyphs(g, text, x, area.getCentreY() - charPx * 0.5f, charPx);
}

// Drop-in dot-matrix replacement for drawFittedText() - greedy word-wrap into at most
// maxLines lines (good enough for the short confirm messages this is used for), then
// truncates rather than shrinking further, same as drawFittedText's own maxLines cap.
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

void paintRetroButton(juce::Graphics &g, juce::Rectangle<float> b, const juce::String &label, bool active) {
	const auto &pal = d110ui::palette();
	g.setColour(active ? pal.seqActiveFill : pal.seqInactiveFill);
	g.fillRoundedRectangle(b.reduced(2.0f), 3.0f);
	g.setColour(active ? pal.seqActiveText : pal.seqInactiveText);
	g.setFont(juce::FontOptions(juce::jlimit(8.0f, 13.0f, b.getHeight() * 0.42f)));
	g.drawText(label, b, juce::Justification::centred);
}

} // namespace

D110SequencerRetroPanel::D110SequencerRetroPanel(D110SequencerHost &p) : processor(p) {
	startTimerHz(15);
	setWantsKeyboardFocus(true);
}

D110SequencerRetroPanel::~D110SequencerRetroPanel() { stopTimer(); }

d110seq::D110SequencerEngine &D110SequencerRetroPanel::engine() { return processor.getSequencer(); }

juce::String D110SequencerRetroPanel::defaultTrackLabel(int t) {
	if (t == D110SequencerEngine::kRhythmTrack) return "RHYTHM";
	if (t < D110SequencerEngine::kNumTracks) return "PART " + juce::String(t + 1);
	return "TRACK " + juce::String(t + 1);
}

void D110SequencerRetroPanel::timerCallback() {
	auto &eng = engine();
	if (eng.isPlaying() || eng.isStepRecording() || eng.isPrecounting()) repaint();
}

// ---------------------------------------------------------------------------
// Navigation stack
// ---------------------------------------------------------------------------

void D110SequencerRetroPanel::pushScreen(Screen s) {
	stack.push_back(std::move(s));
	repaint();
}

void D110SequencerRetroPanel::popScreen() {
	if (!stack.empty()) stack.pop_back();
	repaint();
}

// ---------------------------------------------------------------------------
// Hardware buttons
// ---------------------------------------------------------------------------

void D110SequencerRetroPanel::pressStop() {
	// Same as D110SequencerPanel's plain-click STOP: halts the transport AND sends a MIDI
	// panic, so a note-off scheduled past the stop point never gets left stuck sounding -
	// see D110SequencerPanel::mouseDown()'s own comment on why.
	engine().stop();
	processor.midiPanic();
	repaint();
}

void D110SequencerRetroPanel::pressPlay() {
	engine().play();
	repaint();
}

void D110SequencerRetroPanel::pressRec() {
	auto &eng = engine();
	if (eng.isRecording()) eng.stopRecording();
	else if (eng.getArmedTrack() >= 0) eng.startRecording();
	repaint();
}

void D110SequencerRetroPanel::pressUp() {
	if (stack.empty()) {
		if (engine().isStepRecording()) { engine().stepRest(); repaint(); return; }
		homeAdjust(+1);
		return;
	}
	auto &s = top();
	if (s.kind == ScreenKind::list) {
		auto items = s.buildItems();
		if (!items.empty()) s.cursor = (s.cursor - 1 + (int) items.size()) % (int) items.size();
	} else if (s.kind == ScreenKind::form) {
		if (!s.fields.empty()) {
			auto &f = s.fields[(size_t) juce::jlimit(0, (int) s.fields.size() - 1, s.cursor)];
			*f.value = juce::jlimit(f.minValue, f.maxValue, *f.value + 1);
		}
	} else if (s.kind == ScreenKind::nameEdit) {
		nameEditAdjust(+1);
	}
	repaint();
}

void D110SequencerRetroPanel::pressDown() {
	if (stack.empty()) {
		if (engine().isStepRecording()) { engine().stepBack(); repaint(); return; }
		homeAdjust(-1);
		return;
	}
	auto &s = top();
	if (s.kind == ScreenKind::list) {
		auto items = s.buildItems();
		if (!items.empty()) s.cursor = (s.cursor + 1) % (int) items.size();
	} else if (s.kind == ScreenKind::form) {
		if (!s.fields.empty()) {
			auto &f = s.fields[(size_t) juce::jlimit(0, (int) s.fields.size() - 1, s.cursor)];
			*f.value = juce::jlimit(f.minValue, f.maxValue, *f.value - 1);
		}
	} else if (s.kind == ScreenKind::nameEdit) {
		nameEditAdjust(-1);
	}
	repaint();
}

void D110SequencerRetroPanel::pressLeft() {
	if (stack.empty()) {
		if (!engine().isStepRecording()) {
			homeField = static_cast<HomeField>((static_cast<int>(homeField) + kHomeFieldCount - 1) % kHomeFieldCount);
			if (homeField == HomeField::transport) syncHomeTransportChoice();
			repaint();
		}
		return;
	}
	auto &s = top();
	if (s.kind == ScreenKind::form) {
		if (!s.fields.empty()) s.cursor = (s.cursor - 1 + (int) s.fields.size()) % (int) s.fields.size();
	} else if (s.kind == ScreenKind::confirm) {
		s.confirmYes = false;
	} else if (s.kind == ScreenKind::nameEdit) {
		nameEditMoveCaret(-1);
	}
	repaint();
}

void D110SequencerRetroPanel::pressRight() {
	if (stack.empty()) {
		if (!engine().isStepRecording()) {
			homeField = static_cast<HomeField>((static_cast<int>(homeField) + 1) % kHomeFieldCount);
			if (homeField == HomeField::transport) syncHomeTransportChoice();
			repaint();
		}
		return;
	}
	auto &s = top();
	if (s.kind == ScreenKind::form) {
		if (!s.fields.empty()) s.cursor = (s.cursor + 1) % (int) s.fields.size();
	} else if (s.kind == ScreenKind::confirm) {
		s.confirmYes = true;
	} else if (s.kind == ScreenKind::nameEdit) {
		nameEditMoveCaret(+1);
	}
	repaint();
}

// See the header's callback convention comment: list items may push/pop freely (we never
// hold a reference into `stack` across their call, `items` is a local copy); form/confirm
// onConfirm may only touch engine()/processor state, since popScreen() right after is what
// actually closes the screen.
void D110SequencerRetroPanel::pressEnter() {
	if (stack.empty()) {
		// The transport field fires whatever UP/DOWN dialled up (see homeAdjust()) instead
		// of opening the menu - this is what makes STOP/PLAY/REC reachable from the 4
		// arrows + ENTER + EXIT alone, no mouse needed.
		if (homeField == HomeField::transport) {
			if (homeTransportChoice == 0) pressStop();
			else if (homeTransportChoice == 1) pressPlay();
			else pressRec();
			return;
		}
		pushScreen(buildMainMenu());
		return;
	}
	const ScreenKind kind = stack.back().kind;
	if (kind == ScreenKind::list) {
		auto items = stack.back().buildItems();
		const int listCursor = stack.back().cursor;
		if (listCursor < 0 || listCursor >= (int) items.size()) return;
		auto &item = items[(size_t) listCursor];
		if (item.enabled && item.onEnter) item.onEnter();
		repaint();
		return;
	}
	if (kind == ScreenKind::form) {
		auto onConfirm = stack.back().onConfirm;
		if (onConfirm) onConfirm();
		popScreen();
		return;
	}
	if (kind == ScreenKind::confirm) {
		const bool yes = stack.back().confirmYes;
		auto onConfirm = stack.back().onConfirm;
		if (yes && onConfirm) onConfirm();
		popScreen();
		return;
	}
	if (kind == ScreenKind::nameEdit) {
		nameEditCommit();
		return;
	}
}

void D110SequencerRetroPanel::pressExit() {
	if (!stack.empty()) popScreen();
}

void D110SequencerRetroPanel::syncHomeTransportChoice() {
	auto &eng = engine();
	homeTransportChoice = eng.isRecording() ? 2 : eng.isPlaying() ? 1 : 0;
}

void D110SequencerRetroPanel::homeAdjust(int delta) {
	auto &eng = engine();
	switch (homeField) {
		case HomeField::transport:
			homeTransportChoice = (homeTransportChoice + delta + 3) % 3;
			break;
		case HomeField::track: {
			const int count = juce::jmax(1, eng.activeTrackCount());
			homeSelectedTrack = (homeSelectedTrack + delta + count) % count;
			break;
		}
		case HomeField::bar:
			eng.gotoBar(juce::jmax(1, eng.getCurrentBar() + delta));
			break;
		case HomeField::tempo:
			eng.setTempo(eng.getTempo() + delta);
			break;
		case HomeField::slot: {
			constexpr int n = D110SequencerEngine::kNumSongSlots;
			eng.selectSongSlot((eng.getCurrentSongSlot() + delta + n) % n);
			break;
		}
	}
	repaint();
}

void D110SequencerRetroPanel::visibilityChanged() {
	if (isVisible()) grabKeyboardFocus();
}

bool D110SequencerRetroPanel::keyPressed(const juce::KeyPress &key) {
	if (key.isKeyCode(juce::KeyPress::leftKey)) { pressLeft(); return true; }
	if (key.isKeyCode(juce::KeyPress::rightKey)) { pressRight(); return true; }
	if (key.isKeyCode(juce::KeyPress::upKey)) { pressUp(); return true; }
	if (key.isKeyCode(juce::KeyPress::downKey)) { pressDown(); return true; }
	if (key.isKeyCode(juce::KeyPress::returnKey)) { pressEnter(); return true; } // Enter and Return both map here
	if (key.isKeyCode(juce::KeyPress::backspaceKey)) { pressExit(); return true; }
	return false;
}

void D110SequencerRetroPanel::mouseDown(const juce::MouseEvent &e) {
	grabKeyboardFocus();
	const auto p = e.position;
	if (stopBounds.contains(p)) { pressStop(); return; }
	if (playBounds.contains(p)) { pressPlay(); return; }
	if (recBounds.contains(p)) { pressRec(); return; }
	if (upBounds.contains(p)) { pressUp(); return; }
	if (downBounds.contains(p)) { pressDown(); return; }
	if (leftBounds.contains(p)) { pressLeft(); return; }
	if (rightBounds.contains(p)) { pressRight(); return; }
	if (enterBounds.contains(p)) { pressEnter(); return; }
	if (exitBounds.contains(p)) { pressExit(); return; }
}

// ---------------------------------------------------------------------------
// Screen builders
// ---------------------------------------------------------------------------

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildMainMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "MAIN MENU";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		items.push_back({ "TRACK", "", true, [this] { pushScreen(buildTrackList()); } });
		items.push_back({ "TRANSPORT", "", true, [this] { pushScreen(buildTransportMenu()); } });
		items.push_back({ "RECORD", "", true, [this] { pushScreen(buildRecordMenu()); } });
		items.push_back({ "SONG", "", true, [this] { pushScreen(buildSongMenu()); } });
		items.push_back({ "FILE", "", true, [this] { pushScreen(buildFileMenu()); } });
		auto &eng = engine();
		items.push_back({ "UNDO", eng.canUndo() ? "" : "(none)", eng.canUndo(), [this] {
			                 engine().undo();
			                 repaint();
		                 } });
		if (processor.supportsExtraTracks()) {
			const bool on = eng.getExtraTracksEnabled();
			items.push_back({ "EXTRA TRACKS", on ? "ON" : "OFF", true, [this] {
				                 auto &e = engine();
				                 e.setExtraTracksEnabled(!e.getExtraTracksEnabled());
				                 repaint();
			                 } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTrackList() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "TRACKS";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		auto &eng = engine();
		for (int t = 0; t < eng.activeTrackCount(); ++t) {
			juce::String name = eng.getTrackName(t);
			if (name.isEmpty()) name = defaultTrackLabel(t);
			juce::String flagsText;
			if (eng.isTrackMuted(t)) flagsText += "M";
			if (eng.isTrackSoloed(t)) flagsText += "S";
			if (eng.getArmedTrack() == t) flagsText += "A";
			items.push_back({ name, flagsText, true, [this, t] { pushScreen(buildTrackMenu(t)); } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTrackMenu(int track) {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = defaultTrackLabel(track);
	s.buildItems = [this, track]() {
		std::vector<ListItem> items;
		auto &eng = engine();
		items.push_back({ "MUTE", eng.isTrackMuted(track) ? "ON" : "OFF", true, [this, track] {
			                 auto &e = engine();
			                 e.setTrackMuted(track, !e.isTrackMuted(track));
			                 repaint();
		                 } });
		items.push_back({ "SOLO", eng.isTrackSoloed(track) ? "ON" : "OFF", true, [this, track] {
			                 auto &e = engine();
			                 e.setTrackSoloed(track, !e.isTrackSoloed(track));
			                 repaint();
		                 } });
		items.push_back({ "ARM", eng.getArmedTrack() == track ? "ON" : "OFF", true, [this, track] {
			                 auto &e = engine();
			                 e.armTrack(e.getArmedTrack() == track ? -1 : track);
			                 repaint();
		                 } });
		items.push_back({ "RENAME", "", true, [this, track] { startNameEdit(track); } });
		if (processor.supportsTrackChannelEdit())
			items.push_back({ "CHANNEL", juce::String(eng.channelForTrack(track)), true,
			                   [this, track] { pushScreen(buildChannelForm(track)); } });
		items.push_back({ "QUANTIZE", "", true, [this, track] { pushScreen(buildQuantizeMenu(track)); } });
		if (processor.supportsProgramChangeForTrack(track)) {
			const int program = processor.getTrackProgram(track);
			items.push_back({ "PROGRAM CHG", program < 0 ? "OFF" : juce::String(program + 1), true,
			                   [this, track] { pushScreen(buildProgramForm(track)); } });
		}
		items.push_back(
			{ "CLEAR TRACK", "", eng.trackHasEvents(track), [this, track] { pushScreen(buildClearConfirm(track)); } });
		items.push_back(
			{ "DELETE BARS", "", eng.getBarCount() >= 1, [this, track] { pushScreen(buildDeleteBarsForm(track)); } });
		items.push_back(
			{ "COPY BARS", "", eng.trackHasEvents(track), [this, track] { pushScreen(buildCopyBarsForm(track)); } });
		items.push_back(
			{ "TRANSPOSE", "", eng.trackHasEvents(track), [this, track] { pushScreen(buildTransposeForm(track)); } });
		items.push_back({ "EDIT EVENTS", "", true, [this, track] { pushScreen(buildEventList(track)); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildQuantizeMenu(int track) {
	using d110seq::QuantizeGrid;
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "QUANTIZE";
	s.buildItems = [this, track]() {
		static const std::array<std::pair<QuantizeGrid, const char *>, 7> presets { {
			{ QuantizeGrid::off, "OFF" }, { QuantizeGrid::quarter, "1/4" }, { QuantizeGrid::eighth, "1/8" },
			{ QuantizeGrid::sixteenth, "1/16" }, { QuantizeGrid::eighthTriplet, "1/8 T" },
			{ QuantizeGrid::sixteenthTriplet, "1/16 T" }, { QuantizeGrid::thirtySecond, "1/32" } } };
		const auto current = engine().getTrackQuantize(track);
		std::vector<ListItem> items;
		for (const auto &preset : presets) {
			const QuantizeGrid grid = preset.first;
			items.push_back({ preset.second, grid == current ? "*" : "", true, [this, track, grid] {
				                 engine().pushUndoSnapshot();
				                 engine().quantizeTrack(track, grid);
				                 popScreen();
			                 } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildChannelForm(int track) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "CHANNEL";
	auto value = std::make_shared<int>(engine().channelForTrack(track));
	s.fields.push_back({ "CH", value, 1, 16, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, track, value] { processor.setTrackChannel(track, *value); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildProgramForm(int track) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "PROGRAM CHG";
	const int currentProgram = processor.getTrackProgram(track);
	auto program = std::make_shared<int>(currentProgram >= 0 ? currentProgram + 1 : 0);
	auto bank = std::make_shared<int>(processor.getTrackBank(track));
	s.fields.push_back(
		{ "PRG(0=OFF)", program, 0, 128, [](int v) { return v == 0 ? juce::String("OFF") : juce::String(v); } });
	s.fields.push_back({ "BANK", bank, 1, 128, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, track, program, bank] {
		processor.setTrackProgram(track, *program == 0 ? -1 : *program - 1);
		processor.setTrackBank(track, *bank);
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildClearConfirm(int track) {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "CLEAR TRACK?";
	s.message = "ERASES ALL NOTES ON " + defaultTrackLabel(track);
	s.onConfirm = [this, track] {
		engine().pushUndoSnapshot();
		engine().clearTrack(track);
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildDeleteBarsForm(int track) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = track < 0 ? "DEL BARS(ALL)" : "DELETE BARS";
	auto &eng = engine();
	auto from = std::make_shared<int>(eng.getCurrentBar());
	auto to = std::make_shared<int>(eng.getCurrentBar());
	s.fields.push_back({ "FROM", from, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "TO", to, 1, 9999, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, track, from, to] {
		if (*to >= *from) {
			engine().pushUndoSnapshot();
			engine().deleteBars(track, *from, *to);
		}
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildCopyBarsForm(int track) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = track < 0 ? "COPY BARS(ALL)" : "COPY BARS";
	auto &eng = engine();
	auto from = std::make_shared<int>(eng.getCurrentBar());
	auto to = std::make_shared<int>(eng.getCurrentBar());
	auto dest = std::make_shared<int>(eng.getBarCount() + 1);
	s.fields.push_back({ "FROM", from, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "TO", to, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "DEST BAR", dest, 1, 9999, [](int v) { return juce::String(v); } });
	std::shared_ptr<int> destTrack;
	if (track >= 0) {
		destTrack = std::make_shared<int>(track);
		const int maxTrack = juce::jmax(0, eng.activeTrackCount() - 1);
		s.fields.push_back(
			{ "DEST TRK", destTrack, 0, maxTrack, [](int v) { return D110SequencerRetroPanel::defaultTrackLabel(v); } });
	}
	s.onConfirm = [this, track, from, to, dest, destTrack] {
		if (*to >= *from && *dest >= 1) {
			engine().pushUndoSnapshot();
			engine().copyBars(track, track >= 0 ? *destTrack : -1, *from, *to, *dest);
		}
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTransposeForm(int track) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = track < 0 ? "TRANSPOSE(ALL)" : "TRANSPOSE";
	auto &eng = engine();
	auto from = std::make_shared<int>(eng.getCurrentBar());
	auto to = std::make_shared<int>(eng.getCurrentBar());
	auto semitones = std::make_shared<int>(0);
	s.fields.push_back({ "FROM", from, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "TO", to, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back(
		{ "SEMITONES", semitones, -127, 127, [](int v) { return (v > 0 ? juce::String("+") : juce::String()) + juce::String(v); } });
	s.onConfirm = [this, track, from, to, semitones] {
		if (*to >= *from && *semitones != 0) {
			engine().pushUndoSnapshot();
			engine().transposeBars(track, *from, *to, *semitones);
		}
	};
	return s;
}

// Not bar-navigable in place, unlike D110SequencerPanel::promptForEventList()'s own
// "< Bar N >" strip - operates on whatever bar HOME was on when TRACK > EDIT EVENTS was
// entered. A deliberate v1 simplification: EXIT back to TRACK MENU, change HOME's bar
// quick-field, re-enter EDIT EVENTS for a different bar.
D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildEventList(int track) {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "EVT BAR " + juce::String(engine().getCurrentBar());
	const int bar = engine().getCurrentBar();
	s.buildItems = [this, track, bar]() {
		std::vector<ListItem> items;
		for (const auto &ev : engine().eventsInBarRange(track, bar, bar)) {
			const juce::String label = juce::MidiMessage::getMidiNoteName(ev.note, true, true, 4);
			const juce::String value = "v" + juce::String(ev.velocity);
			const int idx = ev.index;
			const int note = ev.note;
			items.push_back({ label, value, true, [this, track, idx, note] { pushScreen(buildEventItemMenu(track, idx, note)); } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildEventItemMenu(int track, int eventIndex, int note) {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = juce::MidiMessage::getMidiNoteName(note, true, true, 4);
	s.buildItems = [this, track, eventIndex, note]() {
		std::vector<ListItem> items;
		items.push_back({ "EDIT PITCH", "", true,
		                   [this, track, eventIndex, note] { pushScreen(buildEventPitchForm(track, eventIndex, note)); } });
		items.push_back(
			{ "DELETE", "", true, [this, track, eventIndex] { pushScreen(buildEventDeleteConfirm(track, eventIndex)); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildEventPitchForm(int track, int eventIndex, int note) {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "EDIT PITCH";
	auto value = std::make_shared<int>(note);
	s.fields.push_back({ "NOTE", value, 0, 127, [](int v) { return juce::MidiMessage::getMidiNoteName(v, true, true, 4); } });
	s.onConfirm = [this, track, eventIndex, value] {
		engine().pushUndoSnapshot();
		engine().setNoteEventPitch(track, eventIndex, *value);
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildEventDeleteConfirm(int track, int eventIndex) {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "DELETE NOTE?";
	s.message = "REMOVES THIS NOTE EVENT";
	s.onConfirm = [this, track, eventIndex] {
		engine().pushUndoSnapshot();
		engine().deleteNoteEvent(track, eventIndex);
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTransportMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "TRANSPORT";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		items.push_back({ "TEMPO", juce::String(eng.getTempo(), 0) + "BPM", true, [this] { pushScreen(buildTempoForm()); } });
		items.push_back({ "TIME SIG", juce::String(eng.getTimeSigNumerator()) + "/" + juce::String(eng.getTimeSigDenominator()),
		                   true, [this] { pushScreen(buildTimeSigMenu()); } });
		items.push_back(
			{ "METRONOME", eng.getMetronomeEnabled() ? "ON" : "OFF", true, [this] { pushScreen(buildMetronomeMenu()); } });
		items.push_back({ "PRECOUNT", juce::String(eng.getPrecountBars()), true, [this] { pushScreen(buildPrecountForm()); } });
		items.push_back({ "LOOP", loopModeShortLabel(eng.getLoopMode()), true, [this] { pushScreen(buildLoopMenu()); } });
		items.push_back({ "BAR", "", true, [this] { pushScreen(buildBarMenu()); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTempoForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "TEMPO";
	auto value = std::make_shared<int>((int) std::lround(engine().getTempo()));
	s.fields.push_back({ "BPM", value, 20, 300, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, value] { engine().setTempo((double) *value); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTimeSigMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "TIME SIG";
	s.buildItems = [this]() {
		static const std::array<std::pair<int, int>, 6> presets { { { 4, 4 }, { 3, 4 }, { 6, 8 }, { 2, 4 }, { 5, 4 },
		                                                              { 7, 8 } } };
		auto &eng = engine();
		std::vector<ListItem> items;
		for (const auto &preset : presets) {
			const bool current = preset.first == eng.getTimeSigNumerator() && preset.second == eng.getTimeSigDenominator();
			const int num = preset.first, den = preset.second;
			items.push_back({ juce::String(num) + "/" + juce::String(den), current ? "*" : "", true, [this, num, den] {
				                 engine().setTimeSignature(num, den);
				                 popScreen();
			                 } });
		}
		items.push_back({ "CUSTOM...", "", true, [this] { pushScreen(buildTimeSigCustomForm()); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildTimeSigCustomForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "CUSTOM SIG";
	auto num = std::make_shared<int>(engine().getTimeSigNumerator());
	auto den = std::make_shared<int>(engine().getTimeSigDenominator());
	s.fields.push_back({ "NUM", num, 1, 32, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "DEN", den, 1, 32, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, num, den] { engine().setTimeSignature(*num, *den); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildMetronomeMenu() {
	using d110seq::MetronomeMode;
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "METRONOME";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		items.push_back({ "METRO", eng.getMetronomeEnabled() ? "ON" : "OFF", true, [this] {
			                 auto &e = engine();
			                 e.setMetronomeEnabled(!e.getMetronomeEnabled());
			                 repaint();
		                 } });
		const auto mode = eng.getMetronomeMode();
		items.push_back({ "MODE", mode == MetronomeMode::visualOnly ? "VISUAL" : mode == MetronomeMode::audioOnly ? "AUDIO" : "BOTH",
		                   true, [this] {
			                   auto &e = engine();
			                   switch (e.getMetronomeMode()) {
				                   case MetronomeMode::visualOnly: e.setMetronomeMode(MetronomeMode::audioOnly); break;
				                   case MetronomeMode::audioOnly: e.setMetronomeMode(MetronomeMode::both); break;
				                   case MetronomeMode::both: e.setMetronomeMode(MetronomeMode::visualOnly); break;
			                   }
			                   repaint();
		                   } });
		items.push_back({ "USE CH10", eng.getMetronomeUseChannel10() ? "ON" : "OFF", true, [this] {
			                 auto &e = engine();
			                 e.setMetronomeUseChannel10(!e.getMetronomeUseChannel10());
			                 repaint();
		                 } });
		items.push_back({ "REC ONLY", eng.getMetronomeRecordOnly() ? "ON" : "OFF", true, [this] {
			                 auto &e = engine();
			                 e.setMetronomeRecordOnly(!e.getMetronomeRecordOnly());
			                 repaint();
		                 } });
		items.push_back({ "VOLUME", juce::String((int) std::lround(eng.getMetronomeVolume() * 100.0f)) + "%", true,
		                   [this] { pushScreen(buildMetronomeVolumeMenu()); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildMetronomeVolumeMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "METRO VOL";
	s.buildItems = [this]() {
		static constexpr float presets[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f };
		const float current = engine().getMetronomeVolume();
		std::vector<ListItem> items;
		for (float v : presets) {
			const bool selected = std::abs(current - v) < 0.01f;
			items.push_back({ juce::String((int) std::lround(v * 100.0f)) + "%", selected ? "*" : "", true, [this, v] {
				                 engine().setMetronomeVolume(v);
				                 popScreen();
			                 } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildPrecountForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "PRECOUNT";
	auto value = std::make_shared<int>(engine().getPrecountBars());
	s.fields.push_back({ "BARS", value, 0, 2, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, value] { engine().setPrecountBars(*value); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildLoopMenu() {
	using d110seq::LoopMode;
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "LOOP";
	s.buildItems = [this]() {
		const auto current = engine().getLoopMode();
		std::vector<ListItem> items;
		items.push_back({ "OFF", current == LoopMode::off ? "*" : "", true, [this] {
			                 engine().setLoopMode(LoopMode::off);
			                 popScreen();
		                 } });
		items.push_back({ "BAR", current == LoopMode::bar ? "*" : "", true, [this] {
			                 engine().setLoopMode(LoopMode::bar);
			                 popScreen();
		                 } });
		items.push_back({ "PUNCH", current == LoopMode::punch ? "*" : "", true, [this] {
			                 engine().setLoopMode(LoopMode::punch);
			                 popScreen();
		                 } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildBarMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "BAR";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		items.push_back({ "GO TO BAR", juce::String(eng.getCurrentBar()), true, [this] { pushScreen(buildGotoBarForm()); } });
		items.push_back({ "PUNCH IN HERE", "bar " + juce::String(eng.getCurrentBar()), true, [this] {
			                 engine().setPunchIn(engine().getCurrentBar());
			                 repaint();
		                 } });
		items.push_back({ "PUNCH OUT HERE", "bar " + juce::String(eng.getCurrentBar()), true, [this] {
			                 engine().setPunchOut(engine().getCurrentBar());
			                 repaint();
		                 } });
		items.push_back({ "PUNCH RANGE", juce::String(eng.getPunchIn()) + "-" + juce::String(eng.getPunchOut()), true,
		                   [this] { pushScreen(buildPunchForm()); } });
		items.push_back({ "DELETE BARS", "(all tracks)", true, [this] { pushScreen(buildDeleteBarsForm(-1)); } });
		items.push_back({ "COPY BARS", "(all tracks)", true, [this] { pushScreen(buildCopyBarsForm(-1)); } });
		items.push_back({ "TRANSPOSE", "(all tracks)", true, [this] { pushScreen(buildTransposeForm(-1)); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildGotoBarForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "GO TO BAR";
	auto value = std::make_shared<int>(engine().getCurrentBar());
	s.fields.push_back({ "BAR", value, 1, 9999, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, value] { engine().gotoBar(*value); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildPunchForm() {
	Screen s;
	s.kind = ScreenKind::form;
	s.title = "PUNCH RANGE";
	auto in = std::make_shared<int>(engine().getPunchIn());
	auto out = std::make_shared<int>(engine().getPunchOut());
	s.fields.push_back({ "IN", in, 1, 9999, [](int v) { return juce::String(v); } });
	s.fields.push_back({ "OUT", out, 1, 9999, [](int v) { return juce::String(v); } });
	s.onConfirm = [this, in, out] { engine().setPunchRange(*in, *out); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildRecordMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "RECORD";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		items.push_back(
			{ "REC MODE", recordModeShortLabel(engine().getRecordMode()), true, [this] { pushScreen(buildRecordModeMenu()); } });
		items.push_back({ "STEP REC", "", true, [this] { pushScreen(buildStepMenu()); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildRecordModeMenu() {
	using d110seq::RecordMode;
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "REC MODE";
	s.buildItems = [this]() {
		const auto current = engine().getRecordMode();
		std::vector<ListItem> items;
		items.push_back({ "OVERDUB", current == RecordMode::overdub ? "*" : "", true, [this] {
			                 engine().setRecordMode(RecordMode::overdub);
			                 popScreen();
		                 } });
		items.push_back({ "REPLACE", current == RecordMode::replaceRange ? "*" : "", true, [this] {
			                 engine().setRecordMode(RecordMode::replaceRange);
			                 popScreen();
		                 } });
		items.push_back({ "REPLACE+END", current == RecordMode::replaceToEnd ? "*" : "", true, [this] {
			                 engine().setRecordMode(RecordMode::replaceToEnd);
			                 popScreen();
		                 } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildStepMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "STEP REC";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		items.push_back({ "STEP REC", eng.isStepRecording() ? "ON" : "OFF", true, [this] {
			                 auto &e = engine();
			                 if (e.isStepRecording()) e.stopStepRecording();
			                 else if (e.getArmedTrack() >= 0) e.startStepRecording();
			                 repaint();
		                 } });
		items.push_back({ "DURATION", stepDurationShortLabel(eng.getStepDuration()), true,
		                   [this] { pushScreen(buildStepDurationMenu()); } });
		items.push_back({ "DOT", eng.getStepDotted() ? "ON" : "OFF", true, [this] {
			                 auto &e = engine();
			                 e.setStepDotted(!e.getStepDotted());
			                 repaint();
		                 } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildStepDurationMenu() {
	using d110seq::QuantizeGrid;
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "STEP DUR";
	s.buildItems = [this]() {
		static const std::array<QuantizeGrid, 8> presets { { QuantizeGrid::whole, QuantizeGrid::half, QuantizeGrid::quarter,
		                                                       QuantizeGrid::eighth, QuantizeGrid::sixteenth,
		                                                       QuantizeGrid::eighthTriplet, QuantizeGrid::sixteenthTriplet,
		                                                       QuantizeGrid::thirtySecond } };
		const auto current = engine().getStepDuration();
		std::vector<ListItem> items;
		for (auto grid : presets)
			items.push_back({ stepDurationShortLabel(grid), grid == current ? "*" : "", true, [this, grid] {
				                 engine().setStepDuration(grid);
				                 popScreen();
			                 } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSongMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "SONG";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		items.push_back(
			{ "SLOT", juce::String(engine().getCurrentSongSlot() + 1), true, [this] { pushScreen(buildSongSlotList()); } });
		items.push_back({ "COPY TO SLOT", "", true, [this] { pushScreen(buildSongCopyMenu()); } });
		items.push_back({ "NEW SONG", "", true, [this] { pushScreen(buildNewSongConfirm()); } });
		if (processor.supportsSoundSnapshots())
			items.push_back({ "SOUND SNAPSHOT", "", true, [this] { pushScreen(buildSnapshotMenu()); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSongSlotList() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "SELECT SLOT";
	s.buildItems = [this]() {
		auto &eng = engine();
		std::vector<ListItem> items;
		for (int slot = 0; slot < D110SequencerEngine::kNumSongSlots; ++slot) {
			juce::String value = eng.songSlotHasContent(slot) ? "*" : "";
			if (slot == eng.getCurrentSongSlot()) value += "<";
			items.push_back({ "SLOT " + juce::String(slot + 1), value, true, [this, slot] {
				                 engine().selectSongSlot(slot);
				                 popScreen();
			                 } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSongCopyMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "COPY SONG TO";
	s.buildItems = [this]() {
		auto &eng = engine();
		const int current = eng.getCurrentSongSlot();
		std::vector<ListItem> items;
		for (int slot = 0; slot < D110SequencerEngine::kNumSongSlots; ++slot) {
			if (slot == current) continue;
			const juce::String value = eng.songSlotHasContent(slot) ? "OVERWRITE" : "";
			items.push_back(
				{ "SLOT " + juce::String(slot + 1), value, true, [this, slot] { pushScreen(buildSongCopyConfirm(slot)); } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSongCopyConfirm(int destSlot) {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "COPY SONG?";
	s.message = "OVERWRITES SLOT " + juce::String(destSlot + 1);
	s.onConfirm = [this, destSlot] {
		engine().pushUndoSnapshot();
		engine().copyCurrentSongTo(destSlot);
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildNewSongConfirm() {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "NEW SONG?";
	s.message = "CLEARS EVERY TRACK IN THIS SLOT";
	s.onConfirm = [this] {
		engine().pushUndoSnapshot();
		engine().newSong();
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSnapshotMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "SOUND SNAP";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		for (int slot = 0; slot < D110SequencerEngine::kNumSongSlots; ++slot) {
			const bool has = processor.hasSoundSnapshot(slot);
			items.push_back(
				{ "SLOT " + juce::String(slot + 1), has ? "STORED" : "", true, [this, slot] { pushScreen(buildSnapshotSlotMenu(slot)); } });
		}
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSnapshotSlotMenu(int slot) {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "SLOT " + juce::String(slot + 1);
	s.buildItems = [this, slot]() {
		std::vector<ListItem> items;
		items.push_back({ "STORE HERE", "", true, [this, slot] {
			                 processor.storeSoundSnapshotForSlot(slot);
			                 popScreen();
		                 } });
		items.push_back({ "LOAD HERE", "", processor.hasSoundSnapshot(slot),
		                   [this, slot] { pushScreen(buildSnapshotLoadConfirm(slot)); } });
		return items;
	};
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildSnapshotLoadConfirm(int slot) {
	Screen s;
	s.kind = ScreenKind::confirm;
	s.title = "LOAD SOUNDS?";
	s.message = "REPLACES ALL CURRENT SOUNDS, POWER-CYCLES";
	s.onConfirm = [this, slot] { processor.loadSoundSnapshotForSlot(slot); };
	return s;
}

D110SequencerRetroPanel::Screen D110SequencerRetroPanel::buildFileMenu() {
	Screen s;
	s.kind = ScreenKind::list;
	s.title = "FILE";
	s.buildItems = [this]() {
		std::vector<ListItem> items;
		items.push_back({ "LOAD SONG", ".MID", true, [this] { doLoadSong(); popScreen(); } });
		items.push_back({ "SAVE SONG", ".MID", true, [this] { doSaveSong(); popScreen(); } });
		items.push_back({ "LOAD ALL", ".MIDISEQ", true, [this] { doLoadAllSongs(); popScreen(); } });
		items.push_back({ "SAVE ALL", ".MIDISEQ", true, [this] { doSaveAllSongs(); popScreen(); } });
		return items;
	};
	return s;
}

// ---------------------------------------------------------------------------
// Character-wheel rename
// ---------------------------------------------------------------------------

void D110SequencerRetroPanel::startNameEdit(int track) {
	nameEditTrack = track;
	juce::String current = engine().getTrackName(track);
	if (current.isEmpty()) current = defaultTrackLabel(track);
	nameEditBuffer = current.toUpperCase().substring(0, kNameEditLength);
	while (nameEditBuffer.length() < kNameEditLength) nameEditBuffer += " ";
	nameEditCaret = 0;
	Screen s;
	s.kind = ScreenKind::nameEdit;
	s.title = "RENAME";
	pushScreen(s);
}

void D110SequencerRetroPanel::nameEditAdjust(int delta) {
	if (nameEditCaret < 0 || nameEditCaret >= nameEditBuffer.length()) return;
	int idx = kNameEditCharset.indexOfChar(nameEditBuffer[nameEditCaret]);
	if (idx < 0) idx = 0;
	idx = (idx + delta + kNameEditCharset.length()) % kNameEditCharset.length();
	nameEditBuffer = nameEditBuffer.replaceSection(nameEditCaret, 1, juce::String::charToString(kNameEditCharset[idx]));
	repaint();
}

void D110SequencerRetroPanel::nameEditMoveCaret(int delta) {
	nameEditCaret = juce::jlimit(0, kNameEditLength - 1, nameEditCaret + delta);
	repaint();
}

void D110SequencerRetroPanel::nameEditCommit() {
	engine().pushUndoSnapshot();
	engine().setTrackName(nameEditTrack, nameEditBuffer.trim());
	popScreen();
}

// ---------------------------------------------------------------------------
// File dialogs - same juce::FileChooser calls as D110SequencerPanel's own LOAD/SAVE
// handlers (see D110SequencerPanel::mouseDown()/showLoadMenu()/showSaveMenu()).
// ---------------------------------------------------------------------------

void D110SequencerRetroPanel::doLoadSong() {
	auto *chooser = new juce::FileChooser("Load a MIDI file into the sequencer", processor.getLastDialogDir(), "*.mid");
	chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
	                      [this, chooser](const juce::FileChooser &fc) {
		                      const auto file = fc.getResult();
		                      if (file != juce::File()) {
			                      processor.setLastDialogDir(file.getParentDirectory());
			                      engine().loadMidiFile(file);
		                      }
		                      delete chooser;
		                      repaint();
	                      });
}

void D110SequencerRetroPanel::doSaveSong() {
	auto *chooser = new juce::FileChooser("Save the sequencer as a MIDI file", processor.getLastDialogDir(), "*.mid");
	chooser->launchAsync(
		juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
			| juce::FileBrowserComponent::warnAboutOverwriting,
		[this, chooser](const juce::FileChooser &fc) {
			auto file = fc.getResult();
			if (file != juce::File()) {
				if (!file.hasFileExtension("mid")) file = file.withFileExtension("mid");
				processor.setLastDialogDir(file.getParentDirectory());
				engine().saveMidiFile(file);
			}
			delete chooser;
		});
}

void D110SequencerRetroPanel::doLoadAllSongs() {
	auto *chooser =
		new juce::FileChooser("Load all 4 sequencer songs", processor.getLastDialogDir(), "*.midiseq;*.d110songs");
	chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
	                      [this, chooser](const juce::FileChooser &fc) {
		                      const auto file = fc.getResult();
		                      if (file != juce::File()) {
			                      processor.setLastDialogDir(file.getParentDirectory());
			                      processor.importSequencerSongs(file);
		                      }
		                      delete chooser;
		                      repaint();
	                      });
}

void D110SequencerRetroPanel::doSaveAllSongs() {
	auto *chooser = new juce::FileChooser("Save all 4 sequencer songs", processor.getLastDialogDir(), "*.midiseq");
	chooser->launchAsync(
		juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
			| juce::FileBrowserComponent::warnAboutOverwriting,
		[this, chooser](const juce::FileChooser &fc) {
			auto file = fc.getResult();
			if (file != juce::File()) {
				if (!file.hasFileExtension("midiseq")) file = file.withFileExtension("midiseq");
				processor.setLastDialogDir(file.getParentDirectory());
				processor.exportSequencerSongs(file);
			}
			delete chooser;
		});
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void D110SequencerRetroPanel::resized() {
	auto area = getLocalBounds().toFloat();
	if (area.getWidth() < 1.0f || area.getHeight() < 1.0f) return;

	lcdBounds = area.removeFromTop(area.getHeight() * 0.36f).reduced(8.0f, 5.0f);
	area.removeFromTop(3.0f);

	// row1 (STOP/PLAY/REC) is one button tall; row2 holds the 3-row-tall D-pad cross, so
	// it needs roughly 3x row1's share to keep every button roughly square.
	auto row1 = area.removeFromTop(area.getHeight() * 0.25f).reduced(8.0f, 2.0f);
	auto row2 = area.reduced(8.0f, 2.0f);

	const float w1 = row1.getWidth() / 3.0f;
	stopBounds = row1.removeFromLeft(w1).reduced(3.0f);
	playBounds = row1.removeFromLeft(w1).reduced(3.0f);
	recBounds = row1.reduced(3.0f);

	// EXIT is its own button on the left; UP/DOWN/LEFT/RIGHT form a cross-shaped D-pad on
	// the right with ENTER at its centre (Alan's approved layout).
	exitBounds = row2.removeFromLeft(row2.getWidth() / 4.0f).reduced(3.0f);
	auto cross = row2;
	const float colW = cross.getWidth() / 3.0f;
	auto colLeft = cross.removeFromLeft(colW);
	auto colMid = cross.removeFromLeft(colW);
	auto colRight = cross;
	const float subH = colMid.getHeight() / 3.0f;
	upBounds = colMid.removeFromTop(subH).reduced(3.0f);
	downBounds = colMid.removeFromBottom(subH).reduced(3.0f);
	enterBounds = colMid.reduced(3.0f); // whatever's left in the middle column is the middle row
	auto midRow = [subH](juce::Rectangle<float> col) {
		col.removeFromTop(subH);
		col.removeFromBottom(subH);
		return col;
	};
	leftBounds = midRow(colLeft).reduced(3.0f);
	rightBounds = midRow(colRight).reduced(3.0f);
}

void D110SequencerRetroPanel::paint(juce::Graphics &g) {
	const auto &pal = d110ui::palette();
	g.fillAll(pal.panelBg);
	paintLcd(g, lcdBounds);
	paintButtons(g);
}

void D110SequencerRetroPanel::paintButtons(juce::Graphics &g) {
	auto &eng = engine();
	paintRetroButton(g, stopBounds, "STOP", !eng.isPlaying());
	paintRetroButton(g, playBounds, "PLAY", eng.isPlaying() && !eng.isRecording());
	paintRetroButton(g, recBounds, "REC", eng.isRecording());
	paintRetroButton(g, upBounds, juce::String::fromUTF8("\xE2\x96\xB2"), false);   // ▲
	paintRetroButton(g, downBounds, juce::String::fromUTF8("\xE2\x96\xBC"), false); // ▼
	paintRetroButton(g, leftBounds, juce::String::fromUTF8("\xE2\x97\x80"), false); // ◀
	paintRetroButton(g, rightBounds, juce::String::fromUTF8("\xE2\x96\xB6"), false); // ▶
	paintRetroButton(g, enterBounds, "ENTER", false);
	paintRetroButton(g, exitBounds, "EXIT", false);
}

void D110SequencerRetroPanel::paintLcd(juce::Graphics &g, juce::Rectangle<float> area) {
	const auto &pal = d110ui::palette();
	g.setColour(pal.box);
	g.fillRoundedRectangle(area, 4.0f);
	g.setColour(pal.boxBorder);
	g.drawRoundedRectangle(area, 4.0f, 1.5f);

	auto inner = area.reduced(5.0f, 3.0f);
	if (inner.getWidth() <= 0.0f || inner.getHeight() <= 0.0f) return;

	// Fixed 20x4 character grid, like a real alphanumeric LCD - charPx (one glyph's
	// dot-matrix height) is picked so 20 columns and 4 rows both fit `inner`, whichever
	// is the tighter constraint, so the whole glass reads as one consistent grid instead
	// of proportionally-sized floating text.
	constexpr float kCols = 20.0f, kRows = 4.0f;
	const float charPx = juce::jmin(inner.getWidth() / kCols * (7.0f / 6.0f), inner.getHeight() / kRows * 0.85f);
	const float rowH = inner.getHeight() / kRows;

	if (stack.empty()) {
		paintHomeScreen(g, inner, charPx);
		return;
	}

	auto titleArea = inner.removeFromTop(rowH);
	g.setColour(pal.value);
	drawDotText(g, top().title, titleArea, juce::Justification::centredLeft, charPx);

	switch (top().kind) {
		case ScreenKind::list: paintListScreen(g, inner, charPx); break;
		case ScreenKind::form: paintFormScreen(g, inner, charPx); break;
		case ScreenKind::confirm: paintConfirmScreen(g, inner, charPx); break;
		case ScreenKind::nameEdit: paintNameEditScreen(g, inner, charPx); break;
	}
}

void D110SequencerRetroPanel::paintHomeScreen(juce::Graphics &g, juce::Rectangle<float> inner, float charPx) {
	const auto &pal = d110ui::palette();
	auto &eng = engine();
	const float lineH = inner.getHeight() / 4.0f;

	if (eng.isStepRecording()) {
		auto l0 = inner.removeFromTop(lineH);
		auto l1 = inner.removeFromTop(lineH);
		auto l2 = inner.removeFromTop(lineH);
		g.setColour(pal.seqActiveFill);
		drawDotText(g, "STEP RECORDING", l0, juce::Justification::centredLeft, charPx);
		g.setColour(pal.value);
		drawDotText(g, "BAR " + juce::String(eng.getStepBar()) + " STEP " + juce::String(eng.getStepIndexInBar()), l1,
		            juce::Justification::centredLeft, charPx);
		drawDotText(g, "DUR " + stepDurationShortLabel(eng.getStepDuration()) + (eng.getStepDotted() ? " DOT" : ""), l2,
		            juce::Justification::centredLeft, charPx);
		return;
	}

	auto l0 = inner.removeFromTop(lineH);
	auto l1 = inner.removeFromTop(lineH);
	auto l2 = inner.removeFromTop(lineH);

	// The transport field: selected, it shows whatever UP/DOWN has dialled up (fired by
	// ENTER - see pressEnter()/homeAdjust()); otherwise it's a plain live status readout,
	// same as before.
	static const juce::String kTransportLabels[3] = { "STOP", "PLAY", "REC" };
	const bool transportSelected = homeField == HomeField::transport;
	const juce::String transportText = transportSelected
	                                        ? kTransportLabels[(size_t) homeTransportChoice]
	                                        : (eng.isRecording() ? "REC" : eng.isPlaying() ? "PLAY" : "STOP");
	const juce::String barText = "BAR " + juce::String(eng.getCurrentBar()) + "/" + juce::String(eng.getBarCount());
	g.setColour(transportSelected ? pal.seqMetroDownbeat : pal.value);
	drawDotText(g, (transportSelected ? juce::String(">") : juce::String()) + transportText,
	            l0.removeFromLeft(l0.getWidth() * 0.3f), juce::Justification::centredLeft, charPx);
	g.setColour(homeField == HomeField::bar ? pal.seqMetroDownbeat : pal.value);
	drawDotText(g, (homeField == HomeField::bar ? juce::String(">") : juce::String()) + barText, l0,
	            juce::Justification::centredRight, charPx);

	juce::String trackName = eng.getTrackName(homeSelectedTrack);
	if (trackName.isEmpty()) trackName = defaultTrackLabel(homeSelectedTrack);
	juce::String flagsText;
	if (eng.isTrackMuted(homeSelectedTrack)) flagsText += "M";
	if (eng.isTrackSoloed(homeSelectedTrack)) flagsText += "S";
	if (eng.getArmedTrack() == homeSelectedTrack) flagsText += "A";
	g.setColour(homeField == HomeField::track ? pal.seqMetroDownbeat : pal.value);
	drawDotText(g,
	            (homeField == HomeField::track ? juce::String(">") : juce::String(" ")) + trackName + " CH"
	                + juce::String(eng.channelForTrack(homeSelectedTrack)) + " " + flagsText,
	            l1, juce::Justification::centredLeft, charPx);

	auto tempoArea = l2.removeFromLeft(l2.getWidth() * 0.5f);
	g.setColour(homeField == HomeField::tempo ? pal.seqMetroDownbeat : pal.value);
	drawDotText(g,
	            (homeField == HomeField::tempo ? juce::String(">") : juce::String(" ")) + juce::String(eng.getTempo(), 0)
	                + "BPM",
	            tempoArea, juce::Justification::centredLeft, charPx);
	g.setColour(homeField == HomeField::slot ? pal.seqMetroDownbeat : pal.value);
	drawDotText(g,
	            (homeField == HomeField::slot ? juce::String(">") : juce::String(" ")) + "SNG "
	                + juce::String(eng.getCurrentSongSlot() + 1),
	            l2, juce::Justification::centredLeft, charPx);
}

void D110SequencerRetroPanel::paintListScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx) {
	const auto &pal = d110ui::palette();
	auto &s = top();
	auto items = s.buildItems();
	const float lineH = textArea.getHeight() / 3.0f;

	int scroll = 0;
	if (!items.empty()) {
		s.cursor = juce::jlimit(0, (int) items.size() - 1, s.cursor);
		const int maxScroll = juce::jmax(0, (int) items.size() - 3);
		scroll = juce::jlimit(0, maxScroll, s.cursor - 1);
	}

	for (int row = 0; row < 3; ++row) {
		auto lineArea = textArea.removeFromTop(lineH);
		const int idx = scroll + row;
		if (idx < 0 || idx >= (int) items.size()) continue;
		const auto &it = items[(size_t) idx];
		const bool selected = idx == s.cursor;
		g.setColour(it.enabled ? (selected ? pal.seqMetroDownbeat : pal.value) : pal.value.withAlpha(0.35f));
		auto labelArea = lineArea.removeFromLeft(lineArea.getWidth() * 0.62f);
		drawDotText(g, (selected ? juce::String(">") : juce::String(" ")) + it.label, labelArea,
		            juce::Justification::centredLeft, charPx);
		drawDotText(g, it.value, lineArea, juce::Justification::centredRight, charPx);
	}
}

void D110SequencerRetroPanel::paintFormScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx) {
	const auto &pal = d110ui::palette();
	auto &s = top();
	if (!s.fields.empty()) s.cursor = juce::jlimit(0, (int) s.fields.size() - 1, s.cursor);
	const float lineH = textArea.getHeight() / 3.0f;

	for (int line = 0; line < 3; ++line) {
		auto lineArea = textArea.removeFromTop(lineH);
		for (int col = 0; col < 2; ++col) {
			const int idx = line * 2 + col;
			auto colArea = col == 0 ? lineArea.removeFromLeft(lineArea.getWidth() * 0.5f) : lineArea;
			if (idx >= (int) s.fields.size()) continue;
			const auto &f = s.fields[(size_t) idx];
			const bool selected = idx == s.cursor;
			const juce::String text = f.label + " " + f.format(*f.value);
			g.setColour(selected ? pal.seqMetroDownbeat : pal.value);
			drawDotText(g, (selected ? juce::String(">") : juce::String(" ")) + text, colArea,
			            juce::Justification::centredLeft, charPx);
		}
	}
}

void D110SequencerRetroPanel::paintConfirmScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx) {
	const auto &pal = d110ui::palette();
	auto &s = top();
	const float lineH = textArea.getHeight() / 3.0f;
	auto msgArea = textArea.removeFromTop(lineH * 2.0f);
	auto choiceArea = textArea;

	g.setColour(pal.value);
	drawDotFitted(g, s.message, msgArea, juce::Justification::centredLeft, 2, charPx);

	auto noArea = choiceArea.removeFromLeft(choiceArea.getWidth() * 0.5f);
	auto yesArea = choiceArea;
	g.setColour(!s.confirmYes ? pal.seqMetroDownbeat : pal.value.withAlpha(0.5f));
	drawDotText(g, !s.confirmYes ? ">NO" : " NO", noArea, juce::Justification::centredLeft, charPx);
	g.setColour(s.confirmYes ? pal.seqMetroDownbeat : pal.value.withAlpha(0.5f));
	drawDotText(g, s.confirmYes ? ">YES" : " YES", yesArea, juce::Justification::centredLeft, charPx);
}

void D110SequencerRetroPanel::paintNameEditScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx) {
	const auto &pal = d110ui::palette();
	const float lineH = textArea.getHeight() / 3.0f;
	auto nameArea = textArea.removeFromTop(lineH * 2.0f); // extra height for a clearer caret box

	const float charW = nameArea.getWidth() / float(kNameEditLength);
	for (int i = 0; i < kNameEditLength && i < nameEditBuffer.length(); ++i) {
		auto cell = nameArea.removeFromLeft(charW);
		const bool caret = i == nameEditCaret;
		if (caret) {
			g.setColour(pal.value);
			g.fillRect(cell.reduced(1.0f));
			g.setColour(pal.box);
		} else {
			g.setColour(pal.value);
		}
		drawDotText(g, juce::String::charToString(nameEditBuffer[i]), cell, juce::Justification::centred, charPx);
	}
}
