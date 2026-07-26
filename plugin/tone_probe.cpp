// Finds the Tone Temporary Area's base in the firmware's RAM, and at the same time
// answers whether mirroring it is safe at all - by comparing bytes, not by listening.
//
// Every earlier attempt inferred the base from how far a change spread when the timbre
// was switched, which only ever bounds it from BELOW: the tail bytes of two different
// tones need not differ. Mirroring from a base found that way wrote shifted data into the
// engine and audibly wrecked the tone.
//
// This measures instead. Both halves of the plugin independently hold what should be the
// same tone: the firmware loads it into its Tone Temporary Area, and munt loads it into
// mt32emu's timbreTemp[] the moment the already-working Timbre Temporary mirror tells it
// which timbre a part plays (Part.cpp's setTimbre). Both read it out of the same ROM. So
// take munt's 246-byte copy, slide it over all 32 KB of the firmware's RAM, and see where
// it fits.
//
// The result is decisive in either direction:
//   - a clean match at a constant base + part*246  ->  the layouts are identical, and the
//     base is now measured rather than guessed. Mirroring an unedited tone is a no-op,
//     which is the null test the bridge needed.
//   - no strong match anywhere  ->  the internal layout is NOT Roland's exclusive layout,
//     and the mismatch is a fact to work from rather than a guess.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// A Roland tone: 14 bytes of common parameters then four 58-byte partials.
constexpr int kToneBytes = 246;

// mt32emu addresses its memory regions with the three 7-bit address bytes squeezed
// together, so the 0x040000 that goes out on the wire is 0x010000 internally. Getting
// this wrong is the same mistake that made the first version of the bridge fire
// correctly and change nothing.
constexpr juce::uint32 packAddr(juce::uint32 a) {
	return ((a & 0x7f0000u) >> 2) | ((a & 0x7f00u) >> 1) | (a & 0x7fu);
}
constexpr juce::uint32 kToneTempPacked = packAddr(0x040000u);

void runAudio(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	juce::MidiBuffer midi;
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		proc.processBlock(buffer, midi);
	}
}

std::string printableName(const juce::uint8 *p, int len = 10) {
	std::string s;
	for (int i = 0; i < len; ++i)
		s.push_back((p[i] >= 0x20 && p[i] < 0x7f) ? char(p[i]) : '.');
	return s;
}

struct Candidate {
	int offset = -1;
	int matches = 0;
};

// Slides `needle` over the whole RAM image and keeps the best few offsets by number of
// equal bytes. Exhaustive on purpose: 32 KB x 246 is nothing, and anything cleverer could
// miss the answer for a reason that would be hard to see.
std::vector<Candidate> bestMatches(const std::vector<juce::uint8> &ram,
                                   const juce::uint8 *needle, int keep) {
	std::vector<Candidate> top;
	for (int off = 0; off + kToneBytes <= int(ram.size()); ++off) {
		int matches = 0;
		for (int i = 0; i < kToneBytes; ++i)
			if (ram[(size_t)(off + i)] == needle[i]) ++matches;

		if (int(top.size()) < keep || matches > top.back().matches) {
			// Keep the list small and sorted; overlapping windows around a hit would
			// otherwise crowd out genuinely distinct candidates, so skip anything that
			// sits within a tone's length of a better one already held.
			bool nearBetter = false;
			for (auto &c : top)
				if (std::abs(c.offset - off) < kToneBytes) {
					nearBetter = true;
					if (matches > c.matches) c = {off, matches};
					break;
				}
			if (!nearBetter) top.push_back({off, matches});
			std::sort(top.begin(), top.end(),
			          [](const Candidate &a, const Candidate &b) { return a.matches > b.matches; });
			if (int(top.size()) > keep) top.resize(keep);
		}
	}
	return top;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;

	D110AudioProcessor proc;
	std::printf("ROMs loaded : %s\n", proc.isSynthReady() ? "yes" : "NO");
	if (!proc.isSynthReady()) {
		std::printf("last error  : %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}
	proc.prepareToPlay(kSampleRate, kBlock);

	std::printf("\npowering on (boots the real firmware)...\n");
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(6));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	// Same reason audio_test does this: without a known starting state the run is not
	// reproducible and a parameter may already be parked at a limit.
	std::printf("factory reset...\n");
	proc.getCore().factoryReset();
	while (proc.getCore().isResetting())
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	std::this_thread::sleep_for(std::chrono::seconds(2));

	// Push the firmware's whole mirrored state across and let the audio thread drain the
	// queue, so mt32emu's timbreTemp[] holds the tones the firmware currently has loaded
	// rather than whatever the ROM defaults were.
	proc.getCore().resyncMirror();
	runAudio(proc, 1.5);
	std::printf("mirror messages sent: %llu\n",
	            (unsigned long long)proc.getCore().sysexEmitted());

	std::vector<juce::uint8> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) {
		std::printf("no RAM snapshot yet - aborting\n");
		proc.setPoweredOn(false);
		return 1;
	}

	std::printf("\n=== per-part comparison: mt32emu's tone vs the firmware's RAM ===\n");
	std::vector<int> baseByPart(8, -1);
	std::vector<int> matchByPart(8, 0);

	for (int part = 0; part < 8; ++part) {
		juce::uint8 engine[kToneBytes] = {};
		if (!proc.readEngineMemory(kToneTempPacked + juce::uint32(part * kToneBytes),
		                           kToneBytes, engine)) {
			std::printf("part %d: engine read failed\n", part + 1);
			continue;
		}

		bool allSame = true;
		for (int i = 1; i < kToneBytes; ++i)
			if (engine[i] != engine[0]) { allSame = false; break; }
		if (allSame) {
			std::printf("part %d: engine copy is uniform 0x%02X - no tone loaded, skipping\n",
			            part + 1, engine[0]);
			continue;
		}

		const auto top = bestMatches(ram, engine, 3);
		std::printf("\npart %d  engine tone name '%s'\n", part + 1,
		            printableName(engine).c_str());
		for (size_t i = 0; i < top.size(); ++i)
			std::printf("   %s RAM 0x%04X  %3d/%d bytes  '%s'\n",
			            i == 0 ? "best:" : "     ", top[i].offset, top[i].matches, kToneBytes,
			            printableName(&ram[(size_t)top[i].offset]).c_str());

		if (!top.empty()) {
			baseByPart[(size_t)part] = top[0].offset;
			matchByPart[(size_t)part] = top[0].matches;
			// Name the bytes that disagree - a handful in the same places on every part is
			// a live/scratch field, while scattered noise means the layouts differ.
			if (top[0].matches < kToneBytes) {
				std::printf("        differing byte indices:");
				int shown = 0;
				for (int i = 0; i < kToneBytes && shown < 24; ++i)
					if (ram[(size_t)(top[0].offset + i)] != engine[i]) {
						std::printf(" %d", i);
						++shown;
					}
				if (shown == 24) std::printf(" ...");
				std::printf("\n");
			}
		}
	}

	// ---- the verdict -------------------------------------------------------
	std::printf("\n=== verdict ===\n");
	int firstPart = -1;
	for (int p = 0; p < 8; ++p)
		if (baseByPart[(size_t)p] >= 0) { firstPart = p; break; }

	if (firstPart < 0) {
		std::printf("no part produced a comparison - the engine never received a tone.\n");
	} else {
		const int implied = baseByPart[(size_t)firstPart] - firstPart * kToneBytes;
		bool strideHolds = true;
		int worst = kToneBytes;
		for (int p = 0; p < 8; ++p) {
			if (baseByPart[(size_t)p] < 0) continue;
			if (baseByPart[(size_t)p] != implied + p * kToneBytes) strideHolds = false;
			worst = std::min(worst, matchByPart[(size_t)p]);
		}
		std::printf("implied base            : RAM 0x%04X\n", implied);
		std::printf("246-byte stride holds   : %s\n", strideHolds ? "YES" : "NO");
		std::printf("worst per-part match    : %d/%d\n", worst, kToneBytes);
		if (strideHolds && worst == kToneBytes)
			std::printf("*** EXACT - the layouts are identical, mirroring is safe ***\n");
		else if (strideHolds && worst >= kToneBytes - 8)
			std::printf("*** near-exact - check the differing indices above before enabling ***\n");
		else
			std::printf("NOT SAFE - the internal layout is not Roland's exclusive layout.\n");
	}

	// ---- the System Area, audited the same way -----------------------------
	// Its base was pinned by a single byte (Master Tune moved 0x2D94 by the press count),
	// which says nothing about the 22 bytes after it. That matters more here than
	// anywhere else: the block holds masterVol and reserveSettings, so writing the wrong
	// bytes into it does not corrupt one parameter, it drops the level of the whole
	// instrument. Print both sides and let the structure speak for itself.
	{
		std::printf("\n=== System Area audit ===\n");
		std::printf("firmware RAM 0x2D94:");
		for (int i = 0; i < 23; ++i) std::printf(" %02X", ram[(size_t)(0x2D94 + i)]);
		std::printf("\n");

		juce::uint8 sys[23] = {};
		if (proc.readEngineMemory(packAddr(0x100000u), 23, sys)) {
			std::printf("engine System      :");
			for (int i = 0; i < 23; ++i) std::printf(" %02X", sys[i]);
			std::printf("\n");
		}

		// Roland's documented order, so an implausible value is obvious at a glance.
		const juce::uint8 *r = &ram[0x2D94];
		int reserveSum = 0;
		for (int i = 0; i < 9; ++i) reserveSum += r[4 + i];
		std::printf("\nread as Roland's System structure:\n");
		std::printf("  masterTune   %3d   (0-127)\n", r[0]);
		std::printf("  reverbMode   %3d   (0-3)   %s\n", r[1], r[1] <= 3 ? "ok" : "OUT OF RANGE");
		std::printf("  reverbTime   %3d   (0-7)   %s\n", r[2], r[2] <= 7 ? "ok" : "OUT OF RANGE");
		std::printf("  reverbLevel  %3d   (0-7)   %s\n", r[3], r[3] <= 7 ? "ok" : "OUT OF RANGE");
		std::printf("  reserve[9]   %d %d %d %d %d %d %d %d %d  sum %d  %s\n",
		            r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], reserveSum,
		            reserveSum == 32 ? "ok (sums to 32)" : "SHOULD SUM TO 32");
		std::printf("  chanAssign[9] %d %d %d %d %d %d %d %d %d  (0-16)\n",
		            r[13], r[14], r[15], r[16], r[17], r[18], r[19], r[20], r[21]);
		std::printf("  masterVol    %3d   (0-100) %s\n", r[22], r[22] <= 100 ? "ok" : "OUT OF RANGE");
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
