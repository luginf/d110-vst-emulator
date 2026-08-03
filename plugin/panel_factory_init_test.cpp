// Доходит ли документированная процедура факт-сброса ("POWER off, Ctrl+click WRITE/COPY,
// POWER on, ENTER") до прошивки, если её выполнить РЕАЛЬНЫМИ нажатиями по настоящей панели -
// а не вызовом D110Core::setButton() в обход неё, как делает coldstart_test.cpp.
//
// Владелец сообщил, что эта процедура у него не срабатывала никогда, ни разу. Разбор кода
// нашёл причину: D110Panel::setButtonState() до этой правки выходила сразу же, пока
// `!core.isRunning()` - то есть ровно тогда, когда процедура просит защёлкнуть кнопку. Ctrl-
// клик отражался ранним выходом, core.setButton() не вызывался вовсе, и защёлкивался только
// локальный флаг панели, ничего не значащий для настоящей матрицы опроса. Прибор включался
// "чистым" и никогда не видел кнопку зажатой.
//
// Этот стенд жмёт РОВНО те же пять шагов из README - настоящими событиями мыши по настоящему
// D110Panel::mouseDown/mouseUp, с модификатором Ctrl там, где нужна защёлка, - и проверяет
// результат тем же способом, что и plugin/nvram_recovery.cpp: резерв партиалов должен
// собраться в 32, а не остаться тем, что было.
#include "Source/PluginEditor.h"
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;

// Координаты кнопок - те же константы, что в самой панели (см. PluginEditor.cpp).
constexpr float kWriteCopyX = 1473.0f + 63.0f / 2.0f, kWriteCopyY = 80.0f + 26.0f / 2.0f;
constexpr float kEnterX = 1473.0f + 63.0f / 2.0f, kEnterY = 168.0f + 26.0f / 2.0f;
constexpr float kPowerX = 1913.0f + 85.0f / 2.0f, kPowerY = 96.0f + 106.0f / 2.0f;

void renderBlocks(D110AudioProcessor &proc, int blocks) {
	juce::AudioBuffer<float> audio(2, kBlock);
	for (int b = 0; b < blocks; ++b) {
		audio.clear();
		juce::MidiBuffer none;
		proc.processBlock(audio, none);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
}

void render(D110AudioProcessor &proc, double seconds) {
	renderBlocks(proc, int(seconds * kSampleRate / kBlock));
}

// Настоящее событие мыши по настоящей панели - тот же приём, что в panel_render.cpp.
// `holdMs` - сколько РЕАЛЬНОГО времени проходит между нажатием и отпусканием: у мгновенного
// клика (0) это одна и та же миллисекунда, а машина крутится на СВОЁМ потоке и может просто
// не успеть между ними ни разу провернуть цикл опроса. factoryReset() держит Enter 400 мс -
// столько же нужно и здесь для честного сравнения.
void click(D110Panel &panel, juce::Point<float> p, juce::ModifierKeys mods, int holdMs = 0) {
	const juce::MouseEvent down(juce::Desktop::getInstance().getMainMouseSource(), p, mods, 1.0f,
	                            0.0f, 0.0f, 0.0f, 0.0f, &panel, &panel,
	                            juce::Time::getCurrentTime(), p, juce::Time::getCurrentTime(),
	                            1, false);
	panel.mouseDown(down);
	if (holdMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
	const juce::MouseEvent up(juce::Desktop::getInstance().getMainMouseSource(), p,
	                          juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &panel, &panel,
	                          juce::Time::getCurrentTime(), p, juce::Time::getCurrentTime(), 1,
	                          false);
	panel.mouseUp(up);
}

int reserveSum(D110AudioProcessor &proc) {
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	if (!proc.getCore().getRam(ram.data())) return -1;
	int sum = 0;
	for (int i = 0; i < 9; ++i) sum += ram[0x2D98 + i];
	return sum;
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);   // включается сам, по новому умолчанию
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}

	// Портим ЗАВЕДОМО УЗНАВАЕМО, а не число, которое и так могло совпасть: переименовываем
	// патч 1. "Сумма = 32" ничего не докажет, если резерв уже был 32 (так и вышло с первой
	// попыткой этого стенда - подмена в системную область не зацепилась, и "после == 32"
	// было не доказательством, а совпадением). Имя не может взяться ниоткуда, кроме как
	// после настоящего прогона заводского ПЗУ.
	proc.sendName(D110Core::kSysexPatches, 0, "SABOTAGE!");
	render(proc, 1.5);
	{
		std::vector<uint8_t> ram(D110Core::kRamSize, 0);
		proc.getCore().getRam(ram.data());
		std::printf("патч 1 переименован нарочно: \"%.10s\"\n",
		            reinterpret_cast<const char *>(ram.data()));
	}

	D110Panel panel(proc);
	panel.setSize(D110Panel::kRefW, D110Panel::kRefH);

	std::printf("\nвыполняю пять шагов из README настоящими нажатиями по панели...\n");

	std::printf("1. POWER off\n");
	click(panel, { kPowerX, kPowerY }, juce::ModifierKeys());
	render(proc, 1.0);
	std::printf("   isRunning = %s (ожидается: no)\n", proc.getCore().isRunning() ? "yes" : "no");

	std::printf("2. Ctrl+click WRITE/COPY (защёлкнуть, пока выключен)\n");
	click(panel, { kWriteCopyX, kWriteCopyY }, juce::ModifierKeys::ctrlModifier);

	std::printf("3. POWER on\n");
	click(panel, { kPowerX, kPowerY }, juce::ModifierKeys());
	render(proc, 9.0); // дать прошивке подняться и увидеть кнопку зажатой
	std::printf("   isRunning = %s\n", proc.getCore().isRunning() ? "yes" : "no");

	// ПОРЯДОК ПЕРЕСТАВЛЕН относительно README и относительно первой версии этого стенда.
	// D110Core::factoryReset() (доказанно рабочая реализация той же процедуры изнутри)
	// отпускает Write/Copy, ждёт 800 мс и ТОЛЬКО ПОТОМ нажимает Enter - а не наоборот.
	// Первый прогон этого стенда, в буквальном порядке из README (Enter, потом отпустить
	// Write/Copy), с честным контролем (различимое имя патча) НЕ восстановил ничего - имя
	// осталось "SABOTAGE!". Проверяем здесь, действительно ли порядок и есть недостающая
	// часть, а не только снятая защита в setButtonState.
	std::printf("4. Ctrl+click WRITE/COPY (отпустить защёлку)\n");
	click(panel, { kWriteCopyX, kWriteCopyY }, juce::ModifierKeys::ctrlModifier);
	render(proc, 1.0);

	std::printf("5. click ENTER (подтвердить, держим 400 мс - как factoryReset())\n");
	click(panel, { kEnterX, kEnterY }, juce::ModifierKeys(), 400);
	render(proc, 6.0); // дать заводскому сбросу дописать банки

	const int after = reserveSum(proc);
	std::vector<uint8_t> ram(D110Core::kRamSize, 0);
	proc.getCore().getRam(ram.data());
	const std::string patchName(reinterpret_cast<const char *>(ram.data()), 10);
	std::printf("\nрезерв после процедуры: сумма = %d (заводская: 32)\n", after);
	std::printf("патч 1 после процедуры: \"%s\" (заводское: \"Patch   01\")\n",
	            patchName.c_str());

	const bool ok = (after == 32) && (patchName == "Patch   01");
	std::printf("\n%s\n", ok ? "*** ПРОЦЕДУРА С ПАНЕЛИ РАБОТАЕТ ***"
	                        : "*** ПРОЦЕДУРА С ПАНЕЛИ НЕ СРАБОТАЛА ***");

	proc.setPoweredOn(false);
	proc.releaseResources();
	return ok ? 0 : 1;
}
