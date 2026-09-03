#include "NonetSeqHost.h"

#include <cmath>

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
	engine.setChannelSource([this](int trackIndex) { return trackChannels[static_cast<size_t>(trackIndex)]; });
	// No live sound to read a "current" Program Change/Bank/Volume/Pan from - there's no synth
	// here - so saveMidiFile() instead exports the stored per-track settings directly (the same
	// ones advance() sends over MIDI Out at PLAY/REC), Alan's call (2026-08-19) over leaving the
	// export empty the way it was before Volume/Pan existed. Deliberately NOT
	// programChangeOffset/bankOffset-corrected - that knob corrects today's cable/device, not
	// the song itself, so it shouldn't get baked into a file that might be reopened elsewhere.
	// Bank/Bank LSB are only consulted when a Program Change is also present - see
	// setBankSource()'s own comment.
	engine.setProgramSource([this](int t) { return getTrackProgram(t); });
	engine.setBankSource(
		[this](int t) { return getTrackProgram(t) >= 0 ? getTrackBank(t) : -1; });
	engine.setBankLsbSource(
		[this](int t) { return getTrackProgram(t) >= 0 ? getTrackBankLsb(t) : -1; });
	engine.setVolumeSource([this](int t) { return getTrackVolume(t); });
	engine.setPanSource([this](int t) { return getTrackPan(t); });

	midiCollector.reset(currentSampleRate);

	// Read the settings file's saved audio-device state (if any) up front, so a
	// previously-chosen device (see getAudioDeviceManager(), used by NonetSeqMain.cpp's
	// Audio Settings dialog) is what initialise() opens directly below, rather than
	// always the OS default followed by an audible/clock-glitching switch the moment
	// loadSettings() ran. Everything else in the settings file is still read by
	// loadSettings() further down, same as before.
	std::unique_ptr<juce::XmlElement> savedDeviceState;
	if (std::unique_ptr<juce::XmlElement> settingsXml = juce::XmlDocument::parse(settingsFile()))
		if (auto *devXml = settingsXml->getChildByName("DEVICESETUP"))
			savedDeviceState = std::make_unique<juce::XmlElement>(*devXml);

	deviceManager.addAudioCallback(this);
	// 0 in / 2 out: nothing is actually rendered (see this class's own header comment on
	// why an audio device is opened at all) - 2 output channels is just the combination
	// every backend this targets can always open, even with no interface plugged in.
	if (deviceManager.initialise(0, 2, savedDeviceState.get(), true).isNotEmpty()) {
		usingAudioClock = false;
		startFallbackTimer();
	}

	loadSettings();
}

void NonetSeqHost::startFallbackTimer() {
	timerLastMs = juce::Time::getMillisecondCounterHiRes();
	timerMsAccumulator = 0.0;
	startTimerHz(100); // ~10ms ticks - see timerCallback() for why this doesn't drift
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
	startFallbackTimer();
}

void NonetSeqHost::audioDeviceIOCallbackWithContext(
	const float *const *, int, float *const *outputChannelData, int numOutputChannels, int numSamples,
	const juce::AudioIODeviceCallbackContext &) {
	for (int ch = 0; ch < numOutputChannels; ++ch)
		if (outputChannelData[ch] != nullptr) juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
	advance(numSamples, outputChannelData, numOutputChannels);
}

void NonetSeqHost::timerCallback() {
	// Only runs when audioDeviceAboutToStart never fired (no usable output device) - see
	// the constructor's own comment. Advances by however many samples' worth of REAL
	// elapsed time have passed since the last tick, not a fixed 10ms - JUCE's Timer only
	// promises "at least" the requested interval between calls, so a tick delayed by OS
	// scheduling (CPU load, another app hogging the message thread, ...) would otherwise
	// silently advance less than the wall-clock time that actually went by, and the
	// transport would run measurably slow over a long session. timerMsAccumulator carries
	// the sub-sample remainder forward each call (same reasoning as
	// D110SequencerEngine::renderInto()'s own precountRemainingBeats residual) so the
	// total samples advanced always converges on real elapsed time, never stalling short
	// of it.
	const double now = juce::Time::getMillisecondCounterHiRes();
	timerMsAccumulator += now - timerLastMs;
	timerLastMs = now;

	const double msPerSample = 1000.0 / currentSampleRate;
	const int samples = static_cast<int>(timerMsAccumulator / msPerSample);
	if (samples <= 0) return;
	timerMsAccumulator -= samples * msPerSample;
	advance(samples);
}

void NonetSeqHost::advance(int numSamples, float *const *outputChannelData, int numOutputChannels) {
	if (numSamples <= 0) return;

	// PLAY/REC edge (precount included, on purpose - see the header comment): send whichever
	// tracks have a Program Change set, once, before anything else this block might send, so
	// an external synth on that channel has already switched patch by the time real notes
	// (thru or sequenced) reach it. Bank Select MSB (CC0) then LSB (CC32) go first, immediately
	// before their track's Program Change, standard MIDI ordering. programChangeOffset/
	// bankOffset (see their own comment) are applied here, at the very last moment, so
	// trackPrograms/trackBanks themselves always stay in the plain musician-facing numbering
	// the dialog shows, regardless of what correction is currently dialled in - bankOffset only
	// ever touches the MSB, same as before LSB existed; LSB has no separate correction knob,
	// nothing asked for one yet.
	const bool nowPlaying = engine.isPlaying();
	const bool doResync = resyncRequested.exchange(false);
	if ((nowPlaying && !wasPlayingForProgramSend) || doResync) {
		juce::MidiBuffer pcOut;
		for (int t = 0; t < engine.activeTrackCount(); ++t) {
			const int program = getTrackProgram(t);
			const int volume = getTrackVolume(t);
			const int pan = getTrackPan(t);
			// Volume/Pan are independent of Program Change - a track can have either without
			// the other - so only skip this track once all three are unset, not just PC.
			if (program < 0 && volume < 0 && pan < 0) continue;
			const int channel = engine.channelForTrack(t);
			if (program >= 0) {
				const int bank = getTrackBank(t);
				const int bankLsb = getTrackBankLsb(t);
				const int rawProgram = juce::jlimit(0, 127, program + programChangeOffset);
				const int rawBank = juce::jlimit(0, 127, (bank - 1) + bankOffset);
				const int rawBankLsb = juce::jlimit(0, 127, bankLsb - 1);
				pcOut.addEvent(juce::MidiMessage::controllerEvent(channel, 0, rawBank), 0);
				pcOut.addEvent(juce::MidiMessage::controllerEvent(channel, 32, rawBankLsb), 0);
				pcOut.addEvent(juce::MidiMessage::programChange(channel, rawProgram), 0);
			}
			// CC7 (Volume)/CC10 (Pan) - musician-facing 0-100/0-14 (the D-110's own LEVEL/PAN
			// ranges, kept as the one scale both hosts show) scaled to the wire's plain 0-127;
			// 7 (centre pan) lands on 64, standard MIDI centre.
			if (volume >= 0) {
				const int rawVolume = juce::jlimit(0, 127, juce::roundToInt(volume * 127.0f / 100.0f));
				pcOut.addEvent(juce::MidiMessage::controllerEvent(channel, 7, rawVolume), 0);
			}
			if (pan >= 0) {
				const int rawPan = juce::jlimit(0, 127, juce::roundToInt(pan * 127.0f / 14.0f));
				pcOut.addEvent(juce::MidiMessage::controllerEvent(channel, 10, rawPan), 0);
			}
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

	// Capture accepts ANY incoming channel while a track is armed, not just the one that
	// track happens to be set to (Alan's request, 2026-08-26): this app's usual setup is one
	// physical USB controller plugged straight into MIDI In, which sends on whatever channel
	// it always sends on (often a fixed 1) - without this, recording onto any other track
	// meant first flipping MIDI Remap on and hand-picking a channel to match, every time a
	// different track got armed. D110AudioProcessor's own equivalent (PluginProcessor.cpp)
	// deliberately keeps the channel match instead - there, a DAW routes several Parts at
	// once over their own distinct channels simultaneously, and capture has to tell them
	// apart; that scenario doesn't apply here. The captured MidiMessage's own channel is
	// never stored anyway (D110SequencerEngine's per-track data is note-only - see
	// captureEvent()'s own comment - playback always goes out on channelForTrack()), so
	// there's nothing to rewrite before capturing, only the gate to drop.
	const double beatsPerSample = (engine.getTempo() / 60.0) / currentSampleRate;
	const double windowStartBeats = engine.getPositionBeats();
	const int armed = engine.isRecording() ? engine.getArmedTrack() : -1;
	if (armed >= 0) {
		for (const auto meta : fromPort) {
			const auto &msg = meta.getMessage();
			if (msg.isNoteOnOrOff())
				engine.captureEvent(msg, windowStartBeats + static_cast<double>(meta.samplePosition) * beatsPerSample);
		}
	}

	const int stepArmed = engine.isStepRecording() ? engine.getArmedTrack() : -1;
	if (stepArmed >= 0) {
		for (const auto meta : fromPort) {
			const auto &msg = meta.getMessage();
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
	// Metronome click: same short decaying sine as PluginProcessor.cpp's identically-
	// shaped block (see its own comment there for why metronomeSamplesRemaining is
	// checked even on blocks with no NEW click - the ~30ms decay usually outlives one
	// audio block). outputChannelData is only non-null from the real audio callback
	// (never the timer fallback, which has nothing to write into - the click is simply
	// inaudible in that degraded mode, same as everything else about it). Skipped when
	// the metronome is routed through the rhythm channel instead - that real MIDI note
	// (already inside sequencerOut, sent below) IS the click sound in that mode.
	if (outputChannelData != nullptr && (!clicks.empty() || metronomeSamplesRemaining > 0)
	    && !engine.getMetronomeUseChannel10()) {
		constexpr double kClickSeconds = 0.03;
		const int clickTotalSamples = juce::jmax(1, static_cast<int>(currentSampleRate * kClickSeconds));
		const float volume = engine.getMetronomeVolume();
		int cursor = 0;
		auto ringUpTo = [&](int endSample) {
			for (int i = cursor; i < endSample; ++i) {
				if (metronomeSamplesRemaining <= 0) continue;
				const float amp = static_cast<float>(metronomeSamplesRemaining) / static_cast<float>(clickTotalSamples);
				const float s = static_cast<float>(std::sin(metronomePhase)) * amp * 0.25f * volume;
				metronomePhase += juce::MathConstants<double>::twoPi * metronomeFreq / currentSampleRate;
				for (int ch = 0; ch < numOutputChannels; ++ch)
					if (outputChannelData[ch] != nullptr) outputChannelData[ch][i] += s;
				--metronomeSamplesRemaining;
			}
		};
		for (const auto &click : clicks) {
			ringUpTo(click.samplePosition);
			cursor = click.samplePosition;
			metronomeSamplesRemaining = clickTotalSamples;
			metronomeFreq = click.downbeat ? 1500.0 : 1000.0;
			metronomePhase = 0.0;
		}
		ringUpTo(numSamples);
	}

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
	if (midiRemap && m.getChannel() > 0) {
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
	return engine.getTrackProgram(track);
}

void NonetSeqHost::setTrackProgram(int track, int program) {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return;
	engine.setTrackProgram(track, program);
}

int NonetSeqHost::getTrackBank(int track) const {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return 1;
	return engine.getTrackBank(track);
}

void NonetSeqHost::setTrackBank(int track, int bank) {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return;
	engine.setTrackBank(track, bank);
}

int NonetSeqHost::getTrackBankLsb(int track) const {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return 1;
	return engine.getTrackBankLsb(track);
}

void NonetSeqHost::setTrackBankLsb(int track, int bankLsb) {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return;
	engine.setTrackBankLsb(track, bankLsb);
}

// MusE-style instrument definition (.idf) - plain XML, one <MidiInstrument name="..."> with
// any number of <PatchGroup name="..."><Patch name="..." prog="0-based" [hbank="0-based"]
// [lbank="0-based"] [drum="1"] /></PatchGroup> children (see e.g.
// /usr/share/muse/instruments/{gm,xg}.idf on a system with MusE installed, or this project's
// own docs/Roland-D110.idf, generated the other direction - real patch names read off the
// firmware, exported so MusE/other DAWs can label the D-110's own Program Change). A drum
// group's own <Patch> entries carry no useful prog/bank distinction for picking a melodic
// Program Change (they're drum kit names, not GM-numbered patches) - skipped rather than
// shown as confusingly-numbered picks.
bool NonetSeqHost::loadInstrumentDefinition(const juce::File &file) {
	std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(file));
	if (xml == nullptr) return false;
	auto *root = xml->hasTagName("MidiInstrument") ? xml.get() : xml->getChildByName("MidiInstrument");
	if (root == nullptr) return false;

	std::vector<InstrumentPatch> parsed;
	for (auto *group : root->getChildWithTagNameIterator("PatchGroup")) {
		const juce::String groupName = group->getStringAttribute("name");
		for (auto *patch : group->getChildWithTagNameIterator("Patch")) {
			if (patch->getBoolAttribute("drum")) continue;
			InstrumentPatch p;
			p.group = groupName;
			p.name = patch->getStringAttribute("name");
			p.prog = patch->getIntAttribute("prog");
			p.bank = patch->hasAttribute("hbank") ? patch->getIntAttribute("hbank") : -1;
			p.bankLsb = patch->hasAttribute("lbank") ? patch->getIntAttribute("lbank") : -1;
			if (p.name.isNotEmpty()) parsed.push_back(p);
		}
	}
	if (parsed.empty()) return false;

	instrumentPatches = std::move(parsed);
	instrumentDefinitionName = root->getStringAttribute("name");
	instrumentDefinitionPath = file.getFullPathName();
	return true;
}

int NonetSeqHost::getTrackVolume(int track) const {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return -1;
	return engine.getTrackVolume(track);
}

void NonetSeqHost::setTrackVolume(int track, int volume) {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return;
	engine.setTrackVolume(track, volume);
}

int NonetSeqHost::getTrackPan(int track) const {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return -1;
	return engine.getTrackPan(track);
}

void NonetSeqHost::setTrackPan(int track, int pan) {
	if (track < 0 || track >= d110seq::D110SequencerEngine::kMaxTracks) return;
	engine.setTrackPan(track, pan);
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
	engine.setQuantizeMode(static_cast<d110seq::QuantizeMode>(xml->getIntAttribute("seqQuantizeMode", 0)));
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
	// "kbdMidiRemap" (2026-08-25) replaced "kbdOmni" - same setting, inverted sense (Omni=on
	// meant no remap). Fall back to the old attribute, inverted, for a save file from before
	// the rename - see PluginProcessor.cpp's own "kbOmni" fallback for the same reasoning.
	if (xml->hasAttribute("kbdMidiRemap"))
		midiRemap = xml->getIntAttribute("kbdMidiRemap", midiRemap ? 1 : 0) != 0;
	else
		midiRemap = xml->getIntAttribute("kbdOmni", midiRemap ? 0 : 1) == 0;
	keyboardPcInput = xml->getIntAttribute("kbdPcInput", keyboardPcInput ? 1 : 0) != 0;
	keyboardPcLayout = juce::jlimit(0, 1, xml->getIntAttribute("kbdPcLayout", keyboardPcLayout));
	keyboardNumOctaves = juce::jlimit(1, 4, xml->getIntAttribute("kbdNumOctaves", keyboardNumOctaves));
	uiThemeLight = xml->getIntAttribute("uiThemeLight", uiThemeLight ? 1 : 0) != 0;
	sequencerRetroMode = xml->getIntAttribute("sequencerRetroMode", sequencerRetroMode ? 1 : 0) != 0;
	retroKeyBindings = xml->getStringAttribute("retroKeyBindings", retroKeyBindings);
	retroLcdCompactMode = xml->getIntAttribute("retroLcdCompactMode", retroLcdCompactMode ? 1 : 0) != 0;
	setProgramChangeOffset(xml->getIntAttribute("pcOffset", 0));
	setBankOffset(xml->getIntAttribute("bankOffset", 0));

	// Re-parse from the persisted path rather than persisting the patch list itself - see
	// D110SequencerHost::getInstrumentDefinitionPath()'s own comment. A moved/deleted file
	// just leaves the picker empty (loadInstrumentDefinition() already handles that failure
	// quietly), not a load error for the whole settings file.
	const juce::String idfPath = xml->getStringAttribute("instrumentDefinitionPath");
	if (idfPath.isNotEmpty()) loadInstrumentDefinition(juce::File(idfPath));

	for (int t = 0; t < d110seq::D110SequencerEngine::kMaxTracks; ++t)
		setTrackChannel(t, xml->getIntAttribute("chTrack" + juce::String(t), trackChannels[static_cast<size_t>(t)]));
	// pcTrack<n>/bankTrack<n>/bankLsbTrack<n>/volTrack<n>/panTrack<n> (a workspace-wide
	// override, one value shared by all 4 songs) are no longer read here - see saveSettings()'s
	// own comment. readSongsXml() below now restores it per slot instead, from that project's
	// own (already-per-slot) seqProgram<slot><track> etc. attributes if saved after the change,
	// or leaves it at "off" (Track's own default) if the settings file predates it.
	//
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
	xml.setAttribute("seqQuantizeMode", static_cast<int>(engine.getQuantizeMode()));
	xml.setAttribute("seqStepGrid", static_cast<int>(engine.getStepDuration()));
	xml.setAttribute("seqStepDotted", engine.getStepDotted() ? 1 : 0);
	xml.setAttribute("seqLoopMode", static_cast<int>(engine.getLoopMode()));
	xml.setAttribute("seqPunchIn", engine.getPunchIn());
	xml.setAttribute("seqPunchOut", engine.getPunchOut());
	xml.setAttribute("seqExtraTracks", engine.getExtraTracksEnabled() ? 1 : 0);

	xml.setAttribute("kbdMidiChannel", keyboardMidiChannel);
	xml.setAttribute("kbdMidiRemap", midiRemap ? 1 : 0);
	xml.setAttribute("kbdPcInput", keyboardPcInput ? 1 : 0);
	xml.setAttribute("kbdPcLayout", keyboardPcLayout);
	xml.setAttribute("kbdNumOctaves", keyboardNumOctaves);
	xml.setAttribute("uiThemeLight", uiThemeLight ? 1 : 0);
	xml.setAttribute("sequencerRetroMode", sequencerRetroMode ? 1 : 0);
	xml.setAttribute("retroKeyBindings", retroKeyBindings);
	xml.setAttribute("retroLcdCompactMode", retroLcdCompactMode ? 1 : 0);
	xml.setAttribute("pcOffset", programChangeOffset);
	xml.setAttribute("bankOffset", bankOffset);
	xml.setAttribute("instrumentDefinitionPath", instrumentDefinitionPath);

	for (int t = 0; t < d110seq::D110SequencerEngine::kMaxTracks; ++t)
		xml.setAttribute("chTrack" + juce::String(t), trackChannels[static_cast<size_t>(t)]);
	// The per-track Program Change/Bank/BankLsb/Volume/Pan override used to be saved here
	// (pcTrack<n>/bankTrack<n>/etc., a workspace preference) - it's per-slot data now
	// (2026-08-21, Alan's explicit correction: sharing it across all 4 songs "n'a pas de sens"),
	// saved by writeSongsXml() below along with everything else that's part of a song.
	//
	// kMaxTracks unconditionally - see D110SequencerSongsFile.h's own comment on why.
	d110seq::writeSongsXml(engine, xml, d110seq::D110SequencerEngine::kMaxTracks);

	// Device type/output device/sample rate/buffer size chosen via the Audio Settings
	// dialog (NonetSeqMain.cpp) - read back by the constructor, before initialise(), so
	// next launch opens straight into the same device rather than the OS default.
	// createStateXml() always tags the element "DEVICESETUP" itself (see
	// AudioDeviceManager::updateXml()); the constructor looks it up by that same name.
	if (auto deviceState = deviceManager.createStateXml()) xml.addChildElement(deviceState.release());

	settingsFile().getParentDirectory().createDirectory();
	xml.writeTo(settingsFile());
}
