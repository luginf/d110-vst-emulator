// Когда прошивка трогает регистр SO, и какая подпрограмма это делает.
//
// SO - единственный виденный путь от процессора к микросхеме ревербератора BOSS: биты 1-2
// это A13/A14 её ПЗУ, то есть номер программы, бит 3 - "R. SW." на аналоговую плату
// (roland_d10.cpp, so_w). Предыдущий зонд (d110_reverb_path) показал, что при правке
// Reverb Type регистр не пишется ВООБЩЕ, и что за всю загрузку записей всего четыре.
// Значит момент перепрограммирования - какой-то другой, и его надо найти: без него нечего
// подавать на вход будущей эмуляции микросхемы, да и просто неизвестно, отвечает ли
// вообще номер программы за тип ревербератора.
//
// Прибор прогоняется через состояния, в каждом из которых микросхему разумно ожидать
// перепрограммированной, и на каждом печатаются записи в SO вместе с адресом, откуда они
// сделаны. Адрес важнее значения: он даёт точку входа для дизассемблера.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
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

bool isCgrom(const juce::MemoryBlock &d) {
	if (d.getSize() != 4096) return false;
	const auto *p = static_cast<const uint8_t *>(d.getData());
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

void render(D110AudioProcessor &proc, double seconds, juce::MidiBuffer midi = {}) {
	juce::AudioBuffer<float> block(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	bool first = true;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer m;
		if (first) { m = midi; first = false; }
		block.clear();
		proc.processBlock(block, m);
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

// Записи за прошедший отрезок, сведённые по паре "адрес + значение". Одиночные строки
// утонули бы в повторах: лампа MIDI сидит в бите 0 того же регистра и мигает часто.
void report(D110AudioProcessor &proc, const char *what) {
	const auto writes = proc.getCore().takeSoWrites();
	const uint64_t dropped = proc.getCore().soWritesDropped();
	std::printf("\n--- %s ---\n", what);
	if (writes.empty()) {
		std::printf("    записей в SO нет (потеряно %llu)\n", (unsigned long long)dropped);
		return;
	}
	std::map<std::pair<uint16_t, uint8_t>, int> tally;
	for (const auto &w : writes) ++tally[{w.pc, w.value}];
	std::printf("    записей %zu, потеряно %llu, первая на %.0f мс\n", writes.size(),
	            (unsigned long long)dropped, writes.front().ms);
	for (const auto &kv : tally)
		std::printf("      из 0x%04X  значение %02X  (программа %d, R.SW %d, лампа %d)  x%d\n",
		            kv.first.first, kv.first.second, (kv.first.second >> 1) & 3,
		            (kv.first.second >> 3) & 1, kv.first.second & 1, kv.second);
}

// Обращения по адресам, которые карта памяти не покрывает, сведённые по адресу. Так в своё
// время нашёлся интерфейс LA32: то, чего в модели MAME нет, видно только здесь. Если тип
// ревербератора уходит в микросхему не через SO, а через порт, которого драйвер не знает,
// он проявится как адрес, появляющийся ровно на правке типа и не появляющийся в покое.
void reportUnmapped(D110AudioProcessor &proc, const char *what) {
	const auto lines = proc.getCore().takeLogLines();
	std::map<std::string, int> byAddr;
	for (const auto &l : lines) {
		// Формат MAME: "...unmapped program memory write to 1234 = 56 & FF"
		const auto to = l.find(" to ");
		if (to == std::string::npos) continue;
		const bool write = l.find("write") != std::string::npos;
		std::string addr = l.substr(to + 4, 4);
		byAddr[(write ? "W " : "R ") + addr] += 1;
	}
	std::printf("    неотображённые обращения: строк %zu, различных адресов %zu\n",
	            lines.size(), byAddr.size());
	int shown = 0;
	for (const auto &kv : byAddr) {
		std::printf("      %s x%d\n", kv.first.c_str(), kv.second);
		if (++shown >= 14) { std::printf("      ...\n"); break; }
	}
	(void)what;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.getCore().startSoTrace();
	// Ставится ДО пуска машины: перехватчик неотображённых обращений ставится один
	// раз при разборе устройств и позже уже не появится.
	proc.getCore().setLogUnmapped(true);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s\n", proc.getCore().isRunning() ? "работает" : "НЕТ");
	report(proc, "загрузка");

	render(proc, 3.0);
	report(proc, "простой 3 с");
	reportUnmapped(proc, "простой 3 с");

	// Нота с хоста. Лампа MIDI - бит 0 этого же регистра, так что записи здесь ОБЯЗАНЫ
	// быть; это контроль на исправность захвата, а не только измерение.
	{
		juce::MidiBuffer m;
		m.addEvent(juce::MidiMessage::noteOn(2, 60, 0.9f), 0);
		render(proc, 1.5, m);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(2, 60), 0);
		render(proc, 1.5, off);
	}
	report(proc, "нота с хоста (контроль: лампа сидит в бите 0)");

	press(proc, "Exit", 2);
	press(proc, "Patch");
	press(proc, "Number+", 3);
	render(proc, 1.5);
	std::printf("\nэкран после смены патча: \"%s\"\n", screen(proc).c_str());
	report(proc, "смена патча");
	reportUnmapped(proc, "смена патча");

	// Тип ревербератора меняется, а потом играется нота: если микросхема
	// перепрограммируется лениво, к первому звуку после правки, это увидим здесь.
	press(proc, "Exit", 2);
	press(proc, "Patch");
	press(proc, "Edit");
	press(proc, "Group+");
	press(proc, "Number+", 3);
	render(proc, 1.0);
	std::printf("\nэкран после правки типа: \"%s\"\n", screen(proc).c_str());
	report(proc, "правка Reverb Type");
	reportUnmapped(proc, "правка Reverb Type");
	{
		juce::MidiBuffer m;
		m.addEvent(juce::MidiMessage::noteOn(2, 64, 0.9f), 0);
		render(proc, 1.5, m);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(2, 64), 0);
		render(proc, 1.5, off);
	}
	report(proc, "первая нота ПОСЛЕ правки типа");

	// Демо-песня: самый плотный поток нот и смен настроек, какой прибор выдаёт сам.
	press(proc, "Exit", 2);
	press(proc, "Edit");
	press(proc, "Enter");
	press(proc, "Enter");
	render(proc, 12.0);
	std::printf("\nэкран демо: \"%s\"\n", screen(proc).c_str());
	report(proc, "демо-песня, 12 с");

	// Единственная запись в SO после загрузки шла из 0x2D28, и её значение между двумя
	// прогонами оказалось разным - 04 и 00 - при разном сохранённом типе ревербератора.
	// Если это не совпадение, то тип ВСЁ-ТАКИ выбирает программу ПЗУ, просто применяется
	// она однажды при включении. Проверяется прямо: выставить тип, выключить, включить и
	// посмотреть, что записалось. Тип живёт в батарейном ОЗУ и переживает выключение.
	std::printf("\n\n=== тип ревербератора -> номер программы, через выключение ===\n");
	for (int type = 1; type <= 8; ++type) {
		press(proc, "Exit", 2);
		press(proc, "Patch");
		press(proc, "Edit");
		press(proc, "Group+");           // Name -> Reverb Type
		press(proc, "Number-", 10);      // до нижнего упора
		press(proc, "Number+", type - 1);
		render(proc, 0.6);
		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		const std::string shown = screen(proc);
		const uint8_t stored = ram[0x2D95];

		proc.setPoweredOn(false);
		std::this_thread::sleep_for(std::chrono::milliseconds(600));
		proc.getCore().startSoTrace();
		proc.setPoweredOn(true);
		render(proc, 9.0);

		const auto writes = proc.getCore().takeSoWrites();
		std::printf("\n  тип на экране \"%s\"  ОЗУ 0x2D95 = %d\n", shown.c_str(), stored);
		if (writes.empty()) {
			std::printf("    при загрузке записей в SO нет\n");
		} else {
			for (const auto &w : writes)
				std::printf("    из 0x%04X значение %02X -> программа %d, R.SW %d\n", w.pc,
				            w.value, (w.value >> 1) & 3, (w.value >> 3) & 1);
		}
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
