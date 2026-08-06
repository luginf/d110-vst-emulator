#include "D110SequencerPanel.h"

#include "../PluginProcessor.h"
#include "../UiTheme.h"

using d110seq::D110SequencerEngine;

namespace {
constexpr int kNumTracks = D110SequencerEngine::kNumTracks;

void paintToggleButton(juce::Graphics &g, juce::Rectangle<float> b, const juce::String &label, bool active) {
	const auto &pal = d110ui::palette();
	g.setColour(active ? pal.seqActiveFill : pal.seqInactiveFill);
	g.fillRect(b.reduced(2.0f));
	g.setColour(active ? pal.seqActiveText : pal.seqInactiveText);
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
} // namespace

D110SequencerPanel::D110SequencerPanel(D110AudioProcessor &p) : processor(p) { startTimerHz(15); }

D110SequencerPanel::~D110SequencerPanel() { stopTimer(); }

d110seq::D110SequencerEngine &D110SequencerPanel::engine() { return processor.getSequencer(); }

void D110SequencerPanel::timerCallback() {
	// The only thing that changes on its own, without a click, is the bar readout (and the
	// transport buttons' own on/off look) while the transport is rolling.
	if (engine().isPlaying()) repaint();
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
	m.addSeparator();
	m.addItem(7, "Clear track", engine().trackHasEvents(track));

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this, track](int result) {
		using d110seq::QuantizeGrid;
		if (result == 7) { confirmClearTrack(track); return; }
		QuantizeGrid grid;
		switch (result) {
			case 1: grid = QuantizeGrid::off; break;
			case 2: grid = QuantizeGrid::quarter; break;
			case 3: grid = QuantizeGrid::eighth; break;
			case 4: grid = QuantizeGrid::sixteenth; break;
			case 5: grid = QuantizeGrid::eighthTriplet; break;
			case 6: grid = QuantizeGrid::sixteenthTriplet; break;
			default: return;
		}
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
			+ ". Mute/solo/quantize and every other track are kept. This cannot be undone.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Clear", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, track](int result) {
		if (result == 1) {
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

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int result) {
		auto &e = engine();
		switch (result) {
			case 1: promptForBar(); return;
			case 2: e.setPunchIn(e.getCurrentBar()); break;
			case 3: e.setPunchOut(e.getCurrentBar()); break;
			case 4: promptForPunchRange(); return;
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

// NEW clears the current song slot's tracks - destructive and unrecoverable, so it always
// confirms first (Alan's own explicit ask, to avoid an accidental wipe).
void D110SequencerPanel::confirmNewSong() {
	const int slot = engine().getCurrentSongSlot();
	auto *aw = new juce::AlertWindow(
		"Clear this song?",
		"This clears every track's recorded MIDI in song " + juce::String(slot + 1)
			+ ". Tempo, time signature and other settings are kept. This cannot be undone.",
		juce::AlertWindow::WarningIcon);
	aw->addButton("Clear", 1, juce::KeyPress(juce::KeyPress::returnKey));
	aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
	aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw](int result) {
		if (result == 1) {
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
	loadBounds = colF(0.780f, 0.100f);
	saveBounds = colF(0.885f, 0.100f);
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

	if (eng.getMetronomeEnabled() && metroLedBounds.getWidth() > 0.0f) {
		const int total = juce::jmax(1, eng.clicksPerBar());
		const int active = eng.currentClickInBar();
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
	paintToggleButton(g, loadBounds, "LOAD", false);
	paintToggleButton(g, saveBounds, "SAVE", false);

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
		if (recModeBounds.contains(p)) { showRecordModeMenu(); return; }
		if (barReadoutBounds.contains(p)) { showBarMenu(); return; }
		if (barPrevBounds.contains(p)) { eng.gotoBar(1); repaint(); return; }
		if (barNextBounds.contains(p)) { eng.gotoBar(eng.getBarCount()); repaint(); return; }
		for (int t = 0; t < kNumTracks; ++t)
			if (rows[static_cast<size_t>(t)].rowBounds.contains(p)) { showQuantizeMenu(t); return; }
		return;
	}

	if (recModeBounds.contains(p)) { cycleRecordMode(); repaint(); return; }
	if (newBounds.contains(p)) { confirmNewSong(); return; }
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
