// Откуда в начале демо берутся два «Attempted to play unmapped key 25/27».
//
// Движок отказывается играть ритм-клавишу, у которой в Rhythm Setup стоит тембр 127 (OFF)
// - Part.cpp, RhythmPart::noteOn. Возможных объяснений ровно два, и они требуют разных
// правок, поэтому их надо различить, а не выбрать:
//
//   1. Гонка. Прошивка загружает карту ритма при старте песни, а зеркало снимает ОЗУ раз в
//      кадр, и первые удары успевают раньше отправки региона.
//   2. Карта. Прошивка сама держит для этих клавиш OFF и всё равно по ним стучит - тогда
//      расходится либо база региона, либо трактовка поля.
//
// Различает их одно измерение: что лежит в этих записях у ПРОШИВКИ и у ДВИЖКА до начала
// песни и после, и в какой момент относительно первых ударов уходит регион. Счётчик
// отправок региона снимается тем же циклом, что и звук, поэтому времена сопоставимы.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kBlockSeconds = double(kBlock) / kSampleRate;
using Clock = std::chrono::steady_clock;

constexpr uint32_t packed(uint32_t a) {
	return ((a & 0x7f0000u) >> 2) | ((a & 0x7f00u) >> 1) | (a & 0x7fu);
}
constexpr uint32_t kRhythmSysex = 0x030110;
constexpr uint16_t kRhythmRam = 0x2090;
constexpr int kFirstRhythmKey = 24; // запись N описывает клавишу 24 + N

// Отсчёт для всех отметок времени в этом инструменте: он же начало журнала нот, поэтому
// отправки регионов и удары ритма измеряются одной линейкой.
Clock::time_point g_zero;
bool g_watching = false;
struct Emit { double ms; uint64_t c0, c1, c2; };
std::vector<Emit> g_emits;
uint64_t g_prev0 = 0, g_prev1 = 0, g_prev2 = 0;

double elapsedMs() {
	return std::chrono::duration<double, std::milli>(Clock::now() - g_zero).count();
}

// Единственный способ двигать время в этом инструменте: считает звук блоками и на каждом
// блоке снимает счётчики отправок Rhythm Setup (регионы 1..3 в kMirrorRegions).
//
// Опрос идёт и во время нажатий тоже. Пока он начинался только после них, ответ на главный
// вопрос - что раньше, карта ритма или первые удары, - был недостижим: счётчик впервые
// читался уже с накопленным значением, и по нему было видно лишь «когда-то до сих пор».
void pump(D110AudioProcessor &proc, double seconds) {
	juce::AudioBuffer<float> block(2, kBlock);
	const auto begin = Clock::now();
	auto next = begin;
	while (std::chrono::duration<double>(Clock::now() - begin).count() < seconds) {
		juce::MidiBuffer none;
		block.clear();
		proc.processBlock(block, none);
		if (g_watching) {
			const uint64_t c0 = proc.getCore().regionEmitCount(1);
			const uint64_t c1 = proc.getCore().regionEmitCount(2);
			const uint64_t c2 = proc.getCore().regionEmitCount(3);
			if (c0 != g_prev0 || c1 != g_prev1 || c2 != g_prev2) {
				g_emits.push_back({elapsedMs(), c0, c1, c2});
				g_prev0 = c0; g_prev1 = c1; g_prev2 = c2;
			}
		}
		next += std::chrono::microseconds(int64_t(kBlockSeconds * 1e6));
		std::this_thread::sleep_until(next);
	}
}

// Кнопка держится и отпускается ПОД расчёт звука, а не под sleep. В хосте плагин считает
// звук непрерывно, и окно между появлением параметра и его применением равно одному блоку;
// пауза без расчёта копит обе очереди на всё время нажатия и растягивает это окно до
// полутора секунд - то есть измеряет сам инструмент, а не машину.
void press(D110AudioProcessor &proc, std::initializer_list<int> idx, int hold, int settle) {
	for (int i : idx) proc.getCore().setButton(i, true);
	pump(proc, hold / 1000.0);
	for (int i : idx) proc.getCore().setButton(i, false);
	pump(proc, settle / 1000.0);
}

// Записи Rhythm Setup для клавиш `from`..`to`, из ОЗУ прошивки и из движка, рядом.
// Поле тембра: 127 - это OFF, остальное - номер тембра в ритм-банке.
void dumpMap(D110AudioProcessor &proc, const std::vector<uint8_t> &ram, int from, int to,
             const char *when) {
	std::printf("\n  Rhythm Setup, %s\n", when);
	std::printf("   клавиша | прошивка: тембр ур. пан вых | движок: тембр ур. пан вых\n");
	for (int key = from; key <= to; ++key) {
		const int entry = key - kFirstRhythmKey;
		const uint8_t *fw = &ram[kRhythmRam + 4 * entry];
		uint8_t eng[4];
		std::memset(eng, 0xAA, sizeof eng);
		proc.engineReadMemory(packed(kRhythmSysex) + 4u * uint32_t(entry), 4, eng);
		const bool differ = std::memcmp(fw, eng, 4) != 0;
		std::printf("   %7d | %14s%3d %3d %3d %3d | %8s%3d %3d %3d %3d%s\n", key,
		            fw[0] == 127 ? "OFF " : "", fw[0], fw[1], fw[2], fw[3],
		            eng[0] == 127 ? "OFF " : "", eng[0], eng[1], eng[2], eng[3],
		            differ ? "   <-- РАСХОДЯТСЯ" : "");
	}
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	pump(proc, 9.0); // считаем, а не спим: иначе затор зеркала применится позже
	std::printf("прошивка работает: %s   движок открыт: %s\n",
	            proc.getCore().isRunning() ? "да" : "НЕТ",
	            proc.engineIsOpen() ? "да" : "НЕТ");
	if (!proc.engineIsOpen()) return 1;

	// Клавиши 24..31 - те, где стоят пропущенные 25 и 27.
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());
	dumpMap(proc, ram, 24, 31, "ДО запуска песни");

	proc.getCore().resetTallies();
	proc.getCore().startNoteLog();
	g_zero = Clock::now();
	g_watching = true;
	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 500);
	press(proc, {D110Core::buttonIndex(1, 0)}, 200, 500);
	pump(proc, 12.0);

	proc.getCore().getRam(ram.data());
	dumpMap(proc, ram, 24, 31, "ПОСЛЕ 12 секунд песни");

	std::printf("\n  Отправки Rhythm Setup, мс от startNoteLog (нажатия входят в отсчёт):\n");
	if (g_emits.empty()) std::printf("   ни одной - карта ритма не менялась\n");
	for (const auto &e : g_emits)
		std::printf("   %8.1f мс   куски: %llu / %llu / %llu\n", e.ms,
		            (unsigned long long)e.c0, (unsigned long long)e.c1, (unsigned long long)e.c2);

	std::printf("\n  Первые удары ритм-партии (партия 8 = ритм), мс от startNoteLog:\n");
	int shown = 0;
	const auto log = proc.getCore().takeNoteLog();
	for (const auto &e : log) {
		if (!e.on || e.part != 8) continue;
		std::printf("   %8.1f мс   клавиша %3d  velocity %3d\n", e.ms, e.note, e.velocity);
		if (++shown >= 12) break;
	}
	std::printf("   журнал: %zu событий, потеряно %llu; кольцо зеркала потеряло %llu\n",
	            log.size(), (unsigned long long)proc.getCore().noteLogDropped_(),
	            (unsigned long long)proc.getCore().sysexDropped());

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
