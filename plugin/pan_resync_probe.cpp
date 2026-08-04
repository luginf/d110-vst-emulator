// Живая Timbre Temporary читается сплошным центром (pan 7 у всех восьми партий) при том, что
// область памяти патча несёт верные заводские значения (4,10,6,8,2,12,0,14) - то есть
// правильные данные лежат в патче, но в звучащую область не попали. Проверяет одну гипотезу:
// достаточно ли заново ВЫБРАТЬ текущий патч (как нажатие Patch на панели), чтобы прошивка
// сама перенесла его поля в Timbre Temporary - или проблема глубже.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Нажатия кнопок в selectPatch() ведёт juce::Timer, а ему для срабатывания нужен прокрученный
// цикл сообщений - в консольной программе его никто не крутит сам, в отличие от плагина,
// где этим занят хозяин окна. Без прокрутки очередь кнопок стоит на месте вечно, и первая
// версия этого зонда так и не заметила, что 0x2DB9 не сдвинулся ни разу.
void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (std::chrono::steady_clock::now() < until) {
		juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, none);
	}
}

void printPan(D110AudioProcessor &proc, const char *label) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());
	// Пары группа/номер живой области - тон, который реально играет каждая партия сейчас,
	// не то, что записано в патче. Если навигация вправду доходит до прошивки, тут должны
	// смениться цифры вместе с номером патча по 0x2DB9.
	std::printf("%s  (0x2DB9=%d)\n", label, int(ram[(size_t)D110Core::kRamPatchNumber]));
	std::printf("  живые тона партий (группа/номер): ");
	for (int part = 0; part < 8; ++part) {
		const int at = D110Core::kRamTimbreTemp + 16 * part;
		std::printf("%d/%d ", ram[(size_t)at], ram[(size_t)at + 1]);
	}
	std::printf("\n");
	for (int part = 0; part < 8; ++part) {
		const int patchAt = 31 + part * 12 + 9;
		const int liveAt = D110Core::kRamTimbreTemp + 16 * part + 9;
		std::printf("  партия %d: патч=%d  живая=%d\n", part + 1, ram[(size_t)patchAt],
		            ram[(size_t)liveAt]);
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

	printPan(proc, "=== ДО ===");

	const int current = proc.currentPatchNumber();
	// Если просить тот же номер, что уже стоит, bankStep и numberStep выходят нулевыми, и
	// selectPatch нажимает только экран выбора патча - НИ ОДНОГО Bank/Number не нажимается,
	// а копирование поля в живую область, судя по всему, и происходит именно по ним. Поэтому
	// сначала уходим на другой патч - настоящее нажатие Bank/Number, - потом обратно.
	const int away = (current == 0) ? 5 : 0;
	std::printf("\nтекущий патч: %d, ухожу на %d...\n", current, away);
	proc.selectPatch(away);
	render(proc, 4.0);
	printPan(proc, "\n=== НА ЧУЖОМ ПАТЧЕ ===");

	std::printf("\nвозвращаюсь на %d...\n", current);
	proc.selectPatch(current);
	render(proc, 4.0);
	printPan(proc, "\n=== ПОСЛЕ ВОЗВРАТА ===");

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
