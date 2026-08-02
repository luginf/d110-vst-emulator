// Снимает НАСТОЯЩУЮ панель плагина в PNG, чтобы её было на что посмотреть рядом с
// фотографией прибора, а не судить о ней по константам. Рисует ту самую D110Panel, которую
// видит пользователь, - в отличие от d110_lcd_check, который повторяет цикл отрисовки у себя
// и потому годится только для подбора чисел.
//
// Карту двигает не переменная, а щелчок по щели: так проверяется весь путь целиком, от
// попадания мышью до кадра. Ход доводится вызовами таймера панели, поэтому цикл сообщений
// не нужен.
//
// Usage: d110_panel_render [output_dir]
#include "Source/PluginEditor.h"
#include "Source/PluginProcessor.h"

#include <cstdio>

namespace {

// Обрамление щели - та же область, по которой панель ловит щелчок.
constexpr float kSlotX = 1588.0f, kSlotY = 109.0f, kSlotW = 260.0f, kSlotH = 52.0f;

void save(juce::Component &c, const juce::File &out, juce::Rectangle<int> area, int scale) {
	const juce::Image shot = c.createComponentSnapshot(area, false, float(scale));
	juce::PNGImageFormat png;
	out.deleteFile();
	std::unique_ptr<juce::FileOutputStream> stream(out.createOutputStream());
	if (stream != nullptr) png.writeImageToStream(shot, *stream);
	std::printf("  %-22s %dx%d -> %s\n", out.getFileNameWithoutExtension().toRawUTF8(),
	            shot.getWidth(), shot.getHeight(), out.getFullPathName().toRawUTF8());
}

// Дать анимации проехать: карту двигает собственный таймер панели, поэтому нужен настоящий
// цикл сообщений, а не вызов обработчика напрямую (он и закрыт - таймер у панели приватный).
void settle(int ms) {
	juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
}

void clickSlot(D110Panel &panel) {
	const juce::Point<float> p(kSlotX + kSlotW * 0.5f, kSlotY + kSlotH * 0.5f);
	const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), p,
	                         juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	                         &panel, &panel, juce::Time::getCurrentTime(), p,
	                         juce::Time::getCurrentTime(), 1, false);
	panel.mouseDown(e);
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const juce::File dir = (argc > 1) ? juce::File(juce::String(argv[1]))
	                                  : juce::File::getCurrentWorkingDirectory();
	dir.createDirectory();
	std::printf("writing to %s\n", dir.getFullPathName().toRawUTF8());

	D110AudioProcessor proc;
	D110Panel panel(proc);
	panel.setSize(D110Panel::kRefW, D110Panel::kRefH);

	// Крупный план щели с запасом вниз, чтобы было видно и выехавшую карту.
	const juce::Rectangle<int> slot(1560, 95, 340, 161);
	const juce::Rectangle<int> whole(0, 0, D110Panel::kRefW, D110Panel::kRefH);

	save(panel, dir.getChildFile("card_00_inserted.png"), slot, 4);
	save(panel, dir.getChildFile("card_00_inserted_panel.png"), whole, 1);

	// Раскадровка через равные промежутки, а не в трёх выбранных точках: по ней видно и как
	// карта выглядит, и сколько времени занимает ход. Шаг в 100 мс - примерно та частота, с
	// какой глаз успевает разобрать движение.
	clickSlot(panel);
	for (int i = 1; i <= 11; ++i) {
		settle(100);
		save(panel, dir.getChildFile(juce::String::formatted("card_%02d_out_%dms.png", i, i * 100)),
		     slot, 4);
	}
	save(panel, dir.getChildFile("card_20_empty_panel.png"), whole, 1);

	clickSlot(panel);
	for (int i = 1; i <= 11; ++i) {
		settle(100);
		save(panel, dir.getChildFile(juce::String::formatted("card_%02d_in_%dms.png", 20 + i, i * 100)),
		     slot, 4);
	}
	// Карта обязана вернуться ровно в то положение, с которого начали, - иначе гнездо после
	// возврата выглядело бы не так, как до извлечения.
	settle(1500);
	save(panel, dir.getChildFile("card_40_seated.png"), slot, 4);

	std::printf("done\n");
	return 0;
}
