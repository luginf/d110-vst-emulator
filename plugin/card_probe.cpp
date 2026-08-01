// Как прошивка D-110 узнаёт, вставлена ли карта памяти, - и вправду ли она узнаёт это так.
//
// Разбор ПЗУ (подпрограмма 0x770A, целиком выписана в комментарии к D110Core::kCardSize)
// говорит: отдельной линии «карта на месте» в машине нет вовсе. Прошивка ПИШЕТ в карту
// дополнение того, что там прочла, читает то же место обратно и смотрит, изменилось ли оно.
// Не изменилось и читалось как 0xFF - гнездо пустое.
//
// Разбор - это гипотеза, и здесь она проверяется тем, что высказывается сама прошивка. Ей
// подставляются ЧЕТЫРЕ разных карты, отличающиеся ровно одним свойством каждая, и с экрана
// снимается, что она о каждой сказала:
//
//   пустое гнездо (вся карта 0xFF)      ожидается "Card Not Ready"
//   карта из нулей (так MAME и грузит)  ожидается "Illegal Card" - запись проходит,
//                                       но подписи нет
//   отформатированная карта             ожидается работа без сообщения об ошибке
//   она же с защитой от записи          ожидается "Memory Card Write Protected"
//
// Четыре разных ответа на четыре подставленные карты - это и есть контроль: одно только
// "Card Not Ready" на пустом гнезде ничего не доказывало бы, потому что так же выглядело бы
// и меню, которое просто не работает.
//
// Первый аргумент - режим:
//   explore <кнопка> ...  нажать перечисленные кнопки, печатая экран после каждой. Так
//                         ищется сама страница «Save to Card», без домыслов о раскладке меню.
//   cards                 четырёхчастный опыт выше.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <cstring>
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

bool press(D110AudioProcessor &proc, const std::string &name, int times = 1) {
	for (const auto &b : kButtons)
		if (name == b.name) {
			const int idx = D110Core::buttonIndex(b.port, b.bit);
			for (int i = 0; i < times; ++i) {
				proc.getCore().setButton(idx, true);
				render(proc, 0.13);
				proc.getCore().setButton(idx, false);
				render(proc, 0.30);
			}
			return true;
		}
	std::printf("  !!! нет такой кнопки: %s\n", name.c_str());
	return false;
}

// Подпись отформатированной карты - двенадцать байт из ПЗУ 0x7804, за ними тип карты и его
// дополнение (прошивка требует именно пары X / ~X, подпрограмма 0x7785).
void fillFormatted(std::vector<uint8_t> &card) {
	card.assign(D110Core::kCardSize, 0x00);
	static const char kSig[] = "Roland D-10 ";
	std::memcpy(card.data(), kSig, 12);
	card[0x0c] = 'D';
	card[0x0d] = uint8_t(~'D');
	card[0x0e] = 'X';
	card[0x0f] = uint8_t(~'X');
	// Бит 0 последнего байта - защита от записи, ноль означает «защищена».
	card[D110Core::kCardSize - 1] = 0xff;
}

// Одна подставленная карта: залить, дойти до страницы работы с картой, нажать ENTER и снять
// с экрана ответ прошивки.
void tryCard(D110AudioProcessor &proc, const char *what, const std::vector<uint8_t> &card,
             bool inserted, const std::vector<std::string> &path) {
	proc.getCore().setCardImage(card.data());
	proc.getCore().setCardInserted(inserted);
	render(proc, 0.6);

	press(proc, "Exit", 3);
	for (const auto &b : path) press(proc, b);
	const std::string before = screen(proc);
	press(proc, "Enter");
	render(proc, 1.0);
	// ENTER только спрашивает "Sure?", а выполняет WRITE/COPY - подтверждение здесь его, и
	// найдено это перебором кнопок, а не догадкой о меню.
	const std::string asked = screen(proc);
	press(proc, "Write");

	std::printf("  %s\n    до ENTER      : \"%s\"\n    после ENTER   : \"%s\"\n",
	            what, before.c_str(), asked.c_str());
	// Сообщение об ошибке прошивка снимает сама через пару секунд, поэтому экран пишется
	// серией: один снимок «через столько-то» его просто не застаёт.
	std::string last = asked;
	for (int t = 0; t < 20; ++t) {
		const std::string s = screen(proc);
		if (s != last) { std::printf("    %4.1f с ОТВЕТ: \"%s\"\n", t * 0.15, s.c_str()); last = s; }
		render(proc, 0.15);
	}
	press(proc, "Exit", 3);
}

} // namespace

int main(int argc, char **argv) {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	loadCgrom();

	const std::string mode = argc > 1 ? argv[1] : "explore";

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	std::printf("прошивка: %s   знакогенератор: %s\n",
	            proc.getCore().isRunning() ? "работает" : "НЕТ",
	            g_cgrom.empty() ? "НЕ НАЙДЕН" : "загружен");
	press(proc, "Exit", 3);
	std::printf("исходный экран: \"%s\"\n", screen(proc).c_str());

	if (mode == "explore") {
		// Экран снимается не один раз, а серией: сообщения об ошибке карты прошивка держит
		// пару секунд и убирает сама, и один снимок «через столько-то» их просто не застаёт.
		for (int i = 2; i < argc; ++i) {
			// Карта - такое же действие пользователя, как нажатие кнопки, и в разборе шагов
			// ей место в том же списке.
			// Сколько байт батарейного ОЗУ разошлось с прошлой отметкой. "Complete" на экране
			// говорит только то, что операция дошла до конца; изменилась ли от неё память -
			// вопрос отдельный, и на него отвечает счёт байтов, а не сообщение.
			if (std::strcmp(argv[i], "RAMDIFF") == 0) {
				static std::vector<uint8_t> markRam;
				std::vector<uint8_t> now(D110Core::kRamSize, 0);
				proc.getCore().getRam(now.data());
				if (markRam.empty()) std::printf("  RAMDIFF  : отметка поставлена\n");
				else {
					size_t diff = 0;
					for (size_t k = 0; k < now.size(); ++k) if (now[k] != markRam[k]) ++diff;
					std::printf("  RAMDIFF  : разошлось байт: %zu\n", diff);
				}
				markRam = now;
				continue;
			}
			if (std::strcmp(argv[i], "EJECT") == 0 || std::strcmp(argv[i], "INSERT") == 0) {
				proc.getCore().setCardInserted(std::strcmp(argv[i], "INSERT") == 0);
				render(proc, 0.6);
				std::printf("  %-8s : \"%s\"\n", argv[i], screen(proc).c_str());
				continue;
			}
			press(proc, argv[i]);
			std::printf("  после %-8s :\n", argv[i]);
			std::string last;
			for (int t = 0; t < 24; ++t) {
				const std::string s = screen(proc);
				if (s != last) { std::printf("      %5.1f с  \"%s\"\n", t * 0.15, s.c_str()); last = s; }
				render(proc, 0.15);
			}
		}
		proc.setPoweredOn(false);
		return 0;
	}

	if (mode == "roundtrip") {
		// Полный круг, какой прошёл бы владелец: чистая карта - форматирование - запись -
		// извлечение - возврат - чтение. Каждый шаг подтверждается тем, что сказала сама
		// прошивка, и снимком карты, который берётся из плагина.
		auto watch = [&](const char *what, double seconds) {
			std::string last;
			const int steps = int(seconds / 0.15);
			for (int t = 0; t < steps; ++t) {
				const std::string s = screen(proc);
				if (s != last) { std::printf("    %-18s %4.1f с \"%s\"\n", what, t * 0.15, s.c_str()); last = s; }
				render(proc, 0.15);
			}
		};
		std::vector<uint8_t> card(D110Core::kCardSize, 0xff);
		std::vector<uint8_t> seen(D110Core::kCardSize, 0);

		std::printf("\n=== 1. чистая карта в гнездо, Save to Card ===\n");
		proc.getCore().setCardImage(card.data());
		proc.getCore().setCardInserted(true);
		render(proc, 0.6);
		press(proc, "Write"); press(proc, "Enter"); press(proc, "Write");
		watch("после WRITE", 2.5);

		std::printf("\n=== 2. согласиться на форматирование ===\n");
		press(proc, "Enter"); // здесь подтверждает ENTER, а не WRITE - найдено перебором
		watch("формат", 6.0);
		proc.getCore().getCardImage(seen.data());
		std::printf("    подпись на карте: \"%.12s\"  тип %02X/%02X %02X/%02X  байт 0x7FFF=%02X\n",
		            reinterpret_cast<char *>(seen.data()), seen[0x0c], seen[0x0d], seen[0x0e],
		            seen[0x0f], seen[D110Core::kCardSize - 1]);

		std::printf("\n=== 3. записать звуки на карту ===\n");
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Enter"); press(proc, "Write");
		watch("запись", 8.0);
		proc.getCore().getCardImage(seen.data());
		size_t nonzero = 0;
		for (size_t i = 0x10; i < seen.size(); ++i) if (seen[i] != 0x00 && seen[i] != 0xff) ++nonzero;
		std::printf("    байт с данными на карте: %zu\n", nonzero);

		std::printf("\n=== 4. вынуть карту и попробовать читать без неё ===\n");
		proc.getCore().setCardInserted(false);
		render(proc, 0.6);
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Group+"); press(proc, "Enter"); press(proc, "Write");
		// Чтение с карты пишет во внутреннюю память, а она защищена, поэтому прошивка сперва
		// спрашивает "MemProtected / Turn off once ?" - согласиться и идти дальше.
		render(proc, 0.6);
		press(proc, "Enter"); press(proc, "Write");
		watch("без карты", 3.0);

		std::printf("\n=== 5. вернуть её и прочитать ===\n");
		proc.getCore().setCardInserted(true);
		render(proc, 0.6);
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Group+"); press(proc, "Enter"); press(proc, "Write");
		render(proc, 0.6);
		press(proc, "Enter"); press(proc, "Write");
		watch("чтение", 8.0);

		proc.setPoweredOn(false);
		return 0;
	}

	if (mode == "persist") {
		// Карта - носитель, значит она обязана пережить выключение прибора, а гнездо -
		// помнить, вынули из него карту или нет. Метка кладётся в место, до которого прошивке
		// нет дела, чтобы её нельзя было спутать с тем, что записал прибор.
		std::vector<uint8_t> card(D110Core::kCardSize, 0x00);
		fillFormatted(card);
		static const char kMark[] = "MARK-2026";
		std::memcpy(card.data() + 0x100, kMark, sizeof kMark);
		proc.getCore().setCardImage(card.data());
		proc.getCore().setCardInserted(true);
		render(proc, 1.0);

		std::vector<uint8_t> seen(D110Core::kCardSize, 0);
		auto report = [&](const char *when) {
			proc.getCore().getCardImage(seen.data());
			std::printf("  %-26s подпись \"%.12s\"  метка \"%.9s\"  гнездо: %s\n", when,
			            reinterpret_cast<char *>(seen.data()),
			            reinterpret_cast<char *>(seen.data() + 0x100),
			            proc.getCore().cardInserted() ? "карта на месте" : "пусто");
		};
		report("карта вставлена:");

		std::printf("\n=== выключить и включить ===\n");
		proc.setPoweredOn(false);
		std::this_thread::sleep_for(std::chrono::seconds(2));
		proc.setPoweredOn(true);
		render(proc, 9.0);
		report("после включения:");

		std::printf("\n=== вынуть карту, выключить и включить ===\n");
		proc.getCore().setCardInserted(false);
		render(proc, 1.0);
		proc.setPoweredOn(false);
		std::this_thread::sleep_for(std::chrono::seconds(2));
		proc.setPoweredOn(true);
		render(proc, 9.0);
		report("после включения:");
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Enter"); press(proc, "Write");
		std::string last;
		for (int t = 0; t < 20; ++t) {
			const std::string s = screen(proc);
			if (s != last) { std::printf("    Save to Card: \"%s\"\n", s.c_str()); last = s; }
			render(proc, 0.15);
		}

		std::printf("\n=== вернуть карту ===\n");
		proc.getCore().setCardInserted(true);
		render(proc, 1.0);
		report("вставлена обратно:");

		proc.setPoweredOn(false);
		return 0;
	}

	if (mode == "loadverify") {
		// "Complete" на экране говорит только то, что операция дошла до конца. Что чтение с
		// карты и вправду ВОЗВРАЩАЕТ память, показывает лишь сравнение трёх снимков: сразу
		// после записи на карту, после правки в приборе и после чтения обратно. Третий обязан
		// совпасть с первым, а второй - разойтись с ним. Без второго опыт ничего не значил бы:
		// снимки совпали бы и у чтения, которое ничего не делает.
		auto snap = [&] {
			std::vector<uint8_t> v(D110Core::kRamSize, 0);
			proc.getCore().getRam(v.data());
			return v;
		};
		// Счёт разошедшихся байт сам по себе ни о чём не говорит: экранный буфер прошивки
		// живёт в том же ОЗУ и меняется от одного снимка к другому просто так. Поэтому
		// печатаются и адреса - память патчей это 0x0000-0x1FFF (замерено factory_bank_probe),
		// и правка типа ревербератора обязана лечь именно туда.
		auto diff = [](const char *what, const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
			size_t n = 0, inPatches = 0;
			std::string where;
			for (size_t i = 0; i < a.size(); ++i)
				if (a[i] != b[i]) {
					++n;
					if (i < 0x2000) ++inPatches;
					if (n <= 12) {
						char buf[32];
						std::snprintf(buf, sizeof buf, " %04zX:%02X>%02X", i, a[i], b[i]);
						where += buf;
					}
				}
			std::printf("  %-34s всего %3zu, в памяти патчей %3zu %s\n", what, n, inPatches,
			            where.c_str());
			return n;
		};
		auto watch = [&](double seconds) {
			std::string last;
			for (int t = 0; t < int(seconds / 0.15); ++t) {
				const std::string s = screen(proc);
				if (s != last) { std::printf("      %4.1f с \"%s\"\n", t * 0.15, s.c_str()); last = s; }
				render(proc, 0.15);
			}
		};

		std::printf("\n=== снять защиту внутренней памяти ===\n");
		press(proc, "System"); press(proc, "Group+"); press(proc, "Number-");
		std::printf("  %s\n", screen(proc).c_str());
		press(proc, "Exit", 3);

		std::printf("\n=== чистая карта, форматирование, запись ===\n");
		std::vector<uint8_t> blank(D110Core::kCardSize, 0xff);
		proc.getCore().setCardImage(blank.data());
		proc.getCore().setCardInserted(true);
		render(proc, 0.6);
		press(proc, "Write"); press(proc, "Enter"); press(proc, "Write");
		watch(2.5);
		press(proc, "Enter"); // согласиться на форматирование
		watch(4.0);
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Enter"); press(proc, "Write");
		watch(5.0);
		press(proc, "Exit", 3);
		const auto afterSave = snap();

		// Стимул должен менять ХРАНИМУЮ память, а не временную. Правка в Patch Edit не годится:
		// она ложится в рабочую копию, и снимок ОЗУ показал ноль изменений в памяти патчей.
		// Заводской сброс перестраивает память тембров и ритма из ПЗУ пресетов - это заведомо
		// хранимая память, и он перезапускает машину, что заодно проверяет, переживает ли
		// карта перезапуск.
		std::printf("\n=== заводской сброс: память заведомо другая ===\n");
		proc.getCore().factoryReset();
		render(proc, 3.0);
		while (proc.getCore().isResetting() || !proc.getCore().isRunning()) render(proc, 0.5);
		render(proc, 9.0);
		press(proc, "Exit", 3);
		const auto afterEdit = snap();
		diff("после сброса против записанного:", afterSave, afterEdit);

		std::printf("\n=== чтение с карты ===\n");
		press(proc, "System"); press(proc, "Group+"); press(proc, "Number-");
		std::printf("  %s\n", screen(proc).c_str());
		press(proc, "Exit", 3);
		press(proc, "Write"); press(proc, "Group+"); press(proc, "Enter"); press(proc, "Write");
		watch(5.0);
		press(proc, "Exit", 3);
		const auto afterLoad = snap();
		diff("после чтения против записанного:", afterSave, afterLoad);
		diff("после чтения против правки:", afterEdit, afterLoad);

		proc.setPoweredOn(false);
		return 0;
	}

	// Путь до страницы работы с картой. Передаётся с командной строки, потому что найден он
	// режимом explore, а не выведен из раскладки меню.
	std::vector<std::string> path;
	for (int i = 2; i < argc; ++i) path.emplace_back(argv[i]);
	if (path.empty()) { std::printf("нужен путь кнопок, найденный режимом explore\n"); return 1; }

	std::printf("\n=== четыре карты, четыре ответа ===\n");
	std::vector<uint8_t> card(D110Core::kCardSize, 0xff);
	tryCard(proc, "пустое гнездо (0xFF)", card, false, path);

	card.assign(D110Core::kCardSize, 0x00);
	tryCard(proc, "карта из нулей", card, true, path);

	fillFormatted(card);
	tryCard(proc, "отформатированная", card, true, path);

	// Защита от записи - движок НА КАРТЕ, а не байт в её памяти: прошивка читает его как
	// бит 0 порта состояния матрицы IC21. Пока этот адрес был памятью, форматирование само
	// же и защищало только что отформатированную карту.
	proc.getCore().setCardWriteProtect(true);
	tryCard(proc, "она же, защита записи", card, true, path);
	proc.getCore().setCardWriteProtect(false);

	// Контроль сохранности: то, что прошивка записала на карту, обязано пережить извлечение
	// и возврат. Иначе «карта» - это картинка, а не носитель.
	std::printf("\n=== контроль: содержимое переживает извлечение ===\n");
	fillFormatted(card);
	proc.getCore().setCardImage(card.data());
	proc.getCore().setCardInserted(true);
	render(proc, 0.6);
	std::vector<uint8_t> seen(D110Core::kCardSize, 0);
	proc.getCore().getCardImage(seen.data());
	std::printf("  вставлена: подпись \"%.12s\"\n", reinterpret_cast<char *>(seen.data()));
	proc.getCore().setCardInserted(false);
	render(proc, 0.6);
	proc.getCore().getCardImage(seen.data());
	std::printf("  извлечена: подпись \"%.12s\" (буфер плагина)\n",
	            reinterpret_cast<char *>(seen.data()));
	proc.getCore().setCardInserted(true);
	render(proc, 0.6);
	proc.getCore().getCardImage(seen.data());
	std::printf("  вставлена снова: подпись \"%.12s\"\n", reinterpret_cast<char *>(seen.data()));

	proc.setPoweredOn(false);
	return 0;
}
