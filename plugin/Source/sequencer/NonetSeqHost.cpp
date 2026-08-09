#include "NonetSeqHost.h"

#include "D110SequencerSongsFile.h"

NonetSeqHost::NonetSeqHost() {
	// Factory D-110 channel map (Part 1-8 -> MIDI channels 2-9, Rhythm -> 10) - a real
	// D-110 plugged into this app's MIDI Out on its own factory defaults therefore just
	// works without reconfiguring either side. Unlike the plugin (which reads its channel
	// map live off the firmware's own SYSTEM page), this app has no firmware, so
	// trackChannels is real per-track state here, editable via setTrackChannel() - the
	// panel's CH readout, clickable only when supportsTrackChannelEdit() is true.
	for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t)
		trackChannels[static_cast<size_t>(t)] =
			t == d110seq::D110SequencerEngine::kRhythmTrack ? 10 : t + 2;
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

	// Whatever arrived on the system MIDI In port since the last block - the standalone
	// app's only source of live notes, there being no host and no on-screen keyboard here.
	juce::MidiBuffer fromPort;
	midiCollector.removeNextBlockOfMessages(fromPort, numSamples);

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
	// The metronome's audible click itself is not synthesized here - there is no audio
	// output in this app worth the name (see the class header comment) - so only the
	// visual LED strip and the "use rhythm channel" MIDI click (already inside
	// sequencerOut when that mode is on) are how this app's own metronome is heard.

	const juce::ScopedLock lock(osMidiLock);
	if (osMidiOut != nullptr && sequencerOut.getNumEvents() > 0) osMidiOut->sendBlockOfMessagesNow(sequencerOut);
}

void NonetSeqHost::handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &m) {
	midiCollector.addMessageToQueue(m);
}

void NonetSeqHost::setTrackChannel(int track, int channel) {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kNumTracks) return;
	trackChannels[static_cast<size_t>(track)] = juce::jlimit(1, 16, channel);
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
	d110seq::exportSongsFile(engine, file);
}

void NonetSeqHost::importSequencerSongs(const juce::File &file) {
	d110seq::importSongsFile(engine, file);
}

void NonetSeqHost::midiPanic() {
	juce::MidiBuffer panicOut;
	for (int channel = 0; channel < 16; ++channel) {
		panicOut.addEvent(juce::MidiMessage::controllerEvent(channel + 1, 64, 0), 0);
		panicOut.addEvent(juce::MidiMessage::controllerEvent(channel + 1, 123, 0), 0);
	}
	const juce::ScopedLock lock(osMidiLock);
	if (osMidiOut != nullptr) osMidiOut->sendBlockOfMessagesNow(panicOut);
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

	keyboardMidiChannel = juce::jlimit(1, 16, xml->getIntAttribute("kbdMidiChannel", keyboardMidiChannel));
	keyboardOmni = xml->getIntAttribute("kbdOmni", keyboardOmni ? 1 : 0) != 0;
	keyboardPcInput = xml->getIntAttribute("kbdPcInput", keyboardPcInput ? 1 : 0) != 0;
	keyboardPcLayout = juce::jlimit(0, 1, xml->getIntAttribute("kbdPcLayout", keyboardPcLayout));
	uiThemeLight = xml->getIntAttribute("uiThemeLight", uiThemeLight ? 1 : 0) != 0;

	for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t)
		setTrackChannel(t, xml->getIntAttribute("chTrack" + juce::String(t), trackChannels[static_cast<size_t>(t)]));

	d110seq::readSongsXml(engine, *xml);
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

	xml.setAttribute("kbdMidiChannel", keyboardMidiChannel);
	xml.setAttribute("kbdOmni", keyboardOmni ? 1 : 0);
	xml.setAttribute("kbdPcInput", keyboardPcInput ? 1 : 0);
	xml.setAttribute("kbdPcLayout", keyboardPcLayout);
	xml.setAttribute("uiThemeLight", uiThemeLight ? 1 : 0);

	for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t)
		xml.setAttribute("chTrack" + juce::String(t), trackChannels[static_cast<size_t>(t)]);

	d110seq::writeSongsXml(engine, xml);

	settingsFile().getParentDirectory().createDirectory();
	xml.writeTo(settingsFile());
}
