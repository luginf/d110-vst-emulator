// Побайтно сравнивает, что для каждой партии держит ПРОШИВКА и что держит ДВИЖОК, прямо
// во время демо-песни. Всё, что мост не донёс, видно здесь как конкретный расходящийся
// байт и не видно ни с одной из сторон по отдельности.
//
// Снимается ВО ВРЕМЯ демо, потому что демо загружает собственный патч: как партии выглядят
// при загрузке, ничего не говорит о том, как они выглядят на игре.
//
// Штатный результат на «Macho Memory» - расходится ровно байт 0 у партий 6 и 7: прошивка
// держит группу тембра 5, движок 3. Это не потеря в мосту, а предел, объявленный самим
// D-110: таблица максимумов в управляющем ПЗУ даёт группе тембра максимум 3, и запись
// прижимается к нему. Чем это грозит звуку и почему тембр всё равно приходит правильный -
// см. docs/timbre_group_5.md.
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

void press(D110AudioProcessor &proc, std::initializer_list<int> idx, int hold, int settle) {
	for (int i : idx) proc.getCore().setButton(i, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(hold));
	for (int i : idx) proc.getCore().setButton(i, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(settle));
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

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	std::this_thread::sleep_for(std::chrono::seconds(9));
	std::printf("firmware running: %s\n", proc.getCore().isRunning() ? "yes" : "NO");

	press(proc, {D110Core::buttonIndex(1, 7), D110Core::buttonIndex(1, 0)}, 200, 500);
	press(proc, {D110Core::buttonIndex(1, 0)}, 200, 500);

	for (int round = 0; round < 4; ++round) {
		render(proc, 8.0);

		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());

		// Byte for byte, firmware against engine. Roland's PatchTemp is
		// group, timbre, keyShift, fineTune, benderRange, assignMode, reverbSwitch, dummy,
		// then outputLevel and panpot - so a level or a timbre that failed to cross shows up
		// as a specific differing byte rather than as a vague discrepancy.
		std::printf("\n--- after %d s of the demo ---\n", (round + 1) * 8);
		std::printf("  part | grp tmbr kSh fine bnd asg rev  -  LVL PAN | engine same?\n");
		for (int p = 0; p < 8; ++p) {
			const uint8_t *fw = &ram[0x2000 + 16 * p];
			// Движок адресует свою память в УПАКОВАННОМ виде, а не в роландовском виде
			// «три семибитных байта»: MT32EMU_MEMADDR из Structures.h сворачивает
			// 0x030000 в 0x00C000. По роландовскому адресу регион не находится вовсе,
			// readMemory возвращается, не тронув буфер, и всё читается как нули.
			// Поэтому буфер сперва заполняется меткой: молчаливый холостой вызов не
			// должен выглядеть как данные.
			auto packed = [](uint32_t a) {
				return ((a & 0x7f0000u) >> 2) | ((a & 0x7f00u) >> 1) | (a & 0x7fu);
			};
			uint8_t eng[16];
			std::memset(eng, 0xAA, sizeof eng);
			proc.engineReadMemory(packed(0x030000u) + 16u * uint32_t(p), 10, eng);

			std::printf("  %4d |", p + 1);
			for (int i = 0; i < 10; ++i) std::printf(" %3d", fw[i]);
			bool same = true;
			for (int i = 0; i < 10; ++i)
				if (eng[i] != fw[i]) { same = false; break; }
			if (same) {
				std::printf(" | match\n");
			} else {
				std::printf(" | DIFFERS, engine:");
				for (int i = 0; i < 10; ++i)
					std::printf(" %s%d%s", eng[i] != fw[i] ? "[" : "", eng[i],
					            eng[i] != fw[i] ? "]" : "");
				std::printf("\n");
			}
		}
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	std::printf("\ndone\n");
	return 0;
}
