// Whether the real D-110 firmware's "Display Message" SysEx (address 0x200000) can put
// independent text on BOTH physical LCD rows, or only ever fills a single 20-byte buffer -
// Alan asked (2026-08-29) after using the new Utility-tab Send/clipboard feature. munt's own
// Display.cpp documents this SysEx as a single 20-byte custom-message buffer inherited from
// the MT-32 control ROM lineage (addresses 0x200000-0x200013), and docs/sysex_address_map.md
// shows the D-110's own documented address for this area matching munt's exactly - but that's
// secondary evidence about a GENERIC MT-32-family model, not this project's own D-110 ROM
// code. This probe asks the real firmware directly: send a message with different text past
// byte 16 (and past byte 20, and past 32) and read back what the ACTUAL LCD shows via
// core.getLcd(), decoded through the real CGROM exactly as editor_write_probe.cpp does for
// the MAME backend.
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cstdio>
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
	for (const auto &e : juce::RangedDirectoryIterator(D110AudioProcessor::getAutoRomFolder(),
	                                                    true, "*", juce::File::findFiles)) {
		juce::MemoryBlock d;
		if (e.getFile().loadFileAsData(d) && isCgrom(d)) {
			g_cgrom.assign(static_cast<const uint8_t *>(d.getData()),
			               static_cast<const uint8_t *>(d.getData()) + 4096);
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
	return '?';
}

std::string screen(D110AudioProcessor &proc) {
	uint8_t rows[D110CoreType::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return "(no screen)";
	std::string s;
	for (int line = 0; line < D110CoreType::kLines; ++line) {
		if (line) s += " / ";
		for (int col = 0; col < D110CoreType::kCols; ++col)
			s.push_back(decodeCell(rows + ((size_t)line * D110CoreType::kCols + col)
			                        * D110CoreType::kRowsPerChar));
	}
	return s;
}

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> block(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer none;
		block.clear();
		proc.processBlock(block, none);
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

// Sends `length` bytes (not just the usual 20) at the given offset within the Display area,
// bypassing D110AudioProcessor::sendDisplayMessage()'s hardcoded 20-byte length, precisely to
// find out if the firmware honours anything past that.
void sendDisplayRaw(D110AudioProcessor &proc, int offset, const std::vector<uint8_t> &data) {
	uint8_t msg[D110CoreType::kMaxSysexBytes];
	const int n = D110CoreType::buildDt1Message(D110CoreType::kSysexDisplay, offset, data.data(),
	                                            int(data.size()), msg);
	if (n <= 0) { std::printf("    !!! message not built (offset %d, length %d)\n", offset,
	                          int(data.size())); return; }
	proc.getCore().pushMidi(msg, n);
	render(proc, 1.2);
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);

	// Watch the natural boot sequence in slices, instead of jumping straight to "settled" -
	// munt's own Display.cpp documents that custom messages on MT-32-lineage control ROMs are
	// only shown while the display is in "main" (Master Volume) mode, which is likely only
	// the first moment or two after boot, before the firmware settles into Patch Play.
	for (int i = 0; i < 18; ++i) {
		render(proc, 0.5);
		std::printf("t=%.1fs running=%s screen: %s\n", (i + 1) * 0.5,
		            proc.getCore().isRunning() ? "yes" : "no", screen(proc).c_str());
		// Try the display write at every step, not just once settled - whichever boot phase
		// (if any) is receptive to it should show up as a screen change on the NEXT line.
		if (proc.getCore().isRunning() && !g_cgrom.empty()) {
			std::vector<uint8_t> data(20, 'X');
			sendDisplayRaw(proc, 0, data);
		}
	}
	std::printf("firmware: %s   cgrom: %s\n", proc.getCore().isRunning() ? "running" : "NO",
	            g_cgrom.empty() ? "NOT FOUND" : "loaded");
	if (!proc.getCore().isRunning()) return 1;
	if (g_cgrom.empty()) return 1;

	std::printf("boot screen: %s\n", screen(proc).c_str());

	// Control: a write technique already known to land (TimbreTemp part 1 Output Level, same
	// sendAreaData()/core.pushMidi() path as the Display writes below) - if THIS doesn't show
	// up in RAM either, the probe's injection technique itself is broken, not the Display
	// command specifically.
	{
		std::vector<uint8_t> ramBefore(D110CoreType::kRamSize, 0);
		proc.getCore().getRam(ramBefore.data());
		const uint8_t v = 0x55;
		uint8_t msg[D110CoreType::kMaxSysexBytes];
		const int n = D110CoreType::buildDt1Message(D110CoreType::kSysexTimbreTemp, 8, &v, 1, msg);
		proc.getCore().pushMidi(msg, n);
		render(proc, 1.2);
		std::vector<uint8_t> ramAfter(D110CoreType::kRamSize, 0);
		proc.getCore().getRam(ramAfter.data());
		const int addr = D110CoreType::kRamTimbreTemp + 8;
		std::printf("control write (TimbreTemp part1 Output Level=0x55): RAM[0x%04X] %02X -> %02X\n",
		            addr, ramBefore[(size_t)addr], ramAfter[(size_t)addr]);
	}

	// 1. The documented 20-byte buffer, distinct halves: 16 'A's then 4 '1'..'4' - matches
	//    what sendDisplayMessage() itself sends today (offset 0, length 20).
	{
		std::vector<uint8_t> data(20, 'A');
		data[16] = '1'; data[17] = '2'; data[18] = '3'; data[19] = '4';
		sendDisplayRaw(proc, 0, data);
		std::printf("20 bytes 'A'x16+\"1234\": %s\n", screen(proc).c_str());
	}

	// 2. Same, but 32 bytes ('B's then row-2 marker 'Z'x16) - is offset 20..31 shown anywhere?
	{
		std::vector<uint8_t> data(32, 'B');
		for (int i = 16; i < 32; ++i) data[i] = 'Z';
		sendDisplayRaw(proc, 0, data);
		std::printf("32 bytes 'B'x16+\"Z\"x16: %s\n", screen(proc).c_str());
	}

	// 3. Explicit second write AT offset 16 with different content, after first establishing
	//    a known 0..15 - does writing past byte 16 (still within the documented 0x13 range for
	//    individual addressing) change anything visible, on either row?
	{
		std::vector<uint8_t> first(16, 'C');
		sendDisplayRaw(proc, 0, first);
		std::printf("after 16 'C's at offset 0: %s\n", screen(proc).c_str());
		std::vector<uint8_t> second(16, 'D');
		sendDisplayRaw(proc, 16, second);
		std::printf("then 16 'D's at offset 16: %s\n", screen(proc).c_str());
	}

	// 4. Explicit write far past the documented buffer (offset 64) - should be a clean no-op
	//    if the firmware really caps this area at 20/32 bytes.
	{
		std::vector<uint8_t> data(16, 'E');
		sendDisplayRaw(proc, 64, data);
		std::printf("16 'E's at offset 64: %s\n", screen(proc).c_str());
	}

	std::printf("\ndone\n");
	return 0;
}
