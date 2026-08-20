#include "JackMidiInput.h"
#include <jack/midiport.h>

JackMidiInput::JackMidiInput(const juce::String &clientName,
                              std::function<void(const juce::MidiMessage &)> callback)
	: onMessage(std::move(callback)) {
	jack_status_t status{};
	// JackNoStartServer: never launch a JACK server on this app's behalf, just use one if
	// it's already running. That's the difference between "quietly does nothing" (the right
	// behaviour for the many users who never touch JACK) and surprise-starting a whole audio
	// server underneath someone who only wanted to play the D-110.
	auto *c = jack_client_open(clientName.toRawUTF8(), JackNoStartServer, &status);
	if (c == nullptr) return;

	auto *port = jack_port_register(c, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
	if (port == nullptr) {
		jack_client_close(c);
		return;
	}

	client = c;
	inputPort = port;
	jack_set_process_callback(client, processCallback, this);
	if (jack_activate(client) != 0) {
		jack_client_close(client);
		client = nullptr;
		inputPort = nullptr;
	}
}

JackMidiInput::~JackMidiInput() {
	if (client != nullptr) jack_client_close(client);
}

int JackMidiInput::processCallback(jack_nframes_t nframes, void *arg) {
	auto *self = static_cast<JackMidiInput *>(arg);
	void *buf = jack_port_get_buffer(self->inputPort, nframes);
	const jack_nframes_t count = jack_midi_get_event_count(buf);
	// Real elapsed time, not the JACK transport frame position: everything downstream (the
	// collector, then processBlock's own drain) already times messages against
	// Time::getMillisecondCounterHiRes(), same as the existing direct ALSA MIDI input and
	// injectTestNote() both do - see PluginProcessor.cpp's own comments there.
	const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
	for (jack_nframes_t i = 0; i < count; ++i) {
		jack_midi_event_t event{};
		if (jack_midi_event_get(&event, buf, i) != 0 || event.size == 0) continue;
		self->onMessage(juce::MidiMessage(event.buffer, static_cast<int>(event.size), now));
	}
	return 0;
}
