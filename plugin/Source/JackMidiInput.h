#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <jack/jack.h>

// A minimal, optional JACK MIDI input port, independent of JUCE's own MIDI I/O (which on
// Linux only ever reaches ALSA sequencer clients directly - see PluginProcessor.cpp's own
// comment on setMidiInputDevice()/"MIDI ports opened directly"). Gives the Standalone app a
// real "<client name>:midi_in" port that shows up in any JACK patchbay (qjackctl, Catia,
// qpwgraph...), wired up by the user explicitly instead of by picking one fixed ALSA device
// from a dropdown - Alan's request (2026-08-20): a DAW's own MIDI Out should be able to feed
// this directly, instead of a hardware keyboard feeding both the DAW and this app at once.
//
// Fully optional at both compile time (only built when libjack dev headers were found - see
// CMakeLists.txt's D110_HAVE_JACK_MIDI) and run time (silently stays closed if no JACK
// server is reachable, JackNoStartServer - no popup, no log spam for the common case of a
// user who doesn't run JACK/PipeWire-JACK at all).
class JackMidiInput {
public:
	// callback runs on JACK's own realtime process thread - keep it as cheap as the existing
	// juce::MidiInput callback already feeding the same collector elsewhere in this app.
	JackMidiInput(const juce::String &clientName, std::function<void(const juce::MidiMessage &)> callback);
	~JackMidiInput();

	JackMidiInput(const JackMidiInput &) = delete;
	JackMidiInput &operator=(const JackMidiInput &) = delete;

	bool isOpen() const noexcept { return client != nullptr; }

private:
	static int processCallback(jack_nframes_t nframes, void *arg);

	std::function<void(const juce::MidiMessage &)> onMessage;
	jack_client_t *client = nullptr;
	jack_port_t *inputPort = nullptr;
};
