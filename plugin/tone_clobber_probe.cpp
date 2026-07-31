// Почему в демо-песне партии 6 и 7 тише на 21-24 дБ и почему партия 5 не получает нот.
//
// Зацепка прошлой сессии: прошивка держит ГРУППУ ТЕМБРА 5 ровно для этих двух партий,
// тогда как все остальные держат 0 или 1, а движок, происходящий от MT-32, знает только
// группы 0-3. Здесь вопрос задан в форме, на которую измерение уровня ответить не может,
// и ответ берётся из ОДНОГО прогона, чтобы ничего не сравнивать между прогонами:
//
//   Держит ли движок для каждой партии тот тембр, который держит ПРОШИВКА?
//
// Сравнение идёт по ИМЕНИ - первые десять байт тембра это его имя в ASCII, - потому что
// имя однозначно, а уровень нет. Совпавшие партии доказывают, что метод работает именно
// на этом прогоне; несовпавшая партия прямо называет звук, который играет по ошибке.
//
// Три прежних инструмента в этом расследовании дали уверенные неверные ответы, поэтому:
//   * каждая запись, способная переполниться, печатает счётчик потерь, а подсчёты,
//     которые нельзя обрезать, ведутся счётчиками фиксированного размера
//     (D110Core::noteOnsForPart и соседние);
//   * путь чтения из движка доказывается КОНТРОЛЕМ - записали известное значение и
//     прочитали его обратно - прежде чем верить хоть одному его показанию.
//     plugin/part_state_compare.cpp читал из движка нули, и никто не мог сказать,
//     виноват движок или читатель;
//   * играющая песня печатается в каждом блоке, потому что демо переходит от песни к
//     песне, а разные песни используют разные партии.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

// Roland пишет адрес тремя отдельными семибитными байтами, а движок адресует ту же
// память одним упакованным 21-битным числом. MT32EMU_MEMADDR - это преобразование,
// повторённое здесь, чтобы инструменту не пришлось тянуть внутренние заголовки движка.
constexpr uint32_t packed(uint32_t a) {
	return ((a & 0x7f0000u) >> 2) | ((a & 0x7f00u) >> 1) | (a & 0x7fu);
}
constexpr uint32_t kPatchTempSysex = 0x030000; // "Timbre Temporary" в терминах Roland D-110
constexpr uint32_t kToneTempSysex = 0x040000;  // "Tone Temporary" - собственно тембр
constexpr uint16_t kPatchTempRam = 0x2000;
constexpr uint16_t kToneTempRam = 0x21E4;
constexpr int kToneStride = 246;

// ---- чтение ЖКИ, чтобы песня и индикаторы партий попадали в протокол вместе с цифрами ----

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
	const auto dir = D110AudioProcessor::getAutoRomFolder();
	for (const auto &entry : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFiles)) {
		juce::MemoryBlock data;
		if (entry.getFile().loadFileAsData(data) && isCgrom(data)) {
			g_cgrom.assign(static_cast<const uint8_t *>(data.getData()),
			               static_cast<const uint8_t *>(data.getData()) + 4096);
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

std::string lcdRow(D110AudioProcessor &proc, int row) {
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return {};
	std::string s;
	for (int col = 0; col < D110Core::kCols; ++col)
		s.push_back(decodeCell(rows + ((size_t)row * D110Core::kCols + col) * D110Core::kRowsPerChar));
	return s;
}

// Печатает верхнюю строку панели точками, по знакоместу на блок. Индикаторы партий - это
// первые девять знакомест, и означает ли знакоместо "партия играет" или "партия молчит",
// угадывать нельзя: на этом держится вся жалоба "партия 5 не получает нот, хотя её
// индикатор горит". Знакоместо, которое знакогенератор назвать не может, - это
// ПОЛЬЗОВАТЕЛЬСКИЙ символ, а блок активности у Roland именно такой, поэтому на рисунок
// надо смотреть, а не декодировать его.
void dumpIndicatorGlyphs(D110AudioProcessor &proc) {
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) { std::printf("  (no LCD)\n"); return; }
	for (int r = 0; r < D110Core::kRowsPerChar && r < 7; ++r) {
		std::printf("   ");
		for (int col = 0; col < 10; ++col) {
			const uint8_t bits = rows[(size_t)col * D110Core::kRowsPerChar + r];
			for (int b = 4; b >= 0; --b) std::printf("%c", (bits >> b) & 1 ? '#' : '.');
			std::printf(" ");
		}
		std::printf("\n");
	}
	std::printf("   ");
	for (int col = 0; col < 10; ++col)
		std::printf("  %c   ", decodeCell(rows + (size_t)col * D110Core::kRowsPerChar));
	std::printf("   <- что знакогенератор делает из каждого знакоместа\n");
}

void press(D110AudioProcessor &proc, std::initializer_list<int> idx, int hold, int settle) {
	for (int i : idx) proc.getCore().setButton(i, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(hold));
	for (int i : idx) proc.getCore().setButton(i, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(settle));
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

// Первые десять байт тембра - его имя. Непечатаемые байты показываются как '.', чтобы
// блок нулей или мусора выглядел как явно не-имя, а не как пустая строка.
std::string toneName(const uint8_t *p) {
	std::string s;
	for (int i = 0; i < 10; ++i) s.push_back((p[i] >= 0x20 && p[i] < 0x7f) ? char(p[i]) : '.');
	return s;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	// СЧИТАЕМ звук, а не спим. Мост ставит в очередь DT1 на каждый зеркалируемый регион,
	// который меняется, пока прошивка загружается, а разбирает это кольцо только
	// processBlock. Проспать загрузку - значит оставить весь затор в очереди, и первый же
	// последующий расчёт звука его проигрывает: именно так была молча затёрта собственная
	// контрольная запись этого инструмента, и исправный путь чтения выглядел сломанным.
	render(proc, 9.0);
	std::printf("firmware running: %s   sound engine open: %s   LCD font: %s\n",
	            proc.getCore().isRunning() ? "yes" : "NO",
	            proc.engineIsOpen() ? "yes" : "NO",
	            g_cgrom.empty() ? "NOT FOUND (screens will read as ?)" : "loaded");
	if (!proc.engineIsOpen()) {
		std::printf("\nThe engine never opened, so every reading below would be the buffer\n"
		            "this tool filled in itself. Stopping instead of printing them.\n");
		return 1;
	}

	// ---- КОНТРОЛЬ: работает ли вообще путь чтения из движка? ---------------------------
	// Записали, потом прочитали обратно. Пока это не прошло, ничего прочитанное из движка
	// не значит ничего - ровно в таком состоянии и остался part_state_compare.cpp.
	//
	// ДВЕ вещи этот контроль поймал с первой же попытки, и обе прочитались бы как находки
	// про сам D-110:
	//  1. Synth::playSysex ставит сообщение В ОЧЕРЕДЬ; применяется оно во время расчёта
	//     звука. Запись и чтение обратно без расчёта между ними читают значение ДО записи.
	//  2. Мост пересылает весь регион Timbre Temporary всякий раз, когда меняется ОЗУ
	//     прошивки, и это затирает всё записанное здесь. Поэтому рядом печатается счётчик
	//     отправок этого региона: неудачное чтение при ненулевом счётчике - это прошивка
	//     забирает свою память назад, а не сломанный читатель.
	{
		proc.getCore().resetTallies();
		// Целая 16-байтная запись Timbre Temporary для ритм-партии (индекс 8), которую демо
		// по этому пути не правит. Значения лежат внутри собственной таблицы максимумов
		// D-110, поэтому ничего не прижимается и чтение можно сравнивать точно.
		const uint8_t want[16] = {2, 17, 24, 50, 12, 1, 0, 0, 77, 9, 0, 0, 0, 0, 0, 0};
		uint8_t msg[32];
		int n = 0;
		msg[n++] = 0xF0; msg[n++] = 0x41; msg[n++] = 0x10; msg[n++] = 0x16; msg[n++] = 0x12;
		const uint32_t addr = kPatchTempSysex + 0x100; // ритм-партия, по карте самой Roland
		const uint8_t a1 = uint8_t((addr >> 16) & 0x7f), a2 = uint8_t((addr >> 8) & 0x7f),
		              a3 = uint8_t(addr & 0x7f);
		msg[n++] = a1; msg[n++] = a2; msg[n++] = a3;
		uint32_t sum = a1 + a2 + a3;
		for (int i = 0; i < 16; ++i) { msg[n++] = want[i]; sum += want[i]; }
		msg[n++] = uint8_t((128 - (sum & 0x7f)) & 0x7f);
		msg[n++] = 0xF7;
		proc.engineWriteSysexForTest(msg, n);
		render(proc, 0.3); // движок применяет очередь эксклюзивов, пока считает звук

		uint8_t got[16];
		std::memset(got, 0xAA, sizeof got);
		proc.engineReadMemory(packed(kPatchTempSysex) + 16 * 8, 16, got);
		const bool ok = std::memcmp(want, got, 10) == 0;
		std::printf("\nCONTROL - write a known Timbre Temporary entry, read it back:\n  wrote:");
		for (int i = 0; i < 10; ++i) std::printf(" %3d", want[i]);
		std::printf("\n  read :");
		for (int i = 0; i < 10; ++i) std::printf(" %3d", got[i]);
		std::printf("\n  (the bridge re-sent this region %llu times meanwhile)\n",
		            (unsigned long long)proc.getCore().regionEmitCount(0));
		std::printf("  => engine read path %s\n", ok ? "WORKS" : "IS BROKEN - stop here");
		if (!ok) return 1;

		// Возвращаем собственное состояние прошивки на место до всяких измерений.
		proc.getCore().resyncMirror();
		render(proc, 0.5);
	}

	// ---- запускаем демо -----------------------------------------------------------------
	proc.getCore().resetTallies();
	proc.getCore().startNoteLog();
	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 500);
	press(proc, {D110Core::buttonIndex(1, 0)}, 200, 500);

	for (int round = 0; round < 4; ++round) {
		render(proc, 7.5);

		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());

		std::printf("\n=== %2d s into the demo | screen: \"%s\" / \"%s\" ===\n",
		            (round + 1) * 8, lcdRow(proc, 0).c_str(), lcdRow(proc, 1).c_str());
		std::printf(" part | firmware grp/tone lvl pan | tone the FIRMWARE holds"
		            " | tone the ENGINE holds\n");
		for (int p = 0; p < 8; ++p) {
			const uint8_t *fwPatch = &ram[kPatchTempRam + 16 * p];
			const uint8_t *fwTone = &ram[kToneTempRam + kToneStride * p];

			uint8_t engTone[16];
			std::memset(engTone, 0xAA, sizeof engTone);
			proc.engineReadMemory(packed(kToneTempSysex) + uint32_t(kToneStride) * uint32_t(p),
			                      10, engTone);

			const std::string fwName = toneName(fwTone), engName = toneName(engTone);
			std::printf("  %3d | grp %d  tone %2d  %3d %2d | \"%s\" | \"%s\" %s\n",
			            p + 1, fwPatch[0], fwPatch[1], fwPatch[8], fwPatch[9],
			            fwName.c_str(), engName.c_str(),
			            fwName == engName ? "" : "  <-- MISMATCH");
		}
		std::printf("  the nine part indicators, as dots (parts 1-8 then rhythm):\n");
		dumpIndicatorGlyphs(proc);
	}

	// ---- подсчёты, которые ничего не могли потерять -------------------------------------
	const auto log = proc.getCore().takeNoteLog();
	std::printf("\nNotes the firmware started, per part (fixed counters - nothing can be lost):\n");
	std::printf("  part | note-ons | writes to the part byte naming this part\n");
	for (int p = 0; p < 9; ++p)
		std::printf("  %4s | %8llu | %llu\n", p == 8 ? "rhy" : std::to_string(p + 1).c_str(),
		            (unsigned long long)proc.getCore().noteOnsForPart(p),
		            (unsigned long long)proc.getCore().partByteWrites(p));
	std::printf("  part-byte writes naming something that is not a part (9-15):");
	for (int v = 9; v < 16; ++v)
		std::printf(" %llu", (unsigned long long)proc.getCore().partByteWrites(v));
	std::printf("\n  note log held %zu events, dropped %llu\n", log.size(),
	            (unsigned long long)proc.getCore().noteLogDropped_());

	std::printf("\nDT1 messages sent per mirrored region - a region that fires alone can\n"
	            "undo what another region set up:\n");
	for (int i = 0; i < D110Core::kNumMirrorRegions; ++i)
		std::printf("  %-28s %llu\n", D110Core::kMirrorRegions[i].name,
		            (unsigned long long)proc.getCore().regionEmitCount(i));
	std::printf("  mirror messages the ring had no room for: %llu%s\n",
	            (unsigned long long)proc.getCore().sysexDropped(),
	            proc.getCore().sysexDropped() ? "   <-- READINGS ABOVE ARE LOWER BOUNDS" : "");

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
