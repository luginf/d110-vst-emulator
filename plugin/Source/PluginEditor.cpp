#include "PluginEditor.h"
#include "UiTheme.h"
#include <BinaryData.h>

#include <cmath>
#include <cstring>

namespace {

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
	g.drawImageAt(panelImage, 0, 0);

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
	const juce::Rectangle<float> face(b.x, b.y, b.w, b.h);
	paintPressedCap(g, capImages[size_t(index)], recessColours[size_t(index)], face,
	                pressedRect(face, depth, kPressShrink, kPressDrop), depth);
}

void D110Panel::paintPowerSwitch(juce::Graphics &g) const
{
	if (powerMotion.depth < 0.02f)
		return;

	const juce::Rectangle<float> face(kPowerX, kPowerY, kPowerW, kPowerH);
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

	juce::Graphics::ScopedSaveState ss(g);
	juce::Path clip;
	clip.addEllipse(kKnobCx - kKnobSpinR, kKnobCy - kKnobSpinR, kKnobSpinR * 2.0f, kKnobSpinR * 2.0f);
	g.reduceClipRegion(clip);

	// The cut-out was taken from the panel at this origin, so put it back exactly
	// there and spin about the true centre.
	const float ox = std::floor(kKnobCx - kKnobSpinR - 1.0f);
	const float oy = std::floor(kKnobCy - kKnobSpinR - 1.0f);
	g.drawImageTransformed(volumeDisc, juce::AffineTransform::translation(ox, oy)
	                                       .rotated(juce::degreesToRadians(deg), kKnobCx, kKnobCy));
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

	const juce::Rectangle<float> lens(kLampX, kLampY, kLampW, kLampH);
	g.setColour(juce::Colour(0xffe0472a));
	g.fillRect(lens);
	g.setColour(juce::Colour(0xffffc9b0).withAlpha(0.75f));
	g.fillRect(lens.reduced(1.0f, 1.0f));
}

void D110Panel::paintLcd(juce::Graphics &g) const
{
	// Blank the window first and unconditionally: the photograph was taken of a
	// unit with its own glass showing, and anything less than a guaranteed opaque
	// cover here lets that ghost through under the live render.
	g.setColour(processor.isPoweredOn() ? kGlassOn : kGlassOff);
	g.fillRect(kLcdX, kLcdY, kLcdW, kLcdH);

	if (lcdImage.isValid()) {
		// The display is the one part of this panel anybody reads, and the window is often
		// scaled well away from the artwork's own size, so it is worth resampling properly
		// rather than at the default quality.
		g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
		g.drawImage(lcdImage, int(kLcdX), int(kLcdY), int(kLcdW), int(kLcdH),
		            0, 0, lcdImage.getWidth(), lcdImage.getHeight(), false);
	}
}

int D110Panel::buttonAt(juce::Point<float> p) const
{
	if (juce::Rectangle<float>(kBezelX, kBezelY, kBezelW, kBezelH).contains(p))
		return kPowerIndex;

	for (int i = 0; i < kNumButtons; ++i)
		if (juce::Rectangle<float>(kButtons[i].x, kButtons[i].y, kButtons[i].w, kButtons[i].h).contains(p))
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
	if (juce::Rectangle<float>(kSlotHitX, kSlotHitY, kSlotHitW, kSlotHitH).contains(p)) {
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

	if (p.getDistanceFrom({ kKnobCx, kKnobCy }) <= kKnobHitR) {
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
	if (e.position.getDistanceFrom({ kKnobCx, kKnobCy }) <= kKnobHitR)
		processor.setMasterVolume(processor.getMasterVolume() + w.deltaY * 0.5f);
}

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
	// Пункта «пусть ноты озвучивает прошивка» здесь нет намеренно. Это не настройка, а
	// единственное поведение: ноты идут в прошивку, она применяет свои диапазоны клавиш,
	// раскладку по партиям и распределение голосов, зажигает индикаторы в верхней строке
	// и возвращает то, что действительно взяла. Выключение всего этого не давало ничего,
	// кроме менее точного инструмента, и было временной мерой на время, пока ноты роняли
	// панель, - в 0.9.6 это исправлено.
	m.addSeparator();

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

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
		[this, reverb, superMode, reverbOn, superOn, ins, outs](int result) {
			// The port lists are captured as they were when the menu opened, so an entry
			// always means the device the user actually saw and picked.
			if (result == 300) { processor.setMidiInputDevice({}); return; }
			if (result == 301) { processor.setMidiOutputDevice({}); return; }
			if (result >= 500 && result - 500 < outs.size()) {
				processor.setMidiOutputDevice(outs[result - 500].identifier);
				return;
			}
			if (result >= 400 && result - 400 < ins.size()) {
				processor.setMidiInputDevice(ins[result - 400].identifier);
				return;
			}
			switch (result) {
			case 1:
				// Explicit uppercase variants because file glob matching is case-sensitive on Linux
				// (unlike Windows/macOS); "*.*" is included to also offer an all-files fallback.
				fileChooser = std::make_unique<juce::FileChooser>(
					"Select a SysEx bank or MIDI file", juce::File(),
					"*.syx;*.SYX;*.mid;*.MID;*.smf;*.SMF;*.*");
				fileChooser->launchAsync(
					juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
					[this](const juce::FileChooser &fc) {
						const auto file = fc.getResult();
						if (file != juce::File())
							processor.importSysexBank(file);
					});
				break;
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

} // namespace

D110EditorPane::D110EditorPane(D110AudioProcessor &p) : processor(p) {
	setOpaque(true);
	ram.assign(D110CoreType::kRamSize, 0);

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
	case 8:  tab = Tab::Utility; break;
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
	auto area = getLocalBounds().toFloat().reduced(14.0f, 10.0f);
	if (area.getWidth() < 40.0f || area.getHeight() < 40.0f) return;

	const float tabH = juce::jlimit(20.0f, 28.0f, area.getHeight() * 0.06f);
	auto tabs = area.removeFromTop(tabH);
	const float tabW = juce::jmin(120.0f, tabs.getWidth() / float(kNumTabs) - 6.0f);
	for (int i = 0; i < kNumTabs; ++i) {
		tabBounds[(size_t)i] = tabs.removeFromLeft(tabW);
		tabs.removeFromLeft(6.0f);
	}
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

	// Подписи - СОБСТВЕННЫЕ ИМЕНА ПРИБОРА, слово в слово с ламинированной карточки «Tone
	// Parameters» (столбец Display): WG Pitch Cors, P-ENV T1, TVF Freq, TVA-ENV Sus L и так
	// далее. Раньше здесь стояли термины MT-32 - «cutoff» вместо «frequency» и прочее, - и
	// ящик говорил с человеком не теми словами, что панель прибора. Порядок смещений та же
	// карточка подтверждает один в один.
	static const ToneParam kWg[] = {
		{ "WG PITCH CORS",   0,  96 }, { "WG PITCH FINE",  1, 100 },
		{ "WG PITCH KF",     2,  16 }, { "WG BENDER SW",   3,   1 },
		{ "WG WAVEFORM",     4,   3 }, { "PCM",            5, 127 },
		{ "WG PULS WIDTH",   6, 100 }, { "WG PW VELO",     7,  14 },
		{ "P-ENV DEPTH",     8,  10 }, { "P-ENV VELO",     9, 100 },
		{ "P-ENV TIME KF",  10,   4 }, { "P-ENV T1",      11, 100 },
		{ "P-ENV T2",       12, 100 }, { "P-ENV T3",      13, 100 },
		{ "P-ENV T4",       14, 100 },
	};
	static const ToneParam kPitchEnv[] = {
		{ "P-ENV L0",       15, 100 }, { "P-ENV L1",      16, 100 },
		{ "P-ENV L2",       17, 100 }, { "P-ENV SUS L",   18, 100 },
		{ "P-ENV END L",    19, 100 }, { "P-LFO RATE",    20, 100 },
		{ "P-LFO DEPTH",    21, 100 }, { "P-LFO MOD",     22, 100 },
		{ "TVF FREQ",       23, 100 }, { "TVF RESO",      24,  30 },
		{ "TVF FREQ KF",    25,  14 }, { "TVF BIAS P",    26, 127 },
		{ "TVF BIAS LVL",   27,  14 }, { "TVF-ENV DEPT",  28, 100 },
		{ "TVF-ENV VELO",   29, 100 },
	};
	static const ToneParam kTvf[] = {
		{ "TVF-ENV DKF",    30,   4 }, { "TVF-ENV TKF",   31,   4 },
		{ "TVF-ENV T1",     32, 100 }, { "TVF-ENV T2",    33, 100 },
		{ "TVF-ENV T3",     34, 100 }, { "TVF-ENV T4",    35, 100 },
		{ "TVF-ENV T5",     36, 100 }, { "TVF-ENV L1",    37, 100 },
		{ "TVF-ENV L2",     38, 100 }, { "TVF-ENV L3",    39, 100 },
		{ "TVF-ENV SUS L",  40, 100 }, { "TVA LEVEL",     41, 100 },
		{ "TVA VELOCITY",   42, 100 }, { "TVA BIAS P1",   43, 127 },
		{ "TVA BIAS L1",    44,  12 },
	};
	static const ToneParam kTva[] = {
		{ "TVA BIAS P2",    45, 127 }, { "TVA BIAS L2",   46,  12 },
		{ "TVA-ENV TKF",    47,   4 }, { "TVA-ENV T1VF",  48,   4 },
		{ "TVA-ENV T1",     49, 100 }, { "TVA-ENV T2",    50, 100 },
		{ "TVA-ENV T3",     51, 100 }, { "TVA-ENV T4",    52, 100 },
		{ "TVA-ENV T5",     53, 100 }, { "TVA-ENV L1",    54, 100 },
		{ "TVA-ENV L2",     55, 100 }, { "TVA-ENV L3",    56, 100 },
		{ "TVA-ENV SUS L",  57, 100 },
	};

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

	// Верхняя половина - список патчей, нижняя - партии выбранного патча.
	auto listArea = area.removeFromTop(area.getHeight() * 0.58f);
	area.removeFromTop(8.0f);

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

	// --- партии выбранного патча -------------------------------------------------
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
		const int currentPercent = juce::roundToInt(float(getWidth()) / float(D110Panel::kRefW) * 100.0f);
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
} // namespace

// Shows docs/D20infos.png (embedded as BinaryData, see plugin/CMakeLists.txt's
// juce_add_binary_data(D110PanelData ...)) full-size in its own resizable pop-up window,
// scaled down only if it wouldn't otherwise fit the screen. Not modal - see ImagePopupWindow.
void D110EditorPane::showLaReferencePopup() {
	auto image = juce::ImageCache::getFromMemory(BinaryData::D20infos_png, BinaryData::D20infos_pngSize);
	if (image.isNull()) return;

	auto *imageComponent = new juce::ImageComponent();
	imageComponent->setImage(image);
	imageComponent->setImagePlacement(juce::RectanglePlacement::centred
	                                   | juce::RectanglePlacement::onlyReduceInSize);

	const auto *display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
	const auto screenArea =
		display != nullptr ? display->userBounds.toNearestInt() : juce::Rectangle<int>(0, 0, 1600, 900);
	const int w = juce::jmin(image.getWidth(), int(screenArea.getWidth() * 0.85f));
	const int h = juce::jmin(image.getHeight(), int(screenArea.getHeight() * 0.85f));
	imageComponent->setSize(w, h);

	new ImagePopupWindow("LA reference", imageComponent);
}

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
	default:             processor.sendTimbreTempParam(c.index, c.field, uint8_t(v)); break;
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
		case 5: return juce::String(v + 1);             // номер образца PCM
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

void D110EditorPane::paint(juce::Graphics &g) {
	g.fillAll(kEdBack());
	if (cells.empty() && buttons.empty() && tab != Tab::Monitor) layout();

	const float scale = juce::jlimit(0.75f, 1.4f, float(getWidth()) / 1500.0f);
	const juce::Font labelFont(juce::FontOptions(11.0f * scale, juce::Font::bold));
	const juce::Font valueFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
	                                             12.0f * scale, juce::Font::plain));

	const char *kTabs[] = { "PARTS", "TONE", "RHYTHM", "PATCHES", "TIMBRES", "TONES",
	                        "SYSTEM", "MONITOR", "UTILITY" };
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
		drawBox(g, c.bounds, int(i) == hovered || int(i) == dragging);
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
		         "PATCH / BANK / NUMBER buttons. Editing the parts below is audible at once "
		         "while it is the patch being played - the stored record and the live areas "
		         "are both written. Click a row's name to rename it.";
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
	const float scale = juce::jlimit(0.75f, 1.4f, float(getWidth()) / 1500.0f);
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
	if (id == 10) {
		static constexpr int kZoomPresets[] = { 50, 75, 100, 125, 150 };
		const int current = juce::roundToInt(float(getWidth()) / float(D110Panel::kRefW) * 100.0f);
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
		auto *chooser = new juce::FileChooser("Select a SysEx bank or MIDI file", juce::File(),
		                                      "*.syx;*.mid;*.smf");
		chooser->launchAsync(juce::FileBrowserComponent::openMode
		                         | juce::FileBrowserComponent::canSelectFiles,
		                     [this, chooser](const juce::FileChooser &fc) {
			                     const auto file = fc.getResult();
			                     if (file != juce::File()) processor.importSysexBank(file);
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
		auto *chooser = new juce::FileChooser("Export SysEx bank as", juce::File(), "*.syx");
		chooser->launchAsync(juce::FileBrowserComponent::saveMode
		                         | juce::FileBrowserComponent::canSelectFiles
		                         | juce::FileBrowserComponent::warnAboutOverwriting,
		                     [this, chooser](const juce::FileChooser &fc) {
			                     auto file = fc.getResult();
			                     if (file != juce::File()) {
				                     if (!file.hasFileExtension("syx")) file = file.withFileExtension("syx");
				                     processor.exportSysexBank(file);
			                     }
			                     delete chooser;
		                     });
		return;
	}
	if (id == 5) {
		auto *chooser = new juce::FileChooser("Save memory snapshot as", juce::File(), "*.d110snap");
		chooser->launchAsync(juce::FileBrowserComponent::saveMode
		                         | juce::FileBrowserComponent::canSelectFiles
		                         | juce::FileBrowserComponent::warnAboutOverwriting,
		                     [this, chooser](const juce::FileChooser &fc) {
			                     auto file = fc.getResult();
			                     if (file != juce::File()) {
				                     if (!file.hasFileExtension("d110snap"))
					                     file = file.withFileExtension("d110snap");
				                     processor.exportMemorySnapshot(file);
			                     }
			                     delete chooser;
		                     });
		return;
	}
	if (id == 6) {
		auto *chooser = new juce::FileChooser("Load memory snapshot", juce::File(), "*.d110snap");
		chooser->launchAsync(juce::FileBrowserComponent::openMode
		                         | juce::FileBrowserComponent::canSelectFiles,
		                     [this, chooser](const juce::FileChooser &fc) {
			                     const auto file = fc.getResult();
			                     if (file != juce::File()) processor.importMemorySnapshot(file);
			                     delete chooser;
		                     });
		return;
	}
	if (id == 8) { showLaReferencePopup(); return; }
	if (id == 9) { processor.midiPanic(); return; }
	if (id == 20) { processor.storeToneFromPart(part, toneSlot); return; }
	if (id == 21) { processor.auditionTone(part, toneSlot); return; }
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
		// Щелчок по номеру патча делает две вещи: показывает его партии ниже и просит
		// прибор на него перейти.
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
	menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
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

	menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
		[this, slot](int result) {
			if (result <= 0) return; // отменено
			processor.sendRhythmParam(slot, 0, uint8_t(result - 1));
			repaint();
		});
}

void D110EditorPane::mouseDown(const juce::MouseEvent &e) {
	const auto p = e.position;
	for (int i = 0; i < kNumTabs; ++i) {
		if (!tabBounds[(size_t)i].contains(p)) continue;
		selectTab(i);
		return;
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
		if (tab == Tab::Utility && zoomBounds.contains(p)) {
			juce::PopupMenu m;
			static constexpr int kZoomPresets[] = { 50, 75, 100, 125, 150 };
			const int current = juce::roundToInt(float(getWidth()) / float(D110Panel::kRefW) * 100.0f);
			for (int pct : kZoomPresets) m.addItem(pct, juce::String(pct) + "%", true, pct == current);
			m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int result) {
				if (result > 0 && onRequestZoom) onRequestZoom(result);
			});
			return;
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
	// него, и ниже показываются его партии. Ловится по всей строке, а не по одному номеру,
	// потому что перебирать патчи на слух надо мышью, а не прицеливаясь в кнопку.
	if (tab == Tab::Patches && tableArea.contains(p) && rowHeight > 0.0f) {
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
	dragging = cellAt(p);
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
		const int v = valueOf(cells[(size_t)i]);
		if (v < 0) return;
		setValue(cells[(size_t)i], v + (w.deltaY > 0 ? 1 : -1));
		return;
	}
	// Колесо мимо полей листает длинные списки: клавиш ударных восемьдесят пять, тембров сто
	// двадцать восемь, патчей шестьдесят четыре, а на экран помещается меньше.
	if (tab == Tab::Rhythm) rhythmScroll += (w.deltaY > 0 ? -3 : 3);
	else if (tab == Tab::Timbres) timbreScroll += (w.deltaY > 0 ? -3 : 3);
	else if (tab == Tab::Patches) patchScroll += (w.deltaY > 0 ? -3 : 3);
	else if (tab == Tab::Tones) toneScroll += (w.deltaY > 0 ? -3 : 3);
	else if (tab == Tab::Utility) utilityScrollOffset += (w.deltaY > 0 ? -40.0f : 40.0f);
	else return;
	layout();
	repaint();
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

namespace {
constexpr int kWhiteKeysPerOctave = 7;
constexpr int kWhiteSemitones[kWhiteKeysPerOctave] = { 0, 2, 4, 5, 7, 9, 11 };
// True for the white key immediately to the LEFT of a black key (C, D, F, G, A).
constexpr bool kHasBlackToRight[kWhiteKeysPerOctave] = { true, true, false, true, true, true, false };
} // namespace

// The classic two-row tracker layout (FastTracker2, Impulse Tracker, OpenMPT, Renoise: all
// the same table since the 90s). Lower row starts at the keyboard's current base octave,
// upper row one octave above that - the two overlap by an octave, same as every tracker.
// AZERTY's characters are what a French keyboard's PHYSICAL key at that same position
// actually sends: unshifted digits on AZERTY are punctuation (&é"'(-è_çà), not digits, so
// the accidentals' upper row differs there, and three letters move (A/Q, W/Z, M/;).
const std::vector<D110Keyboard::TrackerKey> &D110Keyboard::trackerKeys() {
	static const std::vector<TrackerKey> keys = {
		// lower row: Z S X D C V G B H N J M , L . ; /  (base octave, semitones 0..16)
		{ 0, 'z', 'w' }, { 1, 's', 's' }, { 2, 'x', 'x' }, { 3, 'd', 'd' },
		{ 4, 'c', 'c' }, { 5, 'v', 'v' }, { 6, 'g', 'g' }, { 7, 'b', 'b' },
		{ 8, 'h', 'h' }, { 9, 'n', 'n' }, { 10, 'j', 'j' }, { 11, 'm', ',' },
		{ 12, ',', ';' }, { 13, 'l', 'l' }, { 14, '.', ':' }, { 15, ';', 'm' }, { 16, '/', '!' },
		// upper row: Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P  (one octave up, semitones 12..28)
		{ 12, 'q', 'a' }, { 13, '2', (juce::juce_wchar)0x00E9 /* é */ }, { 14, 'w', 'z' },
		{ 15, '3', '"' }, { 16, 'e', 'e' }, { 17, 'r', 'r' }, { 18, '5', '(' },
		{ 19, 't', 't' }, { 20, '6', '-' }, { 21, 'y', 'y' },
		{ 22, '7', (juce::juce_wchar)0x00E8 /* è */ }, { 23, 'u', 'u' }, { 24, 'i', 'i' },
		{ 25, '9', (juce::juce_wchar)0x00E7 /* ç */ }, { 26, 'o', 'o' },
		{ 27, '0', (juce::juce_wchar)0x00E0 /* à */ }, { 28, 'p', 'p' },
	};
	return keys;
}

D110Keyboard::D110Keyboard(D110AudioProcessor &p) : processor(p) {
	// Restores whatever config the project/session was last saved with - see
	// D110AudioProcessor::getKeyboardMidiChannel() and friends for why the processor, not this
	// component, is the actual source of truth.
	midiChannel = processor.getKeyboardMidiChannel();
	omni = processor.getKeyboardOmni();
	pcKeyboardEnabled = processor.getKeyboardPcInputEnabled();
	pcLayout = processor.getKeyboardPcLayout() == 1 ? PcLayout::azerty : PcLayout::qwerty;

	pcKeyDown.assign(trackerKeys().size(), false);
	setWantsKeyboardFocus(true);
	rebuildKeys();
}

D110Keyboard::~D110Keyboard() { setHeldNote(-1); releaseAllPcNotes(); }

void D110Keyboard::sendNote(int note, float velocity, bool on) {
	if (omni) {
		for (int ch = 1; ch <= 16; ++ch) processor.injectTestNote(ch, note, velocity, on);
	} else {
		processor.injectTestNote(midiChannel, note, velocity, on);
	}
}

void D110Keyboard::releaseAllPcNotes() {
	for (size_t i = 0; i < pcKeyDown.size(); ++i) {
		if (!pcKeyDown[i]) continue;
		pcKeyDown[i] = false;
		sendNote(kLowestNote + octaveShift * 12 + trackerKeys()[i].semitoneFromBase, 0.0f, false);
	}
}

void D110Keyboard::showContextMenu() {
	juce::PopupMenu channelMenu;
	for (int ch = 1; ch <= 16; ++ch)
		channelMenu.addItem(1000 + ch, "Channel " + juce::String(ch), true, !omni && midiChannel == ch);

	juce::PopupMenu layoutMenu;
	layoutMenu.addItem(2001, "QWERTY", true, pcLayout == PcLayout::qwerty);
	layoutMenu.addItem(2002, "AZERTY", true, pcLayout == PcLayout::azerty);

	juce::PopupMenu m;
	m.addSubMenu("MIDI Channel", channelMenu, !omni);
	m.addItem(3000, "Omni (all 16 channels at once)", true, omni);
	m.addSeparator();
	m.addItem(4000, "PC keyboard input (tracker-style)", true, pcKeyboardEnabled);
	m.addSubMenu("PC keyboard layout", layoutMenu, pcKeyboardEnabled);

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int result) {
		if (result >= 1001 && result <= 1016) {
			midiChannel = result - 1000;
			processor.setKeyboardMidiChannel(midiChannel);
			return;
		}
		if (result == 3000) {
			omni = !omni;
			processor.setKeyboardOmni(omni);
			return;
		}
		if (result == 4000) {
			pcKeyboardEnabled = !pcKeyboardEnabled;
			processor.setKeyboardPcInputEnabled(pcKeyboardEnabled);
			if (pcKeyboardEnabled) grabKeyboardFocus();
			else releaseAllPcNotes();
			return;
		}
		if (result == 2001) {
			pcLayout = PcLayout::qwerty;
			processor.setKeyboardPcLayout(0);
			return;
		}
		if (result == 2002) {
			pcLayout = PcLayout::azerty;
			processor.setKeyboardPcLayout(1);
			return;
		}
	});
}

bool D110Keyboard::keyStateChanged(bool /*isKeyDown*/) {
	if (!pcKeyboardEnabled) return false;
	const auto &keys = trackerKeys();
	bool used = false;
	for (size_t i = 0; i < keys.size(); ++i) {
		const juce::juce_wchar c = pcLayout == PcLayout::qwerty ? keys[i].qwerty : keys[i].azerty;
		const bool down = juce::KeyPress::isKeyCurrentlyDown((int)c);
		if (down == pcKeyDown[i]) continue;
		pcKeyDown[i] = down;
		sendNote(kLowestNote + octaveShift * 12 + keys[i].semitoneFromBase, 0.85f, down);
		used = true;
	}
	return used;
}

void D110Keyboard::focusLost(juce::Component::FocusChangeType) { releaseAllPcNotes(); }

void D110Keyboard::changeOctave(int delta) {
	const int shifted = juce::jlimit(-2, 3, octaveShift + delta);
	if (shifted == octaveShift) return;
	if (heldNote >= 0) setHeldNote(-1);
	releaseAllPcNotes(); // held notes are note NUMBERS already sent - shifting octave first
	                     // would leave them stuck on, since the physical key never "changed"
	octaveShift = shifted;
	rebuildKeys();
	repaint();
}

// Geometry only - no drawing, no note math beyond the note NUMBERS each key represents.
// Called on resize and on octave change, both of which invalidate every rectangle.
void D110Keyboard::rebuildKeys() {
	whiteKeys.clear();
	blackKeys.clear();

	auto area = getLocalBounds().toFloat();
	// A thin caption strip above everything, full width, so "OCT n" never has to sit on top
	// of a black key to be legible.
	captionBounds = area.removeFromTop(juce::jmin(16.0f, area.getHeight() * 0.16f));
	const float buttonW = juce::jmin(36.0f, area.getWidth() * 0.06f);
	octaveDownBounds = area.removeFromLeft(buttonW);
	octaveUpBounds = area.removeFromRight(buttonW);
	keysBounds = area;

	if (keysBounds.getWidth() < 1.0f || keysBounds.getHeight() < 1.0f) return;

	constexpr int kNumWhite = kOctaves * kWhiteKeysPerOctave + 1; // trailing C
	const float whiteW = keysBounds.getWidth() / float(kNumWhite);
	const float blackW = whiteW * 0.62f;
	const float blackH = keysBounds.getHeight() * 0.6f;

	for (int i = 0; i < kNumWhite; ++i) {
		const int octaveIndex = i / kWhiteKeysPerOctave;
		const int local = i % kWhiteKeysPerOctave;
		const int note = kLowestNote + (octaveShift + octaveIndex) * 12 + kWhiteSemitones[local];
		const float x = keysBounds.getX() + i * whiteW;
		whiteKeys.push_back({ { x, keysBounds.getY(), whiteW, keysBounds.getHeight() }, note, false });

		// i < kNumWhite - 1 excludes the trailing C: it's the terminal key of the range, with
		// no white key after it to sit between - without this guard its "black key to the
		// right" landed outside keysBounds altogether, on top of the OCT+ button.
		if (i < kNumWhite - 1 && kHasBlackToRight[local]) {
			const float bx = x + whiteW - blackW * 0.5f;
			blackKeys.push_back({ { bx, keysBounds.getY(), blackW, blackH }, note + 1, true });
		}
	}
}

int D110Keyboard::keyAt(juce::Point<float> p) const {
	for (const auto &k : blackKeys)
		if (k.bounds.contains(p)) return k.note;
	for (const auto &k : whiteKeys)
		if (k.bounds.contains(p)) return k.note;
	return -1;
}

void D110Keyboard::setHeldNote(int note) {
	if (note == heldNote) return;
	if (heldNote >= 0) sendNote(heldNote, 0.0f, false);
	heldNote = note;
	if (heldNote >= 0) sendNote(heldNote, 0.85f, true);
	repaint();
}

void D110Keyboard::paint(juce::Graphics &g) {
	const auto &pal = d110ui::palette();
	g.fillAll(pal.panelBg);

	auto paintButton = [&](juce::Rectangle<float> b, const char *label) {
		g.setColour(pal.keyButtonFill);
		g.fillRect(b.reduced(3.0f));
		g.setColour(pal.keyButtonText);
		g.setFont(juce::FontOptions(juce::jlimit(12.0f, 22.0f, b.getWidth() * 0.5f)));
		g.drawText(label, b, juce::Justification::centred);
	};
	// Single glyphs, not "OCT-"/"OCT+": the button is often not much wider than one
	// character once the drawer is squeezed down to the constrainer's minimum width.
	paintButton(octaveDownBounds, "-");
	paintButton(octaveUpBounds, "+");

	for (const auto &k : whiteKeys) {
		g.setColour(k.note == heldNote ? pal.keyWhiteHeld : pal.keyWhite);
		g.fillRect(k.bounds.reduced(1.0f, 0.0f));
		g.setColour(pal.keyWhiteBorder);
		g.drawRect(k.bounds, 1.0f);
	}
	for (const auto &k : blackKeys) {
		g.setColour(k.note == heldNote ? pal.keyBlackHeld : pal.keyBlack);
		g.fillRect(k.bounds);
	}

	// Always shown, not just when shifted: it's the only hint of what -/+ actually do.
	g.setColour(pal.keyCaption);
	g.setFont(juce::FontOptions(11.0f));
	g.drawText("OCT " + (octaveShift > 0 ? juce::String("+") + juce::String(octaveShift)
	                                     : juce::String(octaveShift)),
	           captionBounds, juce::Justification::centred);
}

void D110Keyboard::resized() { rebuildKeys(); }

void D110Keyboard::mouseDown(const juce::MouseEvent &e) {
	// Grabbed on every click, not only when PC input is on: it costs nothing while off, and
	// it means turning PC input on from the menu (itself a click on this component) leaves
	// typing ready to go immediately, with no extra click needed to focus the strip.
	grabKeyboardFocus();

	if (e.mods.isPopupMenu()) { showContextMenu(); return; }
	if (octaveDownBounds.contains(e.position)) { changeOctave(-1); return; }
	if (octaveUpBounds.contains(e.position)) { changeOctave(1); return; }
	const int note = keyAt(e.position);
	if (note < 0) return;
	draggingKey = true;
	setHeldNote(note);
}

void D110Keyboard::mouseDrag(const juce::MouseEvent &e) {
	if (!draggingKey) return;
	const int note = keyAt(e.position);
	setHeldNote(note); // -1 when the mouse drags off every key, which releases cleanly
}

void D110Keyboard::mouseUp(const juce::MouseEvent &) {
	if (!draggingKey) return;
	draggingKey = false;
	setHeldNote(-1);
}

void D110Keyboard::mouseExit(const juce::MouseEvent &) {
	if (!draggingKey) return;
	draggingKey = false;
	setHeldNote(-1);
}

// ---------------------------------------------------------------------------

D110AudioProcessorEditor::D110AudioProcessorEditor(D110AudioProcessor &p)
	: juce::AudioProcessorEditor(&p), processor(p), panel(p), editorPane(p), card(p), keyboard(p),
	  sequencerPanel(p)
{
	// Synced here, not just read lazily by whichever drawer paints first: a project loaded
	// with the light theme should look right the moment this editor appears, including on
	// the very first paint.
	d110ui::setTheme(p.getUiThemeLight() ? d110ui::Theme::Light : d110ui::Theme::Dark);
	// Same idea for the editor pane's own height - read before totalRefHeight() is first
	// called just below, so a project saved with a resized drawer opens already that size.
	editorPaneRefH = juce::jlimit(kMinPaneRefH, kMaxPaneRefH, p.getEditorPaneRefH());

	addAndMakeVisible(panel);
	addAndMakeVisible(editorPane);
	addAndMakeVisible(keyboard);
	addAndMakeVisible(sequencerPanel);
	// Карта добавляется последней и потому лежит поверх обоих - и прибора, и ящика.
	addAndMakeVisible(card);

	panel.onCardSlotClicked = [this] { card.toggle(); };
	card.onEjectNeedsDrawer = [this] {
		expansion = expansionTarget = 1.0f;
		applySize();
	};

	// Ящик закрыт при открытии окна: плагин - это прибор, а редактор к нему добавлен, и до
	// тех пор, пока его не попросили, он не занимает места.
	constrainer.setFixedAspectRatio(double(D110Panel::kRefW) / double(totalRefHeight()));
	constrainer.setSizeLimits(900, 100, D110Panel::kRefW * 2, 4000);
	setConstrainer(&constrainer);

	setResizable(true, true);
	setSize(1500, int(totalRefHeight() * (1500.0f / float(D110Panel::kRefW)) + 0.5f));

	// Zoom presets (Utility tab) call this to resize precisely, the same way a manual
	// drag-resize already does reliably - see D110EditorPane::onRequestZoom's own comment for
	// why this exists instead of a maximise button.
	editorPane.onRequestZoom = [this](int percent) {
		const int targetW = juce::roundToInt(float(D110Panel::kRefW) * float(percent) / 100.0f);
		setSize(targetW, int(totalRefHeight() * (float(targetW) / float(D110Panel::kRefW)) + 0.5f));
	};

	// The THEME toggle flips a process-wide palette (UiTheme.h) - every drawer needs a
	// fresh paint to pick it up, not just the Utility tab that owns the button.
	editorPane.onThemeChanged = [this] {
		editorPane.repaint();
		keyboard.repaint();
		sequencerPanel.repaint();
		repaint();
	};
}

float D110AudioProcessorEditor::totalRefHeight() const {
	return float(D110Panel::kRefH) + kHandleRefH + expansion * editorPaneRefH
	     + kKeyboardHandleRefH + keyboardExpansion * D110Keyboard::kRefH
	     + kSequencerHandleRefH + sequencerExpansion * D110SequencerPanel::kRefH;
}

void D110AudioProcessorEditor::applySize() {
	constrainer.setFixedAspectRatio(double(D110Panel::kRefW) / double(totalRefHeight()));
	const float s = float(getWidth()) / float(D110Panel::kRefW);
	setSize(getWidth(), int(totalRefHeight() * s + 0.5f));
}

// Полоса-ручка: во всю ширину, сразу под фотографией. Полная ширина затем, чтобы она
// читалась ящиком, который выдвигают, а не кнопкой.
juce::Rectangle<float> D110AudioProcessorEditor::handleBand() const {
	const float s = float(getWidth()) / float(D110Panel::kRefW);
	return { 0.0f, float(D110Panel::kRefH) * s, float(getWidth()), kHandleRefH * s };
}

// Its own handle band, stacked below wherever the editor's own drawer currently ends - so
// it follows that drawer up and down as it opens and closes, exactly as the drawer's own
// band follows the panel.
juce::Rectangle<float> D110AudioProcessorEditor::keyboardHandleBand() const {
	const float s = float(getWidth()) / float(D110Panel::kRefW);
	const float top = (float(D110Panel::kRefH) + kHandleRefH + expansion * editorPaneRefH) * s;
	return { 0.0f, top, float(getWidth()), kKeyboardHandleRefH * s };
}

// Same trick a third time: stacked below wherever the keyboard drawer currently ends.
juce::Rectangle<float> D110AudioProcessorEditor::sequencerHandleBand() const {
	const float s = float(getWidth()) / float(D110Panel::kRefW);
	const float top = (float(D110Panel::kRefH) + kHandleRefH + expansion * editorPaneRefH
	                  + kKeyboardHandleRefH + keyboardExpansion * D110Keyboard::kRefH) * s;
	return { 0.0f, top, float(getWidth()), kSequencerHandleRefH * s };
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
		sequencerExpansionTarget = (sequencerExpansionTarget > 0.5f) ? 0.0f : 1.0f;
		sequencerExpansion = sequencerExpansionTarget;
		applySize();
		return;
	}
}

void D110AudioProcessorEditor::mouseDrag(const juce::MouseEvent &e)
{
	if (!keyboardHandlePressed) return;
	const float deltaY = e.position.y - resizeDragStartY;
	// A few pixels of slop before a press-and-hold turns into a resize, so an ordinary click
	// (which always jitters slightly between down and up) doesn't accidentally nudge the
	// height - only actually resizes once the editor pane is open, since dragging this band
	// with nothing above it to resize wouldn't do anything visible.
	if (!resizingEditorPane) {
		if (expansion < 0.5f || std::abs(deltaY) < 4.0f) return;
		resizingEditorPane = true;
	}
	const float s = float(getWidth()) / float(D110Panel::kRefW);
	editorPaneRefH = juce::jlimit(kMinPaneRefH, kMaxPaneRefH, resizeDragStartRefH + deltaY / s);
	applySize();
}

void D110AudioProcessorEditor::mouseUp(const juce::MouseEvent &)
{
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
	bool changed = false;
	if (over != handleHover) { handleHover = over; changed = true; }
	if (overKeyboard != keyboardHandleHover) { keyboardHandleHover = overKeyboard; changed = true; }
	if (overSequencer != sequencerHandleHover) { sequencerHandleHover = overSequencer; changed = true; }
	if (!changed) return;
	// Over the keyboard band specifically, hint at the resize (rather than the plain
	// pointing-hand the other two bands use) only when there's actually something to
	// resize - i.e. the editor pane above it is open.
	juce::MouseCursor cursor = juce::MouseCursor::NormalCursor;
	if (overKeyboard && expansion > 0.5f) cursor = juce::MouseCursor::UpDownResizeCursor;
	else if (over || overKeyboard || overSequencer) cursor = juce::MouseCursor::PointingHandCursor;
	setMouseCursor(cursor);
	repaint();
}

void D110AudioProcessorEditor::mouseExit(const juce::MouseEvent &)
{
	if (!handleHover && !keyboardHandleHover && !sequencerHandleHover) return;
	handleHover = false;
	keyboardHandleHover = false;
	sequencerHandleHover = false;
	repaint();
}

void D110AudioProcessorEditor::resized()
{
	const float s = float(getWidth()) / float(D110Panel::kRefW);
	panel.setBounds(0, 0, D110Panel::kRefW, D110Panel::kRefH);
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
	const int kbH = int(keyboardExpansion * D110Keyboard::kRefH * s + 0.5f);
	keyboard.setBounds(0, kbTop, getWidth(), juce::jmax(0, kbH));

	// Third time: sequencer's own handle band right below the keyboard, drawer under that.
	const int seqTop = kbTop + kbH + int(kSequencerHandleRefH * s + 0.5f);
	const int seqH = int(sequencerExpansion * D110SequencerPanel::kRefH * s + 0.5f);
	sequencerPanel.setBounds(0, seqTop, getWidth(), juce::jmax(0, seqH));

	// Карта живёт в тех же опорных точках, что и панель, поэтому ей нужен только масштаб и
	// то, докуда сейчас доходит окно: по ним она сама поставит себе границы.
	card.setGeometry(s, totalRefHeight());
}
