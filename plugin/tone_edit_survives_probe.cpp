// Переживает ли ПРАВКА ТЕМБРА смену параметра Timbre - на обычном патче, а не на демо.
//
// Исправление MirrorRegion::reassertAfterTimbreTemp было найдено и измерено на демо-песне,
// где партии 6 и 7 несли группу тембра 5 и звучали закрытым хай-хэтом. Демо - случай
// особенный: группу 5 сама панель выставить не даёт, её туда положило ПЗУ пресетов. Отсюда
// вопрос, на который демо ответить не может: а на обычном патче, который правит человек с
// панели, тембр тоже уцелеет, когда следом сдвинется параметр партии?
//
// Порядок здесь такой же, каким решались предыдущие вопросы этого проекта:
//
//  * КОНТРОЛЬ ПЕРЕД ИЗМЕРЕНИЕМ. Путь чтения из движка доказывается записью известного
//    значения и чтением его обратно. Пока это не прошло, ни одно показание движка не
//    значит ничего.
//  * КОНТРОЛЬ, УМЕЮЩИЙ ПОКАЗАТЬ ОТКАЗ. Опыт идёт ДВАЖДЫ в одном прогоне - с
//    подтверждением тембров и без него (D110Core::setToneReassert). Проверка, которая
//    умеет напечатать только "уцелел", выглядит одинаково и когда исправление работает, и
//    когда затирать было нечему. Настоящий ответ дают два прогона рядом.
//  * НИЧЕГО НЕ ПРЕДПОЛАГАТЬ О МЕНЮ. Страницы правки ищутся нажатиями: жмём значение и
//    смотрим, какие байты ОЗУ сдвинулись ровно на число нажатий. Страница, сдвинувшая
//    байт внутри окна тембра, и есть правка тембра; страница, сдвинувшая байт внутри
//    Timbre Temporary, - правка параметра партии.
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

// Roland адресует три семибитных байта, движок - одно упакованное 21-битное число.
constexpr uint32_t packed(uint32_t a) {
	return ((a & 0x7f0000u) >> 2) | ((a & 0x7f00u) >> 1) | (a & 0x7fu);
}
constexpr uint32_t kToneTempSysex = 0x040000;
constexpr uint16_t kTimbreTempRam = 0x2000; // 9 партий по 16 байт
constexpr int kTimbreTempLen = 9 * 16;
constexpr uint16_t kToneTempRam = 0x21E4;
constexpr int kToneStride = 246;
constexpr int kNumToneParts = 8;

struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit", 0, 7}, {"Patch", 0, 6}, {"Timbre", 0, 5}, {"Part+", 0, 4},
	{"Group+", 0, 3}, {"Bank+", 0, 2}, {"Number+", 0, 1}, {"Write", 0, 0},
	{"Edit", 1, 7}, {"Part", 1, 6}, {"System", 1, 5}, {"Part-", 1, 4},
	{"Group-", 1, 3}, {"Bank-", 1, 2}, {"Number-", 1, 1}, {"Enter", 1, 0},
};

// Одно нажатие пути. Разведка находит путь, опыт проигрывает его заново после заводского
// сброса - поэтому путь и хранится, а не описывается словами в комментарии.
struct Step { const char *btn; int times; };
using Path = std::vector<Step>;

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

// СЧИТАЕМ звук, а не спим. Кольцо эксклюзивов разбирает только processBlock, и проспать
// его - значит оставить весь затор в очереди, а потом получить его залпом посреди
// измерения. Так уже была молча затёрта собственная контрольная запись одного из зондов.
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

void walk(D110AudioProcessor &proc, const Path &path) {
	for (const auto &s : path) press(proc, s.btn, s.times);
}

std::string pathText(const Path &path) {
	std::string s;
	for (const auto &st : path) {
		if (!s.empty()) s += " -> ";
		s += st.btn;
		if (st.times != 1) s += " x" + std::to_string(st.times);
	}
	return s;
}

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

// Какой партии принадлежит этот адрес тембра, или -1.
int toneOwner(int addr) {
	if (addr < kToneTempRam) return -1;
	const int part = (addr - kToneTempRam) / kToneStride;
	return part < kNumToneParts ? part : -1;
}

// Байты 0 и 1 записи Timbre Temporary - это ГРУППА и НОМЕР тембра, то есть выбор другого
// звука. Их сдвиг тембр меняет по праву, и опытом на затирание он быть не может.
bool isTimbreSelect(int addr) {
	if (addr < kTimbreTempRam || addr >= kTimbreTempRam + kTimbreTempLen) return false;
	const int off = (addr - kTimbreTempRam) % 16;
	return off == 0 || off == 1;
}

struct Hit { bool found = false; int addr = 0; int part = -1; };

// Сдвинулась ли ГРУППА или НОМЕР тембра у какой-нибудь партии - на любую величину. Если
// да, страница выбирает другой звук, и тембр она переписывает по праву: опытом на затирание
// такая страница быть не может.
bool selectedAnotherTone(const std::vector<uint8_t> &before, const std::vector<uint8_t> &after) {
	for (int i = kTimbreTempRam; i < kTimbreTempRam + kTimbreTempLen; ++i)
		if (isTimbreSelect(i) && before[i] != after[i]) return true;
	return false;
}

// Байт, сдвинувшийся РОВНО на число нажатий, - подпись правимого параметра. Экранный буфер
// прошивки меняется вместе с ним, поэтому совпадение по величине сдвига и берётся: оно
// отделяет параметр от сопутствующего шума.
Hit findShift(const std::vector<uint8_t> &before, const std::vector<uint8_t> &after,
              int presses, bool wantTone) {
	Hit hit;
	int shown = 0;
	for (int i = 0; i < D110Core::kRamSize; ++i) {
		if (before[i] == after[i]) continue;
		const int d = int(after[i]) - int(before[i]);
		if (d != presses) continue;
		const int part = toneOwner(i);
		const bool inTone = part >= 0;
		const bool inTimbre = i >= kTimbreTempRam && i < kTimbreTempRam + kTimbreTempLen;
		if (shown++ < 6)
			std::printf("      0x%04X %d->%d  (%s)\n", i, before[i], after[i],
			            inTone ? "тембр" : inTimbre ? "Timbre Temporary" : "прочее");
		if (hit.found) continue;
		if (wantTone && inTone) { hit = {true, i, part}; }
		if (!wantTone && inTimbre && !isTimbreSelect(i)) { hit = {true, i, (i - kTimbreTempRam) / 16}; }
	}
	if (!shown) std::printf("      (ни один байт не сдвинулся ровно на %d)\n", presses);
	return hit;
}

void factoryReset(D110AudioProcessor &proc) {
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 9.0);
	press(proc, "Exit", 2);
}

// Сравнение тембра целиком, а не по имени: имя однозначно называет ЧУЖОЙ звук, но правка
// одного параметра имени не меняет, и по имени такая потеря невидима. Здесь важно как раз
// то, что теряется тихо.
int compareTone(D110AudioProcessor &proc, const std::vector<uint8_t> &ram, int part,
                const char *label) {
	const uint8_t *fw = &ram[kToneTempRam + kToneStride * part];
	uint8_t eng[kToneStride];
	std::memset(eng, 0xAA, sizeof eng);
	proc.engineReadMemory(packed(kToneTempSysex) + uint32_t(kToneStride) * uint32_t(part),
	                      kToneStride, eng);
	int diff = 0, firstDiff = -1;
	for (int i = 0; i < kToneStride; ++i)
		if (fw[i] != eng[i]) { if (!diff) firstDiff = i; ++diff; }

	std::string fwName, engName;
	for (int i = 0; i < 10; ++i) {
		fwName.push_back((fw[i] >= 0x20 && fw[i] < 0x7f) ? char(fw[i]) : '.');
		engName.push_back((eng[i] >= 0x20 && eng[i] < 0x7f) ? char(eng[i]) : '.');
	}
	std::printf("    %-22s прошивка \"%s\"  движок \"%s\"  расходятся %d из %d байт",
	            label, fwName.c_str(), engName.c_str(), diff, kToneStride);
	if (diff) std::printf("  (первое расхождение байт %d: %d против %d)", firstDiff,
	                      fw[firstDiff], eng[firstDiff]);
	std::printf("\n");
	return diff;
}

// Один опыт целиком: заводской сброс, правка тембра, потом смена параметра партии, и
// сравнение тембра прошивки с тембром движка ДО и ПОСЛЕ этой смены.
bool runExperiment(D110AudioProcessor &proc, bool reassert, const Path &tonePath,
                   const Path &timbrePath, int tonePart) {
	std::printf("\n=================================================================\n");
	std::printf("ОПЫТ: подтверждение тембров после Timbre Temporary %s\n",
	            reassert ? "ВКЛЮЧЕНО (как в плагине)" : "ВЫКЛЮЧЕНО (контроль)");
	std::printf("=================================================================\n");
	proc.getCore().setToneReassert(reassert);

	std::printf("  заводской сброс...\n");
	factoryReset(proc);

	// --- правка тембра ---
	walk(proc, tonePath);
	std::printf("  страница тембра: \"%s\"\n", screen(proc).c_str());
	proc.getCore().resetTallies();
	const auto ramVirgin = snapshot(proc);
	press(proc, "Number+", 5);
	render(proc, 1.0);
	auto ram = snapshot(proc);
	std::printf("  после правки, экран \"%s\"\n", screen(proc).c_str());
	{
		// Параметр на упоре не сдвинется, и тогда затирать будет НЕЧЕГО: и до, и после
		// сравнение сойдётся, а опыт при этом не проверит ничего. Такой прогон обязан
		// сказать это о себе сам.
		const int base = kToneTempRam + kToneStride * tonePart;
		int moved = 0;
		for (int i = 0; i < kToneStride; ++i)
			if (ramVirgin[base + i] != ram[base + i]) ++moved;
		std::printf("    в тембре партии %d сдвинулось байт: %d%s\n", tonePart + 1, moved,
		            moved ? "" : "   <-- ТЕМБР НЕ ИЗМЕНИЛСЯ, опыту нечего терять");
	}
	const int diffBefore = compareTone(proc, ram, tonePart, "до смены параметра:");

	// --- смена параметра партии ---
	press(proc, "Exit", 2);
	walk(proc, timbrePath);
	std::printf("  страница параметра партии: \"%s\"\n", screen(proc).c_str());
	const auto ramBefore = snapshot(proc);
	press(proc, "Number+", 3);
	render(proc, 1.0);
	ram = snapshot(proc);
	std::printf("  после смены, экран \"%s\"\n", screen(proc).c_str());
	{
		int moved = 0;
		for (int i = kTimbreTempRam; i < kTimbreTempRam + kTimbreTempLen; ++i)
			if (ramBefore[i] != ram[i]) ++moved;
		std::printf("    в Timbre Temporary сдвинулось байт: %d%s\n", moved,
		            moved ? "" : "   <-- ПАРАМЕТР НЕ ПОМЕНЯЛСЯ, опыт ничего не проверил");
	}
	const int diffAfter = compareTone(proc, ram, tonePart, "после смены параметра:");

	std::printf("    отправлено DT1: Timbre Temporary %llu, тембр партии %d %llu, "
	            "потеряно %llu\n",
	            (unsigned long long)proc.getCore().regionEmitCount(0),
	            tonePart + 1,
	            (unsigned long long)proc.getCore().regionEmitCount(4 + tonePart),
	            (unsigned long long)proc.getCore().sysexDropped());
	std::printf("  ИТОГ: тембр %s (расхождение %d -> %d байт)\n",
	            diffAfter == 0 ? "УЦЕЛЕЛ" : "ЗАТЁРТ", diffBefore, diffAfter);
	return diffAfter == 0;
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
	std::printf("прошивка: %s   движок: %s   знакогенератор: %s\n",
	            proc.getCore().isRunning() ? "работает" : "НЕТ",
	            proc.engineIsOpen() ? "открыт" : "НЕ ОТКРЫТ",
	            g_cgrom.empty() ? "НЕ НАЙДЕН" : "загружен");
	if (!proc.engineIsOpen()) {
		std::printf("Движок не открылся - любое показание ниже было бы буфером самого\n"
		            "зонда. Останавливаемся.\n");
		return 1;
	}

	// ---- КОНТРОЛЬ: работает ли путь чтения тембра из движка --------------------------
	// Записали в движок известные десять байт имени тембра партии 1 и прочитали обратно.
	// Пока это не прошло, "тембр затёрт" и "читатель сломан" неотличимы.
	{
		static const char kProbeName[10] = {'C','O','N','T','R','O','L','.','.','.'};
		uint8_t msg[32];
		int n = 0;
		msg[n++] = 0xF0; msg[n++] = 0x41; msg[n++] = 0x10; msg[n++] = 0x16; msg[n++] = 0x12;
		const uint32_t addr = kToneTempSysex;
		const uint8_t a1 = uint8_t((addr >> 16) & 0x7f), a2 = uint8_t((addr >> 8) & 0x7f),
		              a3 = uint8_t(addr & 0x7f);
		msg[n++] = a1; msg[n++] = a2; msg[n++] = a3;
		uint32_t sum = a1 + a2 + a3;
		for (char c : kProbeName) { msg[n++] = uint8_t(c); sum += uint8_t(c); }
		msg[n++] = uint8_t((128 - (sum & 0x7f)) & 0x7f);
		msg[n++] = 0xF7;
		proc.engineWriteSysexForTest(msg, n);
		render(proc, 0.4); // очередь эксклюзивов разбирается во время расчёта звука

		uint8_t got[10];
		std::memset(got, 0xAA, sizeof got);
		proc.engineReadMemory(packed(kToneTempSysex), 10, got);
		const bool ok = std::memcmp(kProbeName, got, 10) == 0;
		std::printf("\nКОНТРОЛЬ пути чтения: записали \"CONTROL...\", прочитали \"");
		for (int i = 0; i < 10; ++i)
			std::printf("%c", (got[i] >= 0x20 && got[i] < 0x7f) ? char(got[i]) : '.');
		std::printf("\"  => %s\n", ok ? "РАБОТАЕТ" : "СЛОМАН - дальше идти незачем");
		if (!ok) return 1;
		proc.getCore().resyncMirror(); // вернуть состояние прошивки на место
		render(proc, 1.0);
	}

	// ---- РАЗВЕДКА: где на панели правится тембр, а где параметр партии ----------------
	// Меню не описывается по памяти: страницы ищутся тем, что после нажатий сдвигается в
	// ОЗУ. Байт, сдвинувшийся ровно на число нажатий, называет параметр сам.
	constexpr int kProbePresses = 3;
	Path tonePath, timbrePath;
	int tonePart = -1;

	// Дорога до правок снята измерением раньше и записана в plugin/audio_test.cpp:
	// Exit, Exit -> Timbre -> Edit открывает ПРАВКУ ПАРАМЕТРОВ ПАРТИИ (её первая страница
	// "Tone =" выбирает звук), а ещё одно Edit с этой страницы проваливается в ПРАВКУ
	// ТЕМБРА. Внутри обеих параметры листает Group+, значение меняет Number+.
	const Path kToneEditRoot = {{"Exit", 2}, {"Timbre", 1}, {"Edit", 1}, {"Edit", 1}};
	const Path kTimbreEditRoot = {{"Exit", 2}, {"Timbre", 1}, {"Edit", 1}};

	std::printf("\n=== разведка: страницы правки тембра (Timbre -> Edit -> Edit) ===\n");
	walk(proc, kToneEditRoot);
	std::printf("  правка тембра открылась на: \"%s\"\n", screen(proc).c_str());
	for (int page = 0; page < 8 && tonePart < 0; ++page) {
		const auto before = snapshot(proc);
		press(proc, "Number+", kProbePresses);
		render(proc, 0.5);
		const auto after = snapshot(proc);
		std::printf("  group+ x%d, экран \"%s\"\n", page, screen(proc).c_str());
		if (selectedAnotherTone(before, after)) {
			std::printf("      (страница выбирает другой звук - тембр она меняет по праву)\n");
			press(proc, "Group+");
			continue;
		}
		const Hit hit = findShift(before, after, kProbePresses, true);
		if (hit.found) {
			tonePart = hit.part;
			tonePath = kToneEditRoot;
			if (page) tonePath.push_back({"Group+", page});
			std::printf("    => правка тембра партии %d, байт 0x%04X\n", tonePart + 1, hit.addr);
			break;
		}
		press(proc, "Group+");
	}

	std::printf("\n=== разведка: страницы параметра партии (Timbre -> Edit) ===\n");
	walk(proc, kTimbreEditRoot);
	std::printf("  правка параметров открылась на: \"%s\"\n", screen(proc).c_str());
	bool timbreFound = false;
	for (int page = 0; page < 8 && !timbreFound; ++page) {
		const auto before = snapshot(proc);
		press(proc, "Number+", kProbePresses);
		render(proc, 0.5);
		const auto after = snapshot(proc);
		std::printf("  group+ x%d, экран \"%s\"\n", page, screen(proc).c_str());
		const Hit hit = findShift(before, after, kProbePresses, false);
		if (hit.found) {
			timbreFound = true;
			timbrePath = kTimbreEditRoot;
			if (page) timbrePath.push_back({"Group+", page});
			std::printf("    => параметр партии %d, байт 0x%04X\n", hit.part + 1, hit.addr);
			break;
		}
		press(proc, "Group+");
	}

	if (tonePart < 0 || !timbreFound) {
		std::printf("\nРазведка не нашла %s%s%s. Опыт не ставится: без этого он проверял бы\n"
		            "нажатия, которые ничего не правят. Экраны выше показывают, куда попали.\n",
		            tonePart < 0 ? "страницу правки тембра" : "",
		            (tonePart < 0 && !timbreFound) ? " и " : "",
		            !timbreFound ? "страницу параметра партии" : "");
		proc.setPoweredOn(false);
		proc.releaseResources();
		return 1;
	}
	std::printf("\nпуть к тембру          : %s\n", pathText(tonePath).c_str());
	std::printf("путь к параметру партии: %s\n", pathText(timbrePath).c_str());

	// ---- ОПЫТ, дважды: с подтверждением тембров и без него ----------------------------
	const bool withOff = runExperiment(proc, false, tonePath, timbrePath, tonePart);
	const bool withOn = runExperiment(proc, true, tonePath, timbrePath, tonePart);
	proc.getCore().setToneReassert(true);

	std::printf("\n=== ответ ===\n");
	std::printf("  без подтверждения тембров: правка %s\n", withOff ? "уцелела" : "затёрта");
	std::printf("  с подтверждением тембров : правка %s\n", withOn ? "уцелела" : "затёрта");
	if (withOn && !withOff)
		std::printf("  => исправление работает и на обычном патче, и контроль это доказывает:\n"
		            "     без него та же правка теряется.\n");
	else if (withOn && withOff)
		std::printf("  => правка уцелела в ОБОИХ случаях. Значит на этом патче затирать было\n"
		            "     нечему, и прогон ничего не доказывает - нужен другой параметр.\n");
	else
		std::printf("  => правка НЕ уцелела с включённым подтверждением. Это отказ.\n");

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return withOn ? 0 : 1;
}
