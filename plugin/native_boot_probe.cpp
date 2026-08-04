// Phase 1 verification gate for the native (MAME-free) D-110 CPU core - see
// C:\Users\bd260\.claude\plans\lexical-greeting-whistle.md. Links ONLY Mcs96Cpu + D110Bus,
// nothing from MAME_LIBS - proving the native core actually has zero MAME dependency, which
// is the whole point of this port. Boots the real firmware ROM and prints a PC trace (one
// address per real instruction fetched) to stdout, for comparison against a MAME-backed
// reference trace produced by a companion tool.
#include "Source/native/d110_bus.h"
#include "Source/native/mcs96_cpu.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> loadFile(const char *path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return {};
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// Same data folder every other probe in this project auto-loads from
	// (D110AudioProcessor::getAutoRomFolder()) - hardcoded here since this target links
	// nothing from PluginProcessor.cpp on purpose.
	const char *dataDir = "C:/Program Files/Common Files/VST3/D-110 Data";
	auto firmware = loadFile((std::string(dataDir) + "/d-110.v1.10.ic19.bin").c_str());
	auto presets = loadFile((std::string(dataDir) + "/r15179873-lh5310-97.ic12.bin").c_str());
	if (firmware.size() != 0x8000) {
		std::fprintf(stderr, "firmware ROM missing or wrong size (got %zu, want 0x8000)\n", firmware.size());
		return 1;
	}
	if (presets.size() != 0x20000) {
		std::fprintf(stderr, "presets ROM missing or wrong size (got %zu, want 0x20000)\n", presets.size());
		return 1;
	}

	long instructionBudget = argc > 1 ? std::atol(argv[1]) : 20000;

	D110Bus bus;
	bus.setFirmwareRom(firmware.data(), firmware.size());
	bus.setPresetsRom(presets.data(), presets.size());

	Mcs96Cpu cpu;
	cpu.busRead8 = [&bus](uint16_t a) { return bus.read(a); };
	cpu.busWrite8 = [&bus](uint16_t a, uint8_t v) { bus.write(a, v); };
	// machine_reset()'s m_port0 = 0x80 (battery ok); the samples_timer's 0x10 toggle isn't
	// modelled yet in Phase 1 (no timer device wired up), so this stays fixed at boot value.
	cpu.inP0Cb = [] { return uint8_t(0x80); };

	cpu.reset();

	long instructionCount = 0;
	cpu.onFetch = [&](uint16_t pc) {
		std::printf("%06ld %04x\n", instructionCount, pc);
		++instructionCount;
	};

	// Run in small chunks so the instruction budget is exact regardless of run()'s own
	// internal cycle-batching - each call executes until its cycle budget is exhausted,
	// which is not the same thing as an instruction count, so re-check after every chunk.
	while (instructionCount < instructionBudget)
		cpu.run(200);

	std::fprintf(stderr, "done: %ld instructions, PC=%04x, total_cycles=%llu\n",
	             instructionCount, cpu.pc(), (unsigned long long)cpu.totalCycles());
	return 0;
}
