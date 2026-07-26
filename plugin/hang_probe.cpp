// Finds WHERE the firmware goes when notes reach it and the front panel stops responding.
//
// The panel dying is not a crash - the CPU keeps running and its RAM keeps changing, it
// simply never comes back to scanning the keys. So the question is which loop it is in.
// Sample the program counter densely in two states, healthy and dead, and compare: the
// addresses that appear only in the dead histogram are the loop.
//
// From there the ROM can be disassembled at those addresses to see what it is polling,
// which is the thing MAME does not provide because it emulates no LA32.
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

std::vector<juce::uint8> readLcd(D110AudioProcessor &proc) {
	std::vector<juce::uint8> v(D110Core::kLcdBytes, 0);
	proc.getCore().getLcd(v.data());
	return v;
}

bool panelResponds(D110AudioProcessor &proc) {
	const auto before = readLcd(proc);
	auto tap = [&proc](int port, int bit) {
		const int idx = D110Core::buttonIndex(port, bit);
		proc.getCore().setButton(idx, true);
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		proc.getCore().setButton(idx, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
	};
	tap(0, 5); // Timbre
	const auto after = readLcd(proc);
	tap(0, 7); // Exit
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	return after != before;
}

// Renders real-time audio for `seconds`, optionally playing chords.
void play(D110AudioProcessor &proc, double seconds, bool withChords) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	int block = 0, chordStep = 0;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer midi;
		if (withChords) {
			if (block % 20 == 0)
				for (int n : {0, 4, 7, 12})
					midi.addEvent(juce::MidiMessage::noteOn(2, 48 + (chordStep % 12) + n, 0.85f), 0);
			else if (block % 20 == 14) {
				for (int n : {0, 4, 7, 12})
					midi.addEvent(juce::MidiMessage::noteOff(2, 48 + (chordStep % 12) + n), 0);
				++chordStep;
			}
		}
		buffer.clear();
		proc.processBlock(buffer, midi);
		++block;
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

std::map<uint16_t, uint64_t> snapshotTop(D110AudioProcessor &proc, const char *label) {
	const auto top = proc.getCore().topPcs(20);
	const uint64_t total = proc.getCore().pcSampleTotal();
	std::printf("\n%s  (%llu samples)\n", label, (unsigned long long)total);
	std::map<uint16_t, uint64_t> out;
	for (const auto &h : top) {
		const double pct = total ? 100.0 * double(h.hits) / double(total) : 0.0;
		std::printf("   PC %04X   %8llu  %5.1f%%\n", h.pc, (unsigned long long)h.hits, pct);
		out[h.pc] = h.hits;
	}
	return out;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setForwardNotesToFirmware(true); // the whole point is to provoke it
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(8));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	// ---- healthy baseline: idle, panel alive -------------------------------
	proc.getCore().setPcSampling(true);
	proc.getCore().resetPcHistogram();
	play(proc, 12.0, false);
	std::printf("\npanel responds while idle: %s\n", panelResponds(proc) ? "yes" : "NO");
	const auto healthy = snapshotTop(proc, "=== HEALTHY: where the CPU sits when idle ===");

	// ---- provoke it --------------------------------------------------------
	std::printf("\nplaying chords until the panel stops responding...\n");
	bool dead = false;
	for (int i = 0; i < 10 && !dead; ++i) {
		play(proc, 3.0, true);
		dead = !panelResponds(proc);
		std::printf("  after %d s of chords: panel %s\n", (i + 1) * 3,
		            dead ? "*** DEAD ***" : "responds");
	}
	if (!dead) {
		std::printf("\nthe panel never died - nothing to diagnose in this run\n");
		proc.setPoweredOn(false);
		return 0;
	}

	// ---- dead histogram, gathered fresh ------------------------------------
	proc.getCore().resetPcHistogram();
	play(proc, 12.0, false); // stop playing; is it stuck even with no more notes?
	const bool stillDead = !panelResponds(proc);
	std::printf("\nnotes stopped; panel %s\n",
	            stillDead ? "STILL dead - it is stuck, not merely busy" : "recovered on its own");
	const auto stuck = snapshotTop(proc, "=== DEAD: where the CPU sits with the panel gone ===");

	// ---- the difference is the answer --------------------------------------
	std::printf("\n=== addresses that appear ONLY while dead ===\n");
	bool any = false;
	for (const auto &[pc, hits] : stuck) {
		if (healthy.count(pc)) continue;
		std::printf("   PC %04X   %llu hits   <-- candidate for the stuck loop\n", pc,
		            (unsigned long long)hits);
		any = true;
	}
	if (!any)
		std::printf("   none - it is in the same code, just not reaching the panel scan\n");

	// ---- can the missing interrupt be supplied? ----------------------------
	// The loop waits on a flag only an interrupt handler can set, and MAME drives no
	// external interrupt on this machine at all. Supply the edge ourselves and see which
	// rate, if any, lets the firmware finish the wait and get back to the panel.
	// ---- compare the ways of answering the firmware ------------------------
	std::printf("\n=== answering the stuck wait: three policies compared ===\n");
	struct Policy { const char *name; D110Core::StuckPolicy p; };
	for (const Policy pol : {Policy{"Off        ", D110Core::StuckPolicy::Off},
	                         Policy{"PokeRam    ", D110Core::StuckPolicy::PokeRam},
	                         Policy{"PulseExtInt", D110Core::StuckPolicy::PulseExtInt},
	                         Policy{"La32Stub   ", D110Core::StuckPolicy::La32Stub}}) {
		proc.setPoweredOn(false);
		std::this_thread::sleep_for(std::chrono::seconds(2));
		proc.getCore().setStuckPolicy(pol.p);
		proc.setForwardNotesToFirmware(true);
		proc.setPoweredOn(true);
		std::this_thread::sleep_for(std::chrono::seconds(9));

		const bool bootOk = panelResponds(proc);
		proc.getCore().resetPcHistogram();
		const uint64_t servicesBefore = proc.getCore().la32Services();
		int survivedTo = 0;
		bool alive = bootOk;
		for (int i = 0; i < 5 && alive; ++i) {
			play(proc, 6.0, true);
			alive = panelResponds(proc);
			if (alive) survivedTo = (i + 1) * 6;
		}
		const int ioc1 = proc.getCore().stuckIoc1Value();
		std::printf("  %s : boot %s, survived %2ds of chords %s  (interventions %llu, "
		            "IOC1=%02X -> EXTINT %s)\n",
		            pol.name, bootOk ? "ok" : "DEAD", survivedTo,
		            alive ? "*** STILL ALIVE ***" : "then died",
		            (unsigned long long)proc.getCore().stuckReleases(), ioc1,
		            ioc1 < 0 ? "not sampled" : ((ioc1 & 0x02) ? "DISABLED by IOC1.1" : "accepted"));
		// Is the EXTINT handler at 0x3138 even being entered? If it is not, the interrupt
		// is not being taken. If it is but no status byte was collected, it is bailing out
		// at 0x313D because the pin was no longer high when it ran.
		std::printf("                  handler 3138-3195 entered %llu, reads of 0C00 seen "
		            "%llu, status handed over %llu\n",
		            (unsigned long long)proc.getCore().pcHitsInRange(0x3138, 0x3195),
		            (unsigned long long)proc.getCore().la32Reads(),
		            (unsigned long long)(proc.getCore().la32Services() - servicesBefore));
	}

	// ---- release the wait directly ----------------------------------------
	// The loop polls a flag in battery RAM that the sound board's interrupt would set.
	// Set it, but only while the program counter shows the firmware is actually in that
	// loop, so a healthy machine is never touched.
	std::printf("\n=== releasing the stuck wait in RAM ===\n");
	{
		proc.setPoweredOn(false);
		std::this_thread::sleep_for(std::chrono::seconds(2));
		proc.getCore().setExtIntDivider(0);
		proc.getCore().setStuckPolicy(D110Core::StuckPolicy::PokeRam);
		proc.setForwardNotesToFirmware(true);
		proc.setPoweredOn(true);
		std::this_thread::sleep_for(std::chrono::seconds(9));

		std::printf("  fresh boot, panel %s\n", panelResponds(proc) ? "ok" : "DEAD");
		bool nowDead = false;
		for (int i = 0; i < 3 && !nowDead; ++i) {
			play(proc, 6.0, true);
			nowDead = !panelResponds(proc);
			std::printf("  after %2ds of chords: panel %s   releases %llu\n", (i + 1) * 6,
			            nowDead ? "dead" : "*** ALIVE ***",
			            (unsigned long long)proc.getCore().stuckReleases());
		}

		// The first wait is being released - the PC is no longer parked at 0x29E9. So if
		// the panel is still dead, it is stuck somewhere ELSE, and that somewhere is what
		// this second histogram is for.
		if (nowDead) {
			proc.getCore().resetPcHistogram();
			play(proc, 12.0, false);
			snapshotTop(proc, "=== DEAD DESPITE THE RELEASE: where is it now? ===");
		}
	}

	std::printf("\n=== supplying the external interrupt MAME never drives ===\n");
	// Lit dots are a blunt but honest health check: a booted D-110 has text on its
	// display, a firmware that has fallen over usually does not.
	auto litDots = [&proc] {
		const auto lcd = readLcd(proc);
		int n = 0;
		for (juce::uint8 b : lcd)
			for (int bit = 0; bit < 5; ++bit)
				if (b & (1 << bit)) ++n;
		return n;
	};

	// Divider 0 first: that is EXTINT off, and it re-establishes what a healthy fresh boot
	// looks like after the machine has been power cycled. Without that baseline a "dead"
	// reading below could just as easily mean the restart itself failed.
	for (int divider : {0, 256, 64, 16, 4, 1}) {
		proc.setPoweredOn(false);
		std::this_thread::sleep_for(std::chrono::seconds(2));
		proc.getCore().setExtIntDivider(divider);
		proc.setForwardNotesToFirmware(false); // no notes yet - EXTINT on its own
		proc.setPoweredOn(true);
		std::this_thread::sleep_for(std::chrono::seconds(9));

		const double hz = divider ? double(D110Core::kExtIntTimerHz) / (2.0 * divider) : 0.0;
		const int dotsIdle = litDots();
		const bool aliveIdle = panelResponds(proc);

		// Only if it survived EXTINT on its own is it worth playing notes at it.
		bool alivePlaying = false;
		if (aliveIdle) {
			proc.setForwardNotesToFirmware(true);
			play(proc, 9.0, true);
			alivePlaying = panelResponds(proc);
		}

		std::printf("  EXTINT %6.0f Hz : idle %s (%d dots)   playing %s   edges %llu\n", hz,
		            aliveIdle ? "ok" : "DEAD", dotsIdle,
		            !aliveIdle ? "-" : (alivePlaying ? "*** STILL ALIVE ***" : "dead"),
		            (unsigned long long)proc.getCore().extIntEdges());
		if (aliveIdle && alivePlaying) {
			std::printf("\n  *** THIS RATE KEEPS THE PANEL ALIVE WHILE PLAYING ***\n");
			break;
		}
	}

	proc.getCore().setPcSampling(false);
	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
