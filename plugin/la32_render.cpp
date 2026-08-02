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

// Состояние одного голоса, собранное из записей в регистры.
struct Voice {
	bool seen = false;
	bool isPcm = false;      // 0x0D00 чётный байт, бит 7
	bool sawtooth = false;   // он же, бит 6
	uint8_t resonance = 0;   // 0x0D00 нечётный байт, младшие 5 бит, минус единица
	uint8_t pulseWidth = 0;  // 0x0C40 чётный байт
	uint8_t cutoff = 0;      // 0x0C40 нечётный байт
	uint8_t ampTarget = 0;   // выбранный флагом банк рампы, нечётный байт
	uint16_t pitch = 0;      // 0x0CC0, шестнадцатибитное
};

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

	// Одна нота, короткое окно: нужны значения, которые прошивка положила при выдаче голоса.
	proc.getCore().startSoTrace();
	const uint8_t on[3] = {0x91, uint8_t(note), 100};
	proc.getCore().pushMidi(on, 3);
	render(proc, 0.5);
	proc.getCore().stopSoTrace();
	const uint8_t off[3] = {0x81, uint8_t(note), 0};
	proc.getCore().pushMidi(off, 3);
	const auto writes = proc.getCore().takeSoWrites();
	render(proc, 0.5);

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
		bankOf[s] = v[s].isPcm ? 0x0C00 : 0x0C80;
		const auto res = firstValue.find(uint16_t(0x0D01 + 2 * s));
		if (res != firstValue.end()) v[s].resonance = uint8_t(res->second & 0x1F);
		const auto pw = firstValue.find(uint16_t(0x0C40 + 2 * s));
		if (pw != firstValue.end()) v[s].pulseWidth = pw->second;
		const auto co = firstValue.find(uint16_t(0x0C41 + 2 * s));
		if (co != firstValue.end()) v[s].cutoff = co->second;
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

	std::printf("\n  слот | род      | пила | ширина | срез | резонанс | уровень | высота\n");
	int synthSlot = -1;
	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		if (!v[s].seen) continue;
		std::printf("  %4d | %-8s | %-4s | %6d | %4d | %8d | %7d | %04X\n", s,
		            v[s].isPcm ? "PCM" : "синтез", v[s].sawtooth ? "да" : "нет",
		            v[s].pulseWidth, v[s].cutoff, v[s].resonance, v[s].ampTarget, v[s].pitch);
		if (!v[s].isPcm && synthSlot < 0) synthSlot = s;
	}
	if (synthSlot < 0) { std::printf("\nсинтетического партиала в этой ноте нет\n"); return 1; }

	// Прогон через модель микросхемы. Амплитуда и срез держатся постоянными: здесь
	// проверяется разбор высоты и формы волны, а огибающие уже проверены отдельно.
	const Voice &vv = v[synthSlot];
	MT32Emu::LA32FloatWaveGenerator wg;
	wg.initSynth(vv.sawtooth, vv.pulseWidth, uint8_t(vv.resonance ? vv.resonance : 1));
	const MT32Emu::Bit32u amp = MT32Emu::Bit32u(vv.ampTarget) << 18;
	const MT32Emu::Bit32u cutoff = MT32Emu::Bit32u(vv.cutoff) << 18;

	constexpr double kChipRate = D110Core::kLa32SampleRate;
	const int samples = int(kChipRate * 0.5);
	std::vector<float> out((size_t)samples, 0.0f);
	for (int i = 0; i < samples; ++i)
		out[(size_t)i] = wg.generateNextSample(amp, vv.pitch, cutoff);

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
	std::printf("  пик: %.4f\n", double(peak));

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
