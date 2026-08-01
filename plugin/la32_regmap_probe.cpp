// Карта регистров LA32 - какая её ячейка что означает, снятая с того, что пишет туда сама
// прошивка.
//
// Такой карты нет нигде: MB87136APF не эмулирует ни MAME, ни кто-либо ещё, а сервисные
// заметки дают только выводы микросхемы (docs/service_notes_findings.md) - девять адресных
// линий A0-A8, то есть 512 регистров, восьмибитная шина данных, вход WR и выход прерывания.
// Окно 0x0C00-0x0DFF, найденное раньше по неотображённым обращениям, имеет ровно такой
// размер, так что это и есть весь управляющий интерфейс синтеза.
//
// Значение ячейки нельзя прочитать - можно только заставить его измениться и посмотреть, что
// сдвинулось. Поэтому здесь берутся ТРИ раздражителя, отличающиеся ровно одним свойством:
//
//   A  нота 60, громкость 100     - основа
//   B  нота 72, громкость 100     - та же нота октавой выше: разница = высота
//   C  нота 60, громкость  40     - та же высота, другая сила: разница = громкость
//
// Ячейка, различающаяся между A и B, но одинаковая в A и C, несёт высоту, и наоборот. Одного
// прогона с одной нотой на такой вопрос не хватает: там всё выглядит одинаково значимым.
//
// КОНТРОЛЬ. Перед всеми тремя снимается окно ТОЙ ЖЕ длины вообще без ноты. Если в нём тоже
// идут записи, то «эта ячейка меняется» ничего не значит: прошивка пишет в микросхему и в
// покое (гашение голосов, обслуживание), и такие адреса надо исключить, а не толковать.
#include "Source/PluginProcessor.h"

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

// Ноты подаются в ПРОШИВКУ, а не прямо в движок: регистры LA32 заполняет её собственное
// распределение голосов, и нота, отданная мимо неё, до микросхемы не дошла бы вовсе.
struct Capture {
	std::vector<D110Core::SoWrite> writes;
	std::map<uint16_t, std::vector<uint8_t>> byAddr; // адрес -> значения по порядку
	uint64_t dropped = 0;
};

Capture window(D110AudioProcessor &proc, int note, int velocity, double seconds) {
	proc.getCore().startSoTrace();
	if (note >= 0) {
		const uint8_t on[3] = {0x91, uint8_t(note), uint8_t(velocity)}; // канал 2 = партия 1
		proc.getCore().pushMidi(on, 3);
	}
	render(proc, seconds);
	if (note >= 0) {
		const uint8_t off[3] = {0x81, uint8_t(note), 0};
		proc.getCore().pushMidi(off, 3);
		render(proc, 0.8);
	}
	proc.getCore().stopSoTrace();

	Capture c;
	c.dropped = proc.getCore().soWritesDropped();
	c.writes = proc.getCore().takeSoWrites();
	for (const auto &w : c.writes) c.byAddr[w.addr].push_back(w.value);
	return c;
}

// Каждая нота получает СВОЙ голосовой слот, и прошивка пишет регистры именно его. Поэтому
// сравнивать прогоны по абсолютному адресу нельзя: у одной ноты это 0x0C00-0x0C03, у
// следующей 0x0C04-0x0C07, и любая ячейка окажется «изменившейся» просто потому, что в
// другом прогоне её не трогали. Первая редакция этого зонда так и сделала и уверенно
// назвала половину окна «высотой», а половину «громкостью».
//
// Приведение: адрес = банк (шаг 0x40) + номер внутри банка. Слот прогона - наименьший
// использованный номер внутри банка; от него и отсчитывается смещение.
constexpr uint16_t bankOf(uint16_t addr) { return addr & 0xFFC0; }
constexpr uint16_t indexOf(uint16_t addr) { return addr & 0x3F; }

// (банк, смещение внутри слота) -> значения
std::map<std::pair<uint16_t, int>, std::vector<uint8_t>> normalise(const Capture &c) {
	std::map<uint16_t, int> slotBase;
	for (const auto &[addr, vals] : c.byAddr) {
		const uint16_t bank = bankOf(addr);
		const int idx = indexOf(addr);
		auto it = slotBase.find(bank);
		if (it == slotBase.end() || idx < it->second) slotBase[bank] = idx;
	}
	std::map<std::pair<uint16_t, int>, std::vector<uint8_t>> out;
	for (const auto &[addr, vals] : c.byAddr)
		out[{bankOf(addr), indexOf(addr) - slotBase[bankOf(addr)]}] = vals;
	return out;
}

std::string valuesOfNorm(const std::map<std::pair<uint16_t, int>, std::vector<uint8_t>> &m,
                         uint16_t bank, int off, int maxShown = 5) {
	const auto it = m.find({bank, off});
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

std::string valuesOf(const Capture &c, uint16_t addr, int maxShown = 6) {
	const auto it = c.byAddr.find(addr);
	if (it == c.byAddr.end()) return "-";
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

bool sameValues(const Capture &a, const Capture &b, uint16_t addr) {
	const auto ia = a.byAddr.find(addr), ib = b.byAddr.find(addr);
	if (ia == a.byAddr.end() && ib == b.byAddr.end()) return true;
	if (ia == a.byAddr.end() || ib == b.byAddr.end()) return false;
	return ia->second == ib->second;
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

	// Окно короткое намеренно. Пока нота держится, прошивка обновляет её огибающую из ПЗУ
	// 0x2C0D раз за разом, и на двух секундах кольцо захвата переполнялось, теряя тысячи
	// записей - а потерянные записи делают любую таблицу ниже нижней границей. Полсекунды
	// хватает, чтобы голос был выделен и настроен целиком.
	constexpr double kWindow = 0.5;

	std::printf("\n=== КОНТРОЛЬ: окно %.1f с БЕЗ ноты ===\n", kWindow);
	const Capture quiet = window(proc, -1, 0, kWindow);
	std::printf("  записей: %zu по %zu адресам, потеряно %llu\n", quiet.writes.size(),
	            quiet.byAddr.size(), (unsigned long long)quiet.dropped);
	if (!quiet.byAddr.empty()) {
		std::printf("  адреса, которые пишутся и в покое (их показания ничего не докажут):\n   ");
		int n = 0;
		for (const auto &[addr, vals] : quiet.byAddr) {
			std::printf(" %04X", addr);
			if (++n % 16 == 0) std::printf("\n   ");
		}
		std::printf("\n");
	}

	std::printf("\n=== A: нота 60, громкость 100 ===\n");
	const Capture a = window(proc, 60, 100, kWindow);
	std::printf("  записей: %zu по %zu адресам, потеряно %llu\n", a.writes.size(),
	            a.byAddr.size(), (unsigned long long)a.dropped);

	std::printf("\n=== B: нота 72 (октавой выше), громкость 100 ===\n");
	const Capture b = window(proc, 72, 100, kWindow);
	std::printf("  записей: %zu по %zu адресам, потеряно %llu\n", b.writes.size(),
	            b.byAddr.size(), (unsigned long long)b.dropped);

	std::printf("\n=== C: нота 60, громкость 40 ===\n");
	const Capture c = window(proc, 60, 40, kWindow);
	std::printf("  записей: %zu по %zu адресам, потеряно %llu\n", c.writes.size(),
	            c.byAddr.size(), (unsigned long long)c.dropped);

	if (a.dropped || b.dropped || c.dropped)
		std::printf("\n!!! захват переполнялся - таблица ниже неполна\n");

	// ---- разбор -------------------------------------------------------------------------
	const auto na = normalise(a), nb = normalise(b), nc = normalise(c);

	// Какие абсолютные адреса заняла каждая нота - это и есть доказательство, что слоты
	// разные, и без него нормализация выглядела бы подгонкой.
	std::printf("\n=== какой слот получила каждая нота ===\n");
	for (const auto &[label, cap] : {std::pair{"A 60/100", &a}, {"B 72/100", &b},
	                                 {"C 60/40 ", &c}}) {
		std::printf("  %s :", label);
		std::map<uint16_t, std::pair<int, int>> span; // банк -> мин/макс номер
		for (const auto &[addr, vals] : cap->byAddr) {
			auto &s = span[bankOf(addr)];
			if (!s.second) { s.first = indexOf(addr); s.second = indexOf(addr); }
			s.first = std::min(s.first, int(indexOf(addr)));
			s.second = std::max(s.second, int(indexOf(addr)));
		}
		for (const auto &[bank, s] : span)
			std::printf("  0x%04X+%d..%d", bank, s.first, s.second);
		std::printf("\n");
	}

	std::set<std::pair<uint16_t, int>> all;
	for (const auto *m : {&na, &nb, &nc})
		for (const auto &[key, vals] : *m) all.insert(key);

	std::printf("\n=== что говорит каждый регистр голоса (адреса приведены к слоту) ===\n");
	std::printf("  банк   смещ | A: 60/100      | B: 72/100      | C: 60/40       | вывод\n");
	int pitchOnly = 0, velOnly = 0, both = 0, neither = 0;
	for (const auto &key : all) {
		const auto ia = na.find(key), ib = nb.find(key), ic = nc.find(key);
		const bool haveA = ia != na.end(), haveB = ib != nb.end(), haveC = ic != nc.end();
		const bool pitchMoves = !haveA || !haveB || ia->second != ib->second;
		const bool velMoves = !haveA || !haveC || ia->second != ic->second;
		const char *verdict;
		if (!haveA || !haveB || !haveC) verdict = "есть не во всех прогонах - не толковать";
		else if (pitchMoves && !velMoves) { verdict = "<== ВЫСОТА"; ++pitchOnly; }
		else if (velMoves && !pitchMoves) { verdict = "<== ГРОМКОСТЬ"; ++velOnly; }
		else if (pitchMoves && velMoves) { verdict = "и высота, и громкость"; ++both; }
		else { verdict = "одинаково во всех трёх"; ++neither; }
		std::printf("  0x%04X +%-3d | %-14s | %-14s | %-14s | %s\n", key.first, key.second,
		            valuesOfNorm(na, key.first, key.second).c_str(),
		            valuesOfNorm(nb, key.first, key.second).c_str(),
		            valuesOfNorm(nc, key.first, key.second).c_str(), verdict);
	}

	std::printf("\n  итог: только высота %d, только громкость %d, и то и другое %d,\n"
	            "        одинаково всегда %d\n", pitchOnly, velOnly, both, neither);
	std::printf("\n  Ячейка «одинаково во всех трёх» - это не «ничего не значит»: там могут\n"
	            "  лежать волновая форма, огибающие или структура тембра, которые у одного и\n"
	            "  того же тембра одинаковы по построению. Чтобы разделить их, нужен четвёртый\n"
	            "  раздражитель - ДРУГОЙ тембр, - и это следующий шаг, а не вывод этого.\n");

	// Порядок записей на одну ноту: он говорит, как устроен цикл запуска голоса, чего
	// таблица значений сказать не может.
	std::printf("\n=== порядок первых 40 записей при ноте 60/100 ===\n");
	for (size_t i = 0; i < a.writes.size() && i < 40; ++i)
		std::printf("  %2zu  ПЗУ %04X  ->  0x%04X = %02X\n", i, a.writes[i].pc,
		            a.writes[i].addr, a.writes[i].value);

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
