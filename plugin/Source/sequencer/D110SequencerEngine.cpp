#include "D110SequencerEngine.h"

#include <cmath>
#include <limits>

namespace d110seq {

// Shared by deleteBars()/copyBars()/stepBack() below: rebuilds a track's events, keeping a note
// (both its on and off, wherever the off actually lands) whenever its on-time satisfies `keep`,
// and shifting a kept note's on/off times together by whatever `shiftFor` returns for its
// on-time - so a held note's duration always survives, even one that starts before a boundary
// and ends after it.
template <typename KeepFn, typename ShiftFn>
static void rebuildTrackEvents(juce::MidiMessageSequence &events, KeepFn keep, ShiftFn shiftFor) {
	events.updateMatchedPairs();
	juce::MidiMessageSequence rebuilt;
	for (int i = 0; i < events.getNumEvents(); ++i) {
		const auto *ev = events.getEventPointer(i);
		if (ev->message.isNoteOff()) continue; // carried along with its note-on below
		const double onBeat = ev->message.getTimeStamp();
		if (!keep(onBeat)) continue;
		const double shift = shiftFor(onBeat);
		auto onMsg = ev->message;
		onMsg.setTimeStamp(onBeat + shift);
		rebuilt.addEvent(onMsg);
		if (ev->noteOffObject != nullptr) {
			auto offMsg = ev->noteOffObject->message;
			offMsg.setTimeStamp(offMsg.getTimeStamp() + shift);
			rebuilt.addEvent(offMsg);
		}
	}
	rebuilt.sort();
	rebuilt.updateMatchedPairs();
	events = std::move(rebuilt);
}

D110SequencerEngine::D110SequencerEngine() = default;

void D110SequencerEngine::setChannelSource(std::function<int(int)> f) {
	channelSource = std::move(f);
}

int D110SequencerEngine::channelForTrack(int trackIndex) const {
	if (channelSource) {
		const int ch = channelSource(trackIndex);
		if (ch >= 1 && ch <= 16) return ch;
	}
	return trackIndex == kRhythmTrack ? 10 : 1;
}

void D110SequencerEngine::setProgramSource(std::function<int(int)> f) {
	programSource = std::move(f);
}

void D110SequencerEngine::setTempo(double bpm) {
	tempoBpm = juce::jlimit(20.0, 300.0, bpm);
}

void D110SequencerEngine::setTimeSignature(int numerator, int denominator) {
	timeSigNum = juce::jlimit(1, 32, numerator);
	timeSigDen = juce::jlimit(1, 32, denominator);
}

double D110SequencerEngine::barLengthBeats() const {
	return timeSigNum * (4.0 / timeSigDen);
}

void D110SequencerEngine::setExtraTracksEnabled(bool enabled) {
	if (extraTracksEnabled == enabled) return;
	extraTracksEnabled = enabled;
	if (!enabled && armedTrack >= kNumTracks) armTrack(-1);
}

void D110SequencerEngine::play() { playing = true; }

void D110SequencerEngine::stop() {
	// Folds the in-progress take in first - STOP is also how a take normally ends, and
	// skipping the fold here used to silently discard whatever had just been recorded.
	if (recording) stopRecording();
	playing = false;

	// Snap back to the start of whatever bar the transport was in - "remettre le compteur à
	// zéro" (Alan, 2026-08-07). Without this, positionBeats stays at wherever STOP happened
	// (renderInto() only ever moves it forward, or gotoBar() jumps it, nothing else touches
	// it), so resuming later - REC with no precount, in particular - would start the
	// metronome mid-bar (e.g. beat 4, if the previous take happened to end on beat 3) instead
	// of a clean beat 1, even though every visual/audible cue always renders as if a take
	// starts fresh on the downbeat. This only clears the beat-WITHIN-the-bar remainder, not
	// the bar itself - getCurrentBar() and the bar-navigation buttons land on the same bar
	// before and after, so a stopped position is still exactly where the bar-prev/next UI
	// says it is.
	const double bar = barLengthBeats();
	if (bar > 0.0) positionBeats = std::floor(positionBeats / bar + 1.0e-9) * bar;
}

bool D110SequencerEngine::isPrecounting() const {
	return recording && precountRemainingBeats > 0.0;
}

void D110SequencerEngine::gotoBar(int bar) {
	positionBeats = juce::jmax(0, bar - 1) * barLengthBeats();
	loopBar = juce::jmax(1, bar);
}

void D110SequencerEngine::setLoopMode(LoopMode mode) {
	loopMode = mode;
	double s = 0.0, e = 0.0;
	if (mode == LoopMode::bar) {
		s = double(loopBar - 1) * barLengthBeats();
		e = s + barLengthBeats();
	} else if (mode == LoopMode::punch) {
		s = double(punchInBar - 1) * barLengthBeats();
		e = double(punchOutBar) * barLengthBeats();
	} else {
		return;
	}
	if (positionBeats < s || positionBeats >= e) positionBeats = s;
}

int D110SequencerEngine::clicksPerBar() const {
	const double clickGrid = 4.0 / static_cast<double>(timeSigDen);
	if (clickGrid <= 0.0) return 1;
	return juce::jmax(1, juce::roundToInt(barLengthBeats() / clickGrid));
}

int D110SequencerEngine::currentClickInBar() const {
	const double clickGrid = 4.0 / static_cast<double>(timeSigDen);
	const double bar = barLengthBeats();
	if (clickGrid <= 0.0 || bar <= 0.0) return 0;
	double beatInBar = std::fmod(positionBeats, bar);
	if (beatInBar < 0.0) beatInBar += bar;
	const int idx = static_cast<int>(std::floor(beatInBar / clickGrid + 1.0e-9));
	return juce::jlimit(0, clicksPerBar() - 1, idx);
}

int D110SequencerEngine::precountBeatsElapsed() const {
	const double clickGrid = 4.0 / static_cast<double>(timeSigDen);
	if (clickGrid <= 0.0) return 0;
	const double totalPrecountBeats = double(precountBars) * barLengthBeats();
	const double elapsed = juce::jmax(0.0, totalPrecountBeats - precountRemainingBeats);
	return static_cast<int>(std::floor(elapsed / clickGrid + 1.0e-9));
}

void D110SequencerEngine::setPunchIn(int bar) {
	punchInBar = juce::jmax(1, bar);
	if (punchOutBar < punchInBar) punchOutBar = punchInBar;
}

void D110SequencerEngine::setPunchOut(int bar) {
	punchOutBar = juce::jmax(1, bar);
	if (punchInBar > punchOutBar) punchInBar = punchOutBar;
}

void D110SequencerEngine::setPunchRange(int inBar, int outBar) {
	punchInBar = juce::jmax(1, inBar);
	punchOutBar = juce::jmax(punchInBar, outBar);
}

int D110SequencerEngine::getCurrentBar() const {
	const double bl = barLengthBeats();
	return bl > 0.0 ? static_cast<int>(std::floor(positionBeats / bl)) + 1 : 1;
}

int D110SequencerEngine::getBarCount() const {
	double lastBeat = 0.0;
	// Only the currently active tracks - a hidden extra track's leftover content (kept, not
	// wiped, when extra tracks get disabled - see activeTrackCount()'s own comment) shouldn't
	// inflate the visible BAR N/total readout for a page that isn't showing it.
	for (int t = 0; t < activeTrackCount(); ++t)
		lastBeat = juce::jmax(lastBeat, tracks[static_cast<size_t>(t)].events.getEndTime());
	const double bl = barLengthBeats();
	return bl > 0.0 ? juce::jmax(1, static_cast<int>(std::ceil(lastBeat / bl))) : 1;
}

void D110SequencerEngine::armTrack(int index) {
	jassert(index == -1 || (index >= 0 && index < kMaxTracks));
	if (recording) stopRecording();
	if (stepRecording) stopStepRecording();
	armedTrack = index;
}

void D110SequencerEngine::startRecording() {
	if (armedTrack < 0) return;
	if (stepRecording) stopStepRecording();
	playing = true;
	recording = true;
	// Precount is fictitious - it never moves positionBeats (see renderInto()), so the take
	// starts exactly on the bar the transport was already sitting on when this was called.
	recordStartBeats = positionBeats;
	precountRemainingBeats = double(precountBars) * barLengthBeats();
	recordBuffer.clear();
}

void D110SequencerEngine::stopRecording() {
	if (!recording) return;
	recording = false;
	precountRemainingBeats = 0.0;
	if (armedTrack < 0) {
		recordBuffer.clear();
		return;
	}

	auto &committed = trackAt(armedTrack).events;
	if (recordMode == RecordMode::overdub) {
		// Nothing already there is ever removed - the new notes just join it.
		for (int i = 0; i < recordBuffer.getNumEvents(); ++i)
			committed.addEvent(recordBuffer.getEventPointer(i)->message);
	} else {
		// replaceRange erases only the span actually recorded (the take's start to wherever
		// it was stopped - positionBeats, since nothing but renderInto()/gotoBar() ever move
		// it, so it already reads as "now"); replaceToEnd erases from the take's start
		// onward regardless of where it stopped, a deliberate "wipe the rest" tool.
		const double eraseFrom = recordStartBeats;
		const double eraseTo = recordMode == RecordMode::replaceRange
			? juce::jmax(recordStartBeats, positionBeats)
			: std::numeric_limits<double>::max();
		juce::MidiMessageSequence kept;
		for (int i = 0; i < committed.getNumEvents(); ++i) {
			const auto &msg = committed.getEventPointer(i)->message;
			const double ts = msg.getTimeStamp();
			if (ts < eraseFrom || ts >= eraseTo) kept.addEvent(msg);
		}
		for (int i = 0; i < recordBuffer.getNumEvents(); ++i)
			kept.addEvent(recordBuffer.getEventPointer(i)->message);
		committed = std::move(kept);
	}
	committed.sort();
	committed.updateMatchedPairs();
	recordBuffer.clear();
}

void D110SequencerEngine::setStepDuration(QuantizeGrid grid) {
	stepGrid = grid == QuantizeGrid::off ? QuantizeGrid::quarter : grid;
}

void D110SequencerEngine::startStepRecording() {
	if (armedTrack < 0) return;
	if (recording) stopRecording();
	stepRecording = true;
	stepPositionBeats = double(getCurrentBar() - 1) * barLengthBeats();
	stepHeldNotes.clear();
	stepLengths.clear();
}

void D110SequencerEngine::stopStepRecording() {
	if (!stepRecording) return;
	if (!stepHeldNotes.empty()) commitStepInternal(); // don't drop a chord still mid-entry
	stepRecording = false;
	stepHeldNotes.clear();
	stepLengths.clear();
}

void D110SequencerEngine::commitStepInternal() {
	const double len = currentStepBeats();
	if (armedTrack >= 0 && len > 0.0 && !stepHeldNotes.empty()) {
		auto &events = trackAt(armedTrack).events;
		const int channel = channelForTrack(armedTrack);
		for (const auto &h : stepHeldNotes) {
			auto on = juce::MidiMessage::noteOn(channel, h.note, static_cast<juce::uint8>(h.velocity));
			on.setTimeStamp(stepPositionBeats);
			events.addEvent(on);
			auto off = juce::MidiMessage::noteOff(channel, h.note);
			off.setTimeStamp(stepPositionBeats + len);
			events.addEvent(off);
		}
		events.sort();
		events.updateMatchedPairs();
	}
	stepLengths.push_back(len);
	stepPositionBeats += len;
	stepHeldNotes.clear();
}

void D110SequencerEngine::stepNoteOn(int noteNumber, int velocity) {
	if (!stepRecording) return;
	for (auto &h : stepHeldNotes)
		if (h.note == noteNumber) {
			h.stillDown = true;
			h.velocity = velocity;
			return;
		}
	stepHeldNotes.push_back({noteNumber, velocity, true});
}

void D110SequencerEngine::stepNoteOff(int noteNumber) {
	if (!stepRecording) return;
	bool found = false;
	for (auto &h : stepHeldNotes)
		if (h.note == noteNumber) {
			h.stillDown = false;
			found = true;
		}
	if (!found) return;
	for (const auto &h : stepHeldNotes)
		if (h.stillDown) return; // still waiting on the rest of the chord
	commitStepInternal();
}

void D110SequencerEngine::stepRest() {
	if (!stepRecording || !stepHeldNotes.empty()) return;
	const double len = currentStepBeats();
	stepLengths.push_back(len);
	stepPositionBeats += len;
}

void D110SequencerEngine::stepBack() {
	if (!stepRecording || stepLengths.empty()) return;
	const double len = stepLengths.back();
	stepLengths.pop_back();
	stepPositionBeats -= len;
	if (armedTrack >= 0) {
		const double at = stepPositionBeats;
		rebuildTrackEvents(
			trackAt(armedTrack).events, [&](double onBeat) { return std::abs(onBeat - at) > 1.0e-9; },
			[](double) { return 0.0; });
	}
}

int D110SequencerEngine::getStepBar() const {
	const double bl = barLengthBeats();
	return bl > 0.0 ? static_cast<int>(std::floor(stepPositionBeats / bl)) + 1 : 1;
}

int D110SequencerEngine::getStepIndexInBar() const {
	const double step = currentStepBeats();
	if (step <= 0.0) return 1;
	const double barStart = double(getStepBar() - 1) * barLengthBeats();
	return static_cast<int>(std::round((stepPositionBeats - barStart) / step)) + 1;
}

void D110SequencerEngine::setTrackMuted(int index, bool muted) { trackAt(index).muted = muted; }
bool D110SequencerEngine::isTrackMuted(int index) const { return trackAt(index).muted; }
void D110SequencerEngine::setTrackName(int index, const juce::String &name) { trackAt(index).name = name; }
juce::String D110SequencerEngine::getTrackName(int index) const { return trackAt(index).name; }
void D110SequencerEngine::setTrackSoloed(int index, bool soloed) { trackAt(index).soloed = soloed; }
bool D110SequencerEngine::isTrackSoloed(int index) const { return trackAt(index).soloed; }
bool D110SequencerEngine::trackHasEvents(int index) const { return trackAt(index).events.getNumEvents() > 0; }

bool D110SequencerEngine::anySoloed() const {
	for (int t = 0; t < activeTrackCount(); ++t)
		if (tracks[static_cast<size_t>(t)].soloed) return true;
	return false;
}

double D110SequencerEngine::gridBeats(QuantizeGrid grid) const {
	switch (grid) {
		case QuantizeGrid::quarter: return 1.0;
		case QuantizeGrid::eighth: return 0.5;
		case QuantizeGrid::sixteenth: return 0.25;
		case QuantizeGrid::eighthTriplet: return 1.0 / 3.0;
		case QuantizeGrid::sixteenthTriplet: return 1.0 / 6.0;
		case QuantizeGrid::thirtySecond: return 0.125;
		case QuantizeGrid::half: return 2.0;
		case QuantizeGrid::whole: return 4.0;
		case QuantizeGrid::off:
		default: return 0.0;
	}
}

void D110SequencerEngine::snapTrackToGrid(Track &track, QuantizeGrid grid) const {
	track.quantize = grid;
	const double step = gridBeats(grid);
	if (step <= 0.0) return;

	auto &seq = track.events;
	seq.updateMatchedPairs();
	for (int i = 0; i < seq.getNumEvents(); ++i) {
		auto *ev = seq.getEventPointer(i);
		if (!ev->message.isNoteOn()) continue;
		const double original = ev->message.getTimeStamp();
		const double snapped = juce::jmax(0.0, std::round(original / step) * step);
		const double delta = snapped - original;
		if (delta == 0.0) continue;
		ev->message.setTimeStamp(snapped);
		if (ev->noteOffObject != nullptr)
			ev->noteOffObject->message.setTimeStamp(ev->noteOffObject->message.getTimeStamp() + delta);
	}
	seq.sort();
	seq.updateMatchedPairs();
}

void D110SequencerEngine::pushUndoSnapshot() {
	if (undoStack.size() >= kMaxUndoDepth) undoStack.erase(undoStack.begin());
	undoStack.push_back(UndoSnapshot{tracks, songs, currentSlot});
}

void D110SequencerEngine::undo() {
	if (undoStack.empty()) return;
	auto snapshot = std::move(undoStack.back());
	undoStack.pop_back();
	tracks = std::move(snapshot.tracks);
	songs = std::move(snapshot.songs);
	currentSlot = snapshot.currentSlot;
}

void D110SequencerEngine::quantizeTrack(int index, QuantizeGrid grid) {
	jassert(index >= 0 && index < kMaxTracks);
	snapTrackToGrid(trackAt(index), grid);
}

QuantizeGrid D110SequencerEngine::getTrackQuantize(int index) const { return trackAt(index).quantize; }

void D110SequencerEngine::clearTrack(int index) {
	jassert(index >= 0 && index < kMaxTracks);
	trackAt(index).events.clear();
}

void D110SequencerEngine::deleteBars(int trackIndex, int fromBar, int toBarInclusive) {
	jassert(trackIndex == -1 || (trackIndex >= 0 && trackIndex < kMaxTracks));
	const double bar = barLengthBeats();
	const double fromBeats = double(juce::jmax(1, fromBar) - 1) * bar;
	const double toBeats = double(juce::jmax(fromBar, toBarInclusive)) * bar; // exclusive
	const double rangeLen = toBeats - fromBeats;
	if (rangeLen <= 0.0) return;

	auto apply = [&](Track &track) {
		rebuildTrackEvents(
			track.events, [&](double onBeat) { return onBeat < fromBeats || onBeat >= toBeats; },
			[&](double onBeat) { return onBeat >= toBeats ? -rangeLen : 0.0; });
	};
	if (trackIndex < 0)
		for (int t = 0; t < activeTrackCount(); ++t) apply(trackAt(t));
	else
		apply(trackAt(trackIndex));
}

void D110SequencerEngine::copyBars(int srcTrack, int destTrack, int fromBar, int toBarInclusive,
                                    int destBar) {
	jassert((srcTrack == -1 && destTrack == -1) ||
	        (srcTrack >= 0 && srcTrack < kMaxTracks && destTrack >= 0 && destTrack < kMaxTracks));
	const double bar = barLengthBeats();
	const double fromBeats = double(juce::jmax(1, fromBar) - 1) * bar;
	const double toBeats = double(juce::jmax(fromBar, toBarInclusive)) * bar; // exclusive
	const double rangeLen = toBeats - fromBeats;
	const double destBeats = double(juce::jmax(1, destBar) - 1) * bar;
	if (rangeLen <= 0.0) return;

	auto apply = [&](Track &src, Track &dst) {
		// Snapshot the bars being copied from their ORIGINAL positions first - src and dst may
		// be the same track, and the destination rebuild below mutates dst.events in place.
		juce::MidiMessageSequence snippet;
		src.events.updateMatchedPairs();
		for (int i = 0; i < src.events.getNumEvents(); ++i) {
			const auto *ev = src.events.getEventPointer(i);
			if (ev->message.isNoteOff()) continue;
			const double onBeat = ev->message.getTimeStamp();
			if (onBeat < fromBeats || onBeat >= toBeats) continue;
			auto onMsg = ev->message;
			onMsg.setTimeStamp(onBeat - fromBeats + destBeats);
			snippet.addEvent(onMsg);
			if (ev->noteOffObject != nullptr) {
				auto offMsg = ev->noteOffObject->message;
				offMsg.setTimeStamp(offMsg.getTimeStamp() - fromBeats + destBeats);
				snippet.addEvent(offMsg);
			}
		}

		// Make room: push everything already at/after destBeats on the destination track later
		// by rangeLen, then drop the snippet in.
		rebuildTrackEvents(
			dst.events, [](double) { return true; },
			[&](double onBeat) { return onBeat >= destBeats ? rangeLen : 0.0; });
		for (int i = 0; i < snippet.getNumEvents(); ++i) dst.events.addEvent(snippet.getEventPointer(i)->message);
		dst.events.sort();
		dst.events.updateMatchedPairs();
	};

	if (srcTrack < 0)
		for (int t = 0; t < activeTrackCount(); ++t) apply(trackAt(t), trackAt(t));
	else
		apply(trackAt(srcTrack), trackAt(destTrack));
}

void D110SequencerEngine::transposeBars(int trackIndex, int fromBar, int toBarInclusive, int semitones) {
	jassert(trackIndex == -1 || (trackIndex >= 0 && trackIndex < kMaxTracks));
	if (semitones == 0) return;
	const double bar = barLengthBeats();
	const double fromBeats = double(juce::jmax(1, fromBar) - 1) * bar;
	const double toBeats = double(juce::jmax(fromBar, toBarInclusive)) * bar; // exclusive
	if (toBeats <= fromBeats) return;

	auto apply = [&](Track &track) {
		auto &seq = track.events;
		seq.updateMatchedPairs();
		for (int i = 0; i < seq.getNumEvents(); ++i) {
			auto *ev = seq.getEventPointer(i);
			if (!ev->message.isNoteOn()) continue;
			const double onBeat = ev->message.getTimeStamp();
			if (onBeat < fromBeats || onBeat >= toBeats) continue;
			const int note = juce::jlimit(0, 127, ev->message.getNoteNumber() + semitones);
			ev->message.setNoteNumber(note);
			if (ev->noteOffObject != nullptr) ev->noteOffObject->message.setNoteNumber(note);
		}
	};
	if (trackIndex < 0)
		for (int t = 0; t < activeTrackCount(); ++t) apply(trackAt(t));
	else
		apply(trackAt(trackIndex));
}

std::vector<D110SequencerEngine::NoteEventInfo>
D110SequencerEngine::eventsInBarRange(int trackIndex, int fromBar, int toBarInclusive) const {
	std::vector<NoteEventInfo> out;
	jassert(trackIndex >= 0 && trackIndex < kMaxTracks);
	const double bar = barLengthBeats();
	const double fromBeats = double(juce::jmax(1, fromBar) - 1) * bar;
	const double toBeats = double(juce::jmax(fromBar, toBarInclusive)) * bar; // exclusive
	const auto &events = trackAt(trackIndex).events;
	for (int i = 0; i < events.getNumEvents(); ++i) {
		const auto *ev = events.getEventPointer(i);
		if (!ev->message.isNoteOn()) continue;
		const double onBeat = ev->message.getTimeStamp();
		if (onBeat < fromBeats || onBeat >= toBeats) continue;
		// Bar-relative offset, regardless of which bar in [fromBar, toBarInclusive] this
		// particular note actually falls in - correct even when the caller passes a
		// multi-bar range, though D110SequencerPanel's own event-list dialog only ever
		// passes a single bar today.
		const double barStart = std::floor(onBeat / bar) * bar;
		out.push_back({ i, onBeat - barStart, ev->message.getNoteNumber(), int(ev->message.getVelocity()),
		                 ev->noteOffObject != nullptr
		                     ? ev->noteOffObject->message.getTimeStamp() - onBeat : 0.0 });
	}
	return out;
}

void D110SequencerEngine::deleteNoteEvent(int trackIndex, int index) {
	jassert(trackIndex >= 0 && trackIndex < kMaxTracks);
	auto &events = trackAt(trackIndex).events;
	if (index < 0 || index >= events.getNumEvents()) return;
	events.deleteEvent(index, true); // true = also remove the matching note-off, if any
}

void D110SequencerEngine::setNoteEventPitch(int trackIndex, int index, int newNote) {
	jassert(trackIndex >= 0 && trackIndex < kMaxTracks);
	auto &events = trackAt(trackIndex).events;
	if (index < 0 || index >= events.getNumEvents()) return;
	auto *ev = events.getEventPointer(index);
	if (!ev->message.isNoteOn()) return;
	const int note = juce::jlimit(0, 127, newNote);
	ev->message.setNoteNumber(note);
	if (ev->noteOffObject != nullptr) ev->noteOffObject->message.setNoteNumber(note);
}

D110SequencerEngine::Track &D110SequencerEngine::songTrackAt(int slot, int track) {
	if (slot == currentSlot) return trackAt(track);
	return songs[static_cast<size_t>(slot)].tracks[static_cast<size_t>(track)];
}

const D110SequencerEngine::Track &D110SequencerEngine::songTrackAt(int slot, int track) const {
	if (slot == currentSlot) return trackAt(track);
	return songs[static_cast<size_t>(slot)].tracks[static_cast<size_t>(track)];
}

void D110SequencerEngine::newSong() {
	if (recording) stopRecording();
	for (auto &t : tracks) {
		t.events.clear();
		t.muted = false;
		t.soloed = false;
		t.quantize = QuantizeGrid::off;
	}
	armedTrack = -1;
	positionBeats = 0.0;
	playing = false;
}

void D110SequencerEngine::selectSongSlot(int slot) {
	slot = juce::jlimit(0, kNumSongSlots - 1, slot);
	if (slot == currentSlot) return;
	if (recording) stopRecording();

	auto &outgoing = songs[static_cast<size_t>(currentSlot)];
	outgoing.tempoBpm = tempoBpm;
	outgoing.timeSigNum = timeSigNum;
	outgoing.timeSigDen = timeSigDen;
	outgoing.tracks = tracks;

	const auto &incoming = songs[static_cast<size_t>(slot)];
	tempoBpm = incoming.tempoBpm;
	timeSigNum = incoming.timeSigNum;
	timeSigDen = incoming.timeSigDen;
	tracks = incoming.tracks;

	currentSlot = slot;
	playing = false;
	armedTrack = -1;
	positionBeats = 0.0;
}

void D110SequencerEngine::copyCurrentSongTo(int destSlot) {
	jassert(destSlot >= 0 && destSlot < kNumSongSlots);
	if (destSlot == currentSlot) return;
	auto &dest = songs[static_cast<size_t>(destSlot)];
	dest.tempoBpm = tempoBpm;
	dest.timeSigNum = timeSigNum;
	dest.timeSigDen = timeSigDen;
	dest.tracks = tracks;
}

bool D110SequencerEngine::songSlotHasContent(int slot) const {
	slot = juce::jlimit(0, kNumSongSlots - 1, slot);
	const auto &trks = (slot == currentSlot) ? tracks : songs[static_cast<size_t>(slot)].tracks;
	for (const auto &t : trks)
		if (t.events.getNumEvents() > 0) return true;
	return false;
}

double D110SequencerEngine::slotTempo(int slot) const {
	return slot == currentSlot ? tempoBpm : songs[static_cast<size_t>(slot)].tempoBpm;
}

void D110SequencerEngine::setSlotTempo(int slot, double bpm) {
	if (slot == currentSlot) { setTempo(bpm); return; }
	songs[static_cast<size_t>(slot)].tempoBpm = juce::jlimit(20.0, 300.0, bpm);
}

int D110SequencerEngine::slotTimeSigNumerator(int slot) const {
	return slot == currentSlot ? timeSigNum : songs[static_cast<size_t>(slot)].timeSigNum;
}

int D110SequencerEngine::slotTimeSigDenominator(int slot) const {
	return slot == currentSlot ? timeSigDen : songs[static_cast<size_t>(slot)].timeSigDen;
}

void D110SequencerEngine::setSlotTimeSignature(int slot, int numerator, int denominator) {
	if (slot == currentSlot) { setTimeSignature(numerator, denominator); return; }
	auto &s = songs[static_cast<size_t>(slot)];
	s.timeSigNum = juce::jlimit(1, 32, numerator);
	s.timeSigDen = juce::jlimit(1, 32, denominator);
}

bool D110SequencerEngine::slotTrackMuted(int slot, int track) const { return songTrackAt(slot, track).muted; }
void D110SequencerEngine::setSlotTrackMuted(int slot, int track, bool muted) {
	songTrackAt(slot, track).muted = muted;
}
bool D110SequencerEngine::slotTrackSoloed(int slot, int track) const { return songTrackAt(slot, track).soloed; }
void D110SequencerEngine::setSlotTrackSoloed(int slot, int track, bool soloed) {
	songTrackAt(slot, track).soloed = soloed;
}
QuantizeGrid D110SequencerEngine::slotTrackQuantize(int slot, int track) const {
	return songTrackAt(slot, track).quantize;
}
juce::String D110SequencerEngine::slotTrackName(int slot, int track) const {
	return songTrackAt(slot, track).name;
}
void D110SequencerEngine::setSlotTrackName(int slot, int track, const juce::String &name) {
	songTrackAt(slot, track).name = name;
}
void D110SequencerEngine::setSlotTrackQuantize(int slot, int track, QuantizeGrid grid) {
	snapTrackToGrid(songTrackAt(slot, track), grid);
}

void D110SequencerEngine::renderInto(juce::MidiBuffer &midiMessages, int numSamples, double sampleRate,
                                      std::vector<MetronomeClick> *clicksOut) {
	if (numSamples <= 0 || sampleRate <= 0.0 || !playing) return;

	const double beatsPerSample = (tempoBpm / 60.0) / sampleRate;
	int samplesRendered = 0;

	// Shared by both click-emission sites below (precount and normal playback) - records the
	// click for the visual/audio-domain path (clicksOut) and, when enabled, also fires a real
	// note on the rhythm channel so the metronome can be heard through an actual percussion
	// patch instead of only the internal synthesized beep.
	auto emitClick = [&](int sampleOffset, bool downbeat) {
		if (clicksOut != nullptr) clicksOut->push_back({sampleOffset, downbeat});
		if (metronomeUseChannel10) {
			const int channel = channelForTrack(kRhythmTrack);
			const int note = downbeat ? kMetronomeBellNote : kMetronomeClickNote;
			const juce::uint8 velocity = static_cast<juce::uint8>(
			    juce::jlimit(1, 127, juce::roundToInt(100.0f * metronomeVolume)));
			midiMessages.addEvent(juce::MidiMessage::noteOn(channel, note, velocity), sampleOffset);
			const int offOffset = juce::jlimit(sampleOffset, numSamples - 1, sampleOffset + 512);
			midiMessages.addEvent(juce::MidiMessage::noteOff(channel, note), offOffset);
		}
	};

	// Precount: a fictitious count-in, not a real position on the timeline - it plays the
	// metronome without positionBeats moving and without any track content sounding, so the
	// take that follows starts exactly on the bar the transport was already on, not however
	// many bars further along. Consumes up to precountRemainingBeats worth of this block;
	// whatever's left over falls through to normal playback/recording below using the SAME
	// (unmoved) positionBeats as its start.
	if (recording && precountRemainingBeats > 0.0) {
		const double totalPrecountBeats = double(precountBars) * barLengthBeats();
		const double elapsedBefore = juce::jmax(0.0, totalPrecountBeats - precountRemainingBeats);
		const int precountSamples =
		    juce::jlimit(0, numSamples, juce::roundToInt(precountRemainingBeats / beatsPerSample));

		if (clicksOut != nullptr && metronomeEnabled && metronomeMode != MetronomeMode::visualOnly
		    && precountSamples > 0) {
			const double clickGrid = 4.0 / static_cast<double>(timeSigDen);
			const int clicksPerBarCount =
			    clickGrid > 0.0 ? juce::roundToInt(barLengthBeats() / clickGrid) : 0;
			const double windowStart = elapsedBefore;
			const double windowEnd = elapsedBefore + beatsPerSample * precountSamples;
			double nextClickBeat = std::ceil(windowStart / clickGrid - 1.0e-9) * clickGrid;
			while (nextClickBeat < windowEnd) {
				if (nextClickBeat >= windowStart) {
					int sampleOffset = juce::roundToInt((nextClickBeat - windowStart) / beatsPerSample);
					sampleOffset = juce::jlimit(0, numSamples - 1, sampleOffset);
					const int clickIndex = clicksPerBarCount > 0
					                            ? juce::roundToInt(nextClickBeat / clickGrid) % clicksPerBarCount
					                            : 0;
					emitClick(sampleOffset, clickIndex == 0);
				}
				nextClickBeat += clickGrid;
			}
		}

		// precountSamples < numSamples means it was NOT clamped by numSamples above - i.e. it's
		// exactly how many samples were left in the count-in, by construction, and the count-in
		// ends inside this block. Force the remainder to exactly 0.0 in that case rather than
		// subtracting: precountRemainingBeats is continuous but precountSamples is a rounded
		// sample count, so the subtraction can leave a tiny nonzero residual - which, converted
		// back to samples next block, can itself round down to 0 and never shrink further,
		// stalling precountRemainingBeats just above zero forever. That leaves isPrecounting()
		// stuck true long after the count-in has actually finished and positionBeats has
		// already resumed advancing below - confirmed by testPrecountEndsPromptly() failing
		// under a realistic (non-beat-aligned) block size before this fix.
		precountRemainingBeats = precountSamples < numSamples
		                              ? 0.0
		                              : juce::jmax(0.0, precountRemainingBeats - beatsPerSample * precountSamples);
		samplesRendered = precountSamples;
		if (samplesRendered >= numSamples) return; // the whole block was still count-in
	}

	const bool solo = anySoloed();

	// Loop bounds in beats, if a loop is active - not applied while precounting/recording,
	// since punching in mid-loop would otherwise cut a take short out from under the user.
	double loopStart = 0.0, loopEnd = 0.0;
	bool loopActive = false;
	if (!recording) {
		if (loopMode == LoopMode::bar) {
			loopStart = double(loopBar - 1) * barLengthBeats();
			loopEnd = loopStart + barLengthBeats();
			loopActive = loopEnd > loopStart;
		} else if (loopMode == LoopMode::punch) {
			loopStart = double(punchInBar - 1) * barLengthBeats();
			loopEnd = double(punchOutBar) * barLengthBeats();
			loopActive = loopEnd > loopStart;
		}
	}

	// Render in sub-blocks so a loop wrap that falls mid-block still emits every event on
	// both sides of the seam at the right sample offset, instead of skipping or misplacing
	// whatever's near the loop point.
	while (samplesRendered < numSamples) {
		const double windowStart = positionBeats;
		int samplesThisPass = numSamples - samplesRendered;
		double windowEnd = windowStart + beatsPerSample * samplesThisPass;

		if (loopActive && windowStart >= loopStart && windowEnd > loopEnd) {
			samplesThisPass = juce::jmax(0, juce::roundToInt((loopEnd - windowStart) / beatsPerSample));
			windowEnd = windowStart + beatsPerSample * samplesThisPass;
		}

		for (int t = 0; t < activeTrackCount(); ++t) {
			// The armed track, mid-take, plays back its own already-committed content normally
			// here - new captures go to recordBuffer (a separate sequence this loop never
			// touches), not into the track itself, until stopRecording() folds them in. That's
			// what makes overdub's existing material actually audible while recording.
			//
			// A replace mode (replaceRange/replaceToEnd) is different: that existing content is
			// about to be erased by this very take, over a span stopRecording() won't actually
			// know until the take ends - replaceRange erases up to wherever recording is
			// stopped, replaceToEnd erases everything from the take's start on. Playing it back
			// meanwhile would sound like a doubled performance for content that's being thrown
			// away, so it stays silent for the whole take rather than only over whatever range
			// turns out to get erased.
			const auto &track = trackAt(t);
			if (track.muted) continue;
			if (solo && !track.soloed) continue;
			if (recording && t == armedTrack && recordMode != RecordMode::overdub) continue;

			const auto &seq = track.events;
			for (int i = seq.getNextIndexAtTime(windowStart); i < seq.getNumEvents(); ++i) {
				const auto *ev = seq.getEventPointer(i);
				const double ts = ev->message.getTimeStamp();
				if (ts >= windowEnd) break;
				if (!isNoteEvent(ev->message)) continue; // tracks are note-only in this model
				auto msg = ev->message;
				if (msg.getChannel() > 0) msg.setChannel(channelForTrack(t));
				int sampleOffset = samplesRendered + juce::roundToInt((ts - windowStart) / beatsPerSample);
				sampleOffset = juce::jlimit(0, numSamples - 1, sampleOffset);
				midiMessages.addEvent(msg, sampleOffset);
			}
		}

		// metronomeRecordOnly (most DAWs' "click only when recording") is a no-op during the
		// precount block above - that one only ever runs while recording is already true.
		if (clicksOut != nullptr && metronomeEnabled && metronomeMode != MetronomeMode::visualOnly
		    && (!metronomeRecordOnly || recording)) {
			// Metronome clicks on the meter's own reporting subdivision (a quarter in 4/4,
			// an eighth in 6/8, ...), not on the quarter-note beat unit events are stored in.
			const double clickGrid = 4.0 / static_cast<double>(timeSigDen);
			const double bar = barLengthBeats();
			const int clicksPerBar = clickGrid > 0.0 ? juce::roundToInt(bar / clickGrid) : 0;
			double nextClickBeat = std::ceil(windowStart / clickGrid - 1.0e-9) * clickGrid;
			while (nextClickBeat < windowEnd) {
				if (nextClickBeat >= windowStart) {
					int sampleOffset =
					    samplesRendered + juce::roundToInt((nextClickBeat - windowStart) / beatsPerSample);
					sampleOffset = juce::jlimit(0, numSamples - 1, sampleOffset);
					const int clickIndex =
					    clicksPerBar > 0 ? juce::roundToInt(nextClickBeat / clickGrid) % clicksPerBar : 0;
					emitClick(sampleOffset, clickIndex == 0);
				}
				nextClickBeat += clickGrid;
			}
		}

		positionBeats = windowEnd;
		samplesRendered += samplesThisPass;

		if (loopActive && positionBeats >= loopEnd - 1.0e-9) positionBeats = loopStart;
		if (samplesThisPass <= 0) break; // degenerate loop range (loopEnd <= loopStart) - bail, don't spin
	}
}

void D110SequencerEngine::captureEvent(const juce::MidiMessage &message, double atBeats) {
	if (!recording || armedTrack < 0) return;
	// Precount no longer advances positionBeats (it's fictitious), so without this a note
	// played during the count-in would compute as "at or after recordStartBeats" - the very
	// value positionBeats is frozen at - and get captured immediately, defeating the count-in.
	if (precountRemainingBeats > 0.0) return;
	if (atBeats < recordStartBeats) return;
	if (loopMode == LoopMode::punch) {
		// Punch mode doubles as "record only in this range" - see setLoopMode()'s own comment.
		const double punchInBeats = double(punchInBar - 1) * barLengthBeats();
		const double punchOutBeats = double(punchOutBar) * barLengthBeats();
		if (atBeats < punchInBeats || atBeats >= punchOutBeats) return;
	}
	if (!isNoteEvent(message)) return;

	auto msg = message;
	msg.setTimeStamp(atBeats);
	if (msg.getChannel() > 0) msg.setChannel(channelForTrack(armedTrack));
	recordBuffer.addEvent(msg);
}

bool D110SequencerEngine::saveMidiFile(const juce::File &file) const {
	juce::MidiFile mf;
	constexpr int tpqn = 960;
	mf.setTicksPerQuarterNote(tpqn);

	juce::MidiMessageSequence meta;
	meta.addEvent(juce::MidiMessage::tempoMetaEvent(static_cast<int>(60000000.0 / tempoBpm)), 0.0);
	meta.addEvent(juce::MidiMessage::timeSignatureMetaEvent(timeSigNum, timeSigDen), 0.0);
	mf.addTrack(meta);

	for (int t = 0; t < activeTrackCount(); ++t) {
		juce::MidiMessageSequence seq;
		const int channel = channelForTrack(t);
		// Track Name meta-event (FF 03) - the user's own name if they set one (see
		// setTrackName()), otherwise the same "PART N"/"RHYTHM"/"TRACK N" label the panel
		// falls back to, so a track never reaches the exported file unnamed.
		const auto &trackName = trackAt(t).name;
		const juce::String label = trackName.isNotEmpty()
			? trackName
			: (t == kRhythmTrack ? juce::String("RHYTHM")
			                     : (t < kNumTracks ? ("PART " + juce::String(t + 1))
			                                       : ("TRACK " + juce::String(t + 1))));
		seq.addEvent(juce::MidiMessage::textMetaEvent(3, label), 0.0);
		// A Program Change at time 0, ahead of the track's own notes, so a receiving synth
		// (including this same plugin, reimporting the file) starts the piece on
		// approximately the sound that was live when this was exported. See setProgramSource's
		// own comment for why "approximately", and for what changing it later actually means.
		if (programSource) {
			const int program = programSource(t);
			if (program >= 0 && program < 128)
				seq.addEvent(juce::MidiMessage::programChange(channel, program), 0.0);
		}
		const auto &src = trackAt(t).events;
		for (int i = 0; i < src.getNumEvents(); ++i) {
			auto msg = src.getEventPointer(i)->message;
			if (msg.getChannel() > 0) msg.setChannel(channel);
			msg.setTimeStamp(msg.getTimeStamp() * tpqn);
			seq.addEvent(msg);
		}
		seq.updateMatchedPairs();
		mf.addTrack(seq);
	}

	file.deleteFile();
	juce::FileOutputStream out(file);
	if (!out.openedOk()) return false;
	return mf.writeTo(out);
}

bool D110SequencerEngine::loadMidiFile(const juce::File &file) {
	juce::FileInputStream in(file);
	if (!in.openedOk()) return false;

	juce::MidiFile mf;
	if (!mf.readFrom(in)) return false;

	const short tpqnRaw = mf.getTimeFormat();
	if (tpqnRaw <= 0) return false; // SMPTE time format not supported
	const double tpqn = static_cast<double>(tpqnRaw);

	double newTempo = tempoBpm;
	int newNum = timeSigNum, newDen = timeSigDen;
	for (int t = 0; t < mf.getNumTracks(); ++t) {
		const auto *seq = mf.getTrack(t);
		for (int i = 0; i < seq->getNumEvents(); ++i) {
			const auto &msg = seq->getEventPointer(i)->message;
			if (msg.isTempoMetaEvent()) {
				const double spq = msg.getTempoSecondsPerQuarterNote();
				if (spq > 0.0) newTempo = 60.0 / spq;
			} else if (msg.isTimeSignatureMetaEvent()) {
				int num = 4, den = 4;
				msg.getTimeSignatureInfo(num, den);
				newNum = num;
				newDen = den;
			}
		}
	}

	// Our own saveMidiFile() writes a leading meta-only track before the note tracks; detect
	// and skip it so both our own files and a plain N-track import line up with tracks[0..].
	int startTrack = 0;
	if (mf.getNumTracks() >= activeTrackCount() + 1) {
		const auto *t0 = mf.getTrack(0);
		bool track0HasNotes = false;
		for (int i = 0; i < t0->getNumEvents(); ++i)
			if (t0->getEventPointer(i)->message.isNoteOnOrOff()) {
				track0HasNotes = true;
				break;
			}
		if (!track0HasNotes) startTrack = 1;
	}

	for (int t = 0; t < activeTrackCount(); ++t) {
		juce::MidiMessageSequence fresh;
		juce::String newName; // stays empty if the source track carries no name of its own
		const int srcIndex = startTrack + t;
		if (srcIndex < mf.getNumTracks()) {
			const auto *seq = mf.getTrack(srcIndex);
			for (int i = 0; i < seq->getNumEvents(); ++i) {
				auto msg = seq->getEventPointer(i)->message;
				if (msg.isTrackNameEvent()) newName = msg.getTextFromTextMetaEvent();
				// Tracks are note-only in this model (see captureEvent); drop meta
				// events like End-Of-Track that MidiFile::writeTo appends per track,
				// or they'd otherwise get emitted verbatim by renderInto().
				if (!msg.isNoteOnOrOff()) continue;
				msg.setTimeStamp(msg.getTimeStamp() / tpqn);
				fresh.addEvent(msg);
			}
			fresh.updateMatchedPairs();
		}
		trackAt(t).events = std::move(fresh);
		trackAt(t).name = newName;
	}

	setTempo(newTempo);
	setTimeSignature(newNum, newDen);
	return true;
}

juce::MemoryBlock D110SequencerEngine::serializeTrack(const juce::MidiMessageSequence &events) const {
	juce::MidiFile mf;
	constexpr int tpqn = 960;
	mf.setTicksPerQuarterNote(tpqn);

	juce::MidiMessageSequence seq;
	for (int i = 0; i < events.getNumEvents(); ++i) {
		auto msg = events.getEventPointer(i)->message;
		msg.setTimeStamp(msg.getTimeStamp() * tpqn);
		seq.addEvent(msg);
	}
	mf.addTrack(seq);

	juce::MemoryOutputStream out;
	mf.writeTo(out);
	return out.getMemoryBlock();
}

void D110SequencerEngine::deserializeTrack(juce::MidiMessageSequence &events, const void *data,
                                            size_t size) const {
	if (size == 0) {
		events.clear();
		return;
	}

	juce::MemoryInputStream in(data, size, false);
	juce::MidiFile mf;
	if (!mf.readFrom(in) || mf.getNumTracks() == 0) return;
	const short tpqnRaw = mf.getTimeFormat();
	if (tpqnRaw <= 0) return;
	const double tpqn = static_cast<double>(tpqnRaw);

	juce::MidiMessageSequence fresh;
	const auto *seq = mf.getTrack(0);
	for (int i = 0; i < seq->getNumEvents(); ++i) {
		auto msg = seq->getEventPointer(i)->message;
		if (!msg.isNoteOnOrOff()) continue;
		msg.setTimeStamp(msg.getTimeStamp() / tpqn);
		fresh.addEvent(msg);
	}
	fresh.updateMatchedPairs();
	events = std::move(fresh);
}

juce::MemoryBlock D110SequencerEngine::trackToBytes(int index) const {
	jassert(index >= 0 && index < kMaxTracks);
	return serializeTrack(trackAt(index).events);
}

void D110SequencerEngine::trackFromBytes(int index, const void *data, size_t size) {
	jassert(index >= 0 && index < kMaxTracks);
	deserializeTrack(trackAt(index).events, data, size);
}

juce::MemoryBlock D110SequencerEngine::slotTrackToBytes(int slot, int track) const {
	return serializeTrack(songTrackAt(slot, track).events);
}

void D110SequencerEngine::slotTrackFromBytes(int slot, int track, const void *data, size_t size) {
	deserializeTrack(songTrackAt(slot, track).events, data, size);
}

} // namespace d110seq
