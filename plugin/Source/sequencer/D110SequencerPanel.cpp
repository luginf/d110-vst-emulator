#include "D110SequencerPanel.h"

#include <cmath>

#include "../PluginProcessor.h"
#include "../UiTheme.h"

using d110seq::D110SequencerEngine;

namespace {
constexpr int kNumTracks = D110SequencerEngine::kNumTracks;

// enabled=false dims the button (used only by UNDO, greyed out while its stack is empty) -
// distinct from active, which picks the on/off colour pair rather than an alpha.
void paintToggleButton(juce::Graphics &g, juce::Rectangle<float> b, const juce::String &label, bool active,
                        bool enabled = true) {
	const auto &pal = d110ui::palette();
	auto fill = active ? pal.seqActiveFill : pal.seqInactiveFill;
	auto text = active ? pal.seqActiveText : pal.seqInactiveText;
	if (!enabled) {
		fill = fill.withAlpha(0.35f);
		text = text.withAlpha(0.35f);
	}
	g.setColour(fill);
	g.fillRect(b.reduced(2.0f));
	g.setColour(text);
	g.setFont(juce::FontOptions(juce::jlimit(8.0f, 13.0f, b.getHeight() * 0.5f)));
	g.drawText(label, b, juce::Justification::centred);
}

struct TimeSig {
	int num, den;
};
// Shared by the left-click cycle and the right-click pick-from-list menu.
const std::array<TimeSig, 6> &timeSigPresets() {
	static const std::array<TimeSig, 6> presets{{{4, 4}, {3, 4}, {6, 8}, {2, 4}, {5, 4}, {7, 8}}};
	return presets;
}

juce::String recordModeLabel(d110seq::RecordMode mode) {
	switch (mode) {
		case d110seq::RecordMode::overdub: return "REC: OVERDUB";
		case d110seq::RecordMode::replaceRange: return "REC: REPLACE";
		case d110seq::RecordMode::replaceToEnd: return "REC: REPLACE+END";
	}
	return {};
}

juce::String loopModeLabel(d110seq::LoopMode mode) {
	switch (mode) {
		case d110seq::LoopMode::off: return "LOOP OFF";
		case d110seq::LoopMode::bar: return "LOOP: BAR";
		case d110seq::LoopMode::punch: return "LOOP: PUNCH";
	}
	return {};
}

// QuantizeGrid::off is not a valid step duration (see D110SequencerEngine::setStepDuration()),
// so it's left out of both this list and its label below. Largest to smallest, whole note first -
// the dotted modifier (see the DOT button) covers the in-between durations (dotted half = 3
// beats, ...) without needing a dotted entry for every base grid here.
const std::array<d110seq::QuantizeGrid, 8> &stepGridPresets() {
	using d110seq::QuantizeGrid;
	static const std::array<QuantizeGrid, 8> presets{
		{QuantizeGrid::whole, QuantizeGrid::half, QuantizeGrid::quarter, QuantizeGrid::eighth,
	     QuantizeGrid::sixteenth, QuantizeGrid::eighthTriplet, QuantizeGrid::sixteenthTriplet,
	     QuantizeGrid::thirtySecond}};
	return presets;
}

juce::String stepDurationLabel(d110seq::QuantizeGrid grid) {
	using d110seq::QuantizeGrid;
	switch (grid) {
		case QuantizeGrid::whole: return "STEP 1/1";
		case QuantizeGrid::half: return "STEP 1/2";
		case QuantizeGrid::quarter: return "STEP 1/4";
		case QuantizeGrid::eighth: return "STEP 1/8";
		case QuantizeGrid::sixteenth: return "STEP 1/16";
		case QuantizeGrid::eighthTriplet: return "STEP 1/8 T";
		case QuantizeGrid::sixteenthTriplet: return "STEP 1/16 T";
		case QuantizeGrid::thirtySecond: return "STEP 1/32";
		case QuantizeGrid::off: default: return "STEP 1/4";
	}
}
} // namespace

D110SequencerPanel::D110SequencerPanel(D110AudioProcessor &p) : processor(p) { startTimerHz(15); }

D110SequencerPanel::~D110SequencerPanel() { stopTimer(); }

d110seq::D110SequencerEngine &D110SequencerPanel::engine() { return processor.getSequencer(); }

void D110SequencerPanel::timerCallback() {
	auto &eng = engine();
	if (eng.isPrecounting()) {
		// Edge-detect a new precount beat (positionBeats itself is frozen throughout precount,
		// so it can't drive this the way the normal scrolling LED does) and light the downbeat
		// LED for a short, fixed window - a flash on every beat of the count-in, audio or not,
		// per Alan's own ask (2026-08-07): something to follow along with even without sound.
		const int elapsed = eng.precountBeatsElapsed();
		if (elapsed != lastPrecountBeatsElapsed) {
			lastPrecountBeatsElapsed = elapsed;
			constexpr int kFlashMs = 120;
			precountFlashUntilMs = juce::Time::getMillisecondCounter() + kFlashMs;
		}
	} else {
		lastPrecountBeatsElapsed = -1;
	}
	// The only other things that change on their own, without a click on this panel, are the bar
	// readout (and the transport buttons' own on/off look) while the transport is rolling, and
	// the step-recording readout, which advances from notes played on an external MIDI
	// controller or the on-screen keyboard drawer - neither of which is a click on THIS panel,
	// so nothing else would ever trigger the repaint that shows it.
	if (eng.isPlaying() || eng.isStepRecording()) repaint();
}

void D110SequencerPanel::cycleTimeSignature() {
	const auto &presets = timeSigPresets();
	auto &eng = engine();
	size_t idx = 0;
	for (size_t i = 0; i < presets.size(); ++i)
		if (presets[i].num == eng.getTimeSigNumerator() && presets[i].den == eng.getTimeSigDenominator()) {
			idx = i;
			break;
		}
	idx = (idx + 1) % presets.size();
	eng.setTimeSignature(presets[idx].num, presets[idx].den);
}

// Right-click: pick any of the presets directly instead of stepping through them one at a
// time - handy once you're past the first couple of clicks of cycleTimeSignature().
void D110SequencerPanel::showTimeSignatureMenu() {
	const auto &presets = timeSigPresets();
	auto &eng = engine();

	juce::PopupMenu m;
	for (size_t i = 0; i < presets.size(); ++i) {
		const bool current =
			presets[i].num == eng.getTimeSigNumerator() && presets[i].den == eng.getTimeSigDenominator();
		m.addItem(int(i) + 1, juce::String(presets[i].num) + "/" + juce::String(presets[i].den), true, current);
	}

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int result) {
		const auto &presets = timeSigPresets();
		if (result < 1 || result > int(presets.size())) return;
		engine().setTimeSignature(presets[size_t(result - 1)].num, presets[size_t(result - 1)].den);
		repaint();
	});
}

void D110SequencerPanel::cycleStepDuration() {
	const auto &presets = stepGridPresets();
	auto &eng = engine();
	size_t idx = 0;
	for (size_t i = 0; i < presets.size(); ++i)
		if (presets[i] == eng.getStepDuration()) {
			idx = i;
			break;
		}
	idx = (idx + 1) % presets.size();
	eng.setStepDuration(presets[idx]);
}

// Right-click the step-duration readout: pick any grid directly, same convention as
// showTimeSignatureMenu().
void D110SequencerPanel::showStepDurationMenu() {
	const auto &presets = stepGridPresets();
	auto &eng = engine();

	juce::PopupMenu m;
	for (size_t i = 0; i < presets.size(); ++i)
		m.addItem(int(i) + 1, stepDurationLabel(presets[i]), true, presets[i] == eng.getStepDuration());

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int result) {
		const auto &presets = stepGridPresets();
		if (result < 1 || result > int(presets.size())) return;
		engine().setStepDuration(presets[size_t(result - 1)]);
		repaint();
	});
}

void D110SequencerPanel::cycleRecordMode() {
	using d110seq::RecordMode;
	auto &eng = engine();
	switch (eng.getRecordMode()) {
		case RecordMode::overdub: eng.setRecordMode(RecordMode::replaceRange); break;
		case RecordMode::replaceRange: eng.setRecordMode(RecordMode::replaceToEnd); break;
		case RecordMode::replaceToEnd: eng.setRecordMode(RecordMode::overdub); break;
	}
}

void D110SequencerPanel::showRecordModeMenu() {
	using d110seq::RecordMode;
	auto &eng = engine();
	const auto current = eng.getRecordMode();

	juce::PopupMenu m;
	m.addItem(1, "Overdub - adds to what's already there", true, current == RecordMode::overdub);
	m.addItem(2, "Replace - erases only the punched span", true, current == RecordMode::replaceRange);
	m.addItem(3, "Replace to end - erases from the punch-in point onward", true,
	          current == RecordMode::replaceToEnd);

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int result) {
		using d110seq::RecordMode;
		switch (result) {
			case 1: engine().setRecordMode(RecordMode::overdub); break;
			case 2: engine().setRecordMode(RecordMode::replaceRange); break;
			case 3: engine().setRecordMode(RecordMode::replaceToEnd); break;
			default: return;
		}
		repaint();
	});
}

void D110SequencerPanel::showMetronomeModeMenu() {
	using d110seq::MetronomeMode;
	auto &eng = engine();
	const auto current = eng.getMetronomeMode();

	juce::PopupMenu m;
	m.addItem(1, "Visual only - LED strip, no click", true, current == MetronomeMode::visualOnly);
	m.addItem(2, "Audio only - click, no LED strip", true, current == MetronomeMode::audioOnly);
	m.addItem(3, "Both", true, current == MetronomeMode::both);
	m.addSeparator();
	m.addItem(4, "Use rhythm channel (MIDI ch. 10) instead of the click sound", true,
	          eng.getMetronomeUseChannel10());
	m.addItem(5, "Only while recording, not during plain playback", true,
	          eng.getMetronomeRecordOnly());

	// Result codes 10-15 index into this same array on the way back out.
	static constexpr float kVolumePresets[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f };
	static constexpr const char *kVolumeLabels[] = { "25%", "50%", "75%", "100%", "125%", "150%" };
	const float currentVolume = eng.getMetronomeVolume();
	juce::PopupMenu volumeMenu;
	for (int i = 0; i < 6; ++i)
		volumeMenu.addItem(10 + i, kVolumeLabels[i], true,
		                    std::abs(currentVolume - kVolumePresets[i]) < 0.01f);
	m.addSubMenu("Volume", volumeMenu);

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int result) {
		using d110seq::MetronomeMode;
		auto &eng = engine();
		if (result >= 10 && result < 16) {
			eng.setMetronomeVolume(kVolumePresets[result - 10]);
			repaint();
			return;
		}
		switch (result) {
			case 1: eng.setMetronomeMode(MetronomeMode::visualOnly); break;
			case 2: eng.setMetronomeMode(MetronomeMode::audioOnly); break;
			case 3: eng.setMetronomeMode(MetronomeMode::both); break;
			case 4: eng.setMetronomeUseChannel10(!eng.getMetronomeUseChannel10()); break;
			case 5: eng.setMetronomeRecordOnly(!eng.getMetronomeRecordOnly()); break;
			default: return;
		}
		repaint();
	});
}

// Plain click loads/saves just the current song (.mid) - see mouseDown(). Right-click is the
// shortcut for all 4 slots at once (.d110songs), so there's exactly one action per gesture
// rather than a one-item menu restating what the plain click already does.
void D110SequencerPanel::showLoadMenu() {
	auto *chooser = new juce::FileChooser("Load all 4 sequencer songs", juce::File(), "*.d110songs");
	chooser->launchAsync(
		juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
		[this, chooser](const juce::FileChooser &fc) {
			const auto file = fc.getResult();
			if (file != juce::File()) processor.importSequencerSongs(file);
			delete chooser;
			repaint();
		});
}

void D110SequencerPanel::showSaveMenu() {
	auto *chooser = new juce::FileChooser("Save all 4 sequencer songs", juce::File(), "*.d110songs");
	chooser->launchAsync(
		juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
			| juce::FileBrowserComponent::warnAboutOverwriting,
		[this, chooser](const juce::FileChooser &fc) {
			auto file = fc.getResult();
			if (file != juce::File()) {
				if (!file.hasFileExtension("d110songs")) file = file.withFileExtension("d110songs");
				processor.exportSequencerSongs(file);
			}
			delete chooser;
		});
}

void D110SequencerPanel::showQuantizeMenu(int track) {
	using d110seq::QuantizeGrid;
	const auto current = engine().getTrackQuantize(track);

	juce::PopupMenu m;
	m.addItem(1, "Quantize: Off", true, current == QuantizeGrid::off);
	m.addItem(2, "Quantize: 1/4", true, current == QuantizeGrid::quarter);
	m.addItem(3, "Quantize: 1/8", true, current == QuantizeGrid::eighth);
	m.addItem(4, "Quantize: 1/16", true, current == QuantizeGrid::sixteenth);
	m.addItem(5, "Quantize: 1/8 triplet", true, current == QuantizeGrid::eighthTriplet);
	m.addItem(6, "Quantize: 1/16 triplet", true, current == QuantizeGrid::sixteenthTriplet);
	m.addItem(7, "Quantize: 1/32", true, current == QuantizeGrid::thirtySecond);
	m.addSeparator();
	m.addItem(8, "Clear track", engine().trackHasEvents(track));
	m.addItem(9, "Delete bar(s) on this track...", engine().getBarCount() >= 1);
	m.addItem(10, "Copy bar(s) on this track to...", engine().trackHasEvents(track));
	m.addItem(11, "Transpose bar(s) on this track...", engine().trackHasEvents(track));

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this, track](int result) {
		using d110seq::QuantizeGrid;
		if (result == 8) { confirmClearTrack(track); return; }
		if (result == 9) { promptForDeleteBars(track); return; }
		if (result == 10) { promptForCopyBars(track); return; }
		if (result == 11) { promptForTransposeBars(track); return; }
		QuantizeGrid grid;
		switch (result) {
			case 1: grid = QuantizeGrid::off; break;
			case 2: grid = QuantizeGrid::quarter; break;
			case 3: grid = QuantizeGrid::eighth; break;
			case 4: grid = QuantizeGrid::sixteenth; break;
			case 5: grid = QuantizeGrid::eighthTriplet; break;
			case 6: grid = QuantizeGrid::sixteenthTriplet; break;
			case 7: grid = QuantizeGrid::thirtySecond; break;
			default: return;
		}
		engine().pushUndoSnapshot();
		engine().quantizeTrack(track, grid);
		repaint();
	});
}

void D110SequencerPanel::confirmClearTrack(int track) {
	const bool isRhythm = track == d110seq::D110SequencerEngine::kRhythmTrack;
	const juce::String name = isRhythm ? "RHYTHM" : ("PART " + juce::String(track + 1));
	auto *aw = new juce::AlertWindow(
		"Clear this track?",
		"This clears every recorded event on " + name
			+ ". Mute/solo/quantize and every other track are kept. UNDO can bring it back.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Clear", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, track](int result) {
		if (result == 1) {
			engine().pushUndoSnapshot();
			engine().clearTrack(track);
			repaint();
		}
		delete aw;
	}));
}

void D110SequencerPanel::cycleLoopMode() {
	using d110seq::LoopMode;
	auto &eng = engine();
	switch (eng.getLoopMode()) {
		case LoopMode::off: eng.setLoopMode(LoopMode::bar); break;
		case LoopMode::bar: eng.setLoopMode(LoopMode::punch); break;
		case LoopMode::punch: eng.setLoopMode(LoopMode::off); break;
	}
}

// Right-click on the BAR readout: jump to an entered bar, or set the punch in/out range
// that LoopMode::punch (and, while it's active, recording) uses.
void D110SequencerPanel::showBarMenu() {
	auto &eng = engine();
	juce::PopupMenu m;
	m.addItem(1, "Go to bar...");
	m.addSeparator();
	m.addItem(2, "Set punch in here (bar " + juce::String(eng.getCurrentBar()) + ")");
	m.addItem(3, "Set punch out here (bar " + juce::String(eng.getCurrentBar()) + ")");
	m.addItem(4, "Set punch in/out...");
	m.addSeparator();
	m.addItem(5, "Delete bar(s) (all tracks)...");
	m.addItem(6, "Copy bar(s) to... (all tracks)");
	m.addItem(7, "Transpose bar(s) (all tracks)...");

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int result) {
		auto &e = engine();
		switch (result) {
			case 1: promptForBar(); return;
			case 2: e.setPunchIn(e.getCurrentBar()); break;
			case 3: e.setPunchOut(e.getCurrentBar()); break;
			case 4: promptForPunchRange(); return;
			case 5: promptForDeleteBars(-1); return;
			case 6: promptForCopyBars(-1); return;
			case 7: promptForTransposeBars(-1); return;
			default: return;
		}
		repaint();
	});
}

void D110SequencerPanel::promptForBar() {
	auto &eng = engine();
	auto *aw = new juce::AlertWindow("Go to bar", "Enter the bar number to jump to.",
	                                  juce::AlertWindow::NoIcon);
	aw->addTextEditor("bar", juce::String(eng.getCurrentBar()), "Bar:");
	aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw](int result) {
		if (result == 1) {
			const int bar = aw->getTextEditorContents("bar").getIntValue();
			if (bar >= 1) engine().gotoBar(bar);
			repaint();
		}
		delete aw;
	}));
}

void D110SequencerPanel::promptForPunchRange() {
	auto &eng = engine();
	auto *aw = new juce::AlertWindow(
		"Punch in / out",
		"Used by LOOP: PUNCH for playback looping, and to record only within this range "
		"while that loop mode is active.",
		juce::AlertWindow::NoIcon);
	aw->addTextEditor("in", juce::String(eng.getPunchIn()), "Punch in bar:");
	aw->addTextEditor("out", juce::String(eng.getPunchOut()), "Punch out bar:");
	aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw](int result) {
		if (result == 1) {
			auto &e = engine();
			const int in = aw->getTextEditorContents("in").getIntValue();
			const int out = aw->getTextEditorContents("out").getIntValue();
			if (in >= 1 && out >= 1) e.setPunchRange(in, out);
			repaint();
		}
		delete aw;
	}));
}

// Destructive (see D110SequencerEngine::deleteBars()) and unrecoverable, so it confirms with a
// warning icon the same way confirmClearTrack() does, rather than acting straight from the menu.
void D110SequencerPanel::promptForDeleteBars(int track) {
	auto &eng = engine();
	const bool isRhythm = track == d110seq::D110SequencerEngine::kRhythmTrack;
	const juce::String scope =
		track < 0 ? "every track" : (isRhythm ? "RHYTHM" : ("PART " + juce::String(track + 1)));
	auto *aw = new juce::AlertWindow(
		"Delete bar(s)",
		"Removes the given bar range from " + scope
			+ " and closes the gap by shifting everything after it earlier. UNDO can bring it back.",
		juce::AlertWindow::WarningIcon);
	aw->addTextEditor("from", juce::String(eng.getCurrentBar()), "From bar:");
	aw->addTextEditor("to", juce::String(eng.getCurrentBar()), "To bar:");
	aw->addButton("Delete", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, track](int result) {
		if (result == 1) {
			const int from = aw->getTextEditorContents("from").getIntValue();
			const int to = aw->getTextEditorContents("to").getIntValue();
			if (from >= 1 && to >= from) {
				engine().pushUndoSnapshot();
				engine().deleteBars(track, from, to);
			}
			repaint();
		}
		delete aw;
	}));
}

// Inserts (see D110SequencerEngine::copyBars()) - pushes whatever's already at/after the
// destination later rather than overwriting it, so this is destructive only in the sense that
// it changes bar numbers from the destination onward; still confirmed with a warning icon since
// that reflow can't be undone either.
void D110SequencerPanel::promptForCopyBars(int track) {
	auto &eng = engine();
	const bool isRhythm = track == d110seq::D110SequencerEngine::kRhythmTrack;
	const juce::String scope =
		track < 0 ? "every track, independently" : (isRhythm ? "RHYTHM" : ("PART " + juce::String(track + 1)));
	auto *aw = new juce::AlertWindow(
		"Copy bar(s)",
		"Copies the given bar range and inserts it at the destination bar on " + scope
			+ ", pushing anything already there later to make room. UNDO can bring it back.",
		juce::AlertWindow::WarningIcon);
	aw->addTextEditor("from", juce::String(eng.getCurrentBar()), "From bar:");
	aw->addTextEditor("to", juce::String(eng.getCurrentBar()), "To bar:");
	aw->addTextEditor("dest", juce::String(eng.getBarCount() + 1), "Destination bar:");
	// Only a single track's worth of notes can go to a chosen OTHER track - copying "every
	// track" always keeps each one going to its own same track, so they stay aligned (see
	// copyBars()'s own comment on srcTrack == destTrack == -1).
	if (track >= 0) {
		juce::StringArray names;
		for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t)
			names.add(t == d110seq::D110SequencerEngine::kRhythmTrack ? "RHYTHM" : ("PART " + juce::String(t + 1)));
		aw->addComboBox("destTrack", names, "Destination track:");
		aw->getComboBoxComponent("destTrack")->setSelectedItemIndex(track, juce::dontSendNotification);
	}
	aw->addButton("Copy", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, track](int result) {
		if (result == 1) {
			const int from = aw->getTextEditorContents("from").getIntValue();
			const int to = aw->getTextEditorContents("to").getIntValue();
			const int dest = aw->getTextEditorContents("dest").getIntValue();
			const int destTrack = track >= 0 ? aw->getComboBoxComponent("destTrack")->getSelectedItemIndex() : -1;
			if (from >= 1 && to >= from && dest >= 1) {
				engine().pushUndoSnapshot();
				engine().copyBars(track, destTrack, from, to, dest);
			}
			repaint();
		}
		delete aw;
	}));
}

// Transposes in place - see D110SequencerEngine::transposeBars(). Not confirmed with a warning
// dialog the way delete/copy bars are (nothing is removed or reflowed, only pitches shift), but
// still checkpointed for UNDO since it can touch a lot of notes at once.
void D110SequencerPanel::promptForTransposeBars(int track) {
	auto &eng = engine();
	const bool isRhythm = track == d110seq::D110SequencerEngine::kRhythmTrack;
	const juce::String scope =
		track < 0 ? "every track, independently" : (isRhythm ? "RHYTHM" : ("PART " + juce::String(track + 1)));
	auto *aw = new juce::AlertWindow(
		"Transpose bar(s)",
		"Shifts the pitch of every note in the given bar range on " + scope
			+ " by the given number of semitones (negative to transpose down).",
		juce::AlertWindow::NoIcon);
	aw->addTextEditor("from", juce::String(eng.getCurrentBar()), "From bar:");
	aw->addTextEditor("to", juce::String(eng.getCurrentBar()), "To bar:");
	aw->addTextEditor("semitones", "0", "Semitones:");
	aw->addButton("Transpose", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, track](int result) {
		if (result == 1) {
			const int from = aw->getTextEditorContents("from").getIntValue();
			const int to = aw->getTextEditorContents("to").getIntValue();
			const int semitones = aw->getTextEditorContents("semitones").getIntValue();
			if (from >= 1 && to >= from && semitones != 0) {
				engine().pushUndoSnapshot();
				engine().transposeBars(track, from, to, semitones);
			}
			repaint();
		}
		delete aw;
	}));
}

// Right-click any of the 4 slot buttons: offers copying the CURRENT song into one of the
// other 3, regardless of which slot's button was actually clicked - there's only one sensible
// source (whichever song is live right now), so the menu doesn't need to distinguish.
void D110SequencerPanel::showCopySongMenu() {
	auto &eng = engine();
	const int current = eng.getCurrentSongSlot();

	juce::PopupMenu m;
	for (int s = 0; s < D110SequencerEngine::kNumSongSlots; ++s) {
		if (s == current) continue;
		juce::String label = "Copy this song to Slot " + juce::String(s + 1);
		if (eng.songSlotHasContent(s)) label += " (overwrites it)";
		m.addItem(s + 1, label);
	}

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int result) {
		if (result < 1) return;
		confirmCopySongTo(result - 1);
	});
}

// Destructive to destSlot's existing content (see D110SequencerEngine::copyCurrentSongTo()), so
// it always confirms first, the same as confirmClearTrack()/confirmNewSong() - except when
// destSlot is empty, where there's nothing to lose and confirming would just be a needless click.
void D110SequencerPanel::confirmCopySongTo(int destSlot) {
	auto &eng = engine();
	const int current = eng.getCurrentSongSlot();
	if (!eng.songSlotHasContent(destSlot)) {
		eng.pushUndoSnapshot();
		eng.copyCurrentSongTo(destSlot);
		repaint();
		return;
	}
	auto *aw = new juce::AlertWindow(
		"Copy song to Slot " + juce::String(destSlot + 1) + "?",
		"This overwrites Slot " + juce::String(destSlot + 1) + " with a copy of Slot "
			+ juce::String(current + 1) + " (the current song). UNDO can bring it back.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Copy", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, destSlot](int result) {
		if (result == 1) {
			engine().pushUndoSnapshot();
			engine().copyCurrentSongTo(destSlot);
			repaint();
		}
		delete aw;
	}));
}

// NEW clears the current song slot's tracks - destructive and unrecoverable, so it always
// confirms first (Alan's own explicit ask, to avoid an accidental wipe).
void D110SequencerPanel::confirmNewSong() {
	const int slot = engine().getCurrentSongSlot();
	auto *aw = new juce::AlertWindow(
		"Clear this song?",
		"This clears every track's recorded MIDI in song " + juce::String(slot + 1)
			+ ". Tempo, time signature and other settings are kept. UNDO can bring it back.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Clear", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw](int result) {
		if (result == 1) {
			engine().pushUndoSnapshot();
			engine().newSong();
			repaint();
		}
		delete aw;
	}));
}

void D110SequencerPanel::layout() {
	auto area = getLocalBounds().toFloat();
	if (area.getWidth() < 1.0f || area.getHeight() < 1.0f) return;

	auto transport = area.removeFromTop(juce::jmin(40.0f, area.getHeight() * 0.22f));
	const float tw = transport.getWidth();
	auto colT = [&](float frac, float widthFrac) {
		return juce::Rectangle<float>(transport.getX() + tw * frac, transport.getY(),
		                               tw * widthFrac - 4.0f, transport.getHeight());
	};
	stopBounds = colT(0.000f, 0.060f);
	playBounds = colT(0.060f, 0.060f);
	recBounds = colT(0.120f, 0.060f);
	tempoBounds = colT(0.185f, 0.120f);
	timeSigBounds = colT(0.310f, 0.090f);
	metronomeBounds = colT(0.405f, 0.120f);
	precountBounds = colT(0.530f, 0.130f);
	loopBounds = colT(0.665f, 0.090f);
	barPrevBounds = colT(0.760f, 0.045f);
	barReadoutBounds = colT(0.808f, 0.148f);
	barNextBounds = colT(0.960f, 0.040f);

	area.removeFromTop(juce::jmax(2.0f, area.getHeight() * 0.015f));

	// Visual metronome LED strip - one LED per metronome click in the bar, drawn only when
	// METRO is on (see paint()). Thin, non-interactive, sits right under the transport row.
	metroLedBounds = area.removeFromTop(juce::jmin(14.0f, area.getHeight() * 0.09f)).reduced(2.0f, 0.0f);
	area.removeFromTop(juce::jmax(2.0f, area.getHeight() * 0.015f));

	// Second strip: record mode, NEW and the 4 song-slot buttons on the left, Load/Save
	// right-aligned on the right - all thinner than the busy transport row above.
	auto fileStrip = area.removeFromTop(juce::jmin(26.0f, area.getHeight() * 0.14f));
	const float fw = fileStrip.getWidth();
	auto colF = [&](float frac, float widthFrac) {
		return juce::Rectangle<float>(fileStrip.getX() + fw * frac, fileStrip.getY(), fw * widthFrac - 4.0f,
		                               fileStrip.getHeight());
	};
	recModeBounds = colF(0.000f, 0.220f);
	newBounds = colF(0.225f, 0.075f);
	for (int s = 0; s < D110SequencerEngine::kNumSongSlots; ++s)
		slotBounds[static_cast<size_t>(s)] = colF(0.310f + float(s) * 0.048f, 0.044f);
	undoBounds = colF(0.560f, 0.110f);
	loadBounds = colF(0.780f, 0.100f);
	saveBounds = colF(0.885f, 0.100f);
	area.removeFromTop(juce::jmax(2.0f, area.getHeight() * 0.015f));

	// Third strip: step recording (see D110SequencerEngine's step API) - a toggle, the step
	// duration readout, REST/BACK, and a plain "bar N step M" readout while it's active.
	auto stepStrip = area.removeFromTop(juce::jmin(24.0f, area.getHeight() * 0.13f));
	const float sw = stepStrip.getWidth();
	auto colS = [&](float frac, float widthFrac) {
		return juce::Rectangle<float>(stepStrip.getX() + sw * frac, stepStrip.getY(), sw * widthFrac - 4.0f,
		                               stepStrip.getHeight());
	};
	stepBounds = colS(0.000f, 0.115f);
	stepDurationBounds = colS(0.125f, 0.130f);
	stepDotBounds = colS(0.265f, 0.075f);
	restBounds = colS(0.350f, 0.115f);
	backBounds = colS(0.475f, 0.115f);
	stepInfoBounds = colS(0.600f, 0.400f);
	area.removeFromTop(juce::jmax(2.0f, area.getHeight() * 0.015f));

	const float rowH = area.getHeight() / float(kNumTracks);
	const float rw = area.getWidth();
	auto colR = [&](juce::Rectangle<float> row, float frac, float widthFrac) {
		return juce::Rectangle<float>(row.getX() + rw * frac, row.getY(), rw * widthFrac - 4.0f,
		                               row.getHeight());
	};
	for (int t = 0; t < kNumTracks; ++t) {
		auto row = area.removeFromTop(rowH);
		auto &r = rows[static_cast<size_t>(t)];
		r.rowBounds = row;
		r.label = colR(row, 0.000f, 0.150f);
		r.channelReadout = colR(row, 0.150f, 0.150f);
		r.muteBounds = colR(row, 0.320f, 0.130f);
		r.soloBounds = colR(row, 0.460f, 0.130f);
		r.armBounds = colR(row, 0.600f, 0.130f);
		r.activityBounds = colR(row, 0.750f, 0.230f);
	}
}

void D110SequencerPanel::resized() { layout(); }

void D110SequencerPanel::paint(juce::Graphics &g) {
	const auto &pal = d110ui::palette();
	g.fillAll(pal.panelBg);
	auto &eng = engine();

	paintToggleButton(g, stopBounds, "STOP", !eng.isPlaying());
	paintToggleButton(g, playBounds, "PLAY", eng.isPlaying() && !eng.isRecording());
	paintToggleButton(g, recBounds, "REC", eng.isRecording());
	paintToggleButton(g, tempoBounds, juce::String(eng.getTempo(), 1) + " BPM", false);
	paintToggleButton(
		g, timeSigBounds,
		juce::String(eng.getTimeSigNumerator()) + "/" + juce::String(eng.getTimeSigDenominator()), false);
	paintToggleButton(g, metronomeBounds, "METRO", eng.getMetronomeEnabled());
	const int precountBars = eng.getPrecountBars();
	paintToggleButton(g, precountBounds,
	                   precountBars == 0 ? "PRECOUNT OFF" : "PRECOUNT " + juce::String(precountBars),
	                   precountBars > 0);
	paintToggleButton(g, loopBounds, loopModeLabel(eng.getLoopMode()), eng.getLoopMode() != d110seq::LoopMode::off);
	paintToggleButton(g, barPrevBounds, "<", false);
	paintToggleButton(g, barNextBounds, ">", false);
	paintToggleButton(g, barReadoutBounds,
	                   "BAR " + juce::String(eng.getCurrentBar()) + "/" + juce::String(eng.getBarCount()),
	                   eng.isPrecounting());

	if (eng.getMetronomeEnabled() && eng.getMetronomeMode() != d110seq::MetronomeMode::audioOnly
	    && (!eng.getMetronomeRecordOnly() || eng.isRecording()) && metroLedBounds.getWidth() > 0.0f) {
		const int total = juce::jmax(1, eng.clicksPerBar());
		// Precounting: only the downbeat LED is ever used, but it FLASHES once per beat of the
		// count-in (timerCallback() edge-detects each beat and opens a short window here) -
		// deliberately not scrolling through the strip the way normal recording does below, so
		// it still reads at a glance as "counting in, not recording yet", but each beat is
		// visible even without audio, per Alan's own ask.
		const bool precountFlashOn =
			eng.isPrecounting() && juce::Time::getMillisecondCounter() < precountFlashUntilMs;
		const int active = eng.isPrecounting() ? (precountFlashOn ? 0 : -1) : eng.currentClickInBar();
		const float ledW = metroLedBounds.getWidth() / float(total);
		for (int i = 0; i < total; ++i) {
			juce::Rectangle<float> led(metroLedBounds.getX() + ledW * float(i), metroLedBounds.getY(),
			                            juce::jmax(1.0f, ledW - 3.0f), metroLedBounds.getHeight());
			// Downbeat (leftmost) is orange, every other click is green - only the currently
			// active one is fully lit, the rest sit dim so the strip reads as "scrolling".
			const juce::Colour c = i == 0 ? pal.seqMetroDownbeat : pal.seqMetroBeat;
			g.setColour(i == active ? c : c.withAlpha(0.16f));
			g.fillRect(led);
		}
	}

	paintToggleButton(g, recModeBounds, recordModeLabel(eng.getRecordMode()), false);
	paintToggleButton(g, newBounds, "NEW", false);
	for (int s = 0; s < D110SequencerEngine::kNumSongSlots; ++s) {
		const auto &b = slotBounds[static_cast<size_t>(s)];
		paintToggleButton(g, b, juce::String(s + 1), eng.getCurrentSongSlot() == s);
		if (eng.songSlotHasContent(s)) {
			g.setColour(pal.seqActiveFill);
			g.fillEllipse(b.getRight() - 8.0f, b.getY() + 3.0f, 4.0f, 4.0f);
		}
	}
	paintToggleButton(g, undoBounds, "UNDO", false, eng.canUndo());
	paintToggleButton(g, loadBounds, "LOAD", false);
	paintToggleButton(g, saveBounds, "SAVE", false);

	paintToggleButton(g, stepBounds, "STEP", eng.isStepRecording());
	paintToggleButton(g, stepDurationBounds, stepDurationLabel(eng.getStepDuration()), false);
	paintToggleButton(g, stepDotBounds, "DOT", eng.getStepDotted());
	paintToggleButton(g, restBounds, "REST", false, eng.isStepRecording());
	paintToggleButton(g, backBounds, "BACK", false, eng.isStepRecording());
	if (eng.isStepRecording()) {
		g.setColour(pal.handleLabel);
		g.setFont(juce::FontOptions(juce::jlimit(8.0f, 13.0f, stepInfoBounds.getHeight() * 0.5f)));
		g.drawText("Bar " + juce::String(eng.getStepBar()) + " step " + juce::String(eng.getStepIndexInBar()),
		           stepInfoBounds, juce::Justification::centredLeft);
	}

	for (int t = 0; t < kNumTracks; ++t) {
		const auto &r = rows[static_cast<size_t>(t)];
		const bool isRhythm = t == D110SequencerEngine::kRhythmTrack;

		g.setColour(pal.seqInactiveText);
		g.setFont(juce::FontOptions(juce::jlimit(9.0f, 14.0f, r.label.getHeight() * 0.5f)));
		g.drawText(isRhythm ? "RHYTHM" : ("PART " + juce::String(t + 1)), r.label,
		           juce::Justification::centredLeft);

		g.setColour(pal.handleLabel);
		g.drawText("CH " + juce::String(eng.channelForTrack(t)), r.channelReadout,
		           juce::Justification::centredLeft);

		paintToggleButton(g, r.muteBounds, "MUTE", eng.isTrackMuted(t));
		paintToggleButton(g, r.soloBounds, "SOLO", eng.isTrackSoloed(t));
		paintToggleButton(g, r.armBounds, "ARM", eng.getArmedTrack() == t);

		g.setColour(eng.trackHasEvents(t) ? pal.seqTrackFilled : pal.seqTrackEmpty);
		g.fillRect(r.activityBounds.reduced(2.0f));
	}
}

void D110SequencerPanel::mouseDown(const juce::MouseEvent &e) {
	auto &eng = engine();
	const auto p = e.position;

	if (e.mods.isPopupMenu()) {
		if (timeSigBounds.contains(p)) { showTimeSignatureMenu(); return; }
		if (stepDurationBounds.contains(p)) { showStepDurationMenu(); return; }
		if (recModeBounds.contains(p)) { showRecordModeMenu(); return; }
		if (metronomeBounds.contains(p)) { showMetronomeModeMenu(); return; }
		if (loadBounds.contains(p)) { showLoadMenu(); return; }
		if (saveBounds.contains(p)) { showSaveMenu(); return; }
		if (barReadoutBounds.contains(p)) { showBarMenu(); return; }
		if (barPrevBounds.contains(p)) { eng.gotoBar(1); repaint(); return; }
		if (barNextBounds.contains(p)) { eng.gotoBar(eng.getBarCount()); repaint(); return; }
		if (stopBounds.contains(p)) { processor.midiPanic(); return; }
		for (int t = 0; t < kNumTracks; ++t)
			if (rows[static_cast<size_t>(t)].rowBounds.contains(p)) { showQuantizeMenu(t); return; }
		for (int s = 0; s < D110SequencerEngine::kNumSongSlots; ++s)
			if (slotBounds[static_cast<size_t>(s)].contains(p)) { showCopySongMenu(); return; }
		return;
	}

	if (recModeBounds.contains(p)) { cycleRecordMode(); repaint(); return; }
	if (newBounds.contains(p)) { confirmNewSong(); return; }
	if (undoBounds.contains(p)) {
		if (eng.canUndo()) eng.undo();
		repaint();
		return;
	}
	if (stepBounds.contains(p)) {
		if (eng.isStepRecording()) eng.stopStepRecording();
		else if (eng.getArmedTrack() >= 0) eng.startStepRecording();
		repaint();
		return;
	}
	if (stepDurationBounds.contains(p)) { cycleStepDuration(); repaint(); return; }
	if (stepDotBounds.contains(p)) { eng.setStepDotted(!eng.getStepDotted()); repaint(); return; }
	if (restBounds.contains(p)) {
		if (eng.isStepRecording()) eng.stepRest();
		repaint();
		return;
	}
	if (backBounds.contains(p)) {
		if (eng.isStepRecording()) eng.stepBack();
		repaint();
		return;
	}
	for (int s = 0; s < D110SequencerEngine::kNumSongSlots; ++s)
		if (slotBounds[static_cast<size_t>(s)].contains(p)) { eng.selectSongSlot(s); repaint(); return; }

	if (loadBounds.contains(p)) {
		auto *chooser = new juce::FileChooser("Load a MIDI file into the sequencer", juce::File(), "*.mid");
		chooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
			[this, chooser](const juce::FileChooser &fc) {
				const auto file = fc.getResult();
				if (file != juce::File()) engine().loadMidiFile(file);
				delete chooser;
				repaint();
			});
		return;
	}
	if (saveBounds.contains(p)) {
		auto *chooser = new juce::FileChooser("Save the sequencer as a MIDI file", juce::File(), "*.mid");
		chooser->launchAsync(
			juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
				| juce::FileBrowserComponent::warnAboutOverwriting,
			[this, chooser](const juce::FileChooser &fc) {
				auto file = fc.getResult();
				if (file != juce::File()) {
					if (!file.hasFileExtension("mid")) file = file.withFileExtension("mid");
					engine().saveMidiFile(file);
				}
				delete chooser;
			});
		return;
	}

	if (stopBounds.contains(p)) { eng.stop(); repaint(); return; }
	if (playBounds.contains(p)) { eng.play(); repaint(); return; }
	if (recBounds.contains(p)) {
		if (eng.isRecording()) eng.stopRecording();
		else if (eng.getArmedTrack() >= 0) eng.startRecording();
		repaint();
		return;
	}
	if (timeSigBounds.contains(p)) { cycleTimeSignature(); repaint(); return; }
	if (metronomeBounds.contains(p)) { eng.setMetronomeEnabled(!eng.getMetronomeEnabled()); repaint(); return; }
	if (precountBounds.contains(p)) {
		// 1st click = 1 bar, 2nd = 2 bars, 3rd = off, then back to 1 - see setPrecountBars().
		eng.setPrecountBars((eng.getPrecountBars() + 1) % 3);
		repaint();
		return;
	}
	if (loopBounds.contains(p)) { cycleLoopMode(); repaint(); return; }
	if (barPrevBounds.contains(p)) { eng.gotoBar(juce::jmax(1, eng.getCurrentBar() - 1)); repaint(); return; }
	if (barNextBounds.contains(p)) { eng.gotoBar(eng.getCurrentBar() + 1); repaint(); return; }
	if (barReadoutBounds.contains(p)) {
		draggingBar = true;
		barDragStartY = p.y;
		barDragStartValue = eng.getCurrentBar();
		return;
	}
	if (tempoBounds.contains(p)) {
		draggingTempo = true;
		tempoDragStartY = p.y;
		tempoDragStartValue = eng.getTempo();
		return;
	}

	for (int t = 0; t < kNumTracks; ++t) {
		const auto &r = rows[static_cast<size_t>(t)];
		if (r.muteBounds.contains(p)) { eng.setTrackMuted(t, !eng.isTrackMuted(t)); repaint(); return; }
		if (r.soloBounds.contains(p)) { eng.setTrackSoloed(t, !eng.isTrackSoloed(t)); repaint(); return; }
		if (r.armBounds.contains(p)) { eng.armTrack(eng.getArmedTrack() == t ? -1 : t); repaint(); return; }
	}
}

void D110SequencerPanel::mouseDrag(const juce::MouseEvent &e) {
	if (draggingTempo) {
		// Dragging up raises the tempo, down lowers it - same sense as the panel's own VALUE
		// fields (D110EditorPane's Cell drag), just not sharing their code since this drawer
		// isn't built out of Cells.
		const float dy = tempoDragStartY - e.position.y;
		engine().setTempo(tempoDragStartValue + double(dy) * 0.5);
		repaint();
		return;
	}
	if (draggingBar) {
		// Same drag sense as the tempo field, coarser step (a few pixels per bar rather than
		// per BPM) since bars are a small integer range, not a wide continuous one.
		const float dy = barDragStartY - e.position.y;
		const int bar = barDragStartValue + juce::roundToInt(dy / 6.0f);
		engine().gotoBar(juce::jmax(1, bar));
		repaint();
	}
}

void D110SequencerPanel::mouseUp(const juce::MouseEvent &) {
	draggingTempo = false;
	draggingBar = false;
}

void D110SequencerPanel::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) {
	if (!tempoBounds.contains(e.position)) return;
	engine().setTempo(engine().getTempo() + (wheel.deltaY > 0 ? 1.0 : -1.0));
	repaint();
}
