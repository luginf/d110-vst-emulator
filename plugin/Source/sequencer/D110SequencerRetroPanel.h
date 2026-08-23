#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "D110SequencerEngine.h"
#include "D110SequencerHost.h"

// D-20-style alternate view of the sequencer: a small text LCD plus 9 hardware-style
// buttons (STOP/PLAY/REC, 4 arrows, ENTER, EXIT) instead of D110SequencerPanel's mouse-
// driven grid of controls. Same host contract, same footprint, fully interchangeable
// with D110SequencerPanel - whichever one the owning editor makes visible is a matter of
// D110SequencerHost's own getSequencerRetroMode() flag, not something this class knows or
// cares about itself. Every action here calls the exact same D110SequencerEngine/
// D110SequencerHost methods D110SequencerPanel's mouse controls already do - see
// D110SequencerPanel.cpp for the call sites this mirrors.
//
// Navigation model: a stack of Screens (EXIT pops, a List/Form item's own onEnter/
// onConfirm pushes). HOME is a List Screen like any other (built once by
// buildHomeMenu(), held in homeScreen) - top() returns it whenever the stack is empty,
// so it's never pushed/popped itself, just permanently at the bottom. List/Form/Confirm/
// NameEdit are the only 4 screen kinds - every menu is data (a Screen built by one of the
// buildXyz() methods below), not a bespoke class, so adding a new menu item never needs a
// new paint/input code path. A List row can optionally add a horizontal quick-bar
// (ListItem::quickActions, LEFT/RIGHT-cycled/ENTER-fired) or a live scrub
// (ListItem::onAdjust, LEFT/RIGHT drives it directly) - see ListItem's own comment - used
// by HOME's TRACK/SONG/TRANSPORT/BAR rows to reach fast actions without a submenu hop.
//
// Callback convention, load-bearing for stack safety: a List item's onEnter may freely
// push/pop the navigation stack (that's how menus nest). A Form/Confirm screen's
// onConfirm must NEVER touch the navigation stack itself - only engine()/processor state
// - because the dispatcher (pressEnter) always pops the form/confirm screen itself right
// after calling onConfirm, and holds no reference across that call, but onConfirm mutating
// the stack underneath it would still leave the model inconsistent.
class D110SequencerRetroPanel : public juce::Component, private juce::Timer {
public:
	explicit D110SequencerRetroPanel(D110SequencerHost &);
	~D110SequencerRetroPanel() override;

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	bool keyPressed(const juce::KeyPress &) override;
	void visibilityChanged() override;

	// Same reference footprint as D110SequencerPanel::kRefH - lets both views share one
	// drawer-height calculation in the owning editor/window, regardless of which is
	// visible. The retro content itself is a compact LCD+button cluster, centred within
	// whatever bounds it's given rather than stretched to fill them.
	static constexpr float kRefH = 347.0f;

private:
	void timerCallback() override;
	d110seq::D110SequencerEngine &engine();

	enum class ScreenKind { list, form, confirm, nameEdit };

	// One entry in a list row's horizontal quick-bar (see ListItem::quickActions below).
	struct QuickAction {
		juce::String label;
		std::function<void()> onEnter;
		bool enabled = true;
	};

	// One selectable row in a List screen. label/value are rebuilt fresh from live
	// engine/host state every time the screen is (re)built - see buildItems below - so a
	// List screen never goes stale while it's open.
	//
	// A row is plain by default (ENTER fires onEnter, LEFT/RIGHT do nothing - the original
	// behaviour, unchanged for every pre-existing menu). A row opts into one of two richer
	// behaviours instead, never both:
	//  - quickActions: LEFT/RIGHT cycles a per-row index (persisted in Screen::quickIndex,
	//    since `items` itself is rebuilt fresh every frame), ENTER fires whichever action is
	//    currently dialled. Used by HOME's TRACK/SONG/TRANSPORT rows - lets one row reach
	//    several fast actions (REC/PLAY/MUTE/...) without a submenu hop.
	//  - onAdjust: LEFT/RIGHT calls onAdjust(+-1) directly against live engine state, no
	//    dialled index involved - lets a row that opens a submenu still be scrubbed in place
	//    without opening it. Used by HOME's BAR row (mirroring what HomeField::bar used to do
	//    before HOME became a plain list) and, by the same logic, TEMPO/PRECOUNT/LOOP on their
	//    own submenus (Alan's request, 2026-08-23) - ENTER still opens the full submenu in all
	//    of these for anything the quick scrub doesn't cover (e.g. TEMPO's own faster UP/DOWN
	//    step once inside buildTempoForm()).
	struct ListItem {
		juce::String label;
		juce::String value;
		bool enabled = true;
		std::function<void()> onEnter;
		std::vector<QuickAction> quickActions;
		std::function<void(int)> onAdjust;
	};

	// One editable field in a Form screen. value is heap-allocated (shared_ptr, same
	// idiom D110SequencerPanel::promptForEventList() uses for its own barState) so it
	// stays alive independently of the Screen struct being copied/moved on the
	// navigation stack. UP/DOWN adjusts *value by +-upDownStep (default 1), clamped to
	// [minValue, maxValue]; format renders the raw int for display (e.g. a bar number, a
	// note name, a track label looked up by index, or a unit conversion - see
	// buildTempoForm(), which stores half-BPM units so its own 1 BPM/5.5 BPM steps both
	// land on whole numbers). leftRightStep is 0 by default: on a single-field form that
	// means LEFT/RIGHT adjusts *value by +-upDownStep too, same as UP/DOWN (Alan's request,
	// 2026-08-23 - moving a cursor among one field was always a no-op, so this just gives
	// LEFT/RIGHT something to do); on a multi-field form it instead moves the cursor to the
	// next/previous field, same as always, since something still has to reach the other
	// fields. A field that sets leftRightStep nonzero (only TEMPO's own field does) has
	// LEFT/RIGHT adjust *that* field's value by +-leftRightStep instead of either of the
	// above - only sensible for a single-field form, since it means LEFT/RIGHT no longer
	// reaches any other field while that one has focus.
	struct FormField {
		juce::String label;
		std::shared_ptr<int> value;
		int minValue = 0;
		int maxValue = 999;
		std::function<juce::String(int)> format;
		int upDownStep = 1;
		int leftRightStep = 0;
	};

	// One screen on the navigation stack - see the class comment for the kind-specific
	// fields and the callback convention. buildItems is re-invoked every time a List
	// screen is painted or acted on (never cached), so it always reflects whatever just
	// changed underneath it.
	struct Screen {
		ScreenKind kind = ScreenKind::list;
		juce::String title;
		std::function<std::vector<ListItem>()> buildItems; // list
		std::vector<FormField> fields;                      // form
		juce::String message;                                // confirm question
		std::function<void()> onConfirm;                     // form/confirm
		int cursor = 0;                                       // list row / form field index
		bool confirmYes = false;                              // confirm only
		std::vector<int> quickIndex;                          // list: dialled quickActions index per row
	};

	void pushScreen(Screen s);
	void popScreen();
	// HOME (the permanent base of the stack, never pushed/popped) is a Screen like any
	// other - stack.empty() means "showing HOME", and top() transparently returns the
	// persistent homeScreen member in that case, so every input handler and paintListScreen
	// already written for nested lists works on HOME for free. See buildHomeMenu().
	Screen &top() { return stack.empty() ? homeScreen : stack.back(); }

	// Hardware buttons
	void pressStop();
	void pressPlay();
	void pressRec();
	void pressUp();
	void pressDown();
	void pressLeft();
	void pressRight();
	void pressEnter();
	void pressExit();

	// Customizable D-pad keys (Alan's request, 2026-08-23): the six directional/ENTER/EXIT
	// actions each have one rebindable juce::KeyPress, defaulting to the numeric keypad in
	// the same spatial arrangement as the on-screen cross (7/8/9 above 4/./6, matching
	// EXIT-above-LEFT/ENTER-above-RIGHT) - see defaultRetroKeyBindings() in the .cpp. The
	// plain arrow keys + Return/Backspace stay hard-wired in keyPressed() as a permanent
	// fallback alongside these, exactly as before this feature existed, so rebinding never
	// locks anyone out of the panel. capturingBinding indexes into the same
	// {up,down,left,right,enter,exit} order kBindingCount below uses; -1 means not
	// currently waiting for a key (see buildKeyBindingsMenu()/keyPressed()).
	enum BindingIndex { bindUp, bindDown, bindLeft, bindRight, bindEnter, bindExit, kBindingCount };
	std::array<juce::KeyPress, kBindingCount> keyBindings;
	int capturingBinding = -1;
	static std::array<juce::KeyPress, kBindingCount> defaultRetroKeyBindings();
	// Round-trip through D110SequencerHost::getRetroKeyBindings()/setRetroKeyBindings() - see
	// that method's own comment for the wire format. Called once at construction (decode) and
	// after every rebind/reset (encode, pushed straight to the host so it's never lost even if
	// the app closes without an explicit "save" step for this panel specifically).
	static juce::String encodeKeyBindings(const std::array<juce::KeyPress, kBindingCount> &);
	static std::array<juce::KeyPress, kBindingCount> decodeKeyBindings(const juce::String &);
	void persistKeyBindings() { processor.setRetroKeyBindings(encodeKeyBindings(keyBindings)); }

	// Arms and starts recording on `track` in one gesture (or stops, if it's the one
	// currently recording) - the HOME TRACK row's REC quick action. Unlike the ARM toggle
	// still available under MORE, this never leaves a track armed-but-not-recording.
	void pressTrackRec(int track);

	// Screen builders - one per HOME destination and its own children. Each mirrors the
	// equivalent D110SequencerPanel control 1:1 (see the .cpp for exact call sites). A
	// track parameter of -1 where D110SequencerEngine itself accepts one means "every
	// track", same convention as deleteBars()/copyBars()/transposeBars().
	Screen buildHomeMenu();
	Screen buildTempoSigMetroMenu();
	Screen buildPrecountLoopMenu();
	Screen buildMidiChannelsMenu();
	Screen buildOptionsMenu();
	Screen buildKeyBindingsMenu();
	Screen buildUndoListMenu();
	Screen buildRedoListMenu();
	Screen buildTrackMenu(int track);
	Screen buildQuantizeMenu(int track);
	Screen buildChannelForm(int track);
	Screen buildProgramForm(int track);
	Screen buildCaptureLivePatchConfirm();
	Screen buildClearConfirm(int track);
	Screen buildDeleteBarsForm(int track);
	Screen buildCopyBarsForm(int track);
	Screen buildTransposeForm(int track);
	Screen buildEventList(int track);
	Screen buildEventItemMenu(int track, int eventIndex, int note);
	Screen buildEventPitchForm(int track, int eventIndex, int note);
	Screen buildEventDeleteConfirm(int track, int eventIndex);
	Screen buildTempoForm();
	Screen buildTimeSigMenu();
	Screen buildTimeSigCustomForm();
	Screen buildMetronomeMenu();
	Screen buildMetronomeVolumeMenu();
	Screen buildPrecountForm();
	Screen buildLoopMenu();
	Screen buildBarMenu();
	Screen buildGotoBarForm();
	Screen buildPunchForm();
	Screen buildRecordMenu();
	Screen buildRecordModeMenu();
	Screen buildStepMenu();
	Screen buildStepDurationMenu();
	Screen buildSongMenu();
	Screen buildSongSlotList();
	Screen buildSongCopyMenu();
	Screen buildSongCopyConfirm(int destSlot);
	Screen buildNewSongConfirm();
	Screen buildSnapshotMenu();
	Screen buildSnapshotSlotMenu(int slot);
	Screen buildSnapshotLoadConfirm(int slot);
	Screen buildFileMenu();

	// Character-wheel rename (the nameEdit screen kind) - no physical keyboard, so a name
	// is entered one character at a time: LEFT/RIGHT moves the caret, UP/DOWN cycles the
	// character set at that position, ENTER commits, EXIT cancels. Mirrors
	// D110SequencerPanel::promptForRenameTrack()'s call into
	// D110SequencerEngine::setTrackName().
	void startNameEdit(int track);
	void nameEditAdjust(int delta);
	void nameEditMoveCaret(int delta);
	void nameEditCommit();
	static constexpr int kNameEditLength = 12; // fits the LCD width with room for a caret marker

	// File dialogs - same juce::FileChooser calls as D110SequencerPanel's LOAD/SAVE
	// handlers, just reached from FILE in the menu instead of a click on a LOAD/SAVE
	// button. Not modelled as LCD screens themselves - reinventing a file browser in text
	// would be disproportionate (see the plan).
	void doLoadSong();
	void doSaveSong();
	void doLoadAllSongs();
	void doSaveAllSongs();

	// Rendering - a 20-column character grid, 4 rows by default (see paintLcd()), no
	// dedicated hint/legend row: title takes row 0, every screen kind lays its own content
	// out across the remaining body rows passed to it as `textArea`, all sharing one
	// `charPx` (one glyph's dot-matrix height) so the whole glass reads as a single
	// consistent character grid. OPTIONS > LCD LINES (lcdCompactMode, Alan's request,
	// 2026-08-23) switches this to 2 rows total (title + 1 body row) for roughly double the
	// character size - bodyRows() is the single source of truth every paint*Screen() below
	// windows/scrolls its content against, so both modes share one code path.
	void paintLcd(juce::Graphics &, juce::Rectangle<float> lcdArea);
	void paintButtons(juce::Graphics &);
	juce::String homeStatusText(); // title-row live status ("STOP"/"PLAY BAR 3/8"/...), HOME only
	void paintListScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx);
	void paintFormScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx);
	void paintConfirmScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx);
	void paintNameEditScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx);
	int bodyRows() const { return lcdCompactMode ? 1 : 3; }
	// Places STOP/PLAY/REC and the D-pad within `row` (transport on the left, D-pad anchored
	// to the row's right edge), given a pre-computed cell size and transport-column width -
	// shared by resized()'s landscape and portrait branches, which differ only in how much
	// room they leave around this row for the LCD (see resized()'s own comment). Returns
	// whatever's left in the middle of `row` between the two clusters - only meaningful to
	// the landscape branch, which uses it as lcdBounds; the portrait branch ignores it, since
	// its LCD already has its own separate full-width strip.
	juce::Rectangle<float> layoutTransportAndDpad(juce::Rectangle<float> row, float cell, float transportColW);
	bool lcdCompactMode = false;
	static juce::String defaultTrackLabel(int t);

	D110SequencerHost &processor;

	std::vector<Screen> stack; // empty = HOME (see top())
	Screen homeScreen;          // built once in the constructor via buildHomeMenu()

	juce::String nameEditBuffer;
	int nameEditCaret = 0;
	int nameEditTrack = -1;

	// STOP/PLAY/REC + 4 arrows + ENTER/BACK - laid out once in resized(), painted/hit-
	// tested from these bounds like every other button-grid component in this codebase. The
	// D-pad cluster is always a 3-column, 2-row grid (BACK/UP/ENTER across the top, LEFT/
	// DOWN/RIGHT across the bottom - BACK over LEFT, ENTER over RIGHT, DOWN levelled with
	// LEFT/RIGHT rather than dangling under a blank middle row), and every button (both
	// clusters) is sized off one shared `cell` unit so they stay consistent regardless of
	// aspect ratio - see layoutTransportAndDpad(). Where the LCD goes depends on whether
	// resized() is wider than tall or the reverse (Alan's request, 2026-08-24, after Android
	// surfaced both a landscape and a portrait layout problem the same day): landscape shares
	// one horizontal band between transport (left), the D-pad (right, anchored to the row's
	// right edge), and lcdBounds filling whatever's left in the middle; portrait instead gives
	// lcdBounds its own full-width strip on top, with transport/D-pad sharing the row below
	// it. exitBounds is the BACK button - kept its original member name since pressExit()/
	// EXIT are the same action under the hood, only the on-screen label changed (Alan's
	// request, 2026-08-23).
	juce::Rectangle<float> lcdBounds;
	juce::Rectangle<float> stopBounds, playBounds, recBounds;
	juce::Rectangle<float> upBounds, downBounds, leftBounds, rightBounds, enterBounds, exitBounds;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110SequencerRetroPanel)
};
