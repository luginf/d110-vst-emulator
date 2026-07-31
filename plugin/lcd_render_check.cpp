// Renders the LCD the way the panel does, at a few settings, so the result can be LOOKED
// AT next to docs/lcd_reference.png instead of judged from constants. Calibration tool:
// it deliberately duplicates the drawing loop from PluginEditor.cpp so the parameters can
// be swept without disturbing the plugin, and it is not part of the plugin's own render
// path. Whatever wins here gets copied back by hand.
//
// Usage: d110_lcd_check [output_dir]
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>

namespace {

// The panel's own geometry, from docs/panel_reference_notes.md.
constexpr float kLcdX = 604.0f, kLcdY = 95.0f, kLcdW = 247.0f, kLcdH = 60.0f;
constexpr float kCharX0 = 610.0f, kCellW = 14.408f;
constexpr float kLine0Y = 99.1f, kLineStep = 26.35f;
constexpr float kDotW = kCellW / 6.0f, kDotH = 2.700f;
constexpr int kCols = 16, kLines = 2, kDotRows = 7, kRowsPerChar = 8;

struct Settings {
	const char *name;
	int super;             // supersampling factor
	juce::Colour glass;    // lit field
	juce::Colour ink;      // dot colour
	float gapX, gapY;      // gap between dots, in offscreen pixels
	int outScale;          // how much to blow the result up for inspection
};

// A screen the real unit shows, so the shapes being judged are the real glyph shapes.
// Row 1 is the part-status row, row 2 a patch name. Built from a plain 5x7 table only
// because this tool has no CGROM; the plugin itself uses the machine's own mask ROM.
const char *kSample[2] = { "12345678R RomPly", "1:Macho Memory  " };

// Minimal 5x7 glyphs for the characters the sample uses. Rows are 5 bits, bit 4 leftmost,
// matching the MSM6222B convention the panel decodes.
struct Glyph { char c; juce::uint8 rows[7]; };
const Glyph kFont[] = {
	{' ', {0,0,0,0,0,0,0}},
	{'0', {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}},
	{'1', {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}},
	{'2', {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}},
	{'3', {0x1f,0x02,0x04,0x02,0x01,0x11,0x0e}},
	{'4', {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}},
	{'5', {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e}},
	{'6', {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e}},
	{'7', {0x1f,0x01,0x02,0x04,0x08,0x08,0x08}},
	{'8', {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}},
	{'9', {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}},
	{':', {0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00}},
	{'R', {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}},
	{'o', {0x00,0x00,0x0e,0x11,0x11,0x11,0x0e}},
	{'m', {0x00,0x00,0x1a,0x15,0x15,0x15,0x15}},
	{'P', {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}},
	{'l', {0x0c,0x04,0x04,0x04,0x04,0x04,0x0e}},
	{'y', {0x00,0x00,0x11,0x11,0x0f,0x01,0x0e}},
	{'M', {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}},
	{'a', {0x00,0x00,0x0e,0x01,0x0f,0x11,0x0f}},
	{'c', {0x00,0x00,0x0e,0x11,0x10,0x11,0x0e}},
	{'h', {0x10,0x10,0x16,0x19,0x11,0x11,0x11}},
	{'e', {0x00,0x00,0x0e,0x11,0x1f,0x10,0x0e}},
	{'r', {0x00,0x00,0x16,0x19,0x10,0x10,0x10}},
};

const juce::uint8 *glyphFor(char c) {
	for (const auto &g : kFont)
		if (g.c == c) return g.rows;
	return kFont[0].rows; // space
}

void render(const Settings &s, const juce::File &out) {
	const int w = int(kLcdW) * s.super, h = int(kLcdH) * s.super;
	juce::Image img(juce::Image::RGB, w, h, false);
	{
		juce::Graphics g(img);
		g.fillAll(s.glass);
		auto px = [&s](float panelX) { return (panelX - kLcdX) * s.super; };
		auto py = [&s](float panelY) { return (panelY - kLcdY) * s.super; };

		g.setColour(s.ink);
		for (int line = 0; line < kLines; ++line)
			for (int col = 0; col < kCols; ++col) {
				const juce::uint8 *rows = glyphFor(kSample[line][col]);
				for (int dy = 0; dy < kDotRows; ++dy)
					for (int dx = 0; dx < 5; ++dx) {
						if (!((rows[dy] >> (4 - dx)) & 1)) continue;
						g.fillRect(px(kCharX0 + col * kCellW + dx * kDotW),
						           py(kLine0Y + line * kLineStep + dy * kDotH),
						           kDotW * s.super - s.gapX, kDotH * s.super - s.gapY);
					}
			}
	}

	// Blow it up the way a large plugin window would, so the result is judged at the size
	// it is actually looked at rather than as a thumbnail.
	const int ow = int(kLcdW) * s.outScale, oh = int(kLcdH) * s.outScale;
	juce::Image big(juce::Image::RGB, ow, oh, false);
	{
		juce::Graphics g(big);
		g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
		g.drawImage(img, 0, 0, ow, oh, 0, 0, w, h, false);
	}

	juce::PNGImageFormat png;
	out.deleteFile();
	std::unique_ptr<juce::FileOutputStream> stream(out.createOutputStream());
	if (stream != nullptr) png.writeImageToStream(big, *stream);
	std::printf("  %-28s super=%d gap=%.1f/%.1f -> %s\n", s.name, s.super, s.gapX, s.gapY,
	            out.getFileName().toRawUTF8());
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const juce::File dir = (argc > 1) ? juce::File(juce::String(argv[1]))
	                                  : juce::File::getCurrentWorkingDirectory();
	dir.createDirectory();
	std::printf("writing to %s\n", dir.getFullPathName().toRawUTF8());

	// Current shipping values, for comparison.
	const juce::Colour glassNow(0xff3e7515), inkNow(0xff0a2e05);
	// Candidate: the photo was taken under room light with the camera stopped down, so its
	// sampled green is far darker than the eye sees on a lit module. Brighter field, deeper
	// ink, and finer gaps for a crisper dot.
	const juce::Colour glassBright(0xff6ab81f), inkDeep(0xff05230a);

	render({"current (photo-sampled)", 4, glassNow,    inkNow,  1.9f, 2.1f, 4}, dir.getChildFile("lcd_1_current.png"));
	render({"same, 8x supersample",    8, glassNow,    inkNow,  3.8f, 4.2f, 4}, dir.getChildFile("lcd_2_super8.png"));
	render({"brighter + deeper ink",   8, glassBright, inkDeep, 3.8f, 4.2f, 4}, dir.getChildFile("lcd_3_bright.png"));
	render({"brighter, tighter dots",  8, glassBright, inkDeep, 2.0f, 2.2f, 4}, dir.getChildFile("lcd_4_tight.png"));

	std::printf("done\n");
	return 0;
}
