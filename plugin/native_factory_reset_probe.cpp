// Verifies D110CoreNative::factoryReset() the same way the MAME-backed tool's output was
// checked by hand earlier this session: patch name "Patch   01", reserve settings summing to
// 32, channel assignment reading 1..9 - all at their known RAM offsets (D110Core.cpp's own
// kMirrorRegions comments). Starts from a virgin (never-reset) nvram dir, same as a factory-
// fresh unit, and does the reset entirely through the native core - no MAME involved anywhere.
#include "Source/native/D110CoreNative.h"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const char *nvramDir = argc > 1 ? argv[1] : "C:/temp/claude/native_factory_reset_test";

	D110CoreNative core;
	if (!core.start("C:/Program Files/Common Files/VST3/D-110 Data", nvramDir)) {
		std::printf("failed to start\n");
		return 1;
	}
	core.setStuckPolicy(D110CoreNative::StuckPolicy::La32Stub);

	std::printf("booting (3s)...\n");
	core.runForSeconds(3.0);

	std::printf("factory reset...\n");
	core.factoryReset();
	std::printf("done (isResetting=%s)\n", core.isResetting() ? "true" : "false");

	uint8_t ram[D110CoreNative::kRamSize];
	core.getRam(ram);

	bool ok = true;
	auto check = [&](const char *label, bool cond) {
		std::printf("%-40s %s\n", label, cond ? "PASS" : "FAIL");
		ok &= cond;
	};

	check("patch name == \"Patch   01\"", std::memcmp(ram, "Patch   01", 10) == 0);

	int reserveSum = 0;
	for (int i = 0; i < 9; ++i) reserveSum += ram[0x2D98 + i];
	check("reserve settings sum to 32", reserveSum == 32);

	bool chanOk = true;
	for (int i = 0; i < 9; ++i) if (ram[0x2D98 + 9 + i] != i + 1) chanOk = false;
	check("channel assignment reads 1..9", chanOk);

	core.stop();
	std::printf("\n%s\n", ok ? "ALL PASS" : "SOME FAILED");
	return ok ? 0 : 1;
}
