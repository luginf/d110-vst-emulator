// Phase 3 verification gate, RAM-mirror half: boots the native core, runs it past the
// boot-settle resync (D110Core's own 4-second delay before it trusts the firmware's RAM is
// no longer mid-init), and checks that popSysex() actually starts handing back well-formed
// Roland DT1 messages - the bridge that would drive mt32emu once this core is wired into the
// plugin for real.
#include "Source/native/D110CoreNative.h"

#include <cstdio>

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110CoreNative core;
	if (!core.start("C:/Program Files/Common Files/VST3/D-110 Data")) {
		std::printf("failed to start\n");
		return 1;
	}

	// Past the 4s boot-settle delay, with margin.
	core.runForSeconds(6.0);

	int total = 0;
	int wellFormed = 0;
	uint8_t msg[256];
	int len;
	while ((len = core.popSysex(msg)) > 0) {
		++total;
		const bool ok = len >= 9 && msg[0] == 0xF0 && msg[1] == 0x41 && msg[2] == 0x10
		             && msg[3] == 0x16 && msg[4] == 0x12 && msg[len - 1] == 0xF7;
		if (ok) ++wellFormed;
		if (total <= 5)
			std::printf("msg %2d: len=%3d  addr=%02X %02X %02X  %s\n",
			            total, len, msg[5], msg[6], msg[7], ok ? "well-formed" : "MALFORMED");
	}

	std::printf("\ntotal sysex messages: %d, well-formed: %d\n", total, wellFormed);
	std::printf("%s\n", (total > 0 && total == wellFormed) ? "PASS" : "FAIL");
	return (total > 0 && total == wellFormed) ? 0 : 1;
}
