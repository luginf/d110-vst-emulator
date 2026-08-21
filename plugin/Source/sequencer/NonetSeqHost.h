#pragma once

// Nonet Sequencer's own D110SequencerHost - see D110SequencerHost.h and NonetSeqMain.cpp.
// No firmware, no ROMs, no sound engine, nothing else to power on: just the engine, a
// fixed D-110-style channel map (Part 1-8 -> MIDI channels 2-9, Rhythm -> 10, the same
// factory default the D-110 plugin's own SYSTEM page starts on, so a real D-110 fed from
// this app's MIDI Out needs no reconfiguring), and the same kind of direct system MIDI
// In/Out ports the plugin's own "beside the host" ports are (see
// D110AudioProcessor::setMidiInputDevice/setMidiOutputDevice) - just not beside anything
// else here, since there is no host and no firmware to also feed.
//
// Driven by a real audio device callback rather than a GUI timer, on purpose: this app's
// entire reason to exist is MIDI timing accuracy for external gear, and a hardware-
// clocked audio callback has far less jitter than anything on the message thread - the
// same reasoning that motivated the native CPU core's own move off MAME's non-audio-
// thread stepping (see CLAUDE.md). The device is opened primarily to get that clock, not
// to be an instrument - there's still no synth here - but since a real output stream is
// already open, the sequencer's own metronome click (see advance()'s own comment) is
// synthesized straight into it the same way PluginProcessor.cpp does for the plugin,
// rather than staying silent the way the rest of this app's output is. If no output
// device is available at all
// (audioDeviceAboutToStart() never fires), the constructor starts a plain juce::Timer
// fallback instead - each tick still measures real elapsed time rather than assuming a
// fixed 10ms (see timerCallback()), so it doesn't drift over a long session, but it's
// still at the mercy of OS scheduling for when each tick actually lands, so more jitter
// than the audio-clocked path.

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include "../D110KeyboardHost.h"
#include "D110SequencerEngine.h"
#include "D110SequencerHost.h"

class NonetSeqHost : public D110SequencerHost,
                      public D110KeyboardHost,
                      private juce::AudioIODeviceCallback,
                      private juce::MidiInputCallback,
                      private juce::Timer {
public:
	NonetSeqHost();
	~NonetSeqHost() override;

	d110seq::D110SequencerEngine &getSequencer() override { return engine; }
	void exportSequencerSongs(const juce::File &file) override;
	void importSequencerSongs(const juce::File &file) override;
	void midiPanic() override;
	juce::File getLastDialogDir() const override { return lastDialogDir; }
	void setLastDialogDir(const juce::File &dir) override { lastDialogDir = dir; }

	// Per-track channel override - see D110SequencerHost.h's own comment on why only this
	// app (no firmware SYSTEM page to set channels from otherwise) implements these.
	// engine's channelSource (set in the constructor) reads straight out of trackChannels,
	// so a change here takes effect immediately, nothing else to notify.
	bool supportsTrackChannelEdit() const override { return true; }
	void setTrackChannel(int track, int channel) override;

	// See D110SequencerHost.h - this is the only host that offers the 7 extra tracks at all.
	bool supportsExtraTracks() const override { return true; }

	// Per-track Program Change - see D110SequencerHost.h. Sent from advance() the moment
	// engine.isPlaying() edges false->true (PLAY or REC, precount included - real gear wants
	// the patch settled before notes start, not after).
	bool supportsProgramChange() const override { return true; }
	int getTrackProgram(int track) const override;
	void setTrackProgram(int track, int program) override;
	int getTrackBank(int track) const override;
	void setTrackBank(int track, int bank) override;
	bool supportsBankLsb() const override { return true; }
	int getTrackBankLsb(int track) const override;
	void setTrackBankLsb(int track, int bankLsb) override;

	// Volume/Pan alongside the Program Change - see D110SequencerHost.h's own comment. Sent as
	// real MIDI CC7/CC10, the 0-100/0-14 musician-facing values scaled to the wire's 0-127.
	bool supportsTrackVolumePan() const override { return true; }
	int getTrackVolume(int track) const override;
	void setTrackVolume(int track, int volume) override;
	int getTrackPan(int track) const override;
	void setTrackPan(int track, int pan) override;

	// Manual "resend now" escape hatch - see D110SequencerHost.h's own comment. Just flags a
	// request; advance() does the actual send (same code as the PLAY/REC edge), same reasoning
	// as D110AudioProcessor's own override (one MIDI producer, the audio thread).
	void resyncProgramChanges() override { resyncRequested.store(true); }

	// Global correction applied to the raw Program Change/Bank Select bytes actually sent
	// (advance()), on top of whatever getTrackProgram()/getTrackBank() already resolved to -
	// separately, since only one of the two might need it. Different synths' manuals number
	// these from 0 or from 1 inconsistently, and there's no way to know which from here, so
	// rather than guessing this just nudges the outgoing byte until playback matches what the
	// manual says it should. Set from the Options screen (NonetSeqMain.cpp's OptionsDialog),
	// Nonet-Seq only - the plugin's own Program Change is self-contained (see
	// D110AudioProcessor::selectTimbreForPart()'s own comment on where its channel/value come
	// from), nothing external to disagree with.
	int getProgramChangeOffset() const { return programChangeOffset; }
	void setProgramChangeOffset(int offset) { programChangeOffset = juce::jlimit(-127, 127, offset); }
	int getBankOffset() const { return bankOffset; }
	void setBankOffset(int offset) { bankOffset = juce::jlimit(-127, 127, offset); }

	// D110KeyboardHost - the on-screen keyboard's notes go into midiCollector, exactly the
	// queue a real MIDI In port's handleIncomingMidiMessage() feeds, so they thru to MIDI
	// Out and get captured while recording by the same path a real controller would use.
	void injectTestNote(int channel, int note, float velocity, bool on) override;
	int getKeyboardMidiChannel() const override { return keyboardMidiChannel; }
	void setKeyboardMidiChannel(int channel) override { keyboardMidiChannel = juce::jlimit(1, 16, channel); }
	bool getKeyboardOmni() const override { return keyboardOmni; }
	void setKeyboardOmni(bool omni) override { keyboardOmni = omni; }
	bool getKeyboardPcInputEnabled() const override { return keyboardPcInput; }
	void setKeyboardPcInputEnabled(bool enabled) override { keyboardPcInput = enabled; }
	int getKeyboardPcLayout() const override { return keyboardPcLayout; }
	void setKeyboardPcLayout(int layout) override { keyboardPcLayout = juce::jlimit(0, 1, layout); }
	// See D110KeyboardHost.h and advance()'s own comment for where remoteNoteActive actually
	// gets written.
	bool isNoteActive(int note) const override {
		return note >= 0 && note < 128 && remoteNoteActive[static_cast<size_t>(note)].load();
	}

	// The mini utility panel's own THEME toggle (d110ui::Theme, see UiTheme.h) - persisted
	// the same way the D-110 plugin's Standalone settings file keeps it.
	bool getUiThemeLight() const { return uiThemeLight; }
	void setUiThemeLight(bool light) { uiThemeLight = light; }

	// Same D-20-style retro sequencer view toggle as the D-110 plugin's
	// D110AudioProcessor::getSequencerRetroMode() - see D110SequencerRetroPanel.h.
	bool getSequencerRetroMode() const { return sequencerRetroMode; }
	void setSequencerRetroMode(bool retro) { sequencerRetroMode = retro; }

	// Direct MIDI ports - what the toolbar's "MIDI In"/"MIDI Out" menus list and pick
	// from, the same idea as D110Panel::showOptionsMenu()'s own MIDI In/Out submenus.
	static juce::Array<juce::MidiDeviceInfo> midiInputs() { return juce::MidiInput::getAvailableDevices(); }
	static juce::Array<juce::MidiDeviceInfo> midiOutputs() { return juce::MidiOutput::getAvailableDevices(); }
	void setMidiInputDevice(const juce::String &identifier);
	void setMidiOutputDevice(const juce::String &identifier);
	juce::String getMidiInputId() const { return selInputId; }
	juce::String getMidiOutputId() const { return selOutputId; }

	// Toolbar's MIDI In LED - lit for a short window after the last message actually
	// arrived on the system MIDI In port (handleIncomingMidiMessage(), MIDI thread). Not
	// set by injectTestNote()/the on-screen keyboard - this is specifically "is a real
	// external port sending", the thing Alan couldn't otherwise see was even reaching the
	// app while chasing the channel-2 bug below.
	bool isMidiInActive() const {
		return juce::Time::getMillisecondCounter() - lastMidiInActivityMs.load() < 120;
	}

	// Whole-app state - the 4 song slots plus which MIDI ports were selected - persisted
	// between runs the same way the D-110 plugin's own Standalone settings file works (see
	// architecture.md#firmware-memory-and-plugin-settings), just with nothing else in it.
	static juce::File settingsFile();
	void loadSettings();
	void saveSettings() const;

	// Exposed so NonetSeqMain.cpp's Options dialog can host a real
	// juce::AudioDeviceSelectorComponent against it (device type/output device/sample
	// rate/buffer size - the usual JUCE Audio/MIDI Settings picker). The device chosen
	// here is what the transport's low-jitter clock is tied to (see this class's own
	// header comment) AND, now, what the metronome's audible click actually plays
	// through when it isn't routed via the rhythm channel instead - see advance()'s own
	// comment.
	juce::AudioDeviceManager &getAudioDeviceManager() { return deviceManager; }

private:
	void audioDeviceIOCallbackWithContext(const float *const *inputChannelData, int numInputChannels,
	                                       float *const *outputChannelData, int numOutputChannels,
	                                       int numSamples, const juce::AudioIODeviceCallbackContext &) override;
	void audioDeviceAboutToStart(juce::AudioIODevice *device) override;
	void audioDeviceStopped() override;
	void handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &) override;
	void timerCallback() override; // fallback clock, only running if no audio device opened
	void startFallbackTimer(); // (re)starts timerCallback() ticking, resetting its clock state

	// Shared by the real callback and the timer fallback - advances the engine by
	// numSamples at currentSampleRate, capturing/step-recording live input the same way
	// PluginProcessor::processBlock does, then forwards whatever the sequencer played to
	// osMidiOut. See that method's own comment for why this mirrors it closely.
	// outputChannelData/numOutputChannels are non-null only when called from the real
	// audio callback (never from the timer fallback, which has no buffer to write into)
	// - when present, the metronome's audible click is synthesized straight into them.
	void advance(int numSamples, float *const *outputChannelData = nullptr, int numOutputChannels = 0);

	d110seq::D110SequencerEngine engine;
	juce::AudioDeviceManager deviceManager;
	bool usingAudioClock = false;
	double currentSampleRate = 44100.0;

	// Fallback-timer clock state (timerCallback(), only relevant while usingAudioClock is
	// false) - see timerCallback()'s own comment for why this is a running real-time
	// accumulator rather than a fixed samples-per-tick advance.
	double timerLastMs = 0.0;
	double timerMsAccumulator = 0.0;

	// Metronome click synthesis state - see advance()'s own comment. Same short decaying
	// sine approach and same member-variable-carried-decay reasoning as
	// PluginProcessor.h's identically-named fields (a click's ~30ms decay usually outlives
	// the audio block it started in).
	int metronomeSamplesRemaining = 0;
	double metronomePhase = 0.0;
	double metronomeFreq = 1000.0;

	juce::MidiMessageCollector midiCollector;
	std::unique_ptr<juce::MidiInput> osMidiIn;
	std::unique_ptr<juce::MidiOutput> osMidiOut;
	juce::CriticalSection osMidiLock;
	juce::String selInputId, selOutputId;
	juce::File lastDialogDir;
	std::atomic<juce::uint32> lastMidiInActivityMs { 0 };

	// Factory default (Part 1-8 -> channels 2-9, Rhythm -> 10, extra tracks 10-16 -> whatever
	// channels that leaves free: 1, 11-16) until setTrackChannel() overrides an entry - see
	// the constructor, which points engine's channelSource at this array directly. Sized to
	// kMaxTracks unconditionally (not just when extra tracks are enabled) so a channel chosen
	// for an extra track before disabling them, or before this session, isn't lost.
	std::array<int, d110seq::D110SequencerEngine::kMaxTracks> trackChannels;
	// The fixed Program Change/Bank/BankLsb/Volume/Pan override itself now lives directly on
	// D110SequencerEngine, per song slot (2026-08-21, Alan's explicit correction - it used to
	// be workspace-wide here too, shared by all 4 songs, which he pointed out makes no sense).
	// getTrackProgram()/setTrackProgram()/etc. below just delegate to it; no storage of their
	// own here any more.
	bool wasPlayingForProgramSend = false;
	// Set by resyncProgramChanges() (UI thread), cleared by advance() once acted on - see its
	// own comment.
	std::atomic<bool> resyncRequested{false};
	// See getProgramChangeOffset()/getBankOffset() above.
	int programChangeOffset = 0;
	int bankOffset = 0;

	// See isNoteActive() above - one flag per MIDI note number, written from advance() (the
	// audio/timer thread), read from the message thread by D110Keyboard's own low-Hz timer.
	std::array<std::atomic<bool>, 128> remoteNoteActive{};

	// D110Keyboard's own config - see the accessors above.
	int keyboardMidiChannel = 1;
	bool keyboardOmni = false;
	bool keyboardPcInput = false;
	int keyboardPcLayout = 0;

	bool uiThemeLight = false;
	bool sequencerRetroMode = false;
};
