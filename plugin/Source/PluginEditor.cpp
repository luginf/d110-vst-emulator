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

// Цвета взяты с самого прибора, чтобы ящик читался его продолжением, а не чужой панелью:
// подписи - синие, как шелкография Roland на передней панели, значения - зелёные, как
// индикатор D-110, который у него не янтарный, а с зелёной подсветкой (docs/lcd_reference.png).
const juce::Colour kEdBack(0xff141416);
const juce::Colour kEdBox(0xff1d1d20);
const juce::Colour kEdBorder(0xff34343a);
const juce::Colour kEdLabel(0xff6fa8dc);
const juce::Colour kEdValue(0xff8ede4a);
const juce::Colour kEdDim(0xff787880);

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
	g.setColour(kEdBox);
	g.fillRoundedRectangle(r, 3.0f);
	g.setColour(highlight ? kEdValue.withAlpha(0.55f) : kEdBorder);
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
	ram.assign(D110Core::kRamSize, 0);

	// Поле ввода одно на весь редактор: любое имя набирается им же, просто в разных местах.
	// Прибор принимает только печатные ASCII, поэтому набрать что-то другое здесь нельзя -
	// это ограничение прибора, а не удобства.
	addChildComponent(textEntry);
	textEntry.setMultiLine(false);
	textEntry.setReturnKeyStartsNewLine(false);
	textEntry.setInputRestrictions(20, " !\"#$%&'()*+,-./0123456789:;<=>?@"
	                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
	                                   "abcdefghijklmnopqrstuvwxyz{|}~");
	textEntry.setColour(juce::TextEditor::backgroundColourId, kEdBox);
	textEntry.setColour(juce::TextEditor::textColourId, kEdValue);
	textEntry.setColour(juce::TextEditor::outlineColourId, kEdValue.withAlpha(0.6f));
	textEntry.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f,
	                                    juce::Font::plain));
	textEntry.onReturnKey = [this] {
		switch (textEntryTarget) {
		case 1: processor.sendName(D110Core::kSysexToneTemp, part * D110Core::kToneRecord,
		                           textEntry.getText()); break;
		case 2: processor.sendDisplayMessage(textEntry.getText()); break;
		case 3: processor.sendName(D110Core::kSysexPatches, patchSlot * D110Core::kPatchRecord,
		                           textEntry.getText()); break;
		case 4: processor.sendName(D110Core::kSysexTones, toneSlot * D110Core::kToneMemRecord,
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
		if (processor.getCore().getRam(ram.data())) { ramGen = gen; ramValid = true; }
		repaint();
		return;
	}
	if (ramValid && gen == ramGen) return;
	if (!processor.getCore().getRam(ram.data())) return;
	ramGen = gen;
	ramValid = true;
	repaint();
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
	case Tab::Utility: layoutUtility(area); break;
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
	struct Col { const char *head; int field; int hi; float frac; };
	static const Col kCols[] = {
		{ "PART",       -1,   0, 0.000f },
		{ "TONE GROUP",  0,   3, 0.045f },
		{ "TONE",        1,  63, 0.150f },
		{ "LEVEL",       8, 100, 0.330f },
		{ "PAN",         9,  14, 0.395f },
		{ "KEY SHIFT",   2,  48, 0.455f },
		{ "FINE TUNE",   3, 100, 0.545f },
		{ "BENDER",      4,  24, 0.635f },
		{ "ASSIGN",      5,   3, 0.705f },
		{ "OUTPUT",      6,   7, 0.785f },
		{ "KEY LOW",    10, 127, 0.860f },
		{ "KEY HIGH",   11, 127, 0.930f },
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
	rhythmScroll = juce::jlimit(0, juce::jmax(0, D110Core::kNumRhythmKeys - visible), rhythmScroll);

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
		if (slot >= D110Core::kNumRhythmKeys) break;
		auto row = area.removeFromTop(rowHeight).reduced(0.0f, 2.0f);
		const int key = D110Core::kRhythmFirstKey + slot;
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
	patchScroll = juce::jlimit(0, juce::jmax(0, D110Core::kNumPatches - rows), patchScroll);

	for (int i = 0; i < rows; ++i) {
		const int patch = patchScroll + i;
		if (patch >= D110Core::kNumPatches) break;
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
	const int chosen = juce::jlimit(0, D110Core::kNumPatches - 1, patchSlot);
	labels.push_back({ area.removeFromTop(15.0f),
	                   "PARTS OF PATCH I-" + juce::String(chosen / 8 + 1)
	                       + juce::String(chosen % 8 + 1)
	                       + "   - what it puts into the parts when it is selected",
	                   true });

	struct PCol { const char *head; int field; int hi; float frac; };
	static const PCol kPCols[] = {
		{ "PART",       -1,   0, 0.000f },
		{ "TONE GROUP",  0,   3, 0.045f },
		{ "TONE",        1,  63, 0.150f },
		{ "LEVEL",       8, 100, 0.330f },
		{ "PAN",         9,  14, 0.395f },
		{ "KEY SHIFT",   2,  48, 0.455f },
		{ "FINE TUNE",   3, 100, 0.545f },
		{ "BENDER",      4,  24, 0.635f },
		{ "ASSIGN",      5,   3, 0.705f },
		{ "OUTPUT",      6,   7, 0.785f },
		{ "KEY LOW",    10, 127, 0.860f },
		{ "KEY HIGH",   11, 127, 0.930f },
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
		for (int i = 1; i < kNumPCols; ++i)
			// Запись партии - те же двенадцать байт, что и в Timbre Temporary, начиная с
			// 31-го байта патча: имя 10, ревербератор 3, резерв 9, каналы 9. Измерено
			// сличением с заводским содержимым - панорама этих восьми записей совпала с
			// заводским веером 4 10 6 8 2 12 0 14 байт в байт.
			cells.push_back({ pcolAt(i, row), Area::Patches, chosen,
			                  31 + p * 12 + kPCols[i].field, 0, kPCols[i].hi });
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
	timbreScroll = juce::jlimit(0, juce::jmax(0, D110Core::kNumTimbres - rows), timbreScroll);

	for (int i = 0; i < rows; ++i) {
		const int slot = timbreScroll + i;
		if (slot >= D110Core::kNumTimbres) break;
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
	toneScroll = juce::jlimit(0, juce::jmax(0, D110Core::kNumTones - rows * 3), toneScroll);
	const float colW = w / 3.0f;
	for (int col = 0; col < 3; ++col)
		for (int r = 0; r < rows; ++r) {
			const int slot = toneScroll + col * rows + r;
			if (slot >= D110Core::kNumTones) break;
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
	const float w = area.getWidth();

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
	area.removeFromTop(18.0f);

	labels.push_back({ area.removeFromTop(15.0f), "FACTORY INITIALISATION", true });
	labels.push_back({ area.removeFromTop(18.0f),
	                   "The instrument has its own, and this plugin can perform it exactly: "
	                   "POWER off, Ctrl+click WRITE/COPY to latch the cap down, POWER on, then "
	                   "ENTER. There is no button for it here, because there is none on the "
	                   "hardware either.", false });
	area.removeFromTop(12.0f);

	labels.push_back({ area.removeFromTop(15.0f), "WHERE THE EDITS GO", true });
	labels.push_back({ area.removeFromTop(18.0f),
	                   "Every field in this drawer sends the instrument a Roland exclusive "
	                   "message - the firmware changes its own memory, and the sound engine is "
	                   "brought into line from there. Nothing here writes to the sound engine "
	                   "behind the instrument's back.", false });
}

// --- значения ---------------------------------------------------------------

size_t D110EditorPane::addressOf(const Cell &c) const {
	switch (c.area) {
	case Area::ToneTemp:
		return size_t(D110Core::kRamToneTemp) + size_t(c.index) * D110Core::kToneRecord
		     + size_t(c.field);
	case Area::Rhythm:
		return size_t(D110Core::kRamRhythmTemp) + size_t(c.index) * D110Core::kRhythmRecord
		     + size_t(c.field);
	case Area::System:
		return size_t(D110Core::kRamSystem) + size_t(c.index);
	case Area::Timbres:
		return size_t(D110Core::kRamTimbres) + size_t(c.index) * D110Core::kTimbreRecord
		     + size_t(c.field);
	case Area::Patches:
		return size_t(D110Core::kRamPatches) + size_t(c.index) * D110Core::kPatchRecord
		     + size_t(c.field);
	case Area::Tones:
		return size_t(D110Core::kRamTones) + size_t(c.index) * D110Core::kToneMemRecord
		     + size_t(c.field);
	default:
		return size_t(D110Core::kRamTimbreTemp) + size_t(c.index) * D110Core::kTimbreTempRecord
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
	repaint();
}

juce::String D110EditorPane::nameAt(size_t ramOffset) const {
	if (!ramValid) return {};
	juce::String name;
	for (int i = 0; i < D110Core::kNameChars && ramOffset + size_t(i) < ram.size(); ++i) {
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
		return nameAt(size_t(D110Core::kRamTones) + size_t(number) * D110Core::kToneMemRecord);

	const size_t slot = size_t(group) * 64 + size_t(number);
	if (romToneNameKnown[slot]) return romToneNames[slot];
	if (!processor.engineIsOpen()) return {};

	// Адрес в упакованном виде, как его ждёт readEngineMemory: SysEx 08 00 00 - это 0x020000,
	// а запись банка тембров у движка занимает 256 байт.
	uint8_t buf[D110Core::kNameChars] = {};
	if (!processor.readEngineMemory(juce::uint32(0x020000 + slot * 256), D110Core::kNameChars,
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
	g.fillAll(kEdBack);
	if (cells.empty() && buttons.empty() && tab != Tab::Monitor) layout();

	const float scale = juce::jlimit(0.75f, 1.4f, float(getWidth()) / 1500.0f);
	const juce::Font labelFont(juce::FontOptions(11.0f * scale, juce::Font::bold));
	const juce::Font valueFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
	                                             12.0f * scale, juce::Font::plain));

	const char *kTabs[] = { "PARTS", "TONE", "RHYTHM", "PATCHES", "TIMBRES", "TONES",
	                        "SYSTEM", "MONITOR", "UTILITY" };
	for (int i = 0; i < kNumTabs; ++i) {
		const bool active = (int(tab) == i);
		g.setColour(active ? kEdBox.brighter(0.25f) : kEdBox);
		g.fillRoundedRectangle(tabBounds[(size_t)i], 3.0f);
		g.setColour(active ? kEdValue : kEdBorder);
		g.drawRoundedRectangle(tabBounds[(size_t)i].reduced(0.5f), 3.0f, 1.0f);
		g.setColour(active ? kEdValue : kEdLabel);
		g.setFont(labelFont);
		g.drawText(kTabs[i], tabBounds[(size_t)i], juce::Justification::centred);
	}

	if (!ramValid) {
		g.setColour(kEdDim);
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
			g.setColour(active ? kEdBox.brighter(0.3f) : kEdBox);
			g.fillRoundedRectangle(partBounds[(size_t)p], 3.0f);
			g.setColour(active ? kEdValue : kEdBorder);
			g.drawRoundedRectangle(partBounds[(size_t)p].reduced(0.5f), 3.0f, 1.0f);
			g.setColour(active ? kEdValue : kEdDim);
			g.setFont(labelFont);
			g.drawText(juce::String(p + 1), partBounds[(size_t)p], juce::Justification::centred);
		}
	}
	if (tab == Tab::Tone && !textEntry.isVisible()) {
		g.setColour(kEdValue);
		g.setFont(valueFont);
		g.drawText(nameAt(size_t(D110Core::kRamToneTemp) + size_t(part) * D110Core::kToneRecord),
		           toneNameBounds, juce::Justification::centredLeft);
	}

	g.setFont(labelFont);
	for (const Label &l : labels) {
		g.setColour(l.heading ? kEdLabel : kEdDim);
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
			const juce::String name = nameAt(size_t(D110Core::kRamTones)
			                                 + size_t(slot) * D110Core::kToneMemRecord);
			g.setColour(chosen ? kEdValue : kEdDim);
			g.setFont(valueFont);
			g.drawText(juce::String(slot + 1).paddedLeft(' ', 2) + "  "
			               + (name.isEmpty() ? juce::String("- - -") : name),
			           b.bounds.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
			continue;
		}
		if (b.id >= 400 && b.id < 500) {                    // имя патча
			const int patch = b.id - 400;
			drawBox(g, b.bounds, false);
			g.setColour(kEdValue);
			g.setFont(valueFont);
			g.drawText(nameAt(size_t(D110Core::kRamPatches)
			                  + size_t(patch) * D110Core::kPatchRecord),
			           b.bounds.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
			continue;
		}
		// Патч, который прибор играет сейчас, отмечен - иначе список из 64 одинаковых
		// кнопок не говорит, где ты находишься.
		const bool current = (b.id >= 200 && b.id < 300)
		                   && ramValid
		                   && int(ram[(size_t)D110Core::kRamPatchNumber]) == b.id - 200;
		const bool chosenPatch = (b.id >= 200 && b.id < 300) && (b.id - 200 == patchSlot);
		drawBox(g, b.bounds, current || chosenPatch);
		g.setColour(current ? kEdValue : (b.text.isEmpty() ? kEdDim : kEdLabel));
		g.setFont(labelFont);
		g.drawText(b.text.isEmpty() ? juce::String("click to type") : b.text, b.bounds,
		           juce::Justification::centred);
	}

	g.setFont(valueFont);
	for (size_t i = 0; i < cells.size(); ++i) {
		const Cell &c = cells[i];
		drawBox(g, c.bounds, int(i) == hovered || int(i) == dragging);
		g.setColour(kEdValue);
		g.drawText(textOf(c), c.bounds.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
	}

	// Одна строка о том, чем этот ящик является, чтобы он не читался как отдельный «микшер
	// плагина», живущий своей жизнью.
	g.setColour(kEdDim);
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

	g.setColour(kEdLabel);
	g.setFont(labelFont);
	g.drawText("LA32 VOICE SLOTS - THE FIRMWARE'S OWN TABLE",
	           area.removeFromTop(16.0f), juce::Justification::centredLeft);

	auto pool = area.removeFromTop(52.0f);
	const float bw = pool.getWidth() / 16.0f;
	int busy = 0;
	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		const size_t at = size_t(D110Core::kSlotStateTable) + size_t(s) * 2;
		const int state = (at < ram.size()) ? int(ram[at]) : -1;
		const bool on = (state == D110Core::kSlotBusyValue || state == D110Core::kSlotBusyValueAlt);
		if (on) ++busy;
		const juce::Rectangle<float> r(pool.getX() + bw * float(s % 16),
		                               pool.getY() + 26.0f * float(s / 16), bw - 4.0f, 22.0f);
		g.setColour(on ? kEdValue : kEdBox);
		g.fillRoundedRectangle(r, 2.0f);
		g.setColour(kEdBorder);
		g.drawRoundedRectangle(r.reduced(0.5f), 2.0f, 1.0f);
	}
	area.removeFromTop(4.0f);
	g.setColour(kEdDim);
	g.setFont(valueFont);
	g.drawText(juce::String(busy) + " of " + juce::String(D110Core::kNumHardwareVoices)
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
	g.setColour(kEdLabel);
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
		g.setColour(on ? kEdValue : kEdDim);
		g.setFont(valueFont);
		g.drawText(juce::String(partLabel(p)) + (on ? ": sounding" : ": -"),
		           r.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
	}
	area.removeFromTop(8.0f);

	// Мост в цифрах. Ноль потерянных сообщений - это условие, при котором всё показанное
	// выше вообще что-то значит, поэтому счётчики стоят рядом, а не прячутся в журнале.
	g.setColour(kEdLabel);
	g.setFont(labelFont);
	g.drawText("THE BRIDGE", area.removeFromTop(16.0f), juce::Justification::centredLeft);
	g.setColour(kEdDim);
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

	g.setColour(kEdLabel);
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
		g.setColour(i == 0 ? kEdValue : kEdDim);
		g.drawText(text, area.removeFromTop(lineH), juce::Justification::centredLeft);
	}
	if (n == 0) {
		g.setColour(kEdDim);
		g.drawText("nothing received yet", area.removeFromTop(lineH),
		           juce::Justification::centredLeft);
	}
}

void D110EditorPane::buttonPressed(int id) {
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
			textEntry.setText(nameAt(size_t(D110Core::kRamPatches)
			                         + size_t(patchSlot) * D110Core::kPatchRecord), false);
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

void D110EditorPane::mouseDown(const juce::MouseEvent &e) {
	const auto p = e.position;
	for (int i = 0; i < kNumTabs; ++i) {
		if (!tabBounds[(size_t)i].contains(p)) continue;
		selectTab(i);
		return;
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
			textEntry.setText(nameAt(size_t(D110Core::kRamToneTemp)
			                         + size_t(part) * D110Core::kToneRecord), false);
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
		if (row >= 0 && patch >= 0 && patch < D110Core::kNumPatches && patch != patchSlot) {
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
	if (dragging < 0 || dragStartValue < 0) return;
	// Четыре точки на шаг: достаточно мелко для громкости 0..100 и достаточно крупно, чтобы
	// поле из двух положений не прыгало от дрожания руки.
	const int steps = int((dragStartY - e.position.y) / 4.0f);
	setValue(cells[(size_t)dragging], dragStartValue + steps);
}

void D110EditorPane::mouseUp(const juce::MouseEvent &) {
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

D110AudioProcessorEditor::D110AudioProcessorEditor(D110AudioProcessor &p)
	: juce::AudioProcessorEditor(&p), panel(p), editorPane(p), card(p)
{
	addAndMakeVisible(panel);
	addAndMakeVisible(editorPane);
	// Карта добавляется последней и потому лежит поверх обоих - и прибора, и ящика.
	addAndMakeVisible(card);

	panel.onCardSlotClicked = [this] { card.toggle(); };
	card.onEjectNeedsDrawer = [this] { expansionTarget = 1.0f; };

	// Ящик закрыт при открытии окна: плагин - это прибор, а редактор к нему добавлен, и до
	// тех пор, пока его не попросили, он не занимает места.
	constrainer.setFixedAspectRatio(double(D110Panel::kRefW) / double(totalRefHeight()));
	constrainer.setSizeLimits(900, 100, D110Panel::kRefW * 2, 4000);
	setConstrainer(&constrainer);

	setResizable(true, true);
	setSize(1500, int(totalRefHeight() * (1500.0f / float(D110Panel::kRefW)) + 0.5f));
	startTimerHz(60);
}

float D110AudioProcessorEditor::totalRefHeight() const {
	return float(D110Panel::kRefH) + kHandleRefH + expansion * kPaneRefH;
}

void D110AudioProcessorEditor::applySize() {
	const float s = float(getWidth()) / float(D110Panel::kRefW);
	constrainer.setFixedAspectRatio(double(D110Panel::kRefW) / double(totalRefHeight()));
	setSize(getWidth(), int(totalRefHeight() * s + 0.5f));
}

// Полоса-ручка: во всю ширину, сразу под фотографией. Полная ширина затем, чтобы она
// читалась ящиком, который выдвигают, а не кнопкой.
juce::Rectangle<float> D110AudioProcessorEditor::handleBand() const {
	const float s = float(getWidth()) / float(D110Panel::kRefW);
	return { 0.0f, float(D110Panel::kRefH) * s, float(getWidth()), kHandleRefH * s };
}

void D110AudioProcessorEditor::timerCallback() {
	// Ход ящика. Сглажен к цели, поэтому он приходит на место, а не останавливается вкопанно,
	// и защёлкивается, когда разница мала, - иначе окно меняло бы размер вечно. 0.13 за кадр
	// при 60 кадрах в секунду - это около 360 мс: медленно настолько, чтобы прочитаться
	// выдвижением, и быстро настолько, чтобы не мешать.
	if (std::abs(expansionTarget - expansion) > 0.0015f) {
		expansion += (expansionTarget - expansion) * 0.13f;
		applySize();
	} else if (expansion != expansionTarget) {
		expansion = expansionTarget;
		applySize();
	}
}

void D110AudioProcessorEditor::paint(juce::Graphics &g)
{
	g.fillAll(juce::Colours::black);

	// Ручка. Уголок смотрит вниз, когда ящик закрыт, и вверх, когда открыт.
	const auto band = handleBand();
	g.setColour(juce::Colour(0xff141416));
	g.fillRect(band);
	g.setColour(juce::Colour(handleHover ? 0xff3a3a42 : 0xff26262c));
	g.fillRect(band.reduced(0.0f, band.getHeight() * 0.28f));

	const float cx = band.getCentreX();
	const float cy = band.getCentreY();
	const float a = juce::jmax(3.0f, band.getHeight() * 0.22f);
	const float dir = (expansion > 0.5f) ? -1.0f : 1.0f;
	juce::Path chevron;
	chevron.startNewSubPath(cx - a * 1.6f, cy - a * 0.5f * dir);
	chevron.lineTo(cx, cy + a * 0.5f * dir);
	chevron.lineTo(cx + a * 1.6f, cy - a * 0.5f * dir);
	g.setColour(juce::Colour(handleHover ? 0xffd0d0d8 : 0xff8a8a94));
	g.strokePath(chevron, juce::PathStrokeType(juce::jmax(1.5f, a * 0.35f)));

	// Подпись рядом с уголком, но только пока ящик закрыт: открытый говорит сам за себя.
	if (expansion < 0.02f) {
		g.setFont(juce::FontOptions(juce::jlimit(9.0f, 13.0f, band.getHeight() * 0.55f)));
		g.setColour(juce::Colour(0xff6a6a74));
		g.drawText("EDITOR", band.withTrimmedLeft(14.0f), juce::Justification::centredLeft);
	}
}

void D110AudioProcessorEditor::mouseDown(const juce::MouseEvent &e)
{
	if (!handleBand().contains(e.position)) return;
	expansionTarget = (expansionTarget > 0.5f) ? 0.0f : 1.0f;
	// Ящик закрывают - карте негде лежать, и она возвращается в гнездо. Оставить её висеть
	// за нижним краем окна значило бы потерять её из виду, не сказав об этом; а «не даём
	// закрыть ящик, пока карта снаружи» - это запрет там, где хватает движения.
	if (expansionTarget < 0.5f && card.isOut()) card.insert();
}

void D110AudioProcessorEditor::mouseMove(const juce::MouseEvent &e)
{
	const bool over = handleBand().contains(e.position);
	if (over == handleHover) return;
	handleHover = over;
	setMouseCursor(over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
	repaint();
}

void D110AudioProcessorEditor::mouseExit(const juce::MouseEvent &)
{
	if (!handleHover) return;
	handleHover = false;
	repaint();
}

void D110AudioProcessorEditor::resized()
{
	const float s = float(getWidth()) / float(D110Panel::kRefW);
	panel.setBounds(0, 0, D110Panel::kRefW, D110Panel::kRefH);
	panel.setTransform(juce::AffineTransform::scale(s));

	// Ящик занимает всё, что ниже прибора и полосы-ручки. Он рисуется целиком и обрезается
	// собственными границами - это и создаёт впечатление, что он выезжает из-под прибора.
	const int top = int((float(D110Panel::kRefH) + kHandleRefH) * s + 0.5f);
	editorPane.setBounds(0, top, getWidth(), juce::jmax(0, getHeight() - top));

	// Карта живёт в тех же опорных точках, что и панель, поэтому ей нужен только масштаб и
	// то, докуда сейчас доходит окно: по ним она сама поставит себе границы.
	card.setGeometry(s, totalRefHeight());
}
