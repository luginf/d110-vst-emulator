// Доходит ли ПРАВКА, посланная эксклюзивным сообщением, до самой прошивки - и куда именно
// она ложится в её памяти.
//
// Зеркало (D110Core::emitRegionSysex) до сих пор работало в одну сторону: прошивка правит
// свою память, мост несёт это в звуковой движок. Расширенный редактор идёт в другую
// сторону - он посылает Roland DT1 в MIDI IN прошивки, ровно как это делает внешний
// библиотекарь с настоящим прибором, - и ни одного измерения этого пути ещё не было.
// Отсюда три вопроса, и на каждый нужен ответ, умеющий показать отказ:
//
//   1. Принимает ли прошивка DT1 вообще и в те ли байты кладёт? Проверяется по ВРЕМЕННЫМ
//      областям, чьи адреса в ОЗУ уже измерены другими зондами: попадание в известный
//      байт - это одновременно и результат, и его контроль.
//   2. Принимает ли она запись в память патчей, тембров и тонов, или это запрещает
//      Mem Protect (заводское значение - ON)? Ответ решает, что редактору вообще можно
//      предлагать.
//   3. Где в ОЗУ лежит память ТОНОВ? Единственная область карты Roland, чьё место не
//      измерено: верхние 16 КБ ОЗУ в заводском состоянии сплошь нули, и по содержимому её
//      не найти. Зато можно записать туда имя и посмотреть, какие байты сдвинулись.
//
// И заодно четвёртый, уже про удобство: каким байтом прошивка помнит НОМЕР ТЕКУЩЕГО ПАТЧА,
// чтобы редактор мог переходить на нужный патч кнопками самой панели, а не выдумывать
// смену патча сам.
//
// В конце прогона делается заводской сброс: зонд пишет в настоящую батарейную память
// прибора, общую с плагином, и оставлять в ней свои метки нельзя.
#include "Source/PluginProcessor.h"

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

struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit", 0, 7}, {"Patch", 0, 6}, {"Timbre", 0, 5}, {"Part+", 0, 4},
	{"Group+", 0, 3}, {"Bank+", 0, 2}, {"Number+", 0, 1}, {"Write", 0, 0},
	{"Edit", 1, 7}, {"Part", 1, 6}, {"System", 1, 5}, {"Part-", 1, 4},
	{"Group-", 1, 3}, {"Bank-", 1, 2}, {"Number-", 1, 1}, {"Enter", 1, 0},
};

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
	uint8_t rows[D110Core::kLcdBytes];
	if (!proc.getCore().getLcd(rows)) return "(нет экрана)";
	std::string s;
	for (int row = 0; row < 2; ++row) {
		if (row) s += " / ";
		for (int col = 0; col < D110Core::kCols; ++col)
			s.push_back(decodeCell(rows + ((size_t)row * D110Core::kCols + col)
			                       * D110Core::kRowsPerChar));
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

void press(D110AudioProcessor &proc, const char *name, int times = 1) {
	for (const auto &b : kButtons)
		if (std::strcmp(b.name, name) == 0) {
			const int idx = D110Core::buttonIndex(b.port, b.bit);
			for (int i = 0; i < times; ++i) {
				proc.getCore().setButton(idx, true);
				render(proc, 0.13);
				proc.getCore().setButton(idx, false);
				render(proc, 0.30);
			}
			return;
		}
	std::printf("  !!! нет такой кнопки: %s\n", name);
}

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

// Байты, которые действительно изменились. Рабочие области прошивки (0x2Dxx выше системной,
// 0x36xx - буфер экрана, 0x39xx) шевелятся сами по себе между любыми двумя снимками,
// поэтому они печатаются отдельно от попаданий в ожидаемое место, а не вперемешку.
struct Diff {
	std::vector<int> at;
	bool hit = false;
};

Diff reportDiff(const std::vector<uint8_t> &before, const std::vector<uint8_t> &after,
                int expected, int length, const char *what) {
	Diff d;
	for (int i = 0; i < D110Core::kRamSize; ++i)
		if (before[i] != after[i]) d.at.push_back(i);

	int inRange = 0;
	for (int i : d.at)
		if (expected >= 0 && i >= expected && i < expected + length) ++inRange;
	d.hit = (expected < 0) ? !d.at.empty() : (inRange > 0);

	std::printf("    %s: изменившихся байт %d", what, int(d.at.size()));
	if (expected >= 0)
		std::printf(", из них в ожидаемом месте 0x%04X..0x%04X - %d %s", expected,
		            expected + length - 1, inRange, inRange ? "" : "  <-- НЕ ДОШЛО");
	std::printf("\n      ");
	for (size_t i = 0; i < d.at.size() && i < 14; ++i)
		std::printf("0x%04X %02X->%02X  ", d.at[i], before[d.at[i]], after[d.at[i]]);
	if (d.at.size() > 14) std::printf("... ещё %d", int(d.at.size()) - 14);
	std::printf("\n");
	return d;
}

// Одна правка эксклюзивным сообщением - ровно так, как её будет посылать редактор.
void sendDt1(D110AudioProcessor &proc, uint32_t address, int offset, const uint8_t *data,
             int length) {
	uint8_t msg[D110Core::kMaxSysexBytes];
	const int n = D110Core::buildDt1Message(address, offset, data, length, msg);
	if (n <= 0) { std::printf("    !!! сообщение не построено\n"); return; }
	proc.getCore().pushMidi(msg, n);
	render(proc, 1.2);   // байты идут со скоростью MIDI, прошивке нужно их разобрать
}

void sendByte(D110AudioProcessor &proc, uint32_t address, int offset, uint8_t value) {
	sendDt1(proc, address, offset, &value, 1);
}

// Проверка одной области: снимок, посылка, снимок, отчёт.
bool checkWrite(D110AudioProcessor &proc, const char *what, uint32_t address, int offset,
                const uint8_t *data, int length, int expectedRam) {
	std::printf("\n  %s   адрес %02X %02X %02X + %d\n", what, (address >> 16) & 0x7f,
	            (address >> 8) & 0x7f, address & 0x7f, offset);
	const auto before = snapshot(proc);
	sendDt1(proc, address, offset, data, length);
	const auto after = snapshot(proc);
	const Diff d = reportDiff(before, after, expectedRam, length, "ОЗУ");
	return d.hit;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s   знакогенератор: %s\n",
	            proc.getCore().isRunning() ? "работает" : "НЕТ",
	            g_cgrom.empty() ? "НЕ НАЙДЕН" : "загружен");
	if (!proc.getCore().isRunning()) return 1;

	press(proc, "Exit", 2);
	std::printf("экран: \"%s\"\n", screen(proc).c_str());

	// --- 1. временные области: адреса известны, значит это контроль ------------
	std::printf("\n=== 1. ВРЕМЕННЫЕ ОБЛАСТИ (адреса в ОЗУ уже измерены) ===\n");

	int passed = 0, total = 0;

	// Timbre Temporary, партия 3, громкость. 0x2000 + 2*16 + 8.
	{
		const uint8_t v = 0x55;
		++total;
		if (checkWrite(proc, "Timbre Temporary, партия 3, Output Level = 85",
		               D110Core::kSysexTimbreTemp, 2 * D110Core::kTimbreTempRecord + 8, &v, 1,
		               D110Core::kRamTimbreTemp + 2 * D110Core::kTimbreTempRecord + 8))
			++passed;
	}

	// Tone Temporary, партия 2, имя. 0x21E4 + 246.
	{
		const uint8_t name[10] = { 'P','R','O','B','E','T','O','N','E','2' };
		++total;
		if (checkWrite(proc, "Tone Temporary, партия 2, имя = PROBETONE2",
		               D110Core::kSysexToneTemp, D110Core::kToneRecord, name, 10,
		               D110Core::kRamToneTemp + D110Core::kToneRecord))
			++passed;
	}

	// Rhythm Setup, шестнадцатая запись, громкость. 0x2090 + 16*4 + 1.
	{
		const uint8_t v = 0x40;
		++total;
		if (checkWrite(proc, "Rhythm Setup, запись 17, Output Level = 64",
		               D110Core::kSysexRhythmTemp, 16 * D110Core::kRhythmRecord + 1, &v, 1,
		               D110Core::kRamRhythmTemp + 16 * D110Core::kRhythmRecord + 1))
			++passed;
	}

	// System Area, резерв партиалов партии 1. 0x2D94 + 4.
	{
		const uint8_t v = 3;
		++total;
		if (checkWrite(proc, "System, Partial Reserve партии 1 = 3",
		               D110Core::kSysexSystem, 4, &v, 1, D110Core::kRamSystem + 4))
			++passed;
	}

	// --- 2. память: её может запрещать Mem Protect ---------------------------
	std::printf("\n=== 2. ПАМЯТЬ ПАТЧЕЙ И ТЕМБРОВ (Mem Protect заводски ON) ===\n");

	// Timbre Memory, ячейка 6, Key Shift. 0x2994 + 5*8 + 2. Место измерено содержимым:
	// 128 записей по 8 байт, ровно между Tone Temporary и системной областью.
	{
		const uint8_t v = 30;
		++total;
		if (checkWrite(proc, "Timbre Memory I-A6, Key Shift = 30",
		               D110Core::kSysexTimbres, 5 * D110Core::kTimbreRecord + 2, &v, 1,
		               D110Core::kRamTimbres + 5 * D110Core::kTimbreRecord + 2))
			++passed;
	}

	// Patch Memory, патч 4, имя. 0x0000 + 3*128.
	{
		const uint8_t name[10] = { 'P','R','O','B','E',' ',' ',' ','0','4' };
		++total;
		if (checkWrite(proc, "Patch Memory 4, имя = PROBE   04",
		               D110Core::kSysexPatches, 3 * D110Core::kPatchRecord, name, 10,
		               D110Core::kRamPatches + 3 * D110Core::kPatchRecord))
			++passed;
	}

	// --- 3. память тонов: место НЕ известно, его и ищем ----------------------
	//
	// Единственная область карты Roland, которую нельзя найти по содержимому: в заводском
	// приборе она пуста, и верхние 16 КБ ОЗУ - сплошные нули. Поэтому она ищется записью:
	// два тона, разнесённые на две записи, и место обоих измеряется, а не предполагается.
	//
	// Искать надо ВСЕ вхождения, а не первое: пришедшее сообщение лежит ещё и в приёмном
	// буфере прошивки (0x39xx-0x3Axx), и первое совпадение - всегда он. Первая версия
	// этого зонда попалась именно на этом и объявила базой адрес буфера.
	std::printf("\n=== 3. ПАМЯТЬ ТОНОВ: куда она ляжет? ===\n");
	{
		auto findAll = [](const std::vector<uint8_t> &ram, const uint8_t *pat, int len) {
			std::vector<int> hits;
			for (int i = 0; i + len <= D110Core::kRamSize; ++i)
				if (std::memcmp(ram.data() + i, pat, (size_t)len) == 0) hits.push_back(i);
			return hits;
		};

		const uint8_t name1[10] = { 'P','R','O','B','E','T','O','N','E','1' };
		std::printf("\n  Tone Memory 1, имя = PROBETONE1   адрес 08 00 00\n");
		const auto before = snapshot(proc);
		sendDt1(proc, D110Core::kSysexTones, 0, name1, 10);
		const auto after = snapshot(proc);
		reportDiff(before, after, -1, 10, "ОЗУ");
		const auto hits1 = findAll(after, name1, 10);
		for (int h : hits1)
			std::printf("    PROBETONE1 в ОЗУ по 0x%04X%s\n", h,
			            (h >= 0x3900 && h < 0x3C00) ? "   (приёмный буфер прошивки)" : "");

		// Вторая запись, через тон: если у обеих разница ровно 512 байт, это массив с шагом
		// 256, то есть память тонов, а не случайное совпадение.
		const uint8_t name2[10] = { 'P','R','O','B','E','T','O','N','E','3' };
		std::printf("\n  Tone Memory 3, имя = PROBETONE3   адрес 08 04 00\n");
		sendDt1(proc, D110Core::kSysexTones, 2 * 256, name2, 10);
		render(proc, 0.5);
		const auto after2 = snapshot(proc);
		const auto hits2 = findAll(after2, name2, 10);
		for (int h : hits2)
			std::printf("    PROBETONE3 в ОЗУ по 0x%04X%s\n", h,
			            (h >= 0x3900 && h < 0x3C00) ? "   (приёмный буфер прошивки)" : "");

		bool measured = false;
		for (int a : hits1)
			for (int b : hits2)
				if (b - a == 2 * 256) {
					std::printf("    ИЗМЕРЕНО: RAM 0x%04X == SysEx 08 00 00, шаг 256 байт\n", a);
					measured = true;
				}
		if (!measured)
			std::printf("    пары с шагом 512 нет: память тонов либо не в этих 32 КБ, либо "
			            "запись в неё не принимается\n");
	}

	// --- 5. как прибор НАЗЫВАЕТ четыре группы тонов --------------------------
	//
	// В записи тембра группа - это число 0..3, и звуковой движок понимает их как свои
	// четыре банка (A, B, Memory, Rhythm). Но подписи в редакторе должны быть теми, что
	// показывает сам прибор, а не одолженными у MT-32, - поэтому они не угадываются, а
	// снимаются с индикатора: группа партии 1 ставится эксклюзивным сообщением, экран
	// читается.
	std::printf("\n=== 5. ИМЕНА ЧЕТЫРЁХ ГРУПП ТОНОВ, снятые с индикатора ===\n");
	{
		press(proc, "Exit", 2);
		press(proc, "Timbre");   // экран, где видно группу и номер тембра партии
		render(proc, 0.6);
		std::printf("  экран после Timbre: \"%s\"\n", screen(proc).c_str());
		for (int group = 0; group < 4; ++group) {
			sendByte(proc, D110Core::kSysexTimbreTemp, 0, uint8_t(group));   // партия 1, группа
			sendByte(proc, D110Core::kSysexTimbreTemp, 1, 0);                // и номер 1
			render(proc, 0.8);
			std::printf("    группа %d -> \"%s\"\n", group, screen(proc).c_str());
		}
	}

	// --- 4. чем прошивка помнит номер текущего патча -------------------------
	std::printf("\n=== 4. НОМЕР ТЕКУЩЕГО ПАТЧА: каким байтом? ===\n");
	{
		press(proc, "Exit", 2);
		press(proc, "Patch");
		render(proc, 0.5);
		std::printf("  экран после Patch: \"%s\"\n", screen(proc).c_str());

		constexpr int kPresses = 3;
		const auto before = snapshot(proc);
		press(proc, "Number+", kPresses);
		render(proc, 0.8);
		const auto after = snapshot(proc);
		std::printf("  экран после Number+ x%d: \"%s\"\n", kPresses, screen(proc).c_str());
		std::vector<int> exact;
		for (int i = 0; i < D110Core::kRamSize; ++i)
			if (int(after[i]) - int(before[i]) == kPresses) exact.push_back(i);
		std::printf("  байтов, сдвинувшихся ровно на %d: %d", kPresses, int(exact.size()));
		for (size_t i = 0; i < exact.size() && i < 10; ++i)
			std::printf("   0x%04X %d->%d", exact[i], before[exact[i]], after[exact[i]]);
		std::printf("\n");

		// Bank+ на D-110 листает патчи восьмёрками - если это так, тот же байт сдвинется
		// на 8, и тогда до любого из 64 патчей не больше восьми нажатий.
		const auto beforeBank = snapshot(proc);
		press(proc, "Bank+", 1);
		render(proc, 0.8);
		const auto afterBank = snapshot(proc);
		std::printf("  экран после Bank+: \"%s\"\n", screen(proc).c_str());
		for (int i : exact)
			std::printf("    0x%04X: %d -> %d (шаг %d)\n", i, beforeBank[i], afterBank[i],
			            int(afterBank[i]) - int(beforeBank[i]));
	}

	// --- 6. что прибор ПИШЕТ НА ЭКРАНЕ про общую подстройку -------------------
	//
	// Байт заводской подстройки - 0x4A = 74, а на экране прибора стоит 442. Документированная
	// Roland шкала 0..127 -> 432.1..457.6 Гц даёт для 74 около 447, то есть расходится с
	// прибором (потому эта величина и не переносится в звуковой движок). Значит шкалу надо не
	// вычислять, а СНЯТЬ: подстройка ставится эксклюзивным сообщением, экран читается.
	std::printf("\n=== 6. ШКАЛА ОБЩЕЙ ПОДСТРОЙКИ, снятая с индикатора ===\n");
	{
		press(proc, "Exit", 2);
		press(proc, "System");
		render(proc, 0.8);
		std::printf("  экран после System: \"%s\"\n", screen(proc).c_str());
		for (int v : { 0, 32, 64, 74, 100, 127 }) {
			sendByte(proc, D110Core::kSysexSystem, 0, uint8_t(v));
			render(proc, 1.0);
			std::printf("    байт %3d -> \"%s\"\n", v, screen(proc).c_str());
		}
		sendByte(proc, D110Core::kSysexSystem, 0, 0x4A);   // вернуть заводское
		render(proc, 0.8);
	}

	// Две ячейки памяти тонов, которые зонд подписал своими именами, возвращаются в исходный
	// вид: заводской сброс их НЕ трогает - это видно по тому, что после сброса они остались
	// подписанными, - а оставлять свои метки в памяти прибора нельзя.
	{
		const uint8_t blank[10] = {};
		sendDt1(proc, D110Core::kSysexTones, 0, blank, 10);
		sendDt1(proc, D110Core::kSysexTones, 2 * 256, blank, 10);
	}

	// --- 7. что за байт прибор называет Output Assign -------------------------
	//
	// У MT-32 шестой байт записи партии - Reverb Switch, и подписи редактора были взяты
	// оттуда. Но на ламинированной карточке D-110 (Play Mode, страница Timbre Edit) стоит
	// не он, а Output Assign - назначение на индивидуальные выходы, которых у MT-32 нет
	// вовсе. Заводские значения не решают спора: байт 6 равен 1 (это и «реверберация
	// включена», и «выход 1»), байт 7 равен 0.
	//
	// Поэтому спрашиваем прибор: доходим до страницы Output Assign и листаем значение,
	// глядя, какой байт двигается и до какого предела он доходит. Предел и решает - у
	// выключателя два положения, у назначения выходов девять.
	std::printf("\n=== 7. OUTPUT ASSIGN: КАКОЙ ЭТО БАЙТ И КАКОВ ЕГО ПРЕДЕЛ ===\n");
	{
		press(proc, "Exit", 2);
		press(proc, "Timbre");
		render(proc, 0.6);
		press(proc, "Edit");
		render(proc, 0.8);
		std::printf("  Timbre Edit: \"%s\"\n", screen(proc).c_str());

		// Страницы листает Group+, и их порядок с карточки: Tone Select, Key Shift, Fine
		// Tune, Bender Range, Assign Mode, Output Assign. Порядок не берётся на веру - экран
		// печатается на каждом шаге.
		for (int page = 1; page <= 5; ++page) {
			press(proc, "Group+");
			render(proc, 0.5);
			std::printf("    Group+ x%d: \"%s\"\n", page, screen(proc).c_str());
		}

		constexpr int kPresses = 3;
		const auto before = snapshot(proc);
		press(proc, "Number+", kPresses);
		render(proc, 0.8);
		const auto after = snapshot(proc);
		std::printf("  после Number+ x%d: \"%s\"\n", kPresses, screen(proc).c_str());
		std::printf("  байт 6 партии 1 (0x%04X): %d -> %d\n",
		            D110Core::kRamTimbreTemp + 6, before[D110Core::kRamTimbreTemp + 6],
		            after[D110Core::kRamTimbreTemp + 6]);
		std::printf("  байт 7 партии 1 (0x%04X): %d -> %d\n",
		            D110Core::kRamTimbreTemp + 7, before[D110Core::kRamTimbreTemp + 7],
		            after[D110Core::kRamTimbreTemp + 7]);
		for (int i = 0; i < D110Core::kRamSize; ++i)
			if (int(after[i]) - int(before[i]) == kPresses && i < 0x2D94)
				std::printf("    сдвиг ровно на %d: 0x%04X  %d -> %d\n", kPresses, i,
				            before[i], after[i]);

		// До упора: сколько всего у этого параметра положений. Двадцать нажатий заведомо
		// больше любого из двух предполагаемых пределов.
		press(proc, "Number+", 20);
		render(proc, 1.0);
		const auto atTop = snapshot(proc);
		std::printf("  на упоре: \"%s\"   байт 6 = %d, байт 7 = %d\n", screen(proc).c_str(),
		            atTop[D110Core::kRamTimbreTemp + 6], atTop[D110Core::kRamTimbreTemp + 7]);
	}

	std::printf("\n=== ИТОГ: %d из %d записей дошли ===\n", passed, total);

	// Зонд писал в настоящую батарейную память прибора, общую с плагином. Убираем за собой.
	std::printf("\nзаводской сброс, чтобы не оставлять свои метки в памяти прибора...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 10.0);
	press(proc, "Exit", 2);
	std::printf("экран после сброса: \"%s\"\n", screen(proc).c_str());

	proc.setPoweredOn(false);
	std::printf("готово\n");
	return 0;
}
