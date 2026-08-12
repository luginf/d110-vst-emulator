#include "NonetSeqHost.h"

#include "D110SequencerSongsFile.h"

NonetSeqHost::NonetSeqHost() {
	// Factory D-110 channel map (Part 1-8 -> MIDI channels 2-9, Rhythm -> 10) - a real
	// D-110 plugged into this app's MIDI Out on its own factory defaults therefore just
	// works without reconfiguring either side. Unlike the plugin (which reads its channel
	// map live off the firmware's own SYSTEM page), this app has no firmware, so
	// trackChannels is real per-track state here, editable via setTrackChannel() - the
	// panel's CH readout, clickable only when supportsTrackChannelEdit() is true.
	// Tracks 9-15 (the extra 7, only reachable with extra tracks enabled - see
	// D110SequencerHost::supportsExtraTracks()) get whatever channels the 9 above don't
	// already use: 1, then 11-16.
	for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t)
		trackChannels[static_cast<size_t>(t)] =
			t == d110seq::D110SequencerEngine::kRhythmTrack ? 10 : t + 2;
	for (int t = d110seq::D110SequencerEngine::kNumTracks; t < d110seq::D110SequencerEngine::kMaxTracks; ++t)
		trackChannels[static_cast<size_t>(t)] = t == d110seq::D110SequencerEngine::kNumTracks ? 1 : t + 1;
	trackPrograms.fill(-1);
	engine.setChannelSource([this](int trackIndex) { return trackChannels[static_cast<size_t>(trackIndex)]; });
	// No live "current program" to report for saveMidiFile() - see setProgramSource()'s
	// own comment on what returning < 0 means.
	engine.setProgramSource([](int) { return -1; });

	midiCollector.reset(currentSampleRate);

	deviceManager.addAudioCallback(this);
	// 0 in / 2 out: nothing is actually rendered (see this class's own header comment on
	// why an audio device is opened at all) - 2 output channels is just the combination
	// every backend this targets can always open, even with no interface plugged in.
	if (deviceManager.initialise(0, 2, nullptr, true).isNotEmpty()) {
		usingAudioClock = false;
		startTimerHz(100); // ~10ms ticks - degraded but keeps the transport moving
	}

	loadSettings();
}

NonetSeqHost::~NonetSeqHost() {
	saveSettings();
	stopTimer();
	deviceManager.removeAudioCallback(this);
	if (osMidiIn) osMidiIn->stop();
}

void NonetSeqHost::audioDeviceAboutToStart(juce::AudioIODevice *device) {
	currentSampleRate = device->getCurrentSampleRate() > 0 ? device->getCurrentSampleRate() : 44100.0;
	midiCollector.reset(currentSampleRate);
	usingAudioClock = true;
	stopTimer();
}

void NonetSeqHost::audioDeviceStopped() {
	usingAudioClock = false;
	startTimerHz(100);
}

void NonetSeqHost::audioDeviceIOCallbackWithContext(
	const float *const *, int, float *const *outputChannelData, int numOutputChannels, int numSamples,
	const juce::AudioIODeviceCallbackContext &) {
	for (int ch = 0; ch < numOutputChannels; ++ch)
		if (outputChannelData[ch] != nullptr) juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
	advance(numSamples);
}

void NonetSeqHost::timerCallback() {
	// 100Hz -> 10ms/tick at the nominal sample rate. Only runs when audioDeviceAboutToStart
	// never fired (no usable output device) - see the constructor's own comment.
	advance(static_cast<int>(currentSampleRate * 0.01));
}

void NonetSeqHost::advance(int numSamples) {
	if (numSamples <= 0) return;

	// PLAY/REC edge (precount included, on purpose - see the header comment): send whichever
	// tracks have a Program Change set, once, before anything else this block might send, so
	// an external synth on that channel has already switched patch by the time real notes
	// (thru or sequenced) reach it.
	const bool nowPlaying = engine.isPlaying();
	if (nowPlaying && !wasPlayingForProgramSend) {
		juce::MidiBuffer pcOut;
		for (int t = 0; t < engine.activeTrackCount(); ++t) {
			const int program = trackPrograms[static_cast<size_t>(t)];
			if (program < 0) continue;
			pcOut.addEvent(juce::MidiMessage::programChange(engine.channelForTrack(t), program), 0);
		}
		const juce::ScopedLock lock(osMidiLock);
		if (osMidiOut != nullptr && pcOut.getNumEvents() > 0) osMidiOut->sendBlockOfMessagesNow(pcOut);
	}
	wasPlayingForProgramSend = nowPlaying;

	// Whatever arrived on the system MIDI In port since the last block - the standalone
	// app's only source of live notes, there being no host and no on-screen keyboard here.
	juce::MidiBuffer fromPort;
	midiCollector.removeNextBlockOfMessages(fromPort, numSamples);

	// On-screen keyboard's remote-activity LEDs - see D110KeyboardHost::isNoteActive(). Both
	// halves of what this app can hear go through here: fromPort is the system MIDI In port
	// AND the on-screen keyboard's own notes (injectTestNote feeds the same midiCollector),
	// sequencerOut (marked further below, once rendered) is playback.
	for (const auto meta : fromPort) {
		const auto &msg = meta.getMessage();
		if (!msg.isNoteOnOrOff()) continue;
		const int note = msg.getNoteNumber();
		if (note >= 0 && note < 128) remoteNoteActive[static_cast<size_t>(note)].store(msg.isNoteOn());
	}

	// Thru'd straight to MIDI Out, unlike the plugin's own (sequencer-only) MIDI Out: the
	// plugin always has its internal D-110 emulation to hear what you're playing while you
	// record, this app has no sound engine at all, so without this a live controller would
	// be silent - the external synth reached via MIDI Out is this app's only monitoring.
	{
		const juce::ScopedLock lock(osMidiLock);
		if (osMidiOut != nullptr && fromPort.getNumEvents() > 0) osMidiOut->sendBlockOfMessagesNow(fromPort);
	}

	const double beatsPerSample = (engine.getTempo() / 60.0) / currentSampleRate;
	const double windowStartBeats = engine.getPositionBeats();
	const int armed = engine.isRecording() ? engine.getArmedTrack() : -1;
	if (armed >= 0) {
		const int armedChannel = engine.channelForTrack(armed);
		for (const auto meta : fromPort) {
			const auto &msg = meta.getMessage();
			if (msg.isNoteOnOrOff() && msg.getChannel() == armedChannel)
				engine.captureEvent(msg, windowStartBeats + static_cast<double>(meta.samplePosition) * beatsPerSample);
		}
	}

	const int stepArmed = engine.isStepRecording() ? engine.getArmedTrack() : -1;
	if (stepArmed >= 0) {
		const int armedChannel = engine.channelForTrack(stepArmed);
		for (const auto meta : fromPort) {
			const auto &msg = meta.getMessage();
			if (msg.getChannel() != armedChannel) continue;
			if (msg.isNoteOn()) engine.stepNoteOn(msg.getNoteNumber(), msg.getVelocity());
			else if (msg.isNoteOff()) engine.stepNoteOff(msg.getNoteNumber());
		}
	}

	std::vector<d110seq::D110SequencerEngine::MetronomeClick> clicks;
	juce::MidiBuffer sequencerOut;
	engine.renderInto(sequencerOut, numSamples, currentSampleRate,
	                   engine.getMetronomeEnabled() ? &clicks : nullptr);
	for (const auto meta : sequencerOut) {
		const auto &msg = meta.getMessage();
		if (!msg.isNoteOnOrOff()) continue;
		const int note = msg.getNoteNumber();
		if (note >= 0 && note < 128) remoteNoteActive[static_cast<size_t>(note)].store(msg.isNoteOn());
	}
	// The metronome's audible click itself is not synthesized here - there is no audio
	// output in this app worth the name (see the class header comment) - so only the
	// visual LED strip and the "use rhythm channel" MIDI click (already inside
	// sequencerOut when that mode is on) are how this app's own metronome is heard.

	const juce::ScopedLock lock(osMidiLock);
	if (osMidiOut != nullptr && sequencerOut.getNumEvents() > 0) osMidiOut->sendBlockOfMessagesNow(sequencerOut);
}

void NonetSeqHost::handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &m) {
	// Rechannelized onto the on-screen keyboard's own selected channel first, same as
	// D110AudioProcessor::handleIncomingMidiMessage() - otherwise an external USB controller
	// keeps whatever channel it was sending on (usually 1) regardless of the CH picker here,
	// while the virtual/PC keyboard (injectTestNote, always stamped with keyboardMidiChannel)
	// looked like it worked fine.
	lastMidiInActivityMs.store(juce::Time::getMillisecondCounter());
	if (!keyboardOmni && m.getChannel() > 0) {
		auto msg = m;
		msg.setChannel(keyboardMidiChannel);
		midiCollector.addMessageToQueue(msg);
		return;
	}
	midiCollector.addMessageToQueue(m);
}

void NonetSeqHost::setTrackChannel(int track, int channel) {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return;
	trackChannels[static_cast<size_t>(track)] = juce::jlimit(1, 16, channel);
}

int NonetSeqHost::getTrackProgram(int track) const {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return -1;
	return trackPrograms[static_cast<size_t>(track)];
}

void NonetSeqHost::setTrackProgram(int track, int program) {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return;
	trackPrograms[static_cast<size_t>(track)] = program < 0 ? -1 : juce::jlimit(0, 127, program);
}

void NonetSeqHost::injectTestNote(int channel, int note, float velocity, bool on) {
	auto message = on ? juce::MidiMessage::noteOn(channel, note, velocity)
	                   : juce::MidiMessage::noteOff(channel, note, velocity);
	message.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
	midiCollector.addMessageToQueue(message);
}

void NonetSeqHost::setMidiInputDevice(const juce::String &id) {
	if (osMidiIn) { osMidiIn->stop(); osMidiIn.reset(); }
	selInputId = {};
	if (id.isEmpty()) return;
	osMidiIn = juce::MidiInput::openDevice(id, this);
	if (osMidiIn) { osMidiIn->start(); selInputId = id; }
}

void NonetSeqHost::setMidiOutputDevice(const juce::String &id) {
	std::unique_ptr<juce::MidiOutput> opened;
	if (id.isNotEmpty()) opened = juce::MidiOutput::openDevice(id);
	const juce::ScopedLock lock(osMidiLock);
	osMidiOut = std::move(opened);
	selOutputId = osMidiOut ? id : juce::String();
}

void NonetSeqHost::exportSequencerSongs(const juce::File &file) {
	// kMaxTracks unconditionally - see D110SequencerSongsFile.h's own comment on why.
	d110seq::exportSongsFile(engine, file, d110seq::D110SequencerEngine::kMaxTracks);
}

void NonetSeqHost::importSequencerSongs(const juce::File &file) {
	d110seq::importSongsFile(engine, file, d110seq::D110SequencerEngine::kMaxTracks);
}

void NonetSeqHost::midiPanic() {
	juce::MidiBuffer panicOut;
	for (int channel = 0; channel < 16; ++channel) {
		panicOut.addEvent(juce::MidiMessage::controllerEvent(channel + 1, 64, 0), 0);
		panicOut.addEvent(juce::MidiMessage::controllerEvent(channel + 1, 123, 0), 0);
	}
	const juce::ScopedLock lock(osMidiLock);
	if (osMidiOut != nullptr) osMidiOut->sendBlockOfMessagesNow(panicOut);

	// CC 123 above is an "all notes off" CONTROLLER message, not a literal note-off per note -
	// isNoteActive()'s array only ever gets touched by real note-on/off (see advance()), so
	// without this, the keyboard's remote-activity LEDs would stay lit forever after STOP/
	// panic instead of clearing along with everything else.
	for (auto &a : remoteNoteActive) a.store(false);
}

juce::File NonetSeqHost::settingsFile() {
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
		.getChildFile("Nonet Sequencer.settings");
}

void NonetSeqHost::loadSettings() {
	std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(settingsFile()));
	if (xml == nullptr) return;

	setMidiInputDevice(xml->getStringAttribute("midiIn"));
	setMidiOutputDevice(xml->getStringAttribute("midiOut"));
	lastDialogDir = juce::File(xml->getStringAttribute("lastDialogDir"));

	engine.setMetronomeEnabled(xml->getIntAttribute("seqMetronome", 1) != 0);
	engine.setMetronomeMode(static_cast<d110seq::MetronomeMode>(
		xml->getIntAttribute("seqMetronomeMode", static_cast<int>(d110seq::MetronomeMode::both))));
	engine.setMetronomeUseChannel10(xml->getIntAttribute("seqMetronomeChannel10", 0) != 0);
	engine.setMetronomeRecordOnly(xml->getIntAttribute("seqMetronomeRecordOnly", 0) != 0);
	engine.setMetronomeVolume(static_cast<float>(xml->getDoubleAttribute("seqMetronomeVolume", 1.0)));
	engine.setPrecountBars(xml->getIntAttribute("seqPrecountBars", 1));
	engine.setRecordMode(static_cast<d110seq::RecordMode>(xml->getIntAttribute("seqRecordMode", 0)));
	engine.setStepDuration(static_cast<d110seq::QuantizeGrid>(
		xml->getIntAttribute("seqStepGrid", static_cast<int>(d110seq::QuantizeGrid::eighth))));
	engine.setStepDotted(xml->getIntAttribute("seqStepDotted", 0) != 0);
	engine.setLoopMode(static_cast<d110seq::LoopMode>(xml->getIntAttribute("seqLoopMode", 0)));
	engine.setPunchIn(xml->getIntAttribute("seqPunchIn", engine.getPunchIn()));
	engine.setPunchOut(xml->getIntAttribute("seqPunchOut", engine.getPunchOut()));
	// Read before readSongsXml() below - setExtraTracksEnabled(false) would otherwise disarm
	// a track >= kNumTracks that a stale "seqArmedTrack"-less state might still imply, though
	// in practice armedTrack itself isn't persisted at all (transport state, deliberately not
	// saved - see D110SequencerEngine::pushUndoSnapshot()'s own comment on the same point).
	engine.setExtraTracksEnabled(xml->getIntAttribute("seqExtraTracks", 0) != 0);

	keyboardMidiChannel = juce::jlimit(1, 16, xml->getIntAttribute("kbdMidiChannel", keyboardMidiChannel));
	keyboardOmni = xml->getIntAttribute("kbdOmni", keyboardOmni ? 1 : 0) != 0;
	keyboardPcInput = xml->getIntAttribute("kbdPcInput", keyboardPcInput ? 1 : 0) != 0;
	keyboardPcLayout = juce::jlimit(0, 1, xml->getIntAttribute("kbdPcLayout", keyboardPcLayout));
	uiThemeLight = xml->getIntAttribute("uiThemeLight", uiThemeLight ? 1 : 0) != 0;

	for (int t = 0; t < d110seq::D110SequencerEngine::kMaxTracks; ++t) {
		setTrackChannel(t, xml->getIntAttribute("chTrack" + juce::String(t), trackChannels[static_cast<size_t>(t)]));
		setTrackProgram(t, xml->getIntAttribute("pcTrack" + juce::String(t), -1));
	}

	// kMaxTracks unconditionally - see D110SequencerSongsFile.h's own comment on why.
	d110seq::readSongsXml(engine, *xml, d110seq::D110SequencerEngine::kMaxTracks);
}

void NonetSeqHost::saveSettings() const {
	juce::XmlElement xml("NonetSeqAppState");
	xml.setAttribute("version", 1);
	xml.setAttribute("midiIn", selInputId);
	xml.setAttribute("midiOut", selOutputId);
	xml.setAttribute("lastDialogDir", lastDialogDir.getFullPathName());

	xml.setAttribute("seqMetronome", engine.getMetronomeEnabled() ? 1 : 0);
	xml.setAttribute("seqMetronomeMode", static_cast<int>(engine.getMetronomeMode()));
	xml.setAttribute("seqMetronomeChannel10", engine.getMetronomeUseChannel10() ? 1 : 0);
	xml.setAttribute("seqMetronomeRecordOnly", engine.getMetronomeRecordOnly() ? 1 : 0);
	xml.setAttribute("seqMetronomeVolume", static_cast<double>(engine.getMetronomeVolume()));
	xml.setAttribute("seqPrecountBars", engine.getPrecountBars());
	xml.setAttribute("seqRecordMode", static_cast<int>(engine.getRecordMode()));
	xml.setAttribute("seqStepGrid", static_cast<int>(engine.getStepDuration()));
	xml.setAttribute("seqStepDotted", engine.getStepDotted() ? 1 : 0);
	xml.setAttribute("seqLoopMode", static_cast<int>(engine.getLoopMode()));
	xml.setAttribute("seqPunchIn", engine.getPunchIn());
	xml.setAttribute("seqPunchOut", engine.getPunchOut());
	xml.setAttribute("seqExtraTracks", engine.getExtraTracksEnabled() ? 1 : 0);

	xml.setAttribute("kbdMidiChannel", keyboardMidiChannel);
	xml.setAttribute("kbdOmni", keyboardOmni ? 1 : 0);
	xml.setAttribute("kbdPcInput", keyboardPcInput ? 1 : 0);
	xml.setAttribute("kbdPcLayout", keyboardPcLayout);
	xml.setAttribute("uiThemeLight", uiThemeLight ? 1 : 0);

	for (int t = 0; t < d110seq::D110SequencerEngine::kMaxTracks; ++t) {
		xml.setAttribute("chTrack" + juce::String(t), trackChannels[static_cast<size_t>(t)]);
		xml.setAttribute("pcTrack" + juce::String(t), trackPrograms[static_cast<size_t>(t)]);
	}

	// kMaxTracks unconditionally - see D110SequencerSongsFile.h's own comment on why.
	d110seq::writeSongsXml(engine, xml, d110seq::D110SequencerEngine::kMaxTracks);

	settingsFile().getParentDirectory().createDirectory();
	xml.writeTo(settingsFile());
}
