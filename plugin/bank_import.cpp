// Заливка банка тонов в настоящую батарейную память прибора - и подтверждение, что он
// вправду туда лёг.
//
// Нужен потому, что внутренняя память тонов у D-110 с завода ПУСТА и заводским сбросом не
// наполняется (проверено: после сброса в 0x4000 по-прежнему ноль ненулевых байт из 16384).
// Значит после аварии, обнулившей НВР, вернуть банк нечем: nvram_recovery.cpp чинит
// системную область и патчи, а тоны восстановить не может - их там никогда и не было.
//
// Идёт ТЕМ ЖЕ ПУТЁМ, что и плагин: D110AudioProcessor::importSysexBank() складывает
// сообщения в очередь, а processBlock выдаёт их и в звуковой движок, и в плату управления
// через core.pushMidi(). Своей реализации разбора SysEx здесь нет намеренно - иначе
// инструмент проверял бы себя, а не плагин.
//
// Путь к файлу берётся из аргумента и ТОЛЬКО из него. Имена папок в коллекции содержат
// неразрывный дефис (U+2011), который не переживает передачу через системную кодировку, -
// поэтому файл надо сперва скопировать по пути из обычных знаков и указать этот путь.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Ждать надо по часам и обязательно крутить processBlock: очередь импорта разбирает именно
// он, и никто больше. Счёт итераций тут дал бы ложное "не долетело" - сорок тысяч оборотов
// проходят за секунды, а кабель отдаёт свои 3125 байт в секунду и ни байтом быстрее.
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

// Сколько ненулевых байт в памяти тонов. Мера грубая и выбрана нарочно: она не зависит ни
// от одной догадки о раскладке записи, поэтому "ноль" от "не ноль" различает честно.
int toneBytes(D110AudioProcessor &proc) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return -1;
	int n = 0;
	for (int i = D110Core::kRamTones; i < D110Core::kRamSize; ++i)
		if (ram[(size_t)i] != 0) ++n;
	return n;
}

void printNames(D110AudioProcessor &proc, int count) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return;
	for (int t = 0; t < count; ++t) {
		std::printf("  тон %2d: '", t + 1);
		for (int i = 0; i < 10; ++i)
			std::printf("%c", ram[(size_t)D110Core::kRamTones + (size_t)t * 256 + (size_t)i]);
		std::printf("'\n");
	}
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 2) {
		std::printf("укажите файл банка: d110_bank_import <путь к .syx>\n");
		return 2;
	}
	const juce::File bank(juce::String::fromUTF8(argv[1]));
	if (!bank.existsAsFile()) {
		std::printf("файл не найден: %s\n", argv[1]);
		return 2;
	}

	std::printf("банк: %s (%lld байт)\n", bank.getFullPathName().toRawUTF8(),
	            (long long)bank.getSize());
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

	const int before = toneBytes(proc);
	std::printf("\nдо заливки: ненулевых байт в памяти тонов %d из %d\n",
	            before, D110Core::kRamSize - D110Core::kRamTones);

	proc.importSysexBank(bank);
	std::printf("%s\n", proc.getLastImportMessage().toRawUTF8());

	// С запасом к расчётному времени кабеля: очередь разбирается по блоку за раз, и прошивке
	// нужно ещё успеть разложить принятое по своим банкам. Замеряется всё равно результатом
	// ниже, а не этим сроком.
	std::printf("\nотдаю по кабелю на скорости MIDI...\n");
	render(proc, 25.0);

	const int after = toneBytes(proc);
	std::printf("\nпосле заливки: ненулевых байт в памяти тонов %d из %d\n",
	            after, D110Core::kRamSize - D110Core::kRamTones);
	if (after > 0) printNames(proc, 6);

	// Выключение - единственный момент, когда MAME пишет НВР на диск. Без него всё
	// залитое осталось бы только в памяти процесса.
	std::printf("\nвыключаю (это и есть момент записи на диск)...\n");
	proc.setPoweredOn(false);
	proc.releaseResources();

	// Независимая проверка: файл перечитывается с диска СВОИМИ силами, не через плагин.
	// Иначе подтверждением служил бы тот же код, который только что писал, - и обнуление,
	// случившееся при записи, осталось бы незамеченным ровно так же, как в прошлый раз.
	const juce::File rams = D110AudioProcessor::getNvramRoot().getChildFile("d110").getChildFile("rams");
	juce::MemoryBlock raw;
	if (!rams.loadFileAsData(raw) || raw.getSize() < (size_t)D110Core::kRamSize) {
		std::printf("файл НВР не прочитался: %s\n", rams.getFullPathName().toRawUTF8());
		return 1;
	}
	const auto *p = static_cast<const uint8_t *>(raw.getData());
	int onDisk = 0;
	for (int i = D110Core::kRamTones; i < D110Core::kRamSize; ++i)
		if (p[i] != 0) ++onDisk;
	std::printf("\nв файле на диске: ненулевых байт в памяти тонов %d из %d  %s\n",
	            onDisk, D110Core::kRamSize - D110Core::kRamTones,
	            onDisk > 0 ? "*** банк на месте ***" : "ПУСТО - на диск не легло");
	return onDisk > 0 ? 0 : 1;
}
