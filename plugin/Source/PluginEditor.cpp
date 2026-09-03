#include "PluginEditor.h"
#include "UiTheme.h"
#include <BinaryData.h>

// Only used standalone (see showOptionsMenu's cases 900-903) - guarded like this because the
// header assumes juce_audio_utils' AudioDeviceSelectorComponent/AudioProcessorPlayer are
// already visible, which is only true in the Standalone format's own compile of this file (the
// VST3 format target compiles PluginEditor.cpp too, without that module pulled in).
#include <juce_core/system/juce_TargetPlatform.h>
#if JucePlugin_Build_Standalone
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

#include <cmath>
#include <cstring>

namespace {

// Defined near ImagePopupWindow, further down this file - forward-declared here so both
// D110Panel::showOptionsMenu() (above that point in the file) and D110EditorPane's own
// UTILITY tab button can call it.
void showLaReferencePopup(int windowWidth);

#if JucePlugin_Build_Standalone
// Shared by D110Panel's right-click menu and D110EditorPane's OPTIONS button (see
// D110Panel::showOptionsMenu and D110EditorPane::showOptionsButtonMenu) - action is one of
// the 900-903 IDs both menus use for these four items.
void performStandaloneAppAction(juce::Component &anchor, int action) {
	if (auto *win = dynamic_cast<juce::StandaloneFilterWindow *>(anchor.getTopLevelComponent())) {
		if (action == 900) win->getPluginHolder()->showAudioSettingsDialog();
		else if (action == 901) win->getPluginHolder()->askUserToSaveState();
		else if (action == 902) win->getPluginHolder()->askUserToLoadState();
		else if (action == 903) win->resetToDefaultState();
	}
}
#endif

// --- geometry, all in the reference photograph's own pixels -------------------
// Measured by profiling the image rather than by eye; every number below is
// derived and justified in docs/panel_reference_notes.md.

// The LCD window, after the asset's opening was reshaped to a real module's
// 4.11:1 proportions. The live render fills exactly this rectangle.
constexpr float kLcdX = 604.0f, kLcdY = 95.0f, kLcdW = 247.0f, kLcdH = 60.0f;

// 16x2 character grid inside that window. Everything here is the real hardware's
// geometry (photographed in docs/lcd_reference.png) scaled by 247/300 = 0.82333,
// so the glyphs keep the proportions they have on the actual glass.
constexpr float kCharX0 = 610.0f;   // left edge of column 0
constexpr float kCellW = 14.408f;   // 17.5 px on the real module
constexpr float kLine0Y = 99.1f;    // top of line 1's dot rows
constexpr float kLineStep = 26.35f; // line 1 -> line 2
constexpr int kCols = D110CoreType::kCols;
constexpr int kLines = D110CoreType::kLines;

// A character cell fills its width in 6 dot columns (5 dots + 1 gap), so the
// horizontal pitch follows from the cell. The vertical pitch does NOT follow
// from the line pitch - a real module leaves a wider gap between rows of
// characters than between dot rows inside one. It was measured off a full-height
// glyph instead: R's stem spans 23 px over 7 rows on the reference photo.
constexpr float kDotW = kCellW / 6.0f;
constexpr float kDotH = 2.700f;
constexpr int kDotRows = 7; // the 8th cell row is never used on this display

// Sampled out of docs/lcd_reference.png. This is a POSITIVE display - dark ink on
// a lit green field - so the glass is filled and only the ink dots are drawn.
// There is deliberately no unlit dot grid: profiling a blank character cell on the
// real hardware shows a smooth gradient with no periodicity at the dot pitch, i.e.
// blank cells are plain glass. No halo either, for the same reason.
// These are BRIGHTER than the raw averages taken off docs/lcd_reference.png, deliberately.
// That photo was taken under room light with the camera stopped down so the glyphs would
// not blow out, which lands the field around #3e7515 - far darker than the eye sees on a
// lit module, and dark enough that the panel read as a failing display rather than a
// working one. Rendered side by side with the photograph, these values match what the
// reference actually looks like.
const juce::Colour kGlassOn(0xff6ab81f);  // lit field
const juce::Colour kGlassOff(0xff2c5210); // backlight off: same hue, clearly dimmer
const juce::Colour kInk(0xff05230a);

// Supersampling factor for the offscreen LCD render.
constexpr int kLcdSuper = 8;

// How a pressed cap is drawn. The panel is photographed head-on, so a cap
// travelling into its recess recedes from the viewer: it shrinks slightly towards
// its own centre and the dark recess shows as a ring all the way around it.
// Sliding it down the screen instead reads as tipping it, which is not what the
// control does. These caps are wide and short (63 x 26), so the horizontal shrink
// is what actually reads - and it has to be generous: at 7.5% the recess ring was
// barely 2 px a side and the press was invisible at the default window scale, the
// same mistake that had to be corrected on the TX81Z panel.
constexpr float kPressShrink = 0.13f;
constexpr float kPressDrop = 1.8f;
constexpr float kPowerPressShrink = 0.06f;
constexpr float kPowerPressDrop = 3.0f;

// POWER: the cap face, and the bezel opening it sinks into.
constexpr float kPowerX = 1920.0f, kPowerY = 104.0f, kPowerW = 75.0f, kPowerH = 91.0f;
constexpr float kBezelX = 1913.0f, kBezelY = 96.0f, kBezelW = 85.0f, kBezelH = 106.0f;

// MIDI MESSAGE lamp. The dark slot above POWER runs the full 1915..1997, but only a
// short element in the MIDDLE of it is the actual lamp - profiling the slot picks it
// out cleanly as a lighter block at x 1940..1965, y 57..59, while the bright line
// across the whole width at y 54..55 is a specular reflection off the window's top
// edge, not the part. Lighting the whole slot would be wrong.
//
// It reads unlit in the photograph, so nothing has to be painted out first. Drawn as
// a plain rectangle - no glow halo.
constexpr float kLampX = 1940.0f, kLampY = 56.5f, kLampW = 26.0f, kLampH = 3.5f;

// VOLUME knob. The disc's body ends at r 28 and a clean dark gap runs r 29..33
// before the printed tick ring starts at r 34, so clipping the spin at r 31 takes
// the whole knob and none of the fixed scale. The knob carries its own printed
// white pointer, so it rides on the cut-out and needs no synthetic dot drawn over
// it - but that also means the photograph's own pointer angle has to be subtracted
// before any rotation is applied.
//
// All three angles come from sweeping the panel image around the knob's axis. The
// scale has 21 printed ticks; the two that matter are the outermost, because the
// pointer has to land exactly on them at MIN and at MAX. Each was isolated in its
// own angular window and taken as an intensity-weighted centroid over r 35..42
// (a single wide sweep merges the last two ticks and biases the answer):
//
//     MIN tick  -146.74      MAX tick  +149.96      midpoint  +1.61
//
// The midpoint being 1.6 degrees off vertical is under a pixel at this radius, so
// the knob still reads as pointing straight up at the middle of its travel - which
// is where it now sits by default, since masterVolume's range was made symmetric
// about unity gain.
//
// The photographed pointer is at -151.14 (centroid over r 13..26), a little past
// the MIN tick, so that offset has to be subtracted before any rotation is applied.
constexpr float kKnobCx = 367.5f, kKnobCy = 149.0f;
constexpr float kKnobSpinR = 31.0f, kKnobHitR = 34.0f;
constexpr float kKnobMinDeg = -146.74f, kKnobMaxDeg = 149.96f;
constexpr float kKnobPhotoDeg = -151.14f;

} // namespace

// Left to right, top row then bottom row. Names and scan bits are exactly
// INPUT_PORTS_START(d110) in MAME's src/mame/roland/roland_d10.cpp.
const D110Panel::PanelButton D110Panel::kButtons[D110Panel::kNumButtons] = {
	//   x       y     w     h    name           port  bit
	{  959.0f,  80.0f, 63.0f, 26.0f, "Exit",        0, 0x80 },
	{ 1033.0f,  80.0f, 63.0f, 26.0f, "Patch",       0, 0x40 },
	{ 1107.0f,  80.0f, 63.0f, 26.0f, "Timbre",      0, 0x20 },
	{ 1180.0f,  80.0f, 63.0f, 26.0f, "Part +",      0, 0x10 },
	{ 1253.0f,  80.0f, 63.0f, 26.0f, "Group +",     0, 0x08 },
	{ 1327.0f,  80.0f, 63.0f, 26.0f, "Bank +",      0, 0x04 },
	{ 1399.0f,  80.0f, 63.0f, 26.0f, "Number +",    0, 0x02 },
	{ 1473.0f,  80.0f, 63.0f, 26.0f, "Write/Copy",  0, 0x01 },

	{  959.0f, 168.0f, 63.0f, 26.0f, "Edit",        1, 0x80 },
	{ 1033.0f, 168.0f, 63.0f, 26.0f, "Part",        1, 0x40 },
	{ 1107.0f, 168.0f, 63.0f, 26.0f, "System",      1, 0x20 },
	{ 1180.0f, 168.0f, 63.0f, 26.0f, "Part -",      1, 0x10 },
	{ 1253.0f, 168.0f, 63.0f, 26.0f, "Group -",     1, 0x08 },
	{ 1327.0f, 168.0f, 63.0f, 26.0f, "Bank -",      1, 0x04 },
	{ 1399.0f, 168.0f, 63.0f, 26.0f, "Number -",    1, 0x02 },
	{ 1473.0f, 168.0f, 63.0f, 26.0f, "Enter",       1, 0x01 },
};

D110Panel::D110Panel(D110AudioProcessor &p)
	: processor(p)
{
	panelImage = juce::ImageCache::getFromMemory(BinaryData::panel_reference_png,
	                                             BinaryData::panel_reference_pngSize);
	panelImageCompact = juce::ImageCache::getFromMemory(
		BinaryData::panel_reference_compact_png, BinaryData::panel_reference_compact_pngSize);

	for (const auto &b : kButtons) {
		const juce::Rectangle<float> face(b.x, b.y, b.w, b.h);
		capImages.push_back(cutOut(face));
		recessColours.push_back(recessColourOf(face));
	}
	powerCap = cutOut({ kPowerX, kPowerY, kPowerW, kPowerH });
	powerRecessColour = recessColourOf({ kPowerX, kPowerY, kPowerW, kPowerH });

	volumeDisc = cutOut({ kKnobCx - kKnobSpinR - 1.0f, kKnobCy - kKnobSpinR - 1.0f,
	                      kKnobSpinR * 2.0f + 3.0f, kKnobSpinR * 2.0f + 3.0f });

	setSize(kRefW, kRefH);
	startTimerHz(60);
}

D110Panel::~D110Panel() { stopTimer(); }

void D110Panel::setDisplayScale(float scale)
{
	if (std::abs(scale - lcdDisplayScale) < 0.01f)
		return;
	lcdDisplayScale = scale;
	if (lcdInitialised) {
		rebuildLcdImage();
		repaint();
	}
}

juce::Image D110Panel::cutOut(juce::Rectangle<float> area) const
{
	return panelImage.getClippedImage(
		area.getSmallestIntegerContainer().getIntersection(panelImage.getBounds())).createCopy();
}

// The strip immediately above a cap is the black recess it sits in. Its average
// colour is what shows as a ring around the cap once pressed.
juce::Colour D110Panel::recessColourOf(juce::Rectangle<float> capFace) const
{
	const auto strip = cutOut({ capFace.getX(), capFace.getY() - 4.0f, capFace.getWidth(), 4.0f });
	juce::int64 r = 0, g = 0, b = 0, n = 0;
	for (int y = 0; y < strip.getHeight(); ++y)
		for (int x = 0; x < strip.getWidth(); ++x) {
			const auto c = strip.getPixelAt(x, y);
			r += c.getRed(); g += c.getGreen(); b += c.getBlue(); ++n;
		}
	if (n == 0)
		return juce::Colour(0xff0a0a0a);
	return juce::Colour(juce::uint8(r / n), juce::uint8(g / n), juce::uint8(b / n));
}

// Renders the whole display - glass and ink dots - into an offscreen image, then
// paintLcd() draws that down to the artwork's own kLcdW x kLcdH, and the panel as a
// whole is scaled again by the editor's own Component transform (see setDisplayScale()).
// That second scale is why this can't just supersample by a fixed kLcdSuper: at the
// window's default and smaller sizes the reference artwork itself is being shrunk well
// below its own pixel size, so a source fixed at kLcdSuper-times-the-ARTWORK's own
// resolution makes the compound resize ratio balloon (8x oversample / a 0.4x window
// scale is a ~19:1 downscale) - and JUCE's image resampler, even at high quality, is a
// small-kernel filter that drops thin single-dot strokes at ratios that steep rather
// than area-averaging them. Sizing the source off the CURRENT display scale instead
// keeps that final resize ratio roughly constant regardless of window size, which is
// what actually fixes the strokes breaking up at small sizes.
void D110Panel::rebuildLcdImage()
{
	// How many offscreen pixels per artwork pixel: enough oversampling for the dot
	// antialiasing to look clean, without exploding the buffer at large window sizes.
	const float super = juce::jlimit(2.0f, float(kLcdSuper), 4.0f * lcdDisplayScale);
	const int w = juce::jmax(1, juce::roundToInt(kLcdW * super));
	const int h = juce::jmax(1, juce::roundToInt(kLcdH * super));
	if (!lcdImage.isValid() || lcdImage.getWidth() != w || lcdImage.getHeight() != h)
		lcdImage = juce::Image(juce::Image::RGB, w, h, false);

	juce::Graphics g(lcdImage);

	// Powered off, the backlight simply stops: the glass keeps its colour but goes
	// noticeably dimmer, and nothing is written on it. The same holds in the second or
	// two after POWER while the machine starts and the firmware has not drawn yet.
	if (!processor.isPoweredOn() || !lcdLive) {
		g.fillAll(kGlassOff);
		return;
	}

	g.fillAll(kGlassOn);

	// Panel-space -> offscreen-pixel. Deliberately NOT rounded to whole pixels: the
	// character cell is 14.4 panel px across 6 dot columns, so snapping makes dot
	// widths alternate between two values and the glyph strokes come out visibly
	// ragged. Antialiasing at `super` and downscaling from there keeps every dot
	// the same size.
	auto px = [super](float panelX) { return (panelX - kLcdX) * super; };
	auto py = [super](float panelY) { return (panelY - kLcdY) * super; };
	// Tuned at the original fixed kLcdSuper=8 as a FRACTION of a dot - the smallest gap
	// that keeps the dots of one stroke visibly running together instead of breaking into
	// separate squares. `super` now varies with the window, so the gap has to scale with
	// it to stay that same fraction rather than eating a growing share of a shrunk dot.
	const float kDotGapX = 2.0f * (super / float(kLcdSuper)), kDotGapY = 2.2f * (super / float(kLcdSuper));

	g.setColour(kInk);
	for (int line = 0; line < kLines; ++line)
		for (int col = 0; col < kCols; ++col) {
			// One byte per dot row, straight out of the real MSM6222B: bit 4 is the
			// leftmost dot. The glyphs are therefore the genuine mask CGROM's, and the
			// cursor and its blink are the controller's own - nothing here interprets
			// character codes or consults a font.
			const juce::uint8 *rows = lcdRows + ((size_t)line * kCols + col) * D110CoreType::kRowsPerChar;
			for (int dy = 0; dy < kDotRows; ++dy)
				for (int dx = 0; dx < 5; ++dx) {
					if (!((rows[dy] >> (4 - dx)) & 1))
						continue;
					const float x0 = px(kCharX0 + col * kCellW + dx * kDotW);
					const float y0 = py(kLine0Y + line * kLineStep + dy * kDotH);
					g.fillRect(x0, y0, kDotW * super - kDotGapX, kDotH * super - kDotGapY);
				}
		}
}

void D110Panel::timerCallback()
{
	bool needsRepaint = false;

	// Every moving part eases towards its real position, so it has a little mass
	// and settles instead of teleporting.
	auto ease = [&needsRepaint](ButtonMotion &m) {
		const float target = (m.held || m.latched) ? 1.0f : 0.0f;
		if (std::abs(target - m.depth) > 0.002f) {
			m.depth += (target - m.depth) * 0.30f;
			needsRepaint = true;
		} else if (m.depth != target) {
			m.depth = target;
			needsRepaint = true;
		}
	};

	const bool power = processor.isPoweredOn();
	// POWER is the panel's one latching control: it stays pushed in while the unit
	// is on, and comes back out when switched off.
	powerMotion.latched = power;

	for (auto &m : motion)
		ease(m);
	ease(powerMotion);

	const float volume = processor.getMasterVolume();
	if (volumeDisplayed < 0.0f)
		volumeDisplayed = volume; // no swing on the first paint
	if (std::abs(volume - volumeDisplayed) > 0.0005f) {
		volumeDisplayed += (volume - volumeDisplayed) * 0.16f;
		needsRepaint = true;
	} else if (volumeDisplayed != volume) {
		volumeDisplayed = volume;
		needsRepaint = true;
	}

	bool lcdChanged = !lcdInitialised;

	// The display is whatever the emulated MSM6222B is actually showing. While the unit
	// is off there is no machine running, so the glass just sits dark.
	if (power) {
		juce::uint8 rows[D110CoreType::kLcdBytes];
		if (processor.getCore().getLcd(rows)) {
			if (!lcdLive || std::memcmp(rows, lcdRows, sizeof(rows)) != 0) {
				std::memcpy(lcdRows, rows, sizeof(rows));
				lcdLive = true;
				lcdChanged = true;
			}
		}
	} else if (lcdLive) {
		lcdLive = false;
		lcdChanged = true;
	}

	// The MIDI MESSAGE lamp still follows mt32emu, which is the half that actually sees
	// the notes. Once the firmware's own SO register drives it, this moves across.
	const auto snap = processor.getLcdSnapshot();
	if (snap.midiLedOn != lastSnapshot.midiLedOn) {
		lastSnapshot = snap;
		needsRepaint = true;
	}

	if (power != lastPowerOn) {
		lastPowerOn = power;
		lcdChanged = true;
	}

	if (lcdChanged) {
		rebuildLcdImage();
		lcdInitialised = true;
		needsRepaint = true;
	}

	if (needsRepaint)
		repaint(); // whole panel: a clipped repaint would silently drop the lamp
}

void D110Panel::paint(juce::Graphics &g)
{
	g.drawImageAt(processor.getCompactPanelMode() ? panelImageCompact : panelImage, 0, 0);

	for (int i = 0; i < kNumButtons; ++i)
		paintButton(g, i);

	paintPowerSwitch(g);
	paintVolumeKnob(g);
	paintMidiLamp(g);
	paintLcd(g);
}

// Where a cap's photographed image is drawn for a given press depth: shrunk about
// its own centre, so it recedes into the panel rather than sliding down it, and
// seated a fraction lower.
juce::Rectangle<float> D110Panel::pressedRect(juce::Rectangle<float> face, float depth,
                                              float shrink, float drop)
{
	const float s = 1.0f - shrink * depth;
	return face.withSizeKeepingCentre(face.getWidth() * s, face.getHeight() * s)
	           .translated(0.0f, drop * depth);
}

// Draws a cap at `dst` over its own footprint `face`, with the recess showing as a
// ring around it and the bezel shadowing its upper edge.
void D110Panel::paintPressedCap(juce::Graphics &g, const juce::Image &cap, juce::Colour recess,
                                juce::Rectangle<float> face, juce::Rectangle<float> dst, float depth)
{
	g.setColour(recess);
	g.fillRect(face);

	// Placed by transform rather than by drawImage's integer rectangle: these caps are
	// only 26 px tall, so a 7.5% shrink is under 2 px and rounding it away would leave
	// the press with no visible travel at all.
	g.drawImageTransformed(cap, juce::AffineTransform::scale(dst.getWidth() / float(cap.getWidth()),
	                                                        dst.getHeight() / float(cap.getHeight()))
	                                .translated(dst.getX(), dst.getY()));

	// Less light reaches a cap once it is down in its recess, and the bezel throws
	// a shadow across its top edge.
	g.setColour(juce::Colours::black.withAlpha(0.18f * depth));
	g.fillRect(dst);

	juce::ColourGradient shade(juce::Colours::black.withAlpha(0.55f * depth), dst.getX(), dst.getY(),
	                           juce::Colours::transparentBlack, dst.getX(), dst.getY() + dst.getHeight() * 0.45f, false);
	g.setGradientFill(shade);
	g.fillRect(dst.withHeight(dst.getHeight() * 0.45f));
}

void D110Panel::paintButton(juce::Graphics &g, int index) const
{
	const float depth = motion[index].depth;
	if (depth < 0.02f)
		return; // untouched: the photograph already shows the cap correctly

	const auto &b = kButtons[index];
	const bool compact = processor.getCompactPanelMode();
	const juce::Rectangle<float> face(mapX(b.x, compact), b.y, b.w, b.h);
	paintPressedCap(g, capImages[size_t(index)], recessColours[size_t(index)], face,
	                pressedRect(face, depth, kPressShrink, kPressDrop), depth);
}

void D110Panel::paintPowerSwitch(juce::Graphics &g) const
{
	if (powerMotion.depth < 0.02f)
		return;

	const juce::Rectangle<float> face(mapX(kPowerX, processor.getCompactPanelMode()), kPowerY, kPowerW, kPowerH);
	paintPressedCap(g, powerCap, powerRecessColour, face,
	                pressedRect(face, powerMotion.depth, kPowerPressShrink, kPowerPressDrop),
	                powerMotion.depth);
}

void D110Panel::paintVolumeKnob(juce::Graphics &g) const
{
	const float value = juce::jlimit(0.0f, 1.0f, volumeDisplayed < 0.0f ? processor.getMasterVolume()
	                                                                    : volumeDisplayed);
	// The cut-out already carries the photograph's own pointer angle, so only the
	// difference between where the pointer should be and where it was shot is applied.
	const float deg = kKnobMinDeg + (kKnobMaxDeg - kKnobMinDeg) * value - kKnobPhotoDeg;
	if (std::abs(deg) < 0.05f)
		return; // at rest: the photograph is already correct

	const float cx = mapX(kKnobCx, processor.getCompactPanelMode());

	juce::Graphics::ScopedSaveState ss(g);
	juce::Path clip;
	clip.addEllipse(cx - kKnobSpinR, kKnobCy - kKnobSpinR, kKnobSpinR * 2.0f, kKnobSpinR * 2.0f);
	g.reduceClipRegion(clip);

	// The cut-out was taken from the panel at this origin, so put it back exactly
	// there (shifted, in compact mode) and spin about the true centre.
	const float ox = std::floor(cx - kKnobSpinR - 1.0f);
	const float oy = std::floor(kKnobCy - kKnobSpinR - 1.0f);
	g.drawImageTransformed(volumeDisc, juce::AffineTransform::translation(ox, oy)
	                                       .rotated(juce::degreesToRadians(deg), cx, kKnobCy));
}

void D110Panel::paintMidiLamp(juce::Graphics &g) const
{
	// A plain rectangular lens. The photographed lamp is unlit, so the unlit state
	// needs nothing drawn at all.
	//
	// What lights it, from munt's Display::checkDisplayStateUpdated(): a MIDI message
	// having been played since the last reset - an 80 ms blink, BLINK_TIME_MILLIS -
	// OR any of the eight VOICE parts currently sounding ("the LED represents activity
	// of the voice parts only", so the Rhythm part does not light it). On the real
	// hardware the firmware drives it directly: bit 0 of the SO register at 0x0200,
	// per so_w() in MAME's roland_d10.cpp - which is what this will be rebound to once
	// the firmware is running, and which will also settle whether it flashes at
	// power-on.
	if (!processor.isPoweredOn() || !lastSnapshot.midiLedOn)
		return;

	const juce::Rectangle<float> lens(mapX(kLampX, processor.getCompactPanelMode()), kLampY, kLampW, kLampH);
	g.setColour(juce::Colour(0xffe0472a));
	g.fillRect(lens);
	g.setColour(juce::Colour(0xffffc9b0).withAlpha(0.75f));
	g.fillRect(lens.reduced(1.0f, 1.0f));
}

void D110Panel::paintLcd(juce::Graphics &g) const
{
	const float lcdX = mapX(kLcdX, processor.getCompactPanelMode());

	// Blank the window first and unconditionally: the photograph was taken of a
	// unit with its own glass showing, and anything less than a guaranteed opaque
	// cover here lets that ghost through under the live render.
	g.setColour(processor.isPoweredOn() ? kGlassOn : kGlassOff);
	g.fillRect(lcdX, kLcdY, kLcdW, kLcdH);

	if (lcdImage.isValid()) {
		// The display is the one part of this panel anybody reads, and the window is often
		// scaled well away from the artwork's own size, so it is worth resampling properly
		// rather than at the default quality.
		g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
		g.drawImage(lcdImage, int(lcdX), int(kLcdY), int(kLcdW), int(kLcdH),
		            0, 0, lcdImage.getWidth(), lcdImage.getHeight(), false);
	}
}

int D110Panel::buttonAt(juce::Point<float> p) const
{
	const bool compact = processor.getCompactPanelMode();
	if (juce::Rectangle<float>(mapX(kBezelX, compact), kBezelY, kBezelW, kBezelH).contains(p))
		return kPowerIndex;

	for (int i = 0; i < kNumButtons; ++i)
		if (juce::Rectangle<float>(mapX(kButtons[i].x, compact), kButtons[i].y, kButtons[i].w, kButtons[i].h)
		        .contains(p))
			return i;

	return -1;
}

// Every button now closes its real switch in the firmware's 2x8 scan matrix, so the
// menus, the patch and timbre editors and the Write/Copy confirmations are the
// Roland firmware's own - not anything reimplemented here. kButtons carries each
// button's port and mask straight from INPUT_PORTS_START(d110); D110Core wants the
// bit NUMBER, so the mask is converted here.
void D110Panel::setButtonState(int index, bool down)
{
	if (index < 0 || index >= kNumButtons) return;
	// Раньше здесь стоял ранний выход, пока прибор выключен ("nothing to press while the
	// unit is off") - и он молча ломал ДОКУМЕНТИРОВАННУЮ процедуру факт-сброса: она прямо
	// требует защёлкнуть WRITE/COPY, ПОКА ПРИБОР ВЫКЛЮЧЕН, и только потом включить. Ctrl-клик
	// в этот момент попадал сюда, отражался ранним выходом, и `core.setButton()` не
	// вызывался вовсе - защёлкивался только локальный флаг панели `m.latched`, который ничего
	// не значит для настоящей матрицы опроса. Прибор включался, ничего не видел зажатым, и
	// пятишаговая процедура из README не срабатывала НИКОГДА, ни для одного пользователя -
	// не через раз, а структурно, самим порядком проверки.
	//
	// Убирать защиту можно без риска: `D110Core::setButton()` - это голая атомарная запись в
	// `wantButtons`, ей ничего не нужно от работающей машины, и `D110Core::factoryReset()`
	// делает ровно то же самое внутри себя - защёлкивает Write/Copy ДО вызова `start()` - и
	// это работает, им пользуется даже `plugin/nvram_recovery.cpp`. Обычный, незащёлкнутый
	// клик по-прежнему безвреден на выключенном приборе: mouseUp снимает то же самое
	// нажатие, и итог - ноль.
	const auto &b = kButtons[index];
	int bit = 0;
	while (bit < 7 && !((b.scanBit >> bit) & 1)) ++bit;
	processor.getCore().setButton(D110CoreType::buttonIndex(b.scanPort, bit), down);
}

void D110Panel::mouseDown(const juce::MouseEvent &e)
{
	// Emulator settings live on a right-click, keeping the photographed panel free
	// of controls the hardware does not have.
	if (e.mods.isPopupMenu()) {
		showOptionsMenu();
		return;
	}

	const auto p = e.position;

	// Щель карты памяти. Сама карта панели не принадлежит - она ездит по всему окну и живёт
	// отдельным компонентом, - но щель нарисована на приборе, и попадание в неё ловит панель.
	// Compact mode splices this whole section out of the photo - see kCompactCardCutStart/End -
	// so there is nothing here to hit at all.
	if (!processor.getCompactPanelMode()
	    && juce::Rectangle<float>(kSlotHitX, kSlotHitY, kSlotHitW, kSlotHitH).contains(p)) {
		if (onCardSlotClicked) onCardSlotClicked();
		return;
	}

	const int idx = buttonAt(p);

	if (idx == kPowerIndex) {
		processor.togglePower();
		if (!processor.isPoweredOn())
			for (auto &m : motion) { m.held = false; m.latched = false; }
		// Only one emulated control board can exist per process, so a second instance
		// cannot be switched on. Say so plainly - the alternative is a POWER button that
		// silently does nothing.
		if (processor.isPowerBlocked())
			juce::NativeMessageBox::showMessageBoxAsync(
				juce::MessageBoxIconType::WarningIcon, "D-110 Emulator",
				processor.getLastError());
		return;
	}

	if (idx >= 0) {
		auto &m = motion[size_t(idx)];

		// A plain click is always momentary, and always releases a cap that was latched.
		// Latching is deliberate only: it needs a modifier. It used to be a double-click,
		// which meant ordinary quick clicking latched buttons by accident and left them
		// stuck down - and worse, a single click on a latched cap did nothing, so there
		// was no obvious way out.
		if (e.mods.isCtrlDown() || e.mods.isAltDown()) {
			m.latched = !m.latched;
			m.held = false;
			setButtonState(idx, m.latched);
			return;
		}

		if (m.latched) {
			m.latched = false;
			m.held = false;
			setButtonState(idx, false);
			return;
		}

		m.held = true;
		setButtonState(idx, true);
		return;
	}

	if (p.getDistanceFrom({ mapX(kKnobCx, processor.getCompactPanelMode()), kKnobCy }) <= kKnobHitR) {
		drag = Drag::volume;
		dragStart = p;
		dragStartValue = processor.getMasterVolume();
	}
}

void D110Panel::mouseDrag(const juce::MouseEvent &e)
{
	if (drag != Drag::volume)
		return;
	processor.setMasterVolume(dragStartValue + (dragStart.y - e.position.y) / 120.0f);
}

void D110Panel::mouseUp(const juce::MouseEvent &)
{
	// A latched cap keeps its own `latched` flag, so clearing `held` here leaves the
	// switch closed - which is the whole point of latching, since real D-110 procedures
	// need two buttons at once and one mouse cannot do that.
	for (int i = 0; i < kNumButtons; ++i) {
		if (!motion[size_t(i)].held) continue;
		motion[size_t(i)].held = false;
		if (!motion[size_t(i)].latched) setButtonState(i, false);
	}
	drag = Drag::none;
}

// Deliberately empty. Latching a button lives on ctrl/alt-click (see mouseDown):
// hanging it off a double-click meant that clicking a button twice in quick
// succession - which is exactly how anyone steps a value - silently latched it down.
void D110Panel::mouseDoubleClick(const juce::MouseEvent &) {}

void D110Panel::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w)
{
	if (e.position.getDistanceFrom({ mapX(kKnobCx, processor.getCompactPanelMode()), kKnobCy }) <= kKnobHitR)
		processor.setMasterVolume(processor.getMasterVolume() + w.deltaY * 0.5f);
}

namespace {
// Linux's native picker (zenity/kdialog, juce_FileChooser_linux.cpp) shells out with the raw
// filter string and matches it with a case-sensitive glob - "*.mid" alone never matches
// "Bank.MID" there (unlike Windows/macOS, or JUCE's own non-native fallback browser, both
// case-insensitive). Simply widening the pattern (adding "*.SYX"/"*.MID"/"*.SMF") fixes the
// common cases, but a stray ".Mid" or similar still wouldn't match anything - and folding a
// permissive "*.*" into that same always-on filter (an earlier attempt) is explicitly NOT what
// Alan wants (2026-08-27): the default should stay narrow, with "*.*" reachable only as an
// explicit, separate choice. juce::FileChooser's single filter string can't express two
// independently selectable filter groups on Linux (addZenityArgs()/addKDialogArgs() always
// collapse everything into one "--file-filter="), so this bypasses it for the two SysEx/MIDI
// import pickers (the panel's own right-click menu, and the Utility tab's copy of the same
// feature) and talks to zenity/kdialog directly, mirroring their own tool-selection logic
// (isKdeFullSession() favouring kdialog) so behaviour matches what juce::FileChooser would have
// picked anyway. Falls back to a plain juce::FileChooser (with the widened pattern, no "*.*")
// on any other platform, or if neither zenity nor kdialog is installed.
class TieredNativeFileChooser final : private juce::Timer {
public:
	using Callback = std::function<void(const juce::File &)>;

	static bool isAvailable() { return exeIsAvailable("zenity") || exeIsAvailable("kdialog"); }

	TieredNativeFileChooser(const juce::String &title, const juce::File &startDir,
	                         const juce::String &strictLabel, const juce::String &strictPatterns,
	                         Callback callbackIn)
	    : callback(std::move(callbackIn)) {
		juce::StringArray args;
		if (useKdialog()) {
			args.add("kdialog");
			args.add("--title=" + title);
			args.add("--getopenfilename");
			args.add(startDir.getFullPathName());
			// kdialog's own filter syntax: one or more "patterns|label" groups, "\n"-separated.
			args.add(strictPatterns + "|" + strictLabel + "\n*.*|All files");
		} else {
			args.add("zenity");
			args.add("--file-selection");
			args.add("--title=" + title);
			args.add("--filename=" + startDir.getFullPathName() + "/");
			// Two separate --file-filter= args, so both show up as independently selectable
			// entries in the dialog's own filter dropdown, strict one active by default.
			args.add("--file-filter=" + strictLabel + " | " + strictPatterns);
			args.add("--file-filter=All files | *.*");
		}
		child.start(args, juce::ChildProcess::wantStdOut);
		startTimer(100);
	}

private:
	static bool exeIsAvailable(const juce::String &executable) {
		juce::ChildProcess p;
		if (!p.start("which " + executable)) return false;
		p.waitForProcessToFinish(5000);
		return p.getExitCode() == 0;
	}
	static bool isKdeFullSession() {
		return juce::SystemStats::getEnvironmentVariable("KDE_FULL_SESSION", {}).equalsIgnoreCase("true");
	}
	static bool useKdialog() { return exeIsAvailable("kdialog") && (isKdeFullSession() || !exeIsAvailable("zenity")); }

	void timerCallback() override {
		if (child.isRunning()) return;
		stopTimer();
		const auto result = child.readAllProcessOutput().trim();
		child.waitForProcessToFinish(1000);
		const auto file = result.isNotEmpty()
		                       ? juce::File::getCurrentWorkingDirectory().getChildFile(result)
		                       : juce::File();
		const auto cb = callback;
		cb(file);
		delete this;
	}

	juce::ChildProcess child;
	Callback callback;
};
} // namespace

void D110Panel::showOptionsMenu()
{
	auto *reverb = processor.parameters.getParameter("reverbEnabled");
	auto *superMode = processor.parameters.getParameter("superMode");
	const bool reverbOn = reverb != nullptr && reverb->getValue() > 0.5f;
	const bool superOn = superMode != nullptr && superMode->getValue() > 0.5f;

	juce::PopupMenu m;
	m.addItem(1, "Import SysEx/MIDI Bank...");
	m.addSeparator();
	m.addItem(2, "Reverb", true, reverbOn);
	m.addItem(3, "Super Mode (unofficial, extra polyphony)", true, superOn);
	// Движок защиты от записи - он на самой карте, а не в приборе, поэтому и в меню он стоит
	// отдельно от настроек эмулятора. Прошивка читает его как бит 0 порта состояния матрицы
	// карты; см. docs/memory_card.md.
	m.addItem(4, "Memory card write protect", true, processor.getCore().cardWriteProtect());
	m.addItem(5, "Retro Sequencer (D-20 style LCD+buttons)", true, processor.getSequencerRetroMode());
	// Github issue #3: the LA Reference (structures/envelopes chart, UTILITY tab) was only
	// reachable by opening the editor drawer and navigating there. Repeated here so it's one
	// right-click away, the same shortcut the channel/remap entries below get.
	m.addItem(6, "LA Reference (algorithms & envelopes)...");
	// Same setting D110Keyboard's own right-click already exposes (see its showContextMenu) -
	// repeated here so it's reachable without opening/finding the on-screen keyboard drawer.
	// Github issue #4: with MIDI Remap on, this channel is also what ALL incoming MIDI (host-
	// or port-fed, not just the on-screen keyboard) gets forced onto - see processBlock()'s
	// rechannelizing block and handleIncomingMidiMessage(). Off is the default since
	// 2026-08-25 for exactly that reason: it's the only setting where external MIDI reaches
	// each Part on the channel it was actually sent on, unmodified. Called "Omni" until this
	// same day - see D110KeyboardHost.h's own comment on why that name was dropped.
	{
		const int curCh = processor.getKeyboardMidiChannel();
		const bool remapOn = processor.getMidiRemap();
		juce::PopupMenu channelMenu;
		for (int ch = 1; ch <= 16; ++ch)
			channelMenu.addItem(700 + ch, "Channel " + juce::String(ch), true, remapOn && curCh == ch);
		m.addSubMenu("MIDI Channel (on-screen keyboard / forced remap target)", channelMenu, remapOn);
		m.addItem(717, "MIDI Remap (force everything onto the channel above)", true, remapOn);
	}
	// Пункта «пусть ноты озвучивает прошивка» здесь нет намеренно. Это не настройка, а
	// единственное поведение: ноты идут в прошивку, она применяет свои диапазоны клавиш,
	// раскладку по партиям и распределение голосов, зажигает индикаторы в верхней строке
	// и возвращает то, что действительно взяла. Выключение всего этого не давало ничего,
	// кроме менее точного инструмента, и было временной мерой на время, пока ноты роняли
	// панель, - в 0.9.6 это исправлено.
	m.addSeparator();

	// The standalone window now uses the OS's own native title bar (Alan's request, see
	// PluginEditor::parentHierarchyChanged) instead of JUCE's default custom-drawn one, which
	// was the only place these four lived - JUCE's own Options button. Nothing to relocate for
	// the plugin builds: a DAW host already offers its own audio/MIDI setup and project
	// save/load, so these only make sense standalone.
	const bool isStandalone = processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
	if (isStandalone) {
		m.addItem(900, "Audio/MIDI Settings...");
		m.addItem(901, "Save current state...");
		m.addItem(902, "Load a saved state...");
		m.addItem(903, "Reset to default state");
		m.addSeparator();
	}

	// Direct MIDI ports, beside whatever the host routes in. This is how an external
	// editor reaches the module the way it would reach the hardware.
	const auto ins = D110AudioProcessor::midiInputs();
	const auto outs = D110AudioProcessor::midiOutputs();
	juce::PopupMenu inMenu, outMenu;
	inMenu.addItem(300, "(none - host only)", true, processor.getMidiInputId().isEmpty());
	for (int i = 0; i < ins.size(); ++i)
		inMenu.addItem(400 + i, ins[i].name, true, processor.getMidiInputId() == ins[i].identifier);
	outMenu.addItem(301, "(none)", true, processor.getMidiOutputId().isEmpty());
	for (int i = 0; i < outs.size(); ++i)
		outMenu.addItem(500 + i, outs[i].name, true, processor.getMidiOutputId() == outs[i].identifier);
	m.addSubMenu("MIDI In", inMenu);
	m.addSubMenu("MIDI Out", outMenu);
	m.addSeparator();

	if (processor.isSynthReady()) {
		m.addItem(100, "Control ROM: " + processor.getControlRomDescription(), false, false);
		m.addItem(101, "PCM ROM: " + processor.getPcmRomDescription(), false, false);
	} else {
		auto error = processor.getLastError();
		m.addItem(100, error.isNotEmpty() ? error : juce::String("No ROMs loaded"), false, false);
	}
	m.addItem(102, processor.getCore().isRunning()
	                   ? juce::String("Control board: D-110 firmware running")
	                   : juce::String("Control board: stopped (switch POWER on)"),
	          false, false);
	if (processor.getLastImportMessage().isNotEmpty())
		m.addItem(103, processor.getLastImportMessage(), false, false);
	m.addItem(104, "Part 1 listens on MIDI channel 2, as on real hardware", false, false);
	m.addItem(108, juce::String(D110AudioProcessor::kExtendedPolyphonyLabel)
	                   + " (real hardware: 32) - see UTILITY tab", false, false);
	m.addItem(105, D110AudioProcessor::nvramIsBesideRoms()
	                   ? juce::String("Firmware memory: in the D-110 Data folder")
	                   : juce::String("Firmware memory: in app data (data folder not writable)"),
	          false, false);
	// There is no Factory Reset command here any more, and deliberately so: the D-110 has
	// its own way of doing it and the panel can now perform it exactly. Ctrl+click latches
	// a cap down, which is what makes "hold this button while switching on" possible with
	// one mouse. Latching is on a modifier rather than on a long press because the firmware
	// repeats a held button - a long press is how you scroll a value, and it must stay that
	// way.
	m.addSeparator();
	m.addItem(106, "Factory init, as on the hardware:", false, false);
	m.addItem(107,
		"   POWER off, Ctrl+click WRITE/COPY, POWER on, Ctrl+click WRITE/COPY "
		"again, then ENTER",
		false, false);

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
		[this, reverb, superMode, reverbOn, superOn, ins, outs](int result) {
			// The port lists are captured as they were when the menu opened, so an entry
			// always means the device the user actually saw and picked.
			if (result == 300) { processor.setMidiInputDevice({}); return; }
			if (result == 301) { processor.setMidiOutputDevice({}); return; }
			if (result > 700 && result <= 716) {
				processor.setKeyboardMidiChannel(result - 700);
				return;
			}
			if (result >= 500 && result - 500 < outs.size()) {
				processor.setMidiOutputDevice(outs[result - 500].identifier);
				return;
			}
			if (result >= 400 && result - 400 < ins.size()) {
				processor.setMidiInputDevice(ins[result - 400].identifier);
				return;
			}
			switch (result) {
			case 1: {
				auto onPicked = [this](const juce::File &file) {
					if (file != juce::File()) {
						processor.setLastDialogDir(file.getParentDirectory());
						processor.importSysexBank(file);
					}
				};
				if (TieredNativeFileChooser::isAvailable()) {
					// See TieredNativeFileChooser's own comment: a real, independently selectable
					// "All files" fallback in the same dialog, not a permissive filter always on.
					new TieredNativeFileChooser("Select a SysEx bank or MIDI file", processor.getLastDialogDir(),
					                            "SysEx/MIDI files", "*.syx *.SYX *.mid *.MID *.smf *.SMF", onPicked);
					break;
				}
				fileChooser = std::make_unique<juce::FileChooser>(
					"Select a SysEx bank or MIDI file", processor.getLastDialogDir(),
					"*.syx;*.SYX;*.mid;*.MID;*.smf;*.SMF");
				fileChooser->launchAsync(
					juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
					[onPicked](const juce::FileChooser &fc) { onPicked(fc.getResult()); });
				break;
			}
			case 2:
				if (reverb != nullptr) {
					reverb->beginChangeGesture();
					reverb->setValueNotifyingHost(reverbOn ? 0.0f : 1.0f);
					reverb->endChangeGesture();
				}
				break;
			case 3:
				if (superMode != nullptr) {
					superMode->beginChangeGesture();
					superMode->setValueNotifyingHost(superOn ? 0.0f : 1.0f);
					superMode->endChangeGesture();
				}
				break;
			case 4:
				processor.getCore().setCardWriteProtect(!processor.getCore().cardWriteProtect());
				break;
			case 5:
				processor.setSequencerRetroMode(!processor.getSequencerRetroMode());
				if (onSequencerModeChanged) onSequencerModeChanged();
				break;
			case 6:
				// NOT getWidth(): this panel is drawn at native reference resolution and
				// scaled visually via a Component transform (see D110AudioProcessorEditor::
				// resized()'s panel.setTransform), so getWidth() here would return the
				// UNSCALED reference width (2124/1497) rather than the window's actual pixel
				// width. getTopLevelComponent() is the real, untransformed editor window.
				showLaReferencePopup(getTopLevelComponent()->getWidth());
				break;
			case 717:
				processor.setMidiRemap(!processor.getMidiRemap());
				break;
			case 900:
			case 901:
			case 902:
			case 903:
#if JucePlugin_Build_Standalone
				performStandaloneAppAction(*this, result);
#endif
				break;
			default:
				break;
			}
		});
}

// ---------------------------------------------------------------------------
// Расширенный редактор

namespace {

// Colours taken from the instrument itself, so the drawer reads as its continuation rather
// than a foreign panel: labels are blue, like Roland's own silkscreen on the front panel;
// values are green, like the D-110's own display, which is backlit green rather than amber
// (docs/lcd_reference.png). Functions rather than constants because the theme (Utility ->
// THEME) switches at runtime - see UiTheme.h for the actual dark/light palette; these six
// names are just the facade left in place so the dozens of call sites below didn't need
// touching.
inline juce::Colour kEdBack() { return d110ui::palette().panelBg; }
inline juce::Colour kEdBox() { return d110ui::palette().box; }
inline juce::Colour kEdBorder() { return d110ui::palette().boxBorder; }
inline juce::Colour kEdLabel() { return d110ui::palette().label; }
inline juce::Colour kEdValue() { return d110ui::palette().value; }
inline juce::Colour kEdDim() { return d110ui::palette().dim; }

// Восемь голосовых партий и ритм - ровно те девять, для которых у прибора есть запись в
// Timbre Temporary.
const char *partLabel(int p) {
	static const char *kNames[] = { "1", "2", "3", "4", "5", "6", "7", "8", "R" };
	return (p >= 0 && p < 9) ? kNames[p] : "?";
}

// Четыре группы тонов, как их РАЗЛОЖИЛ САМ ПРИБОР: группа партии 1 ставилась эксклюзивным
// сообщением, и имя тона читалось с индикатора (plugin/editor_write_probe.cpp, раздел 5).
//   0 -> AcouPiano1   пресетная группа A
//   1 -> Fantasy      пресетная группа B
//   2 -> имя, только что записанное в память тонов - то есть внутренняя память
//   3 -> ClsdHiHat1   ударные
const char *toneGroupLabel(int g) {
	// Буквы - Roland'овские: на ламинированной карточке «Preset Tones» группы названы a, b и
	// r, а четвёртая - внутренняя память, которую прибор заполняет только сам пользователь.
	static const char *kNames[] = { "a PRESET", "b PRESET", "i INTERNAL", "r RHYTHM" };
	return (g >= 0 && g < 4) ? kNames[g] : "?";
}

// Восемь типов ревербератора с той же карточки, плюс OFF девятым значением.
const char *reverbTypeLabel(int v) {
	static const char *kNames[] = { "1 SMALL ROOM", "2 MEDIUM ROOM", "3 MEDIUM HALL",
	                                "4 LARGE HALL", "5 PLATE", "6 DELAY 1", "7 DELAY 2",
	                                "8 DELAY 3" };
	return (v >= 0 && v < 8) ? kNames[v] : "OFF";
}

// Назначение на выходы. У прибора это MIX плюс шесть индивидуальных выходов - MULTI OUT 1-6
// по блок-схеме сервисных заметок, - и байт идёт от 1 (MIX) до 7 (выход 6). Измерено на
// самой странице Timbre Edit: три нажатия сдвинули байт 6 с 1 на 4, а экран показал «3»
// (plugin/editor_write_probe.cpp, раздел 7).
juce::String outputAssignText(int v) {
	return (v <= 1) ? juce::String("MIX") : juce::String(v - 1);
}

void drawBox(juce::Graphics &g, juce::Rectangle<float> r, bool highlight) {
	g.setColour(kEdBox());
	g.fillRoundedRectangle(r, 3.0f);
	g.setColour(highlight ? kEdValue().withAlpha(0.55f) : kEdBorder());
	g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
}

// Панорама у Roland: 0 - вправо до упора, 7 - середина, 14 - влево до упора. Прибор пишет
// её как расстояние и сторону - «3>» это три шага вправо (docs/factory_defaults.md).
juce::String panText(int v) {
	const int off = v - 7;
	if (off == 0) return "C";
	return (off < 0) ? (juce::String(-off) + ">") : ("<" + juce::String(off));
}

// Roland считает октавы так, что нота 0 - это C-1, а 127 - G9: именно это показывает
// страница Key Range самого прибора (docs/factory_defaults.md). У JUCE это задаётся номером
// октавы для среднего до, и он равен четырём, а не трём.
juce::String noteName(int note) {
	return juce::MidiMessage::getMidiNoteName(note, true, true, 4);
}

// The PCM ROM's own 128 wave names, one table per bank, straight from Roland's own
// "PCM Sounds" appendix - Github issue #2 asked for names instead of a bare 0-127 number.
// Which bank a partial's PCM field (offset 5) indexes into is the OTHER bit of the
// neighbouring WAVEFORM field (offset 4: 0/1 = bank 1, 2/3 = bank 2 - see
// docs/la32_register_map.md's "WG PCM Bank / PCM Wave" entry), so callers need that byte
// too, not just this table. Bank 2's first 30 are the same rhythm hits as Bank 1's (marked
// with a "*" in the manual - pitch there isn't affected by Master Tuning), the rest are
// unnamed Loop-N/Jam-N combination sounds, transcribed as-is rather than invented.
const char *const kPcmBank1Names[128] = {
	"Bass Drum-1", "Bass Drum-2", "Bass Drum-3", "Snare Drum-1", "Snare Drum-2",
	"Snare Drum-3", "Snare Drum-4", "Tom Tom-1", "Tom Tom-2", "High-Hat",
	"High-Hat (Loop)", "Crash Cymbal-1", "Crash Cymbal-2 (Loop)", "Ride Cymbal-1",
	"Ride Cymbal-2 (Loop)", "Cup", "China Cymbal-1", "China Cymbal-2 (Loop)", "Rim Shot",
	"Hand Clap", "Mute High Conga", "Conga", "Bongo", "Cowbell", "Tambourine", "Agogo",
	"Claves", "Timbale High", "Timbale Low", "Cabasa", "Timpani Attack", "Timpani",
	"Acoustic Piano High", "Acoustic Piano Low", "Piano Forte Thump", "Organ Percussion",
	"Trumpet", "Lips", "Trombone", "Clarinet", "Flute High", "Flute Low", "Steamer",
	"Indian Flute", "Breath", "Vibraphone High", "Vibraphone Low", "Marimba",
	"Xylophone High", "Xylophone Low", "Kalimba", "Wind Bell", "Chime Bar", "Hammer",
	"Guiro", "Chink", "Nails", "Fretless Bass", "Pull Bass", "Slap Bass", "Thump Bass",
	"Acoustic Bass", "Electric Bass", "Gut Guitar", "Steel Guitar", "Dirty Guitar",
	"Pizzicato", "Harp", "Contrabass", "Cello", "Violin-1", "Violin-2", "Koto",
	"Drawbars (Loop)", "High Organ (Loop)", "Low Organ (Loop)", "Trumpet (Loop)",
	"Trombone (Loop)", "Sax-1 (Loop)", "Sax-2 (Loop)", "Reed (Loop)", "Slap Bass (Loop)",
	"Acoustic Bass (Loop)", "Electric Bass-1 (Loop)", "Electric Bass-2 (Loop)",
	"Gut Guitar (Loop)", "Steel Guitar (Loop)", "Electric Guitar (Loop)", "Clav (Loop)",
	"Cello (Loop)", "Violin (Loop)", "Electric Piano-1 (Loop)", "Electric Piano-2 (Loop)",
	"Harpsichord-1 (Loop)", "Harpsichord-2 (Loop)", "Telephone Bell",
	"Female Voice-1 (Loop)", "Female Voice-2 (Loop)", "Male Voice-1 (Loop)",
	"Male Voice-2 (Loop)", "Spectrum-1 (Loop)", "Spectrum-2 (Loop)", "Spectrum-3 (Loop)",
	"Spectrum-4 (Loop)", "Spectrum-5 (Loop)", "Spectrum-6 (Loop)", "Spectrum-7 (Loop)",
	"Spectrum-8 (Loop)", "Spectrum-9 (Loop)", "Spectrum-10 (Loop)", "Noise (Loop)",
	"Shot-1", "Shot-2", "Shot-3", "Shot-4", "Shot-5", "Shot-6", "Shot-7", "Shot-8",
	"Shot-9", "Shot-10", "Shot-11", "Shot-12", "Shot-13", "Shot-14", "Shot-15", "Shot-16",
	"Shot-17",
};
const char *const kPcmBank2Names[128] = {
	"Bass Drum-1*", "Bass Drum-2*", "Bass Drum-3*", "Snare Drum-1*", "Snare Drum-2*",
	"Snare Drum-3*", "Snare Drum-4*", "Tom Tom-1*", "Tom Tom-2*", "High-Hat*",
	"High-Hat* (Loop)", "Crash Cymbal-1*", "Crash Cymbal-2* (Loop)", "Ride Cymbal-1*",
	"Ride Cymbal-2* (Loop)", "Cup*", "China Cymbal-1*", "China Cymbal-2* (Loop)",
	"Rim Shot*", "Hand Clap*", "Mute High Conga*", "Conga*", "Bongo*", "Cowbell*",
	"Tambourine*", "Agogo*", "Claves*", "Timbale High*", "Timbale Low*", "Cabasa*",
	"Loop-1", "Loop-2", "Loop-3", "Loop-4", "Loop-5", "Loop-6", "Loop-7", "Loop-8",
	"Loop-9", "Loop-10", "Loop-11", "Loop-12", "Loop-13", "Loop-14", "Loop-15", "Loop-16",
	"Loop-17", "Loop-18", "Loop-19", "Loop-20", "Loop-21", "Loop-22", "Loop-23", "Loop-24",
	"Loop-25", "Loop-26", "Loop-27", "Loop-28", "Loop-29", "Loop-30", "Loop-31", "Loop-32",
	"Loop-33", "Loop-34", "Loop-35", "Loop-36", "Loop-37", "Loop-38", "Loop-39", "Loop-40",
	"Loop-41", "Loop-42", "Loop-43", "Loop-44", "Loop-45", "Loop-46", "Loop-47", "Loop-48",
	"Loop-49", "Loop-50", "Loop-51", "Loop-52", "Loop-53", "Loop-54", "Loop-55", "Loop-56",
	"Loop-57", "Loop-58", "Loop-59", "Loop-60", "Loop-61", "Loop-62", "Loop-63", "Loop-64",
	"Jam-1 (Loop)", "Jam-2 (Loop)", "Jam-3 (Loop)", "Jam-4 (Loop)", "Jam-5 (Loop)",
	"Jam-6 (Loop)", "Jam-7 (Loop)", "Jam-8 (Loop)", "Jam-9 (Loop)", "Jam-10 (Loop)",
	"Jam-11 (Loop)", "Jam-12 (Loop)", "Jam-13 (Loop)", "Jam-14 (Loop)", "Jam-15 (Loop)",
	"Jam-16 (Loop)", "Jam-17 (Loop)", "Jam-18 (Loop)", "Jam-19 (Loop)", "Jam-20 (Loop)",
	"Jam-21 (Loop)", "Jam-22 (Loop)", "Jam-23 (Loop)", "Jam-24 (Loop)", "Jam-25 (Loop)",
	"Jam-26 (Loop)", "Jam-27 (Loop)", "Jam-28 (Loop)", "Jam-29 (Loop)", "Jam-30 (Loop)",
	"Jam-31 (Loop)", "Jam-32 (Loop)", "Jam-33 (Loop)", "Jam-34 (Loop)",
};

} // namespace

D110EditorPane::D110EditorPane(D110AudioProcessor &p) : processor(p), soundbankBrowser(p) {
	setOpaque(true);
	// So arrow keys reach keyPressed() below right after a click here, the same pattern
	// D110Keyboard already uses (grabs focus on every mouseDown, see its own comment).
	setWantsKeyboardFocus(true);
	ram.assign(D110CoreType::kRamSize, 0);

	// Hidden until the SOUNDBANKS tab is selected (layout()'s own Tab::Soundbanks case) - a
	// real child Component, unlike every other tab.
	addChildComponent(soundbankBrowser);

	// Поле ввода одно на весь редактор: любое имя набирается им же, просто в разных местах.
	// Прибор принимает только печатные ASCII, поэтому набрать что-то другое здесь нельзя -
	// это ограничение прибора, а не удобства.
	addChildComponent(textEntry);
	textEntry.setMultiLine(false);
	textEntry.setReturnKeyStartsNewLine(false);
	textEntry.setInputRestrictions(20, " !\"#$%&'()*+,-./0123456789:;<=>?@"
	                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
	                                   "abcdefghijklmnopqrstuvwxyz{|}~");
	textEntry.setColour(juce::TextEditor::backgroundColourId, kEdBox());
	textEntry.setColour(juce::TextEditor::textColourId, kEdValue());
	textEntry.setColour(juce::TextEditor::outlineColourId, kEdValue().withAlpha(0.6f));
	textEntry.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f,
	                                    juce::Font::plain));
	textEntry.onReturnKey = [this] {
		switch (textEntryTarget) {
		case 1: processor.sendName(D110CoreType::kSysexToneTemp, part * D110CoreType::kToneRecord,
		                           textEntry.getText()); break;
		case 2: processor.sendDisplayMessage(textEntry.getText()); break;
		case 3: processor.sendName(D110CoreType::kSysexPatches, patchSlot * D110CoreType::kPatchRecord,
		                           textEntry.getText()); break;
		case 4: processor.sendName(D110CoreType::kSysexTones, toneSlot * D110CoreType::kToneMemRecord,
		                           textEntry.getText()); break;
		default: break;
		}
		// Надпись на индикатор посылается сколько угодно раз подряд, поэтому её поле
		// остаётся открытым; имя набирается один раз и закрывается.
		if (textEntryTarget != 2) {
			textEntryTarget = 0;
			textEntryButton = -1;
			textEntry.setVisible(false);
		}
		repaint();
	};
	textEntry.onEscapeKey = [this] {
		textEntryTarget = 0;
		textEntryButton = -1;
		textEntry.setVisible(false);
		repaint();
	};

	startTimerHz(12);
}

void D110EditorPane::timerCallback() { refreshFromInstrument(); }

void D110EditorPane::selectTab(int index) {
	switch (index) {
	case 1:  tab = Tab::Tone; break;
	case 2:  tab = Tab::Rhythm; break;
	case 3:  tab = Tab::Patches; break;
	case 4:  tab = Tab::Timbres; break;
	case 5:  tab = Tab::Tones; break;
	case 6:  tab = Tab::System; break;
	case 7:  tab = Tab::Monitor; break;
	case 8:  tab = Tab::Soundbanks; soundbankBrowser.refresh(); break;
	case 9:  tab = Tab::Utility; break;
	default: tab = Tab::Parts; break;
	}
	layout();
	repaint();
}

void D110EditorPane::refreshFromInstrument() {
	if (!processor.getCore().isRunning()) {
		if (ramValid) { ramValid = false; repaint(); }
		return;
	}
	// Память перечитывается только когда она действительно менялась - счётчик поколений для
	// того и заведён. Монитор - исключение: лента MIDI и занятость голосов интересны именно
	// тем, как они меняются.
	const uint64_t gen = processor.getCore().ramGeneration();
	if (tab == Tab::Monitor) {
		if (processor.getCore().getRam(ram.data())) { ramGen = gen; ramValid = true; reapplyPendingEdits(); }
		repaint();
		return;
	}
	if (ramValid && gen == ramGen) return;
	if (!processor.getCore().getRam(ram.data())) return;
	ramGen = gen;
	ramValid = true;
	reapplyPendingEdits();
	repaint();
}

void D110EditorPane::reapplyPendingEdits() {
	if (pendingEdits.empty()) return;
	// Дольше любой реально измеренной задержки моста (0-18 мс, note_latency_probe.cpp) с
	// большим запасом, но не бесконечно: если прошивка так и не подтвердила байт за это
	// время, доверять свежепрочитанному значению безопаснее, чем зависнуть на неверном.
	constexpr juce::int64 kGraceMs = 400;
	const juce::int64 now = juce::Time::getMillisecondCounter();
	for (size_t i = 0; i < pendingEdits.size();) {
		const PendingEdit &p = pendingEdits[i];
		const bool confirmed = p.address < ram.size() && ram[p.address] == p.value;
		const bool expired = now - p.sentMs > kGraceMs;
		if (confirmed || expired) {
			pendingEdits.erase(pendingEdits.begin() + long(i));
			continue;
		}
		if (p.address < ram.size()) ram[p.address] = p.value;
		++i;
	}
}

// --- разметка ---------------------------------------------------------------

void D110EditorPane::resized() { layout(); }

void D110EditorPane::layout() {
	cells.clear();
	labels.clear();
	buttons.clear();
	// Unlike every other tab (drawn entirely from the cells/labels/buttons vectors above),
	// SOUNDBANKS hosts one real child Component - see the Tab::Soundbanks case below for why.
	soundbankBrowser.setVisible(tab == Tab::Soundbanks);
	auto area = getLocalBounds().toFloat().reduced(14.0f, 10.0f);
	if (area.getWidth() < 40.0f || area.getHeight() < 40.0f) return;

	const float tabH = juce::jlimit(20.0f, 28.0f, area.getHeight() * 0.06f);
	auto tabs = area.removeFromTop(tabH);
	const float tabW = juce::jmin(120.0f, tabs.getWidth() / float(kNumTabs) - 6.0f);
	for (int i = 0; i < kNumTabs; ++i) {
		tabBounds[(size_t)i] = tabs.removeFromLeft(tabW);
		tabs.removeFromLeft(6.0f);
	}
	optionsButtonBounds = (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone
	                        && tabs.getWidth() > 40.0f)
	                          ? tabs.removeFromRight(juce::jmin(90.0f, tabs.getWidth()))
	                          : juce::Rectangle<float>{};
	area.removeFromTop(8.0f);
	area.removeFromBottom(16.0f);   // полоса под пояснением внизу

	switch (tab) {
	case Tab::Parts:   layoutParts(area); break;
	case Tab::Tone:    layoutTone(area); break;
	case Tab::Rhythm:  layoutRhythm(area); break;
	case Tab::Patches: layoutPatches(area); break;
	case Tab::Timbres: layoutTimbres(area); break;
	case Tab::Tones:   layoutTones(area); break;
	case Tab::System:  layoutSystem(area); break;
	case Tab::Soundbanks:
		contentArea = area;
		soundbankBrowser.setBounds(area.toNearestInt());
		break;
	case Tab::Utility: contentArea = area; layoutUtility(area); break;
	default:           contentArea = area; break;   // монитор рисуется целиком
	}

	// Поле ввода принадлежит месту, а не редактору: со сменой вкладки оно исчезает.
	if (textEntryTarget != 0 && textEntry.isVisible()) {
		textEntryTarget = 0;
		textEntryButton = -1;
		textEntry.setVisible(false);
	}
}

// Timbre Temporary: то, чем прибор играет ПРЯМО СЕЙЧАС. Девять записей по шестнадцать байт,
// одна на партию. Это тот самый блок, который переносится в звуковой движок, поэтому здесь
// правится всё, что слышно, - и всё, что показывает страница PART SET на приборе.
void D110EditorPane::layoutParts(juce::Rectangle<float> area) {
	// MIDI CH's field is a marker, not a Timbre Temporary offset: the channel isn't part of
	// that record at all, it's the System Area's per-part channel array (RAM 0x2D94+13..21,
	// SysEx 0x100000+13..21 - see kMirrorRegions' own comment on that layout), so its cell is
	// built as Area::System below instead of following the others into Area::TimbreTemp.
	constexpr int kMidiChMarker = -2;
	struct Col { const char *head; int field; int hi; float frac; };
	static const Col kCols[] = {
		{ "PART",           -1,   0, 0.000f },
		{ "TONE GROUP",      0,   3, 0.045f },
		{ "TONE",            1,  63, 0.120f },
		{ "MIDI CH", kMidiChMarker, 16, 0.230f },
		{ "LEVEL",           8, 100, 0.330f },
		{ "PAN",             9,  14, 0.395f },
		{ "KEY SHIFT",       2,  48, 0.455f },
		{ "FINE TUNE",       3, 100, 0.545f },
		{ "BENDER",          4,  24, 0.635f },
		{ "ASSIGN",          5,   3, 0.705f },
		{ "OUTPUT",          6,   7, 0.785f },
		{ "KEY LOW",        10, 127, 0.860f },
		{ "KEY HIGH",       11, 127, 0.930f },
	};
	constexpr int kNumCols = int(sizeof(kCols) / sizeof(kCols[0]));
	const float w = area.getWidth();
	auto colAt = [&](int c, juce::Rectangle<float> row) {
		const float right = (c + 1 < kNumCols) ? kCols[c + 1].frac : 1.0f;
		return juce::Rectangle<float>(row.getX() + w * kCols[c].frac, row.getY(),
		                              w * (right - kCols[c].frac) - 8.0f, row.getHeight());
	};

	auto head = area.removeFromTop(18.0f);
	for (int i = 0; i < kNumCols; ++i)
		labels.push_back({ colAt(i, head), kCols[i].head, true });

	tableArea = area;
	rowHeight = juce::jmax(20.0f, area.getHeight() / 9.4f);

	for (int p = 0; p < 9; ++p) {
		auto row = area.removeFromTop(rowHeight).reduced(0.0f, 2.0f);
		labels.push_back({ colAt(0, row), partLabel(p), true });
		for (int i = 1; i < kNumCols; ++i) {
			// У ритм-партии тон не выбирается здесь: её звуки заданы по клавишам, на вкладке
			// RHYTHM, и группа с номером в этой записи ни на что не влияют. Предлагать их
			// значило бы предлагать крутить то, чего не слышно.
			if (p == 8 && (kCols[i].field == 0 || kCols[i].field == 1)) continue;
			if (kCols[i].field == kMidiChMarker) {
				// The array holds one channel per part IN ORDER, rhythm included last -
				// exactly the layout tone_probe/bridge_probe measured (see kMirrorRegions'
				// "System (reserve + channels)" comment: offsets 13..21, part 1..8 then rhythm.
				cells.push_back({ colAt(i, row), Area::System, 13 + p, 0, 0, kCols[i].hi });
				continue;
			}
			cells.push_back({ colAt(i, row), Area::TimbreTemp, p, kCols[i].field, 0,
			                  kCols[i].hi });
		}
	}
}

// Подписи - СОБСТВЕННЫЕ ИМЕНА ПРИБОРА, слово в слово с ламинированной карточки «Tone
// Parameters» (столбец Display): WG Pitch Cors, P-ENV T1, TVF Freq, TVA-ENV Sus L и так далее.
// Раньше здесь стояли термины MT-32 - «cutoff» вместо «frequency» и прочее, - и ящик говорил с
// человеком не теми словами, что панель прибора. Порядок смещений та же карточка подтверждает
// один в один. Static class data (used to be layoutTone()-local) rather than four separate
// copies: randomizeTone() below also walks the full 58-byte partial record, and a second,
// hand-kept copy of every offset/range would drift the moment one of these four changed.
const D110EditorPane::ToneParam D110EditorPane::kWg[] = {
	{ "WG PITCH CORS",   0,  96 }, { "WG PITCH FINE",  1, 100 },
	{ "WG PITCH KF",     2,  16 }, { "WG BENDER SW",   3,   1 },
	{ "WG WAVEFORM",     4,   3 }, { "PCM",            5, 127 },
	{ "WG PULS WIDTH",   6, 100 }, { "WG PW VELO",     7,  14 },
	{ "P-ENV DEPTH",     8,  10 }, { "P-ENV VELO",     9, 100 },
	{ "P-ENV TIME KF",  10,   4 }, { "P-ENV T1",      11, 100 },
	{ "P-ENV T2",       12, 100 }, { "P-ENV T3",      13, 100 },
	{ "P-ENV T4",       14, 100 },
};
const D110EditorPane::ToneParam D110EditorPane::kPitchEnv[] = {
	{ "P-ENV L0",       15, 100 }, { "P-ENV L1",      16, 100 },
	{ "P-ENV L2",       17, 100 }, { "P-ENV SUS L",   18, 100 },
	{ "P-ENV END L",    19, 100 }, { "P-LFO RATE",    20, 100 },
	{ "P-LFO DEPTH",    21, 100 }, { "P-LFO MOD",     22, 100 },
	{ "TVF FREQ",       23, 100 }, { "TVF RESO",      24,  30 },
	{ "TVF FREQ KF",    25,  14 }, { "TVF BIAS P",    26, 127 },
	{ "TVF BIAS LVL",   27,  14 }, { "TVF-ENV DEPT",  28, 100 },
	{ "TVF-ENV VELO",   29, 100 },
};
const D110EditorPane::ToneParam D110EditorPane::kTvf[] = {
	{ "TVF-ENV DKF",    30,   4 }, { "TVF-ENV TKF",   31,   4 },
	{ "TVF-ENV T1",     32, 100 }, { "TVF-ENV T2",    33, 100 },
	{ "TVF-ENV T3",     34, 100 }, { "TVF-ENV T4",    35, 100 },
	{ "TVF-ENV T5",     36, 100 }, { "TVF-ENV L1",    37, 100 },
	{ "TVF-ENV L2",     38, 100 }, { "TVF-ENV L3",    39, 100 },
	{ "TVF-ENV SUS L",  40, 100 }, { "TVA LEVEL",     41, 100 },
	{ "TVA VELOCITY",   42, 100 }, { "TVA BIAS P1",   43, 127 },
	{ "TVA BIAS L1",    44,  12 },
};
const D110EditorPane::ToneParam D110EditorPane::kTva[] = {
	{ "TVA BIAS P2",    45, 127 }, { "TVA BIAS L2",   46,  12 },
	{ "TVA-ENV TKF",    47,   4 }, { "TVA-ENV T1VF",  48,   4 },
	{ "TVA-ENV T1",     49, 100 }, { "TVA-ENV T2",    50, 100 },
	{ "TVA-ENV T3",     51, 100 }, { "TVA-ENV T4",    52, 100 },
	{ "TVA-ENV T5",     53, 100 }, { "TVA-ENV L1",    54, 100 },
	{ "TVA-ENV L2",     55, 100 }, { "TVA-ENV L3",    56, 100 },
	{ "TVA-ENV SUS L",  57, 100 },
};

// Тон партии: та самая 246-байтная запись, которую прибор редактирует своими страницами
// Edit. У D-110 тон - это до ЧЕТЫРЁХ партиалов, и «структура» задаёт, как они соединены
// попарно: сумма или кольцевая модуляция.
void D110EditorPane::layoutTone(juce::Rectangle<float> area) {
	const float w = area.getWidth();

	{
		auto row = area.removeFromTop(20.0f);
		labels.push_back({ row.removeFromLeft(w * 0.05f), "PART", true });
		for (int p = 0; p < 8; ++p) {
			partBounds[(size_t)p] = row.removeFromLeft(28.0f);
			row.removeFromLeft(4.0f);
		}
		// Имя рисуется в paint(), а не запоминается здесь: разметка считается при изменении
		// размера, когда память прибора ещё может быть не прочитана.
		toneNameBounds = row.removeFromLeft(160.0f).reduced(10.0f, 0.0f);
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "click the name to rename the tone this part is playing", false });
		area.removeFromTop(8.0f);
	}

	// LOCK PARTIALS + DEGRADE/RANDOM - a request to add the same "Lock Partials" feature
	// ~/src/D110/edisyn's own D-110 tone editor has (RolandD110Tone), plus two generators of
	// its own: DEGRADE nudges the CURRENT tone a little (a minority of fields, small steps),
	// RANDOM replaces it outright. See setValue()'s own comment for the lock, and
	// randomizeTone() for the two generators.
	{
		// Kept to the LEFT half rather than spanning the row: the memory card widget floats
		// over the drawer's top-right corner (see D110MemoryCard) regardless of tab, so a
		// right-aligned button up here would sit underneath it at some window widths.
		auto row = area.removeFromTop(22.0f);
		buttons.push_back({ row.removeFromLeft(130.0f),
		                     juce::String("LOCK PARTIALS: ") + (lockPartials ? "ON" : "OFF"),
		                     22 });
		row.removeFromLeft(8.0f);
		buttons.push_back({ row.removeFromLeft(90.0f), "DEGRADE", 23 });
		row.removeFromLeft(8.0f);
		buttons.push_back({ row.removeFromLeft(90.0f), "RANDOM", 24 });
		row.removeFromLeft(8.0f);
		labels.push_back({ row.removeFromLeft(juce::jmin(row.getWidth(), w * 0.35f)).reduced(6.0f, 0.0f),
		                    "editing Partial 1 also sets Partials 2-4 to match", false });
		area.removeFromTop(8.0f);
	}

	{
		auto labelRow = area.removeFromTop(14.0f);
		auto boxRow = area.removeFromTop(24.0f);
		const char *kNames[] = { "STRUCTURE 1-2", "STRUCTURE 3-4", "PARTIAL MUTE", "ENV MODE" };
		const int kOffsets[] = { 10, 11, 12, 13 };
		const int kHi[] = { 12, 12, 15, 1 };
		const float cw = w / 4.0f;
		for (int i = 0; i < 4; ++i) {
			const float x = labelRow.getX() + cw * float(i);
			labels.push_back({ juce::Rectangle<float>(x, labelRow.getY(), cw - 10.0f, 14.0f),
			                   kNames[i], true });
			cells.push_back({ juce::Rectangle<float>(x, boxRow.getY(), cw - 10.0f, 24.0f),
			                  Area::ToneTemp, part, kOffsets[i], 0, kHi[i] });
		}
		area.removeFromTop(10.0f);
	}

	// По строке на партиал: то, что слышно сразу. Полная запись партиала - 58 байт, и класть
	// все на один экран значило бы сделать их нечитаемыми, поэтому подробности ниже.
	struct Col { const char *head; int offset; int hi; float frac; };
	static const Col kCols[] = {
		{ "",           -1,   0, 0.00f },
		{ "WG PITCH",    0,  96, 0.07f },
		{ "PITCH FINE",  1, 100, 0.20f },
		{ "WAVEFORM",    4,   3, 0.31f },
		{ "PCM",         5, 127, 0.42f },
		{ "PULS WIDTH",  6, 100, 0.53f },
		{ "TVF FREQ",   23, 100, 0.65f },
		{ "TVF RESO",   24,  30, 0.78f },
		{ "TVA LEVEL",  41, 100, 0.89f },
	};
	constexpr int kNumCols = int(sizeof(kCols) / sizeof(kCols[0]));

	auto head = area.removeFromTop(16.0f);
	for (int i = 0; i < kNumCols; ++i) {
		const float right = (i + 1 < kNumCols) ? kCols[i + 1].frac : 1.0f;
		labels.push_back({ juce::Rectangle<float>(head.getX() + w * kCols[i].frac, head.getY(),
		                                          w * (right - kCols[i].frac) - 8.0f, 16.0f),
		                   kCols[i].head, true });
	}

	const float rowH = juce::jlimit(20.0f, 30.0f, area.getHeight() / 11.0f);
	for (int partial = 0; partial < 4; ++partial) {
		auto row = area.removeFromTop(rowH).reduced(0.0f, 3.0f);
		const int base = 14 + partial * 58;
		tonePartialBounds[(size_t)partial] =
			juce::Rectangle<float>(row.getX(), row.getY(), w * kCols[1].frac - 6.0f,
			                       row.getHeight());
		labels.push_back({ tonePartialBounds[(size_t)partial],
		                   "PARTIAL " + juce::String(partial + 1), partial == tonePartial });
		for (int i = 1; i < kNumCols; ++i) {
			const float right = (i + 1 < kNumCols) ? kCols[i + 1].frac : 1.0f;
			cells.push_back({ juce::Rectangle<float>(row.getX() + w * kCols[i].frac, row.getY(),
			                                         w * (right - kCols[i].frac) - 8.0f,
			                                         row.getHeight()),
			                  Area::ToneTemp, part, base + kCols[i].offset, 0, kCols[i].hi });
		}
	}

	// Подробности выбранного партиала: три огибающие и LFO - то, чем тон на самом деле и
	// делается. Огибающие пятиступенчатые, и последний уровень - это уровень удержания,
	// поэтому подписи именно такие, а не «1..5».
	area.removeFromTop(8.0f);
	labels.push_back({ area.removeFromTop(15.0f),
	                   "PARTIAL " + juce::String(tonePartial + 1)
	                       + " IN FULL - click another partial's name above to switch",
	                   true });
	const int base = 14 + tonePartial * 58;

	const float colW = w / 4.0f;
	auto column = [&](int i) {
		return juce::Rectangle<float>(area.getX() + colW * float(i), area.getY(),
		                              colW - 12.0f, area.getHeight());
	};
	layoutParamColumn(column(0), base, kWg, int(sizeof(kWg) / sizeof(ToneParam)));
	layoutParamColumn(column(1), base, kPitchEnv, int(sizeof(kPitchEnv) / sizeof(ToneParam)));
	layoutParamColumn(column(2), base, kTvf, int(sizeof(kTvf) / sizeof(ToneParam)));
	layoutParamColumn(column(3), base, kTva, int(sizeof(kTva) / sizeof(ToneParam)));
}

void D110EditorPane::layoutParamColumn(juce::Rectangle<float> column, int partialBase,
                                       const ToneParam *params, int count) {
	if (count <= 0) return;
	const float rowH = juce::jlimit(14.0f, 22.0f, column.getHeight() / float(count));
	for (int i = 0; i < count; ++i) {
		auto row = column.removeFromTop(rowH);
		if (row.getHeight() < 10.0f) return;
		labels.push_back({ row.removeFromLeft(column.getWidth() * 0.60f), params[i].name, false });
		cells.push_back({ row.reduced(0.0f, 1.0f), Area::ToneTemp, part,
		                  partialBase + params[i].offset, 0, params[i].hi });
	}
}

// Установка ударных: одна строка на клавишу. Записей восемьдесят пять - столько на экран не
// помещается, поэтому показывается окно, а список листается колесом мыши мимо полей.
void D110EditorPane::layoutRhythm(juce::Rectangle<float> area) {
	auto head = area.removeFromTop(18.0f);
	tableArea = area;
	const float w = area.getWidth();
	rowHeight = juce::jlimit(18.0f, 30.0f, area.getHeight() / 14.0f);
	const int visible = juce::jmax(1, int(area.getHeight() / rowHeight));
	rhythmScroll = juce::jlimit(0, juce::jmax(0, D110CoreType::kNumRhythmKeys - visible), rhythmScroll);

	const float colFrac[5] = { 0.00f, 0.16f, 0.52f, 0.68f, 0.84f };
	const char *kHead[5] = { "KEY", "DRUM SOUND", "LEVEL", "PAN", "OUTPUT" };
	auto colAt = [&](int c, juce::Rectangle<float> row) {
		const float right = (c + 1 < 5) ? colFrac[c + 1] : 1.0f;
		return juce::Rectangle<float>(row.getX() + w * colFrac[c], row.getY(),
		                              w * (right - colFrac[c]) - 8.0f, row.getHeight());
	};
	for (int i = 0; i < 5; ++i)
		labels.push_back({ colAt(i, head), kHead[i], true });

	for (int i = 0; i < visible; ++i) {
		const int slot = rhythmScroll + i;
		if (slot >= D110CoreType::kNumRhythmKeys) break;
		auto row = area.removeFromTop(rowHeight).reduced(0.0f, 2.0f);
		const int key = D110CoreType::kRhythmFirstKey + slot;
		labels.push_back({ colAt(0, row),
		                   juce::String(key) + "  " + noteName(key), true });
		// Диапазон 0..127, а не MT-32-шные 0..94: заводская установка ударных D-110 держит
		// значения до 0x67 = 103, что за пределом MT-32 - измерено по батарейному ОЗУ.
		cells.push_back({ colAt(1, row), Area::Rhythm, slot, 0, 0, 127 });
		cells.push_back({ colAt(2, row), Area::Rhythm, slot, 1, 0, 100 });
		cells.push_back({ colAt(3, row), Area::Rhythm, slot, 2, 0, 14 });
		cells.push_back({ colAt(4, row), Area::Rhythm, slot, 3, 0, 7 });
	}
}

// Память патчей: 64 записи по 128 байт. Патч у D-110 - это ВЕСЬ прибор разом: имя,
// ревербератор, резерв партиалов, карта каналов и назначение восьми партий. Щелчок по
// номеру просит прошивку перейти на этот патч её собственными кнопками.
void D110EditorPane::layoutPatches(juce::Rectangle<float> area) {
	const float w = area.getWidth();

	{
		auto row = area.removeFromTop(20.0f);
		labels.push_back({ row.removeFromLeft(w * 0.30f),
		                   "PATCH MEMORY - THE 64 THE INSTRUMENT CALLS I-11 TO I-88", true });
		labels.push_back({ row.reduced(10.0f, 0.0f),
		                   "click a number and the instrument selects it, exactly as its own "
		                   "PATCH, BANK and NUMBER buttons do", false });
		area.removeFromTop(6.0f);
	}

	// Sub-tab strip - see PatchesSubTab's own comment for why this replaced a fixed
	// vertical split of the two views. Painted in paint()'s own Patches-specific block,
	// right below the main tab strip's painting; bounds only computed here.
	{
		auto row = area.removeFromTop(20.0f);
		patchesSubTabBounds[0] = row.removeFromLeft(w * 0.16f);
		row.removeFromLeft(w * 0.01f);
		patchesSubTabBounds[1] = row.removeFromLeft(w * 0.18f);
		area.removeFromTop(6.0f);
	}

	if (patchesSubTab == PatchesSubTab::AllPatches) {
		layoutPatchesList(area);
	} else {
		layoutPatchesParts(area);
	}
}

void D110EditorPane::layoutPatchesList(juce::Rectangle<float> area) {
	const float w = area.getWidth();
	auto listArea = area;

	const float colFrac[5] = { 0.00f, 0.09f, 0.42f, 0.58f, 0.75f };
	const char *kHead[5] = { "PATCH", "NAME", "REVERB TYPE", "REVERB TIME", "REVERB LEVEL" };
	auto colAt = [&](int c, juce::Rectangle<float> row) {
		const float right = (c + 1 < 5) ? colFrac[c + 1] : 1.0f;
		return juce::Rectangle<float>(row.getX() + w * colFrac[c], row.getY(),
		                              w * (right - colFrac[c]) - 8.0f, row.getHeight());
	};
	auto head = listArea.removeFromTop(16.0f);
	for (int i = 0; i < 5; ++i)
		labels.push_back({ colAt(i, head), kHead[i], true });

	tableArea = listArea;
	rowHeight = juce::jlimit(16.0f, 24.0f, listArea.getHeight() / 13.0f);
	const int rows = juce::jmax(1, int(listArea.getHeight() / rowHeight));
	patchScroll = juce::jlimit(0, juce::jmax(0, D110CoreType::kNumPatches - rows), patchScroll);

	for (int i = 0; i < rows; ++i) {
		const int patch = patchScroll + i;
		if (patch >= D110CoreType::kNumPatches) break;
		auto row = listArea.removeFromTop(rowHeight).reduced(0.0f, 2.0f);
		// Номер - кнопка: по ней прибор переходит на этот патч. Подписан так же, как его
		// показывает индикатор: банк 1-8 и номер 1-8.
		buttons.push_back({ colAt(0, row),
		                    "I-" + juce::String(patch / 8 + 1) + juce::String(patch % 8 + 1),
		                    200 + patch });
		buttons.push_back({ colAt(1, row), {}, 400 + patch });   // имя, набирается на месте
		cells.push_back({ colAt(2, row), Area::Patches, patch, 10, 0, 8 });
		cells.push_back({ colAt(3, row), Area::Patches, patch, 11, 0, 7 });
		cells.push_back({ colAt(4, row), Area::Patches, patch, 12, 0, 7 });
	}
}

// The PARTS OF PATCH sub-tab - what a patch puts into the 8 parts when it's selected. Its
// own function (rather than the tail end of layoutPatchesList()) since PatchesSubTab means
// only one of the two is ever laid out per pass, each getting the tab's whole height.
void D110EditorPane::layoutPatchesParts(juce::Rectangle<float> area) {
	const float w = area.getWidth();
	const int chosen = juce::jlimit(0, D110CoreType::kNumPatches - 1, patchSlot);
	labels.push_back({ area.removeFromTop(15.0f),
	                   "PARTS OF PATCH I-" + juce::String(chosen / 8 + 1)
	                       + juce::String(chosen % 8 + 1)
	                       + "   - what it puts into the parts when it is selected",
	                   true });

	// MIDI CH is a marker, not an offset-within-the-part-record field: the channel map is its
	// own 9-byte block at patch offset 22-30 (one byte per part, rhythm last), sitting BEFORE
	// the part records that start at 31 - not inside them. Confirmed empirically
	// (plugin/native_timbre_group_number_probe.cpp): a freshly factory-reset patch 0 reads
	// "4 4 4 4 3 3 3 2 5" at offset 13 (the reserve block right before it) and "1 2 3 4 5 6 7
	// 8 9" at offset 22 - exactly the reserve/channel-map values docs/factory_defaults.md
	// already confirmed for the LIVE System-area copy at 0x2D98/0x2DA1, byte for byte.
	constexpr int kChannelMarker = -2;
	struct PCol { const char *head; int field; int hi; float frac; };
	static const PCol kPCols[] = {
		{ "PART",       -1,   0, 0.000f },
		{ "TONE GROUP",  0,   3, 0.045f },
		{ "TONE",        1,  63, 0.150f },
		{ "MIDI CH", kChannelMarker, 16, 0.260f },
		{ "LEVEL",       8, 100, 0.360f },
		{ "PAN",         9,  14, 0.425f },
		{ "KEY SHIFT",   2,  48, 0.485f },
		{ "FINE TUNE",   3, 100, 0.575f },
		{ "BENDER",      4,  24, 0.665f },
		{ "ASSIGN",      5,   3, 0.735f },
		{ "OUTPUT",      6,   7, 0.815f },
		{ "KEY LOW",    10, 127, 0.880f },
		{ "KEY HIGH",   11, 127, 0.940f },
	};
	constexpr int kNumPCols = int(sizeof(kPCols) / sizeof(kPCols[0]));
	auto pcolAt = [&](int c, juce::Rectangle<float> row) {
		const float right = (c + 1 < kNumPCols) ? kPCols[c + 1].frac : 1.0f;
		return juce::Rectangle<float>(row.getX() + w * kPCols[c].frac, row.getY(),
		                              w * (right - kPCols[c].frac) - 8.0f, row.getHeight());
	};
	auto phead = area.removeFromTop(16.0f);
	for (int i = 0; i < kNumPCols; ++i)
		labels.push_back({ pcolAt(i, phead), kPCols[i].head, true });

	const float prowH = juce::jlimit(16.0f, 28.0f, area.getHeight() / 8.5f);
	for (int p = 0; p < 8; ++p) {
		auto row = area.removeFromTop(prowH).reduced(0.0f, 2.0f);
		labels.push_back({ pcolAt(0, row), partLabel(p), true });
		for (int i = 1; i < kNumPCols; ++i) {
			if (kPCols[i].field == kChannelMarker) {
				cells.push_back({ pcolAt(i, row), Area::Patches, chosen, 22 + p, 0, kPCols[i].hi });
				continue;
			}
			// Запись партии - те же двенадцать байт, что и в Timbre Temporary, начиная с
			// 31-го байта патча: имя 10, ревербератор 3, резерв 9, каналы 9. Измерено
			// сличением с заводским содержимым - панорама этих восьми записей совпала с
			// заводским веером 4 10 6 8 2 12 0 14 байт в байт.
			cells.push_back({ pcolAt(i, row), Area::Patches, chosen,
			                  31 + p * 12 + kPCols[i].field, 0, kPCols[i].hi });
		}
	}
}

// Память тембров: 128 записей по 8 байт. Тембр у D-110 - это НАЗНАЧЕНИЕ тона: какой тон
// играть, с каким сдвигом, подстройкой, диапазоном колеса и реверберацией. Щелчок по номеру
// посылает смену программы на канал выбранной партии - как с внешней клавиатуры.
void D110EditorPane::layoutTimbres(juce::Rectangle<float> area) {
	const float w = area.getWidth();

	{
		auto row = area.removeFromTop(22.0f);
		labels.push_back({ row.removeFromLeft(w * 0.16f), "TIMBRE MEMORY - 128", true });
		labels.push_back({ row.removeFromLeft(w * 0.05f), "PART", true });
		for (int p = 0; p < 8; ++p) {
			partBounds[(size_t)p] = row.removeFromLeft(28.0f);
			row.removeFromLeft(4.0f);
		}
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "click a timbre number to send that program change on the chosen "
		                   "part's own MIDI channel", false });
		area.removeFromTop(6.0f);
	}

	const float colFrac[8] = { 0.00f, 0.07f, 0.17f, 0.42f, 0.53f, 0.64f, 0.75f, 0.87f };
	const char *kHead[8] = { "TIMBRE", "GROUP", "TONE", "KEY SHIFT", "FINE TUNE",
	                         "BENDER", "ASSIGN", "OUTPUT" };
	auto colAt = [&](int c, juce::Rectangle<float> row) {
		const float right = (c + 1 < 8) ? colFrac[c + 1] : 1.0f;
		return juce::Rectangle<float>(row.getX() + w * colFrac[c], row.getY(),
		                              w * (right - colFrac[c]) - 8.0f, row.getHeight());
	};
	auto head = area.removeFromTop(16.0f);
	for (int i = 0; i < 8; ++i)
		labels.push_back({ colAt(i, head), kHead[i], true });

	tableArea = area;
	rowHeight = juce::jlimit(18.0f, 28.0f, area.getHeight() / 14.0f);
	const int rows = juce::jmax(1, int(area.getHeight() / rowHeight));
	timbreScroll = juce::jlimit(0, juce::jmax(0, D110CoreType::kNumTimbres - rows), timbreScroll);

	for (int i = 0; i < rows; ++i) {
		const int slot = timbreScroll + i;
		if (slot >= D110CoreType::kNumTimbres) break;
		auto row = area.removeFromTop(rowHeight).reduced(0.0f, 2.0f);
		buttons.push_back({ colAt(0, row), juce::String(slot + 1), 600 + slot });
		cells.push_back({ colAt(1, row), Area::Timbres, slot, 0, 0, 3 });
		cells.push_back({ colAt(2, row), Area::Timbres, slot, 1, 0, 63 });
		cells.push_back({ colAt(3, row), Area::Timbres, slot, 2, 0, 48 });
		cells.push_back({ colAt(4, row), Area::Timbres, slot, 3, 0, 100 });
		cells.push_back({ colAt(5, row), Area::Timbres, slot, 4, 0, 24 });
		cells.push_back({ colAt(6, row), Area::Timbres, slot, 5, 0, 3 });
		cells.push_back({ colAt(7, row), Area::Timbres, slot, 6, 0, 7 });
	}
}

// Память тонов: 64 ячейки по 256 байт, верхняя половина батарейного ОЗУ. Это те тона,
// которые тембр называет группой INTERNAL. Тон целиком - 246 байт, поэтому он не правится
// здесь по полю, а переносится целиком: из партии в ячейку и обратно.
void D110EditorPane::layoutTones(juce::Rectangle<float> area) {
	const float w = area.getWidth();

	labels.push_back({ area.removeFromTop(15.0f),
	                   "TONE MEMORY - THE 64 SLOTS A TIMBRE CALLS 'INTERNAL'", true });
	{
		auto row = area.removeFromTop(26.0f);
		labels.push_back({ row.removeFromLeft(w * 0.10f),
		                   "SLOT " + juce::String(toneSlot + 1), true });
		labels.push_back({ row.removeFromLeft(w * 0.04f), "PART", true });
		for (int p = 0; p < 8; ++p) {
			partBounds[(size_t)p] = row.removeFromLeft(28.0f);
			row.removeFromLeft(4.0f);
		}
		row.removeFromLeft(10.0f);
		buttons.push_back({ row.removeFromLeft(150.0f), "STORE PART'S TONE", 20 });
		row.removeFromLeft(8.0f);
		buttons.push_back({ row.removeFromLeft(150.0f), "RECALL INTO PART", 21 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "246 bytes each way, as an external librarian would send them",
		                   false });
		area.removeFromTop(10.0f);
	}

	tableArea = area;
	rowHeight = juce::jlimit(18.0f, 26.0f, area.getHeight() / 12.0f);
	const int rows = juce::jmax(1, int(area.getHeight() / rowHeight));
	toneRows = rows;
	toneScroll = juce::jlimit(0, juce::jmax(0, D110CoreType::kNumTones - rows * 3), toneScroll);
	const float colW = w / 3.0f;
	for (int col = 0; col < 3; ++col)
		for (int r = 0; r < rows; ++r) {
			const int slot = toneScroll + col * rows + r;
			if (slot >= D110CoreType::kNumTones) break;
			buttons.push_back({ juce::Rectangle<float>(area.getX() + colW * float(col),
			                                           area.getY() + rowHeight * float(r),
			                                           colW - 12.0f, rowHeight - 3.0f),
			                    {}, 100 + slot });
		}
}

void D110EditorPane::layoutSystem(juce::Rectangle<float> area) {
	const float w = area.getWidth();
	const float labelH = 15.0f;
	const float used = 3.0f * labelH + 2.0f * labelH * 0.85f;
	const float boxH = juce::jlimit(26.0f, 44.0f, (area.getHeight() - used) / 4.2f);
	// Промежуток ограничен сверху: на этой вкладке всего три блока, и на высоком окне
	// оставшееся место растягивало их до края экрана, так что подпись блока оказывалась
	// в полусотне точек от своих же полей.
	const float gap = juce::jlimit(10.0f, 40.0f, (area.getHeight() - used - 3.0f * boxH) / 3.0f);

	{
		auto labelRow = area.removeFromTop(labelH);
		auto boxRow = area.removeFromTop(boxH);
		const char *kNames[] = { "MASTER TUNE", "REVERB TYPE", "REVERB TIME", "REVERB LEVEL" };
		const int kHi[] = { 127, 8, 7, 7 };
		const float cw = w / 4.0f;
		for (int i = 0; i < 4; ++i) {
			const float x = labelRow.getX() + cw * float(i);
			labels.push_back({ juce::Rectangle<float>(x, labelRow.getY(), cw - 10.0f, labelH),
			                   kNames[i], true });
			cells.push_back({ juce::Rectangle<float>(x, boxRow.getY(), cw - 10.0f, boxH),
			                  Area::System, i, 0, 0, kHi[i] });
		}
		area.removeFromTop(gap);
	}

	const char *kBlocks[] = { "PARTIAL RESERVE - 32 PARTIALS SHARED OUT, AND THEY MUST SUM TO 32",
	                          "MIDI CHANNEL" };
	for (int block = 0; block < 2; ++block) {
		labels.push_back({ area.removeFromTop(labelH).withWidth(w * 0.6f), kBlocks[block], true });
		auto partRow = area.removeFromTop(labelH * 0.85f);
		auto boxRow = area.removeFromTop(boxH);
		const float cw = w / 9.0f;
		for (int i = 0; i < 9; ++i) {
			const juce::Rectangle<float> r(boxRow.getX() + cw * float(i), boxRow.getY(),
			                               cw - 10.0f, boxH);
			labels.push_back({ juce::Rectangle<float>(r.getX(), partRow.getY(), r.getWidth(),
			                                          partRow.getHeight()),
			                   partLabel(i), false, juce::Justification::centred });
			cells.push_back({ r, Area::System, (block == 0 ? 4 : 13) + i, 0, 0,
			                  block == 0 ? 32 : 16 });
		}
		area.removeFromTop(gap);
	}
}

void D110EditorPane::layoutUtility(juce::Rectangle<float> area) {
	// Gutter for the scrollbar, reserved even when it turns out not to be needed - so no
	// field's right edge ever has to jump sideways depending on whether scrolling is active.
	area.removeFromRight(10.0f);
	const float visibleH = area.getHeight();
	// Rectangle::removeFromTop() CLAMPS once its source rectangle runs out of height instead
	// of going negative - which is exactly the "sections keep landing at the same spot" bug
	// this hit the first time: if area were left at its real (short) visible height, every
	// removeFromTop() below would silently stop shrinking it the moment content overflowed,
	// so utilityContentHeight (measured from how much area shrank) would floor at visibleH
	// and never register as "too tall" - no matter how much actually overflowed. A tall
	// working rectangle - comfortably above any realistic content total - sidesteps that; the
	// real visible height (visibleH, captured just above) is what the clamp/scrollbar math
	// below is measured against instead.
	constexpr float kWorkingH = 6000.0f;
	area.setHeight(kWorkingH);
	// Shifted up by however far down the list we are; every section below still lays out with
	// plain top-down removeFromTop() calls; the ones scrolled above the tab strip or below the
	// caption strip just end up clipped by paint()'s clip region for this tab.
	area.translate(0.0f, -utilityScrollOffset);
	const float w = area.getWidth();

	labels.push_back({ area.removeFromTop(15.0f), "MIDI PANIC", true });
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(150.0f), "MIDI PANIC", 9 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "kills every note currently sounding, on the engine and the "
		                   "firmware both, on all 16 MIDI channels - for the rare case "
		                   "something leaves notes hung", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "WINDOW SIZE", true });
	{
		auto row = area.removeFromTop(28.0f);
		const int currentPercent = juce::roundToInt(float(getWidth()) / float(D110Panel::currentRefW(processor.getCompactPanelMode())) * 100.0f);
		zoomBounds = row.removeFromLeft(150.0f);
		buttons.push_back({ zoomBounds, juce::String(currentPercent) + "%", 10 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "click to step through 50/75/100/125/150%, right-click to pick "
		                   "one directly - a plain resize, not a window manager maximise "
		                   "(see the panel notes on why)", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "THEME", true });
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(150.0f),
		                    d110ui::getTheme() == d110ui::Theme::Light ? "LIGHT" : "DARK", 11 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "click to switch this drawer's own colours between dark and "
		                   "light - the photographed panel itself always keeps its own "
		                   "colours", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "FONT SIZE", true });
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(150.0f), processor.getUiFontScaleBig() ? "BIG" : "NORMAL", 26 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "click to switch every label and control between normal size and "
		                   "a larger one for HDPI screens with no OS-level scaling - "
		                   "Standalone only, a VST3/AU host window can't be resized this way",
		                   false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "SEQUENCER", true });
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(150.0f), processor.getSequencerRetroMode() ? "RETRO" : "NORMAL", 13 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "click to switch the sequencer drawer between the mouse-driven "
		                   "grid and the D-20-style LCD+buttons view - same toggle as the "
		                   "panel's own right-click Options menu", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "QUANTIZE MODE", true });
	{
		auto row = area.removeFromTop(28.0f);
		const bool soft = processor.getSequencer().getQuantizeMode() == d110seq::QuantizeMode::soft;
		buttons.push_back({ row.removeFromLeft(150.0f), soft ? "SOFT" : "HARD", 15 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "HARD moves a track's own recorded notes onto the grid for good; "
		                   "SOFT leaves them exactly as played and only snaps them live during "
		                   "playback - picking OFF on a track's own quantize control then plays "
		                   "the original recording again, unchanged", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "PANEL SIZE", true });
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(150.0f), processor.getCompactPanelMode() ? "COMPACT" : "FULL", 14 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "compact splices out the Roland wordmark, PHONES jack and MEMORY CARD "
		                   "slot and narrows the window to match, keeping only VOLUME, the LCD, "
		                   "the button grid and POWER - the card becomes unreachable while it's "
		                   "on, switch back to FULL to use it again", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "DEBUG", true });
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(150.0f),
		                    processor.getDebugModeEnabled() ? "LOGGING ON" : "LOGGING OFF", 12 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "when on, writes a running note-attempt tally to "
		                   "~/d110_diagnostic_log.txt every few seconds - off by default, only "
		                   "useful while chasing a real playback issue", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "ROM FOLDER", true });
	{
		auto row = area.removeFromTop(28.0f);
		const auto custom = D110AudioProcessor::getCustomRomFolder();
		romFolderBounds = row.removeFromLeft(220.0f);
		buttons.push_back({ romFolderBounds, custom.isEmpty() ? juce::String("AUTO") : custom, 16 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "click to pick a folder to search for the ROM files, instead of the "
		                   "usual automatic locations (beside the plugin/binary, or its own data "
		                   "folder) - right-click to go back to AUTO", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "SOUNDBANKS FOLDER", true });
	{
		auto row = area.removeFromTop(28.0f);
		const auto custom = D110AudioProcessor::getSoundbankSourceFolder();
		soundbankFolderBounds = row.removeFromLeft(220.0f);
		buttons.push_back({ soundbankFolderBounds,
		                     custom.isEmpty() ? juce::String("NOT SET") : custom, 25 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "click to pick the folder of SysEx patch libraries the SOUNDBANKS "
		                   "tab's RESCAN button scans - right-click to clear", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "MESSAGE ON THE INSTRUMENT'S OWN DISPLAY",
	                   true });
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(w * 0.40f), {}, 3 });   // само поле ввода
		row.removeFromLeft(10.0f);
		buttons.push_back({ row.removeFromLeft(100.0f), "SEND", 1 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "twenty characters, as Roland's own command carries them - this "
		                   "display shows the first sixteen", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "BANKS", true });
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(190.0f), "IMPORT SysEx / MIDI BANK", 2 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "the file goes to the instrument whole, exactly as a librarian "
		                   "would pour it into MIDI IN", false });
	}
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(190.0f), "EXPORT SysEx BANK...", 7 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "built straight from memory, instantly, rather than captured off a "
		                   "live transfer - a real Roland dump, playable back in above or into "
		                   "actual hardware over MIDI", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "MEMORY SNAPSHOT", true });
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(150.0f), "SAVE SNAPSHOT...", 5 });
		row.removeFromLeft(10.0f);
		buttons.push_back({ row.removeFromLeft(150.0f), "LOAD SNAPSHOT...", 6 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "every patch, timbre, system setting and the memory card, in one "
		                   "file - loading one powers the instrument off and back on with that "
		                   "memory in place", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "FACTORY INITIALISATION", true });
	labels.push_back({ area.removeFromTop(18.0f),
	                   "The instrument has its own, and this plugin can perform it exactly: "
	                   "POWER off, Ctrl+click WRITE/COPY to latch the cap down, POWER on, "
	                   "Ctrl+click WRITE/COPY again to release it, then ENTER to confirm - "
	                   "release before confirming, not after. There is no button for it here, "
	                   "because there is none on the hardware either.", false });
	area.removeFromTop(12.0f);

	labels.push_back({ area.removeFromTop(15.0f), "WHERE THE EDITS GO", true });
	labels.push_back({ area.removeFromTop(18.0f),
	                   "Every field in this drawer sends the instrument a Roland exclusive "
	                   "message - the firmware changes its own memory, and the sound engine is "
	                   "brought into line from there. Nothing here writes to the sound engine "
	                   "behind the instrument's back.", false });
	area.removeFromTop(12.0f);

	labels.push_back({ area.removeFromTop(15.0f), "REFERENCE", true });
	{
		auto row = area.removeFromTop(28.0f);
		buttons.push_back({ row.removeFromLeft(150.0f), "LA REFERENCE", 8 });
		labels.push_back({ row.reduced(12.0f, 0.0f),
		                   "LA synthesis parameter reference sheet (from the D-20, the D-110's "
		                   "sibling with the same LA32 engine), for looking things up while "
		                   "editing tones", false });
	}
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), D110AudioProcessor::kExtendedPolyphonyLabel, true });
	labels.push_back({ area.removeFromTop(28.0f),
	                   "The real D-110 shares just 32 synthesis voices across all nine parts, "
	                   "and fast, overlapping playing on more than one part at once can "
	                   "genuinely run out and drop notes - measured, not assumed "
	                   "(plugin/multi_part_polyphony_probe.cpp). This is our own sound engine, "
	                   "so it isn't held to that ceiling: it renders with four times the "
	                   "voices, measured comfortably cheap on a modern CPU even with all nine "
	                   "parts playing at once. Everything else about the instrument - the "
	                   "firmware, the panel, the sound itself - is unchanged.", false });

	// How much vertical space every section above actually consumed, regardless of scroll
	// position (translate() earlier moved the content, not how much of it there is) - this is
	// what the scrollbar's own thumb size and the clamp below are measured against.
	utilityContentHeight = kWorkingH - area.getHeight();
	const float maxScroll = juce::jmax(0.0f, utilityContentHeight - visibleH);
	utilityScrollOffset = juce::jlimit(0.0f, maxScroll, utilityScrollOffset);

	if (utilityContentHeight > visibleH) {
		constexpr float kTrackW = 8.0f;
		utilityScrollTrack = { contentArea.getRight() - kTrackW, contentArea.getY(),
		                       kTrackW, contentArea.getHeight() };
		const float thumbH = juce::jmax(24.0f, visibleH * (visibleH / utilityContentHeight));
		const float thumbY = contentArea.getY()
		                    + (contentArea.getHeight() - thumbH) * (utilityScrollOffset / maxScroll);
		utilityScrollThumb = { utilityScrollTrack.getX(), thumbY, kTrackW, thumbH };
	} else {
		utilityScrollTrack = utilityScrollThumb = juce::Rectangle<float>();
	}
}

namespace {
// DialogWindow::LaunchOptions::launchAsync() always calls enterModalState(true, ...) - an
// app-modal dialog, which blocked clicking the rest of the editor while the reference image
// was open (Alan's report). A plain top-level DocumentWindow behaves like any other
// independent window instead - closable and resizable, but not modal - which is all this
// popup ever needed. Self-deletes on close, the same ownership contract the dialog had.
class ImagePopupWindow : public juce::DocumentWindow {
public:
	ImagePopupWindow(const juce::String &title, juce::Component *ownedContent)
		: DocumentWindow(title, juce::Colours::black, DocumentWindow::closeButton) {
		setUsingNativeTitleBar(true);
		setContentOwned(ownedContent, true);
		setResizable(true, true);
		centreWithSize(getWidth(), getHeight());
		setVisible(true);
	}
	void closeButtonPressed() override { delete this; }
};

// Content of SysexTonePickerWindow below - a checklist of every internal Tone Memory record
// decoded from a SysEx/MIDI file that came in with more than Bank I's own 64 slots. Plain stock
// JUCE widgets (ListBox/TextButton/Label), not this app's usual hand-painted Cell/Button
// drawing - same call already made for SoundbankBrowser.cpp's confirm-overwrite AlertWindow:
// a one-off utility dialog outside the main panel doesn't need to match its custom rendering,
// and a real ListBox gets scrolling/keyboard nav for free on what could be a very long list.
class SysexTonePickerContent : public juce::Component, private juce::ListBoxModel {
public:
	SysexTonePickerContent(D110AudioProcessor &proc, juce::File source,
	                       std::vector<d110bank::DecodedTone> tonesIn)
		: processor(proc), sourceFile(std::move(source)), tones(std::move(tonesIn)),
		  selected(tones.size(), false) {
		header.setText(juce::String(int(tones.size())) + " tones found, which is above the 64 "
		               "tone limit of Bank I - select which ones you want to import.",
		               juce::dontSendNotification);
		header.setColour(juce::Label::textColourId, juce::Colours::white);
		header.setJustificationType(juce::Justification::topLeft);
		addAndMakeVisible(header);

		list.setModel(this);
		list.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff1a1a1a));
		list.setColour(juce::ListBox::outlineColourId, juce::Colours::grey);
		list.setRowHeight(22);
		list.updateContent();
		addAndMakeVisible(list);

		counter.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
		addAndMakeVisible(counter);
		updateCounter();

		selectFirst64.setButtonText("Select first 64");
		selectFirst64.onClick = [this] {
			std::fill(selected.begin(), selected.end(), false);
			for (size_t i = 0; i < selected.size() && i < 64; ++i) selected[i] = true;
			list.repaint();
			updateCounter();
		};
		addAndMakeVisible(selectFirst64);

		clearAll.setButtonText("Clear");
		clearAll.onClick = [this] {
			std::fill(selected.begin(), selected.end(), false);
			list.repaint();
			updateCounter();
		};
		addAndMakeVisible(clearAll);

		importButton.setButtonText("Import Selected");
		importButton.onClick = [this] { doImport(); };
		addAndMakeVisible(importButton);

		splitButton.setButtonText("Split into files instead...");
		splitButton.onClick = [this] { doSplit(); };
		addAndMakeVisible(splitButton);

		cancelButton.setButtonText("Cancel");
		cancelButton.onClick = [this] { closeParentWindow(); };
		addAndMakeVisible(cancelButton);

		setSize(560, 520);
	}

	void resized() override {
		auto area = getLocalBounds().reduced(10);
		header.setBounds(area.removeFromTop(48));
		area.removeFromTop(6);
		auto buttonsRow = area.removeFromBottom(28);
		cancelButton.setBounds(buttonsRow.removeFromRight(80));
		buttonsRow.removeFromRight(6);
		splitButton.setBounds(buttonsRow.removeFromRight(180));
		buttonsRow.removeFromRight(6);
		importButton.setBounds(buttonsRow.removeFromRight(140));
		area.removeFromBottom(6);
		auto counterRow = area.removeFromBottom(24);
		selectFirst64.setBounds(counterRow.removeFromLeft(120));
		counterRow.removeFromLeft(6);
		clearAll.setBounds(counterRow.removeFromLeft(70));
		counterRow.removeFromLeft(10);
		counter.setBounds(counterRow);
		area.removeFromBottom(6);
		list.setBounds(area);
	}

	void paint(juce::Graphics &g) override { g.fillAll(juce::Colour(0xff202020)); }

private:
	int getNumRows() override { return int(tones.size()); }

	void paintListBoxItem(int row, juce::Graphics &g, int w, int h, bool) override {
		if (row < 0 || row >= int(tones.size())) return;
		g.fillAll(juce::Colour(0xff1a1a1a));
		const bool on = selected[size_t(row)];
		g.setColour(on ? juce::Colours::yellow : juce::Colours::grey);
		g.drawRect(4, h / 2 - 6, 12, 12, 1);
		if (on) g.fillRect(6, h / 2 - 4, 8, 8);
		g.setColour(juce::Colours::white);
		g.drawText(juce::String(row + 1).paddedLeft('0', 3) + "  " + tones[size_t(row)].name,
		           24, 0, w - 28, h, juce::Justification::centredLeft);
	}

	void listBoxItemClicked(int row, const juce::MouseEvent &) override {
		if (row < 0 || row >= int(tones.size())) return;
		if (!selected[size_t(row)] && countSelected() >= 64) return; // at the cap - ignore more picks
		selected[size_t(row)] = !selected[size_t(row)];
		list.repaintRow(row);
		updateCounter();
	}

	int countSelected() const { return int(std::count(selected.begin(), selected.end(), true)); }

	void updateCounter() {
		const int n = countSelected();
		counter.setText(juce::String(n) + " / 64 selected", juce::dontSendNotification);
		importButton.setEnabled(n > 0);
	}

	void closeParentWindow() {
		if (auto *dw = findParentComponentOfClass<juce::DocumentWindow>()) dw->closeButtonPressed();
	}

	void doImport() {
		std::vector<d110bank::DecodedTone> chosen;
		for (size_t i = 0; i < tones.size(); ++i)
			if (selected[i]) chosen.push_back(tones[i]);
		// A temp file, not written alongside the source: this is a one-shot transient bank, not
		// something meant to be kept around and reimported later - see "Split into files
		// instead..." below for the case Alan does want files kept.
		const auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
			.getChildFile("d110_import_selected_"
			              + juce::String(juce::Time::getCurrentTime().toMilliseconds()) + ".syx");
		if (d110bank::buildToneBankFile(chosen, tempFile)) processor.importSysexBank(tempFile);
		closeParentWindow();
	}

	void doSplit() {
		const auto parts = d110bank::splitToneSysexFile(sourceFile);
		if (parts.empty()) return;
		int totalTones = 0;
		for (const auto &p : parts) totalTones += p.toneCount;
		juce::PopupMenu menu;
		menu.addSectionHeader(juce::String(totalTones) + " tones split into "
		                       + juce::String(int(parts.size())) + " files, alongside the source");
		for (size_t i = 0; i < parts.size(); ++i)
			menu.addItem(int(i) + 1, "Import part " + juce::String(i + 1) + " ("
			                             + juce::String(parts[i].toneCount) + " tones) into Bank I");
		menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&splitButton),
			[this, parts](int result) {
				if (result <= 0) return; // cancelled
				processor.importSysexBank(parts[size_t(result - 1)].file);
				closeParentWindow();
			});
	}

	D110AudioProcessor &processor;
	juce::File sourceFile;
	std::vector<d110bank::DecodedTone> tones;
	std::vector<bool> selected;
	juce::Label header, counter;
	juce::ListBox list;
	juce::TextButton selectFirst64, clearAll, importButton, splitButton, cancelButton;
};

// Shown from D110EditorPane's "IMPORT SysEx / MIDI BANK" (see buttonPressed(), id==2) whenever
// the picked file decodes to more than 64 internal Tone Memory records - Alan's request,
// 2026-09-01: rather than importing the file blind and losing whatever didn't fit in Bank I's 64
// slots, ask which ones to keep. Not modal, self-deletes on close - same reasoning as
// ImagePopupWindow above.
class SysexTonePickerWindow : public juce::DocumentWindow {
public:
	SysexTonePickerWindow(D110AudioProcessor &proc, const juce::File &source,
	                      std::vector<d110bank::DecodedTone> tones)
		: DocumentWindow("Select tones to import", juce::Colour(0xff202020), DocumentWindow::closeButton) {
		setUsingNativeTitleBar(true);
		setContentOwned(new SysexTonePickerContent(proc, source, std::move(tones)), true);
		setResizable(true, true);
		centreWithSize(getWidth(), getHeight());
		setVisible(true);
	}
	void closeButtonPressed() override { delete this; }
};

// Shows docs/D20infos.png (embedded as BinaryData, see plugin/CMakeLists.txt's
// juce_add_binary_data(D110PanelData ...)) in its own resizable pop-up window, sized to half
// the emulator's own current window width (Alan's request, 2026-08-25) rather than a fixed
// fraction of the screen - windowWidth is whichever caller's own getWidth(), which is the
// app's current window width either way (D110Panel and D110EditorPane are both full window
// width). Height follows the image's own aspect ratio, so nothing letterboxes; the screen-size
// clamp is just a safety net for an extreme window size, not the normal case. Not modal - see
// ImagePopupWindow. A free function (declared up top, see showLaReferencePopup's forward
// declaration) rather than a D110EditorPane member: Github issue #3 asked for a way to reach
// it without opening the drawer/UTILITY tab at all, so D110Panel::showOptionsMenu() (its own
// right-click menu) needed to call this too - and the two components are siblings, neither
// owning the other.
void showLaReferencePopup(int windowWidth) {
	auto image = juce::ImageCache::getFromMemory(BinaryData::D20infos_png, BinaryData::D20infos_pngSize);
	if (image.isNull() || image.getWidth() <= 0 || image.getHeight() <= 0) return;

	auto *imageComponent = new juce::ImageComponent();
	imageComponent->setImage(image);
	imageComponent->setImagePlacement(juce::RectanglePlacement::centred
	                                   | juce::RectanglePlacement::onlyReduceInSize);

	const auto *display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
	const auto screenArea =
		display != nullptr ? display->userBounds.toNearestInt() : juce::Rectangle<int>(0, 0, 1600, 900);

	// Both clamps below can fire together on a short, wide screen - scaling w and h down by the
	// same factor each time (rather than clamping them independently) keeps the image's own
	// aspect ratio intact either way.
	double w = juce::jmax(200, windowWidth / 2);
	double h = w * image.getHeight() / image.getWidth();
	const double maxW = screenArea.getWidth() * 0.9, maxH = screenArea.getHeight() * 0.9;
	if (w > maxW) { h *= maxW / w; w = maxW; }
	if (h > maxH) { w *= maxH / h; h = maxH; }
	imageComponent->setSize(juce::roundToInt(w), juce::roundToInt(h));

	new ImagePopupWindow("LA reference", imageComponent);
}
} // namespace

// --- значения ---------------------------------------------------------------

size_t D110EditorPane::addressOf(const Cell &c) const {
	switch (c.area) {
	case Area::ToneTemp:
		return size_t(D110CoreType::kRamToneTemp) + size_t(c.index) * D110CoreType::kToneRecord
		     + size_t(c.field);
	case Area::Rhythm:
		return size_t(D110CoreType::kRamRhythmTemp) + size_t(c.index) * D110CoreType::kRhythmRecord
		     + size_t(c.field);
	case Area::System:
		return size_t(D110CoreType::kRamSystem) + size_t(c.index);
	case Area::Timbres:
		return size_t(D110CoreType::kRamTimbres) + size_t(c.index) * D110CoreType::kTimbreRecord
		     + size_t(c.field);
	case Area::Patches:
		return size_t(D110CoreType::kRamPatches) + size_t(c.index) * D110CoreType::kPatchRecord
		     + size_t(c.field);
	case Area::Tones:
		return size_t(D110CoreType::kRamTones) + size_t(c.index) * D110CoreType::kToneMemRecord
		     + size_t(c.field);
	default:
		return size_t(D110CoreType::kRamTimbreTemp) + size_t(c.index) * D110CoreType::kTimbreTempRecord
		     + size_t(c.field);
	}
}

int D110EditorPane::valueOf(const Cell &c) const {
	if (!ramValid) return -1;
	const size_t at = addressOf(c);
	return (at < ram.size()) ? int(ram[at]) : -1;
}

void D110EditorPane::setValue(const Cell &c, int value) {
	const int v = juce::jlimit(c.lo, c.hi, value);
	switch (c.area) {
	case Area::ToneTemp: processor.sendToneTempParam(c.index, c.field, uint8_t(v)); break;
	case Area::Rhythm:   processor.sendRhythmParam(c.index, c.field, uint8_t(v)); break;
	case Area::System:   processor.sendSystemParam(c.index, uint8_t(v)); break;
	case Area::Timbres:  processor.sendTimbreMemoryParam(c.index, c.field, uint8_t(v)); break;
	case Area::Patches:  processor.editPatchField(c.index, c.field, uint8_t(v)); break;
	case Area::Tones:    break;   // тон целиком, а не по байту - см. вкладку TONES
	default:
		processor.sendTimbreTempParam(c.index, c.field, uint8_t(v));
		// Picking a tone (group or number, fields 0/1) here only ever writes those two
		// bytes - unlike the real panel's own tone selection, which (found by diffing two
		// of Alan's own memory snapshots, 2026-08-07) leaves the part in normal polyphonic
		// assign whatever it picks. This used to also force ASSIGN to POLY 3 here, to work
		// around mt32emu's Part::playPoly() intermittently dropping retriggered notes in
		// POLY 1/2 (a synth-wide isAbortingPoly() gate blocking retriggers even when the
		// partial pool had spare partials available) - that root cause is fixed directly in
		// munt/mt32emu/src/Part.cpp now (2026-08-11), so POLY 1/2 inherited from a previous
		// tone is no longer a problem and a user's deliberate ASSIGN choice, on this or any
		// other tone, is no longer silently overridden.
		break;
	}
	// Значение показывается сразу, не дожидаясь, пока прошивка его примет и таймер это
	// увидит: иначе поле под курсором отставало бы на десятую долю секунды.
	const size_t at = addressOf(c);
	if (ramValid && at < ram.size()) ram[at] = uint8_t(v);

	// Отмечено как непринятое - см. PendingEdit. Тот же адрес заменяет свою прежнюю запись
	// (не копится), потому что важен только самый последний запрошенный шаг колеса.
	const juce::int64 now = juce::Time::getMillisecondCounter();
	bool replaced = false;
	for (auto &p : pendingEdits)
		if (p.address == at) { p.value = uint8_t(v); p.sentMs = now; replaced = true; break; }
	if (!replaced) pendingEdits.push_back({at, uint8_t(v), now});

	// LOCK PARTIALS (Tone tab): a manual edit to a Partial 1 field also sets the same field, to
	// the same value, on Partials 2-4 - same feature/wording as ~/src/D110/edisyn's own "Lock
	// Partials" checkbox for this instrument (RolandD110Tone.registerPartialLock). Partial 1 is
	// field 14..71 (14 + 58-byte record, partial index 0); guarded by suppressPartialLock so
	// randomizeTone() below can still randomize all four partials independently even while this
	// is on, matching Edisyn's own choice there - its lock only ever propagates a value the
	// user dialled in by hand, never one its Randomize/Mutate set in bulk.
	if (lockPartials && !suppressPartialLock && c.area == Area::ToneTemp
	    && c.field >= 14 && c.field < 14 + 58) {
		const int relOffset = c.field - 14;
		for (int p = 1; p <= 3; ++p)
			setValue({ {}, Area::ToneTemp, c.index, 14 + 58 * p + relOffset, c.lo, c.hi }, v);
	}

	repaint();
}

// Tone tab's DEGRADE/RANDOM buttons. RANDOM (fullyRandom=true) picks every field completely
// fresh, uniformly within its own range - the whole tone, common fields (structures, partial
// mute, env mode) and all four partials' 58 bytes each. DEGRADE (false) is a light dusting of
// variation instead: each field independently has a fixed chance of being touched at all
// (kDegradeTouchProb), and when it is, only nudges by a small delta around its CURRENT value
// (kDegradeSpanFrac of its own range) rather than jumping anywhere in it - meant to feel like
// "still recognisably this tone", not a fresh one. Both ignore lockPartials (see
// suppressPartialLock and setValue()'s own comment) so all four partials come out independent
// either way, exactly like ~/src/D110/edisyn's own Randomize/Mutate does for this instrument.
void D110EditorPane::randomizeTone(bool fullyRandom) {
	juce::Random &rng = juce::Random::getSystemRandom();
	constexpr float kDegradeTouchProb = 0.35f;
	constexpr float kDegradeSpanFrac = 0.12f;

	suppressPartialLock = true;

	auto touchField = [&](int field, int hi) {
		if (hi <= 0) return;   // nothing to vary
		// WG PITCH (kWg[0], offset 0 of every partial - the coarse note it plays) is left
		// alone by both generators: randomising it re-pitches or detunes the partial outright,
		// which reads as broken rather than as a variation of the tone (Alan's report,
		// 2026-08-25) - unlike every other field here, it isn't "the same tone, tweaked".
		if (field >= 14 && (field - 14) % 58 == 0) return;
		if (fullyRandom) {
			int v = rng.nextInt(hi + 1);
			// Field 12 is the PARTIAL MUTE mask (bit i = partial i+1 sounds) - 0 mutes all
			// four, which would make a freshly "random" tone silent rather than just
			// different. Re-roll away from that one specific value instead of excluding it
			// from randomisation entirely.
			if (field == 12 && v == 0) v = 1 + rng.nextInt(hi);
			setValue({ {}, Area::ToneTemp, part, field, 0, hi }, v);
			return;
		}
		if (rng.nextFloat() >= kDegradeTouchProb) return;
		const int cur = valueOf({ {}, Area::ToneTemp, part, field, 0, hi });
		if (cur < 0) return;
		const int span = juce::jmax(1, juce::roundToInt(float(hi) * kDegradeSpanFrac));
		const int delta = rng.nextInt(span * 2 + 1) - span;
		if (delta != 0) setValue({ {}, Area::ToneTemp, part, field, 0, hi }, cur + delta);
	};

	// Common: STRUCTURE 1-2, STRUCTURE 3-4, PARTIAL MUTE, ENV MODE (see layoutTone()'s own
	// kOffsets/kHi for this same set - not worth a shared table for four entries).
	static const int kCommonOffsets[] = { 10, 11, 12, 13 };
	static const int kCommonHi[]      = { 12, 12, 15,  1 };
	for (int i = 0; i < 4; ++i) touchField(kCommonOffsets[i], kCommonHi[i]);

	// All four partials, all 58 bytes each - kWg/kPitchEnv/kTvf/kTva together cover the whole
	// record (see their own definition, just above layoutTone()).
	for (int p = 0; p < 4; ++p) {
		const int base = 14 + p * 58;
		for (auto &e : kWg)       touchField(base + e.offset, e.hi);
		for (auto &e : kPitchEnv) touchField(base + e.offset, e.hi);
		for (auto &e : kTvf)      touchField(base + e.offset, e.hi);
		for (auto &e : kTva)      touchField(base + e.offset, e.hi);
	}

	suppressPartialLock = false;
	repaint();
}

juce::String D110EditorPane::nameAt(size_t ramOffset) const {
	if (!ramValid) return {};
	juce::String name;
	for (int i = 0; i < D110CoreType::kNameChars && ramOffset + size_t(i) < ram.size(); ++i) {
		const char ch = char(ram[ramOffset + size_t(i)]);
		if (ch < 32 || ch > 126) break;
		name += ch;
	}
	return name.trimEnd();
}

// Имя тона по группе и номеру. Внутренние тона - из памяти самой прошивки, которая их и
// хранит; пресетные и ударные живут в ПЗУ, и их имя спрашивается у звукового движка,
// загрузившего то же самое ПЗУ. Спрашивается по одному разу: имена в ПЗУ не меняются, а
// лезть в чужой поток на каждой перерисовке незачем.
juce::String D110EditorPane::toneName(int group, int number) const {
	if (group < 0 || group > 3 || number < 0 || number > 63) return {};
	if (group == 2)
		return nameAt(size_t(D110CoreType::kRamTones) + size_t(number) * D110CoreType::kToneMemRecord);

	const size_t slot = size_t(group) * 64 + size_t(number);
	if (romToneNameKnown[slot]) return romToneNames[slot];
	if (!processor.engineIsOpen()) return {};

	// Адрес в упакованном виде, как его ждёт readEngineMemory: SysEx 08 00 00 - это 0x020000,
	// а запись банка тембров у движка занимает 256 байт.
	uint8_t buf[D110CoreType::kNameChars] = {};
	if (!processor.readEngineMemory(juce::uint32(0x020000 + slot * 256), D110CoreType::kNameChars,
	                                buf))
		return {};
	juce::String name;
	for (unsigned char ch : buf) {
		if (ch < 32 || ch > 126) break;
		name += char(ch);
	}
	romToneNames[slot] = name.trimEnd();
	romToneNameKnown[slot] = true;
	return romToneNames[slot];
}

juce::String D110EditorPane::textOf(const Cell &c) const {
	const int v = valueOf(c);
	if (v < 0) return "--";

	// Поля партии одинаковы в трёх местах - во временной области, в памяти тембров и внутри
	// патча, - потому что это одна и та же запись Roland. Значит и печатаются они одним
	// куском кода, а не тремя расходящимися.
	auto partField = [&](int field) -> juce::String {
		switch (field) {
		case 0: return toneGroupLabel(v);
		case 1: {
			// Группа стоит в записи прямо перед номером - и во временной области, и в
			// памяти тембров, и внутри патча, потому что это одна и та же запись Roland.
			const size_t at = addressOf(c);
			const int group = (at >= 1 && at - 1 < ram.size()) ? int(ram[at - 1]) : 0;
			const juce::String name = toneName(group, v);
			return juce::String(v + 1) + (name.isEmpty() ? juce::String() : "  " + name);
		}
		case 2: {
			const int semis = v - 24;   // 0..48 это -24..+24 полутона
			return (semis > 0 ? "+" : "") + juce::String(semis);
		}
		case 3: return juce::String(v - 50);          // подстройка, центы
		case 5: return "POLY " + juce::String(v + 1);
		case 6: return outputAssignText(v);
		case 9: return panText(v);
		case 10: case 11: return juce::String(v) + "  " + noteName(v);
		default: return juce::String(v);
		}
	};

	// Тон - запись Roland из общей части и четырёх партиалов по 58 байт. Величины в ней
	// имеют собственные шкалы, и показывать их сырыми байтами значило бы заставлять читателя
	// держать эти шкалы в голове.
	if (c.area == Area::ToneTemp) {
		if (c.field == 10 || c.field == 11) return juce::String(v + 1);   // структуры с единицы
		if (c.field == 12) {                                             // маска партиалов
			juce::String mask;
			for (int i = 0; i < 4; ++i) mask += ((v >> i) & 1) ? juce::String(i + 1) : "-";
			return mask;
		}
		if (c.field == 13) return v ? "NO SUSTAIN" : "NORMAL";
		if (c.field < 14) return juce::String(v);
		switch ((c.field - 14) % 58) {
		case 0: {   // высота партиала: 0..96 это C1..C9
			static const char *kNote[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G",
			                               "G#", "A", "A#", "B" };
			return juce::String(kNote[v % 12]) + juce::String(1 + v / 12);
		}
		case 1: return juce::String(v - 50);            // подстройка, центы
		case 3: return v ? "ON" : "OFF";                // колесо высоты тона
		case 4: {
			static const char *kWave[] = { "SQU/1", "SAW/1", "SQU/2", "SAW/2" };
			return kWave[juce::jlimit(0, 3, v)];
		}
		case 5: {   // номер образца PCM - имя из ПЗУ, банк решает соседнее поле WAVEFORM
			const int n = juce::jlimit(0, 127, v);
			// A customized slot's real audio no longer matches the factory name at all - show
			// the source file's own name (falling back to a plain "custom" marker for an old
			// project saved before names were tracked) instead of the now-wrong factory name.
			if (processor.hasCustomPcmWave(n)) {
				const auto customName = processor.getCustomPcmWaveName(n);
				return juce::String(v + 1) + " *" + (customName.isNotEmpty() ? customName : juce::String("custom"));
			}
			const size_t waveAt = addressOf(c) - 1;
			const bool bank2 = (waveAt < ram.size()) && ((ram[waveAt] & 2) != 0);
			return juce::String(v + 1) + "  " + (bank2 ? kPcmBank2Names[n] : kPcmBank1Names[n]);
		}
		case 7: return juce::String(v - 7);             // чувствительность ширины импульса
		case 15: case 16: case 17: case 18: case 19:    // уровни огибающей высоты, -50..+50
			return juce::String(v - 50);
		case 27: return juce::String(v - 7);            // TVF bias level
		case 44: case 46: return juce::String(-v);      // TVA bias levels, 0..-12
		default: return juce::String(v);
		}
	}

	switch (c.area) {
	case Area::TimbreTemp: return partField(c.field);
	case Area::Timbres:    return partField(c.field);
	case Area::Patches:
		switch (c.field) {
		// Три шкалы ревербератора у прибора РАЗНЫЕ, и это снято с его собственного экрана:
		// Type идёт 1..8 и затем OFF, Time 1..8, а Level 0..7 (docs/factory_defaults.md).
		// Заводской патч хранит 04 04 04 и показывает 5, 5, 4.
		case 10: return reverbTypeLabel(v);
		case 11: return juce::String(v + 1);
		case 12: return juce::String(v);
		default:
			// Всё, что начиная с 31-го байта, - это записи партий: тот же разбор, но
			// смещение внутри записи считается от её начала.
			if (c.field >= 31) return partField((c.field - 31) % 12);
			// The 9-byte channel-map block, offset 22-30 - same 0-15/"OFF" encoding as the
			// LIVE System-area copy (see the Area::System case's own >= 13 branch).
			if (c.field >= 22 && c.field <= 30) return (v > 15) ? juce::String("OFF") : juce::String(v + 1);
			return juce::String(v);
		}
	case Area::Rhythm:
		switch (c.field) {
		case 0: {
			// 0..63 - тембры внутренней памяти, 64..127 - звуки ударных. Заводская установка
			// держит здесь значения до 103, поэтому предел именно 127, а не MT-32-шные 94.
			if (v < 64) return "TIMBRE " + juce::String(v + 1);
			const juce::String name = toneName(3, v - 64);
			// Незанятая клавиша указывает на пустое место банка ударных, и оно так и
			// называется - "OFF". Печатать рядом с ним ещё и номер значило бы делать вид,
			// будто там что-то есть.
			if (name == "OFF") return "OFF";
			return "RHY " + juce::String(v - 63) + (name.isEmpty() ? juce::String()
			                                                       : "  " + name);
		}
		case 2: return panText(v);
		case 3: return outputAssignText(v);   // Rhythm Setup: Tone, Level, Pan, Output Assign
		default: return juce::String(v);
		}
	case Area::System:
		switch (c.index) {
		case 0: {
			// Общая подстройка. Шкала СНЯТА С ИНДИКАТОРА ПРИБОРА, а не взята из руководства:
			// документированное Roland отображение 0-127 -> 432.1-457.6 Гц дало бы для
			// заводского байта 74 около 447, тогда как прибор показывает 442. Развёрнутая по
			// шести значениям (plugin/editor_write_probe.cpp, раздел 6), его собственная
			// шкала ложится на 440 Гц при байте 64 и примерно 0.2 Гц на шаг: 64 -> "440" и
			// 74 -> "442" совпадают точно, у остальных четырёх на экране есть дробная часть,
			// и пишет её прибор собственным знаком из ОЗУ знакогенератора - прочитать её
			// нечем.
			//
			// Отсюда и тильда: число получено этой шкалой и на краях может разойтись с
			// экраном прибора на герц. Байт стоит рядом, потому что правится именно он - и
			// именно он НЕ переносится в звуковой движок, чей строй считается иначе
			// (docs/sysex_address_map.md).
			const double hz = 440.0 + double(v - 64) * 0.2;
			return juce::String(v) + "   ~" + juce::String(hz, 1) + " Hz";
		}
		case 1: return reverbTypeLabel(v);
		case 2: return juce::String(v + 1);   // время 1..8
		case 3: return juce::String(v);       // уровень 0..7, и он один такой
		default:
			if (c.index >= 13) return (v > 15) ? juce::String("OFF") : juce::String(v + 1);
			return juce::String(v);
		}
	default: return juce::String(v);
	}
}

// --- рисование --------------------------------------------------------------

bool D110EditorPane::isPartialMuteCell(const Cell &c) const {
	return c.area == Area::ToneTemp && c.field == 12;
}

juce::Rectangle<float> D110EditorPane::partialMuteSegment(const Cell &c, int partial) const {
	const float segW = c.bounds.getWidth() / 4.0f;
	return juce::Rectangle<float>(c.bounds.getX() + segW * float(partial), c.bounds.getY(),
	                              segW - 2.0f, c.bounds.getHeight());
}

void D110EditorPane::paintPartialMuteCell(juce::Graphics &g, const Cell &c, bool hover) const {
	const int v = juce::jmax(0, valueOf(c));
	const juce::Font valueFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f,
	                                             juce::Font::plain));
	for (int i = 0; i < 4; ++i) {
		const auto seg = partialMuteSegment(c, i);
		const bool on = ((v >> i) & 1) != 0;
		g.setColour(on ? kEdBox().brighter(0.3f) : kEdBox());
		g.fillRoundedRectangle(seg, 3.0f);
		g.setColour(on ? kEdValue() : (hover ? kEdValue().withAlpha(0.55f) : kEdBorder()));
		g.drawRoundedRectangle(seg.reduced(0.5f), 3.0f, 1.0f);
		g.setColour(on ? kEdValue() : kEdDim());
		g.setFont(valueFont);
		g.drawText(juce::String(i + 1), seg, juce::Justification::centred);
	}
}

// getWidth() alone shrinks in Utility tab's PANEL SIZE = COMPACT mode even though the panel's
// own on-screen zoom doesn't (D110AudioProcessorEditor's own resize math keeps window px per
// reference-space px, "s", identical across the toggle - see D110Panel::kCompactRefW's own
// comment - compact mode just has fewer reference units to draw, so only the total window
// narrows). This drawer's font size used to be a flat getWidth()/1500, so toggling compact
// shrank its text along with the window even though nothing about this drawer's own content
// got smaller - down to the 0.75 floor at the app's default size (Alan's report, 2026-08-25).
// Normalising against currentRefW() instead keeps it at the size it had before compact mode
// existed, matching how the panel's own buttons/knobs already behave under the same toggle.
float D110EditorPane::fontScale() const {
	const float s = float(getWidth())
	              / float(D110Panel::currentRefW(processor.getCompactPanelMode()));
	return juce::jlimit(0.75f, 1.4f, s / (1500.0f / float(D110Panel::kRefW)));
}

void D110EditorPane::paint(juce::Graphics &g) {
	g.fillAll(kEdBack());
	if (cells.empty() && buttons.empty() && tab != Tab::Monitor) layout();

	const float scale = fontScale();
	const juce::Font labelFont(juce::FontOptions(11.0f * scale, juce::Font::bold));
	const juce::Font valueFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
	                                             12.0f * scale, juce::Font::plain));

	const char *kTabs[] = { "PARTS", "TONE", "RHYTHM", "PATCHES", "TIMBRES", "TONES",
	                        "SYSTEM", "MONITOR", "SOUNDBANKS", "UTILITY" };
	for (int i = 0; i < kNumTabs; ++i) {
		const bool active = (int(tab) == i);
		g.setColour(active ? kEdBox().brighter(0.25f) : kEdBox());
		g.fillRoundedRectangle(tabBounds[(size_t)i], 3.0f);
		g.setColour(active ? kEdValue() : kEdBorder());
		g.drawRoundedRectangle(tabBounds[(size_t)i].reduced(0.5f), 3.0f, 1.0f);
		g.setColour(active ? kEdValue() : kEdLabel());
		g.setFont(labelFont);
		g.drawText(kTabs[i], tabBounds[(size_t)i], juce::Justification::centred);
	}

	if (!optionsButtonBounds.isEmpty()) {
		g.setColour(kEdBox());
		g.fillRoundedRectangle(optionsButtonBounds, 3.0f);
		g.setColour(kEdBorder());
		g.drawRoundedRectangle(optionsButtonBounds.reduced(0.5f), 3.0f, 1.0f);
		g.setColour(kEdLabel());
		g.setFont(labelFont);
		g.drawText("OPTIONS", optionsButtonBounds, juce::Justification::centred);
	}

	if (!ramValid) {
		g.setColour(kEdDim());
		g.setFont(labelFont);
		g.drawText("SWITCH THE INSTRUMENT ON TO EDIT IT",
		           getLocalBounds().reduced(20), juce::Justification::centred);
		return;
	}

	if (tab == Tab::Monitor) {
		paintMonitor(g, contentArea);
		return;
	}

	// Выбор партии: на этих вкладках партия - не параметр прибора, а то, ЧЬЮ запись мы
	// смотрим, поэтому она сделана рядом маленьких кнопок, а не полем со значением.
	if (tab == Tab::Tone || tab == Tab::Timbres || tab == Tab::Tones) {
		for (int p = 0; p < 8; ++p) {
			const bool active = (p == part);
			g.setColour(active ? kEdBox().brighter(0.3f) : kEdBox());
			g.fillRoundedRectangle(partBounds[(size_t)p], 3.0f);
			g.setColour(active ? kEdValue() : kEdBorder());
			g.drawRoundedRectangle(partBounds[(size_t)p].reduced(0.5f), 3.0f, 1.0f);
			g.setColour(active ? kEdValue() : kEdDim());
			g.setFont(labelFont);
			g.drawText(juce::String(p + 1), partBounds[(size_t)p], juce::Justification::centred);
		}
	}
	if (tab == Tab::Tone && !textEntry.isVisible()) {
		g.setColour(kEdValue());
		g.setFont(valueFont);
		g.drawText(nameAt(size_t(D110CoreType::kRamToneTemp) + size_t(part) * D110CoreType::kToneRecord),
		           toneNameBounds, juce::Justification::centredLeft);
	}

	// PATCHES' own sub-tab strip - same active/inactive styling as the main tab strip
	// above, just smaller and scoped to this one tab. See PatchesSubTab's own comment.
	if (tab == Tab::Patches) {
		const char *kSubTabs[] = { "ALL PATCHES", "PARTS OF PATCH" };
		for (int i = 0; i < 2; ++i) {
			const bool active = (int(patchesSubTab) == i);
			g.setColour(active ? kEdBox().brighter(0.25f) : kEdBox());
			g.fillRoundedRectangle(patchesSubTabBounds[(size_t)i], 3.0f);
			g.setColour(active ? kEdValue() : kEdBorder());
			g.drawRoundedRectangle(patchesSubTabBounds[(size_t)i].reduced(0.5f), 3.0f, 1.0f);
			g.setColour(active ? kEdValue() : kEdLabel());
			g.setFont(labelFont);
			g.drawText(kSubTabs[i], patchesSubTabBounds[(size_t)i], juce::Justification::centred);
		}
	}

	// UTILITY is the one tab whose content can run taller than the drawer (see
	// layoutUtility()'s scroll offset) - clipped in its own scope, not for the rest of
	// paint(), since that also draws the tab strip above and the caption strip below, neither
	// of which utilityScrollOffset should be allowed to draw over.
	{
	juce::Graphics::ScopedSaveState clipState(g);
	if (tab == Tab::Utility) g.reduceClipRegion(contentArea.getSmallestIntegerContainer());

	g.setFont(labelFont);
	for (const Label &l : labels) {
		g.setColour(l.heading ? kEdLabel() : kEdDim());
		g.drawText(l.text, l.bounds, l.just);
	}

	// Кнопки рисуются как поля, только подписью по центру. Ячейки памяти тонов и имена
	// патчей - тоже кнопки, но подписываются они содержимым.
	for (const Button &b : buttons) {
		if (textEntry.isVisible() && b.id == textEntryButton) continue;
		if (b.id >= 100 && b.id < 200) {                    // ячейка памяти тонов
			const int slot = b.id - 100;
			const bool chosen = (slot == toneSlot);
			drawBox(g, b.bounds, chosen);
			const juce::String name = nameAt(size_t(D110CoreType::kRamTones)
			                                 + size_t(slot) * D110CoreType::kToneMemRecord);
			g.setColour(chosen ? kEdValue() : kEdDim());
			g.setFont(valueFont);
			g.drawText(juce::String(slot + 1).paddedLeft(' ', 2) + "  "
			               + (name.isEmpty() ? juce::String("- - -") : name),
			           b.bounds.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
			continue;
		}
		if (b.id == 22) {   // LOCK PARTIALS - a real lit/unlit toggle, not just its own label text
			g.setColour(lockPartials ? kEdBox().brighter(0.3f) : kEdBox());
			g.fillRoundedRectangle(b.bounds, 3.0f);
			g.setColour(lockPartials ? kEdValue() : kEdBorder());
			g.drawRoundedRectangle(b.bounds.reduced(0.5f), 3.0f, 1.0f);
			g.setColour(lockPartials ? kEdValue() : kEdDim());
			g.setFont(labelFont);
			g.drawText(b.text, b.bounds, juce::Justification::centred);
			continue;
		}
		if (b.id >= 400 && b.id < 500) {                    // имя патча
			const int patch = b.id - 400;
			drawBox(g, b.bounds, false);
			g.setColour(kEdValue());
			g.setFont(valueFont);
			g.drawText(nameAt(size_t(D110CoreType::kRamPatches)
			                  + size_t(patch) * D110CoreType::kPatchRecord),
			           b.bounds.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
			continue;
		}
		// Патч, который прибор играет сейчас, отмечен - иначе список из 64 одинаковых
		// кнопок не говорит, где ты находишься.
		const bool current = (b.id >= 200 && b.id < 300)
		                   && ramValid
		                   && int(ram[(size_t)D110CoreType::kRamPatchNumber]) == b.id - 200;
		const bool chosenPatch = (b.id >= 200 && b.id < 300) && (b.id - 200 == patchSlot);
		drawBox(g, b.bounds, current || chosenPatch);
		g.setColour(current ? kEdValue() : (b.text.isEmpty() ? kEdDim() : kEdLabel()));
		g.setFont(labelFont);
		g.drawText(b.text.isEmpty() ? juce::String("click to type") : b.text, b.bounds,
		           juce::Justification::centred);
	}

	g.setFont(valueFont);
	for (size_t i = 0; i < cells.size(); ++i) {
		const Cell &c = cells[i];
		if (isPartialMuteCell(c)) {
			paintPartialMuteCell(g, c, int(i) == hovered);
			continue;
		}
		drawBox(g, c.bounds, int(i) == hovered || int(i) == dragging
		                     || (tab == Tab::Patches && int(i) == focusedPatchCell));
		g.setColour(kEdValue());
		g.drawText(textOf(c), c.bounds.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
	}
	} // end of the UTILITY clip scope

	// Thin scrollbar on the right - only while the UTILITY tab genuinely doesn't fit; the
	// track/thumb are computed in layoutUtility() along with the rest of that tab's geometry.
	if (tab == Tab::Utility && !utilityScrollTrack.isEmpty()) {
		g.setColour(kEdBox());
		g.fillRoundedRectangle(utilityScrollTrack, 3.0f);
		g.setColour(kEdBorder().brighter(0.2f));
		g.fillRoundedRectangle(utilityScrollThumb.reduced(1.0f), 3.0f);
	}

	// Одна строка о том, чем этот ящик является, чтобы он не читался как отдельный «микшер
	// плагина», живущий своей жизнью.
	g.setColour(kEdDim());
	g.setFont(juce::FontOptions(10.0f * scale));
	juce::String footer;
	switch (tab) {
	case Tab::Parts:
		footer = "Drag a field to edit, or roll the wheel over it. Every change is sent to the "
		         "instrument as exclusive data, exactly as an external editor would - the "
		         "panel's own display shows it too. This is the block the sound engine mirrors.";
		break;
	case Tab::Tone:
		footer = "The 246 bytes the part is actually playing. Edits here are audible at once; "
		         "they live in the temporary area, so storing them means putting the tone into "
		         "the TONES memory.";
		break;
	case Tab::Patches:
		footer = "Clicking a row puts that patch on the instrument itself, by pressing its own "
		         "PATCH / BANK / NUMBER buttons. Switch to PARTS OF PATCH to edit its parts - "
		         "audible at once while it is the patch being played, the stored record and "
		         "the live areas are both written. Click a row's name to rename it.";
		break;
	case Tab::Rhythm:
		footer = "One row per drum key, 85 of them - roll the wheel over the list to scroll. "
		         "The rhythm part's own level and pan are on the PARTS tab.";
		break;
	case Tab::Timbres:
		footer = "A timbre says which tone to play and how. Editing one changes stored memory, "
		         "which the parts pick up when that timbre is next selected.";
		break;
	case Tab::System:
		footer = "Master Tune and Reverb Type are the instrument's own and are deliberately NOT "
		         "carried to the sound engine - the two scales disagree, and eight reverb types "
		         "onto the engine's four have no honest mapping (docs/sysex_address_map.md).";
		break;
	case Tab::Tones:
		footer = "Clicking a slot loads that tone into the chosen part at once, so it can be "
		         "heard - the same 246 bytes the RECALL button sends. STORE puts the part's "
		         "current tone back into the slot.";
		break;
	default:
		footer = "Every change is sent to the instrument as exclusive data, exactly as an "
		         "external editor would - the panel shows it too.";
		break;
	}
	g.drawText(footer, getLocalBounds().reduced(16, 6), juce::Justification::bottomLeft);
}

// Монитор. Показывает три вещи, которых больше нигде не видно: занятость голосов LA32,
// клавиши, которые прошивка считает нажатыми, и то, что действительно приходит по MIDI.
// Всё это читается из памяти самой прошивки, а не из звукового движка, - то есть отвечает
// на вопрос «что об этом думает прибор», а не «что услышал эмулятор».
void D110EditorPane::paintMonitor(juce::Graphics &g, juce::Rectangle<float> area) {
	const float scale = fontScale();
	const juce::Font labelFont(juce::FontOptions(11.0f * scale, juce::Font::bold));
	const juce::Font valueFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
	                                             12.0f * scale, juce::Font::plain));

	g.setColour(kEdLabel());
	g.setFont(labelFont);
	g.drawText("LA32 VOICE SLOTS - THE FIRMWARE'S OWN TABLE",
	           area.removeFromTop(16.0f), juce::Justification::centredLeft);

	auto pool = area.removeFromTop(52.0f);
	const float bw = pool.getWidth() / 16.0f;
	int busy = 0;
	for (int s = 0; s < D110CoreType::kNumHardwareVoices; ++s) {
		const size_t at = size_t(D110CoreType::kSlotStateTable) + size_t(s) * 2;
		const int state = (at < ram.size()) ? int(ram[at]) : -1;
		const bool on = (state == D110CoreType::kSlotBusyValue || state == D110CoreType::kSlotBusyValueAlt);
		if (on) ++busy;
		const juce::Rectangle<float> r(pool.getX() + bw * float(s % 16),
		                               pool.getY() + 26.0f * float(s / 16), bw - 4.0f, 22.0f);
		g.setColour(on ? kEdValue() : kEdBox());
		g.fillRoundedRectangle(r, 2.0f);
		g.setColour(kEdBorder());
		g.drawRoundedRectangle(r.reduced(0.5f), 2.0f, 1.0f);
	}
	area.removeFromTop(4.0f);
	g.setColour(kEdDim());
	g.setFont(valueFont);
	g.drawText(juce::String(busy) + " of " + juce::String(D110CoreType::kNumHardwareVoices)
	               + " busy   -   and the sound engine has "
	               + juce::String(processor.engineActivePartials()) + " of "
	               + juce::String(int(processor.enginePartialCount())) + " partials sounding",
	           area.removeFromTop(18.0f), juce::Justification::centredLeft);
	area.removeFromTop(8.0f);

	// Какие партии звучат. Спрашивается у ЗВУКОВОГО ДВИЖКА, а не вычитывается из таблиц
	// контекстов прошивки, и вот почему: контекст, который прошивка бросила, в снимке памяти
	// выглядит точно так же, как звучащий, - нота на месте, бит освобождения не выставлен, -
	// потому что прошивка помечает контексты своими ЗАПИСЯМИ, а не состоянием. Первая версия
	// этой панели читала таблицы напрямую и показывала на ритм-партии четырнадцать нажатых
	// клавиш там, где не звучало ни одной.
	const uint32_t engineParts = processor.enginePartStates();
	g.setColour(kEdLabel());
	g.setFont(labelFont);
	g.drawText("PARTS THE SOUND ENGINE IS HOLDING",
	           area.removeFromTop(16.0f), juce::Justification::centredLeft);
	auto parts = area.removeFromTop(30.0f);
	const float pw = parts.getWidth() / 9.0f;
	for (int p = 0; p < 9; ++p) {
		const bool on = ((engineParts >> p) & 1u) != 0;
		const juce::Rectangle<float> r(parts.getX() + pw * float(p), parts.getY(),
		                               pw - 8.0f, 26.0f);
		drawBox(g, r, on);
		g.setColour(on ? kEdValue() : kEdDim());
		g.setFont(valueFont);
		g.drawText(juce::String(partLabel(p)) + (on ? ": sounding" : ": -"),
		           r.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
	}
	area.removeFromTop(8.0f);

	// Мост в цифрах. Ноль потерянных сообщений - это условие, при котором всё показанное
	// выше вообще что-то значит, поэтому счётчики стоят рядом, а не прячутся в журнале.
	g.setColour(kEdLabel());
	g.setFont(labelFont);
	g.drawText("THE BRIDGE", area.removeFromTop(16.0f), juce::Justification::centredLeft);
	g.setColour(kEdDim());
	g.setFont(valueFont);
	g.drawText("mirror messages to the engine: "
	               + juce::String(juce::int64(processor.getCore().sysexEmitted()))
	               + "   dropped: "
	               + juce::String(juce::int64(processor.getCore().sysexDropped()))
	               + "        MIDI bytes into the firmware: "
	               + juce::String(juce::int64(processor.getCore().midiDelivered()))
	               + "   dropped: "
	               + juce::String(juce::int64(processor.getCore().midiDropped())),
	           area.removeFromTop(18.0f), juce::Justification::centredLeft);
	area.removeFromTop(4.0f);
	// Sample rate this instance was actually told by its host in prepareToPlay() - added
	// 2026-08-20 while chasing Alan's "notes arrive pitched up, only in Carla" report, to see
	// straight from the running instance whether the host handed it a different rate than
	// expected rather than guessing from outside. munt/mt32emu's own native rate is a fixed
	// 32000 Hz (MT32EMU_SAMPLE_RATE) - the sound engine's SampleRateConverter always converts
	// from that to whatever this line shows, so a wrong value here would explain a constant
	// pitch/speed shift with no MIDI event involved at all.
	g.setColour(kEdDim());
	g.setFont(valueFont);
	g.drawText("host sample rate: " + juce::String(processor.getSampleRate(), 0) + " Hz",
	           area.removeFromTop(18.0f), juce::Justification::centredLeft);
	area.removeFromTop(4.0f);
	// Diagnostic counter for the stuck-continuous-note report Alan filed 2026-08-18/19 (see
	// project_sequencer_channel_collision_fix memory) - Part::abortFirstPoly()'s fallback
	// release (munt/mt32emu/src/Part.cpp) increments this every time it fires. Should stay at
	// 0 most sessions; if it climbs right when a note gets stuck, that's the mechanism.
	const uint32_t abortFallbacks = processor.engineAbortFallbackCount();
	g.setColour(abortFallbacks > 0 ? kEdValue() : kEdDim());
	g.drawText("stuck-voice guard fired: " + juce::String(juce::int64(abortFallbacks)) + " time(s) this session",
	           area.removeFromTop(18.0f), juce::Justification::centredLeft);
	// Second candidate for the same stuck-note report, added once the first (above) came back
	// at 0 while the bug still reproduced - see project_sequencer_channel_collision_fix
	// memory. A nonzero count here means the emulated firmware genuinely lost a MIDI byte
	// (most likely a note-off) because it hadn't read the previous one yet when the next
	// arrived - independent of munt entirely.
	const uint32_t serialOverruns = processor.getCore().serialOverrunCount();
	g.setColour(serialOverruns > 0 ? kEdValue() : kEdDim());
	g.drawText("MIDI UART overruns (byte lost, firmware too slow to read it): "
	               + juce::String(juce::int64(serialOverruns)) + " time(s) this session",
	           area.removeFromTop(18.0f), juce::Justification::centredLeft);
	area.removeFromTop(8.0f);

	g.setColour(kEdLabel());
	g.setFont(labelFont);
	g.drawText("MIDI IN", area.removeFromTop(16.0f), juce::Justification::centredLeft);

	D110AudioProcessor::MidiLogEntry log[16];
	const int n = processor.getMidiLog(log, 16);
	g.setFont(valueFont);
	const float lineH = 15.0f * scale;
	for (int i = 0; i < n && area.getHeight() > lineH; ++i) {
		const auto &e = log[(size_t)i];
		const int chan = (e.status & 0xf0) < 0xf0 ? (e.status & 0x0f) + 1 : 0;
		juce::String text;
		switch (e.status & 0xf0) {
		case 0x80: text = "note off   ch" + juce::String(chan) + "  " + noteName(e.data1); break;
		case 0x90: text = (e.data2 == 0 ? "note off   ch" : "note on    ch") + juce::String(chan)
		                  + "  " + noteName(e.data1) + "  vel " + juce::String(e.data2); break;
		case 0xB0: text = "control    ch" + juce::String(chan) + "  #" + juce::String(e.data1)
		                  + " = " + juce::String(e.data2); break;
		case 0xC0: text = "program    ch" + juce::String(chan) + "  "
		                  + juce::String(e.data1 + 1); break;
		case 0xE0: text = "pitch bend ch" + juce::String(chan); break;
		default:
			text = (e.status == 0xF0) ? ("exclusive  " + juce::String(e.size) + " bytes")
			                          : ("status " + juce::String::toHexString(e.status));
			break;
		}
		g.setColour(i == 0 ? kEdValue() : kEdDim());
		g.drawText(text, area.removeFromTop(lineH), juce::Justification::centredLeft);
	}
	if (n == 0) {
		g.setColour(kEdDim());
		g.drawText("nothing received yet", area.removeFromTop(lineH),
		           juce::Justification::centredLeft);
	}
}

void D110EditorPane::buttonPressed(int id) {
	if (id == 11) {
		const bool light = d110ui::getTheme() != d110ui::Theme::Light;
		d110ui::setTheme(light ? d110ui::Theme::Light : d110ui::Theme::Dark);
		processor.setUiThemeLight(light);
		if (onThemeChanged) onThemeChanged();
		return;
	}
	if (id == 26) {
		const bool big = !processor.getUiFontScaleBig();
		processor.setUiFontScaleBig(big);
		d110ui::setFontScale(big ? d110ui::FontScale::Big : d110ui::FontScale::Normal);
		if (onFontScaleChanged) onFontScaleChanged();
		layout();
		repaint();
		return;
	}
	if (id == 12) {
		processor.setDebugModeEnabled(!processor.getDebugModeEnabled());
		layout();
		repaint();
		return;
	}
	if (id == 13) {
		processor.setSequencerRetroMode(!processor.getSequencerRetroMode());
		if (onSequencerModeChanged) onSequencerModeChanged();
		layout();
		repaint();
		return;
	}
	if (id == 14) {
		processor.setCompactPanelMode(!processor.getCompactPanelMode());
		if (onCompactPanelModeChanged) onCompactPanelModeChanged();
		layout();
		repaint();
		return;
	}
	if (id == 15) {
		auto &eng = processor.getSequencer();
		eng.setQuantizeMode(eng.getQuantizeMode() == d110seq::QuantizeMode::soft ? d110seq::QuantizeMode::hard
		                                                                         : d110seq::QuantizeMode::soft);
		layout();
		repaint();
		return;
	}
	if (id == 16) {
		// Асинхронный диалог, поэтому объект должен пережить вызов - тот же приём, что и у
		// остальных FileChooser в этом файле.
		const auto startDir = D110AudioProcessor::getCustomRomFolder().isNotEmpty()
		                           ? juce::File(D110AudioProcessor::getCustomRomFolder())
		                           : D110AudioProcessor::getAutoRomFolder();
		auto *chooser = new juce::FileChooser("Choose a folder to search for the D-110 ROM files", startDir);
		chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
		                     [this, chooser](const juce::FileChooser &fc) {
			                     const auto dir = fc.getResult();
			                     if (dir != juce::File()) D110AudioProcessor::setCustomRomFolder(dir.getFullPathName());
			                     layout();
			                     repaint();
			                     delete chooser;
		                     });
		return;
	}
	if (id == 25) {
		const auto startDir = D110AudioProcessor::getSoundbankSourceFolder().isNotEmpty()
		                           ? juce::File(D110AudioProcessor::getSoundbankSourceFolder())
		                           : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
		auto *chooser =
			new juce::FileChooser("Choose the folder of SysEx patch libraries to scan", startDir);
		chooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
			[this, chooser](const juce::FileChooser &fc) {
				const auto dir = fc.getResult();
				if (dir != juce::File()) D110AudioProcessor::setSoundbankSourceFolder(dir.getFullPathName());
				layout();
				repaint();
				delete chooser;
			});
		return;
	}
	if (id == 10) {
		static constexpr int kZoomPresets[] = { 50, 75, 100, 125, 150 };
		const int current = juce::roundToInt(float(getWidth()) / float(D110Panel::currentRefW(processor.getCompactPanelMode())) * 100.0f);
		int next = kZoomPresets[0];
		for (size_t i = 0; i < std::size(kZoomPresets); ++i)
			if (kZoomPresets[i] > current) { next = kZoomPresets[i]; break; }
		if (onRequestZoom) onRequestZoom(next);
		return;
	}
	if (id == 1) {
		processor.sendDisplayMessage(textEntry.getText());
		return;
	}
	if (id == 2) {
		// Диалог асинхронный, поэтому объект обязан пережить вызов; он и живёт в поле панели
		// прибора, у которой уже есть такой же выбор в меню правой кнопки.
		auto onPicked = [this](const juce::File &file) {
			if (file == juce::File()) return;
			processor.setLastDialogDir(file.getParentDirectory());
			// Alan's request, 2026-09-01: a file with more internal Tone Memory records than
			// Bank I has slots for (64) used to just overrun it silently on import - anything
			// past tone 64 was either unreachable or clobbered earlier tones, depending on what
			// the real firmware's own address decode does with an out-of-range write. Counting
			// first and, only over the limit, asking which ones to keep catches that up front
			// instead. A file with <=64 tones (the overwhelming common case, and also any file
			// that isn't tone data at all - Patches/Timbres/System, or empty) is imported exactly
			// as before: whole, unexamined, byte-for-byte.
			const auto tones = d110bank::decodeTonesFromFile(file);
			if (int(tones.size()) <= 64) {
				processor.importSysexBank(file);
				return;
			}
			new SysexTonePickerWindow(processor, file, tones);
		};
		if (TieredNativeFileChooser::isAvailable()) {
			// See TieredNativeFileChooser's own comment: gives a real, independently selectable
			// "All files" fallback in the same dialog, which a single juce::FileChooser pattern
			// string can't express on Linux.
			new TieredNativeFileChooser("Select a SysEx bank or MIDI file", processor.getLastDialogDir(),
			                            "SysEx/MIDI files", "*.syx *.SYX *.mid *.MID *.smf *.SMF", onPicked);
			return;
		}
		auto *chooser = new juce::FileChooser("Select a SysEx bank or MIDI file", processor.getLastDialogDir(),
		                                      "*.syx;*.SYX;*.mid;*.MID;*.smf;*.SMF");
		chooser->launchAsync(juce::FileBrowserComponent::openMode
		                         | juce::FileBrowserComponent::canSelectFiles,
		                     [chooser, onPicked](const juce::FileChooser &fc) {
			                     onPicked(fc.getResult());
			                     delete chooser;
		                     });
		return;
	}
	if (id == 3) {
		for (const Button &b : buttons) {
			if (b.id != 3) continue;
			textEntryTarget = 2;
			textEntryButton = 3;
			textEntry.setText({}, false);
			textEntry.setBounds(b.bounds.toNearestInt());
			textEntry.setVisible(true);
			textEntry.grabKeyboardFocus();
			break;
		}
		return;
	}
	if (id == 7) {
		auto *chooser = new juce::FileChooser("Export SysEx bank as", processor.getLastDialogDir(), "*.syx");
		chooser->launchAsync(juce::FileBrowserComponent::saveMode
		                         | juce::FileBrowserComponent::canSelectFiles
		                         | juce::FileBrowserComponent::warnAboutOverwriting,
		                     [this, chooser](const juce::FileChooser &fc) {
			                     auto file = fc.getResult();
			                     if (file != juce::File()) {
				                     if (!file.hasFileExtension("syx")) file = file.withFileExtension("syx");
				                     processor.setLastDialogDir(file.getParentDirectory());
				                     processor.exportSysexBank(file);
			                     }
			                     delete chooser;
		                     });
		return;
	}
	if (id == 5) {
		// Default filename dated rather than a bare "song.d110snap" - see
		// D110SequencerPanel::showSaveMenu()'s own comment (same reasoning, same "song-" prefix).
		const auto defaultFile = processor.getLastDialogDir().getChildFile(
			juce::Time::getCurrentTime().formatted("song-%Y-%m-%d.d110snap"));
		auto *chooser = new juce::FileChooser("Save memory snapshot as", defaultFile, "*.d110snap");
		chooser->launchAsync(juce::FileBrowserComponent::saveMode
		                         | juce::FileBrowserComponent::canSelectFiles
		                         | juce::FileBrowserComponent::warnAboutOverwriting,
		                     [this, chooser](const juce::FileChooser &fc) {
			                     auto file = fc.getResult();
			                     if (file != juce::File()) {
				                     if (!file.hasFileExtension("d110snap"))
					                     file = file.withFileExtension("d110snap");
				                     processor.setLastDialogDir(file.getParentDirectory());
				                     processor.exportMemorySnapshot(file);
			                     }
			                     delete chooser;
		                     });
		return;
	}
	if (id == 6) {
		auto *chooser = new juce::FileChooser("Load memory snapshot", processor.getLastDialogDir(), "*.d110snap");
		chooser->launchAsync(juce::FileBrowserComponent::openMode
		                         | juce::FileBrowserComponent::canSelectFiles,
		                     [this, chooser](const juce::FileChooser &fc) {
			                     const auto file = fc.getResult();
			                     if (file != juce::File()) {
				                     processor.setLastDialogDir(file.getParentDirectory());
				                     processor.importMemorySnapshot(file);
			                     }
			                     delete chooser;
		                     });
		return;
	}
	// getWidth() would also work here (this drawer, unlike D110Panel, is bounded at real pixel
	// width, not transform-scaled - see D110AudioProcessorEditor::resized()), but going through
	// getTopLevelComponent() keeps both call sites reading the same thing rather than one
	// relying on that distinction staying true.
	if (id == 8) { showLaReferencePopup(getTopLevelComponent()->getWidth()); return; }
	if (id == 9) { processor.midiPanic(); return; }
	if (id == 20) { processor.storeToneFromPart(part, toneSlot); return; }
	if (id == 21) { processor.auditionTone(part, toneSlot); return; }
	if (id == 22) { lockPartials = !lockPartials; layout(); repaint(); return; }
	if (id == 23) { randomizeTone(false); return; }   // DEGRADE
	if (id == 24) { randomizeTone(true); return; }    // RANDOM
	if (id >= 100 && id < 200) {
		// Щелчок по ячейке не просто выделяет её, а СТАВИТ тон в выбранную партию - иначе
		// перебирать шестьдесят четыре тона на слух пришлось бы через кнопку, по два
		// движения на каждый.
		toneSlot = id - 100;
		processor.auditionTone(part, toneSlot);
		layout();
		repaint();
		return;
	}
	if (id >= 200 && id < 300) {
		// Щелчок по номеру патча запоминает его как показанный на под-вкладке PARTS OF
		// PATCH и просит прибор на него перейти - но не переключает саму под-вкладку,
		// чтобы не сбивать пользователя, листающего ALL PATCHES.
		patchSlot = id - 200;
		processor.selectPatch(id - 200);
		layout();
		repaint();
		return;
	}
	if (id >= 400 && id < 500) {
		// Имя патча набирается на своём месте.
		for (const Button &b : buttons) {
			if (b.id != id) continue;
			patchSlot = id - 400;
			textEntryTarget = 3;
			textEntryButton = id;
			textEntry.setText(nameAt(size_t(D110CoreType::kRamPatches)
			                         + size_t(patchSlot) * D110CoreType::kPatchRecord), false);
			textEntry.setBounds(b.bounds.toNearestInt());
			textEntry.setVisible(true);
			textEntry.grabKeyboardFocus();
			break;
		}
		return;
	}
	if (id >= 600 && id < 800) {
		processor.selectTimbreForPart(part, id - 600);
		return;
	}
}

// --- мышь -------------------------------------------------------------------

int D110EditorPane::cellAt(juce::Point<float> p) const {
	for (size_t i = 0; i < cells.size(); ++i)
		if (cells[i].bounds.contains(p)) return int(i);
	return -1;
}

// Список всех тонов сразу, по правому клику - см. вызов в mouseDown(). Группа и номер идут
// парой в одном же нажатии пункта меню: разводить их на два отдельных шага (сначала группа,
// потом номер) значило бы на мгновение отправить прибору несуществующую пару, как это уже
// было измерено и задокументировано для переноса группы/номера патча. Общий код для живой
// области партии (Parts, groupField=0, пишет через sendTimbreTempParam) и записи патча
// (PARTS OF PATCH внутри Patches, groupField=31+12*p, пишет через editPatchField) - у обеих
// та же пара байт группа+номер, разнится только куда её слать и с каким смещением записи.
void D110EditorPane::showToneListMenu(Area area, int index, int groupField) {
	juce::PopupMenu menu;
	for (int group = 0; group < 4; ++group) {
		juce::PopupMenu sub;
		for (int number = 0; number < 64; ++number) {
			juce::String label = juce::String(number + 1) + "  " + toneName(group, number);
			sub.addItem(group * 64 + number + 1, label);
		}
		menu.addSubMenu(toneGroupLabel(group), sub);
	}
	menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
		[this, area, index, groupField](int result) {
			if (result <= 0) return; // отменено
			const int id = result - 1;
			const int group = id / 64, number = id % 64;
			// Through setValue(), not a direct processor.send*Param() call, so this updates the
			// local ram[] mirror and pendingEdits the same way any other cell edit does - see
			// setValue()'s own comment. Without that, a wheel-drag right after picking a tone
			// here read the OLD, not-yet-refreshed cached value as its starting point, so
			// incrementing continued from the REPLACED tone's number instead of the one just
			// picked (Alan's report, 2026-08-06).
			setValue({ {}, area, index, groupField, 0, 3 }, group);
			setValue({ {}, area, index, groupField + 1, 0, 63 }, number);
		});
}

// Правый клик по DRUM SOUND на вкладке Rhythm - тот же приём, но поле не пара группа+номер,
// а один байт 0..127: 0..63 - тембры внутренней памяти (TIMBRE), 64..127 - звуки ударных
// (RHY), см. textOf() Area::Rhythm case 0.
void D110EditorPane::showRhythmSoundMenu(int slot) {
	juce::PopupMenu menu;
	juce::PopupMenu timbreSub;
	for (int n = 0; n < 64; ++n)
		timbreSub.addItem(n + 1, juce::String(n + 1) + "  " + toneName(2, n));
	menu.addSubMenu(toneGroupLabel(2), timbreSub);

	juce::PopupMenu rhySub;
	for (int n = 0; n < 64; ++n)
		rhySub.addItem(64 + n + 1, juce::String(n + 1) + "  " + toneName(3, n));
	menu.addSubMenu(toneGroupLabel(3), rhySub);

	menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
		[this, slot](int result) {
			if (result <= 0) return; // отменено
			processor.sendRhythmParam(slot, 0, uint8_t(result - 1));
			repaint();
		});
}

// Правый клик по PCM - список всех 128 образцов ПЗУ выбранного банка по имени. Банк решает
// соседнее поле WAVEFORM (тот же байт, что читает textOf()'s Area::ToneTemp case 5), поэтому
// список читает его тем же способом, а не спрашивает заново.
void D110EditorPane::showPcmWaveMenu(const Cell &pcmCell) {
	const size_t waveAt = addressOf(pcmCell) - 1;
	const bool bank2 = (waveAt < ram.size()) && ((ram[waveAt] & 2) != 0);
	const char *const *names = bank2 ? kPcmBank2Names : kPcmBank1Names;
	const int currentWave = valueOf(pcmCell);

	juce::PopupMenu menu;
	juce::PopupMenu lower, upper;
	for (int n = 0; n < 64; ++n)
		lower.addItem(n + 1, juce::String(n + 1).paddedLeft('0', 3) + "  " + names[n]);
	for (int n = 64; n < 128; ++n)
		upper.addItem(n + 1, juce::String(n + 1).paddedLeft('0', 3) + "  " + names[n]);
	menu.addSubMenu("001 - 064", lower);
	menu.addSubMenu("065 - 128", upper);
	menu.addSeparator();

	// Custom PCM samples (desktop only): replaces what wave number currentWave actually
	// SOUNDS LIKE - every part/patch referencing it hears the change, the same as swapping a
	// real ROM chip - see D110AudioProcessor::loadCustomPcmWave()'s own comment. ID ranges:
	// 1-128 above (wave picks), 9000/9001 fixed actions below, 9100+i for the i-th sample
	// library entry (capped well under the wave-pick range's own 1-128 to avoid collisions -
	// no overlap possible either way, but keeping them visually far apart in the source too).
	static constexpr int kLoadFromFileId = 9000;
	static constexpr int kRestoreFactoryId = 9001;
	static constexpr int kChooseFolderId = 9002;
	static constexpr int kToggleLoopId = 9003;
	static constexpr int kLibraryBaseId = 9100;

	menu.addItem(kLoadFromFileId, "Load custom sample from file...");

	const auto sampleFolder = D110AudioProcessor::getCustomSampleFolder();
	juce::Array<juce::File> libraryFiles;
	if (sampleFolder.isNotEmpty() && juce::File(sampleFolder).isDirectory()) {
		libraryFiles = juce::File(sampleFolder).findChildFiles(
			juce::File::findFiles, false,
			"*.wav;*.WAV;*.aif;*.AIF;*.aiff;*.AIFF;*.flac;*.FLAC;*.ogg;*.OGG;*.mp3;*.MP3");
		libraryFiles.sort();
	}
	if (sampleFolder.isEmpty()) {
		menu.addItem(kChooseFolderId, "Set sample library folder...");
	} else {
		juce::PopupMenu library;
		for (int i = 0; i < libraryFiles.size() && i < 900; ++i)
			library.addItem(kLibraryBaseId + i, libraryFiles[i].getFileNameWithoutExtension());
		if (libraryFiles.isEmpty())
			library.addItem(kLibraryBaseId - 1, "(no audio files found)", false);
		library.addSeparator();
		library.addItem(kChooseFolderId, "Change folder...");
		menu.addSubMenu("Sample Library (" + juce::String(libraryFiles.size()) + ")", library);
	}

	// Only meaningful (and only shown) once a wave is actually customized - the real LA32 PCM
	// engine only supports "loop the whole stored sample" or "play once", no loop start point
	// or ping-pong (a genuine hardware limitation, not a missing feature here).
	if (processor.hasCustomPcmWave(currentWave)) {
		menu.addItem(kToggleLoopId, juce::String("Loop: ") + (processor.getCustomPcmWaveLoop(currentWave) ? "ON" : "OFF"));
	}

	menu.addItem(kRestoreFactoryId, "Restore factory sample",
	             processor.hasCustomPcmWave(currentWave));

	menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
		[this, pcmCell, currentWave, libraryFiles](int result) {
			if (result <= 0) return; // отменено
			if (result <= 128) {
				setValue(pcmCell, result - 1);
			} else if (result == kLoadFromFileId) {
				auto *chooser = new juce::FileChooser("Choose a custom sample for PCM wave "
					+ juce::String(currentWave + 1));
				chooser->launchAsync(juce::FileBrowserComponent::openMode
				                         | juce::FileBrowserComponent::canSelectFiles,
				                     [this, currentWave, chooser](const juce::FileChooser &fc) {
					                     const auto file = fc.getResult();
					                     if (file != juce::File()) processor.loadCustomPcmWave(currentWave, file);
					                     delete chooser;
				                     });
			} else if (result == kRestoreFactoryId) {
				processor.restoreFactoryPcmWave(currentWave);
			} else if (result == kToggleLoopId) {
				processor.setCustomPcmWaveLoop(currentWave, !processor.getCustomPcmWaveLoop(currentWave));
			} else if (result == kChooseFolderId) {
				auto *chooser = new juce::FileChooser("Choose your sample library folder",
					D110AudioProcessor::getCustomSampleFolder().isNotEmpty()
						? juce::File(D110AudioProcessor::getCustomSampleFolder())
						: juce::File());
				chooser->launchAsync(juce::FileBrowserComponent::openMode
				                         | juce::FileBrowserComponent::canSelectDirectories,
				                     [currentWave, chooser](const juce::FileChooser &fc) {
					                     const auto dir = fc.getResult();
					                     if (dir != juce::File())
						                     D110AudioProcessor::setCustomSampleFolder(dir.getFullPathName());
					                     delete chooser;
				                     });
			} else if (result >= kLibraryBaseId && result - kLibraryBaseId < libraryFiles.size()) {
				processor.loadCustomPcmWave(currentWave, libraryFiles[result - kLibraryBaseId]);
			}
		});
}

void D110EditorPane::mouseDown(const juce::MouseEvent &e) {
	const auto p = e.position;
	grabKeyboardFocus();
	if (!optionsButtonBounds.isEmpty() && optionsButtonBounds.contains(p)) {
		if (onOptionsButtonClicked) onOptionsButtonClicked();
		return;
	}
	for (int i = 0; i < kNumTabs; ++i) {
		if (!tabBounds[(size_t)i].contains(p)) continue;
		selectTab(i);
		return;
	}

	if (tab == Tab::Patches) {
		for (int i = 0; i < 2; ++i) {
			if (!patchesSubTabBounds[(size_t)i].contains(p)) continue;
			patchesSubTab = static_cast<PatchesSubTab>(i);
			layout();
			repaint();
			return;
		}
	}

	if (tab == Tab::Utility && !utilityScrollTrack.isEmpty()) {
		if (utilityScrollThumb.contains(p)) {
			draggingUtilityScroll = true;
			utilityScrollDragStartY = p.y;
			utilityScrollDragStartOffset = utilityScrollOffset;
			return;
		}
		if (utilityScrollTrack.contains(p)) {
			// Clicking the track above/below the thumb pages toward that side, the way an
			// ordinary scrollbar does - not a jump straight to the click point.
			const float page = contentArea.getHeight() * 0.8f;
			utilityScrollOffset += (p.y < utilityScrollThumb.getY() ? -page : page);
			layout();
			repaint();
			return;
		}
	}

	// Правый клик по группе/номеру тона открывает список всех тонов сразу, вместо того чтобы
	// перебирать их колесом по одному. Только TONE GROUP/TONE - у остальных полей нет
	// естественного «имени» на каждое значение, список был бы бессмыслен. Работает и на живой
	// области партии (Parts), и на записи патча (PARTS OF PATCH внутри Patches), и на DRUM
	// SOUND ритм-секции (Rhythm) - три разных поля со своим адресом, но один и тот же приём.
	if (e.mods.isPopupMenu()) {
		if (tab == Tab::Utility && romFolderBounds.contains(p)) {
			D110AudioProcessor::setCustomRomFolder({});
			layout();
			repaint();
			return;
		}
		if (tab == Tab::Utility && soundbankFolderBounds.contains(p)) {
			D110AudioProcessor::setSoundbankSourceFolder({});
			layout();
			repaint();
			return;
		}
		if (tab == Tab::Utility) {
			for (const Button &b : buttons) {
				if (b.id != 1 || !b.bounds.contains(p)) continue;
				const juce::String hex = D110AudioProcessor::displayMessageSysexHex(textEntry.getText());
				juce::PopupMenu m;
				m.addItem("Copy SysEx to clipboard", [hex] { juce::SystemClipboard::copyTextToClipboard(hex); });
				m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
				return;
			}
		}
		if (tab == Tab::Utility && zoomBounds.contains(p)) {
			juce::PopupMenu m;
			static constexpr int kZoomPresets[] = { 50, 75, 100, 125, 150 };
			const int current = juce::roundToInt(float(getWidth()) / float(D110Panel::currentRefW(processor.getCompactPanelMode())) * 100.0f);
			for (int pct : kZoomPresets) m.addItem(pct, juce::String(pct) + "%", true, pct == current);
			m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this](int result) {
				if (result > 0 && onRequestZoom) onRequestZoom(result);
			});
			return;
		}
		// ALL PATCHES row list, right-click -> "Send to real D-110" - Alan's request,
		// 2026-08-29, the Patches-tab equivalent of the Soundbanks browser's own menu of the
		// same name (SoundbankBrowser::showContextMenuFor()). One item, not a submenu: unlike
		// a Tone (which can go to any of several places), a Patch has exactly one natural
		// destination - the same-numbered slot on the real unit, keeping the two in sync.
		if (tab == Tab::Patches && patchesSubTab == PatchesSubTab::AllPatches
		    && tableArea.contains(p) && rowHeight > 0.0f) {
			const int row = int((p.y - tableArea.getY()) / rowHeight);
			const int patch = patchScroll + row;
			if (row >= 0 && patch >= 0 && patch < D110CoreType::kNumPatches) {
				juce::PopupMenu menu;
				menu.addItem("Send to real D-110", [this, patch] {
					if (!processor.hasExternalMidiOutput()) {
						juce::NativeMessageBox::showMessageBoxAsync(
							juce::MessageBoxIconType::WarningIcon, "D-110 Emulator",
							"No MIDI Out device selected - pick one first (MIDI Output menu), "
							"then try again.");
						return;
					}
					processor.sendPatchToExternalMidi(
						patch, ram.data() + D110CoreType::kRamPatches + size_t(patch) * D110CoreType::kPatchRecord);
				});
				menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
				return;
			}
		}
		const int i = cellAt(p);
		if (i >= 0) {
			const Cell &c = cells[(size_t)i];
			if (tab == Tab::Parts && c.area == Area::TimbreTemp
			    && (c.field == 0 || c.field == 1)) {
				showToneListMenu(c.area, c.index, 0);
				return;
			}
			if (tab == Tab::Patches && c.area == Area::Patches) {
				// Запись партии внутри патча начинается на 31-м байте, по 12 байт на партию
				// (см. layoutPatches()); группа стоит первым байтом записи, номер - вторым.
				const int off = c.field - 31;
				if (off >= 0 && off < 8 * 12 && (off % 12 == 0 || off % 12 == 1)) {
					showToneListMenu(c.area, c.index, c.field - (off % 12));
					return;
				}
			}
			if (tab == Tab::Rhythm && c.area == Area::Rhythm && c.field == 0) {
				showRhythmSoundMenu(c.index);
				return;
			}
			if (tab == Tab::Tone && c.area == Area::ToneTemp && c.field >= 14
			    && (c.field - 14) % 58 == 5) {
				showPcmWaveMenu(c);
				return;
			}
		}
	}
	if (tab == Tab::Tone || tab == Tab::Timbres || tab == Tab::Tones) {
		for (int i = 0; i < 8; ++i) {
			if (!partBounds[(size_t)i].contains(p)) continue;
			part = i;
			layout();
			repaint();
			return;
		}
	}
	if (tab == Tab::Tone) {
		for (int i = 0; i < 4; ++i) {
			if (!tonePartialBounds[(size_t)i].contains(p)) continue;
			tonePartial = i;
			layout();
			repaint();
			return;
		}
		// Щелчок по имени тона открывает его для набора прямо на месте. Имя настоящее -
		// прибор показывает его на своём индикаторе.
		if (toneNameBounds.contains(p)) {
			textEntryTarget = 1;
			textEntry.setText(nameAt(size_t(D110CoreType::kRamToneTemp)
			                         + size_t(part) * D110CoreType::kToneRecord), false);
			textEntry.setBounds(toneNameBounds.toNearestInt());
			textEntry.setVisible(true);
			textEntry.grabKeyboardFocus();
			return;
		}
	}
	// На вкладке патчей попадание в строку - это и есть выбор патча: прибор переходит на
	// него, готовый к показу на под-вкладке PARTS OF PATCH при переходе туда вручную.
	// Ловится по всей строке, а не по одному номеру, потому что перебирать патчи на слух
	// надо мышью, а не прицеливаясь в кнопку. Сама под-вкладка ALL PATCHES не переключается -
	// иначе пролистывание патчей мышью выбрасывало бы из списка на каждом клике.
	if (tab == Tab::Patches && patchesSubTab == PatchesSubTab::AllPatches
	    && tableArea.contains(p) && rowHeight > 0.0f) {
		const int row = int((p.y - tableArea.getY()) / rowHeight);
		const int patch = patchScroll + row;
		if (row >= 0 && patch >= 0 && patch < D110CoreType::kNumPatches && patch != patchSlot) {
			patchSlot = patch;
			processor.selectPatch(patch);
			layout();
			repaint();
			// Значение под курсором всё равно остаётся правимым: выбор патча и правка его
			// поля - разные вещи, и обе делаются одним и тем же щелчком по одной строке.
		}
	}

	for (const Button &b : buttons) {
		if (!b.bounds.contains(p)) continue;
		buttonPressed(b.id);
		return;
	}

	// PARTIAL MUTE (Github issue #1): four independent toggles, not a drag/wheel field - a
	// click just flips whichever partial's button it landed on and stops there.
	{
		const int i = cellAt(p);
		if (i >= 0 && isPartialMuteCell(cells[(size_t)i])) {
			const Cell &c = cells[(size_t)i];
			const int v = valueOf(c);
			if (v >= 0) {
				int bit = 3;
				for (int partial = 0; partial < 4; ++partial)
					if (partialMuteSegment(c, partial).contains(p)) { bit = partial; break; }
				setValue(c, v ^ (1 << bit));
			}
			return;
		}
	}

	dragging = cellAt(p);
	// PARTS OF PATCH: whatever cell the click landed on (or -1, clicking away clears it)
	// becomes the target for Up/Down arrow-key nudges, mirroring the mouse wheel's own
	// "whatever cell is under the cursor" reach - see keyPressed().
	if (tab == Tab::Patches) focusedPatchCell = dragging;
	if (dragging < 0) return;
	dragStartY = p.y;
	dragStartValue = valueOf(cells[(size_t)dragging]);
	repaint();
}

void D110EditorPane::mouseDrag(const juce::MouseEvent &e) {
	if (draggingUtilityScroll) {
		const float maxScroll = juce::jmax(0.0f, utilityContentHeight - contentArea.getHeight());
		const float trackRange = juce::jmax(1.0f, utilityScrollTrack.getHeight()
		                                          - utilityScrollThumb.getHeight());
		const float deltaPx = e.position.y - utilityScrollDragStartY;
		utilityScrollOffset = juce::jlimit(0.0f, maxScroll,
		    utilityScrollDragStartOffset + deltaPx * (maxScroll / trackRange));
		layout();
		repaint();
		return;
	}
	if (dragging < 0 || dragStartValue < 0) return;
	// Четыре точки на шаг: достаточно мелко для громкости 0..100 и достаточно крупно, чтобы
	// поле из двух положений не прыгало от дрожания руки.
	const int steps = int((dragStartY - e.position.y) / 4.0f);
	setValue(cells[(size_t)dragging], dragStartValue + steps);
}

void D110EditorPane::mouseUp(const juce::MouseEvent &) {
	if (draggingUtilityScroll) { draggingUtilityScroll = false; return; }
	if (dragging < 0) return;
	dragging = -1;
	repaint();
}

void D110EditorPane::mouseMove(const juce::MouseEvent &e) {
	const int was = hovered;
	hovered = cellAt(e.position);
	setMouseCursor(hovered >= 0 ? juce::MouseCursor::UpDownResizeCursor
	                            : juce::MouseCursor::NormalCursor);
	if (hovered != was) repaint();
}

void D110EditorPane::mouseExit(const juce::MouseEvent &) {
	if (hovered < 0) return;
	hovered = -1;
	repaint();
}

void D110EditorPane::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
	const int i = cellAt(e.position);
	if (i >= 0) {
		if (isPartialMuteCell(cells[(size_t)i])) return;   // click-to-toggle only, see mouseDown()
		const int v = valueOf(cells[(size_t)i]);
		if (v < 0) return;
		setValue(cells[(size_t)i], v + (w.deltaY > 0 ? 1 : -1));
		return;
	}
	// Колесо мимо полей листает длинные списки: клавиш ударных восемьдесят пять, тембров сто
	// двадцать восемь, патчей шестьдесят четыре, а на экран помещается меньше.
	if (tab == Tab::Rhythm) rhythmScroll += (w.deltaY > 0 ? -3 : 3);
	else if (tab == Tab::Timbres) timbreScroll += (w.deltaY > 0 ? -3 : 3);
	else if (tab == Tab::Patches && patchesSubTab == PatchesSubTab::AllPatches) patchScroll += (w.deltaY > 0 ? -3 : 3);
	else if (tab == Tab::Tones) toneScroll += (w.deltaY > 0 ? -3 : 3);
	else if (tab == Tab::Utility) utilityScrollOffset += (w.deltaY > 0 ? -40.0f : 40.0f);
	else return;
	layout();
	repaint();
}

bool D110EditorPane::keyPressed(const juce::KeyPress &key) {
	const bool up = key.isKeyCode(juce::KeyPress::upKey);
	const bool down = key.isKeyCode(juce::KeyPress::downKey);
	const bool left = key.isKeyCode(juce::KeyPress::leftKey);
	const bool right = key.isKeyCode(juce::KeyPress::rightKey);
	if (!up && !down && !left && !right) return false;

	// TONES: walk the 3-column grid built in layoutTones() - column-major, so col/r come
	// from dividing the linear slot number by the row count, same math as that layout.
	// Consumes the key even at a grid edge (nothing to move to) so it doesn't leak up to the
	// retro sequencer's own D-pad handling while this tab is what the user is navigating.
	if (tab == Tab::Tones) {
		const int rows = juce::jmax(1, toneRows);
		int col = (toneSlot - toneScroll) / rows;
		int r = (toneSlot - toneScroll) % rows;
		if (up)        { if (r > 0) --r; else return true; }
		else if (down) { if (r < rows - 1) ++r; else return true; }
		else if (left) { if (col > 0) --col; else return true; }
		else           { if (col < 2) ++col; else return true; }
		const int newSlot = toneScroll + col * rows + r;
		if (newSlot < 0 || newSlot >= D110CoreType::kNumTones) return true;
		// Same as clicking the slot Button in mouseDown()'s generic button loop - id 100+slot -
		// select AND audition in one move.
		toneSlot = newSlot;
		processor.auditionTone(part, toneSlot);
		layout();
		repaint();
		return true;
	}

	// PARTS OF PATCH: Up/Down nudges whichever cell was last clicked, the same as the mouse
	// wheel already does to whatever cell is under the cursor.
	if (tab == Tab::Patches && patchesSubTab == PatchesSubTab::PartsOfPatch
	    && (up || down) && focusedPatchCell >= 0 && focusedPatchCell < int(cells.size())) {
		const Cell &c = cells[(size_t)focusedPatchCell];
		const int v = valueOf(c);
		if (v < 0) return true;
		setValue(c, v + (up ? 1 : -1));
		repaint();
		return true;
	}

	return false;
}

// ---------------------------------------------------------------------------
// Карта памяти

D110MemoryCard::D110MemoryCard(D110AudioProcessor &p) : processor(p) {
	cardImage = juce::ImageCache::getFromMemory(BinaryData::memory_card_m256d_png,
	                                            BinaryData::memory_card_m256d_pngSize);
	// Окно можно закрыть и открыть заново, а карта всё это время остаётся там, где её
	// оставили: положение берётся у прибора, а не с нуля.
	travel = target = processor.getCore().cardInserted() ? 0.0f : 1.0f;
	setInterceptsMouseClicks(false, false);
	startTimerHz(60);
}

// Левый верхний угол карты в опорных точках панели. Путь от гнезда до места на ящике -
// прямая, и положение на нём это смесь двух концов: так закон движения остаётся тем же,
// каким его сняли раскадровкой (plugin/panel_render.cpp), и при этом карта может лежать
// где угодно, куда её утащили мышью.
juce::Point<float> D110MemoryCard::position() const {
	return { kCardX + (rest.x - kCardX) * travel,
	         kCardSeatedY + (rest.y - kCardSeatedY) * travel };
}

void D110MemoryCard::setGeometry(float panelScale, float totalRefHeight) {
	scale = panelScale;
	totalRefH = totalRefHeight;
	updateBounds();
}

// Компонент занимает ровно то, что от карты ВИДНО. Верх обрезан по кромке щели: карта, стоящая
// в гнезде, торчит из него на восемнадцать точек, и всё, что выше, - это уже прибор, поверх
// которого карте лезть незачем.
void D110MemoryCard::updateBounds() {
	const auto p = position();
	const float visibleTop = juce::jmax(p.y, kCardClipTop);
	const float visibleH = p.y + kCardHeight - visibleTop;
	if (visibleH <= 1.0f || !cardImage.isValid()) {
		setVisible(false);
		return;
	}
	setVisible(true);
	setBounds(juce::Rectangle<float>(p.x * scale, visibleTop * scale,
	                                 kCardWidth * scale, visibleH * scale).toNearestInt());
	// Мышь карта перехватывает только когда она вправду лежит на ящике. Пока она в гнезде
	// или ещё едет, щелчок обязан достаться щели на панели - иначе карту нельзя было бы
	// вставить обратно тем же движением, каким её вынули.
	setInterceptsMouseClicks(travel > 0.999f, false);
}

void D110MemoryCard::paint(juce::Graphics &g) {
	if (!cardImage.isValid()) return;

	const auto p = position();
	const float visibleTop = juce::jmax(p.y, kCardClipTop);
	// Картинка рисуется целиком, но со сдвигом вверх на срезанную часть: обрезает её граница
	// самого компонента.
	g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
	g.drawImage(cardImage,
	            juce::Rectangle<float>(0.0f, (p.y - visibleTop) * scale,
	                                   kCardWidth * scale, kCardHeight * scale),
	            juce::RectanglePlacement::stretchToFit);

	// То, что видно НАД полом проёма, - это не наклейка, а ТОРЕЦ карты: чёрная пластиковая
	// стенка её корпуса. Картинка же - вид на карту плашмя, и её верхние строки кремовые,
	// поэтому вставленная карта отсвечивала в щели белым, чего у настоящей быть не может.
	// Полоса в проёме закрашивается цветом корпуса, а не затемняется наполовину.
	if (visibleTop >= kSlotBottom) return;
	const float edgeH = (kSlotBottom - visibleTop) * scale;
	g.setColour(juce::Colour(0xff121214));
	g.fillRect(juce::Rectangle<float>(0.0f, 0.0f, float(getWidth()), edgeH));
	// Тонкая светлая грань по самому верху - ребро корпуса ловит свет, и без неё торец
	// сливается с чернотой щели в сплошное пятно.
	g.setColour(juce::Colour(0x40ffffff));
	g.fillRect(juce::Rectangle<float>(0.0f, 0.0f, float(getWidth()),
	                                  juce::jmax(1.0f, scale)));
	// И тень, которую проём бросает на карту сразу под собой: без неё карта выходит из щели
	// по резкой линии, будто нарисована поверх панели.
	const float shadeH = 14.0f * scale;
	g.setGradientFill(juce::ColourGradient(
		juce::Colours::black.withAlpha(kSlotShadeAlpha), 0.0f, edgeH,
		juce::Colours::transparentBlack, 0.0f, edgeH + shadeH, false));
	g.fillRect(juce::Rectangle<float>(0.0f, edgeH, float(getWidth()), shadeH));
}

void D110MemoryCard::toggle() {
	if (target > 0.5f) { insert(); return; }

	// Извлечение. Контакты размыкаются, как только карта тронулась из гнезда, а замыкаются
	// лишь когда она села до конца (см. timerCallback). Прошивка узнаёт карту, ПИША в неё,
	// так что «наполовину вставленная» карта для неё не отличается от вставленной, и делать
	// вид, будто отличается, было бы враньём о железе.
	target = 1.0f;
	processor.getCore().setCardInserted(false);
	// Ящик открывается сам: карте надо куда-то лечь, а на закрытом ящике места нет.
	if (onEjectNeedsDrawer) onEjectNeedsDrawer();
}

void D110MemoryCard::insert() {
	target = 0.0f;
	dragging = false;
}

void D110MemoryCard::mouseDown(const juce::MouseEvent &e) {
	if (travel < 0.999f) return;
	dragging = true;
	// Запоминается место хвата, а не центр: карта не должна прыгать под курсор.
	dragGrab = { float(e.x) / scale, float(e.y) / scale };
	setMouseCursor(juce::MouseCursor::DraggingHandCursor);
}

void D110MemoryCard::mouseDrag(const juce::MouseEvent &e) {
	if (!dragging) return;
	// Точка мыши в опорных точках окна: событие приходит в координатах компонента, а он сам
	// ездит, поэтому к ним прибавляется его собственное положение.
	const float x = (float(getX()) + float(e.x)) / scale - dragGrab.x;
	const float y = (float(getY()) + float(e.y)) / scale - dragGrab.y;
	// Карта не залезает на прибор и не уходит за край окна: она остаётся целиком видимой в
	// пределах ящика, ради которого всё это и делалось.
	// Восемь точек запаса снизу: без них карту можно было положить ровно по нижнему краю
	// окна, и последняя строчка её надписи оказывалась срезана - выглядело это не как
	// «положили к краю», а как «карта не поместилась».
	constexpr float kMargin = 8.0f;
	const float top = float(D110Panel::kRefH) + D110AudioProcessorEditor::kHandleRefH;
	rest = { juce::jlimit(0.0f, float(D110Panel::kRefW) - kCardWidth, x),
	         juce::jlimit(top, juce::jmax(top, totalRefH - kCardHeight - kMargin), y) };
	updateBounds();
	repaint();
}

void D110MemoryCard::mouseUp(const juce::MouseEvent &) {
	dragging = false;
	setMouseCursor(juce::MouseCursor::NormalCursor);
}

void D110MemoryCard::timerCallback() {
	// Картинка следует за прибором, а не наоборот. Окно можно открыть уже после того, как
	// проект восстановил вынутую карту, и тогда рисовать её в гнезде было бы враньём. Сверка
	// делается только на покое, чтобы не спорить с идущим ходом.
	if (travel == target) {
		const float shouldBe = processor.getCore().cardInserted() ? 0.0f : 1.0f;
		if (target != shouldBe) target = shouldBe;
	}
	if (travel == target) return;

	// Ход занимает около девяти десятых секунды при 60 кадрах в секунду. Скорость зависит от
	// того, насколько карта далека от ОБОИХ упоров, поэтому она трогается мягко, разгоняется
	// к середине и мягко подходит к упору. Прежний закон считал только оставшееся расстояние:
	// карта срывалась с места на полной скорости и первую треть пути была неразличима, а
	// замедлялась там, где смотреть уже не на что.
	const float dir = target > travel ? 1.0f : -1.0f;
	const float fromEnd = juce::jmin(travel, 1.0f - travel);   // ноль у любого упора
	travel += dir * kCardStep * (0.30f + 1.40f * fromEnd);
	if (dir * (travel - target) > 0.0f) {
		travel = target;
		// Карта села в разъём - только теперь она есть для прошивки.
		if (travel == 0.0f) processor.getCore().setCardInserted(true);
	}
	updateBounds();
	repaint();
}

// ---------------------------------------------------------------------------
// Test keyboard

// ---------------------------------------------------------------------------

D110AudioProcessorEditor::D110AudioProcessorEditor(D110AudioProcessor &p)
	: juce::AudioProcessorEditor(&p), processor(p), panel(p), editorPane(p), card(p), keyboard(p),
	  sequencerPanel(p), sequencerRetroPanel(p)
{
	// Synced here, not just read lazily by whichever drawer paints first: a project loaded
	// with the light theme should look right the moment this editor appears, including on
	// the very first paint.
	d110ui::setTheme(p.getUiThemeLight() ? d110ui::Theme::Light : d110ui::Theme::Dark);
	// Same idea for the FONT SIZE toggle - see UiTheme.h's own comment on why the actual
	// Desktop-global call only ever happens in a Standalone build: a VST3/AU instance shares
	// its process with the host and every other plugin, so setGlobalScaleFactor() there would
	// resize the DAW itself, not just this editor.
	d110ui::setFontScale(p.getUiFontScaleBig() ? d110ui::FontScale::Big : d110ui::FontScale::Normal);
#if JucePlugin_Build_Standalone
	juce::Desktop::getInstance().setGlobalScaleFactor(
		p.getUiFontScaleBig() ? d110ui::kBigScaleFactor : 1.0f);
#endif
	// Same idea for the editor pane's own height - read before totalRefHeight() is first
	// called just below, so a project saved with a resized drawer opens already that size.
	editorPaneRefH = juce::jlimit(kMinPaneRefH, kMaxPaneRefH, p.getEditorPaneRefH());
	keyboardPaneRefH = juce::jlimit(kMinKeyboardPaneRefH, kMaxKeyboardPaneRefH, p.getKeyboardPaneRefH());
	sequencerPaneRefH = juce::jlimit(kMinSequencerPaneRefH, kMaxSequencerPaneRefH, p.getSequencerPaneRefH());

	addAndMakeVisible(panel);
	addAndMakeVisible(editorPane);
	addAndMakeVisible(keyboard);
	addAndMakeVisible(sequencerPanel);
	addChildComponent(sequencerRetroPanel); // shown instead of sequencerPanel in retro mode - see resized()
	// Карта добавляется последней и потому лежит поверх обоих - и прибора, и ящика.
	addAndMakeVisible(card);

	sequencerPanel.setVisible(!processor.getSequencerRetroMode());
	sequencerRetroPanel.setVisible(processor.getSequencerRetroMode());
	// Nothing to show in compact mode - the slot itself is spliced out of the panel photo.
	card.setVisible(!processor.getCompactPanelMode());

	panel.onCardSlotClicked = [this] { card.toggle(); };
	auto refreshSequencerMode = [this] {
		sequencerPanel.setVisible(!processor.getSequencerRetroMode());
		sequencerRetroPanel.setVisible(processor.getSequencerRetroMode());
		sequencerPanel.repaint();
		sequencerRetroPanel.repaint();
	};
	panel.onSequencerModeChanged = refreshSequencerMode;
	editorPane.onSequencerModeChanged = refreshSequencerMode;
	editorPane.onOptionsButtonClicked = [this] { panel.showOptionsMenu(); };
	card.onEjectNeedsDrawer = [this] {
		expansion = expansionTarget = 1.0f;
		applySize();
	};

	// Utility tab's PANEL SIZE toggle (see D110EditorPane::onCompactPanelModeChanged's own
	// comment). processor.getCompactPanelMode() has already flipped by the time this runs -
	// D110EditorPane::buttonPressed sets it before calling the callback, same order as the
	// SEQUENCER toggle above. Keeps the window at the same reference-to-screen scale it was
	// already at, re-measured against the new (narrower/wider) reference width, so the window
	// shrinks/grows by exactly the spliced section's own screen size rather than jumping to a
	// fixed zoom percent; a card left out of its slot is put back first, the same guard the
	// drawer-close handler above already needs, since compact mode hides the card outright.
	editorPane.onCompactPanelModeChanged = [this] {
		if (card.isOut()) card.insert();
		const bool compact = processor.getCompactPanelMode();
		const float s = float(getWidth()) / float(D110Panel::currentRefW(!compact));
		const int targetW = juce::roundToInt(float(D110Panel::currentRefW(compact)) * s);
		constrainer.setFixedAspectRatio(double(D110Panel::currentRefW(compact)) / double(totalRefHeight()));
		constrainer.setSizeLimits(900, 100, D110Panel::currentRefW(compact) * 2, 4000);
		card.setVisible(!compact);
		setSize(targetW, int(totalRefHeight() * (float(targetW) / float(D110Panel::currentRefW(compact))) + 0.5f));
		repaint();
	};

	// Ящик закрыт при открытии окна: плагин - это прибор, а редактор к нему добавлен, и до
	// тех пор, пока его не попросили, он не занимает места.
	constrainer.setFixedAspectRatio(double(D110Panel::currentRefW(processor.getCompactPanelMode())) / double(totalRefHeight()));
	constrainer.setSizeLimits(900, 100, D110Panel::currentRefW(processor.getCompactPanelMode()) * 2, 4000);
	setConstrainer(&constrainer);

	setResizable(true, true);
	setSize(1500, int(totalRefHeight() * (1500.0f / float(D110Panel::currentRefW(processor.getCompactPanelMode()))) + 0.5f));

	// Zoom presets (Utility tab) call this to resize precisely, the same way a manual
	// drag-resize already does reliably - see D110EditorPane::onRequestZoom's own comment for
	// why this exists instead of a maximise button (feedback_no_blind_wm_fixes memory: a native
	// one sent the window off-screen on Alan's real desktop, twice now).
	editorPane.onRequestZoom = [this](int percent) {
		const int targetW = juce::roundToInt(float(D110Panel::currentRefW(processor.getCompactPanelMode())) * float(percent) / 100.0f);
		setSize(targetW, int(totalRefHeight() * (float(targetW) / float(D110Panel::currentRefW(processor.getCompactPanelMode()))) + 0.5f));
	};

	// The THEME toggle flips a process-wide palette (UiTheme.h) - every drawer needs a
	// fresh paint to pick it up, not just the Utility tab that owns the button.
	editorPane.onThemeChanged = [this] {
		editorPane.repaint();
		keyboard.repaint();
		sequencerPanel.repaint();
		sequencerRetroPanel.repaint();
		repaint();
	};

	// The FONT SIZE toggle's actual effect - process-wide, Standalone-only, see UiTheme.h's
	// own comment on why a VST3/AU build never calls this.
	editorPane.onFontScaleChanged = [this] {
#if JucePlugin_Build_Standalone
		juce::Desktop::getInstance().setGlobalScaleFactor(
			processor.getUiFontScaleBig() ? d110ui::kBigScaleFactor : 1.0f);
#endif
	};

	// No ROMs found on this launch (or the auto-scan failed for some other reason) - offer
	// the folder picker right here, up front, rather than making the user find the Utility
	// tab first. Deferred with callAsync so the editor's own window exists (an AlertWindow
	// needs a screen to appear on) before the dialog pops up.
	if (!processor.isSynthReady())
		juce::MessageManager::callAsync([this] { showRomSetupDialog(); });
}

void D110AudioProcessorEditor::showRomSetupDialog() {
	if (processor.isSynthReady()) return;

	auto *aw = new juce::AlertWindow("D-110 ROM files not found",
	                                 processor.getLastError().isNotEmpty()
	                                     ? processor.getLastError()
	                                     : juce::String("The D-110 emulator needs the original Roland ROM files "
	                                                    "to run. See the Utility tab, or docs/roms.md, for where "
	                                                    "to get them and where to put them."),
	                                 juce::AlertWindow::WarningIcon);
	aw->addButton("Choose ROM Folder...", 1);
	aw->addButton("Later", 0);
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this](int result) {
		if (result != 1) return;
		// Async dialog, so the chooser has to outlive this call - same trick as every other
		// FileChooser in this file.
		auto *chooser = new juce::FileChooser("Choose a folder to search for the D-110 ROM files",
		                                      D110AudioProcessor::getAutoRomFolder());
		chooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
			[this, chooser](const juce::FileChooser &fc) {
				const auto dir = fc.getResult();
				if (dir != juce::File()) {
					D110AudioProcessor::setCustomRomFolder(dir.getFullPathName());
					processor.reloadRomsAndPowerOn();
					refreshFromInstrument();
					repaint();
					if (!processor.isSynthReady()) showRomSetupDialog();
				}
				delete chooser;
			});
	}), true);
}

void D110AudioProcessorEditor::parentHierarchyChanged()
{
	juce::AudioProcessorEditor::parentHierarchyChanged();
	// Plugin builds: no such window exists (the host draws its own), and wrapperType tells
	// them apart at runtime same as PluginProcessor.cpp already does elsewhere. Standalone:
	// StandaloneFilterWindow's ctor hardcodes JUCE's own custom-drawn title bar (the "Options"
	// button lived there - now on the panel's right-click menu, see D110Panel::showOptionsMenu),
	// there's no constructor hook to ask for the native one instead, so it's flipped here, the
	// first time this editor is far enough up the hierarchy to reach that window. Guarded by
	// isUsingNativeTitleBar() so repeated hierarchy-change notifications don't keep tearing
	// down and rebuilding the native window peer.
	if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone) return;
	if (auto *dw = dynamic_cast<juce::DocumentWindow *>(getTopLevelComponent()))
		// Deliberately NOT calling setTitleBarButtonsRequired(allButtons, ...) here: a native
		// maximise button was tried on this same window in an earlier session (2026-08-06) and
		// sent it off-screen on Alan's real desktop (confirmed again 2026-08-19) - see the
		// feedback_no_blind_wm_fixes memory. Stick to minimise+close (StandaloneFilterWindow's
		// own default) unless Alan explicitly asks for maximise again.
		if (!dw->isUsingNativeTitleBar()) dw->setUsingNativeTitleBar(true);
}

float D110AudioProcessorEditor::totalRefHeight() const {
	return float(D110Panel::kRefH) + kHandleRefH + expansion * editorPaneRefH
	     + kKeyboardHandleRefH + keyboardExpansion * keyboardPaneRefH
	     + kSequencerHandleRefH + sequencerExpansion * sequencerPaneRefH
	     + sequencerExpansion * kSequencerResizeGripRefH;
}

void D110AudioProcessorEditor::applySize() {
	constrainer.setFixedAspectRatio(double(D110Panel::currentRefW(processor.getCompactPanelMode())) / double(totalRefHeight()));
	const float s = float(getWidth()) / float(D110Panel::currentRefW(processor.getCompactPanelMode()));
	setSize(getWidth(), int(totalRefHeight() * s + 0.5f));
}

// Полоса-ручка: во всю ширину, сразу под фотографией. Полная ширина затем, чтобы она
// читалась ящиком, который выдвигают, а не кнопкой.
juce::Rectangle<float> D110AudioProcessorEditor::handleBand() const {
	const float s = float(getWidth()) / float(D110Panel::currentRefW(processor.getCompactPanelMode()));
	return { 0.0f, float(D110Panel::kRefH) * s, float(getWidth()), kHandleRefH * s };
}

// Its own handle band, stacked below wherever the editor's own drawer currently ends - so
// it follows that drawer up and down as it opens and closes, exactly as the drawer's own
// band follows the panel.
juce::Rectangle<float> D110AudioProcessorEditor::keyboardHandleBand() const {
	const float s = float(getWidth()) / float(D110Panel::currentRefW(processor.getCompactPanelMode()));
	const float top = (float(D110Panel::kRefH) + kHandleRefH + expansion * editorPaneRefH) * s;
	return { 0.0f, top, float(getWidth()), kKeyboardHandleRefH * s };
}

// Same trick a third time: stacked below wherever the keyboard drawer currently ends.
juce::Rectangle<float> D110AudioProcessorEditor::sequencerHandleBand() const {
	const float s = float(getWidth()) / float(D110Panel::currentRefW(processor.getCompactPanelMode()));
	const float top = (float(D110Panel::kRefH) + kHandleRefH + expansion * editorPaneRefH
	                  + kKeyboardHandleRefH + keyboardExpansion * keyboardPaneRefH) * s;
	return { 0.0f, top, float(getWidth()), kSequencerHandleRefH * s };
}

// The sequencer drawer's own resize grip, right under it - see kSequencerResizeGripRefH's
// comment for why this one isn't a dual-role band like the two above. Zero height (so it
// never hit-tests true) whenever the drawer itself is collapsed, exactly like the drawer's
// own bounds in resized() below.
juce::Rectangle<float> D110AudioProcessorEditor::sequencerResizeBand() const {
	const float s = float(getWidth()) / float(D110Panel::currentRefW(processor.getCompactPanelMode()));
	const float top = (float(D110Panel::kRefH) + kHandleRefH + expansion * editorPaneRefH
	                  + kKeyboardHandleRefH + keyboardExpansion * keyboardPaneRefH
	                  + kSequencerHandleRefH + sequencerExpansion * sequencerPaneRefH) * s;
	return { 0.0f, top, float(getWidth()), sequencerExpansion * kSequencerResizeGripRefH * s };
}

namespace {
// Shared by both drawer handles - the editor's own and the test keyboard's. The chevron
// points away from the drawer's current resting edge: down when collapsed (there's more to
// reveal below), up when open.
void paintDrawerHandle(juce::Graphics &g, juce::Rectangle<float> band, bool open, bool hover,
                       const char *labelWhenClosed) {
	const auto &pal = d110ui::palette();
	g.setColour(pal.handleBg);
	g.fillRect(band);
	g.setColour(hover ? pal.handleBarHover : pal.handleBar);
	g.fillRect(band.reduced(0.0f, band.getHeight() * 0.28f));

	const float cx = band.getCentreX();
	const float cy = band.getCentreY();
	const float a = juce::jmax(3.0f, band.getHeight() * 0.22f);
	const float dir = open ? -1.0f : 1.0f;
	juce::Path chevron;
	chevron.startNewSubPath(cx - a * 1.6f, cy - a * 0.5f * dir);
	chevron.lineTo(cx, cy + a * 0.5f * dir);
	chevron.lineTo(cx + a * 1.6f, cy - a * 0.5f * dir);
	g.setColour(hover ? pal.handleChevronHover : pal.handleChevron);
	g.strokePath(chevron, juce::PathStrokeType(juce::jmax(1.5f, a * 0.35f)));

	// Label next to the chevron, but only while closed: open already speaks for itself.
	if (!open) {
		g.setFont(juce::FontOptions(juce::jlimit(9.0f, 13.0f, band.getHeight() * 0.55f)));
		g.setColour(pal.handleLabel);
		g.drawText(labelWhenClosed, band.withTrimmedLeft(14.0f), juce::Justification::centredLeft);
	}
}
} // namespace

void D110AudioProcessorEditor::paint(juce::Graphics &g)
{
	g.fillAll(d110ui::palette().panelBg);
	paintDrawerHandle(g, handleBand(), expansion > 0.5f, handleHover, "EDITOR");
	paintDrawerHandle(g, keyboardHandleBand(), keyboardExpansion > 0.5f, keyboardHandleHover, "KEYBOARD");
	paintDrawerHandle(g, sequencerHandleBand(), sequencerExpansion > 0.5f, sequencerHandleHover, "SEQUENCER");

	// Pure resize grip, no chevron/label - it doesn't fold anything, it's just a thicker
	// grab target than the drawer's own 1px edge would be. Same bar colour as the other
	// bands so it still reads as "part of this drawer's furniture".
	const auto grip = sequencerResizeBand();
	if (!grip.isEmpty()) {
		const auto &pal = d110ui::palette();
		g.setColour(pal.handleBg);
		g.fillRect(grip);
		g.setColour(sequencerResizeHover ? pal.handleBarHover : pal.handleBar);
		g.fillRect(grip.reduced(0.0f, grip.getHeight() * 0.28f));
	}
}

void D110AudioProcessorEditor::mouseDown(const juce::MouseEvent &e)
{
	// Drawers snap open/closed instantly rather than easing - see timerCallback()'s own
	// comment. A resize driven by a live window drag (host or window manager) can overlap
	// a drawer click, and while the easing was still catching up it kept calling
	// applySize()/setSize() for several more frames after the drag's own mouse-up - which
	// read as the window continuing to resize on its own.
	if (handleBand().contains(e.position)) {
		expansionTarget = (expansionTarget > 0.5f) ? 0.0f : 1.0f;
		expansion = expansionTarget;
		// Ящик закрывают - карте негде лежать, и она возвращается в гнездо. Оставить её висеть
		// за нижним краем окна значило бы потерять её из виду, не сказав об этом; а «не даём
		// закрыть ящик, пока карта снаружи» - это запрет там, где хватает движения.
		if (expansionTarget < 0.5f && card.isOut()) card.insert();
		applySize();
		return;
	}
	if (keyboardHandleBand().contains(e.position)) {
		// Dual role: this band is also the boundary directly below the editor pane, so
		// dragging it (rather than just clicking it) resizes that pane instead of toggling
		// the keyboard drawer - see mouseDrag()'s threshold check, which decides which of
		// the two this gesture turns out to be. Only meaningful while the editor is open;
		// while it's closed there's nothing above this band to resize, so it stays a plain
		// toggle exactly as before.
		keyboardHandlePressed = true;
		resizingEditorPane = false;
		resizeDragStartY = e.position.y;
		resizeDragStartRefH = editorPaneRefH;
		return;
	}
	if (sequencerHandleBand().contains(e.position)) {
		// Same dual role, one drawer down: drag resizes the KEYBOARD pane above this band,
		// plain click toggles the sequencer drawer - see keyboardHandleBand's case just above.
		sequencerHandlePressed = true;
		resizingKeyboardPane = false;
		resizeDragStartY = e.position.y;
		resizeDragStartRefH = keyboardPaneRefH;
		return;
	}
	if (sequencerResizeBand().contains(e.position)) {
		// No toggle role here - the sequencer drawer is the last one, nothing below it to
		// fold - so this is a plain resize grab from the first pixel of movement.
		sequencerResizeHandlePressed = true;
		resizeDragStartY = e.position.y;
		resizeDragStartRefH = sequencerPaneRefH;
		return;
	}
}

void D110AudioProcessorEditor::mouseDrag(const juce::MouseEvent &e)
{
	const float s = float(getWidth()) / float(D110Panel::currentRefW(processor.getCompactPanelMode()));
	const float deltaY = e.position.y - resizeDragStartY;

	if (keyboardHandlePressed) {
		// A few pixels of slop before a press-and-hold turns into a resize, so an ordinary
		// click (which always jitters slightly between down and up) doesn't accidentally
		// nudge the height - only actually resizes once the editor pane is open, since
		// dragging this band with nothing above it to resize wouldn't do anything visible.
		if (!resizingEditorPane) {
			if (expansion < 0.5f || std::abs(deltaY) < 4.0f) return;
			resizingEditorPane = true;
		}
		editorPaneRefH = juce::jlimit(kMinPaneRefH, kMaxPaneRefH, resizeDragStartRefH + deltaY / s);
		applySize();
		return;
	}
	if (sequencerHandlePressed) {
		if (!resizingKeyboardPane) {
			if (keyboardExpansion < 0.5f || std::abs(deltaY) < 4.0f) return;
			resizingKeyboardPane = true;
		}
		keyboardPaneRefH =
			juce::jlimit(kMinKeyboardPaneRefH, kMaxKeyboardPaneRefH, resizeDragStartRefH + deltaY / s);
		applySize();
		return;
	}
	if (sequencerResizeHandlePressed) {
		sequencerPaneRefH =
			juce::jlimit(kMinSequencerPaneRefH, kMaxSequencerPaneRefH, resizeDragStartRefH + deltaY / s);
		applySize();
	}
}

void D110AudioProcessorEditor::mouseUp(const juce::MouseEvent &)
{
	if (sequencerResizeHandlePressed) {
		sequencerResizeHandlePressed = false;
		processor.setSequencerPaneRefH(sequencerPaneRefH);
		return;
	}
	if (sequencerHandlePressed) {
		sequencerHandlePressed = false;
		if (resizingKeyboardPane) {
			processor.setKeyboardPaneRefH(keyboardPaneRefH);
			resizingKeyboardPane = false;
			return;
		}
		// No real drag happened - an ordinary click, so this band keeps behaving as the
		// sequencer drawer's toggle, exactly as before it gained its second role.
		sequencerExpansionTarget = (sequencerExpansionTarget > 0.5f) ? 0.0f : 1.0f;
		sequencerExpansion = sequencerExpansionTarget;
		applySize();
		return;
	}
	if (!keyboardHandlePressed) return;
	keyboardHandlePressed = false;
	if (resizingEditorPane) {
		// The drag itself already applied every intermediate height live - this just makes
		// the final one stick across sessions, the same way WINDOW SIZE/THEME do.
		processor.setEditorPaneRefH(editorPaneRefH);
		resizingEditorPane = false;
		return;
	}
	// No real drag happened - an ordinary click, so this band keeps behaving as the
	// keyboard drawer's toggle, exactly as before this band gained its second role.
	keyboardExpansionTarget = (keyboardExpansionTarget > 0.5f) ? 0.0f : 1.0f;
	keyboardExpansion = keyboardExpansionTarget;
	applySize();
}

void D110AudioProcessorEditor::mouseMove(const juce::MouseEvent &e)
{
	const bool over = handleBand().contains(e.position);
	const bool overKeyboard = keyboardHandleBand().contains(e.position);
	const bool overSequencer = sequencerHandleBand().contains(e.position);
	const bool overSequencerResize = sequencerResizeBand().contains(e.position);
	bool changed = false;
	if (over != handleHover) { handleHover = over; changed = true; }
	if (overKeyboard != keyboardHandleHover) { keyboardHandleHover = overKeyboard; changed = true; }
	if (overSequencer != sequencerHandleHover) { sequencerHandleHover = overSequencer; changed = true; }
	if (overSequencerResize != sequencerResizeHover) { sequencerResizeHover = overSequencerResize; changed = true; }
	if (!changed) return;
	// Over the keyboard/sequencer bands specifically, hint at the resize (rather than the
	// plain pointing-hand the toggle-only band uses) only when there's actually something to
	// resize - i.e. the pane above it is open. The grip is always a resize cursor - it has
	// no toggle role to fall back to.
	juce::MouseCursor cursor = juce::MouseCursor::NormalCursor;
	if (overSequencerResize) cursor = juce::MouseCursor::UpDownResizeCursor;
	else if (overKeyboard && expansion > 0.5f) cursor = juce::MouseCursor::UpDownResizeCursor;
	else if (overSequencer && keyboardExpansion > 0.5f) cursor = juce::MouseCursor::UpDownResizeCursor;
	else if (over || overKeyboard || overSequencer) cursor = juce::MouseCursor::PointingHandCursor;
	setMouseCursor(cursor);
	repaint();
}

void D110AudioProcessorEditor::mouseExit(const juce::MouseEvent &)
{
	if (!handleHover && !keyboardHandleHover && !sequencerHandleHover && !sequencerResizeHover) return;
	handleHover = false;
	keyboardHandleHover = false;
	sequencerHandleHover = false;
	sequencerResizeHover = false;
	repaint();
}

// D110Keyboard::mouseDown() grabs keyboard focus on every click, deliberately, so playing a
// note there is always ready to type right after (see its own comment) - but that means
// clicking a piano key while using the retro sequencer's D-pad (EXIT/ENTER/arrows) leaves
// those physical keys silently doing nothing afterwards, since JUCE delivers key events to
// whichever component currently has focus, not to sequencerRetroPanel. Confirmed empirically,
// 2026-08-23 (Alan: EXIT stopped working "depuis n'importe quel endroit"). Rather than fight
// over who holds focus, catch it here: keyPressed() bubbles up the PARENT chain when the
// focused component doesn't consume it (D110Keyboard has no keyPressed() override at all,
// only keyStateChanged() - see its own header comment on why - so it always returns false and
// lets this run), and this editor is the nearest shared ancestor of both.
bool D110AudioProcessorEditor::keyPressed(const juce::KeyPress &key)
{
	return processor.getSequencerRetroMode() && sequencerRetroPanel.keyPressed(key);
}

void D110AudioProcessorEditor::resized()
{
	const float s = float(getWidth()) / float(D110Panel::currentRefW(processor.getCompactPanelMode()));
	panel.setBounds(0, 0, D110Panel::currentRefW(processor.getCompactPanelMode()), D110Panel::kRefH);
	panel.setTransform(juce::AffineTransform::scale(s));
	panel.setDisplayScale(s);

	// Ящик занимает всё, что ниже прибора и полосы-ручки его ручки, и ровно до начала
	// собственной полосы-ручки клавиатуры. Он рисуется целиком и обрезается собственными
	// границами - это и создаёт впечатление, что он выезжает из-под прибора.
	const int paneTop = int((float(D110Panel::kRefH) + kHandleRefH) * s + 0.5f);
	const int paneH = int(expansion * editorPaneRefH * s + 0.5f);
	editorPane.setBounds(0, paneTop, getWidth(), juce::jmax(0, paneH));

	// Клавиатура - тот же приём, второй раз подряд: своя полоса-ручка сразу под ящиком
	// (открытым или нет), сама - под ней, обрезанная собственными границами.
	const int kbTop = paneTop + paneH + int(kKeyboardHandleRefH * s + 0.5f);
	const int kbH = int(keyboardExpansion * keyboardPaneRefH * s + 0.5f);
	keyboard.setBounds(0, kbTop, getWidth(), juce::jmax(0, kbH));

	// Third time: sequencer's own handle band right below the keyboard, drawer under that.
	const int seqTop = kbTop + kbH + int(kSequencerHandleRefH * s + 0.5f);
	const int seqH = int(sequencerExpansion * sequencerPaneRefH * s + 0.5f);
	// Same bounds either way (D110SequencerRetroPanel::kRefH matches) - only the one
	// processor.getSequencerRetroMode() picked is actually visible, see the constructor
	// and panel.onSequencerModeChanged.
	sequencerPanel.setBounds(0, seqTop, getWidth(), juce::jmax(0, seqH));
	sequencerRetroPanel.setBounds(0, seqTop, getWidth(), juce::jmax(0, seqH));

	// Карта живёт в тех же опорных точках, что и панель, поэтому ей нужен только масштаб и
	// то, докуда сейчас доходит окно: по ним она сама поставит себе границы. Nothing to
	// position in compact mode - the slot itself is spliced out of the photo, and the card
	// stays hidden (see the constructor and onCompactPanelModeChanged).
	if (!processor.getCompactPanelMode()) card.setGeometry(s, totalRefHeight());
}
