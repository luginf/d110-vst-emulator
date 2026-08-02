// Что банк 0x0CC0 несёт ВО ВРЕМЕНИ, пока нота звучит.
//
// Карта регистров (docs/la32_register_map.md) разобрала всё, что пишется В МОМЕНТ выдачи
// голоса: ширину импульса, срез, резонанс, форму волны, выбор волны ПЗУ, высоту. Осталась
// та часть интерфейса, которая работает не разово, а непрерывно: банк 0x0CC0 переписывается
// из ПЗУ 0x2C0D всё время, пока нота держится.
//
// Вопрос не праздный, он решает форму всей эмуляции. Модель микросхемы в munt
// (LA32WaveGenerator) требует на КАЖДЫЙ отсчёт три величины - амплитуду, высоту и срез, -
// а в потоке на слот приходится два байта. Значит либо поток мультиплексирован, либо
// амплитуда и срез идут своими путями, которые прежний захват не разделил.
//
// Прежние заходы на этот банк проваливались ТРИЖДЫ, и каждый раз по одной и той же причине:
// потоки сравнивались как МНОЖЕСТВА значений. У прогонов разное число обновлений, поэтому
// векторы не равны, даже когда их начала совпадают поэлементно, - и банк объявлялся
// «зависящим от всего». Здесь поток не сравнивается ни с чем: он ПЕЧАТАЕТСЯ как ряд во
// времени, вместе с адресом подпрограммы, сделавшей запись.
//
// Режимы:
//   observe            одна нота, полный разбор всего окна 0x0C00-0x0DFF по времени
//   env <группа> <шаг> та же нота после правки одного параметра огибающей - для сравнения
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
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
	{"Exit", 0, 7}, {"Patch", 0, 6}, {"Timbre", 0, 5}, {"Part+", 0, 4},
	{"Group+", 0, 3}, {"Bank+", 0, 2}, {"Number+", 0, 1}, {"Write", 0, 0},
	{"Edit", 1, 7}, {"Part", 1, 6}, {"System", 1, 5}, {"Part-", 1, 4},
	{"Group-", 1, 3}, {"Bank-", 1, 2}, {"Number-", 1, 1}, {"Enter", 1, 0},
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
	std::printf("  !!! нет такой кнопки: %s\n", name);
}

std::vector<uint8_t> ramOf(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

struct Run {
	std::vector<D110Core::SoWrite> writes;
	std::vector<int> slots;
	uint64_t dropped = 0;
	double heldMs = 0, releasedMs = 0;
	// Сколько раз за ноту прошивка получила ответ от микросхемы. У настоящей LA32 вывод INT
	// поднимает ЗАВЕРШЕНИЕ РАМПЫ (munt: LA32Ramp::checkInterrupt), то есть этих ответов
	// должно быть примерно столько же, сколько ступеней у огибающих. Если их тысячи или
	// ноль - огибающие прошивки идут не по тому пути, что на железе, и «регистр не
	// сдвинулся» может означать именно это, а не отсутствие параметра.
	uint64_t servicesBefore = 0, servicesAfter = 0;
};

// Нота держится долго НАМЕРЕННО. Огибающая TVA у большинства тембров за полсекунды не
// доходит и до фазы поддержки, а различить «поток несёт огибающую» и «поток несёт высоту»
// можно только там, где огибающая заведомо движется, - то есть на атаке и на затухании.
Run playOne(D110AudioProcessor &proc, int note, int velocity, double hold, double tail) {
	Run r;
	r.servicesBefore = proc.getCore().la32Services();
	proc.getCore().startSoTrace();
	const auto t0 = Clock::now();
	const uint8_t on[3] = {0x91, uint8_t(note), uint8_t(velocity)};
	proc.getCore().pushMidi(on, 3);
	render(proc, hold);
	r.heldMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

	const uint8_t off[3] = {0x81, uint8_t(note), 0};
	proc.getCore().pushMidi(off, 3);
	render(proc, tail);
	r.releasedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
	proc.getCore().stopSoTrace();

	r.servicesAfter = proc.getCore().la32Services();
	r.dropped = proc.getCore().soWritesDropped();
	r.writes = proc.getCore().takeSoWrites();

	// Слоты этой ноты - те, что затронуты в банке выдачи 0x0C00. Банк огибающих для этого не
	// годится: он параллельно доигрывает предыдущие ноты.
	for (const auto &w : r.writes)
		if ((w.addr & 0xFFC0) == D110Core::kLa32TapBase) {
			const int slot = (w.addr & 0x3F) / kBytesPerSlot;
			if (std::find(r.slots.begin(), r.slots.end(), slot) == r.slots.end())
				r.slots.push_back(slot);
		}
	std::sort(r.slots.begin(), r.slots.end());
	return r;
}

const char *bankName(uint16_t bank) {
	switch (bank) {
	case 0x0C00: return "0C00";
	case 0x0C40: return "0C40";
	case 0x0C80: return "0C80";
	case 0x0CC0: return "0CC0";
	case 0x0D00: return "0D00";
	default: return "????";
	}
}

// Сколько раз за ноту переписан каждый банк, и кто его пишет. Это и есть ответ на вопрос
// «что настраивается разово, а что течёт»: разовая настройка даёт единицы записей, поток -
// тысячи.
void reportBanks(const Run &r) {
	std::map<uint16_t, size_t> perBank;
	std::map<uint16_t, std::map<uint16_t, size_t>> pcPerBank;
	for (const auto &w : r.writes) {
		const uint16_t bank = w.addr & 0xFFC0;
		++perBank[bank];
		++pcPerBank[bank][w.pc];
	}
	std::printf("\n  банк | записей | подпрограммы, которые в него пишут\n");
	for (const auto &[bank, n] : perBank) {
		std::printf("  %s | %7zu |", bankName(bank), n);
		std::vector<std::pair<size_t, uint16_t>> pcs;
		for (const auto &[pc, cnt] : pcPerBank[bank]) pcs.push_back({cnt, pc});
		std::sort(pcs.rbegin(), pcs.rend());
		for (size_t i = 0; i < pcs.size() && i < 4; ++i)
			std::printf(" ПЗУ %04X x%zu", pcs[i].second, pcs[i].first);
		std::printf("\n");
	}
}

// Ряд значений во времени для одного адреса. Печатаются только МОМЕНТЫ ИЗМЕНЕНИЯ: поток,
// переписывающий одно и то же значение тысячу раз, и поток, ведущий огибающую, по числу
// записей неотличимы, а по числу РАЗНЫХ значений - совершенно.
void reportSeries(const Run &r, uint16_t addr, double onMs, int maxShown = 24) {
	std::vector<std::pair<double, uint8_t>> changes;
	int have = -1;
	for (const auto &w : r.writes) {
		if (w.addr != addr) continue;
		if (have == int(w.value)) continue;
		changes.push_back({w.ms, w.value});
		have = int(w.value);
	}
	size_t total = 0;
	for (const auto &w : r.writes) if (w.addr == addr) ++total;
	std::printf("    %04X: записей %5zu, разных значений подряд %3zu |", addr, total,
	            changes.size());
	const int step = int(changes.size()) > maxShown ? int(changes.size()) / maxShown : 1;
	int shown = 0;
	for (size_t i = 0; i < changes.size(); i += size_t(step)) {
		if (shown++ >= maxShown) break;
		std::printf(" %.0f:%02X", changes[i].first - onMs, changes[i].second);
	}
	std::printf("\n");
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// Правок может понадобиться НЕСКОЛЬКО сразу, и это не удобство, а необходимость. Время
	// атаки TVA само по себе ничего не меняет, если уровни огибающей стоят на максимуме:
	// рампе некуда идти, и «регистр не сдвинулся» будет означать не «параметр не доходит»,
	// а «раздражитель ничего не раздражал». Тройки: группа, параметр, шаг (минус - вниз).
	const std::string mode = argc > 1 ? argv[1] : "observe";
	struct Edit { int group, bank, presses; };
	std::vector<Edit> edits;
	for (int i = 2; i + 2 < argc; i += 3)
		edits.push_back({std::atoi(argv[i]), std::atoi(argv[i + 1]), std::atoi(argv[i + 2])});

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 10.0);
	if (!proc.getCore().isRunning()) { std::printf("прошивка не поднялась\n"); return 1; }

	proc.getCore().setTraceFilter(D110Core::kLa32TapBase, D110Core::kLa32TapEnd);

	std::printf("заводской сброс, чтобы тембр был известным...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 9.0);
	{
		const auto ram = ramOf(proc);
		std::printf("  тембр партии 1: группа %d, номер %d; структуры %d и %d\n",
		            ram[0x2000], ram[0x2001], ram[0x21E4 + 10], ram[0x21E4 + 11]);
	}

	// КОНТРОЛЬ. Окно той же длины без ноты обязано молчать: если в нём есть записи, всё
	// дальнейшее надо читать с поправкой на фон, а не как «вызвано нотой».
	std::printf("\n=== контроль: окно без ноты ===\n");
	{
		proc.getCore().startSoTrace();
		render(proc, 2.5);
		proc.getCore().stopSoTrace();
		const auto w = proc.getCore().takeSoWrites();
		std::printf("  записей: %zu%s\n", w.size(), w.empty() ? "" : "  !!! окно не молчит");
	}

	if (mode == "env") {
		for (const auto &e : edits) {
			std::printf("\nправка: Part+ x2, Group+ x%d, Bank+ x%d, Number%s x%d\n", e.group,
			            e.bank, e.presses < 0 ? "-" : "+", std::abs(e.presses));
			press(proc, "Exit", 3);
			press(proc, "Timbre");
			press(proc, "Edit");
			press(proc, "Edit");
			press(proc, "Part+", 2);
			if (e.group) press(proc, "Group+", e.group);
			if (e.bank) press(proc, "Bank+", e.bank);
			const auto before = ramOf(proc);
			press(proc, e.presses < 0 ? "Number-" : "Number+", std::abs(e.presses));
			render(proc, 0.6);
			const auto after = ramOf(proc);
			std::printf("  сдвинулось в тембре:");
			bool any = false;
			for (int i = 0; i < 246; ++i)
				if (before[(size_t)(0x21E4 + i)] != after[(size_t)(0x21E4 + i)]) {
					any = true;
					std::printf(" +%d(%d->%d)", i, before[(size_t)(0x21E4 + i)],
					            after[(size_t)(0x21E4 + i)]);
				}
			std::printf("%s\n", any ? "" : " НИЧЕГО - правка не состоялась");
		}
		press(proc, "Exit", 3);
	}

	constexpr double kHold = 1.5, kTail = 1.2;
	std::printf("\n=== одна нота 60, сила 100: держим %.1f с, потом %.1f с после снятия ===\n",
	            kHold, kTail);
	const Run r = playOne(proc, 60, 100, kHold, kTail);
	const double onMs = r.writes.empty() ? 0.0 : r.writes.front().ms;
	std::printf("  всего записей %zu, потеряно %llu, слоты ноты:", r.writes.size(),
	            (unsigned long long)r.dropped);
	for (int s : r.slots) std::printf(" %d", s);
	std::printf("\n");
	if (r.dropped) std::printf("  !!! захват переполнился - всё ниже нижняя граница\n");
	std::printf("  ответов микросхемы за ноту (LA32 services): %llu\n",
	            (unsigned long long)(r.servicesAfter - r.servicesBefore));

	reportBanks(r);

	std::printf("\n  ряды во времени, мс от первой записи (только моменты ИЗМЕНЕНИЯ значения)\n");
	for (int slot : r.slots) {
		std::printf("  --- слот %d ---\n", slot);
		for (uint16_t bank2 : {0x0C00, 0x0C40, 0x0C80, 0x0CC0, 0x0D00})
			for (int b = 0; b < kBytesPerSlot; ++b)
				reportSeries(r, uint16_t(bank2 + slot * kBytesPerSlot + b), onMs);
	}

	proc.setPoweredOn(false);
	std::printf("\nготово\n");
	return 0;
}
