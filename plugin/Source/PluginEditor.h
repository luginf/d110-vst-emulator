#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "D110Keyboard.h"
#include "PluginProcessor.h"
#include "sequencer/D110SequencerPanel.h"
#include "sequencer/D110SequencerRetroPanel.h"

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
// Emulator settings live on a right-click, never a control drawn onto the panel - the hardware
// has no such thing. Standalone-only items (Audio/MIDI Settings, save/load state, reset) are
// folded into the same menu too, now that the standalone window uses a native title bar and no
// longer has JUCE's own Options button to carry them (see D110AudioProcessorEditor's
// parentHierarchyChanged and D110Panel::showOptionsMenu).
class D110Panel : public juce::Component, private juce::Timer {
public:
	// Reference space = the photo's own pixels.
	static constexpr int kRefW = 2124;
	static constexpr int kRefH = 256;

	// Compact mode (Utility tab, "PANEL SIZE") splices sections out of the photograph entirely -
	// docs/panel_reference_compact.png, built by cutting [0,244) (the Roland wordmark and the
	// PHONES jack, both purely decorative - PHONES has no hit region at all, see
	// panel_reference_notes.md's "Decorative only") and [1560,1865) (the MEMORY CARD section)
	// out of panel_reference.png and rejoining the three remaining strips (Alan's request,
	// 2026-08-20, matching a mockup he supplied - "only the essentials": VOLUME, the LCD, the
	// button grid, POWER, the MIDI MESSAGE lamp). Every reference-space X coordinate used to
	// paint or hit-test something goes through mapX() below rather than being used raw, which
	// folds both cuts into one lookup: subtract kCompactLeftCutEnd always, and kCompactCardShift
	// on top of that for anything at or past the card section. currentRefW() is what every
	// window-sizing calculation in PluginEditor.cpp reads instead of the bare kRefW constant, so
	// the whole editor - window aspect ratio, zoom percent, drawer widths - narrows along with
	// the panel itself.
	//
	// Alan re-cropped the file himself afterwards (2026-08-20) to also trim the last 78px of the
	// photo's own right edge - the decorative right rack ear, past POWER/the MIDI MESSAGE lamp
	// (kBezelX+kBezelW=1998, kLampX+kLampW=1966, both comfortably clear) - so kCompactRefW is
	// the actual measured width of his file (cross-checked pixel-for-pixel against
	// panel_reference.png: [244,1560) + [1865,2046)), not just kRefW minus the two cuts above;
	// no mapX() change needed since nothing hit-tested/painted ever sat past x=2046.
	static constexpr float kCompactLeftCutEnd = 244.0f;
	static constexpr float kCompactCardCutStart = 1560.0f;
	static constexpr float kCompactCardCutEnd = 1865.0f;
	static constexpr float kCompactCardShift = kCompactCardCutEnd - kCompactCardCutStart;
	static constexpr int kCompactRefW = 1497;
	static int currentRefW(bool compact) { return compact ? kCompactRefW : kRefW; }
	static float mapX(float refX, bool compact) {
		if (!compact) return refX;
		float x = refX - kCompactLeftCutEnd;
		if (refX >= kCompactCardCutEnd) x -= kCompactCardShift;
		return x;
	}

	explicit D110Panel(D110AudioProcessor &);
	~D110Panel() override;

	void paint(juce::Graphics &) override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseDoubleClick(const juce::MouseEvent &) override;
	void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

	// Щелчок по щели карты памяти. Сама карта панели больше не принадлежит - она ездит по
	// всему окну, включая ящик, и потому живёт отдельным компонентом (D110MemoryCard). А вот
	// ЩЕЛЬ - часть фотографии прибора, и ловить попадание в неё должна панель.
	std::function<void()> onCardSlotClicked;
	// Options menu's Retro Sequencer toggle - the editor swaps which sequencer view is
	// visible in response (see D110AudioProcessorEditor's own resized()); this panel has
	// no reference to the sequencer drawer itself, hence the callback rather than a
	// direct call.
	std::function<void()> onSequencerModeChanged;
	// Обрамление щели, а не сам проём: попасть мышью в полоску высотой тридцать точек трудно,
	// а обрамление - это ровно то, что человек видит как «щель».
	static constexpr float kSlotHitX = 1588.0f, kSlotHitY = 109.0f;
	static constexpr float kSlotHitW = 260.0f, kSlotHitH = 52.0f;

	// Забирает у прибора всё, что панель показывает: индикатор, лампу, положение ручки, ход
	// карты. В работе это делает таймер панели; отдельно вызывается там, где очереди
	// сообщений нет, - например при съёмке панели в файл (plugin/editor_shot.cpp).
	void refreshFromInstrument() { timerCallback(); }

	// The window/reference-artwork ratio the editor is about to apply as a Component
	// transform. The LCD's offscreen render is supersampled relative to THIS, not to a
	// fixed reference-space multiplier, so the resample the transform then performs is
	// always a modest, fixed ratio - see rebuildLcdImage() for why that matters.
	void setDisplayScale(float scale);

	// Public so D110EditorPane's OPTIONS button (standalone only - see its
	// onOptionsButtonClicked) can reuse the exact same menu the panel's own right-click
	// already shows, content included, rather than keeping a second copy in sync.
	void showOptionsMenu();

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

	juce::Image cutOut(juce::Rectangle<float>) const;
	juce::Colour recessColourOf(juce::Rectangle<float>) const;

	void rebuildLcdImage();
	void paintLcd(juce::Graphics &) const;
	void paintButton(juce::Graphics &, int index) const;
	void paintPowerSwitch(juce::Graphics &) const;
	void paintVolumeKnob(juce::Graphics &) const;
	void paintMidiLamp(juce::Graphics &) const;

	static juce::Rectangle<float> pressedRect(juce::Rectangle<float> face, float depth,
	                                          float shrink, float drop);
	static void paintPressedCap(juce::Graphics &, const juce::Image &cap, juce::Colour recess,
	                            juce::Rectangle<float> face, juce::Rectangle<float> dst, float depth);

	D110AudioProcessor &processor;

	juce::Image panelImage;
	juce::Image panelImageCompact; // see kCompactCutStart/End above

	juce::Image lcdImage;                    // offscreen dot-matrix render, rebuilt only on change
	float lcdDisplayScale = 1.0f;            // last scale passed to setDisplayScale()
	std::vector<juce::Image> capImages;      // one cut-out per button
	std::vector<juce::Colour> recessColours; // the recess each cap sinks into
	juce::Image powerCap;
	juce::Colour powerRecessColour;
	juce::Image volumeDisc;                  // the knob, lifted out to be spun about its own axis

	// The display exactly as the real MSM6222B renders it: one byte per dot row, bit 4
	// leftmost. No font table and no character codes are involved on this path - the
	// glyphs come from the controller's own mask CGROM inside the emulated machine.
	juce::uint8 lcdRows[D110CoreType::kLcdBytes] = {};
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


// Карта памяти M-256D - отдельный компонент, лежащий ПОВЕРХ всего окна.
//
// Раньше её рисовала сама панель, и потому карта могла только проехать мимо кадра и
// скрыться: панель ростом с прибор, 256 точек, а карта - 370. Теперь под прибором есть
// ящик, и извлечённой карте есть куда лечь. Она выезжает из щели, ложится на ящик целиком
// видимой и там остаётся; за неё можно взяться левой кнопкой и передвинуть куда удобно.
//
// Компонент, а не рисование поверх детей, именно ради этого: тащить мышью можно только то,
// что само получает события мыши, а ящик под картой - живой компонент со своими полями.
// Пока карта сидит в гнезде, она мышь НЕ перехватывает - щелчок по щели тогда достаётся
// панели, как и раньше.
class D110MemoryCard : public juce::Component, private juce::Timer {
public:
	explicit D110MemoryCard(D110AudioProcessor &);

	void paint(juce::Graphics &) override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;

	// Извлечь или вставить - то же самое, что щелчок по щели на приборе.
	void toggle();
	void insert();
	// Вставлена ли она (по положению, а не по мнению прошивки): нужно хозяину окна, чтобы
	// вернуть карту в гнездо, когда ящик закрывают, - лежать ей тогда негде.
	bool isOut() const { return target > 0.5f; }

	// Масштаб панели и полная высота окна в опорных точках. Задаёт хозяин окна при каждом
	// изменении размера: карта живёт в тех же опорных точках, что и панель, поэтому и
	// ездит вместе с ней при любом масштабе.
	void setGeometry(float panelScale, float totalRefHeight);

	// Извлечение просит открыть ящик: карта ложится на него, и на закрытом ящике ей просто
	// негде быть.
	std::function<void()> onEjectNeedsDrawer;

	// Геометрия, снятая с фотографии панели (docs/panel_reference_notes.md): проём щели
	// 1600, 120, размером 236 x 30. Ширина карты в истинном масштабе прибора - те же 236,
	// что и ширина проёма, и это совпадение служит проверкой масштаба.
	static constexpr float kCardX = 1600.0f;
	static constexpr float kCardWidth = 236.0f;
	static constexpr float kCardHeight = 370.0f;
	static constexpr float kSlotBottom = 150.0f;   // пол проёма: ниже него карта уже снаружи
	static constexpr float kCardSeatedY = kSlotBottom - kCardHeight;   // торцом в проёме
	// Верх отсечения. Карта не прячется за щель целиком: вставленная, она стоит торцом в
	// проёме, и восемнадцать точек её края видны - иначе занятое гнездо ничем не отличалось
	// бы от пустого. Восемнадцать - это около четырёх миллиметров в масштабе панели
	// (4.4 точки на миллиметр), то есть толщина корпуса карты у хвата.
	static constexpr float kCardClipTop = 132.0f;
	// Внутри проёма карта в тени. Тень рисуется поверх неё градиентом, а не заложена в
	// картинку: карта сквозь проём проезжает, и затемняться должно место, а не карта.
	static constexpr float kSlotShadeAlpha = 0.58f;
	// Доля пути за кадр в самой быстрой точке хода; закон движения - в timerCallback.
	static constexpr float kCardStep = 0.032f;

private:
	void timerCallback() override;
	// Куда карта поедет, если её извлечь: на ящик, ниже полосы-ручки и ниже ряда вкладок,
	// чтобы она их не закрывала. Точка эта - только НАЧАЛЬНАЯ: дальше её задаёт мышь.
	static constexpr float kRestX = kCardX;
	static constexpr float kRestY = 360.0f;
	juce::Point<float> position() const;   // левый верхний угол, в опорных точках
	void updateBounds();

	D110AudioProcessor &processor;
	juce::Image cardImage;

	// Насколько карта вышла: 0 - сидит в гнезде, 1 - лежит на ящике. Между этими двумя
	// точками она едет по прямой, поэтому положение - это их смесь, а не отдельная пара
	// координат: закон движения остаётся тем же, каким он был снят раскадровкой.
	float travel = 0.0f;
	float target = 0.0f;
	juce::Point<float> rest{ kRestX, kRestY };

	float scale = 1.0f;
	float totalRefH = 1190.0f;

	bool dragging = false;
	juce::Point<float> dragGrab;   // где именно за карту взялись, в опорных точках

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110MemoryCard)
};

// The minimal on-screen test keyboard (D110Keyboard, see D110Keyboard.h) sits under its
// own handle band here, foldable like the extended editor's drawer but by a separate
// action, and open by default since it's the most direct way to hear the instrument
// without any MIDI cabling. In this plugin it reaches the firmware through
// D110AudioProcessor::injectTestNote(), which hands notes to the same collector
// (osMidiCollector) that handleIncomingMidiMessage(MidiInput*, ...) does.

// Расширенный редактор - ящик, выезжающий из-под прибора.
//
// Он НАРИСОВАН КОДОМ, и это осознанное расхождение с самой панелью: панель - фотокомпозит,
// потому что изображает существующую вещь, а редактора у D-110 не существует вовсе. У
// прибора на всё про всё индикатор в две строки по шестнадцать знаков, и добраться по нему
// до пятидесяти восьми величин партиала - это десятки нажатий; ящик показывает их разом.
//
// Ни одно поле не трогает звуковой движок напрямую. Правка уходит ПРИБОРУ эксклюзивным
// сообщением - тем же путём, каким её послал бы внешний редактор настоящему D-110, -
// прошивка меняет свою память, а зеркало доносит это до движка. Поэтому правка отсюда и
// правка с панели это одно и то же событие, и обе видны в обоих местах.
//
// Каждый адрес, по которому пишет эта панель, ИЗМЕРЕН, а не взят из руководства на веру:
// plugin/editor_write_probe.cpp посылает по одной записи в каждую область и смотрит, какой
// байт батарейного ОЗУ сдвинулся.
class D110EditorPane : public juce::Component, private juce::Timer {
public:
	explicit D110EditorPane(D110AudioProcessor &);

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseMove(const juce::MouseEvent &) override;
	void mouseExit(const juce::MouseEvent &) override;
	void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

	// Перечитывает память прибора. Обычно это делает таймер; отдельно вызывается снимком
	// панели в файл, которому не на чем крутить очередь сообщений.
	void refreshFromInstrument();
	void selectTab(int index);

	// Utility tab's WINDOW SIZE control (a percentage of the reference width, e.g. 100) - set
	// by the owning D110AudioProcessorEditor, which is the one that actually knows how to
	// resize itself. A plain callback rather than this pane reaching upward through some
	// back-reference, matching how panel.onCardSlotClicked/card.onEjectNeedsDrawer are already
	// wired in D110AudioProcessorEditor's own constructor.
	//
	// Exists instead of a maximise button: on the owner's own window manager, several attempts
	// at correcting what a native maximise actually does on the wire each traded one
	// window-manager-specific bug for another that couldn't be reproduced or verified from
	// here. A percentage resize is exactly the same setSize() call a manual drag-resize
	// already makes reliably, just computed from a chosen number instead of a mouse
	// position - no window-manager or monitor-geometry involvement at all.
	std::function<void(int percent)> onRequestZoom;

	// Utility tab's THEME toggle flips a process-wide d110ui::Theme (see UiTheme.h) that
	// every custom-drawn drawer's paint() reads on its own - this callback exists only to
	// tell the owner to repaint them all immediately rather than waiting for whichever one
	// happens to redraw next.
	std::function<void()> onThemeChanged;

	// Utility tab's SEQUENCER toggle mirrors D110Panel's own right-click Options entry (same
	// processor.getSequencerRetroMode() flag) - this callback exists so the owner can swap
	// which sequencer drawer view is visible, same as D110Panel::onSequencerModeChanged does
	// for the right-click path.
	std::function<void()> onSequencerModeChanged;

	// Utility tab's PANEL SIZE toggle (processor.getCompactPanelMode()) - fired after the flag
	// itself has already flipped, same "just tell the owner" shape as onSequencerModeChanged,
	// except this one also changes the window's own width/aspect ratio, which only
	// D110AudioProcessorEditor (holding getWidth()/setSize()/the panel/the constrainer) can do -
	// see its own wiring for the actual resize math.
	std::function<void()> onCompactPanelModeChanged;

	// OPTIONS button (standalone only - see optionsButtonBounds below) - the owner wires this
	// to D110Panel::showOptionsMenu() so it's the exact same menu as the panel's own
	// right-click, content included, rather than a second copy of it living here too.
	std::function<void()> onOptionsButtonClicked;

private:
	void timerCallback() override;
	// Держит недавно посланные, ещё не подтверждённые правки поверх свежепрочитанной ram -
	// см. комментарий у PendingEdit. Вызывается сразу после каждого getRam() в
	// refreshFromInstrument(), обоих мест.
	void reapplyPendingEdits();

	// Куда пишет поле. Области - те же, что у Roland, и адрес каждой в ОЗУ прошивки
	// измерен; см. D110Core.
	enum class Area { TimbreTemp, ToneTemp, Rhythm, System, Timbres, Patches, Tones };

	struct Cell {
		juce::Rectangle<float> bounds;
		Area area = Area::TimbreTemp;
		int index = 0;   // партия, клавиша, ячейка памяти или смещение в системной области
		int field = 0;   // смещение внутри записи; для System не используется
		int lo = 0, hi = 127;
	};

	// Правый клик по TONE GROUP/TONE - список всех тонов (a/b/i/r, 64 в каждой) вместо
	// перебора колесом по одному. Общий для живой области партии (Parts) и записи патча
	// (PARTS OF PATCH внутри Patches) - у обеих одна и та же пара байт группа+номер, разнится
	// только куда её послать (index) и с каким смещением записи (groupField - адрес байта
	// группы; номер идёт следующим байтом).
	void showToneListMenu(Area area, int index, int groupField);
	// Правый клик по DRUM SOUND на вкладке Rhythm - список всех тембров ударных и памяти,
	// тем же чтением их имён, что и showToneListMenu.
	void showRhythmSoundMenu(int slot);
	// Правый клик по полю PCM партиала (Tone tab) - список всех 128 образцов ПЗУ по имени,
	// вместо перебора номера колесом. Github issue #2.
	void showPcmWaveMenu(const Cell &pcmCell);

	enum class Tab { Parts, Tone, Rhythm, Patches, Timbres, Tones, System, Monitor, Utility };
	static constexpr int kNumTabs = 9;

	// The PATCHES tab's own two views, switched by a small sub-tab strip under the main
	// one - the 64-patch list and the 8-part breakdown of whichever one is selected used to
	// share the tab as a fixed 58/42 vertical split; shrinking the drawer past a certain
	// height clipped the bottom of whichever section didn't have its own scroll (only the
	// patch list does, via patchScroll). Splitting into two full-height sub-tabs instead
	// means neither view is ever squeezed below what it needs, whatever the drawer's height.
	enum class PatchesSubTab { AllPatches, PartsOfPatch };

	// Один параметр партиала: подпись, смещение внутри его 58-байтной записи и предел.
	struct ToneParam {
		const char *name;
		int offset;
		int hi;
	};

	struct Button {
		juce::Rectangle<float> bounds;
		juce::String text;
		int id = 0;
	};

	// Подпись, посчитанная вместе с полями. Держать их в одном списке нужно затем, чтобы
	// заголовок столбца и сам столбец не считались дважды по разным формулам и однажды не
	// разъехались.
	struct Label {
		juce::Rectangle<float> bounds;
		juce::String text;
		bool heading = true;
		juce::Justification just = juce::Justification::centredLeft;
	};

	void layout();
	void layoutParts(juce::Rectangle<float> area);
	void layoutTone(juce::Rectangle<float> area);
	void layoutRhythm(juce::Rectangle<float> area);
	void layoutPatches(juce::Rectangle<float> area);
	void layoutPatchesList(juce::Rectangle<float> area);
	void layoutPatchesParts(juce::Rectangle<float> area);
	void layoutTimbres(juce::Rectangle<float> area);
	void layoutTones(juce::Rectangle<float> area);
	void layoutSystem(juce::Rectangle<float> area);
	void layoutUtility(juce::Rectangle<float> area);
	void layoutParamColumn(juce::Rectangle<float> column, int partialBase,
	                       const ToneParam *params, int count);
	void paintMonitor(juce::Graphics &, juce::Rectangle<float> area);
	// This drawer's own label/value font size, relative to how it looked at the app's default
	// window size - see the .cpp for why this isn't simply getWidth()/1500.
	float fontScale() const;
	// PARTIAL MUTE (Tone tab, field 12) - four per-partial ON/OFF toggles instead of the raw
	// 0-15 bitmask a generic Cell would show. Github issue #1: cycling a 16-value drag/wheel
	// field to find one bit was impractical. Still a genuine Cell/byte underneath (see
	// setValue's ToneTemp case) - only the paint/hit-test are special-cased.
	void paintPartialMuteCell(juce::Graphics &, const Cell &, bool hover) const;
	bool isPartialMuteCell(const Cell &) const;
	juce::Rectangle<float> partialMuteSegment(const Cell &, int partial) const;
	void buttonPressed(int id);
	// showLaReferencePopup() - shows docs/D20infos.png (embedded via D110PanelData/BinaryData)
	// in its own pop-up window - is now a free function in PluginEditor.cpp's own anonymous
	// namespace, not a member: D110Panel::showOptionsMenu() needed to call it too (Github
	// issue #3), and neither component owns the other.

	int cellAt(juce::Point<float>) const;
	size_t addressOf(const Cell &) const;
	int valueOf(const Cell &) const;
	void setValue(const Cell &, int value);
	juce::String textOf(const Cell &) const;
	// Имя из памяти прибора: десять знаков, как их показывает индикатор.
	juce::String nameAt(size_t ramOffset) const;
	// Имя тона по паре «группа, номер». Пресетные группы и ритм живут в ПЗУ, поэтому их
	// имена берутся у звукового движка, который загрузил то же ПЗУ; внутренние тона - из
	// памяти самой прошивки.
	juce::String toneName(int group, int number) const;

	D110AudioProcessor &processor;

	Tab tab = Tab::Parts;
	std::array<juce::Rectangle<float>, kNumTabs> tabBounds{};
	// Standalone only, sits in the leftover space right of the tab row (empty in the plugin
	// builds, and in standalone whenever the window's too narrow for it to fit) - fires
	// onOptionsButtonClicked, somewhere visible instead of needing to know the panel's
	// right-click menu exists.
	juce::Rectangle<float> optionsButtonBounds{};

	// Чьи записи показывают вкладки, у которых есть «текущая партия».
	int part = 0;
	std::array<juce::Rectangle<float>, 8> partBounds{};
	juce::Rectangle<float> toneNameBounds;
	// Utility tab's WINDOW SIZE button - stored separately from the generic `buttons` list
	// (which only dispatches left-clicks) because right-click on it also needs handling, in
	// mouseDown()'s popup-menu branch.
	juce::Rectangle<float> zoomBounds;
	juce::Rectangle<float> romFolderBounds;
	int tonePartial = 0;
	std::array<juce::Rectangle<float>, 4> tonePartialBounds{};

	// Длинные списки листаются колесом мимо полей.
	int rhythmScroll = 0;
	int timbreScroll = 0;
	int patchScroll = 0;
	int toneScroll = 0;
	int patchSlot = 0;    // патч, чьи партии показаны на под-вкладке PARTS OF PATCH
	int toneSlot = 0;     // выбранная ячейка памяти тонов

	// See PatchesSubTab's own comment.
	PatchesSubTab patchesSubTab = PatchesSubTab::AllPatches;
	std::array<juce::Rectangle<float>, 2> patchesSubTabBounds{};

	// UTILITY's own scroll - unlike the other tabs (which page a fixed-size row list), this
	// one stacks sections of unequal, growing height, so it scrolls in raw pixels rather than
	// row units. utilityContentHeight is measured as a side effect of layoutUtility() itself
	// (the space its sections actually consumed, regardless of scroll position); the two
	// rectangles are the track/thumb hit regions layoutUtility() leaves behind for
	// mouseDown/mouseDrag, empty when everything already fits without scrolling.
	float utilityScrollOffset = 0.0f;
	float utilityContentHeight = 0.0f;
	juce::Rectangle<float> utilityScrollTrack, utilityScrollThumb;
	bool draggingUtilityScroll = false;
	float utilityScrollDragStartY = 0.0f, utilityScrollDragStartOffset = 0.0f;

	juce::Rectangle<float> tableArea;
	juce::Rectangle<float> contentArea;   // для вкладок, которые рисуются целиком
	float rowHeight = 0.0f;

	std::vector<Label> labels;
	std::vector<Cell> cells;
	std::vector<Button> buttons;

	// Одно поле ввода на весь редактор: им набирается любое имя и надпись для индикатора.
	juce::TextEditor textEntry;
	int textEntryTarget = 0;   // 0 - никуда, 1 - имя тона партии, 2 - надпись на индикатор,
	                           // 3 - имя патча, 4 - имя тона в памяти
	int textEntryButton = -1;  // кнопка, на месте которой стоит поле ввода
	int hovered = -1, dragging = -1;
	float dragStartY = 0.0f;
	int dragStartValue = 0;

	// Снимок памяти прибора, обновляемый по таймеру: редактор всегда показывает то, что в
	// приборе на самом деле, включая правки, сделанные с его собственной панели.
	std::vector<uint8_t> ram;
	uint64_t ramGen = 0;
	bool ramValid = false;

	// Правки, посланные пользователем и ещё не подтверждённые настоящим ответом прибора.
	// setValue() ставит оптимистичное значение в `ram` сразу, чтобы поле под курсором не
	// отставало, - но правка идёт в прошивку тем же путём, что и нота, с той же реальной
	// задержкой (0-18 мс измерено сегодня), а сама память прошивки меняется постоянно по
	// не связанным с этим причинам (мигание курсора, лампа MIDI), и каждое такое изменение
	// поднимает общий счётчик поколения. Без этого списка ближайшее срабатывание таймера
	// перечитывает ВСЮ память и стирает ещё не принятую прошивкой правку до её настоящего
	// значения - подёргивание при быстрой прокрутке колесом это ровно оно. Список удерживает
	// оптимистичное значение до тех пор, пока прошивка сама не подтвердит его или не истечёт
	// срок ожидания.
	struct PendingEdit { size_t address; uint8_t value; juce::int64 sentMs; };
	std::vector<PendingEdit> pendingEdits;

	// Имена тонов из ПЗУ спрашиваются у движка по одному разу: они не меняются, а читать их
	// на каждой перерисовке значило бы лезть в чужой поток по десять раз в секунду.
	mutable std::array<juce::String, 4 * 64> romToneNames;
	mutable std::array<bool, 4 * 64> romToneNameKnown{};

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110EditorPane)
};

// Прибор, ящик и полоса-ручка между ними.
//
// Ящик открывается ВНИЗ, как у остальных синтезаторов этой серии: сам прибор остаётся
// целым, а редактор выезжает из-под него. Ручка - во всю ширину, чтобы читалась ящиком, а
// не кнопкой, и стоит НИЖЕ фотографии, а не на ней: на лицевой стороне прибора нет и не
// может быть органов управления, которых нет у железа.
class D110AudioProcessorEditor : public juce::AudioProcessorEditor {
public:
	explicit D110AudioProcessorEditor(D110AudioProcessor &);

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseMove(const juce::MouseEvent &) override;
	void mouseExit(const juce::MouseEvent &) override;
	// Forwards to the retro sequencer's own D-pad navigation when it doesn't otherwise reach
	// it - see the .cpp for why this is needed (D110Keyboard grabs keyboard focus on every
	// click, so a physical EXIT/ENTER/arrow press after playing a note on it would otherwise
	// silently do nothing).
	bool keyPressed(const juce::KeyPress &) override;
	// Standalone only: switches the plugin wrapper's window from JUCE's own custom-drawn
	// title bar to the OS's native one, to match Nonet Sequencer's window (Alan's request) -
	// see the .cpp for why this is done here rather than at window construction.
	void parentHierarchyChanged() override;

	// Перечитать прибор обеими половинами сразу - панелью и ящиком. Нужно снимку целого
	// редактора, у которого нет ни окна, ни очереди сообщений, чтобы крутить их таймеры.
	void refreshFromInstrument() {
		panel.refreshFromInstrument();
		editorPane.refreshFromInstrument();
	}
	// Открыть ящик без мыши - тем же снимком.
	void setExpanded(bool open) { expansion = expansionTarget = open ? 1.0f : 0.0f; }
	// Same, for the sequencer drawer - closed by default in normal use, but editor_shot's
	// whole-editor snapshot wants to show it open, the way it already does for the others.
	void setSequencerExpanded(bool open) { sequencerExpansion = sequencerExpansionTarget = open ? 1.0f : 0.0f; }

	// Высота полосы-ручки в опорных точках панели.
	static constexpr float kHandleRefH = 34.0f;
	// Default/initial height of the drawer, in the same reference units - what a new project
	// (or one saved before this was adjustable) opens with. Used to be chosen so the UTILITY
	// tab fit entirely without a scrollbar - but its list of sections only grows, and resizing
	// the WHOLE drawer's (and so the whole window's) height for every new section isn't the
	// right trade-off. UTILITY now has its own scrollbar instead (layoutUtility()'s
	// utilityScrollOffset), so this only needs to stay comfortable for the nine-part table
	// (the most demanding of the other tabs) - about 530 points at the usual window width
	// (1500 points, scale 0.71). The actual live height is editorPaneRefH below, which starts
	// at this default but can be dragged - see the KEYBOARD handle band's dual role in
	// mouseDown/mouseDrag.
	static constexpr float kPaneRefH = 750.0f;
	static constexpr float kMinPaneRefH = 260.0f;
	static constexpr float kMaxPaneRefH = 1600.0f;
	// Handle band above the test keyboard - slimmer than the editor's own, in keeping with
	// the keyboard being the minimal add-on rather than the main drawer. Doubles as the
	// keyboard pane's own resize handle exactly the way this band doubles for the editor
	// pane above it - see keyboardPaneRefH below.
	static constexpr float kKeyboardHandleRefH = 26.0f;
	static constexpr float kMinKeyboardPaneRefH = 70.0f;
	static constexpr float kMaxKeyboardPaneRefH = 400.0f;
	// Handle band above the sequencer drawer - same slim treatment as the keyboard's, and the
	// same dual role for the KEYBOARD pane above it.
	static constexpr float kSequencerHandleRefH = 26.0f;
	// The sequencer drawer is the last one, with no further drawer below it to lend it a handle
	// band - so it gets its own thin resize-only grip instead, right under it (Alan asked for
	// this 2026-08-19: the drawer "n'est pas très haute" and had no way to grow at all before).
	static constexpr float kSequencerResizeGripRefH = 10.0f;
	static constexpr float kMinSequencerPaneRefH = 200.0f;
	static constexpr float kMaxSequencerPaneRefH = 1400.0f;

private:
	// Shown once from the constructor when no ROMs were found, so a fresh install can point
	// at a folder right away instead of having to first find the Utility tab inside the
	// extended editor drawer - see its own .cpp comment.
	void showRomSetupDialog();

	float totalRefHeight() const;
	void applySize();
	juce::Rectangle<float> handleBand() const;
	juce::Rectangle<float> keyboardHandleBand() const;
	juce::Rectangle<float> sequencerHandleBand() const;
	juce::Rectangle<float> sequencerResizeBand() const;

	// Needed directly (not just by the child components below, which each keep their own
	// reference) for setEditorPaneRefH() - see mouseUp()'s use of it.
	D110AudioProcessor &processor;

	D110Panel panel;
	D110EditorPane editorPane;
	// Карта добавляется ПОСЛЕ ящика и потому лежит поверх него - иначе выехавшая карта
	// пряталась бы за полями редактора.
	D110MemoryCard card;
	// Stacked below the extended editor's own drawer, with its own independent fold state -
	// see D110Keyboard's header comment for why.
	D110Keyboard keyboard;
	// Stacked below the keyboard, third drawer down, same independent-fold treatment.
	D110SequencerPanel sequencerPanel;
	// D-20-style alternate view of the same drawer - see processor.getSequencerRetroMode()
	// and D110Panel::onSequencerModeChanged below. Both are always constructed (cheap,
	// stateless views over the same host/engine); resized() shows exactly one of the two.
	D110SequencerRetroPanel sequencerRetroPanel;
	juce::ComponentBoundsConstrainer constrainer;

	float expansion = 0.0f;        // сглаженное 0..1
	float expansionTarget = 0.0f;  // то, что задал щелчок по ручке
	bool handleHover = false;

	// The editor pane's own live height (see kPaneRefH's comment) - adjustable by dragging
	// the KEYBOARD handle band, which is the boundary directly below it. Persisted through
	// D110AudioProcessor::get/setEditorPaneRefH the same way the WINDOW SIZE/THEME choices
	// are, so a resized drawer stays resized across sessions.
	float editorPaneRefH = kPaneRefH;
	// Set on mouseDown over the KEYBOARD handle band, before it's known whether this turns
	// into a resize-drag or stays a plain click; resolved in mouseUp (toggle) or mouseDrag
	// (resize, once the pointer has moved past a small threshold - see mouseDrag()). Same
	// resizeDragStartY/resizeDragStartRefH pair is reused for all three resize gestures below -
	// only one can ever be in progress at a time, so which resizingXxx flag is set says which.
	bool keyboardHandlePressed = false;
	bool resizingEditorPane = false;
	float resizeDragStartY = 0.0f;
	float resizeDragStartRefH = 0.0f;

	float keyboardExpansion = 1.0f;       // eased 0..1, open by default
	float keyboardExpansionTarget = 1.0f;
	bool keyboardHandleHover = false;

	// The keyboard pane's own live height - same idea as editorPaneRefH, adjustable by
	// dragging the SEQUENCER handle band (the boundary directly below it), the same dual-role
	// trick keyboardHandleBand already uses for the editor pane. Persisted through
	// D110AudioProcessor::get/setKeyboardPaneRefH.
	float keyboardPaneRefH = D110Keyboard::kRefH;
	// Mirrors keyboardHandlePressed/resizingEditorPane above, for this second dual-role band.
	bool sequencerHandlePressed = false;
	bool resizingKeyboardPane = false;

	// Closed by default, unlike the keyboard: a bigger, more specialised drawer, better as
	// an opt-in reveal than something that greets every session already open.
	float sequencerExpansion = 0.0f;
	float sequencerExpansionTarget = 0.0f;
	bool sequencerHandleHover = false;

	// The sequencer pane's own live height - same idea again, adjustable by dragging its own
	// resize grip (sequencerResizeBand(), below the drawer - there's no further drawer there to
	// double up on, unlike the two above). Persisted through
	// D110AudioProcessor::get/setSequencerPaneRefH.
	float sequencerPaneRefH = D110SequencerPanel::kRefH;
	bool sequencerResizeHandlePressed = false;
	bool sequencerResizeHover = false;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110AudioProcessorEditor)
};
