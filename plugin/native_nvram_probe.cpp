// Phase 3 verification gate, NVRAM half: does D110CoreNative's save/load round-trip actually
// work, and can it read a folder an existing MAME-backed D110Core session wrote? Both matter -
// the first proves the plumbing, the second proves format compatibility (same 32KB raw dump
// nvram_device::DEFAULT_ALL_0 uses), which is a real requirement: a user's existing saved
// patches must still be there if this core is ever switched on.
#include "Source/native/D110CoreNative.h"

#include <cstdio>
#include <cstring>

namespace {
const char *kRoms = "C:/Program Files/Common Files/VST3/D-110 Data";

bool check(const char *label, bool cond) {
	std::printf("%-55s %s\n", label, cond ? "PASS" : "FAIL");
	return cond;
}
} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	bool allOk = true;

	// --- round trip: poke a marker pattern, stop() (writes files), start() again (reads
	// them back), confirm the marker survived exactly. ---
	{
		const char *dir = "C:/temp/claude/native_nvram_roundtrip";
		D110CoreNative core;
		allOk &= check("start (fresh nvram dir)", core.start(kRoms, dir));
		core.pokeRamForTest(0x0010, 0xA5);
		core.pokeRamForTest(0x7FFF, 0x3C);
		core.stop();

		D110CoreNative core2;
		allOk &= check("restart (same nvram dir)", core2.start(kRoms, dir));
		uint8_t ram[D110CoreNative::kRamSize];
		allOk &= check("getRam after restart", core2.getRam(ram));
		allOk &= check("marker byte at 0x0010 survived", ram[0x0010] == 0xA5);
		allOk &= check("marker byte at 0x7FFF survived", ram[0x7FFF] == 0x3C);
		core2.stop();
	}

	// --- cross-core: point at the folder core_test.cpp (the MAME-backed original) already
	// wrote via nvram_device::DEFAULT_ALL_0's own file format - must load without crashing
	// and produce exactly kRamSize bytes. ---
	{
		const char *mameDir = "C:/temp/claude/mame_core_test_nvram2";
		D110CoreNative core;
		allOk &= check("start (existing MAME-backed nvram dir)", core.start(kRoms, mameDir));
		uint8_t ram[D110CoreNative::kRamSize];
		allOk &= check("getRam from MAME-backed folder", core.getRam(ram));
		core.stop();
	}

	std::printf("\n%s\n", allOk ? "ALL PASS" : "SOME FAILED");
	return allOk ? 0 : 1;
}
