// Reported: importing a SysEx bank changes nothing at all.
//
// It went only to the sound engine, so the control board never learned of it and the next
// RAM mirror overwrote the lot with the firmware's own unchanged state - an import that
// looked accepted and was silently undone a frame later.
//
// The check: build a Roland "Data set 1" that moves one parameter the firmware keeps in
// battery RAM, send it the way an imported bank is sent, and read that RAM back.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// F0 41 <dev> 16 12 <a1 a2 a3> <data> <sum> F7 - the same framing the bridge emits.
std::vector<juce::uint8> makeDT1(juce::uint32 address, std::vector<juce::uint8> data) {
	std::vector<juce::uint8> m{0xF0, 0x41, 0x10, 0x16, 0x12};
	const juce::uint8 a1 = juce::uint8((address >> 16) & 0x7f);
	const juce::uint8 a2 = juce::uint8((address >> 8) & 0x7f);
	const juce::uint8 a3 = juce::uint8(address & 0x7f);
	m.insert(m.end(), {a1, a2, a3});
	juce::uint32 sum = a1 + a2 + a3;
	for (juce::uint8 b : data) {
		m.push_back(b & 0x7f);
		sum += b & 0x7f;
	}
	m.push_back(juce::uint8((128 - (sum & 0x7f)) & 0x7f));
	m.push_back(0xF7);
	return m;
}

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	juce::MidiBuffer midi;
	for (int b = 0; b < int(seconds * kSampleRate / kBlock); ++b) {
		buffer.clear();
		proc.processBlock(buffer, midi);
	}
}

int fineTune(D110AudioProcessor &proc) {
	std::vector<juce::uint8> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return -1;
	return ram[0x2003];
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(8));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	const int before = fineTune(proc);
	std::printf("Fine Tune before the import : %d\n", before);

	// Timbre Temporary part 1, byte 3 is fineTune - RAM 0x2003, SysEx 0x030003.
	const int wanted = (before == 42) ? 58 : 42;
	const auto message = makeDT1(0x030003, {juce::uint8(wanted)});

	// Deliver it exactly as an imported bank is delivered: queued on the message thread,
	// drained by processBlock.
	{
		juce::MemoryBlock raw(message.data(), message.size());
		const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
		                     .getChildFile("d110_import_test.syx");
		tmp.replaceWithData(raw.getData(), raw.getSize());
		proc.importSysexBank(tmp);
		std::printf("import said: %s\n", proc.getLastImportMessage().toRawUTF8());
		tmp.deleteFile();
	}

	// Let it be drained, travel down the emulated cable and be acted on.
	render(proc, 0.5);
	std::this_thread::sleep_for(std::chrono::seconds(3));
	render(proc, 0.5);

	const int after = fineTune(proc);
	std::printf("Fine Tune after the import  : %d (asked for %d)\n", after, wanted);
	std::printf("%s\n", after == wanted
	                        ? "*** THE IMPORT REACHED THE FIRMWARE ***"
	                        : "NOT APPLIED - the firmware never took it");

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
