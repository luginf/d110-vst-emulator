// pan_verify.cpp только что показал 6 из 8 партий полностью беззвучными после заводского
// сброса. Прежде чем подозревать сегодняшние правки Synth.cpp (ревербератор, multi-output),
// нужно исключить более простое объяснение: не заполняет ли собственный factoryReset() этого
// теста живую область Timbre Temporary для всех восьми партий, а только для той, что была
// текущим патчем. Дамп группы/номера тона и уровня громкости для всех 8 партий сразу после
// того же сброса, каким пользуется pan_verify.cpp.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
using Clock = std::chrono::steady_clock;

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto until = Clock::now() + std::chrono::duration<double>(seconds);
	while (Clock::now() < until) {
		juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, none);
	}
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}

	std::printf("заводской сброс (тот же вызов, что в pan_verify.cpp)...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting()) render(proc, 0.2);
	render(proc, 2.0);
	proc.getCore().resyncMirror();
	render(proc, 0.5);

	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());

	std::printf("\nтекущий патч (0x2DB9): %d\n\n", int(ram[(size_t)D110Core::kRamPatchNumber]));
	std::printf("партия | живая группа/номер | живой Output Level | имя тона в Tone Temp\n");
	for (int part = 0; part < 8; ++part) {
		const int at = D110Core::kRamTimbreTemp + 16 * part;
		const int group = ram[(size_t)at];
		const int number = ram[(size_t)at + 1];
		const int level = ram[(size_t)at + 8];
		const int toneAt = D110Core::kRamToneTemp + part * D110Core::kToneRecord;
		std::string name;
		for (int i = 0; i < 10; ++i) name.push_back(char(ram[(size_t)toneAt + i]));
		std::printf("  %d    |   %3d / %3d       |   %3d              | '%s'\n", part + 1, group,
		            number, level, name.c_str());
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
