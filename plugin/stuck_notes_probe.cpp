// «Нажимаю на всех партиях ноты, нота залипает и не отходит».
//
// Все прежние прогоны d110_polyphony шли по ОДНОЙ партии, на канале 2, и залипания не
// показали ни разу: 168 нот принято, 167 отпущено, в покое ноль слотов и ноль партиалов.
// Разница, которую называет владелец, - именно во множестве партий сразу, и её ни один
// стенд до сих пор не воспроизводил.
//
// Почему это может быть не всё равно. Снятие ноты в движке рождается ДВУМЯ путями
// (D110Core.cpp, releaseContext): прошивка пометила голос возвращённым - f460 бит 6, - либо
// контекст отдали новой ноте, пока старая звучала, то есть голос украли. Пока слоты
// текли, воровство шло постоянно и второй путь работал за первый. Теперь слоты
// возвращаются, воровства нет, и весь груз лёг на f460. На одной партии он держит; вопрос
// в том, держит ли на девяти, когда контекстов разбирают вдесятеро больше.
//
// Меряется накоплением по кругам, а не одним снимком: залипание - это то, что НЕ уходит,
// поэтому единственный честный признак - остаток при отпущенных клавишах, и он должен
// расти от круга к кругу, если жалоба верна.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Партия N отвечает на канале N+1 у заводского прибора, партия 9 - ритм на канале 10.
constexpr int kFirstChannel = 2;
constexpr int kNumParts = 9;

void render(D110AudioProcessor &proc, double seconds, juce::MidiBuffer *midi = nullptr) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const int blocks = int(seconds * kSampleRate / kBlock);
	for (int b = 0; b < blocks; ++b) {
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, (b == 0 && midi) ? *midi : none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

std::vector<uint8_t> snapshot(D110AudioProcessor &proc) {
	std::vector<uint8_t> v(D110Core::kRamSize, 0);
	proc.getCore().getRam(v.data());
	return v;
}

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

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}
	proc.setForwardNotesToFirmware(true);
	std::printf("прошивка: работает   партиалов у движка: %d\n", proc.engineActivePartials());
	std::printf("политика: %s\n\n", "шаблонная (La32Ramps + режим 1)");

	std::printf("круг | взято | отпущено | разница | слотов | партиалов\n");
	std::printf("     |       |          | нарастающим итогом при ОТПУЩЕННЫХ клавишах\n");

	for (int round = 1; round <= 6; ++round) {
		// Аккорд по всем девяти партиям разом - именно так, как описана жалоба. Ноты
		// разные у каждой партии, чтобы прошивке негде было счесть их повтором и погасить
		// старую вместо выдачи новой.
		juce::MidiBuffer on;
		for (int p = 0; p < kNumParts; ++p)
			on.addEvent(juce::MidiMessage::noteOn(kFirstChannel + p, 48 + p * 2, 0.9f), 0);
		render(proc, 1.0, &on);

		juce::MidiBuffer off;
		for (int p = 0; p < kNumParts; ++p)
			off.addEvent(juce::MidiMessage::noteOff(kFirstChannel + p, 48 + p * 2), 0);
		render(proc, 0.2, &off);

		// Три секунды тишины ПОСЛЕ снятия: у любого честного затухания этого с запасом
		// хватает, а залипшая нота столько же и останется висеть.
		render(proc, 3.0);

		const uint64_t ons = proc.getCore().firmwareNoteOns();
		const uint64_t offs = proc.getCore().firmwareNoteOffs();
		std::printf("  %2d | %5llu | %8llu | %7lld | %6d | %9d\n", round,
		            (unsigned long long)ons, (unsigned long long)offs,
		            (long long)ons - (long long)offs, busySlots(snapshot(proc)),
		            proc.engineActivePartials());
	}

	std::printf("\nчитается так: разница и партиалы обязаны стоять на месте от круга к кругу.\n"
	            "Если они РАСТУТ - ноты вправду не заканчиваются, и жалоба воспроизведена.\n");

	// Разведение по одной партии. Общий прогон выше показывает, СКОЛЬКО не закончилось, но
	// не говорит, у кого именно, - а девять партий устроены не одинаково: восьмая это ритм,
	// где удар односторонний и снятие приходит не так, как у клавишной партии. Пока не
	// известно, какая партия оставляет остаток, любая правка будет наугад.
	std::printf("\n=== по одной партии ===\n");
	std::printf("партия | канал | взято | отпущено | разница | партиалов после\n");
	for (int p = 0; p < kNumParts; ++p) {
		const uint64_t onsBefore = proc.getCore().firmwareNoteOns();
		const uint64_t offsBefore = proc.getCore().firmwareNoteOffs();

		// Три ноты подряд, каждая со своим снятием - меньше не покажет повторяемости,
		// больше не нужно, потому что остаток виден уже на первой.
		for (int i = 0; i < 3; ++i) {
			juce::MidiBuffer on;
			on.addEvent(juce::MidiMessage::noteOn(kFirstChannel + p, 48 + i * 3, 0.9f), 0);
			render(proc, 0.6, &on);
			juce::MidiBuffer off;
			off.addEvent(juce::MidiMessage::noteOff(kFirstChannel + p, 48 + i * 3), 0);
			render(proc, 0.2, &off);
		}
		render(proc, 3.0);

		const long long ons = (long long)(proc.getCore().firmwareNoteOns() - onsBefore);
		const long long offs = (long long)(proc.getCore().firmwareNoteOffs() - offsBefore);
		std::printf("  %4d | %5d | %5lld | %8lld | %7lld | %14d  %s\n", p + 1, kFirstChannel + p,
		            ons, offs, ons - offs, proc.engineActivePartials(),
		            (ons != offs) ? "<-- ОСТАТОК" : "");
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
