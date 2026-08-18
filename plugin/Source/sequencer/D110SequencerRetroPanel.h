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
// onConfirm pushes), plus HOME (stack empty - not itself a Screen). List/Form/Confirm/
// NameEdit are the only 4 screen kinds - every menu is data (a Screen built by one of the
// buildXyz() methods below), not a bespoke class, so adding a new menu item never needs a
// new paint/input code path.
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

	// The HOME screen's own quick-adjust fields - LEFT/RIGHT cycles which one is active,
	// UP/DOWN adjusts it directly, no menu needed for these four most-common tweaks.
	// transport first - the most-used field, and what makes STOP/PLAY/REC reachable from
	// the 6-key set (4 arrows + ENTER + EXIT) alone, no mouse needed - see pressEnter().
	enum class HomeField { transport, track, bar, tempo, slot };
	static constexpr int kHomeFieldCount = 5;

	enum class ScreenKind { list, form, confirm, nameEdit };

	// One selectable row in a List screen. label/value are rebuilt fresh from live
	// engine/host state every time the screen is (re)built - see buildItems below - so a
	// List screen never goes stale while it's open.
	struct ListItem {
		juce::String label;
		juce::String value;
		bool enabled = true;
		std::function<void()> onEnter;
	};

	// One editable field in a Form screen. value is heap-allocated (shared_ptr, same
	// idiom D110SequencerPanel::promptForEventList() uses for its own barState) so it
	// stays alive independently of the Screen struct being copied/moved on the
	// navigation stack. UP/DOWN adjusts *value by +-1, clamped to [minValue, maxValue];
	// format renders the raw int for display (e.g. a bar number, a note name, a track
	// label looked up by index).
	struct FormField {
		juce::String label;
		std::shared_ptr<int> value;
		int minValue = 0;
		int maxValue = 999;
		std::function<juce::String(int)> format;
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
	};

	void pushScreen(Screen s);
	void popScreen();
	Screen &top() { return stack.back(); }

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

	void homeAdjust(int delta);
	void syncHomeTransportChoice(); // snaps homeTransportChoice to the live transport state

	// Screen builders - one per MAIN MENU destination and its own children. Each mirrors
	// the equivalent D110SequencerPanel control 1:1 (see the .cpp for exact call sites).
	// A track parameter of -1 where D110SequencerEngine itself accepts one means "every
	// track", same convention as deleteBars()/copyBars()/transposeBars().
	Screen buildMainMenu();
	Screen buildTrackList();
	Screen buildTrackMenu(int track);
	Screen buildQuantizeMenu(int track);
	Screen buildChannelForm(int track);
	Screen buildProgramForm(int track);
	Screen buildClearConfirm(int track);
	Screen buildDeleteBarsForm(int track);
	Screen buildCopyBarsForm(int track);
	Screen buildTransposeForm(int track);
	Screen buildEventList(int track);
	Screen buildEventItemMenu(int track, int eventIndex, int note);
	Screen buildEventPitchForm(int track, int eventIndex, int note);
	Screen buildEventDeleteConfirm(int track, int eventIndex);
	Screen buildTransportMenu();
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

	// Rendering - fixed 20x4 character grid (see paintLcd()), no dedicated hint/legend
	// row: title takes row 0, every screen kind lays its own content out across the
	// remaining 3 rows passed to it as `textArea`, all sharing one `charPx` (one glyph's
	// dot-matrix height) so the whole glass reads as a single consistent character grid.
	void paintLcd(juce::Graphics &, juce::Rectangle<float> lcdArea);
	void paintButtons(juce::Graphics &);
	void paintHomeScreen(juce::Graphics &g, juce::Rectangle<float> inner, float charPx);
	void paintListScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx);
	void paintFormScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx);
	void paintConfirmScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx);
	void paintNameEditScreen(juce::Graphics &g, juce::Rectangle<float> textArea, float charPx);
	static juce::String defaultTrackLabel(int t);

	D110SequencerHost &processor;

	std::vector<Screen> stack; // empty = HOME
	HomeField homeField = HomeField::transport;
	int homeSelectedTrack = 0;
	// Which action UP/DOWN has dialled up on the transport field, 0=STOP/1=PLAY/2=REC -
	// ENTER fires it (see pressEnter()). Re-synced to the live transport state whenever
	// the field is (re)selected, so it always starts showing what's actually happening.
	int homeTransportChoice = 0;

	juce::String nameEditBuffer;
	int nameEditCaret = 0;
	int nameEditTrack = -1;

	// STOP/PLAY/REC + 4 arrows + ENTER/EXIT - laid out once in resized(), painted/hit-
	// tested from these bounds like every other button-grid component in this codebase.
	// UP/DOWN/LEFT/RIGHT/ENTER form a cross-shaped D-pad (ENTER at its centre) on the
	// right; EXIT is its own button on the left - see resized().
	juce::Rectangle<float> lcdBounds;
	juce::Rectangle<float> stopBounds, playBounds, recBounds;
	juce::Rectangle<float> upBounds, downBounds, leftBounds, rightBounds, enterBounds, exitBounds;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110SequencerRetroPanel)
};
