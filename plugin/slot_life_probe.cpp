// Кто и когда пишет в таблицу слотов LA32 - и освобождает ли её хоть кто-нибудь.
//
// Прибор играет примерно две ноты за раз вместо восьми, и измерено, что при отпущенных
// клавишах у прошивки занят 31 слот из 32 (см. `d110_polyphony`). Обработчик прерывания по
// 0x3138 выбирает путь по этой самой таблице - `rams[0x2DC0 + 2v] == 0x80` значит «слот
// свободен», - так что всё упирается в вопрос, кто возвращает туда 0x80.
//
// Спрашивать об этом надо не дизассемблер, а работающую прошивку: перехват записей в окно
// диспетчеризации (CPU 0xEDC0-0xEFFF) уже есть, и он говорит АДРЕС, ЗНАЧЕНИЕ и ТОТ САМЫЙ
// PC, откуда запись сделана. Если 0x80 не пишет никто - освобождать слоты некому, и это
// ответ. Если пишет, но редко - ответ другой, и адрес скажет, какая подпрограмма это делает.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kChannel = 2;   // партия 1 у заводского прибора

void renderBlocks(D110AudioProcessor &proc, int blocks, juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

void render(D110AudioProcessor &proc, double seconds) {
	renderBlocks(proc, int(seconds * kSampleRate / kBlock));
}

int busySlots(D110AudioProcessor &proc) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return -1;
	int busy = 0;
	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		const uint8_t v = ram[size_t(D110Core::kSlotStateTable) + size_t(s) * 2];
		if (v == D110Core::kSlotBusyValue || v == D110Core::kSlotBusyValueAlt) ++busy;
	}
	return busy;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) { std::printf("прошивка не работает\n"); return 1; }
	std::printf("занятых слотов до игры: %d из %d\n", busySlots(proc),
	            D110Core::kNumHardwareVoices);

	proc.getCore().setVoiceCtxTap(true);

	// Шесть нот подряд, отпускаются сразу: этого хватает, чтобы увидеть и выдачу слота, и
	// то, что происходит (или не происходит) в конце жизни голоса.
	for (int i = 0; i < 6; ++i) {
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, 48 + i * 3, 0.9f), 0);
		renderBlocks(proc, 14, &on);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kChannel, 48 + i * 3), 0);
		renderBlocks(proc, 10, &off);
	}
	// Долгая тишина: если освобождение вообще бывает, оно случится тут.
	render(proc, 6.0);
	proc.getCore().setVoiceCtxTap(false);

	const auto events = proc.getCore().takeCtxEvents();
	std::printf("событий записи в окно диспетчеризации: %d\n\n", int(events.size()));

	// Только сама таблица слотов. Перехват пишет СМЕЩЕНИЕ В ОЗУ, а не адрес процессора:
	// таблица - это rams 0x2DC0 + 2*slot, то есть 0x2DC0..0x2DFF. Первая версия этого зонда
	// фильтровала по 0xEDC0 и получила «ноль записей» там, где их девяносто две.
	struct Key { uint16_t pc; uint8_t value; };
	std::map<uint32_t, int> byPcValue;
	int toSlotTable = 0;
	for (const auto &e : events) {
		if (e.addr < D110Core::kSlotStateTable || e.addr > D110Core::kSlotStateTable + 63) continue;
		++toSlotTable;
		byPcValue[(uint32_t(e.pc) << 8) | e.value] += 1;
	}
	std::printf("=== записи В ТАБЛИЦУ СЛОТОВ (0xEDC0..0xEDFF): %d ===\n", toSlotTable);
	std::printf("  откуда (PC)   значение   сколько раз   что это значит\n");
	for (const auto &kv : byPcValue) {
		const uint16_t pc = uint16_t(kv.first >> 8);
		const uint8_t value = uint8_t(kv.first & 0xff);
		const char *meaning = (value == D110Core::kSlotIdleValue)   ? "СВОБОДЕН"
		                    : (value == D110Core::kSlotBusyValue)   ? "занят (0x40)"
		                    : (value == D110Core::kSlotBusyValueAlt) ? "занят (0x20)"
		                                                            : "?";
		std::printf("  0x%04X        0x%02X       %6d        %s\n", pc, value, kv.second, meaning);
	}
	if (toSlotTable == 0)
		std::printf("  ни одной записи - таблицу слотов за этот прогон не трогали вовсе\n");

	// Первые события по порядку: по ним видно, идёт ли выдача и возврат парой или только выдача.
	std::printf("\n=== первые двадцать записей в таблицу слотов, по порядку ===\n");
	int shown = 0;
	for (const auto &e : events) {
		if (e.addr < D110Core::kSlotStateTable || e.addr > D110Core::kSlotStateTable + 63) continue;
		std::printf("  PC 0x%04X  слот %2d  <- 0x%02X\n", e.pc, (e.addr - D110Core::kSlotStateTable) / 2, e.value);
		if (++shown >= 20) break;
	}

	// И соседние массивы того же окна - чтобы было видно, чем ещё занят обработчик.
	std::map<uint16_t, int> byArea;
	for (const auto &e : events) byArea[uint16_t(e.addr & 0xFFC0)] += 1;
	std::printf("\n=== куда ещё писали в этом окне ===\n");
	for (const auto &kv : byArea)
		std::printf("  0x%04X..0x%04X  %6d записей\n", kv.first, kv.first + 0x3F, kv.second);

	std::printf("\nзанятых слотов после игры и шести секунд тишины: %d из %d\n",
	            busySlots(proc), D110Core::kNumHardwareVoices);

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
