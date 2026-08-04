// Diagnostic: exactly the same experiment as native_note_probe.cpp (start with a factory-
// reset nvram, wait, pushMidi() a raw note-on, wait, check for a NoteEvent) but against the
// real MAME-backed D110Core directly - no PluginProcessor/JUCE layer at all. Isolates whether
// a raw pushMidi() note-on reaching popNoteEvent() needs anything beyond what
// native_note_probe.cpp already does, before assuming the native core has a bug.
#include "Source/D110Core.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

const char *kRomPath =
	"C:\\Users\\bd260\\Downloads\\MAME 0.288 ROMs (non-merged);"
	"C:\\Users\\bd260\\Downloads\\MAME_0.288_ROMs_[merged]";

int main(int argc, char **argv) {
	const std::string nvram = (argc > 1) ? argv[1] : "C:/temp/claude/mame_factory_reset_nvram";

	D110Core core;
	std::printf("starting (nvram dir: %s)\n", nvram.c_str());
	core.start(kRomPath, nvram);
	for (int i = 0; i < 12 && !core.isRunning(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	std::printf("running: %s\n", core.isRunning() ? "yes" : "NO");
	std::this_thread::sleep_for(std::chrono::seconds(9));

	const uint8_t noteOn[3] = { 0x91, 60, 100 };
	std::printf("pushMidi note-on (ch2, note 60, vel 100)...\n");
	core.pushMidi(noteOn, 3);

	D110Core::NoteEvent ev;
	bool sawNoteOn = false;
	for (int i = 0; i < 100 && !sawNoteOn; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		while (core.popNoteEvent(ev)) {
			std::printf("NoteEvent @ %.2fs: part=%d note=%d vel=%d %s\n",
			            i * 0.05, ev.part, ev.note, ev.velocity, ev.on ? "ON" : "OFF");
			if (ev.on && ev.note == 60) sawNoteOn = true;
		}
	}

	std::printf("\nfirmwareNoteOns=%llu firmwareNoteOffs=%llu midiDelivered=%llu midiDropped=%llu\n",
	            (unsigned long long)core.firmwareNoteOns(), (unsigned long long)core.firmwareNoteOffs(),
	            (unsigned long long)core.midiDelivered(), (unsigned long long)core.midiDropped());
	std::printf("note-on: %s\n", sawNoteOn ? "PASS" : "FAIL");

	bool sawNoteOff = false;
	if (sawNoteOn) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		const uint8_t noteOff[3] = { 0x81, 60, 0 };
		std::printf("pushMidi note-off (ch2, note 60)...\n");
		core.pushMidi(noteOff, 3);
		for (int i = 0; i < 100 && !sawNoteOff; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			while (core.popNoteEvent(ev)) {
				std::printf("NoteEvent @ %.2fs: part=%d note=%d vel=%d %s\n",
				            i * 0.05, ev.part, ev.note, ev.velocity, ev.on ? "ON" : "OFF");
				if (!ev.on && ev.note == 60) sawNoteOff = true;
			}
		}
		std::printf("note-off: %s\n", sawNoteOff ? "PASS" : "FAIL");
	}

	core.stop();
	return (sawNoteOn && sawNoteOff) ? 0 : 1;
}
