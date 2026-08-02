// Доходит ли правка из расширенного редактора до прибора, до звука и обратно?
//
// Ящик ничего не подкладывает ни в память прошивки, ни в звуковой движок: он посылает
// прибору эксклюзивное сообщение на один параметр - ровно то же, что прислал бы внешний
// редактор по MIDI. Дальше работает уже проверенный путь: прошивка меняет свою память,
// зеркало переносит изменение в движок. Здесь проверяются обе половины этой цепочки.
//
// Проверок три, и каждая устроена так, чтобы уметь показать отказ:
//
//   1. КАЖДЫЙ отправитель процессора кладёт свой байт в измеренное место памяти прошивки.
//      Байт сперва читается, потом ставится ЗАВЕДОМО ДРУГОЙ - иначе «совпало» ничего не
//      значит, ведь параметр мог уже там стоять.
//   2. Правка слышна: громкость партии 1 опускается со ста до десяти, и берётся тот же
//      аккорд. Контроль - та же пара измерений БЕЗ правки между ними.
//   3. Переход на патч кнопками панели доводит прибор до запрошенного номера, а не до
//      соседнего: проверяются и «вперёд через границу банка», и обратный ход.
//
// Всё, что зонд наменял, в конце снимается заводским сбросом: память прибора - общая с
// плагином, и оставлять в ней свои метки нельзя.
#include "Source/PluginProcessor.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

int g_passed = 0, g_failed = 0;

void check(bool ok, const char *what, const juce::String &detail) {
	std::printf("  [%s] %s   %s\n", ok ? " OK " : "FAIL", what, detail.toRawUTF8());
	if (ok) ++g_passed; else ++g_failed;
}

void render(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> block(2, kBlock);
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int i = 0; i < blocks; ++i) {
		juce::MidiBuffer none;
		block.clear();
		proc.processBlock(block, none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

int byteAt(D110AudioProcessor &proc, int offset) {
	const auto ram = snapshot(proc);
	return (offset >= 0 && offset < D110Core::kRamSize) ? int(ram[(size_t)offset]) : -1;
}

// Значение, заведомо отличное от текущего и не выходящее за предел параметра.
uint8_t differentFrom(int current, int hi) {
	const int candidate = (current == hi) ? hi - 1 : current + 1;
	return uint8_t(juce::jlimit(0, hi, candidate));
}

double chordRms(D110AudioProcessor &proc, int channel) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	juce::MidiBuffer on;
	for (int note : { 48, 52, 55 }) on.addEvent(juce::MidiMessage::noteOn(channel, note, 0.9f), 0);
	double sumSq = 0.0;
	int64_t samples = 0;
	const int blocks = int(1.5 * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		juce::MidiBuffer midi;
		if (b == 0) midi = on;
		proc.processBlock(buffer, midi);
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
		if (b < blocks / 5) continue;   // атака не в счёт: считаем установившийся звук
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
			const float *d = buffer.getReadPointer(ch);
			for (int i = 0; i < buffer.getNumSamples(); ++i) {
				sumSq += double(d[i]) * double(d[i]);
				++samples;
			}
		}
	}
	juce::MidiBuffer off;
	for (int note : { 48, 52, 55 }) off.addEvent(juce::MidiMessage::noteOff(channel, note), 0);
	buffer.clear();
	proc.processBlock(buffer, off);
	render(proc, 0.8);
	return samples > 0 ? std::sqrt(sumSq / double(samples)) : 0.0;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s   ПЗУ: %s\n\n", proc.getCore().isRunning() ? "работает" : "НЕТ",
	            proc.isSynthReady() ? "загружены" : "НЕТ");
	if (!proc.getCore().isRunning()) return 1;

	// --- 1. каждый отправитель кладёт свой байт куда должен ---------------------
	std::printf("=== 1. КАЖДАЯ ОБЛАСТЬ, ЧЕРЕЗ ОТПРАВИТЕЛИ ПРОЦЕССОРА ===\n");
	{
		struct Case {
			const char *what;
			int ramOffset;
			int hi;
			std::function<void(uint8_t)> send;
		};
		const Case kCases[] = {
			{ "Timbre Temporary, партия 3, Output Level",
			  D110Core::kRamTimbreTemp + 2 * D110Core::kTimbreTempRecord + 8, 100,
			  [&proc](uint8_t v) { proc.sendTimbreTempParam(2, 8, v); } },
			{ "Tone Temporary, партия 2, TVF Cutoff партиала 1",
			  D110Core::kRamToneTemp + D110Core::kToneRecord + 14 + 23, 100,
			  [&proc](uint8_t v) { proc.sendToneTempParam(1, 14 + 23, v); } },
			{ "Rhythm Setup, запись 17, Output Level",
			  D110Core::kRamRhythmTemp + 16 * D110Core::kRhythmRecord + 1, 100,
			  [&proc](uint8_t v) { proc.sendRhythmParam(16, 1, v); } },
			{ "System, Partial Reserve партии 2",
			  D110Core::kRamSystem + 5, 32,
			  [&proc](uint8_t v) { proc.sendSystemParam(5, v); } },
			{ "Timbre Memory, ячейка 6, Key Shift",
			  D110Core::kRamTimbres + 5 * D110Core::kTimbreRecord + 2, 48,
			  [&proc](uint8_t v) { proc.sendTimbreMemoryParam(5, 2, v); } },
			{ "Patch Memory, патч 4, Reverb Level",
			  D110Core::kRamPatches + 3 * D110Core::kPatchRecord + 12, 7,
			  [&proc](uint8_t v) { proc.sendPatchMemoryParam(3, 12, v); } },
		};

		for (const Case &c : kCases) {
			const int before = byteAt(proc, c.ramOffset);
			const uint8_t wanted = differentFrom(before, c.hi);
			c.send(wanted);
			render(proc, 1.0);
			const int after = byteAt(proc, c.ramOffset);
			check(after == int(wanted), c.what,
			      "0x" + juce::String::toHexString(c.ramOffset) + ": " + juce::String(before)
			          + " -> " + juce::String(after) + ", хотели " + juce::String(int(wanted)));
		}

		// Имя - десять байт разом, тем же путём.
		proc.sendName(D110Core::kSysexToneTemp, 0, "EditorTest");
		render(proc, 1.2);
		const auto ram = snapshot(proc);
		juce::String read;
		for (int i = 0; i < 10; ++i) read += char(ram[(size_t)D110Core::kRamToneTemp + (size_t)i]);
		check(read == "EditorTest", "Tone Temporary, партия 1, имя", "прочитано \"" + read + "\"");
	}

	// --- 2. слышно ли это ------------------------------------------------------
	//
	// Контроль обязателен: сам по себе «стало тише» ничего не доказывает, потому что второе
	// измерение отличается от первого ещё и тем, что оно второе. Поэтому сперва берутся два
	// измерения БЕЗ правки между ними, и только потом - с правкой.
	std::printf("\n=== 2. ДОХОДИТ ЛИ ПРАВКА ДО ЗВУКА ===\n");
	{
		proc.sendTimbreTempParam(0, 8, 100);   // партия 1 на полной громкости
		render(proc, 1.0);
		const double a = chordRms(proc, 2);    // партия 1 отвечает на канале 2
		const double control = chordRms(proc, 2);
		proc.sendTimbreTempParam(0, 8, 10);    // и та же партия на десяти
		render(proc, 1.0);
		const double quiet = chordRms(proc, 2);

		const double controlDb = 20.0 * std::log10(juce::jmax(1e-9, control) / juce::jmax(1e-9, a));
		const double quietDb = 20.0 * std::log10(juce::jmax(1e-9, quiet) / juce::jmax(1e-9, a));
		std::printf("  RMS: %.6f -> контроль %.6f (%.1f дБ) -> после правки %.6f (%.1f дБ)\n",
		            a, control, controlDb, quiet, quietDb);
		check(std::abs(controlDb) < 1.5, "контроль: без правки громкость не меняется",
		      juce::String(controlDb, 2) + " дБ");
		check(quietDb < -6.0, "Output Level 100 -> 10 слышен",
		      juce::String(quietDb, 2) + " дБ");
		proc.sendTimbreTempParam(0, 8, 100);
		render(proc, 0.5);
	}

	// --- 3. переход на патч кнопками панели -------------------------------------
	//
	// Таймер, который жмёт кнопки, живёт на очереди сообщений, а в консольной программе её
	// никто не крутит - поэтому здесь она крутится явно. В плагине этим занимается хозяин
	// окна, и ничего заводить не нужно.
	std::printf("\n=== 3. ПЕРЕХОД НА ПАТЧ КНОПКАМИ ПАНЕЛИ ===\n");
	{
		auto pump = [&proc](int ms) {
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
			while (std::chrono::steady_clock::now() < until) {
				juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
				juce::AudioBuffer<float> block(2, kBlock);
				juce::MidiBuffer none;
				block.clear();
				proc.processBlock(block, none);
			}
		};

		// Вперёд через границу банка, назад внутри банка и точно в начало - три разных пути
		// через ту же арифметику.
		for (int target : { 27, 3, 0 }) {
			proc.selectPatch(target);
			pump(4000);
			const int now = byteAt(proc, D110Core::kRamPatchNumber);
			check(now == target, "переход на патч",
			      "просили I-" + juce::String(target / 8 + 1) + juce::String(target % 8 + 1)
			          + ", прибор стоит на I-" + juce::String(now / 8 + 1)
			          + juce::String(now % 8 + 1));
		}
	}

	// --- 4. правка слышна СРАЗУ ------------------------------------------------
	//
	// Две вещи, которые на приборе устроены не так, как ждёт рука. Тон из памяти сам по себе
	// не звучит - звучит временная область партии, и тон надо туда положить. Патч в памяти
	// тоже не звучит - прибор играет из временных областей, куда патч попадает только при
	// выборе. Редактор делает оба переноса сам; здесь проверяется, что он их вправду делает.
	std::printf("\n=== 4. ПОДСТАНОВКА: ТОН И ПАТЧ СЛЫШНЫ СРАЗУ ===\n");
	{
		// Тон из ячейки памяти - в партию 3.
		const auto ram = snapshot(proc);
		const size_t slotAt = size_t(D110Core::kRamTones) + 4 * D110Core::kToneMemRecord;
		juce::String wanted;
		for (int i = 0; i < 10; ++i) {
			const char ch = char(ram[slotAt + size_t(i)]);
			wanted += (ch >= 32 && ch < 127) ? ch : ' ';
		}
		proc.auditionTone(2, 4);
		render(proc, 1.6);
		const auto after = snapshot(proc);
		juce::String got;
		for (int i = 0; i < 10; ++i) {
			const char ch = char(after[size_t(D110Core::kRamToneTemp)
			                          + 2 * D110Core::kToneRecord + size_t(i)]);
			got += (ch >= 32 && ch < 127) ? ch : ' ';
		}
		// Пустая ячейка ничего не доказывает: если память тонов не заполнена, сравнивать
		// нечего, и это надо сказать, а не выдать за успех.
		if (wanted.trim().isEmpty())
			std::printf("  [ -- ] ячейка памяти тонов пуста, проверять нечего\n");
		else
			check(got == wanted, "тон из памяти встал в партию 3",
			      "ячейка \"" + wanted + "\", в партии \"" + got + "\"");

		// Поле патча, который прибор играет: должно измениться И в памяти, И в живой области.
		const int current = proc.currentPatchNumber();
		if (current < 0) {
			std::printf("  [FAIL] номер текущего патча не прочитан\n");
			++g_failed;
		} else {
			constexpr int kPart = 1;
			const int field = 31 + kPart * 12 + 8;   // Output Level второй партии
			const int storedAt = D110Core::kRamPatches + current * D110Core::kPatchRecord + field;
			const int liveAt = D110Core::kRamTimbreTemp + kPart * D110Core::kTimbreTempRecord + 8;
			const uint8_t v = differentFrom(byteAt(proc, liveAt), 100);
			proc.editPatchField(current, field, v);
			render(proc, 1.4);
			check(byteAt(proc, storedAt) == int(v), "правка легла в память патча",
			      "0x" + juce::String::toHexString(storedAt) + " = "
			          + juce::String(byteAt(proc, storedAt)));
			check(byteAt(proc, liveAt) == int(v), "и в живую область, то есть слышна",
			      "0x" + juce::String::toHexString(liveAt) + " = "
			          + juce::String(byteAt(proc, liveAt)));
		}

		// КОНТРОЛЬ: у ЧУЖОГО патча живая область двигаться не должна - иначе редактор менял
		// бы звук там, где его не просили.
		if (current >= 0) {
			const int other = (current + 1) % D110Core::kNumPatches;
			constexpr int kPart = 3;
			const int field = 31 + kPart * 12 + 8;
			const int liveAt = D110Core::kRamTimbreTemp + kPart * D110Core::kTimbreTempRecord + 8;
			const int before = byteAt(proc, liveAt);
			proc.editPatchField(other, field, differentFrom(before, 100));
			render(proc, 1.4);
			check(byteAt(proc, liveAt) == before,
			      "контроль: правка ЧУЖОГО патча звук не трогает",
			      "живой байт остался " + juce::String(byteAt(proc, liveAt)));
		}
	}

	std::printf("\n=== ИТОГ: %d прошло, %d не прошло ===\n", g_passed, g_failed);

	// Убираем за собой: зонд писал в настоящую батарейную память прибора.
	std::printf("\nзаводской сброс...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 8.0);

	proc.setPoweredOn(false);
	proc.releaseResources();
	return g_failed == 0 ? 0 : 1;
}
