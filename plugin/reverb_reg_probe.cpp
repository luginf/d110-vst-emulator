// Каким адресом и каким значением прошивка задаёт ТИП ревербератора.
//
// Прошлый зонд (plugin/so_trace_probe.cpp) закрыл защёлку SO: тип её не трогает, при любом
// из восьми типов при загрузке пишется одно и то же. Отсюда был сделан вывод, что тип
// уходит по шине звуковой платы 0x0C00-0x0D02. Сервисные заметки говорят, что вывод
// неверен: у микросхемы ревербератора IC5 СВОЙ вход от процессора - пять бит данных D1-D5
// и два строба STB0/STB1, собранных из выходов вентильной матрицы
// (STB0 = НЕ(EXIO1 · WL), STB1 = НЕ(EXIO2 · WL)). Подробности и откуда это взято -
// docs/service_notes_findings.md.
//
// Адреса EXIO1/EXIO2 внутри матрицы и на схеме не названы, поэтому здесь они НЕ
// угадываются: перехват берёт весь свободный участок 0x0400-0x0BFF целиком
// (D110Core::kExtIoTapBase) и печатает всё, что туда записали, вместе с адресом
// подпрограммы.
//
// Устройство опыта:
//   * КОНТРОЛЬ ПЕРЕД ВЫВОДОМ. «Ни одной записи» - отрицательный результат, и верить ему
//     нельзя, пока тот же захват не покажет записи там, где они заведомо есть. Поэтому
//     сначала печатается загрузка целиком: если перехват мёртв, это видно сразу, а не
//     превращается в вывод про D-110.
//   * КОНТРОЛЬНЫЙ ПРОГОН БЕЗ СМЕНЫ ТИПА. Те же нажатия и та же нота, но тип не меняется.
//     Без него «значение изменилось» ничего не значит: измениться оно могло от самих
//     нажатий или от ноты.
//   * ДВА РАЗДРАЖИТЕЛЯ. После установки типа берётся и пауза, и нота: если микросхема
//     программируется не в момент правки, а при следующем распределении голоса, разница
//     появится только после ноты.
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

// Ноту берём через прошивку, а не прямо в движок: программировать ревербератор она стала бы
// в своём собственном распределении голоса, а не по приходу MIDI в чужую половину.
void playNote(D110AudioProcessor &proc, uint8_t note) {
	const uint8_t on[3] = {0x91, note, 100}; // канал 2 - это партия 1 по заводской раскладке
	const uint8_t off[3] = {0x81, note, 0};
	proc.getCore().pushMidi(on, 3);
	render(proc, 1.2);
	proc.getCore().pushMidi(off, 3);
	render(proc, 0.6);
}

// Сводка одного окна захвата. Группировка идёт по ПАРЕ «адрес и подпрограмма», а не по
// одному адресу: по 0x021A пишут ДВЕ разные вещи - опрос панели гонит туда стробы столбцов
// сотнями в секунду, а правка ревербератора пишет один раз, - и сваленные в одну строку
// значения выглядят как один поток, в котором ничего не разобрать.
using Key = std::pair<uint16_t, uint16_t>; // адрес порта и адрес писавшей подпрограммы

struct Window {
	std::map<Key, std::set<uint8_t>> values;
	std::map<Key, size_t> hits;
	size_t count = 0;
	uint64_t dropped = 0;
};

Window collect(D110AudioProcessor &proc) {
	Window w;
	w.dropped = proc.getCore().soWritesDropped();
	for (const auto &e : proc.getCore().takeSoWrites()) {
		w.values[{e.addr, e.pc}].insert(e.value);
		++w.hits[{e.addr, e.pc}];
		++w.count;
	}
	return w;
}

void printWindow(const Window &w, const char *indent) {
	if (w.values.empty()) { std::printf("%s(ни одной записи)\n", indent); return; }
	for (const auto &[key, values] : w.values) {
		std::printf("%s0x%04X из ПЗУ %04X, %zu раз <-", indent, key.first, key.second,
		            w.hits.at(key));
		int shown = 0;
		for (uint8_t v : values) {
			if (shown++ == 12) { std::printf(" ...(всего %d)", int(values.size())); break; }
			std::printf(" %02X", v);
		}
		std::printf("\n");
	}
	if (w.dropped)
		std::printf("%s!!! захват потерял %llu записей - всё выше это нижняя граница\n",
		            indent, (unsigned long long)w.dropped);
}

// Идём в Patch Edit на страницу Reverb Type. Дорога снята зондом d110_reverb_path:
// Patch -> Edit открывает Name, дальше Group+ листает параметры (Name, Reverb Type,
// Reverb Time, Reverb Level), а значение меняет Number+.
void toReverbType(D110AudioProcessor &proc) {
	press(proc, "Exit", 2);
	press(proc, "Patch");
	press(proc, "Edit");
	press(proc, "Group+"); // Name -> Reverb Type
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);

	// Захват включается ДО подачи питания: загрузка - единственное место, где защёлка SO
	// заведомо пишется, и она же контроль работоспособности перехвата.
	proc.getCore().startSoTrace();
	proc.setPoweredOn(true);
	render(proc, 10.0);
	std::printf("прошивка: %s   знакогенератор: %s\n",
	            proc.getCore().isRunning() ? "работает" : "НЕТ",
	            g_cgrom.empty() ? "НЕ НАЙДЕН" : "загружен");

	// ---- КОНТРОЛЬ: жив ли перехват вообще ---------------------------------------------
	std::printf("\n=== КОНТРОЛЬ: всё, что записано во внешний ввод-вывод при загрузке ===\n");
	std::printf("  (перехват стоит на 0x0200-0x0201, 0x0280-0x0281 и 0x0400-0x0BFF)\n");
	{
		const Window boot = collect(proc);
		std::printf("  записей: %zu\n", boot.count);
		printWindow(boot, "    ");
		if (boot.count == 0) {
			std::printf("\n  Перехват не увидел НИ ОДНОЙ записи, даже защёлки SO, про которую\n"
			            "  известно, что при загрузке она пишется пять раз. Значит сломан\n"
			            "  захват, а не молчит прошивка. Дальше идти незачем.\n");
			proc.setPoweredOn(false);
			proc.releaseResources();
			return 1;
		}
	}

	// ---- развёртка типа ----------------------------------------------------------------
	// Тип живёт в ОЗУ по 0x2D95 и меняется от Number+ на своей странице; это уже измерено
	// зондом d110_reverb_path, и здесь оно ещё раз печатается рядом - чтобы «значение в ОЗУ
	// не изменилось» нельзя было спутать с «микросхеме ничего не написали».
	auto reverbTypeByte = [&proc] {
		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		return ram[0x2D95];
	};

	std::printf("\n=== развёртка: восемь типов ревербератора ===\n");
	toReverbType(proc);
	std::printf("  страница: \"%s\"\n", screen(proc).c_str());
	press(proc, "Number-", 10); // на нижний упор, чтобы шаги были предсказуемы
	render(proc, 0.5);
	std::printf("  после спуска на упор: \"%s\"  ОЗУ 0x2D95 = %d\n",
	            screen(proc).c_str(), reverbTypeByte());

	std::map<Key, std::set<uint8_t>> perAddrAcrossTypes;
	for (int step = 0; step < 8; ++step) {
		proc.getCore().startSoTrace();
		press(proc, "Number+");
		render(proc, 1.0);
		const Window afterEdit = collect(proc);

		proc.getCore().startSoTrace();
		playNote(proc, 60);
		const Window afterNote = collect(proc);

		std::printf("\n  --- шаг %d: экран \"%s\"  ОЗУ 0x2D95 = %d ---\n",
		            step + 1, screen(proc).c_str(), reverbTypeByte());
		std::printf("    сразу после правки (%zu записей):\n", afterEdit.count);
		printWindow(afterEdit, "      ");
		std::printf("    после ноты (%zu записей):\n", afterNote.count);
		printWindow(afterNote, "      ");

		for (const auto &w : {afterEdit, afterNote})
			for (const auto &[key, values] : w.values)
				perAddrAcrossTypes[key].insert(values.begin(), values.end());
	}

	// ---- КОНТРОЛЬНЫЙ ПРОГОН: те же нажатия и та же нота, но тип НЕ меняется -------------
	// Без него «по этому адресу значения разные» не значит «их задаёт тип»: их могли задать
	// сами нажатия, нота или просто время.
	std::printf("\n=== КОНТРОЛЬ: те же действия, но без смены типа ===\n");
	press(proc, "Exit", 2);
	std::printf("  экран: \"%s\"  ОЗУ 0x2D95 = %d (дальше не меняется)\n",
	            screen(proc).c_str(), reverbTypeByte());
	std::map<Key, std::set<uint8_t>> perAddrControl;
	for (int step = 0; step < 8; ++step) {
		proc.getCore().startSoTrace();
		press(proc, "Group+"); // нажатие, ничего не правящее
		render(proc, 1.0);
		playNote(proc, 60);
		const Window w = collect(proc);
		for (const auto &[key, values] : w.values)
			perAddrControl[key].insert(values.begin(), values.end());
	}
	std::printf("  за восемь одинаковых проходов:\n");
	{
		Window c;
		c.values = perAddrControl;
		for (const auto &[key, values] : perAddrControl) c.hits[key] = values.size();
		printWindow(c, "    ");
	}

	// ---- ответ --------------------------------------------------------------------------
	std::printf("\n=== ответ ===\n");
	// Решает не число разных значений, а сравнение с контролем: адрес, куда пишут ТОЛЬКО
	// при смене типа, называет путь однозначно, сколько бы бит он ни нёс. Тип разложен по
	// двум портам (биты 1-3 в 0x021A, бит 0 в 0x0800), и требование «восемь разных значений
	// на одном адресе» отвергло бы верный ответ - первая редакция этого зонда так и делала.
	std::printf("  порт  | из ПЗУ | значений при смене типа | при контроле | вывод\n");
	for (const auto &[key, values] : perAddrAcrossTypes) {
		const size_t ctrl = perAddrControl.count(key) ? perAddrControl.at(key).size() : 0;
		const char *verdict = "пишется и без смены типа";
		if (ctrl == 0) verdict = "<== ПИШЕТСЯ ТОЛЬКО ПРИ СМЕНЕ ТИПА";
		else if (values.size() > ctrl) verdict = "пишется всегда, но при смене типа шире";
		std::printf("  0x%04X |   %04X | %23zu | %12zu | %s\n", key.first, key.second,
		            values.size(), ctrl, verdict);
	}
	for (const auto &[key, values] : perAddrControl)
		if (!perAddrAcrossTypes.count(key))
			std::printf("  0x%04X |   %04X | %23d | %12zu | только в контроле\n",
			            key.first, key.second, 0, values.size());

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\nготово\n");
	return 0;
}
