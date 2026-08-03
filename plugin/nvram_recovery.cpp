// Восстановление настоящей батарейной памяти прибора после того, как резкий обрыв питания
// застал файл прямо во время записи и обнулил его: резерв партиалов 0 вместо 32, каналы
// все 0 вместо 1..9, у каждой партии тон 0/0 - ровно то, что видно на приборе как "все
// партии пустые, кроме первой, а там AcouPiano1" (группа 0, номер 0).
//
// Идёт ТЕМ ЖЕ ПУТЁМ, что и сам плагин - через D110AudioProcessor и его собственный
// getNvramFolder()/getMachineNvramFolder(), без единого своего аргумента пути, - поэтому
// правит именно тот файл, который откроет установленный VST3 или standalone в следующий
// раз, а не отдельную копию для тестового стенда.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("папка НВР (та же, что у установленного плагина): %s\n",
	            D110AudioProcessor::getNvramRoot().getFullPathName().toRawUTF8());

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	std::printf("ПЗУ прибора: %s\n", proc.isSynthReady() ? "загружены" : "НЕ НАЙДЕНЫ");
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}

	// Снимок ДО - подтвердить диагноз тем же путём, каким его снял плагин.
	{
		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		std::printf("\nдо восстановления: резерв partial = %d (должно быть 32), "
		            "канал партии 1 = %d (должно быть 1)\n",
		            int(ram[0x2D98]) + ram[0x2D99] + ram[0x2D9A] + ram[0x2D9B] + ram[0x2D9C]
		                + ram[0x2D9D] + ram[0x2D9E] + ram[0x2D9F] + ram[0x2DA0],
		            int(ram[0x2DA1]));
	}

	std::printf("\nвыполняю заводской сброс...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 10.0); // дать прошивке дописать банки тембров/тонов после сброса

	{
		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		int reserveSum = 0;
		for (int i = 0; i < 9; ++i) reserveSum += ram[0x2D98 + i];
		std::printf("\nпосле сброса: резерв partial сумма = %d (должно быть 32), "
		            "канал партии 1 = %d (должно быть 1), партия 1 группа/номер = %d/%d\n",
		            reserveSum, int(ram[0x2DA1]), int(ram[0x2000]), int(ram[0x2001]));
	}

	// Выключение - это единственный момент, когда MAME пишет НВР на диск. Без него весь
	// восстановленный заводской набор остался бы только в памяти процесса.
	std::printf("\nвыключаю (это и есть момент записи на диск)...\n");
	proc.setPoweredOn(false);
	proc.releaseResources();

	std::printf("готово - НВР записана\n");
	return 0;
}
