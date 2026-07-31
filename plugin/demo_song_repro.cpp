// Reproduces the user's own exact steps, on the real firmware, instead of assuming the
// synthetic MIDI tests already cover it: power on, hold EDIT+ENTER to reach ROM Play,
// press ENTER to start Song 1, and watch what happens - first with the sound-board
// interface off (StuckPolicy::Off, matching what a user sees today) to confirm this path
// hits the exact same freeze the session's LA32 work has been chasing, then with the
// session's La32Stub fix engaged, since the demo song runs entirely on MAME's own thread
// (the firmware plays itself - no host MIDI injection at all), which may not suffer the
// cross-thread timing race found in the realistic-MIDI tests.
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

std::vector<uint8_t> g_cgrom;

bool isCgrom(const juce::MemoryBlock &data) {
	if (data.getSize() != 4096) return false;
	const auto *p = static_cast<const uint8_t *>(data.getData());
	static const uint8_t kA[7] = {0x0e, 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11};
	for (int r = 0; r < 7; ++r)
		if ((p[16 * 0x41 + r] & 0x1f) != kA[r]) return false;
	return true;
}

void loadCgrom() {
	const auto dir = D110AudioProcessor::getAutoRomFolder();
	for (const auto &entry : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFiles)) {
		const auto f = entry.getFile();
		if (f.hasFileExtension("zip")) {
			juce::ZipFile zip(f);
			for (int i = 0; i < zip.getNumEntries(); ++i) {
				const auto *e = zip.getEntry(i);
				if (e == nullptr || e->uncompressedSize != 4096) continue;
				std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(i));
				if (stream == nullptr) continue;
				juce::MemoryBlock data;
				stream->readIntoMemoryBlock(data);
				if (isCgrom(data)) {
					g_cgrom.assign(static_cast<const uint8_t *>(data.getData()),
					               static_cast<const uint8_t *>(data.getData()) + 4096);
					return;
				}
			}
			continue;
		}
		juce::MemoryBlock data;
		if (f.loadFileAsData(data) && isCgrom(data)) {
			g_cgrom.assign(static_cast<const uint8_t *>(data.getData()),
			               static_cast<const uint8_t *>(data.getData()) + 4096);
			return;
		}
	}
}

char decodeCell(const uint8_t *rows) {
	if (g_cgrom.empty()) return '?';
	for (int code = 0x20; code < 0x80; ++code) {
		bool same = true;
		for (int r = 0; r < 7; ++r)
			if ((g_cgrom[(size_t)16 * code + r] & 0x1f) != (rows[r] & 0x1f)) { same = false; break; }
		if (same) return char(code);
	}
	bool blank = true;
	for (int r = 0; r < 7; ++r) if (rows[r] & 0x1f) blank = false;
	return blank ? ' ' : '?';
}

std::string lcdText(D110AudioProcessor &proc) {
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return {};
	std::string s;
	for (int line = 0; line < D110Core::kLines; ++line) {
		if (line) s.push_back('/');
		for (int col = 0; col < D110Core::kCols; ++col)
			s.push_back(decodeCell(rows + ((size_t)line * D110Core::kCols + col) * D110Core::kRowsPerChar));
	}
	return s;
}

// Runs `seconds` of real-time audio with no MIDI at all - the demo song plays itself once
// started, exactly as on real hardware.
void idle(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	juce::MidiBuffer empty;
	const auto begin = Clock::now();
	auto next = begin;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		buffer.clear();
		proc.processBlock(buffer, empty);
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

void press(D110AudioProcessor &proc, std::initializer_list<int> indices, int holdMs, int settleMs) {
	for (int idx : indices) proc.getCore().setButton(idx, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
	for (int idx : indices) proc.getCore().setButton(idx, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
}

struct Decoded { const char *array; int index; };

Decoded decode(uint16_t addr) {
	if (addr >= 0x2DC0 && addr < 0x2E00) return {"edc0", (addr - 0x2DC0) / 2};
	if (addr >= 0x2E00 && addr < 0x2E40) return {"ee00", (addr - 0x2E00) / 2};
	if (addr >= 0x2E40 && addr < 0x2E80) return {"ee40", (addr - 0x2E40)};
	if (addr >= 0x2E80 && addr < 0x2EC0) return {"ee80", (addr - 0x2E80) / 2};
	if (addr >= 0x2EC0 && addr < 0x2F00) return {"eec0", (addr - 0x2EC0) / 2};
	if (addr >= 0x2F00 && addr < 0x2F40) return {"ef00", (addr - 0x2F00) / 2};
	if (addr >= 0x2F80 && addr < 0x2FC0) return {"ef80", (addr - 0x2F80) / 2};
	if (addr >= 0x33C0 && addr < 0x33E0) return {"f3c0", addr - 0x33C0};
	if (addr >= 0x3400 && addr < 0x3420) return {"f400", addr - 0x3400};
	if (addr >= 0x3420 && addr < 0x3440) return {"f420", addr - 0x3420};
	if (addr >= 0x3440 && addr < 0x3460) return {"f440", addr - 0x3440};
	if (addr >= 0x3460 && addr < 0x3480) return {"f460", addr - 0x3460};
	if (addr >= 0x3480 && addr < 0x34a0) return {"f480", addr - 0x3480};
	return {"?", -1};
}

bool buttonStillWorks(D110AudioProcessor &proc) {
	const auto before = lcdText(proc);
	press(proc, {D110Core::buttonIndex(0, 5)}, 150, 400); // Timbre
	const auto afterTimbre = lcdText(proc);
	press(proc, {D110Core::buttonIndex(0, 7)}, 150, 250); // Exit
	return afterTimbre != before;
}

void runOnce(const char *label, D110Core::StuckPolicy policy) {
	std::printf("\n========== %s ==========\n", label);
	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.getCore().setStuckPolicy(policy);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");
	std::printf("idle screen: [%s]\n", lcdText(proc).c_str());

	// EDIT (bottom row col0) + ENTER (bottom row col7) together -> ROM Play.
	std::printf("\npressing EDIT+ENTER together...\n");
	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 400);
	std::printf("after EDIT+ENTER: [%s]\n", lcdText(proc).c_str());

	// ENTER alone -> play the selected song (Song 1 by default).
	std::printf("\npressing ENTER to play the demo song...\n");
	proc.getCore().setVoiceCtxTap(true);
	proc.getCore().takeCtxEvents(); // drop anything from menu navigation
	proc.getCore().setButton(D110Core::buttonIndex(1, 0), true);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	proc.getCore().setButton(D110Core::buttonIndex(1, 0), false);

	// The firmware's OWN report of which parts are sounding: row 1 shows "12345678R", and a
	// part that is playing has its character replaced by the CGRAM block, which has no CGROM
	// glyph and so decodes as '?'. That is an independent check on the note bridge - if the
	// display lights a part the bridge never reported, the bridge is losing it; if neither
	// shows it, the song simply does not use it.
	bool everActive[9] = {};
	// Everything in ONE run this time: the firmware's own indicators, the notes the bridge
	// delivers, and the raw writes the bridge builds those notes from. Comparing figures
	// taken from different runs is what produced a wrong answer earlier - a part can be lit
	// in one run and never touched in the next, and no amount of care afterwards recovers
	// which was which.
	proc.getCore().setVoiceCtxTap(true);
	proc.getCore().takeCtxEvents();
	proc.getCore().startNoteLog();
	proc.getCore().setPcSampling(true);
	for (int tick = 0; tick < 75 * 5; ++tick) {
		idle(proc, 0.2);
		const auto screen = lcdText(proc);
		for (int p = 0; p < 9 && p < int(screen.size()); ++p)
			if (screen[(size_t)p] == '?') everActive[p] = true;
		if (tick % 25 == 24)
			std::printf("  %2ds  screen [%s]\n", (tick + 1) / 5, screen.c_str());
	}

	std::printf("\n  parts the FIRMWARE's own display ever showed as sounding:\n   ");
	for (int p = 0; p < 9; ++p)
		std::printf(" %s%s", (p == 8) ? "R" : std::to_string(p + 1).c_str(),
		            everActive[p] ? "=yes" : "=no ");
	std::printf("\n");

	// --- the three views, side by side, from this one run ---------------------
	// Drop counts FIRST, and loudly: a capture that hit its ceiling makes every tally below
	// a lower bound rather than a count, and reading it as a count is exactly how this
	// investigation went wrong three times.
	const uint64_t noteDrops = proc.getCore().noteLogDropped_();
	const uint64_t ctxDrops = proc.getCore().ctxDropped_();
	std::printf("\n  capture integrity: note log dropped %llu, ctx log dropped %llu%s\n",
	            (unsigned long long)noteDrops, (unsigned long long)ctxDrops,
	            (noteDrops || ctxDrops) ? "   *** TALLIES BELOW ARE LOWER BOUNDS ***" : "  (complete)");

	int delivered[9] = {};
	for (const auto &e : proc.getCore().takeNoteLog())
		if (e.on && e.part < 9) ++delivered[e.part];

	// Every write of the part byte, by the value written. f3a0 holds part*16, so 0x40 is
	// part 5 and 0x70 is part 8 - the two that go silent. If the firmware never writes
	// those values, it never assigns those parts and the bridge cannot be losing them; if
	// it does write them, the loss is ours.
	int partByteWrites[9] = {};
	std::map<int, std::map<uint16_t, int>> pcsPerPart; // part -> PC -> count
	int noteByteWrites[D110Core::kNumVoiceContexts] = {};
	for (const auto &e : proc.getCore().takeCtxEvents()) {
		if (e.addr >= D110Core::kNoteTable &&
		    e.addr < D110Core::kNoteTable + D110Core::kNumVoiceContexts)
			++noteByteWrites[e.addr - D110Core::kNoteTable];
		if (e.addr >= D110Core::kPartTable &&
		    e.addr < D110Core::kPartTable + D110Core::kNumVoiceContexts) {
			const int part = e.value >> 4;
			if (part < 9) { ++partByteWrites[part]; ++pcsPerPart[part][e.pc]; }
		}
	}

	std::printf("\n  part | display | f3a0 writes | notes delivered | verdict\n");
	for (int p = 0; p < 9; ++p) {
		const char *verdict =
			(partByteWrites[p] == 0 && !everActive[p]) ? "not used by this song"
			: (partByteWrites[p] == 0 && everActive[p]) ? "*** LIT BUT NEVER ASSIGNED ***"
			: (delivered[p] == 0) ? "*** ASSIGNED BUT NO NOTE SENT ***"
			: "ok";
		std::printf("   %4d | %7s | %11d | %15d | %s\n", p + 1, everActive[p] ? "lit" : "-",
		            partByteWrites[p], delivered[p], verdict);
	}

	std::printf("\n  which code writes the part byte, per part:\n");
	for (const auto &[part, pcs] : pcsPerPart) {
		std::printf("    part %d:", part + 1);
		for (const auto &[pc, n] : pcs) std::printf("  PC %04X x%d", pc, n);
		std::printf("\n");
	}

	// NOT buttonStillWorks() here. During ROM Play the firmware legitimately stays on the
	// play screen and ignores Timbre, so that check reported a DEAD panel for a machine
	// that was demonstrably fine - the part indicators were still moving and the song had
	// advanced to the next title. What actually proves liveness during playback is that the
	// display keeps changing, so that is what is asked.
	const bool alive = everActive[0] || everActive[8];
	std::printf("\nfirmware alive through the demo (display kept moving): %s   screen [%s]\n",
	            alive ? "YES" : "*** NO ***", lcdText(proc).c_str());
	if (!alive) {
		const auto top = proc.getCore().topPcs(6);
		std::printf("top PCs while stuck:\n");
		for (const auto &h : top) std::printf("   PC %04X  %llu\n", h.pc, (unsigned long long)h.hits);

		const auto samples = proc.getCore().takePort2Samples();
		std::printf("\nEXTINT line ground-truth samples near the handler check (%d captured), first 30:\n",
		            int(samples.size()));
		int shown = 0;
		for (const auto &s : samples) {
			std::printf("  PC %04X  EXTINT_line=%d  stuckIntHigh=%d  la32Pending=%d\n", s.pc,
			            s.port2, s.stuckIntHigh ? 1 : 0, s.la32Pending ? 1 : 0);
			if (++shown >= 30) break;
		}

		std::printf("\nunresolved wait context: r52 = %d (0x%02X), stuck for %llu consecutive ticks\n",
		            proc.getCore().lastUnresolvedContext_(), proc.getCore().lastUnresolvedContext_(),
		            (unsigned long long)proc.getCore().unresolvedStreak_());
		{
			std::vector<uint8_t> ram(D110Core::kRamSize, 0);
			proc.getCore().getRam(ram.data());
			std::printf("edc0[]/ee01[] table (busy-flag / context-owner per hardware slot):\n");
			for (int n = 0; n < 32; ++n) {
				const uint8_t busy = ram[0x2DC0 + 2 * n];
				const uint8_t owner = ram[0x2E01 + 2 * n];
				if (busy != 0x80) // only print non-idle slots
					std::printf("  slot %2d: edc0=%02X  ee01(owner)=%02X\n", n, busy, owner);
			}
		}

		const auto ctx = proc.getCore().takeCtxEvents();
		std::vector<D110Core::CtxEvent> named;
		for (const auto &e : ctx)
			if (decode(e.addr).index >= 0) named.push_back(e);
		std::printf("\n%d named dispatch/completion events captured; LAST 60 (closest to the stall):\n",
		            int(named.size()));
		const int start = int(named.size()) > 60 ? int(named.size()) - 60 : 0;
		for (int i = start; i < int(named.size()); ++i) {
			const auto d = decode(named[(size_t)i].addr);
			std::printf("  PC %04X  %s[%d] = %02X\n", named[(size_t)i].pc, d.array, d.index,
			            named[(size_t)i].value);
		}
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();
	std::printf("character ROM for LCD decode: %s\n", g_cgrom.empty() ? "NOT FOUND (screens show as '?')" : "loaded");

	runOnce("StuckPolicy::La32Stub - longer run after the pending_irq level-7 fix",
	        D110Core::StuckPolicy::La32Stub);

	std::printf("\ndone\n");
	return 0;
}
