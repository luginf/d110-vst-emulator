// До сих пор панорама ритм-секции сверялась только БАЙТОМ в памяти против заводского
// дампа - никогда настоящим звуком через оба канала, как это сделано для обычных партий
// (pan_verify.cpp). А с тех пор в Synth.cpp дважды правился сам путь рендера (ревербератор
// BOSS, потом шесть индивидуальных выходов) - байт мог остаться верным, а путь до реального
// стерео мог сломаться. Меряет L/R RMS для нескольких ритм-клавиш с разными значениями pan
// (крайний вправо, крайне влево, центр, кик) и сверяет с тем, что предсказывает сам байт.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kRhythmChannel = 10;
using Clock = std::chrono::steady_clock;

void render(D110AudioProcessor &proc, double seconds, juce::MidiBuffer *midi = nullptr) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto until = Clock::now() + std::chrono::duration<double>(seconds);
	bool first = true;
	while (Clock::now() < until) {
		juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, (first && midi) ? *midi : none);
		first = false;
	}
}

struct Balance { double l = 0, r = 0; };

Balance measureBalance(D110AudioProcessor &proc, int note) {
	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(kRhythmChannel, note, 0.95f), 0);
	juce::AudioBuffer<float> buffer(2, kBlock);
	double sumL = 0, sumR = 0;
	int n = 0;
	const auto until = Clock::now() + std::chrono::duration<double>(0.4);
	bool first = true;
	while (Clock::now() < until) {
		juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, first ? on : none);
		first = false;
		for (int i = 0; i < buffer.getNumSamples(); ++i) {
			sumL += double(buffer.getSample(0, i)) * buffer.getSample(0, i);
			sumR += double(buffer.getSample(1, i)) * buffer.getSample(1, i);
			++n;
		}
	}
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(kRhythmChannel, note), 0);
	render(proc, 0.3, &off);
	return { std::sqrt(sumL / std::max(1, n)), std::sqrt(sumR / std::max(1, n)) };
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
	proc.setForwardNotesToFirmware(true);

	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());

	std::printf("клавиша | тембр | pan-байт | предсказано %%R | измерено L/R RMS | измерено %%R\n");
	static const struct { int note; const char *label; } kCases[] = {
		{ 36, "Bass Drum 1 (кик)" },
		{ 82, "" }, // самый правый pan в текущей карте
		{ 81, "" }, // самый левый pan в текущей карте
		{ 38, "Acoustic Snare" },
		{ 42, "Closed Hi-Hat" },
	};
	for (const auto &c : kCases) {
		const int at = D110Core::kRamRhythmTemp + (c.note - D110Core::kRhythmFirstKey) * D110Core::kRhythmRecord;
		const int tembr = ram[(size_t)at];
		const int panByte = ram[(size_t)at + 2];
		const double predictedPctR = 100.0 * (14 - panByte) / 14.0; // 0=R,14=L -> шкала в %R

		const Balance bal = measureBalance(proc, c.note);
		const double total = bal.l + bal.r;
		const double measuredPctR = total > 1e-9 ? 100.0 * bal.r / total : -1.0;

		std::printf("  %3d  | %5d | %8d | %13.1f | L=%.4f R=%.4f | %10.1f  %s\n", c.note, tembr,
		            panByte, predictedPctR, bal.l, bal.r, measuredPctR, c.label);
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
