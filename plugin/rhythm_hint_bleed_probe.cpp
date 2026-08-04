// Воспроизводит ровно то, что заметили на слух: играешь работающий барабан (кик, клавиша
// 35), потом Closed Hi-Hat (клавиша 42, тембр 64 - один из трёх сломанных) - и на хай-хэте
// звучит СОСЕДНИЙ звук, будто "отпечатался" кик. Причина была в том, что подсказка ставилась
// в очередь для ЛЮБОЙ ритм-ноты, а забирается только тремя сломанными клавишами - кик душил
// очередь, и хай-хэт получал чужой ключ.
//
// Проверяется через NoteLog: он несёт ev.note ПОСЛЕ подмены - то самое значение, что реально
// ушло в движок. Если чинит - для клавиши 42 там всегда 42, независимо от того, что играли
// до неё.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
using Clock = std::chrono::steady_clock;

void render(D110AudioProcessor &proc, double seconds, juce::MidiBuffer *midi = nullptr) {
	juce::AudioBuffer<float> buffer(2, kBlock);
	const auto until = Clock::now() + std::chrono::duration<double>(seconds);
	bool first = true;
	while (Clock::now() < until) {
		juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
		buffer.clear();
		juce::MidiBuffer none;
		proc.processBlock(buffer, (first && midi) ? *midi : none);
		first = false;
	}
}

void hit(D110AudioProcessor &proc, int note) {
	juce::MidiBuffer on;
	on.addEvent(juce::MidiMessage::noteOn(10, note, 0.9f), 0);
	render(proc, 0.4, &on);
	juce::MidiBuffer off;
	off.addEvent(juce::MidiMessage::noteOff(10, note), 0);
	render(proc, 0.3, &off);
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

	int failures = 0;
	// Сценарий из жалобы: серия "чужих" ударов (кик, потом ещё и снейр), потом хай-хэт -
	// и так десять раз подряд, чтобы поймать даже редко проявляющуюся аномалию.
	for (int round = 1; round <= 10; ++round) {
		proc.getCore().takeNoteLog();
		proc.getCore().startNoteLog();

		hit(proc, 35); // кик - работающая клавиша, раньше засоряла очередь
		hit(proc, 38); // снейр - тоже работающая
		hit(proc, 42); // Closed Hi-Hat - сломанная, требует подмены

		const auto events = proc.getCore().takeNoteLog();
		int hatNote = -1;
		for (const auto &e : events)
			if (e.on && e.part == 8) hatNote = int(e.note); // последний по счёту - хай-хэт
		const bool ok = (hatNote == 42);
		std::printf("круг %2d: клавиша 42 дошла до движка как %3d  %s\n", round, hatNote,
		            ok ? "верно" : "*** ОШИБКА ***");
		if (!ok) ++failures;
	}

	std::printf("\nитого: %d ошибок из 10\n", failures);

	proc.setPoweredOn(false);
	proc.releaseResources();
	return failures > 0 ? 1 : 0;
}
