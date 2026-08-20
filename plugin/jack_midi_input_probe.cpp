// Verifies the Standalone app's JACK MIDI input port (Source/JackMidiInput.h/.cpp, Alan's
// request 2026-08-20: a real JACK MIDI port so MusE's own MIDI Out can be routed into the
// D-110 in a patchbay, instead of the D-110's own ALSA device picker grabbing a physical
// keyboard directly and bypassing the DAW). No GUI, no audio device, no ROMs needed - this
// only exercises the JACK client lifecycle and the note reaching D110KeyboardHost's own
// activity array (the same array the on-screen keyboard's remote-activity LEDs read), which
// is downstream of the exact same handleIncomingMidiMessage()/osMidiCollector path the
// existing directly-opened ALSA MIDI input already uses (see PluginProcessor.cpp's own "MIDI
// ports opened directly" comment) - so this also stands in for a JACK-vs-ALSA regression
// check on that shared code.
//
// Needs a real, reachable JACK server (JackNoStartServer - this probe never launches one
// itself). Point JACK_DEFAULT_SERVER at a throwaway one if you don't want to touch a real
// session's graph, e.g.:
//   jackd -n d110test -d dummy -r 48000 -p 512 &
//   JACK_DEFAULT_SERVER=d110test ./jack_midi_input_probe

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <juce_audio_processors/juce_audio_processors.h>

#include "Source/PluginProcessor.h"

#ifdef D110_HAVE_JACK_MIDI
#include <jack/jack.h>
#include <jack/midiport.h>
#endif

namespace {

int failures = 0;

void check(bool condition, const char *what) {
	std::printf("  %s   %s\n", condition ? "ok" : "FAIL", what);
	if (!condition) ++failures;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs) {
	const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
	while (juce::Time::getMillisecondCounter() < deadline) {
		if (predicate()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return predicate();
}

#ifdef D110_HAVE_JACK_MIDI
// 0 = idle, 1 = send note-on requested, 2 = note-on sent, 3 = send note-off requested,
// 4 = note-off sent. Set by the main thread, consumed by the JACK process callback (a
// realtime thread) - the one piece of state genuinely shared across threads here.
std::atomic<int> senderStep{ 0 };
jack_port_t *senderOutPort = nullptr;

int senderProcess(jack_nframes_t nframes, void *) {
	void *buf = jack_port_get_buffer(senderOutPort, nframes);
	jack_midi_clear_buffer(buf);
	const int step = senderStep.load();
	if (step == 1) {
		unsigned char noteOn[3] = { 0x90, 60, 100 };
		jack_midi_event_write(buf, 0, noteOn, 3);
		senderStep.store(2);
	} else if (step == 3) {
		unsigned char noteOff[3] = { 0x80, 60, 0 };
		jack_midi_event_write(buf, 0, noteOff, 3);
		senderStep.store(4);
	}
	return 0;
}
#endif

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;

	// Mirrors exactly what JUCE's real Standalone wrapper does before constructing the
	// filter (juce_CreatePluginFilter.h) - the same mechanism that makes
	// PluginProcessor.cpp's own wrapperType == wrapperType_Standalone checks reliable.
	juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
	D110AudioProcessor processor;
	juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);

	check(processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone,
	      "wrapperType is Standalone right after construction");

	std::printf("-- prepareToPlay: opens the JACK MIDI input port --\n");
	processor.prepareToPlay(48000.0, 512);

#ifndef D110_HAVE_JACK_MIDI
	std::printf("  (skipped - built without D110_HAVE_JACK_MIDI, libjack dev headers not found)\n");
#else
	// Give the JACK client thread a moment to finish activating before we go looking for its
	// port from the outside.
	std::this_thread::sleep_for(std::chrono::milliseconds(300));

	jack_status_t status{};
	auto *sender = jack_client_open("d110_jack_midi_probe_sender", JackNoStartServer, &status);
	check(sender != nullptr,
	      "companion JACK client opened (needs a reachable JACK server - see this probe's own doc comment)");
	if (sender != nullptr) {
		senderOutPort = jack_port_register(sender, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
		check(senderOutPort != nullptr, "companion MIDI output port registered");
		jack_set_process_callback(sender, senderProcess, nullptr);
		check(jack_activate(sender) == 0, "companion client activated");

		// Found by pattern rather than hardcoding JucePlugin_Name, matching what a real
		// patchbay search would actually show a user.
		juce::String targetPort;
		const char **ports = jack_get_ports(sender, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
		if (ports != nullptr) {
			for (int i = 0; ports[i] != nullptr; ++i) {
				const juce::String name(ports[i]);
				if (name.contains("midi_in")) {
					targetPort = name;
					break;
				}
			}
			jack_free(ports);
		}
		check(targetPort.isNotEmpty(), "the D-110's own JACK MIDI input port is visible via jack_get_ports");
		if (targetPort.isNotEmpty()) std::printf("  (found port: %s)\n", targetPort.toRawUTF8());

		const bool connected =
			targetPort.isNotEmpty() && jack_connect(sender, "d110_jack_midi_probe_sender:out",
			                                         targetPort.toRawUTF8()) == 0;
		check(connected, "companion port connects to the D-110's midi_in port");

		std::printf("-- a note sent over the JACK connection reaches D110KeyboardHost::isNoteActive() --\n");
		check(!processor.isNoteActive(60), "note 60 inactive before anything is sent");

		juce::AudioBuffer<float> audio(2, 512);
		juce::MidiBuffer hostMidi;

		if (connected) {
			senderStep.store(1);
			check(waitUntil([&] { return senderStep.load() == 2; }, 2000), "companion client sent the note-on");
			check(waitUntil(
				      [&] {
					      audio.clear();
					      hostMidi.clear();
					      processor.processBlock(audio, hostMidi);
					      return processor.isNoteActive(60);
				      },
				      2000),
			      "note 60 active once processBlock() has drained the JACK-delivered note-on");

			senderStep.store(3);
			check(waitUntil([&] { return senderStep.load() == 4; }, 2000), "companion client sent the note-off");
			check(waitUntil(
				      [&] {
					      audio.clear();
					      hostMidi.clear();
					      processor.processBlock(audio, hostMidi);
					      return !processor.isNoteActive(60);
				      },
				      2000),
			      "note 60 inactive again once processBlock() has drained the JACK-delivered note-off");
		}

		jack_client_close(sender);
	}
#endif

	std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d CHECK(S) FAILED\n", failures);
	return failures == 0 ? 0 : 1;
}
