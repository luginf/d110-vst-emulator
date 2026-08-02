// Первый мост от РЕГИСТРОВ к ЗВУКУ: снять состояние, которое настоящая прошивка положила в
// LA32, и прогнать его через модель той же микросхемы.
//
// Модель уже есть - `LA32FloatWaveGenerator` из munt. Чего у неё нет, так это подачи
// состояния от живой прошивки: munt подаёт ей то, что вычислил его собственный синтезатор,
// заменяющий прошивку. Здесь наоборот - берётся то, что прошивка написала в регистры, и
// разобрано по docs/la32_register_map.md.
//
// Проверка не на слух. У ноты 60 частота известна заранее, и она меряется по нулям сигнала.
// Если разбор регистров верен, основной тон обязан совпасть; если шкала высоты понята
// неверно - разойдётся, и на сколько именно, тоже будет видно. Заодно это разрешает
// расхождение 4111 против 4096 на октаву, записанное в документе как неразъяснённое.
//
// Пока только СИНТЕТИЧЕСКИЙ партиал: для PCM нужен разбор адреса и длины волны в ПЗУ, а он
// ещё не сделан.
#include "Source/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "LA32FloatWaveGenerator.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr double kChipRate = D110Core::kLa32SampleRate;
// Генератор ждёт не уровень, а его ЛОГАРИФМИЧЕСКОЕ ДОПОЛНЕНИЕ - см. подробный комментарий у
// первого использования ниже.
constexpr MT32Emu::Bit32u kAmpFull = 67117056;
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

struct Btn { const char *name; int port; int bit; };
const Btn kButtons[] = {
	{"Exit", 0, 7}, {"Patch", 0, 6}, {"Timbre", 0, 5}, {"Part+", 0, 4},
	{"Group+", 0, 3}, {"Bank+", 0, 2}, {"Number+", 0, 1}, {"Write", 0, 0},
	{"Edit", 1, 7}, {"Part", 1, 6}, {"System", 1, 5}, {"Part-", 1, 4},
	{"Group-", 1, 3}, {"Bank-", 1, 2}, {"Number-", 1, 1}, {"Enter", 1, 0},
};

void pressButton(D110AudioProcessor &proc, const char *name, int times = 1) {
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

// Состояние одного голоса, собранное из записей в регистры.
struct Voice {
	bool seen = false;
	bool isPcm = false;      // 0x0D00 чётный байт, бит 7
	bool sawtooth = false;   // он же, бит 6
	uint8_t resonance = 0;   // 0x0D00 нечётный байт, младшие 5 бит, минус единица
	uint8_t pulseWidth = 0;  // 0x0C40 чётный байт (СИНТЕЗ) - у PCM-партиала не участвует
	uint8_t cutoff = 0;      // 0x0C40 нечётный байт (СИНТЕЗ) - у PCM это pos волны, см. ниже
	uint8_t wavePos = 0;     // 0x0C40 нечётный байт (PCM) - см. ниже
	uint8_t ampTarget = 0;   // выбранный флагом банк рампы, нечётный байт
	uint16_t pitch = 0;      // 0x0CC0, шестнадцатибитное
	// 0x0D00 чётный байт, бит 5. Совпал с Partial::isRingModulatingNoMix() из munt на всех
	// одиннадцати достижимых структурах и обоих слотах - 22 точки без исключений
	// (docs/la32_register_map.md). У ведущего партиала он взведён только при mix 2, у
	// ведомого - при mix 1 и mix 2, то есть везде, где идёт кольцевая модуляция без
	// подмешивания ведущего.
	bool ringNoMix = false;
	// Он же, бит 6, у PCM-партиала: гаснет ровно у ведомого в кольцевой модуляции. Это в
	// точности условие pcmWaveInterpolated у munt, где сказано, что у такого партиала
	// умножитель интерполяции занят кольцевым модулятором. У синтетического партиала тот же
	// бит несёт пилу - разделить эти два смысла на нынешних данных нечем, оба не опровергнуты.
	bool pcmInterpolated = true;
	// 0x0D00 нечётный байт у PCM-партиала: старшие биты - длина и цикл волны (см. ниже),
	// младшие - резонанс, тот же, что и у синтетического.
	bool pcmLoop = false;
	uint32_t pcmLen = 0;
};

// Таблица волн из ПЗУ пресетов не читается здесь вовсе - и это не упущение. Разбор регистров
// (docs/la32_register_map.md, "PCM: адрес и длина волны читаются прямо из регистров") нашёл
// точное совпадение регистра 0x0C40.x.1 с байтом `pos` этой таблицы для трёх разных значений
// pcmWave подряд - но сама таблица нужна только ПРОШИВКЕ, чтобы вычислить адрес; микросхеме
// таблица неизвестна вовсе, она получает уже готовый адрес и длину. Поэтому рендер ниже берёт
// адрес и длину прямо из регистров, как это делает и настоящая LA32.

// Раскодирование сырых байт волнового ПЗУ в те же логарифмические отсчёты, что строит
// Synth::loadPCMROM в munt - тот же файл, та же чересстрочная развёртка по двум микросхемам,
// повторённая здесь один в один, потому что публичного доступа к уже загруженным данным
// синтеза плагин не даёт, а сами байты те же самые, что он загружает через тот же файл.
std::vector<MT32Emu::Bit16s> decodePcmRom(const juce::File &waveRom) {
	juce::MemoryBlock d;
	std::vector<MT32Emu::Bit16s> out;
	if (!waveRom.loadFileAsData(d)) return out;
	const auto *p = static_cast<const uint8_t *>(d.getData());
	const size_t n = d.getSize() / 2;
	out.resize(n);
	static const int order[16] = {0, 9, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 8};
	for (size_t i = 0; i < n; ++i) {
		const uint8_t s = p[2 * i], c = p[2 * i + 1];
		int16_t log = 0;
		for (int u = 0; u < 16; ++u) {
			const int bit = order[u] < 8 ? ((s >> (7 - order[u])) & 1)
			                             : ((c >> (7 - (order[u] - 8))) & 1);
			log = int16_t(log | (bit << (15 - u)));
		}
		out[i] = log;
	}
	return out;
}

// Частота ноты при строе, который D-110 показывает на экране.
double noteHz(int note, double masterTuneHz) {
	return masterTuneHz * std::pow(2.0, (note - 69) / 12.0);
}

// Основной тон по переходам через ноль вверх, на установившемся куске. Способ грубый, но для
// вопроса «та ли октава и тот ли полутон» его хватает с запасом, а тонкую долю он даёт по
// среднему периоду, а не по одному.
double fundamentalHz(const std::vector<float> &x, double sr) {
	size_t first = 0, last = 0;
	int crossings = 0;
	for (size_t i = 1; i < x.size(); ++i) {
		if (!(x[i - 1] <= 0.0f && x[i] > 0.0f)) continue;
		if (!crossings++) first = i;
		last = i;
	}
	if (crossings < 3) return 0.0;
	return sr * double(crossings - 1) / double(last - first);
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const int note = argc > 1 ? std::atoi(argv[1]) : 60;
	// Сила нажатия - управляемый вход: она двигает ЦЕЛЬ рампы, то есть уровень. Проверять
	// закон громкости надо по НАКЛОНУ, а не по одному пику: абсолютный пик зависит ещё и от
	// формы волны, а отношение двух пиков при известной разнице уровней - уже нет.
	const int velocity = argc > 3 ? std::atoi(argv[3]) : 100;
	// На сколько шагов сдвинуть структуру пары 1&2 от заводской. Нужно, чтобы проверить путь
	// кольцевой модуляции: у заводского тембра структура 2, то есть простое сложение, и на
	// нём кольцевой модулятор не работает вовсе - а значит и утверждать про него нечего.
	const int structureSteps = argc > 4 ? std::atoi(argv[4]) : 0;
	const juce::File outDir = argc > 2 ? juce::File(juce::String(argv[2]))
	                                   : juce::File::getCurrentWorkingDirectory();

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 10.0);
	if (!proc.getCore().isRunning()) { std::printf("прошивка не поднялась\n"); return 1; }

	proc.getCore().setStuckPolicy(D110Core::StuckPolicy::La32Ramps);
	proc.getCore().setLa32PresetFf(true);
	proc.getCore().setLa32StatusMode(1);
	proc.getCore().setTraceFilter(D110Core::kLa32TapBase, D110Core::kLa32TapEnd);

	std::printf("заводской сброс...\n");
	proc.getCore().factoryReset();
	render(proc, 3.0);
	while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
	render(proc, 9.0);

	if (structureSteps) {
		pressButton(proc, "Exit", 3);
		pressButton(proc, "Timbre");
		pressButton(proc, "Edit");
		pressButton(proc, "Edit");
		pressButton(proc, "Group+", 1); // общая часть, страница структуры пары 1&2
		pressButton(proc, "Number+", structureSteps);
		render(proc, 0.4);
		pressButton(proc, "Exit", 3);
		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		std::printf("структура пары 1&2: %d\n", ram[0x21E4 + 10]);
	}

	// Одна нота, короткое окно: нужны значения, которые прошивка положила при выдаче голоса.
	// Нота держится и отпускается ВНУТРИ окна захвата: ступени огибающей приходят и при
	// нажатии, и при снятии, и без второй половины проверять в рампе нечего.
	proc.getCore().startSoTrace();
	const uint8_t on[3] = {0x91, uint8_t(note), uint8_t(velocity)};
	proc.getCore().pushMidi(on, 3);
	render(proc, 1.0);
	const uint8_t off[3] = {0x81, uint8_t(note), 0};
	proc.getCore().pushMidi(off, 3);
	render(proc, 1.2);
	proc.getCore().stopSoTrace();
	const auto writes = proc.getCore().takeSoWrites();

	// Разбор. Берётся ПЕРВОЕ значение каждого регистра: ведущее 0xFF - это установка, а не
	// данные, но здесь оно в чётных байтах, которые нас в этой части не интересуют.
	Voice v[D110Core::kNumHardwareVoices];
	int bankOf[D110Core::kNumHardwareVoices] = {};
	std::map<uint16_t, uint8_t> firstValue;
	for (const auto &w : writes)
		if (!firstValue.count(w.addr)) firstValue[w.addr] = w.value;

	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		const auto flag = firstValue.find(uint16_t(0x0D00 + 2 * s));
		if (flag == firstValue.end()) continue;
		v[s].seen = true;
		v[s].isPcm = (flag->second & 0x80) != 0;
		v[s].sawtooth = (flag->second & 0x40) != 0;
		v[s].pcmInterpolated = (flag->second & 0x40) != 0;
		v[s].ringNoMix = (flag->second & 0x20) != 0;
		bankOf[s] = v[s].isPcm ? 0x0C00 : 0x0C80;
		const auto res = firstValue.find(uint16_t(0x0D01 + 2 * s));
		if (res != firstValue.end()) {
			v[s].resonance = uint8_t(res->second & 0x1F);
			// Измерено точным совпадением (docs/la32_register_map.md): для тона со сменой
			// pcmWave 61->81 регистр 0D00.x.1 дал 18->88, а сама запись в ПЗУ - len 0x10 и
			// 0x80. 0x18 = 0x10|0x08, 0x88 = 0x80|0x08 - старшие биты в точности байт len,
			// младшие не тронуты правкой волны. Значит длина и цикл лежат в тех же старших
			// битах, что у синтетического партиала пусты.
			v[s].pcmLoop = (res->second & 0x80) != 0;
			v[s].pcmLen = 0x800u << ((res->second & 0x70) >> 4);
		}
		const auto pw = firstValue.find(uint16_t(0x0C40 + 2 * s));
		if (pw != firstValue.end()) v[s].pulseWidth = pw->second;
		const auto co = firstValue.find(uint16_t(0x0C41 + 2 * s));
		// Один и тот же нечётный байт банка 0x0C40 несёт РАЗНОЕ в зависимости от рода
		// партиала: срез у синтетического, байт pos волны у PCM. Измерено точным совпадением
		// с таблицей волн (docs/la32_register_map.md): для тона 61 и 64 регистр дал BA и C0
		// - ровно то же, что pos у записей 61 и 64 в самой таблице, без единого расхождения.
		if (co != firstValue.end()) {
			if (v[s].isPcm) v[s].wavePos = co->second;
			else v[s].cutoff = co->second;
		}
		const auto amp = firstValue.find(uint16_t(bankOf[s] + 2 * s + 1));
		if (amp != firstValue.end()) v[s].ampTarget = amp->second;
		const auto pl = firstValue.find(uint16_t(0x0CC0 + 2 * s));
		const auto ph = firstValue.find(uint16_t(0x0CC1 + 2 * s));
		// Ведущее 0xFF в потоке высоты - сброс, а не данные: берётся первое НЕ 0xFF.
		uint8_t lo = 0, hi = 0;
		for (const auto &w : writes) {
			if (w.addr == uint16_t(0x0CC0 + 2 * s) && w.value != 0xFF && !lo) lo = w.value;
			if (w.addr == uint16_t(0x0CC1 + 2 * s) && w.value != 0xFF && !hi) hi = w.value;
		}
		(void)pl; (void)ph;
		v[s].pitch = uint16_t((hi << 8) | lo);
	}

	std::printf("\n  слот | род      | пила/адрес | ширина/длина | срез | резонанс | цикл |"
	            " уровень | высота\n");
	int synthSlot = -1, pcmSlot = -1;
	// Слоты ноты по порядку выдачи: первый из пары - ведущий партиал, второй - ведомый.
	std::vector<int> pairSlots;
	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		if (!v[s].seen) continue;
		if (v[s].isPcm)
			std::printf("  %4d | %-8s | %010X | %12u | %4s | %8d | %-4s | %7d | %04X\n", s,
			            "PCM", unsigned(v[s].wavePos) << 11, v[s].pcmLen, "-", v[s].resonance,
			            v[s].pcmLoop ? "да" : "нет", v[s].ampTarget, v[s].pitch);
		else
			std::printf("  %4d | %-8s | %-10s | %12d | %4d | %8d | %-4s | %7d | %04X\n", s,
			            "синтез", v[s].sawtooth ? "пила" : "прямоуг", v[s].pulseWidth,
			            v[s].cutoff, v[s].resonance, "-", v[s].ampTarget, v[s].pitch);
		if (!v[s].isPcm && synthSlot < 0) synthSlot = s;
		if (v[s].isPcm && pcmSlot < 0) pcmSlot = s;
		pairSlots.push_back(s);
	}

	// Волновое ПЗУ - те же байты, что автозагрузка плагина берёт из тех же файлов MAME
	// (waveIc8+waveIc7), собранные в том же порядке (PluginProcessor.cpp:
	// pcmRomPath = "assembled from MAME chip dumps: wave IC8 + IC7").
	const auto romDir = D110AudioProcessor::getAutoRomFolder();
	std::vector<MT32Emu::Bit16s> pcmRom;
	{
		juce::MemoryBlock ic7, ic8;
		for (const auto &e : juce::RangedDirectoryIterator(romDir, false, "*", juce::File::findFiles)) {
			const auto name = e.getFile().getFileName().toLowerCase();
			if (name.contains("r15179878")) e.getFile().loadFileAsData(ic8); // wave IC8, идёт первой
			if (name.contains("r15179880")) e.getFile().loadFileAsData(ic7); // wave IC7, идёт второй
		}
		if (ic7.getSize() && ic8.getSize()) {
			juce::MemoryBlock joined(ic8);
			joined.append(ic7.getData(), ic7.getSize());
			const juce::File tmp = outDir.getChildFile("_pcm_rom.bin");
			tmp.replaceWithData(joined.getData(), joined.getSize());
			pcmRom = decodePcmRom(tmp);
			tmp.deleteFile();
		}
	}
	std::printf("\n  волновое ПЗУ: %s\n", pcmRom.empty() ? "НЕ НАЙДЕНО" :
	            (juce::String(pcmRom.size()) + " отсчётов").toRawUTF8());

	// --- ПАРА партиалов как ОДИН голос ---------------------------------------------------
	// На D-110 нота берёт два слота не потому, что звучит дважды, а потому что голос состоит
	// из пары партиалов, и микросхема сама решает, сложить их или перемножить кольцевой
	// модулятором. Пока они рендерились порознь, это был не голос, а его половинки.
	//
	// Кто ведущий, а кто ведомый, решает порядок слотов: прошивка выдаёт их подряд, и первый
	// из пары - ведущий. Режим смешивания берётся из бита 5, найденного перебором структур.
	if (int(pairSlots.size()) == 2) {
		const Voice &m = v[pairSlots[0]], &sv = v[pairSlots[1]];
		// ringModulated - идёт ли кольцевая модуляция вообще; mixed - подмешивается ли к её
		// выходу ведущий партиал. По munt: init(hasRingModulatingSlave(), mixType == 1), а
		// mixType == 1 это ровно "ведомый в кольце, ведущий подмешан" - то есть бит 5 у
		// ведомого взведён, а у ведущего нет.
		const bool ringModulated = sv.ringNoMix;
		const bool mixed = sv.ringNoMix && !m.ringNoMix;
		std::printf("\n  === голос как пара: ведущий слот %d, ведомый слот %d ===\n",
		            pairSlots[0], pairSlots[1]);
		std::printf("  кольцевая модуляция: %s, ведущий подмешан: %s\n",
		            ringModulated ? "да" : "нет", mixed ? "да" : "нет");

		bool ok = true;
		MT32Emu::LA32FloatPartialPair pair;
		pair.init(ringModulated, mixed);
		const MT32Emu::LA32PartialPair::PairType kRoles[2] = {
			MT32Emu::LA32PartialPair::MASTER, MT32Emu::LA32PartialPair::SLAVE};
		for (int i = 0; i < 2 && ok; ++i) {
			const Voice &pv = v[pairSlots[(size_t)i]];
			if (!pv.isPcm) {
				pair.initSynth(kRoles[i], pv.sawtooth, pv.pulseWidth,
				               uint8_t(pv.resonance ? pv.resonance : 1));
				continue;
			}
			const uint32_t addr = uint32_t(pv.wavePos) << 11;
			if (pcmRom.empty() || addr + pv.pcmLen > pcmRom.size()) {
				std::printf("  PCM-волна по адресу %06X недоступна - пара пропущена\n", addr);
				ok = false;
				break;
			}
			std::printf("  слот %d: волна %06X, длина %u, цикл %s, интерполяция %s\n",
			            pairSlots[(size_t)i], addr, pv.pcmLen, pv.pcmLoop ? "да" : "нет",
			            pv.pcmInterpolated ? "да" : "нет");
			pair.initPCM(kRoles[i], pcmRom.data() + addr, pv.pcmLen, pv.pcmLoop);
		}

		if (ok) {
			const int nPair = int(kChipRate * 0.4);
			std::vector<float> outPair((size_t)nPair, 0.0f);
			float peakPair = 0.0f;
			for (int i = 0; i < nPair; ++i) {
				for (int p = 0; p < 2; ++p) {
					const Voice &pv = v[pairSlots[(size_t)p]];
					const MT32Emu::Bit32u a = kAmpFull - (MT32Emu::Bit32u(pv.ampTarget) << 18);
					const MT32Emu::Bit32u c = pv.isPcm ? (240u << 18)
					                                   : (MT32Emu::Bit32u(pv.cutoff) << 18);
					pair.generateNextSample(kRoles[p], a, pv.pitch, c);
				}
				outPair[(size_t)i] = pair.nextOutSample();
				peakPair = std::max(peakPair, std::abs(outPair[(size_t)i]));
			}
			// Печатается с запасом знаков намеренно: кольцевая модуляция ПЕРЕМНОЖАЕТ два
			// сигнала, и произведение двух тихих партиалов на четырёх знаках выглядит нулём,
			// хотя нулём не является. Один раз это уже чуть не прочиталось как «не работает».
			std::printf("  пик пары: %.8f\n", double(peakPair));
			const juce::File wavPair =
				outDir.getChildFile("la32_pair_note" + juce::String(note) + ".wav");
			juce::AudioBuffer<float> bufPair(1, nPair);
			std::memcpy(bufPair.getWritePointer(0), outPair.data(), sizeof(float) * (size_t)nPair);
			wavPair.deleteFile();
			juce::WavAudioFormat fmtPair;
			std::unique_ptr<juce::FileOutputStream> stPair(wavPair.createOutputStream());
			if (stPair != nullptr) {
				std::unique_ptr<juce::AudioFormatWriter> wrPair(
					fmtPair.createWriterFor(stPair.get(), kChipRate, 1, 16, {}, 0));
				if (wrPair != nullptr) {
					stPair.release();
					wrPair->writeFromAudioSampleBuffer(bufPair, 0, nPair);
				}
			}
			std::printf("  записано: %s\n", wavPair.getFullPathName().toRawUTF8());
		}
	}

	if (synthSlot < 0) { std::printf("\nсинтетического партиала в этой ноте нет\n"); return 0; }

	// Прогон через модель микросхемы. Амплитуда и срез держатся постоянными: здесь
	// проверяется разбор высоты и формы волны, а огибающие уже проверены отдельно.
	const Voice &vv = v[synthSlot];
	MT32Emu::LA32FloatWaveGenerator wg;
	wg.initSynth(vv.sawtooth, vv.pulseWidth, uint8_t(vv.resonance ? vv.resonance : 1));
	// Генератор ждёт не уровень, а его ЛОГАРИФМИЧЕСКОЕ ДОПОЛНЕНИЕ: у munt в Partial.cpp
	// стоит `ampRampVal = 67117056 - ampRamp.nextValue()`, и дальше `amp = 2^(-ampVal/2^22)`,
	// то есть большее число значит тише. Регистр же несёт именно УРОВЕНЬ, и подавать его
	// напрямую - значит вывернуть громкость наизнанку: полный уровень 255 дал бы тишину.
	// Шаг единицы уровня выходит 2^(1/16), то есть около 0.376 дБ.
	const MT32Emu::Bit32u amp = kAmpFull - (MT32Emu::Bit32u(vv.ampTarget) << 18);
	const MT32Emu::Bit32u cutoff = MT32Emu::Bit32u(vv.cutoff) << 18;

	const int samples = int(kChipRate * 0.5);
	std::vector<float> out((size_t)samples, 0.0f);
	for (int i = 0; i < samples; ++i)
		out[(size_t)i] = wg.generateNextSample(amp, vv.pitch, cutoff);

	// --- то же, но с ЖИВОЙ огибающей -------------------------------------------------
	// Выше амплитуда держалась постоянной, чтобы проверить высоту и громкость по отдельности.
	// Здесь в рендер подаётся то, что прошивка на самом деле велела микросхеме делать во
	// времени: ступени рампы по мере их прихода. Закон движения тот же, что в D110Core.
	{
		struct Ev { double ms; uint8_t inc, target; };
		std::vector<Ev> events;
		uint8_t pendingInc = 0;
		const uint16_t evenAddr = uint16_t(bankOf[synthSlot] + 2 * synthSlot);
		for (const auto &w : writes) {
			if (w.addr == evenAddr) pendingInc = w.value;
			else if (w.addr == uint16_t(evenAddr + 1)) events.push_back({w.ms, pendingInc, w.value});
		}
		const double t0 = events.empty() ? 0.0 : events.front().ms;

		MT32Emu::LA32FloatWaveGenerator wg2;
		wg2.initSynth(vv.sawtooth, vv.pulseWidth, uint8_t(vv.resonance ? vv.resonance : 1));
		const int n2 = int(kChipRate * 2.5);
		std::vector<float> out2((size_t)n2, 0.0f);
		double current = 0.0, increment = 0.0, target = 0.0;
		bool descending = false, running = false;
		size_t next = 0;
		double landedAtMs = -1.0;
		for (int i = 0; i < n2; ++i) {
			const double ms = 1000.0 * i / kChipRate;
			while (next < events.size() && events[next].ms - t0 <= ms) {
				const Ev &e = events[next++];
				target = double(e.target) * double(1 << 18);
				if (e.inc == 0) { running = false; continue; }
				if (e.inc == 0xFF) { current = target; running = false; continue; }
				const double large = std::pow(2.0, (double(e.inc & 0x7F) + 24.0) / 8.0);
				descending = (e.inc & 0x80) != 0;
				increment = descending ? large + 1.0 : large;
				running = !((descending && current <= target) || (!descending && current >= target));
				if (!running) current = target;
			}
			if (running) {
				current += descending ? -increment : increment;
				if (descending ? current <= target : current >= target) {
					current = target;
					running = false;
					if (landedAtMs < 0.0) landedAtMs = ms;
				}
			}
			const MT32Emu::Bit32u a =
				kAmpFull - MT32Emu::Bit32u(std::min(current, double(kAmpFull)));
			out2[(size_t)i] = wg2.generateNextSample(a, vv.pitch, cutoff);
		}

		// Огибающая печатается по огибающей ЗВУКА, а не по внутреннему счётчику: иначе
		// проверялось бы, что переменная равна сама себе.
		std::printf("\n  огибающая рендера (пик по 25 мс, дБ от максимума):\n   ");
		double top = 0.0;
		std::vector<double> env;
		for (int b = 0; b + int(kChipRate / 40) <= n2; b += int(kChipRate / 40)) {
			double p = 0.0;
			for (int i = b; i < b + int(kChipRate / 40); ++i) p = std::max(p, std::abs(double(out2[(size_t)i])));
			env.push_back(p);
			top = std::max(top, p);
		}
		for (size_t i = 0; i < env.size(); i += 4)
			std::printf(" %.0f:%.0f", 25.0 * double(i), top > 0 ? 20.0 * std::log10(std::max(env[i], 1e-9) / top) : 0.0);
		std::printf("\n  ступеней рампы за ноту: %zu", events.size());
		for (const auto &e : events)
			std::printf(" | %.0f мс: цель %d, приращение %02X", e.ms - t0, e.target, e.inc);
		std::printf("\n");

		const juce::File wav2 = outDir.getChildFile("la32_env_note" + juce::String(note) + ".wav");
		juce::AudioBuffer<float> buf2(1, n2);
		std::memcpy(buf2.getWritePointer(0), out2.data(), sizeof(float) * (size_t)n2);
		wav2.deleteFile();
		juce::WavAudioFormat fmt2;
		std::unique_ptr<juce::FileOutputStream> st2(wav2.createOutputStream());
		if (st2 != nullptr) {
			std::unique_ptr<juce::AudioFormatWriter> w2(
				fmt2.createWriterFor(st2.get(), kChipRate, 1, 16, {}, 0));
			if (w2 != nullptr) { st2.release(); w2->writeFromAudioSampleBuffer(buf2, 0, n2); }
		}
		std::printf("  с огибающей: %s\n", wav2.getFullPathName().toRawUTF8());
	}

	// Меряется установившийся кусок, без первых миллисекунд.
	const std::vector<float> steady(out.begin() + samples / 4, out.end());
	const double got = fundamentalHz(steady, kChipRate);
	const double want440 = noteHz(note, 440.0), want442 = noteHz(note, 442.0);
	float peak = 0.0f;
	for (float x : steady) peak = std::max(peak, std::abs(x));

	std::printf("\n  нота %d, слот %d (синтетический)\n", note, synthSlot);
	std::printf("  основной тон:      %8.2f Гц\n", got);
	std::printf("  ожидается при 440: %8.2f Гц   при 442: %8.2f Гц\n", want440, want442);
	if (got > 0.0) {
		const double cents = 1200.0 * std::log2(got / want442);
		std::printf("  расхождение с 442: %+8.1f цента   (октав: %+.2f)\n", cents, cents / 1200.0);
	}
	// Пик проверяется не «на глаз», а против того, что предсказывает сам закон громкости:
	// каждая единица уровня - 2^(1/16). Если разбор уровня верен, предсказание и измерение
	// обязаны сойтись с точностью до формы волны (у неё пик всегда ниже полной шкалы).
	const double predicted = std::pow(2.0, -double(kAmpFull - (MT32Emu::Bit32u(vv.ampTarget) << 18))
	                                            / 4194304.0);
	std::printf("  пик: %.4f   потолок по уровню %d: %.4f   ниже потолка на %.1f дБ\n",
	            double(peak), vv.ampTarget, predicted,
	            peak > 0.0f ? 20.0 * std::log10(predicted / double(peak)) : 0.0);

	const juce::File wav = outDir.getChildFile("la32_note" + juce::String(note) + ".wav");
	{
		juce::AudioBuffer<float> buf(1, samples);
		std::memcpy(buf.getWritePointer(0), out.data(), sizeof(float) * (size_t)samples);
		wav.deleteFile();
		juce::WavAudioFormat fmt;
		std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
		if (stream != nullptr) {
			std::unique_ptr<juce::AudioFormatWriter> writer(
				fmt.createWriterFor(stream.get(), kChipRate, 1, 16, {}, 0));
			if (writer != nullptr) {
				stream.release(); // писатель им теперь владеет
				writer->writeFromAudioSampleBuffer(buf, 0, samples);
			}
		}
	}
	std::printf("  записано: %s\n", wav.getFullPathName().toRawUTF8());

	proc.setPoweredOn(false);
	return 0;
}
