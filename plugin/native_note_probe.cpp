// Phase 4 verification gate: does a MIDI note-on, fed through pushMidi() and paced into the
// CPU's own serial receiver at 3125 bytes/sec, actually reach the firmware and come back out
// as a NoteEvent via popNoteEvent() - the same round trip note_latency_probe.cpp measures
// against the MAME-backed D110Core, and the thing this whole port exists to make jitter-free.
#include "Source/native/D110CoreNative.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// A virgin (never factory-reset) battery RAM has no valid channel assignment - real
	// hardware documents this explicitly (roland_d10.cpp's own top-of-file comment) and it's
	// why bare pushMidi() got nothing the first time this probe ran. Load a properly
	// factory-reset folder instead (see mame_factory_reset_tool.cpp for how it was made -
	// through the real MAME-backed core's own factoryReset(), then loaded here via the
	// cross-core NVRAM compatibility Phase 3 already proved).
	const char *nvramDir = argc > 1 ? argv[1] : "C:/temp/claude/mame_factory_reset_nvram";
	D110CoreNative core;
	if (!core.start("C:/Program Files/Common Files/VST3/D-110 Data", nvramDir)) {
		std::printf("failed to start\n");
		return 1;
	}
	// Matches PluginProcessor::setPoweredOn()'s real, unconditional choice - D110CoreNative
	// itself now defaults to Off, same as D110Core's own default.
	core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Stub);

	// Real hardware (and the MAME-backed core, at real wall-clock speed) needs several
	// seconds before it is ready to register notes - see core_test.cpp's own comment.
	// Same amount of emulated CPU time here, just not spent at real wall-clock speed.
	const double warmup = argc > 2 ? std::atof(argv[2]) : 9.0;
	core.verboseNoteWatchForTest = argc > 3;   // pass a 4th (any) arg to see the whole boot
	std::printf("warming up (%.1fs emulated)...\n", warmup);
	core.runForSeconds(warmup);

	// Part 1 answers on MIDI channel 2 (1-based) per the mirror region comment in
	// D110Core.cpp/ram_mirror.cpp - status byte 0x91 = note-on, channel 2 (0-based 1).
	const uint8_t noteOn[3] = { 0x91, 60, 100 };
	std::printf("sending note-on (ch2, note 60, vel 100)...\n");
	core.verboseNoteWatchForTest = true;
	core.pushMidi(noteOn, 3);

	std::printf("serialRxReady before: %s\n", core.serialRxReadyForTest() ? "true" : "false");

	D110CoreNative::NoteEvent ev;
	bool sawNoteOn = false;
	const double stepSeconds = 0.05;
	double waited = 0.0;
	bool loggedReady = false;
	while (waited < 5.0) {
		core.runForSeconds(stepSeconds);
		waited += stepSeconds;
		if (!loggedReady && core.serialRxReadyForTest()) {
			std::printf("serialRxReady became true @ %.2fs (pc=%04x)\n", waited, core.pcForTest());
			loggedReady = true;
		}
		while (core.popNoteEvent(ev)) {
			std::printf("NoteEvent @ %.2fs: part=%d note=%d vel=%d %s\n",
			            waited, ev.part, ev.note, ev.velocity, ev.on ? "ON" : "OFF");
			if (ev.on && ev.note == 60) sawNoteOn = true;
		}
		if (sawNoteOn) break;
	}

	bool sawNoteOff = false;
	if (sawNoteOn) {
		core.runForSeconds(0.5);
		const uint8_t noteOff[3] = { 0x81, 60, 0 };
		std::printf("\nvoiceCtxWrites before note-off: %llu\n", (unsigned long long)core.voiceCtxWriteCountForTest());
		std::printf("sending note-off (ch2, note 60)...\n");
		core.verboseNoteWatchForTest = true;
		core.pushMidi(noteOff, 3);
		double waitedOff = 0.0;
		while (waitedOff < 10.0) {
			core.runForSeconds(stepSeconds);
			waitedOff += stepSeconds;
			while (core.popNoteEvent(ev)) {
				std::printf("NoteEvent @ %.2fs: part=%d note=%d vel=%d %s\n",
				            waitedOff, ev.part, ev.note, ev.velocity, ev.on ? "ON" : "OFF");
				if (!ev.on && ev.note == 60) sawNoteOff = true;
			}
			if (sawNoteOff) break;
		}
		std::printf("%s\n", sawNoteOff ? "note-off PASS" : "note-off FAIL");
		std::printf("stuckLoopHits=%llu extIntHigh=%s la32Pending=%s pc=%04x\n",
		            (unsigned long long)core.stuckLoopHitsForTest(),
		            core.extIntHighForTest() ? "true" : "false",
		            core.la32PendingForTest() ? "true" : "false", core.pcForTest());
	}

	std::printf("\nfirmwareNoteOns=%llu firmwareNoteOffs=%llu midiPending=%zu voiceCtxWrites=%llu\n",
	            (unsigned long long)core.firmwareNoteOns(), (unsigned long long)core.firmwareNoteOffs(),
	            core.midiQueuePendingForTest(), (unsigned long long)core.voiceCtxWriteCountForTest());
	std::printf("%s\n", sawNoteOn ? "PASS" : "FAIL (no matching note-on seen within 5s)");
	return (sawNoteOn && sawNoteOff) ? 0 : 1;
}
