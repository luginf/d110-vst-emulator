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
// Driven by a real (silent) audio device callback rather than a GUI timer, on purpose:
// this app's entire reason to exist is MIDI timing accuracy for external gear, and a
// hardware-clocked audio callback has far less jitter than anything on the message
// thread - the same reasoning that motivated the native CPU core's own move off MAME's
// non-audio-thread stepping (see CLAUDE.md). Nothing is actually rendered to it; a real
// device is opened purely to get that clock. If no output device is available at all
// (audioDeviceAboutToStart() never fires), the constructor starts a plain juce::Timer
// fallback instead - degraded jitter, but still usable.

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

private:
	void audioDeviceIOCallbackWithContext(const float *const *inputChannelData, int numInputChannels,
	                                       float *const *outputChannelData, int numOutputChannels,
	                                       int numSamples, const juce::AudioIODeviceCallbackContext &) override;
	void audioDeviceAboutToStart(juce::AudioIODevice *device) override;
	void audioDeviceStopped() override;
	void handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &) override;
	void timerCallback() override; // fallback clock, only running if no audio device opened

	// Shared by the real callback and the timer fallback - advances the engine by
	// numSamples at currentSampleRate, capturing/step-recording live input the same way
	// PluginProcessor::processBlock does, then forwards whatever the sequencer played to
	// osMidiOut. See that method's own comment for why this mirrors it closely.
	void advance(int numSamples);

	d110seq::D110SequencerEngine engine;
	juce::AudioDeviceManager deviceManager;
	bool usingAudioClock = false;
	double currentSampleRate = 44100.0;

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
	// -1 = none, else a raw 0-127 MIDI program number - see setTrackProgram()/advance().
	std::array<int, d110seq::D110SequencerEngine::kMaxTracks> trackPrograms;
	bool wasPlayingForProgramSend = false;

	// See isNoteActive() above - one flag per MIDI note number, written from advance() (the
	// audio/timer thread), read from the message thread by D110Keyboard's own low-Hz timer.
	std::array<std::atomic<bool>, 128> remoteNoteActive{};

	// D110Keyboard's own config - see the accessors above.
	int keyboardMidiChannel = 1;
	bool keyboardOmni = false;
	bool keyboardPcInput = false;
	int keyboardPcLayout = 0;

	bool uiThemeLight = false;
};
