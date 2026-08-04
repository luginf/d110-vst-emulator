// «Живая Timbre Temporary читается сплошным центром сразу после включения, ДО первого
// нажатия кнопки» - но это могло быть накопленным следом сегодняшних опытов в реальной
// памяти прибора, а не свойством самой прошивки. Единственный чистый способ разделить два
// объяснения - девственная НВР во ВРЕМЕННОЙ папке, не задевая настоящий файл владельца ни
// байтом. D110Core берёт путь к НВР явным аргументом (как в core_test.cpp) - реальный путь
// D110AudioProcessor::getNvramRoot() сюда не участвует вовсе.
#include "Source/D110Core.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

const char *kRomPath =
	"C:\\Users\\bd260\\Downloads\\MAME 0.288 ROMs (non-merged);"
	"C:\\Users\\bd260\\Downloads\\MAME_0.288_ROMs_[merged]";

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const std::string nvram = "virgin_boot_probe_nvram";
	std::printf("девственная НВР во временной папке: %s\n", nvram.c_str());

	D110Core core;
	core.start(kRomPath, nvram);
	for (int i = 0; i < 12 && !core.isRunning(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	std::printf("прошивка: %s\n", core.isRunning() ? "работает" : "НЕ ЗАПУСТИЛАСЬ");
	if (!core.isRunning()) return 1;

	// setPoweredOn(true) в плагине делает ровно это на девственной памяти: сразу
	// заводской сброс, без единого нажатия пользователя.
	std::printf("девственная память - выполняю заводской сброс, как это делает плагин...\n");
	core.factoryReset();
	while (core.isResetting()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
	std::this_thread::sleep_for(std::chrono::seconds(2));

	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	core.getRam(ram.data());
	std::printf("\n=== СРАЗУ ПОСЛЕ заводского сброса, ни одной кнопки пользователем ===\n");
	std::printf("текущий патч (0x2DB9): %d\n", int(ram[(size_t)D110Core::kRamPatchNumber]));
	std::printf("живая панорама партий: ");
	for (int part = 0; part < 8; ++part)
		std::printf("%d ", ram[(size_t)D110Core::kRamTimbreTemp + 16 * part + 9]);
	std::printf("\nданные патча 0, панорама:      ");
	for (int part = 0; part < 8; ++part)
		std::printf("%d ", ram[31 + part * 12 + 9]);
	std::printf("\nожидается: 4 10 6 8 2 12 0 14\n");

	core.stop();
	return 0;
}
