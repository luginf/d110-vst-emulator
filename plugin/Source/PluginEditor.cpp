#include "PluginEditor.h"
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
constexpr int kCols = D110Core::kCols;
constexpr int kLines = D110Core::kLines;

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

// Renders the whole display - glass and ink dots - into an offscreen image at
// kLcdSuper times the panel's own resolution. Rebuilt only when the contents
// actually change, which is also the only time the panel repaints for the LCD's
// sake, so this costs nothing per frame.
void D110Panel::rebuildLcdImage()
{
	const int w = int(kLcdW) * kLcdSuper, h = int(kLcdH) * kLcdSuper;
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
	// ragged. Antialiasing at kLcdSuper and downscaling from there keeps every dot
	// the same size.
	auto px = [](float panelX) { return (panelX - kLcdX) * kLcdSuper; };
	auto py = [](float panelY) { return (panelY - kLcdY) * kLcdSuper; };
	// In offscreen pixels, so they scale with kLcdSuper - and they are deliberately a much
	// smaller FRACTION of a dot than they used to be. At the old ratio every stroke broke
	// into separate squares and the display read as dying; on the reference photograph the
	// dots of a stroke visibly run together, which is what these values reproduce.
	constexpr float kDotGapX = 2.0f, kDotGapY = 2.2f;

	g.setColour(kInk);
	for (int line = 0; line < kLines; ++line)
		for (int col = 0; col < kCols; ++col) {
			// One byte per dot row, straight out of the real MSM6222B: bit 4 is the
			// leftmost dot. The glyphs are therefore the genuine mask CGROM's, and the
			// cursor and its blink are the controller's own - nothing here interprets
			// character codes or consults a font.
			const juce::uint8 *rows = lcdRows + ((size_t)line * kCols + col) * D110Core::kRowsPerChar;
			for (int dy = 0; dy < kDotRows; ++dy)
				for (int dx = 0; dx < 5; ++dx) {
					if (!((rows[dy] >> (4 - dx)) & 1))
						continue;
					const float x0 = px(kCharX0 + col * kCellW + dx * kDotW);
					const float y0 = py(kLine0Y + line * kLineStep + dy * kDotH);
					g.fillRect(x0, y0, kDotW * kLcdSuper - kDotGapX, kDotH * kLcdSuper - kDotGapY);
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
		juce::uint8 rows[D110Core::kLcdBytes];
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
	if (!processor.getCore().isRunning()) return; // nothing to press while the unit is off
	const auto &b = kButtons[index];
	int bit = 0;
	while (bit < 7 && !((b.scanBit >> bit) & 1)) ++bit;
	processor.getCore().setButton(D110Core::buttonIndex(b.scanPort, bit), down);
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
	m.addItem(5, "Let the firmware voice the notes (part indicators, its own key ranges)",
	          true, processor.getForwardNotesToFirmware());
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
	m.addItem(107, "   POWER off, Ctrl+click WRITE/COPY, POWER on, then ENTER", false, false);

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
				fileChooser = std::make_unique<juce::FileChooser>(
					"Select a SysEx bank or MIDI file", juce::File(), "*.syx;*.mid;*.smf");
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
			case 5: {
				const bool turningOn = !processor.getForwardNotesToFirmware();
				processor.setForwardNotesToFirmware(turningOn);
				// Say what it costs before it costs it, rather than leaving the user to
				// discover a dead panel and assume the plugin has crashed.
				if (turningOn)
					juce::NativeMessageBox::showMessageBoxAsync(
						juce::MessageBoxIconType::WarningIcon, "D-110 Emulator",
						"The top row will now light the part being played.\n\n"
						"The cost: MAME emulates no LA32, so once notes reach the firmware "
						"it stops scanning the front panel within a few seconds. The sound "
						"keeps playing, but the buttons and display stop responding until "
						"you switch POWER off and on.");
				break;
			}
			case 3:
				if (superMode != nullptr) {
					superMode->beginChangeGesture();
					superMode->setValueNotifyingHost(superOn ? 0.0f : 1.0f);
					superMode->endChangeGesture();
				}
				break;
			default:
				break;
			}
		});
}

// ---------------------------------------------------------------------------

D110AudioProcessorEditor::D110AudioProcessorEditor(D110AudioProcessor &p)
	: juce::AudioProcessorEditor(&p), panel(p)
{
	addAndMakeVisible(panel);

	constrainer.setFixedAspectRatio(double(D110Panel::kRefW) / double(D110Panel::kRefH));
	constrainer.setSizeLimits(900, 108, D110Panel::kRefW, D110Panel::kRefH);
	setConstrainer(&constrainer);

	setResizable(true, true);
	setSize(1500, 181);
}

void D110AudioProcessorEditor::paint(juce::Graphics &g) { g.fillAll(juce::Colours::black); }

void D110AudioProcessorEditor::resized()
{
	const float s = float(getWidth()) / float(D110Panel::kRefW);
	panel.setBounds(0, 0, D110Panel::kRefW, D110Panel::kRefH);
	panel.setTransform(juce::AffineTransform::scale(s));
}
