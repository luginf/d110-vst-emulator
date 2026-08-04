// Stress test beyond native_note_probe.cpp's single note: a 4-note chord, overlapping (all
// held together, then all released together) - the case that actually exercises
// D110CoreNative::serviceLa32Stub()'s per-context slot lookup, since a single note never
// needs to distinguish between voices.
#include "Source/native/D110CoreNative.h"

#include <cstdio>
#include <set>

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const char *nvramDir = argc > 1 ? argv[1] : "C:/temp/claude/mame_factory_reset_nvram";

	D110CoreNative core;
	if (!core.start("C:/Program Files/Common Files/VST3/D-110 Data", nvramDir)) {
		std::printf("failed to start\n");
		return 1;
	}
	core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Stub);
	core.runForSeconds(9.0);

	const uint8_t notes[4] = { 60, 64, 67, 72 };
	for (uint8_t n : notes) {
		const uint8_t on[3] = { 0x91, n, 100 };
		core.pushMidi(on, 3);
		core.runForSeconds(0.02); // stagger slightly, as a real chord strum would
	}
	core.runForSeconds(1.0);

	std::set<int> onSeen, offSeen;
	D110CoreNative::NoteEvent ev;
	while (core.popNoteEvent(ev)) {
		std::printf("NoteEvent: note=%d vel=%d %s\n", ev.note, ev.velocity, ev.on ? "ON" : "OFF");
		if (ev.on) onSeen.insert(ev.note);
	}
	std::printf("notes on: %d/4\n", int(onSeen.size()));

	for (uint8_t n : notes) {
		const uint8_t off[3] = { 0x81, n, 0 };
		core.pushMidi(off, 3);
		core.runForSeconds(0.02);
	}
	core.runForSeconds(2.0);
	while (core.popNoteEvent(ev)) {
		std::printf("NoteEvent: note=%d vel=%d %s\n", ev.note, ev.velocity, ev.on ? "ON" : "OFF");
		if (!ev.on) offSeen.insert(ev.note);
	}
	std::printf("notes off: %d/4\n", int(offSeen.size()));

	const bool ok = onSeen.size() == 4 && offSeen == onSeen;
	std::printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
