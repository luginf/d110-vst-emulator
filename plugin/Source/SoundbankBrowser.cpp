#include "SoundbankBrowser.h"

#include "PluginProcessor.h"
#include "UiTheme.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace {
constexpr float kRowHeight = 22.0f;
constexpr float kGroupStripWidth = 260.0f; // wide enough for 3 legible columns
constexpr int kGroupColumns = 3;
// Minimum width for one tone-list column before another one is added - wide enough for a
// long real name plus a " (N)" disambiguation suffix without truncating.
constexpr float kListColumnMinWidth = 200.0f;

// Internal Tone Memory slots are labelled plainly "SLOT 1".."SLOT 64" - the exact string the
// TONES tab's own slot list already uses (D110EditorPane::layoutTones(), PluginEditor.cpp,
// "SLOT " + juce::String(toneSlot + 1)) - no decade-skip scheme the way Patch numbering has.
juce::String toneSlotLabel(int slot) { return "SLOT " + juce::String(slot + 1); }
} // namespace

// Runs Database::rescan() (a recursive folder walk + per-file SysEx decode - can genuinely
// take a while for a big personal library) off the message thread so the browser stays
// responsive. Polled by PollTimer rather than driven by a completion callback since
// juce::Thread has no message-thread-safe callback of its own.
//
// Crash fix (Alan's report, 2026-08-28): this used to rescan() the PROCESSOR'S OWN shared
// Database instance directly from this background thread, while paint()/rebuildFilteredList()
// kept reading that same instance's `entries` vector from the message thread the whole time a
// scan was running - a plain unsynchronised concurrent read/write on a std::vector (rescan()
// push_back()-ing, which can reallocate, while paint() iterates it), undefined behaviour that
// crashed reliably on a real-sized library (scan slow enough for at least one repaint to land
// mid-scan). Fixed by giving this thread its OWN private Database instance (same on-disk root,
// but a separate in-memory object) - the shared instance is now only ever touched from the
// message thread, in checkRescanProgress()'s post-scan load().
struct SoundbankBrowser::RescanThread : public juce::Thread {
	RescanThread(const juce::File &dbRoot, juce::File folder)
		: juce::Thread("D-110 Soundbank Rescan"), database(dbRoot), sourceFolder(std::move(folder)) {}

	void run() override {
		const int n = database.rescan(sourceFolder);
		added.store(n, std::memory_order_release);
		finished.store(true, std::memory_order_release);
	}

	d110bank::Database database; // this thread's own - never touched by the message thread
	juce::File sourceFolder;
	std::atomic<bool> finished{ false };
	std::atomic<int> added{ 0 };
};

void SoundbankBrowser::PollTimer::timerCallback() { owner.checkRescanProgress(); }

SoundbankBrowser::SoundbankBrowser(D110AudioProcessor &processorToUse) : processor(processorToUse) {
	// So mouseDown()'s grabKeyboardFocus() (see its own comment) actually has somewhere neutral
	// to park focus, instead of it silently failing/going to the nearest focusable ancestor.
	setWantsKeyboardFocus(true);

	groupKeys.add("ALL");
	groupKeys.add("FAVORITES");
	for (char c = 'A'; c <= 'Z'; ++c) groupKeys.add(juce::String::charToString((juce::juce_wchar) c));
	groupKeys.add("0-9");
	groupKeys.add("_");

	searchBox.setTextToShowWhenEmpty("Click to search all sounds...", juce::Colours::grey);
	searchBox.onTextChange = [this] { rebuildFilteredList(); repaint(); };
	addAndMakeVisible(searchBox);

	// Alan's request, 2026-08-28: a way to get back out of a search - see clearSearch().
	clearSearchButton.onClick = [this] { clearSearch(); };
	addAndMakeVisible(clearSearchButton);

	rescanButton.onClick = [this] { startRescan(); };
	addAndMakeVisible(rescanButton);

	injectButton.onClick = [this] { showInjectMenu(); };
	injectButton.setEnabled(false);
	addAndMakeVisible(injectButton);

	statusLabel.setJustificationType(juce::Justification::centredLeft);
	addAndMakeVisible(statusLabel);

	refresh();
}

SoundbankBrowser::~SoundbankBrowser() {
	pollTimer.stopTimer();
	if (rescanThread != nullptr) rescanThread->waitForThreadToExit(2000);
}

void SoundbankBrowser::refresh() {
	processor.getSoundbankDatabase().load();
	selectedEntry = nullptr;
	injectButton.setEnabled(false);
	rebuildFilteredList();

	const int total = processor.getSoundbankDatabase().size();
	statusLabel.setText(total == 0
	                         ? juce::String("No soundbank scanned yet - choose a folder in the "
	                                        "Utility tab, then RESCAN.")
	                         : juce::String(total) + (total == 1 ? " sound" : " sounds")
	                               + " in the database.",
	                     juce::dontSendNotification);
	repaint();
}

void SoundbankBrowser::rebuildFilteredList() {
	filtered.clear();
	const auto &db = processor.getSoundbankDatabase();
	const auto &favorites = processor.getSoundbankFavorites();
	const auto query = searchBox.getText().trim();
	// Alan's request, 2026-08-28: while actively searching, search EVERY letter, not just
	// whichever one happens to be selected - a query overrides the group filter entirely
	// rather than narrowing it further.
	const bool searching = query.isNotEmpty();

	std::vector<const d110bank::Entry *> pool;
	if (searching || selectedGroup == "ALL") {
		pool.reserve(size_t(db.size()));
		for (const auto &e : db.all()) pool.push_back(&e);
		std::sort(pool.begin(), pool.end(), [](const d110bank::Entry *a, const d110bank::Entry *b) {
			return a->displayName.compareIgnoreCase(b->displayName) < 0;
		});
	} else if (selectedGroup == "FAVORITES") {
		for (const auto &e : db.all())
			if (favorites.contains(e)) pool.push_back(&e);
		std::sort(pool.begin(), pool.end(), [](const d110bank::Entry *a, const d110bank::Entry *b) {
			return a->displayName.compareIgnoreCase(b->displayName) < 0;
		});
	} else {
		pool = db.byLetter(selectedGroup);
	}

	for (const auto *e : pool)
		if (query.isEmpty() || e->displayName.containsIgnoreCase(query)) filtered.push_back(e);

	listScrollRow = 0;
	if (selectedEntry != nullptr
	    && std::find(filtered.begin(), filtered.end(), selectedEntry) == filtered.end()) {
		selectedEntry = nullptr;
		injectButton.setEnabled(false);
	}
}

void SoundbankBrowser::pickGroup(const juce::String &group) {
	selectedGroup = group;
	rebuildFilteredList();
	repaint();
}

void SoundbankBrowser::pickPart(int part) {
	selectedPart = juce::jlimit(0, 7, part);
	repaint();
}

void SoundbankBrowser::clearSearch() {
	// sendTextChangeMessage=true so onTextChange fires immediately (rebuildFilteredList()),
	// same as if Alan had selected the text and pressed Delete.
	searchBox.setText({}, true);
	repaint();
}

void SoundbankBrowser::startRescan() {
	if (rescanThread != nullptr) return; // already running

	const auto folder = D110AudioProcessor::getSoundbankSourceFolder();
	if (folder.isEmpty() || !juce::File(folder).isDirectory()) {
		statusLabel.setText("Choose a Soundbanks folder first (Utility tab).",
		                     juce::dontSendNotification);
		return;
	}

	rescanButton.setEnabled(false);
	statusLabel.setText("Scanning...", juce::dontSendNotification);
	rescanThread =
		std::make_unique<RescanThread>(d110bank::Database::defaultRoot(), juce::File(folder));
	rescanThread->startThread();
	pollTimer.startTimer(150);
}

void SoundbankBrowser::checkRescanProgress() {
	if (rescanThread == nullptr || !rescanThread->finished.load(std::memory_order_acquire)) return;

	const int added = rescanThread->added.load(std::memory_order_acquire);
	rescanThread->waitForThreadToExit(2000);
	rescanThread.reset();
	pollTimer.stopTimer();
	rescanButton.setEnabled(true);

	// The background instance already wrote a fresh index.json - this just re-reads it, on the
	// message thread, the only thread ever allowed to touch the SHARED Database (see
	// RescanThread's own comment above for why).
	processor.getSoundbankDatabase().load();
	rebuildFilteredList();
	// Alan's request, 2026-08-28: report how many sounds a scan found, both new-this-time and
	// the running total.
	const int total = processor.getSoundbankDatabase().size();
	statusLabel.setText("Scan complete: " + juce::String(added) + " new sound"
	                         + (added == 1 ? juce::String() : juce::String("s")) + " added ("
	                         + juce::String(total) + " total).",
	                     juce::dontSendNotification);
	repaint();
}

void SoundbankBrowser::showInjectMenu() {
	if (selectedEntry == nullptr) return;
	showInjectMenuFor(*selectedEntry);
}

void SoundbankBrowser::showInjectMenuFor(const d110bank::Entry &entryRef) {
	// Copied, not referenced/pointed-to: this menu and its confirm dialog are both async and
	// can easily outlive `entryRef`'s own backing storage - a background RESCAN finishing
	// while this menu is still open calls Database::load(), which clears and rebuilds the
	// shared `entries` vector `entryRef` (and anything pointing into it) lives in. Cheap copy,
	// avoids a dangling-pointer crash of exactly the kind already found and fixed once this
	// session (see RescanThread's own comment above).
	const d110bank::Entry entry = entryRef;

	// Live internal Tone Memory names, for a "which slot am I overwriting" menu - same RAM
	// path the TONES tab itself reads (D110EditorPane::toneName(), PluginEditor.cpp), just
	// without that class's own cached snapshot since this component isn't part of it.
	std::vector<juce::uint8> ram(size_t(D110CoreType::kRamSize), 0);
	const bool haveRam = processor.getCore().getRam(ram.data());

	// Captures `ram`/`haveRam` BY VALUE, not by reference: this lambda is itself captured into
	// the async menu callback below and must stay valid after this function returns, long
	// after `ram`'s own stack storage would otherwise be gone.
	auto nameOfSlot = [ram, haveRam](int slot) -> juce::String {
		if (!haveRam) return {};
		const size_t base =
			size_t(D110CoreType::kRamTones) + size_t(slot) * size_t(D110CoreType::kToneMemRecord);
		juce::String name;
		for (int c = 0; c < D110CoreType::kNameChars; ++c) {
			const char ch = char(ram[base + size_t(c)]);
			if (ch < 32 || ch > 126) break;
			name << ch;
		}
		return name.trim();
	};

	juce::PopupMenu menu;
	for (int slot = 0; slot < D110CoreType::kNumTones; ++slot) {
		juce::String label = toneSlotLabel(slot);
		const auto name = nameOfSlot(slot);
		if (name.isNotEmpty()) label << "  " << name;
		menu.addItem(slot + 1, label);
	}

	menu.showMenuAsync(
		juce::PopupMenu::Options().withTargetComponent(injectButton), [this, entry, nameOfSlot](int result) {
			if (result <= 0) return;
			const int slot = result - 1;
			// Alan's request, 2026-08-28: never silently overwrite a slot he's using - confirm
			// first, naming exactly what would be lost.
			const auto currentName = nameOfSlot(slot);
			juce::AlertWindow::showAsync(
				juce::MessageBoxOptions()
					.withIconType(juce::MessageBoxIconType::WarningIcon)
					.withTitle("Overwrite tone?")
					.withMessage("This will overwrite " + toneSlotLabel(slot)
					             + (currentName.isNotEmpty() ? " (currently \"" + currentName + "\")"
					                                          : juce::String())
					             + " with \"" + entry.displayName + "\".")
					.withButton("Inject")
					.withButton("Cancel"),
				[this, entry, slot](int confirmResult) {
					if (confirmResult == 1) injectInto(entry, slot); // see AlertWindow::show()'s
					                                                 // own doc: 2 buttons, first
					                                                 // added returns 1
				});
		});
}

void SoundbankBrowser::injectInto(const d110bank::Entry &entry, int slot) {
	juce::uint8 data[256];
	if (!d110bank::Database::readToneBytes(entry, data)) {
		statusLabel.setText("Could not read \"" + entry.displayName + "\" - its file may be missing.",
		                     juce::dontSendNotification);
		return;
	}
	processor.injectSoundbankTone(slot, data);
	statusLabel.setText("Injected \"" + entry.displayName + "\" into " + toneSlotLabel(slot) + ".",
	                     juce::dontSendNotification);
}

// Right-click on a row - Alan's request, 2026-08-28.
void SoundbankBrowser::showContextMenuFor(const d110bank::Entry &entryRef) {
	selectedEntry = &entryRef;
	injectButton.setEnabled(true);
	repaint();

	auto &favorites = processor.getSoundbankFavorites();
	const bool isFav = favorites.contains(entryRef);
	// Copied, not pointed-to - this menu is async and can outlive entryRef's own backing
	// storage (a background rescan finishing while it's open reloads the shared Database's
	// `entries` vector) - see showInjectMenuFor()'s own comment for the same fix, same reason.
	const d110bank::Entry entry = entryRef;

	juce::PopupMenu menu;
	menu.addItem(isFav ? "Remove from Favorites" : "Add to Favorites", [this, entry] {
		processor.getSoundbankFavorites().toggle(entry);
		// FAVORITES may be the currently shown group, or a just-removed entry may need to
		// disappear from it immediately either way.
		rebuildFilteredList();
		repaint();
	});

	juce::PopupMenu sendMenu;
	for (int p = 0; p < 8; ++p)
		sendMenu.addItem("Part " + juce::String(p + 1),
		                  [this, entry, p] { auditionEntryToPart(entry, p); });
	menu.addSubMenu("Send to", sendMenu);

	menu.addItem("Inject to slot...", [this, entry] { showInjectMenuFor(entry); });

	menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

void SoundbankBrowser::resized() {
	auto area = getLocalBounds().toFloat().reduced(4.0f);

	auto top = area.removeFromTop(26.0f);
	clearSearchButton.setBounds(top.removeFromRight(28.0f).toNearestInt());
	top.removeFromRight(2.0f);
	searchBox.setBounds(top.toNearestInt());
	area.removeFromTop(4.0f);

	// Which Part a double-click auditions into - see the `selectedPart` member's own comment.
	auto partRow = area.removeFromTop(24.0f);
	area.removeFromTop(4.0f);
	partStripArea = partRow;
	partRow.removeFromLeft(50.0f); // room for the "PART" label, painted separately
	partBounds.clear();
	const float partBtnW = partRow.getWidth() / 8.0f;
	for (int p = 0; p < 8; ++p)
		partBounds.push_back(
			{ partRow.getX() + float(p) * partBtnW, partRow.getY(), partBtnW, partRow.getHeight() });

	auto bottom = area.removeFromBottom(28.0f);
	injectButton.setBounds(bottom.removeFromRight(150.0f).reduced(2.0f).toNearestInt());
	rescanButton.setBounds(bottom.removeFromRight(90.0f).reduced(2.0f).toNearestInt());
	statusLabel.setBounds(bottom.toNearestInt());
	area.removeFromBottom(4.0f);

	groupStripArea = area.removeFromLeft(kGroupStripWidth);
	area.removeFromLeft(4.0f);
	listArea = area;

	// Alan's request, 2026-08-28: 29 entries (ALL/A-Z/0-9/_) in one narrow column read too
	// small. Column-major fill (A,B,... fills the first column top-to-bottom before starting
	// the second) reads the same order a single column would, just wrapped - the same layout a
	// phone contacts app uses for an alphabet strip. ALL and FAVORITES (both longer labels than
	// a single letter, and the two most likely to be reached for first) get their own full-width
	// row each, above the 3-column letter grid, rather than being squeezed into one narrow
	// column cell where "FAVORITES (n)" doesn't fit.
	groupBounds.clear();
	auto stripArea = groupStripArea;
	const float wideRowH = 26.0f;
	groupBounds.push_back(stripArea.removeFromTop(wideRowH)); // ALL
	groupBounds.push_back(stripArea.removeFromTop(wideRowH)); // FAVORITES

	const int letterCount = groupKeys.size() - 2; // everything after ALL/FAVORITES
	const int rows = (letterCount + kGroupColumns - 1) / kGroupColumns;
	const float colW = stripArea.getWidth() / float(kGroupColumns);
	const float groupRowH = juce::jmin(28.0f, stripArea.getHeight() / float(juce::jmax(1, rows)));
	for (int i = 0; i < letterCount; ++i) {
		const int col = i / rows;
		const int row = i % rows;
		groupBounds.push_back({ stripArea.getX() + float(col) * colW,
		                         stripArea.getY() + float(row) * groupRowH, colW, groupRowH });
	}
}

void SoundbankBrowser::paint(juce::Graphics &g) {
	const auto &pal = d110ui::palette();
	g.fillAll(pal.panelBg);

	const auto &db = processor.getSoundbankDatabase();

	g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
	g.setColour(pal.label);
	g.drawText("PART", partStripArea.withWidth(46.0f), juce::Justification::centredLeft);
	g.setFont(juce::Font(juce::FontOptions(13.0f)));
	for (int p = 0; p < int(partBounds.size()); ++p) {
		const auto &b = partBounds[size_t(p)];
		const bool selected = p == selectedPart;
		g.setColour(selected ? pal.seqActiveFill : pal.box);
		g.fillRect(b.reduced(1.0f));
		g.setColour(pal.boxBorder);
		g.drawRect(b, 0.5f);
		g.setColour(selected ? pal.seqActiveText : pal.value);
		g.drawText(juce::String(p + 1), b, juce::Justification::centred);
	}

	g.setFont(juce::Font(juce::FontOptions(13.0f)));
	for (int i = 0; i < groupKeys.size() && i < int(groupBounds.size()); ++i) {
		const auto &b = groupBounds[size_t(i)];
		const bool selected = groupKeys[i] == selectedGroup;
		g.setColour(selected ? pal.seqActiveFill : pal.box);
		g.fillRect(b.reduced(1.0f));
		g.setColour(pal.boxBorder);
		g.drawRect(b, 0.5f);
		g.setColour(selected ? pal.seqActiveText : pal.value);
		const int count = groupKeys[i] == "ALL"       ? db.size()
		                 : groupKeys[i] == "FAVORITES" ? processor.getSoundbankFavorites().size()
		                                                : db.countForLetter(groupKeys[i]);
		g.drawText(groupKeys[i] + " (" + juce::String(count) + ")", b.reduced(4.0f, 0.0f),
		           juce::Justification::centredLeft);
	}

	g.setColour(pal.box);
	g.fillRect(listArea);
	g.setColour(pal.boxBorder);
	g.drawRect(listArea, 0.5f);

	// Alan's request, 2026-08-28: a single column left most of a wide window's width empty
	// ("espace perdu"). Flows into as many columns as fit (row-major - left to right, then
	// wrap - so scrolling moves every column together as one grid, per his own question "ça
	// doit pouvoir scroller sur plusieurs colonnes en même temps" - yes, one scroll position
	// for the whole grid, not one per column).
	rowBounds.clear();
	rowEntries.clear();
	listColumns = juce::jmax(1, int(listArea.getWidth() / kListColumnMinWidth));
	const float colW = listArea.getWidth() / float(listColumns);
	const int visibleGridRows = juce::jmax(0, int(listArea.getHeight() / kRowHeight));
	const int totalGridRows = (int(filtered.size()) + listColumns - 1) / listColumns;
	listScrollRow = juce::jlimit(0, juce::jmax(0, totalGridRows - visibleGridRows), listScrollRow);

	g.setFont(juce::Font(juce::FontOptions(14.0f))); // grossi - Alan's request, 2026-08-28
	for (int gridRow = 0; gridRow < visibleGridRows; ++gridRow) {
		for (int col = 0; col < listColumns; ++col) {
			const int index = (listScrollRow + gridRow) * listColumns + col;
			if (index >= int(filtered.size())) continue;
			const auto *entry = filtered[size_t(index)];
			const juce::Rectangle<float> cellRect(listArea.getX() + float(col) * colW,
			                                       listArea.getY() + float(gridRow) * kRowHeight, colW,
			                                       kRowHeight);
			rowBounds.push_back(cellRect);
			rowEntries.push_back(entry);
			const bool selected = entry == selectedEntry;
			if (selected) {
				g.setColour(pal.seqActiveFill);
				g.fillRect(cellRect);
			}
			g.setColour(selected ? pal.seqActiveText : pal.value);
			const bool isFav = processor.getSoundbankFavorites().contains(*entry);
			g.drawText((isFav ? juce::String(juce::CharPointer_UTF8("\xe2\x98\x85 ")) : juce::String())
			               + entry->displayName,
			           cellRect.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
		}
	}

	if (filtered.empty() && db.size() > 0) {
		g.setColour(pal.dim);
		g.drawText("No match.", listArea.reduced(6.0f), juce::Justification::centred);
	}
}

void SoundbankBrowser::mouseDown(const juce::MouseEvent &e) {
	// Alan's report, 2026-08-28: clicking a tone (or a group/part button) left keyboard focus
	// sitting in the search box from an earlier click into it, so typing on the on-screen
	// keyboard's PC-tracker input afterwards silently typed into the search field instead of
	// playing notes. Every click this handler ever sees is already OUTSIDE the search box (a
	// click landing ON it is handled by TextEditor's own mouseDown first, which grabs focus
	// INTO it - that path is untouched, still works normally), so parking focus on this
	// Component itself here is safe and unconditional - it does nothing with the keyboard
	// itself, so there's nowhere for a stray keystroke to go missing into.
	grabKeyboardFocus();

	const auto pos = e.position;

	for (int i = 0; i < int(groupBounds.size()); ++i)
		if (groupBounds[size_t(i)].contains(pos)) {
			pickGroup(groupKeys[i]);
			return;
		}

	for (int i = 0; i < int(partBounds.size()); ++i)
		if (partBounds[size_t(i)].contains(pos)) {
			pickPart(i);
			return;
		}

	for (int i = 0; i < int(rowBounds.size()); ++i) {
		if (!rowBounds[size_t(i)].contains(pos)) continue;
		if (e.mods.isPopupMenu()) {
			showContextMenuFor(*rowEntries[size_t(i)]);
			return;
		}
		selectedEntry = rowEntries[size_t(i)];
		injectButton.setEnabled(true);
		repaint();
		return;
	}
}

// Quick-audition path, Alan's request 2026-08-28: double-click a sound and it goes straight
// into the SELECTED PART's live/working tone (D110AudioProcessor::auditionToneBytes()) -
// immediately audible, exactly "comme quand on pick un tone dans Parts, ça devient
// l'instrument courant" (Alan's own comparison). No Tone Memory slot is spent doing this -
// unlike "Inject to slot...", this writes straight into Tone Temporary, so it's free to
// audition as many sounds as you like without touching any of the 64 stored slots.
void SoundbankBrowser::mouseDoubleClick(const juce::MouseEvent &e) {
	const auto pos = e.position;
	for (int i = 0; i < int(rowBounds.size()); ++i) {
		if (!rowBounds[size_t(i)].contains(pos)) continue;
		const auto *entry = rowEntries[size_t(i)];
		selectedEntry = entry;
		injectButton.setEnabled(true);
		auditionEntryToPart(*entry, selectedPart);
		return;
	}
}

// Corruption fix (Alan's report, 2026-08-28: garbled LCD, wrong sound on double-click).
// Tone Temporary is NOT "the record minus its leading 10-byte name" - it mirrors the record's
// own first 246 bytes UNCHANGED, name included, exactly what the already-proven
// auditionTone(part, slot) (PluginProcessor.cpp) copies (`ram.data() + kRamTones +
// slot*kToneMemRecord`, no offset - only the record's LAST 10 bytes, 246-255, are ever
// excluded). Skipping the first 10 bytes here instead shifted every real parameter after it by
// 10 bytes, corrupting the whole tone.
void SoundbankBrowser::auditionEntryToPart(const d110bank::Entry &entry, int part) {
	juce::uint8 data[256];
	if (!d110bank::Database::readToneBytes(entry, data)) {
		statusLabel.setText("Could not read \"" + entry.displayName + "\" - its file may be missing.",
		                     juce::dontSendNotification);
		repaint();
		return;
	}
	processor.auditionToneBytes(part, data);
	statusLabel.setText("Playing \"" + entry.displayName + "\" on Part " + juce::String(part + 1) + ".",
	                     juce::dontSendNotification);
	repaint();
}

void SoundbankBrowser::mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &wheel) {
	listScrollRow -= juce::roundToInt(wheel.deltaY * 4.0f);
	listScrollRow = juce::jmax(0, listScrollRow);
	repaint();
}
