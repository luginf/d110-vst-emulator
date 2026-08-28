#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "SoundbankDatabase.h"

class D110AudioProcessor;

// The Soundbanks feature's one UI component - shared verbatim between the desktop extended
// editor's own SOUNDBANKS tab and Android's swapped-in Soundbanks view (same reuse pattern as
// D110Keyboard/D110SequencerPanel: constructed straight from a D110AudioProcessor&, no
// separate Host interface, since both apps already talk to the same processor API directly).
// Alan's request, 2026-08-28: browse a personal SysEx tone library by name, grouped
// alphabetically with a count per letter and an overall total (both shown once a scan has
// run); a chosen tone can either be dropped straight into a part for instant, non-destructive
// audition (double-click - see mouseDoubleClick()'s own comment), or written permanently into
// one chosen internal Tone Memory slot ("Inject to slot...", with a confirm step naming what
// it would overwrite - Alan's request, 2026-08-28: never silently clobber a slot he's using).
class SoundbankBrowser : public juce::Component {
public:
	explicit SoundbankBrowser(D110AudioProcessor &processorToUse);
	~SoundbankBrowser() override;

	// Reloads the database's index (cheap - see d110bank::Database::load()) and refreshes the
	// list/counts. Call whenever this becomes visible, in case a rescan ran from elsewhere
	// (e.g. Android and the desktop plugin never share a live process, but the database on
	// disk is the same folder if Alan points both at it).
	void refresh();

	void paint(juce::Graphics &g) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &e) override;
	void mouseDoubleClick(const juce::MouseEvent &e) override;
	void mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) override;

private:
	void rebuildFilteredList();
	void startRescan();
	// Both "Inject to slot..." paths (the button, for `selectedEntry`, and the right-click
	// menu, for whichever row was clicked) end up here.
	void showInjectMenu();
	void showInjectMenuFor(const d110bank::Entry &entry);
	// Writes `entry`'s 256 bytes permanently into internal Tone Memory slot `slot` (via
	// D110AudioProcessor::injectSoundbankTone()) and updates the status line - what "Inject to
	// slot..." does once its confirmation step (showInjectMenu()'s own comment) is accepted.
	void injectInto(const d110bank::Entry &entry, int slot);
	// Shared by double-click (the currently PART-selector part) and the right-click "Send to"
	// submenu (any part, explicitly chosen) - see mouseDoubleClick()'s own comment for why this
	// writes straight into Tone Temporary rather than a stored slot.
	void auditionEntryToPart(const d110bank::Entry &entry, int part);
	// Right-click on a row - Alan's request, 2026-08-28: Favorites toggle, "Send to Part N"
	// (1-8, quick-audition without changing the PART selector), "Inject to slot..." (same as
	// the button).
	void showContextMenuFor(const d110bank::Entry &entry);
	void pickGroup(const juce::String &group);
	void pickPart(int part);
	void clearSearch();

	D110AudioProcessor &processor;

	juce::TextEditor searchBox;
	juce::TextButton clearSearchButton{ "X" };
	juce::TextButton rescanButton{ "RESCAN" };
	juce::TextButton injectButton{ "INJECT TO SLOT..." };
	juce::Label statusLabel;

	// "ALL", "FAVORITES", "A".."Z", "0-9", "_" - fixed browse order. A non-empty search query
	// overrides `selectedGroup` entirely (searches every letter, not just the selected one -
	// Alan's request, 2026-08-28) - see rebuildFilteredList().
	juce::StringArray groupKeys;
	juce::String selectedGroup = "ALL";
	std::vector<const d110bank::Entry *> filtered;
	const d110bank::Entry *selectedEntry = nullptr;

	// Which Part a double-click auditions into (0-7) - explicit and remembered rather than
	// guessed from e.g. the keyboard's MIDI channel, since there's no reliable channel->Part
	// mapping available here (the real mapping lives in the firmware's own live SYSTEM area,
	// not any fixed formula - see D110AudioProcessor::selectTimbreForPart()'s own comment).
	// Mirrors exactly what picking a tone for a Part in the PARTS tab already asks for: which
	// part.
	int selectedPart = 0;
	std::vector<juce::Rectangle<float>> partBounds; // 8, one per Part button

	juce::Rectangle<float> listArea, groupStripArea, partStripArea;
	std::vector<juce::Rectangle<float>> groupBounds; // parallel to groupKeys

	// The tone list flows into as many columns as listArea's width allows (Alan's request,
	// 2026-08-28: a single column wasted most of a wide window's width) - row-major (left to
	// right, then wrap), so scrolling moves every column together as one grid, the same way
	// text wraps. `rowBounds`/`rowEntries` are parallel, one pair per visible CELL (not
	// necessarily one per `filtered` row anymore) - mouseDown()/mouseDoubleClick() index
	// `rowEntries` directly rather than re-deriving a position from `listScrollRow`.
	int listColumns = 1;
	int listScrollRow = 0; // scroll position in GRID ROWS, not raw `filtered` indices
	std::vector<juce::Rectangle<float>> rowBounds;
	std::vector<const d110bank::Entry *> rowEntries;

	// A rescan walks an arbitrary folder and can genuinely take a while for a big library -
	// runs off the message thread so the UI stays responsive; polled by a timer rather than a
	// direct callback since juce::Thread callbacks don't run on the message thread.
	struct RescanThread;
	std::unique_ptr<RescanThread> rescanThread;
	class PollTimer : public juce::Timer {
	public:
		explicit PollTimer(SoundbankBrowser &ownerToUse) : owner(ownerToUse) {}
		void timerCallback() override;

	private:
		SoundbankBrowser &owner;
	};
	PollTimer pollTimer{ *this };
	void checkRescanProgress();

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SoundbankBrowser)
};
