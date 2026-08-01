// Карта регистров LA32 - какая ячейка что означает, снятая с того, что пишет туда прошивка.
//
// Такой карты нет нигде: MB87136APF не эмулирует никто. Сервисные заметки дают только выводы
// (docs/service_notes_findings.md): девять адресных линий A0-A8, то есть 512 регистров,
// восьмибитная шина данных, вход WR, выход прерывания. Окно 0x0C00-0x0DFF имеет ровно такой
// размер, значит это и есть весь управляющий интерфейс синтеза.
//
// Прочитать регистр нельзя, только записать. Поэтому значение выясняется тем, что его
// заставляют измениться: четыре раздражителя, каждый отличается от основного ровно ОДНИМ
// свойством.
//
//   A  нота 60, громкость 100, тембр как есть   - основа
//   B  нота 72                                   - разница с A = высота
//   C  громкость 40                              - разница с A = сила нажатия
//   D  другой тембр                              - разница с A = тембр
//
// ЧТО СЧИТАТЬ ЕДИНИЦЕЙ. Первая редакция этого зонда сравнивала прогоны по абсолютному
// адресу и объявила половину окна «высотой», а половину «громкостью» - артефакт целиком:
// каждая нота получает свои голосовые слоты, и ячейка выглядит изменившейся просто оттого,
// что в другом прогоне её не трогали.
//
// Вторая редакция приводила адрес к «наименьшему использованному смещению в банке», и на
// банке огибающих 0x0CC0 это тоже соврало: он продолжает обслуживать УЖЕ ОТПУЩЕННЫЙ голос
// предыдущей ноты, так что минимум принадлежал не той ноте.
//
// Здесь слот не угадывается вовсе. Прошивка ведёт таблицу состояний слотов (rams 0x2DC0 + 2n,
// см. D110Core::kSlotStateTable): свободный слот держит 0x80. Снимок этой таблицы до и после
// ноты прямо называет слоты, которые ей выделили. Банк идёт через 0x40 = 64 байта на 32
// слота, то есть **два байта на слот**, а нота занимает четыре, потому что берёт два
// партиала - это согласуется с прежним измерением, где на одну ноту ровно два слота
// переходили из 0x80 в 0x40.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;
constexpr int kBytesPerSlot = 2;

struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit", 0, 7}, {"Timbre", 0, 5}, {"Number+", 0, 1}, {"Edit", 1, 7},
	{"Number-", 1, 1},
};

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
}

std::vector<uint8_t> ramOf(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

struct Capture {
	std::vector<D110Core::SoWrite> writes;
	std::vector<int> slots;        // слоты этой ноты, взятые из её же записей
	std::vector<int> slotsByTable; // они же по таблице состояний - только для сверки
	std::map<uint16_t, std::vector<uint8_t>> byAddr;
	uint64_t dropped = 0;
};

// (банк, номер партиала внутри ноты, байт внутри слота) -> значения
using Key = std::tuple<uint16_t, int, int>;

std::map<Key, std::vector<uint8_t>> normalise(const Capture &c) {
	std::map<Key, std::vector<uint8_t>> out;
	for (const auto &[addr, vals] : c.byAddr) {
		const uint16_t bank = addr & 0xFFC0;
		const int within = addr & 0x3F;
		const int slot = within / kBytesPerSlot;
		const int byteInSlot = within % kBytesPerSlot;
		// Записи в слоты, этой ноте не принадлежащие, отбрасываются: банк огибающих
		// параллельно доигрывает предыдущую ноту, и без этого он всё портит.
		const auto it = std::find(c.slots.begin(), c.slots.end(), slot);
		if (it == c.slots.end()) continue;
		out[{bank, int(it - c.slots.begin()), byteInSlot}] = vals;
	}
	return out;
}

Capture window(D110AudioProcessor &proc, int note, int velocity, double seconds) {
	const auto before = ramOf(proc);
	proc.getCore().startSoTrace();
	const uint8_t on[3] = {0x91, uint8_t(note), uint8_t(velocity)}; // канал 2 = партия 1
	proc.getCore().pushMidi(on, 3);
	render(proc, seconds);

	// Снимок таблицы состояний СНЯТ, пока нота ещё звучит: после снятия слоты
	// освобождаются и назвать их было бы уже нечем.
	const auto during = ramOf(proc);

	const uint8_t off[3] = {0x81, uint8_t(note), 0};
	proc.getCore().pushMidi(off, 3);
	render(proc, 0.9);
	proc.getCore().stopSoTrace();

	Capture c;
	c.dropped = proc.getCore().soWritesDropped();
	c.writes = proc.getCore().takeSoWrites();
	for (const auto &w : c.writes) c.byAddr[w.addr].push_back(w.value);

	// Слоты берутся ИЗ САМИХ ЗАПИСЕЙ, по банку 0x0C00. Он пишется только при выдаче голоса,
	// в отличие от банка огибающих 0x0CC0, который параллельно доигрывает прежние ноты, -
	// значит слоты, затронутые в нём за это окно, и есть слоты этой ноты.
	//
	// Прежний признак - «запись в таблице состояний ушла от 0x80» - работал только для
	// слотов, ни разу не использованных, потому что прошивка НИКОГДА не возвращает слот в
	// 0x80: цикл освобождения по ПЗУ 0x29BB таблицу edc0 не трогает (la32_interface.md).
	// Свободных слотов всего 32, четыре раздражителя занимали шестнадцать, и хроматика
	// умирала после четвёртой ноты. Ждать дольше не помогало и помочь не могло.
	for (const auto &[addr, vals] : c.byAddr)
		if ((addr & 0xFFC0) == D110Core::kLa32TapBase)
			c.slots.push_back((addr & 0x3F) / kBytesPerSlot);
	c.slots.erase(std::unique(c.slots.begin(), c.slots.end()), c.slots.end());

	// Таблица состояний остаётся как сверка: пока слоты ещё не кончились, оба способа
	// обязаны называть одно и то же, и расхождение сразу видно.
	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		const int off2 = D110Core::kSlotStateTable + 2 * s;
		if (before[(size_t)off2] == D110Core::kSlotIdleValue &&
		    during[(size_t)off2] != D110Core::kSlotIdleValue)
			c.slotsByTable.push_back(s);
	}
	return c;
}

std::string show(const std::map<Key, std::vector<uint8_t>> &m, const Key &k, int maxShown = 5) {
	const auto it = m.find(k);
	if (it == m.end()) return "-";
	std::string s;
	int shown = 0;
	for (uint8_t v : it->second) {
		if (shown++ == maxShown) { s += " ..."; break; }
		char buf[8];
		std::snprintf(buf, sizeof buf, "%s%02X", s.empty() ? "" : " ", v);
		s += buf;
	}
	return s;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 10.0);
	std::printf("прошивка: %s\n", proc.getCore().isRunning() ? "работает" : "НЕТ");
	if (!proc.getCore().isRunning()) return 1;

	// Захват сужается на окно LA32: по 0x021A параллельно идёт опрос панели тысячами
	// записей в секунду, и без фильтра кольцо забилось бы им одним.
	proc.getCore().setTraceFilter(D110Core::kLa32TapBase, D110Core::kLa32TapEnd);

	// Окно короткое намеренно: пока нота держится, банк 0x0CC0 обновляется без остановки, и
	// на двух секундах кольцо переполнялось, теряя тысячи записей.
	constexpr double kWindow = 0.5;

	std::printf("\n=== КОНТРОЛЬ: окно %.1f с БЕЗ ноты ===\n", kWindow);
	{
		proc.getCore().startSoTrace();
		render(proc, kWindow);
		proc.getCore().stopSoTrace();
		const auto w = proc.getCore().takeSoWrites();
		std::printf("  записей: %zu\n", w.size());
		if (!w.empty())
			std::printf("  !!! окно не молчит - всё ниже надо читать с поправкой на это\n");
	}

	struct Run { const char *label; Capture cap; };
	std::vector<Run> runs;

	std::printf("\n=== A: нота 60, громкость 100, тембр по умолчанию ===\n");
	runs.push_back({"A 60/100", window(proc, 60, 100, kWindow)});

	std::printf("=== B: нота 72, громкость 100 ===\n");
	runs.push_back({"B 72/100", window(proc, 72, 100, kWindow)});

	std::printf("=== C: нота 60, громкость 40 ===\n");
	runs.push_back({"C 60/40 ", window(proc, 60, 40, kWindow)});

	// Четвёртый раздражитель: ДРУГОЙ ТЕМБР при той же ноте и громкости. Дорога снята
	// раньше (plugin/audio_test.cpp): Exit, Exit -> Timbre -> Edit открывается на странице
	// «Tone =», и Number+ выбирает другой звук.
	std::printf("=== D: нота 60, громкость 100, ДРУГОЙ тембр ===\n");
	// Тембр выбирается от УПОРА, а не «плюс семь от того, что было». Память прошивки живёт
	// между прогонами, поэтому относительный выбор уползает: в одном прогоне это был тембр
	// 31, в следующем 38 - а тот берёт уже четыре партиала вместо двух, и сравнивать стало
	// не с чем. От нижнего упора номер один и тот же всегда.
	press(proc, "Exit", 2);
	press(proc, "Timbre");
	press(proc, "Edit");
	press(proc, "Number-", 40);
	press(proc, "Number+", 7);
	render(proc, 1.0);
	{
		const auto ram = ramOf(proc);
		std::printf("  тембр партии 1: группа %d, номер %d (было 0/17 у заводского)\n",
		            ram[0x2000], ram[0x2001]);
	}
	runs.push_back({"D 60/100 другой тембр", window(proc, 60, 100, kWindow)});

	for (const auto &r : runs) {
		std::printf("  %-22s: записей %5zu, потеряно %llu, слоты:", r.label,
		            r.cap.writes.size(), (unsigned long long)r.cap.dropped);
		for (int s : r.cap.slots) std::printf(" %d", s);
		if (r.cap.slots.empty()) std::printf(" (НИ ОДНОГО - слот не определился)");
		std::printf("   | по таблице состояний:");
		for (int s : r.cap.slotsByTable) std::printf(" %d", s);
		if (r.cap.slotsByTable.empty()) std::printf(" (пусто - свободные слоты кончились)");
		std::printf("%s\n", r.cap.slots == r.cap.slotsByTable ? "   СОВПАЛО" : "");
	}

	bool usable = true;
	for (const auto &r : runs)
		if (r.cap.slots.empty() || r.cap.dropped) usable = false;
	if (!usable)
		std::printf("\nНе у всех прогонов определились слоты или захват переполнялся -\n"
		            "таблица сравнения пропускается, чтобы её не приняли за результат.\n"
		            "Хроматика ниже от этого не зависит и всё равно снимается.\n");

	std::vector<std::map<Key, std::vector<uint8_t>>> norm;
	for (const auto &r : runs) norm.push_back(normalise(r.cap));

	std::set<Key> all;
	if (usable)
		for (const auto &m : norm)
			for (const auto &[k, v] : m) all.insert(k);

	if (usable)
	std::printf("\n=== что говорит каждый регистр (банк, партиал ноты, байт в слоте) ===\n");
	std::printf("  банк   пар байт | A              | B (высота)     | C (сила)       "
	            "| D (тембр)      | вывод\n");
	std::map<std::string, int> tally;
	for (const auto &k : all) {
		const bool have0 = norm[0].count(k) != 0;
		std::string verdict;
		if (!have0) verdict = "нет в основном прогоне";
		else {
			const auto &base = norm[0].at(k);
			const bool dPitch = !norm[1].count(k) || norm[1].at(k) != base;
			const bool dVel = !norm[2].count(k) || norm[2].at(k) != base;
			const bool dTone = !norm[3].count(k) || norm[3].at(k) != base;
			if (!dPitch && !dVel && !dTone) verdict = "не меняется ни от чего";
			else {
				if (dPitch) verdict += "ВЫСОТА ";
				if (dVel) verdict += "СИЛА ";
				if (dTone) verdict += "ТЕМБР ";
			}
		}
		tally[verdict]++;
		std::printf("  0x%04X  %d   %d  | %-14s | %-14s | %-14s | %-14s | %s\n",
		            std::get<0>(k), std::get<1>(k), std::get<2>(k),
		            show(norm[0], k).c_str(), show(norm[1], k).c_str(),
		            show(norm[2], k).c_str(), show(norm[3], k).c_str(), verdict.c_str());
	}

	if (usable) {
		std::printf("\n  сводка:\n");
		for (const auto &[v, n] : tally) std::printf("    %-28s %d\n", v.c_str(), n);
		std::printf("\n  Регистр, помеченный ровно одним свойством, назван этим свойством и\n"
		            "  ничем другим - остальные три раздражителя его не сдвинули. Помеченный\n"
		            "  несколькими требует ещё одного опыта, а не толкования.\n");
	}

	// ---- хроматика: как регистр движется НА ПОЛУТОН ------------------------------------
	// Сравнение потоков целиком выше объявило банк 0x0CC0 зависящим «от всего», и это
	// артефакт длины: у прогонов разное число обновлений, поэтому векторы не равны, даже
	// когда их начала совпадают поэлементно. А начала совпадают: у ноты 60 при силе 100 и
	// при силе 40 первые значения одинаковы, а у ноты 72 отличаются ровно на 17.
	//
	// Множеством значений такой вопрос не решается - нужен закон. Тринадцать нот подряд, у
	// каждой берётся ПЕРВОЕ осмысленное значение (ведущее FF пропускается), и шаг виден
	// прямо в столбце.
	std::printf("\n=== хроматика: первое значение каждого регистра, ноты 60..72 ===\n");
	press(proc, "Exit", 2); // вернуться из меню правки тембра

	auto firstReal = [](const std::vector<uint8_t> &v) -> int {
		for (uint8_t x : v)
			if (x != 0xFF) return x;
		return -1;
	};

	std::vector<Key> watch;
	for (uint16_t bank : {0x0C00, 0x0C40, 0x0C80, 0x0CC0, 0x0D00})
		for (int p = 0; p < 2; ++p)
			for (int b = 0; b < kBytesPerSlot; ++b) watch.push_back({bank, p, b});

	std::printf("  нота |");
	for (const auto &k : watch)
		std::printf(" %04X.%d.%d", std::get<0>(k), std::get<1>(k), std::get<2>(k));
	std::printf("\n");

	std::map<Key, std::vector<int>> series;
	for (int note = 60; note <= 72; ++note) {
		// Слот опознаётся по переходу 0x80 -> занят, поэтому перед нотой он обязан успеть
		// освободиться. У тембра из четырёх партиалов на ноту уходит четыре слота, и без
		// этой паузы уже с пятой ноты свободных не оставалось - в прогоне подряд не
		// определились восемь нот из тринадцати.
		render(proc, 1.5);
		const Capture cap = window(proc, note, 100, 0.35);
		const auto n = normalise(cap);
		std::printf("  %4d |", note);
		for (const auto &k : watch) {
			const auto it = n.find(k);
			const int v = (it == n.end()) ? -1 : firstReal(it->second);
			series[k].push_back(v);
			if (v < 0) std::printf("       -");
			else std::printf("      %02X", v);
		}
		std::printf("%s\n", cap.slots.empty() ? "   (слот не определился)" : "");
	}

	std::printf("\n  шаг на полутон (разности подряд идущих нот):\n");
	for (const auto &k : watch) {
		const auto &s = series[k];
		bool anyMissing = false;
		for (int v : s) if (v < 0) anyMissing = true;
		if (anyMissing) continue;
		std::printf("    %04X.%d.%d :", std::get<0>(k), std::get<1>(k), std::get<2>(k));
		int total = 0;
		for (size_t i = 1; i < s.size(); ++i) {
			std::printf(" %+d", s[i] - s[i - 1]);
			total += s[i] - s[i - 1];
		}
		std::printf("   всего за октаву %+d\n", total);
	}
	std::printf("\n  Регистр, у которого сумма за октаву не ноль, следует за клавишей.\n"
	            "  Ровная единица на полутон - полутоновая шкала; 16 или 17 за октаву -\n"
	            "  шкала другая, и какая именно, показывает сам ряд разностей.\n");

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
