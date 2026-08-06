#include "D110SequencerPanel.h"

#include "../PluginProcessor.h"

using d110seq::D110SequencerEngine;

namespace {
constexpr int kNumTracks = D110SequencerEngine::kNumTracks;

void paintToggleButton(juce::Graphics &g, juce::Rectangle<float> b, const juce::String &label, bool active) {
	g.setColour(active ? juce::Colour(0xff6ab81f) : juce::Colour(0xff26262c));
	g.fillRect(b.reduced(2.0f));
	g.setColour(active ? juce::Colour(0xff0a0a0c) : juce::Colour(0xffb8b8c0));
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

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this, track](int result) {
		using d110seq::QuantizeGrid;
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

void D110SequencerPanel::layout() {
	auto area = getLocalBounds().toFloat();
	if (area.getWidth() < 1.0f || area.getHeight() < 1.0f) return;

	auto transport = area.removeFromTop(juce::jmin(40.0f, area.getHeight() * 0.22f));
	const float tw = transport.getWidth();
	auto colT = [&](float frac, float widthFrac) {
		return juce::Rectangle<float>(transport.getX() + tw * frac, transport.getY(),
		                               tw * widthFrac - 4.0f, transport.getHeight());
	};
	stopBounds = colT(0.000f, 0.070f);
	playBounds = colT(0.070f, 0.070f);
	recBounds = colT(0.140f, 0.070f);
	tempoBounds = colT(0.220f, 0.140f);
	timeSigBounds = colT(0.370f, 0.100f);
	metronomeBounds = colT(0.480f, 0.140f);
	precountBounds = colT(0.630f, 0.150f);
	barPrevBounds = colT(0.790f, 0.050f);
	barReadoutBounds = colT(0.840f, 0.110f);
	barNextBounds = colT(0.955f, 0.045f);

	area.removeFromTop(juce::jmax(2.0f, area.getHeight() * 0.015f));

	// Second strip: record mode on the left, Load/Save right-aligned on the right - both
	// thinner than the busy transport row above.
	auto fileStrip = area.removeFromTop(juce::jmin(26.0f, area.getHeight() * 0.14f));
	const float fw = fileStrip.getWidth();
	recModeBounds = { fileStrip.getX(), fileStrip.getY(), fw * 0.320f - 4.0f, fileStrip.getHeight() };
	loadBounds = { fileStrip.getX() + fw * 0.780f, fileStrip.getY(), fw * 0.100f - 4.0f,
	               fileStrip.getHeight() };
	saveBounds = { fileStrip.getX() + fw * 0.885f, fileStrip.getY(), fw * 0.100f - 4.0f,
	               fileStrip.getHeight() };
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
	g.fillAll(juce::Colour(0xff141416));
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
	paintToggleButton(g, barPrevBounds, "<", false);
	paintToggleButton(g, barNextBounds, ">", false);
	paintToggleButton(g, barReadoutBounds,
	                   "BAR " + juce::String(eng.getCurrentBar()) + "/" + juce::String(eng.getBarCount()),
	                   eng.isPrecounting());

	paintToggleButton(g, recModeBounds, recordModeLabel(eng.getRecordMode()), false);
	paintToggleButton(g, loadBounds, "LOAD", false);
	paintToggleButton(g, saveBounds, "SAVE", false);

	for (int t = 0; t < kNumTracks; ++t) {
		const auto &r = rows[static_cast<size_t>(t)];
		const bool isRhythm = t == D110SequencerEngine::kRhythmTrack;

		g.setColour(juce::Colour(0xffb8b8c0));
		g.setFont(juce::FontOptions(juce::jlimit(9.0f, 14.0f, r.label.getHeight() * 0.5f)));
		g.drawText(isRhythm ? "RHYTHM" : ("PART " + juce::String(t + 1)), r.label,
		           juce::Justification::centredLeft);

		g.setColour(juce::Colour(0xff6a6a74));
		g.drawText("CH " + juce::String(eng.channelForTrack(t)), r.channelReadout,
		           juce::Justification::centredLeft);

		paintToggleButton(g, r.muteBounds, "MUTE", eng.isTrackMuted(t));
		paintToggleButton(g, r.soloBounds, "SOLO", eng.isTrackSoloed(t));
		paintToggleButton(g, r.armBounds, "ARM", eng.getArmedTrack() == t);

		g.setColour(eng.trackHasEvents(t) ? juce::Colour(0xff3f7a10) : juce::Colour(0xff26262c));
		g.fillRect(r.activityBounds.reduced(2.0f));
	}
}

void D110SequencerPanel::mouseDown(const juce::MouseEvent &e) {
	auto &eng = engine();
	const auto p = e.position;

	if (e.mods.isPopupMenu()) {
		if (timeSigBounds.contains(p)) { showTimeSignatureMenu(); return; }
		if (recModeBounds.contains(p)) { showRecordModeMenu(); return; }
		for (int t = 0; t < kNumTracks; ++t)
			if (rows[static_cast<size_t>(t)].rowBounds.contains(p)) { showQuantizeMenu(t); return; }
		return;
	}

	if (recModeBounds.contains(p)) { cycleRecordMode(); repaint(); return; }

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
	if (barPrevBounds.contains(p)) { eng.gotoBar(juce::jmax(1, eng.getCurrentBar() - 1)); repaint(); return; }
	if (barNextBounds.contains(p)) { eng.gotoBar(eng.getCurrentBar() + 1); repaint(); return; }
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
	if (!draggingTempo) return;
	// Dragging up raises the tempo, down lowers it - same sense as the panel's own VALUE
	// fields (D110EditorPane's Cell drag), just not sharing their code since this drawer
	// isn't built out of Cells.
	const float dy = tempoDragStartY - e.position.y;
	engine().setTempo(tempoDragStartValue + double(dy) * 0.5);
	repaint();
}

void D110SequencerPanel::mouseUp(const juce::MouseEvent &) { draggingTempo = false; }

void D110SequencerPanel::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) {
	if (!tempoBounds.contains(e.position)) return;
	engine().setTempo(engine().getTempo() + (wheel.deltaY > 0 ? 1.0 : -1.0));
	repaint();
}
