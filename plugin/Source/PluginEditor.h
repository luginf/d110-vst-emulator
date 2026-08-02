#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

#include <array>
#include <vector>

// The front panel IS the reference photograph. docs/panel_reference.png is drawn as the
// background at 1:1 and every control is an invisible hit-region placed at coordinates measured
// off that photo (see docs/panel_reference_notes.md), so position and photorealism are both
// correct by construction rather than by hand-drawing.
//
// Only two things are ever painted over it:
//
//  * the LCD, replaced inside exactly the rectangle its window occupies in the photo. Its content
//    is the emulated MSM6222B's own rendered dot matrix, glyphs and all, so nothing here consults
//    a font - only the dot geometry and colours are ours (docs/lcd_reference.png);
//  * the MIDI MESSAGE lamp, the only indicator a D-110 has.
//
// Buttons and the VOLUME knob are never redrawn: the cap or the disc is cut straight out of the
// photograph, and the cut-out itself recedes into its recess or spins about its own axis.
//
// Emulator settings live on a right-click, never a control drawn onto the panel - the hardware has
// no such thing, and the JUCE standalone wrapper already puts its own Options button in the
// top-left corner of its window.
class D110Panel : public juce::Component, private juce::Timer {
public:
	// Reference space = the photo's own pixels.
	static constexpr int kRefW = 2124;
	static constexpr int kRefH = 256;

	explicit D110Panel(D110AudioProcessor &);
	~D110Panel() override;

	void paint(juce::Graphics &) override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseDoubleClick(const juce::MouseEvent &) override;
	void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

private:
	// One front-panel cap as it sits in the photograph. `scanPort`/`scanBit` are this button's
	// position in the real 2x8 key-scan matrix, straight out of INPUT_PORTS_START(d110) in MAME's
	// src/mame/roland/roland_d10.cpp - these now close the actual switch in the running firmware.
	struct PanelButton {
		float x, y, w, h;
		const char *name;
		int scanPort;        // 0 = SC0 (top row), 1 = SC1 (bottom row)
		juce::uint8 scanBit;
	};

	struct ButtonMotion {
		bool held = false;    // mouse currently down on it
		bool latched = false; // double-clicked: stays held so combos are possible
		float depth = 0.0f;   // eased 0..1, how far the cap has sunk
	};

	enum class Drag { none, volume };

	void timerCallback() override;
	int buttonAt(juce::Point<float>) const; // index into kButtons, kPowerIndex, or -1
	void setButtonState(int index, bool down); // closes/opens the real scan-matrix switch
	void showOptionsMenu();

	juce::Image cutOut(juce::Rectangle<float>) const;
	juce::Colour recessColourOf(juce::Rectangle<float>) const;

	void rebuildLcdImage();
	void paintLcd(juce::Graphics &) const;
	void paintButton(juce::Graphics &, int index) const;
	void paintPowerSwitch(juce::Graphics &) const;
	void paintVolumeKnob(juce::Graphics &) const;
	void paintMidiLamp(juce::Graphics &) const;
	void paintMemoryCard(juce::Graphics &) const;

	static juce::Rectangle<float> pressedRect(juce::Rectangle<float> face, float depth,
	                                          float shrink, float drop);
	static void paintPressedCap(juce::Graphics &, const juce::Image &cap, juce::Colour recess,
	                            juce::Rectangle<float> face, juce::Rectangle<float> dst, float depth);

	D110AudioProcessor &processor;

	juce::Image panelImage;

	// Карта памяти M-256D. Её фотография ВЫШЕ окна: 236 x 370 против панели в 256, - поэтому
	// она не выглядывает из щели, а ПРОЕЗЖАЕТ мимо, как титры, и наклейка успевает пройти
	// целиком. Одновременно видны 106 точек - промежуток между низом щели и низом панели.
	//
	// Геометрия снята с фотографии панели (docs/panel_reference_notes.md): проём щели
	// 1600, 120, размером 236 x 30. Ширина карты в истинном масштабе прибора - те же 236,
	// что и ширина проёма, и это совпадение служит проверкой масштаба.
	juce::Image cardImage;
	static constexpr float kCardX = 1600.0f;
	static constexpr float kCardWidth = 236.0f;
	static constexpr float kCardHeight = 370.0f;
	static constexpr float kSlotBottom = 150.0f; // пол проёма: ниже него карта уже снаружи
	static constexpr float kCardSeatedY = kSlotBottom - kCardHeight; // торцом в проёме
	// Верх отсечения. Карта не прячется за щель целиком: вставленная, она стоит торцом в
	// проёме, и восемнадцать точек её края видны - иначе занятое гнездо ничем не отличалось бы
	// от пустого. Восемнадцать - это около четырёх миллиметров в масштабе панели (4.4 точки на
	// миллиметр), то есть толщина корпуса карты у хвата; проём выше 30 точек, и оставшаяся над
	// картой темнота читается как глубина щели, из которой карта и выезжает.
	static constexpr float kCardClipTop = 132.0f;
	// Внутри проёма карта в тени. Тень нарисована градиентом поверх неё, от kCardClipTop до
	// пола проёма, а не заложена в картинку: карта сквозь проём проезжает, и затемняться
	// должно место, а не карта.
	static constexpr float kSlotShadeAlpha = 0.58f;
	// Доля пути за кадр в самой быстрой точке хода; закон движения - в timerCallback.
	static constexpr float kCardStep = 0.032f;
	// Насколько карта выдвинута: 0 - сидит в приборе, 1 - ушла за нижний край окна.
	float cardTravel = 0.0f;
	float cardTarget = 0.0f;
	juce::Image lcdImage;                    // offscreen dot-matrix render, rebuilt only on change
	std::vector<juce::Image> capImages;      // one cut-out per button
	std::vector<juce::Colour> recessColours; // the recess each cap sinks into
	juce::Image powerCap;
	juce::Colour powerRecessColour;
	juce::Image volumeDisc;                  // the knob, lifted out to be spun about its own axis

	// The display exactly as the real MSM6222B renders it: one byte per dot row, bit 4
	// leftmost. No font table and no character codes are involved on this path - the
	// glyphs come from the controller's own mask CGROM inside the emulated machine.
	juce::uint8 lcdRows[D110Core::kLcdBytes] = {};
	bool lcdLive = false;

	static constexpr int kNumButtons = 16;
	static constexpr int kPowerIndex = -2;   // pseudo-index returned by buttonAt()
	static const PanelButton kButtons[kNumButtons];

	std::array<ButtonMotion, kNumButtons> motion;
	ButtonMotion powerMotion;

	float volumeDisplayed = -1.0f;           // eased towards the parameter, <0 = not yet initialised
	Drag drag = Drag::none;
	juce::Point<float> dragStart;
	float dragStartValue = 0.0f;

	D110AudioProcessor::LcdSnapshot lastSnapshot; // to skip repaints when nothing changed
	bool lastPowerOn = false;
	bool lcdInitialised = false;

	std::unique_ptr<juce::FileChooser> fileChooser;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110Panel)
};

class D110AudioProcessorEditor : public juce::AudioProcessorEditor {
public:
	explicit D110AudioProcessorEditor(D110AudioProcessor &);

	void paint(juce::Graphics &) override;
	void resized() override;

private:
	D110Panel panel;
	juce::ComponentBoundsConstrainer constrainer;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110AudioProcessorEditor)
};
