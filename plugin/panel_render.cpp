// Снимает НАСТОЯЩЕЕ окно плагина в PNG, чтобы его было на что посмотреть рядом с
// фотографией прибора, а не судить о нём по константам. Рисуется тот самый
// D110AudioProcessorEditor, который видит пользователь, - в отличие от d110_lcd_check,
// который повторяет цикл отрисовки у себя и потому годится только для подбора чисел.
//
// Снимается целое окно, а не одна панель, и вот почему: карта памяти больше не принадлежит
// панели. Раньше она могла только проехать мимо кадра и скрыться - панель ростом с прибор,
// 256 точек, а карта 370. Теперь под прибором есть ящик, карта выезжает НА НЕГО и там
// остаётся целиком видимой, так что её ход виден только в окне целиком.
//
// Карту двигает не переменная, а щелчок по щели: так проверяется весь путь, от попадания
// мышью до кадра.
//
// Usage: d110_panel_render [output_dir]
#include "Source/PluginEditor.h"
#include "Source/PluginProcessor.h"

#include <cstdio>

namespace {

void save(juce::Component &c, const juce::File &out, juce::Rectangle<int> area, float scale) {
	const juce::Image shot = c.createComponentSnapshot(area, false, scale);
	juce::PNGImageFormat png;
	out.deleteFile();
	std::unique_ptr<juce::FileOutputStream> stream(out.createOutputStream());
	if (stream != nullptr) png.writeImageToStream(shot, *stream);
	std::printf("  %-24s %dx%d -> %s\n", out.getFileNameWithoutExtension().toRawUTF8(),
	            shot.getWidth(), shot.getHeight(), out.getFullPathName().toRawUTF8());
}

// Дать анимации проехать: карту и ящик двигают собственные таймеры, поэтому нужен настоящий
// цикл сообщений, а не вызов обработчика напрямую.
void settle(int ms) {
	juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
}

// Панель - первый ребёнок окна, и щель ловит именно она: сама щель нарисована на приборе.
D110Panel *panelOf(juce::Component &editor) {
	for (int i = 0; i < editor.getNumChildComponents(); ++i)
		if (auto *p = dynamic_cast<D110Panel *>(editor.getChildComponent(i)))
			return p;
	return nullptr;
}

void clickSlot(D110Panel &panel) {
	const juce::Point<float> p(D110Panel::kSlotHitX + D110Panel::kSlotHitW * 0.5f,
	                           D110Panel::kSlotHitY + D110Panel::kSlotHitH * 0.5f);
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
	std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
	if (editor == nullptr) { std::printf("нет редактора\n"); return 1; }
	// Ширина окна - та же, с какой плагин открывается.
	constexpr int kWidth = 1500;
	const float s = float(kWidth) / float(D110Panel::kRefW);
	editor->setSize(kWidth, int((float(D110Panel::kRefH)
	                             + D110AudioProcessorEditor::kHandleRefH) * s + 0.5f));
	settle(200);

	D110Panel *panel = panelOf(*editor);
	if (panel == nullptr) { std::printf("панель не найдена\n"); return 1; }

	// Крупный план щели, и он же - место, куда карта поедет: от прибора вниз, на ящик.
	auto closeUp = [s] {
		return juce::Rectangle<float>(1520.0f * s, 90.0f * s, 400.0f * s, 660.0f * s)
			.toNearestInt();
	};

	save(*editor, dir.getChildFile("card_00_inserted.png"), closeUp(), 2.0f);
	save(*editor, dir.getChildFile("card_00_inserted_whole.png"), editor->getLocalBounds(), 1.0f);

	// Раскадровка через равные промежутки, а не в трёх выбранных точках: по ней видно и как
	// карта выглядит, и сколько времени занимает ход. Шаг в 100 мс - примерно та частота, с
	// какой глаз успевает разобрать движение. Извлечение само открывает ящик, поэтому кадры
	// показывают заодно и его ход.
	clickSlot(*panel);
	for (int i = 1; i <= 11; ++i) {
		settle(100);
		save(*editor, dir.getChildFile(juce::String::formatted("card_%02d_out_%dms.png", i, i * 100)),
		     closeUp(), 2.0f);
	}
	settle(600);
	save(*editor, dir.getChildFile("card_20_out_whole.png"), editor->getLocalBounds(), 1.0f);

	// --- перетаскивание -------------------------------------------------------
	//
	// Извлечённую карту можно взять левой кнопкой и переложить куда угодно в пределах ящика.
	// Проверяется тем же способом, что и щель: настоящими событиями мыши по настоящему
	// компоненту, а не переменной, - и по сдвигу его границ видно, доехала карта или нет.
	{
		D110MemoryCard *card = nullptr;
		for (int i = 0; i < editor->getNumChildComponents(); ++i)
			if (auto *c = dynamic_cast<D110MemoryCard *>(editor->getChildComponent(i))) card = c;

		if (card == nullptr) {
			std::printf("  !!! карта не найдена среди детей окна\n");
		} else {
			const auto before = card->getBounds();
			auto sendMouse = [&card](juce::Point<float> at, int what) {
				const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), at,
				                         juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f,
				                         0.0f, 0.0f, 0.0f, card, card,
				                         juce::Time::getCurrentTime(), at,
				                         juce::Time::getCurrentTime(), 1, false);
				if (what == 0) card->mouseDown(e);
				else if (what == 1) card->mouseDrag(e);
				else card->mouseUp(e);
			};
			// Взялись за середину карты и повели влево и вниз, кадр за кадром.
			const juce::Point<float> grab(float(card->getWidth()) * 0.5f,
			                              float(card->getHeight()) * 0.5f);
			sendMouse(grab, 0);
			for (int step = 1; step <= 8; ++step) {
				sendMouse(grab + juce::Point<float>(-90.0f * float(step), 22.0f * float(step)), 1);
				settle(40);
				if (step % 4 == 0)
					save(*editor, dir.getChildFile(juce::String::formatted("card_5%d_dragged.png",
					                                                       step / 4)),
					     editor->getLocalBounds(), 1.0f);
			}
			sendMouse(grab, 2);
			const auto after = card->getBounds();
			std::printf("  перетаскивание: %d,%d -> %d,%d (сдвиг %d,%d)\n", before.getX(),
			            before.getY(), after.getX(), after.getY(),
			            after.getX() - before.getX(), after.getY() - before.getY());
			// Карта не имеет права залезть на прибор: ниже полосы-ручки, и только там.
			const float s2 = float(kWidth) / float(D110Panel::kRefW);
			const int floorY = int((float(D110Panel::kRefH)
			                        + D110AudioProcessorEditor::kHandleRefH) * s2);
			std::printf("  карта ниже прибора: %s (верх %d, граница %d)\n",
			            after.getY() >= floorY ? "да" : "НЕТ", after.getY(), floorY);
		}
	}

	clickSlot(*panel);
	for (int i = 1; i <= 11; ++i) {
		settle(100);
		save(*editor, dir.getChildFile(juce::String::formatted("card_%02d_in_%dms.png", 20 + i, i * 100)),
		     closeUp(), 2.0f);
	}
	// Карта обязана вернуться ровно в то положение, с которого начали, - иначе гнездо после
	// возврата выглядело бы не так, как до извлечения.
	settle(1500);
	save(*editor, dir.getChildFile("card_40_seated.png"), closeUp(), 2.0f);

	std::printf("done\n");
	return 0;
}
