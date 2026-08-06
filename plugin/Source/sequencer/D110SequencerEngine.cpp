#include "D110SequencerEngine.h"

#include <cmath>
#include <limits>

namespace d110seq {

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

void D110SequencerEngine::play() { playing = true; }

void D110SequencerEngine::stop() {
	// Folds the in-progress take in first - STOP is also how a take normally ends, and
	// skipping the fold here used to silently discard whatever had just been recorded.
	if (recording) stopRecording();
	playing = false;
}

bool D110SequencerEngine::isPrecounting() const {
	return recording && positionBeats < recordStartBeats;
}

void D110SequencerEngine::gotoBar(int bar) {
	positionBeats = juce::jmax(0, bar - 1) * barLengthBeats();
}

int D110SequencerEngine::getCurrentBar() const {
	const double bl = barLengthBeats();
	return bl > 0.0 ? static_cast<int>(std::floor(positionBeats / bl)) + 1 : 1;
}

int D110SequencerEngine::getBarCount() const {
	double lastBeat = 0.0;
	for (const auto &tr : tracks) lastBeat = juce::jmax(lastBeat, tr.events.getEndTime());
	const double bl = barLengthBeats();
	return bl > 0.0 ? juce::jmax(1, static_cast<int>(std::ceil(lastBeat / bl))) : 1;
}

void D110SequencerEngine::armTrack(int index) {
	jassert(index == -1 || (index >= 0 && index < kNumTracks));
	if (recording) stopRecording();
	armedTrack = index;
}

void D110SequencerEngine::startRecording() {
	if (armedTrack < 0) return;
	playing = true;
	recording = true;
	recordStartBeats = positionBeats + double(precountBars) * barLengthBeats();
	recordBuffer.clear();
}

void D110SequencerEngine::stopRecording() {
	if (!recording) return;
	recording = false;
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

void D110SequencerEngine::setTrackMuted(int index, bool muted) { trackAt(index).muted = muted; }
bool D110SequencerEngine::isTrackMuted(int index) const { return trackAt(index).muted; }
void D110SequencerEngine::setTrackSoloed(int index, bool soloed) { trackAt(index).soloed = soloed; }
bool D110SequencerEngine::isTrackSoloed(int index) const { return trackAt(index).soloed; }
bool D110SequencerEngine::trackHasEvents(int index) const { return trackAt(index).events.getNumEvents() > 0; }

bool D110SequencerEngine::anySoloed() const {
	for (const auto &t : tracks)
		if (t.soloed) return true;
	return false;
}

double D110SequencerEngine::gridBeats(QuantizeGrid grid) const {
	switch (grid) {
		case QuantizeGrid::quarter: return 1.0;
		case QuantizeGrid::eighth: return 0.5;
		case QuantizeGrid::sixteenth: return 0.25;
		case QuantizeGrid::eighthTriplet: return 1.0 / 3.0;
		case QuantizeGrid::sixteenthTriplet: return 1.0 / 6.0;
		case QuantizeGrid::off:
		default: return 0.0;
	}
}

void D110SequencerEngine::quantizeTrack(int index, QuantizeGrid grid) {
	jassert(index >= 0 && index < kNumTracks);
	trackAt(index).quantize = grid;
	const double step = gridBeats(grid);
	if (step <= 0.0) return;

	auto &seq = trackAt(index).events;
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

QuantizeGrid D110SequencerEngine::getTrackQuantize(int index) const { return trackAt(index).quantize; }

void D110SequencerEngine::renderInto(juce::MidiBuffer &midiMessages, int numSamples, double sampleRate,
                                      std::vector<MetronomeClick> *clicksOut) {
	if (numSamples <= 0 || sampleRate <= 0.0 || !playing) return;

	const double beatsPerSample = (tempoBpm / 60.0) / sampleRate;
	const double windowStart = positionBeats;
	const double windowEnd = positionBeats + beatsPerSample * numSamples;

	const bool solo = anySoloed();
	for (int t = 0; t < kNumTracks; ++t) {
		// Even the armed track, mid-take, plays back its own already-committed content
		// normally here - new captures go to recordBuffer (a separate sequence this loop
		// never touches), not into the track itself, until stopRecording() folds them in.
		// That's what makes overdub's existing material actually audible while recording.
		const auto &track = trackAt(t);
		if (track.muted) continue;
		if (solo && !track.soloed) continue;

		const auto &seq = track.events;
		for (int i = seq.getNextIndexAtTime(windowStart); i < seq.getNumEvents(); ++i) {
			const auto *ev = seq.getEventPointer(i);
			const double ts = ev->message.getTimeStamp();
			if (ts >= windowEnd) break;
			if (!isNoteEvent(ev->message)) continue; // tracks are note-only in this model
			auto msg = ev->message;
			if (msg.getChannel() > 0) msg.setChannel(channelForTrack(t));
			int sampleOffset = juce::roundToInt((ts - windowStart) / beatsPerSample);
			sampleOffset = juce::jlimit(0, numSamples - 1, sampleOffset);
			midiMessages.addEvent(msg, sampleOffset);
		}
	}

	if (clicksOut != nullptr && metronomeEnabled) {
		// Metronome clicks on the meter's own reporting subdivision (a quarter in 4/4,
		// an eighth in 6/8, ...), not on the quarter-note beat unit events are stored in.
		const double clickGrid = 4.0 / static_cast<double>(timeSigDen);
		const double bar = barLengthBeats();
		const int clicksPerBar = clickGrid > 0.0 ? juce::roundToInt(bar / clickGrid) : 0;
		double nextClickBeat = std::ceil(windowStart / clickGrid - 1.0e-9) * clickGrid;
		while (nextClickBeat < windowEnd) {
			if (nextClickBeat >= windowStart) {
				int sampleOffset = juce::roundToInt((nextClickBeat - windowStart) / beatsPerSample);
				sampleOffset = juce::jlimit(0, numSamples - 1, sampleOffset);
				const int clickIndex =
				    clicksPerBar > 0 ? juce::roundToInt(nextClickBeat / clickGrid) % clicksPerBar : 0;
				clicksOut->push_back({sampleOffset, clickIndex == 0});
			}
			nextClickBeat += clickGrid;
		}
	}

	positionBeats = windowEnd;
}

void D110SequencerEngine::captureEvent(const juce::MidiMessage &message, double atBeats) {
	if (!recording || armedTrack < 0) return;
	if (atBeats < recordStartBeats) return;
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

	for (int t = 0; t < kNumTracks; ++t) {
		juce::MidiMessageSequence seq;
		const int channel = channelForTrack(t);
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

	// Our own saveMidiFile() writes a leading meta-only track before the 9 note
	// tracks; detect and skip it so both our own files and a plain 9-track import
	// line up with tracks[0..8].
	int startTrack = 0;
	if (mf.getNumTracks() >= kNumTracks + 1) {
		const auto *t0 = mf.getTrack(0);
		bool track0HasNotes = false;
		for (int i = 0; i < t0->getNumEvents(); ++i)
			if (t0->getEventPointer(i)->message.isNoteOnOrOff()) {
				track0HasNotes = true;
				break;
			}
		if (!track0HasNotes) startTrack = 1;
	}

	for (int t = 0; t < kNumTracks; ++t) {
		juce::MidiMessageSequence fresh;
		const int srcIndex = startTrack + t;
		if (srcIndex < mf.getNumTracks()) {
			const auto *seq = mf.getTrack(srcIndex);
			for (int i = 0; i < seq->getNumEvents(); ++i) {
				auto msg = seq->getEventPointer(i)->message;
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
	}

	setTempo(newTempo);
	setTimeSignature(newNum, newDen);
	return true;
}

juce::MemoryBlock D110SequencerEngine::trackToBytes(int index) const {
	jassert(index >= 0 && index < kNumTracks);
	juce::MidiFile mf;
	constexpr int tpqn = 960;
	mf.setTicksPerQuarterNote(tpqn);

	juce::MidiMessageSequence seq;
	const auto &src = trackAt(index).events;
	for (int i = 0; i < src.getNumEvents(); ++i) {
		auto msg = src.getEventPointer(i)->message;
		msg.setTimeStamp(msg.getTimeStamp() * tpqn);
		seq.addEvent(msg);
	}
	mf.addTrack(seq);

	juce::MemoryOutputStream out;
	mf.writeTo(out);
	return out.getMemoryBlock();
}

void D110SequencerEngine::trackFromBytes(int index, const void *data, size_t size) {
	jassert(index >= 0 && index < kNumTracks);
	if (size == 0) {
		trackAt(index).events.clear();
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
	trackAt(index).events = std::move(fresh);
}

} // namespace d110seq
