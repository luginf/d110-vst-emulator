// Reproduces the user's report under La32Ramps: a few fast notes, a few real chords
// (multiple simultaneous overlapping notes, not native_chord_probe.cpp's all-at-once-then-
// all-off pattern) - and checks whether the CPU's PC ever stops advancing (a genuine hang),
// as opposed to just losing notes.
#include "Source/native/D110CoreNative.h"

#include <cstdio>

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const char *nvramDir = argc > 1 ? argv[1] : "C:/temp/claude/mame_factory_reset_nvram";

	D110CoreNative core;
	if (!core.start("C:/Program Files/Common Files/VST3/D-110 Data", nvramDir)) return 1;
	core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Ramps);
	core.runForSeconds(9.0);

	auto checkAlive = [&](const char *label) {
		const uint16_t pc1 = core.pcForTest();
		core.runForSeconds(0.1);
		const uint16_t pc2 = core.pcForTest();
		// A live CPU visits many different PCs in 0.1s; seeing it land on the exact same
		// address is not proof of a hang by itself (a tight, legitimate loop could do that
		// briefly), but "same PC, and it stays that way for several checks in a row" is.
		std::printf("%-28s pc %04x -> %04x %s\n", label, pc1, pc2, pc1 == pc2 ? "(SAME)" : "");
		return pc1 != pc2;
	};

	D110CoreNative::NoteEvent ev;
	auto drain = [&]() { while (core.popNoteEvent(ev)) {} };

	// A few fast, isolated notes first (matches the user's own description of the order of
	// events - fast notes, THEN chords).
	std::printf("--- fast notes ---\n");
	for (int i = 0; i < 8; ++i) {
		const uint8_t n = uint8_t(60 + i);
		const uint8_t on[3] = { 0x91, n, 100 };
		core.pushMidi(on, 3);
		core.runForSeconds(0.06);
		const uint8_t off[3] = { 0x81, n, 0 };
		core.pushMidi(off, 3);
		core.runForSeconds(0.04);
		drain();
	}
	checkAlive("after fast notes");

	// Real chords: several notes on AT ONCE (genuinely overlapping in the MIDI stream, not
	// sequential), held together, released together, repeated a few times in a row.
	std::printf("\n--- real chords ---\n");
	const uint8_t chordNotes[4] = { 48, 52, 55, 60 };
	for (int chord = 0; chord < 6; ++chord) {
		uint8_t bytes[12];
		for (int i = 0; i < 4; ++i) { bytes[i * 3] = 0x91; bytes[i * 3 + 1] = uint8_t(chordNotes[i] + chord); bytes[i * 3 + 2] = 100; }
		core.pushMidi(bytes, 12);
		if (chord == 2) {
			// Fine-grained trace right through the point where chord 2 hangs.
			for (int step = 0; step < 250; ++step) {
				core.runForSeconds(0.001);
				std::printf("  t=%dms pc=%04x\n", step, core.pcForTest());
			}
		} else {
			core.runForSeconds(0.25);
		}
		drain();
		if (!checkAlive("  mid-chord")) { std::printf("HUNG during chord %d (held)\n", chord); return 1; }

		for (int i = 0; i < 4; ++i) { bytes[i * 3] = 0x81; bytes[i * 3 + 1] = uint8_t(chordNotes[i] + chord); bytes[i * 3 + 2] = 0; }
		core.pushMidi(bytes, 12);
		core.runForSeconds(0.15);
		drain();
		if (!checkAlive("  after release")) { std::printf("HUNG during chord %d (release)\n", chord); return 1; }
	}

	std::printf("\nfinal PC=%04x, no hang detected across the whole run\n", core.pcForTest());
	return 0;
}
