// Куда деваются ноты при быстрой игре: их теряет прибор или их некуда играть?
//
// Жалоба звучит одинаково в обоих случаях - «часть нот не звучит», - а причины
// противоположные, и лечатся они разным. Поэтому зонд считает ноту на ТРЁХ рубежах подряд:
//
//   1. сколько нот отправлено в плагин;
//   2. сколько из них ПРОШИВКА взяла - она сама решает, какой партии играть и хватает ли
//      голосов, и о каждой взятой ноте пишет в свои таблицы, откуда мост их и читает;
//   3. сколько партиалов при этом занято у звукового движка и сколько партий звучит.
//
// Между первым и вторым рубежом стоит заглушка LA32 (D110Core::StuckPolicy::La32Stub):
// микросхему синтеза не эмулирует ни MAME, ни этот проект, и прошивке отвечают за неё. Если
// теряет она, потери видны именно здесь - отправлено больше, чем взято.
//
// Между вторым и третьим - полифония: у D-110 тридцать два партиала на всё, тон стоит от
// одного до четырёх партиалов, и отпущенная нота держит свои партиалы, пока не отзвучит её
// затухание. Тон в четыре партиала - это восемь нот на весь прибор, и это не поломка, а
// свойство машины. Чтобы одно не выдать за другое, каждый прогон идёт ДВАЖДЫ: тоном в два
// партиала и тоном в четыре. Если потери удваиваются вместе с партиалами - дело в
// полифонии; если они одинаковы - дело не в ней.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Партия 1 отвечает на канале 2 у заводского прибора.
constexpr int kChannel = 2;

struct Tally {
	int sent = 0;
	int firmwareStarted = 0;
	int peakPartials = 0;
	int peakVoices = 0;   // сколько голосов прошивка держала одновременно
};

void renderBlocks(D110AudioProcessor &proc, int blocks, juce::MidiBuffer *first = nullptr) {
	juce::AudioBuffer<float> audio(2, kBlock);
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer midi;
		if (b == 0 && first != nullptr) midi = *first;
		proc.processBlock(audio, midi);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

void render(D110AudioProcessor &proc, double seconds) {
	renderBlocks(proc, int(seconds * kSampleRate / kBlock));
}

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

// Сколько голосов прошивка держит прямо сейчас - по её собственной таблице слотов LA32.
int busySlots(const std::vector<uint8_t> &ram) {
	int busy = 0;
	for (int s = 0; s < D110Core::kNumHardwareVoices; ++s) {
		const size_t at = size_t(D110Core::kSlotStateTable) + size_t(s) * 2;
		if (at >= ram.size()) continue;
		const uint8_t v = ram[at];
		if (v == D110Core::kSlotBusyValue || v == D110Core::kSlotBusyValueAlt) ++busy;
	}
	return busy;
}

// Один прогон: `count` нот подряд, по `noteMs` каждая, с промежутком `gapMs`.
Tally play(D110AudioProcessor &proc, int count, int noteMs, int gapMs) {
	Tally t;
	const uint64_t startedBefore = proc.getCore().firmwareNoteOns();

	for (int i = 0; i < count; ++i) {
		const int note = 48 + (i % 13);
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kChannel, note, 0.9f), 0);
		renderBlocks(proc, juce::jmax(1, int(double(noteMs) * kSampleRate / (kBlock * 1000.0))),
		             &on);
		++t.sent;
		t.peakPartials = std::max(t.peakPartials, proc.engineActivePartials());
		t.peakVoices = std::max(t.peakVoices, busySlots(snapshot(proc)));

		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kChannel, note), 0);
		renderBlocks(proc, juce::jmax(1, int(double(gapMs) * kSampleRate / (kBlock * 1000.0))),
		             &off);
	}
	render(proc, 1.5);   // дать затуханиям отзвучать
	t.firmwareStarted = int(proc.getCore().firmwareNoteOns() - startedBefore);
	return t;
}

// Ставит партии 1 тон по группе и номеру - это два байта её записи в Timbre Temporary.
void setPartTone(D110AudioProcessor &proc, int group, int number) {
	proc.sendTimbreTempParam(0, 0, uint8_t(group));
	proc.sendTimbreTempParam(0, 1, uint8_t(number));
	render(proc, 1.2);
}

void report(const char *what, const Tally &t, int partialsPerNote) {
	const int lost = t.sent - t.firmwareStarted;
	std::printf("  %-34s отправлено %3d, прошивка взяла %3d%s   пик: партиалов %2d, "
	            "голосов %2d   потолок по партиалам ~%d нот\n",
	            what, t.sent, t.firmwareStarted,
	            lost > 0 ? "  <-- ПОТЕРЯ" : "            ", t.peakPartials, t.peakVoices,
	            partialsPerNote > 0 ? 32 / partialsPerNote : 0);
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s   движок: %s   партиалов у движка: %d\n\n",
	            proc.getCore().isRunning() ? "работает" : "НЕТ",
	            proc.engineIsOpen() ? "открыт" : "НЕТ",
	            int(proc.enginePartialCount()));
	if (!proc.getCore().isRunning() || !proc.engineIsOpen()) return 1;

	// La32Ramps + правильная кодировка байта состояния (slot+1, docs/la32_register_map.md)
	// доводит счётчик ступени eec0[voice] до 7 и вправду освобождает слот
	// (plugin/slot_life_probe.cpp). Проверяем здесь, что это чинит именно полифонию, а не
	// только сам факт освобождения таблицы.
	proc.getCore().setStuckPolicy(D110Core::StuckPolicy::La32Ramps);
	proc.getCore().setLa32StatusMode(1);
	std::printf("политика: La32Ramps, режим байта состояния = 1 (слот+1)\n\n");

	// Два тона с ЗАВЕДОМО разным числом партиалов, по ламинированной карточке Preset Tones:
	// a02 «Acou Piano 2» - два партиала, b01 «Fantasy» - четыре. Это и есть контроль: если
	// потери от полифонии, они обязаны быть разными; если от заглушки - одинаковыми.
	struct Case { const char *name; int group, number, partials; };
	const Case kCases[] = {
		{ "a02 Acou Piano 2 (2 партиала)", 0, 1, 2 },
		{ "b01 Fantasy (4 партиала)",      1, 0, 4 },
		// Третий случай - ВНУТРЕННИЙ тон, группа 2 «i INTERNAL». Заводские группы a и b
		// лежат в ПЗУ и одинаковы у всех, а эта память набивается банком со стороны, и её
		// огибающие - чужие. Жалоба «ноты длятся и не затухают» пришла именно тогда, когда
		// в этой памяти впервые появился банк, так что проверять её надо отдельно: у зонда
		// до сих пор не было ни одного случая, где тон брался бы не из ПЗУ.
		//
		// Число партиалов у залитого тона заранее неизвестно, поэтому в графе «потолок»
		// стоит 0 - считать его не по чему, и выдумывать нечего. Смотреть надо на другое:
		// сходятся ли принятые ноты с отпущенными и падают ли партиалы к нулю в покое.
		{ "i01 (внутренний, из залитого банка)", 2, 0, 0 },
	};

	for (const Case &c : kCases) {
		std::printf("=== %s ===\n", c.name);
		setPartTone(proc, c.group, c.number);

		// Медленно: нота 250 мс, пауза 250 мс. Так на приборе никто ничего не теряет, и это
		// нижняя граница - если теряется ЗДЕСЬ, дело не в полифонии вовсе.
		report("медленно, 2 ноты в секунду", play(proc, 12, 250, 250), c.partials);
		// Быстро: 100 мс нота, 20 мс пауза - примерно восемь нот в секунду, темп пассажа.
		report("быстро, ~8 нот в секунду", play(proc, 24, 100, 20), c.partials);
		// И внахлёст: ноты не отпускаются, пока не набрано восемь, - так партиалы кончаются
		// гарантированно, и видно, на каком голосе прибор начинает воровать.
		{
			Tally t;
			const uint64_t before = proc.getCore().firmwareNoteOns();
			for (int i = 0; i < 10; ++i) {
				juce::MidiBuffer on;
				on.addEvent(juce::MidiMessage::noteOn(kChannel, 48 + i * 2, 0.9f), 0);
				renderBlocks(proc, 6, &on);
				++t.sent;
				t.peakPartials = std::max(t.peakPartials, proc.engineActivePartials());
				t.peakVoices = std::max(t.peakVoices, busySlots(snapshot(proc)));
			}
			juce::MidiBuffer off;
			for (int i = 0; i < 10; ++i)
				off.addEvent(juce::MidiMessage::noteOff(kChannel, 48 + i * 2), 0);
			renderBlocks(proc, 4, &off);
			render(proc, 2.0);
			t.firmwareStarted = int(proc.getCore().firmwareNoteOns() - before);
			report("аккорд из 10 внахлёст", t, c.partials);
		}
		std::printf("\n");
	}

	// --- кто именно упирается: движок или прошивка ----------------------------
	//
	// Резерв партиалов есть у обоих. Прошивка раздаёт по нему свои голоса, а движок - свои
	// партиалы, и байты у них ОДНИ И ТЕ ЖЕ: системная область переносится зеркалом. Значит
	// поднять резерв обычным путём - значит поднять его сразу у двоих, и по такому опыту не
	// скажешь, кто мешал.
	//
	// Поэтому опыт ставится дважды. Сперва резерв поднимается ТОЛЬКО У ДВИЖКА, минуя
	// прошивку (engineWriteSysexForTest - для того он и есть), потом обычным путём, у обоих.
	// Если пик партиалов вырастет от первого - предел ставил движок; если только от
	// второго - прошивка.
	{
		auto chord = [&proc](const char *what) {
			int peak = 0;
			for (int i = 0; i < 10; ++i) {
				juce::MidiBuffer on;
				on.addEvent(juce::MidiMessage::noteOn(kChannel, 48 + i * 2, 0.9f), 0);
				renderBlocks(proc, 6, &on);
				peak = std::max(peak, proc.engineActivePartials());
			}
			juce::MidiBuffer off;
			for (int i = 0; i < 10; ++i)
				off.addEvent(juce::MidiMessage::noteOff(kChannel, 48 + i * 2), 0);
			renderBlocks(proc, 4, &off);
			render(proc, 2.0);
			std::printf("  %-46s пик партиалов %2d\n", what, peak);
			return peak;
		};

		std::printf("=== КТО СТАВИТ ПРЕДЕЛ ===\n");
		setPartTone(proc, 0, 1);            // тон в два партиала: десять нот это двадцать
		chord("как есть, заводской резерв 4 4 4 4 3 3 3 2 5");

		// Резерв только в движке: партии 1 все тридцать два, остальным по нулю.
		{
			uint8_t data[9] = { 32, 0, 0, 0, 0, 0, 0, 0, 0 };
			uint8_t msg[D110Core::kMaxSysexBytes];
			const int n = D110Core::buildDt1Message(D110Core::kSysexSystem, 4, data, 9, msg);
			if (n > 0) proc.engineWriteSysexForTest(msg, n);
			render(proc, 0.5);
		}
		chord("резерв 32 ТОЛЬКО у движка");

		// А теперь обычным путём - через прошивку, как это делает редактор.
		for (int i = 0; i < 9; ++i) proc.sendSystemParam(4 + i, i == 0 ? 32 : 0);
		render(proc, 1.5);
		chord("резерв 32 у прошивки И у движка");

		// Вернуть заводской резерв. Девять значений связаны суммой 32, поэтому они уходят
		// ОДНИМ сообщением: по одному прибор их отвергнет, и прибор при этом прав.
		const uint8_t factory[9] = { 4, 4, 4, 4, 3, 3, 3, 2, 5 };
		proc.sendAreaData(D110Core::kSysexSystem, 4, factory, 9);
		render(proc, 1.5);
		std::printf("  резерв возвращён к заводскому 4 4 4 4 3 3 3 2 5\n");
	}
	std::printf("\n");

	// Что осталось висеть после всего. Занятый слот при отпущенных клавишах - это утечка
	// голосов, и она бы объясняла «со временем начинает есть ноты» куда лучше полифонии.
	render(proc, 3.0);
	const auto ram = snapshot(proc);
	std::printf("=== после всего, при отпущенных клавишах ===\n");
	std::printf("  занятых слотов у прошивки: %d из %d\n", busySlots(ram),
	            D110Core::kNumHardwareVoices);
	std::printf("  партиалов у движка: %d\n", proc.engineActivePartials());
	std::printf("  нот принято прошивкой всего: %llu, отпущено: %llu\n",
	            (unsigned long long)proc.getCore().firmwareNoteOns(),
	            (unsigned long long)proc.getCore().firmwareNoteOffs());

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
