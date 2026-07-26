// Finds the Tone Temporary Area's base in the firmware's RAM - positively, by content.
//
// The span-difference method used earlier was not good enough: it only ever bounded
// the base from below, because the tail bytes of two different tones need not differ.
// Trusting it wrote shifted data into the sound engine and audibly wrecked the tone.
//
// A Roland tone begins with TONE NAME 1..10, ten printable ASCII characters (owner's
// manual, note *5-1-1). So: find every run of ten printable characters in RAM, then
// check which of those positions behave like an array with a 246-byte stride - that is
// the Tone Temporary Area, and the first of them is its base.
#include "Source/D110Core.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

const char *kRomPath =
	"C:\\Users\\bd260\\Downloads\\MAME 0.288 ROMs (non-merged);"
	"C:\\Users\\bd260\\Downloads\\MAME_0.288_ROMs_[merged]";

struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit", 0, 7}, {"Patch", 0, 6}, {"Timbre", 0, 5}, {"Part+", 0, 4},
	{"Group+", 0, 3}, {"Bank+", 0, 2}, {"Number+", 0, 1}, {"Write", 0, 0},
	{"Edit", 1, 7}, {"Part", 1, 6}, {"System", 1, 5}, {"Part-", 1, 4},
	{"Group-", 1, 3}, {"Bank-", 1, 2}, {"Number-", 1, 1}, {"Enter", 1, 0},
};

D110Core *g_core = nullptr;

void press(const char *name, int times = 1, int holdMs = 130) {
	for (const auto &b : kButtons)
		if (std::strcmp(b.name, name) == 0) {
			const int idx = D110Core::buttonIndex(b.port, b.bit);
			for (int i = 0; i < times; ++i) {
				g_core->setButton(idx, true);
				std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
				g_core->setButton(idx, false);
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
			}
			return;
		}
}

std::vector<uint8_t> snapshot() {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	g_core->getRam(v.data());
	return v;
}

bool printable(uint8_t c) { return c >= 0x20 && c < 0x7f; }

// Every offset where at least `len` printable characters run.
std::vector<int> nameCandidates(const std::vector<uint8_t> &ram, int len) {
	std::vector<int> out;
	for (int i = 0; i + len <= D110Core::kRamSize; ++i) {
		bool ok = true;
		for (int k = 0; k < len; ++k)
			if (!printable(ram[i + k])) { ok = false; break; }
		if (ok) out.push_back(i);
	}
	return out;
}

std::string text(const std::vector<uint8_t> &ram, int at, int len) {
	std::string s;
	for (int i = 0; i < len; ++i) s.push_back(char(ram[at + i]));
	return s;
}

} // namespace

int main(int argc, char **argv) {
	const std::string nvram = (argc > 1) ? argv[1] : "d110_nvram";

	D110Core core;
	g_core = &core;
	std::printf("booting D-110 firmware (nvram: %s)\n", nvram.c_str());
	core.start(kRomPath, nvram);
	std::this_thread::sleep_for(std::chrono::seconds(8));
	std::printf("running: %s\n\n", core.isRunning() ? "yes" : "NO");

	press("Exit", 2);
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	const auto ram = snapshot();

	// Look for the array signature: a name candidate that has another one exactly 246
	// bytes later, and again 246 after that. Only the Tone Temporary Area does that.
	const auto cands = nameCandidates(ram, 10);
	std::printf("printable 10-char runs: %d\n\n", int(cands.size()));

	std::vector<char> isName(D110Core::kRamSize, 0);
	for (int c : cands) isName[(size_t)c] = 1;

	std::printf("=== offsets that repeat with a 246-byte stride ===\n");
	int found = 0;
	for (int c : cands) {
		if (c + 2 * 246 + 10 > D110Core::kRamSize) continue;
		if (!isName[(size_t)(c + 246)] || !isName[(size_t)(c + 2 * 246)]) continue;
		// Count how far the chain runs - the real area should give eight parts.
		int chain = 1;
		while (chain < 8 && c + chain * 246 + 10 <= D110Core::kRamSize
		       && isName[(size_t)(c + chain * 246)])
			++chain;
		std::printf("  0x%04X  chain of %d   '%s' | '%s' | '%s'\n", c, chain,
		            text(ram, c, 10).c_str(), text(ram, c + 246, 10).c_str(),
		            text(ram, c + 2 * 246, 10).c_str());
		if (++found >= 12) break;
	}
	if (!found) std::printf("  none - the tone name may not be stored as plain ASCII here\n");

	// For orientation, also show which names sit near the inferred region.
	std::printf("\n=== 10-char runs anywhere in 0x2000..0x2C00 ===\n");
	int shown = 0;
	for (int c : cands) {
		if (c < 0x2000 || c >= 0x2C00) continue;
		std::printf("  0x%04X  '%s'\n", c, text(ram, c, 10).c_str());
		if (++shown >= 30) break;
	}

	std::printf("\nstopping...\n");
	core.stop();
	std::printf("done\n");
	return 0;
}
