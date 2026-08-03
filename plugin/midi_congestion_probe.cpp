// Насколько на самом деле расходится время, когда прошивка УЗНАЁТ о ноте, если несколько
// партий бьют одновременно - то, что раньше объяснялось на словах (общая очередь MIDI к
// прошивке на настоящей скорости кабеля, 3125 байт/с), здесь измеряется по-настоящему.
//
// D110Core уже несёт для этого готовый инструмент: startNoteLog()/takeNoteLog() ставит
// метку РЕАЛЬНОГО времени (steady_clock, миллисекунды от старта записи) на каждую ноту,
// которую прошивка сама зарегистрировала в своих таблицах. Это не время, когда MIDI ушёл в
// плагин, а время, когда прошивка ДЕЙСТВИТЕЛЬНО об этом узнала - то есть ровно то звено, где
// и живёт задержка общей последовательной очереди.
//
// Опыт: девять партий (восемь голосовых + ритм) бьют РОВНО ОДНОВРЕМЕННО - все в одном
// MidiBuffer на нулевой позиции внутри одного processBlock, как их подал бы хост на плотной
// доле, - и так восемь долей подряд в РЕАЛЬНОМ времени (120 уд/мин, не ускоренно). Номер доли
// зашит в velocity, поэтому запись в NoteLog можно однозначно приписать своей доле, а не
// гадать по близости меток.
#include "Source/PluginProcessor.h"

#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kBeats = 8;
constexpr double kBeatMs = 500.0; // 120 уд/мин
// Партии 1-8 отвечают на каналах 2-9, ритм - на 10 (заводская карта).
constexpr int kChannels[9] = { 2, 3, 4, 5, 6, 7, 8, 9, 10 };

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

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}
	std::printf("прошивка работает, начинаю плотный паттерн - %d долей по %.0f мс "
	            "(120 уд/мин), девять партий разом на каждой\n\n",
	            kBeats, kBeatMs);

	proc.getCore().startNoteLog();
	const auto t0 = std::chrono::steady_clock::now();

	for (int beat = 0; beat < kBeats; ++beat) {
		// Ждать РЕАЛЬНОЕ время до своей доли - не ускоренно, иначе измерение показало бы не
		// то, с чем сталкивается играющий в настоящем темпе.
		const double targetMs = beat * kBeatMs;
		while (std::chrono::duration<double, std::milli>(
		           std::chrono::steady_clock::now() - t0)
		           .count() < targetMs)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));

		juce::MidiBuffer hit;
		// Номер доли зашит в velocity (10 + beat), чтобы запись в NoteLog можно было
		// однозначно приписать своей доле - не по близости меток времени, а по значению.
		for (int ch : kChannels)
			hit.addEvent(juce::MidiMessage::noteOn(ch, 48, juce::uint8(10 + beat)), 0);
		juce::AudioBuffer<float> audio(2, kBlock);
		audio.clear();
		proc.processBlock(audio, hit);

		// Снятие - через 80 мс, чтобы голоса не копились без нужды; на измерение регистрации
		// note-on это не влияет.
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
		juce::MidiBuffer off;
		for (int ch : kChannels) off.addEvent(juce::MidiMessage::noteOff(ch, 48), 0);
		juce::AudioBuffer<float> audio2(2, kBlock);
		audio2.clear();
		proc.processBlock(audio2, off);
	}

	render(proc, 1.5); // дать хвосту очереди дойти до прошивки

	const auto log = proc.getCore().takeNoteLog();
	const auto dropped = proc.getCore().noteLogDropped_();
	std::printf("записей в журнале: %d, потеряно: %llu\n\n", int(log.size()),
	            (unsigned long long)dropped);

	std::printf("=== разброс регистрации ВНУТРИ каждой доли (девять партий, ударили разом) ===\n");
	double worstSpread = 0.0, worstDriftMs = 0.0;
	int worstDriftBeat = -1;
	double firstBeatFirstMs = -1.0;
	for (int beat = 0; beat < kBeats; ++beat) {
		std::vector<double> ms;
		for (const auto &e : log)
			if (e.on && e.velocity == 10 + beat) ms.push_back(e.ms);
		if (ms.empty()) {
			std::printf("  доля %d: НИ ОДНА нота не зарегистрирована\n", beat + 1);
			continue;
		}
		std::sort(ms.begin(), ms.end());
		const double spread = ms.back() - ms.front();
		worstSpread = std::max(worstSpread, spread);
		if (beat == 0) firstBeatFirstMs = ms.front();
		// Отставание первой ноты доли от того, где она должна была бы стоять, если бы каждая
		// доля начинала с чистого листа - т.е. от идеальной сетки, отсчитанной с первой доли.
		const double intendedMs = firstBeatFirstMs + beat * kBeatMs;
		const double driftMs = ms.front() - intendedMs;
		if (std::abs(driftMs) > std::abs(worstDriftMs)) { worstDriftMs = driftMs; worstDriftBeat = beat; }
		std::printf("  доля %d: нот зарегистрировано %2d/9   разброс внутри доли %.2f мс   "
		            "отставание первой ноты от сетки %+.2f мс\n",
		            beat + 1, int(ms.size()), spread, driftMs);
	}

	std::printf("\nсамый большой разброс внутри одной доли: %.2f мс "
	            "(теоретический потолок для девяти трёхбайтных нот подряд: %.2f мс)\n",
	            worstSpread, (9 * 3 - 1) * 1000.0 / D110Core::kMidiBytesPerSecond);
	std::printf("самое большое отставание от сетки: %.2f мс, на доле %d%s\n", worstDriftMs,
	            worstDriftBeat + 1,
	            std::abs(worstDriftMs) > kBeatMs * 0.05
	                ? "  <-- растёт от доли к доле, а не только разброс внутри удара"
	                : "  (в пределах разброса одного удара, не накапливается)");

	// --- стресс: где именно средняя плотность начинает превышать канал ------------
	//
	// Умеренный удар (выше) не копит отставания вовсе - средняя нагрузка там смехотворно
	// мала (27 байт раз в 500 мс = 54 байт/с против канала в 3125 байт/с). Здесь плотность
	// поднимается настоящим потоком - шестнадцатые по трём партиям ударных сразу, - и очередь
	// смотрится НАПРЯМУЮ по счётчикам байт (midiForwarded/midiDelivered), а не косвенно по
	// журналу нот: разница между ними - это в точности то, что ещё сидит в очереди и не
	// доехало до прошивки.
	std::printf("\n=== СТРЕСС: сплошной плотный поток, слежение за очередью напрямую ===\n");
	{
		// Подобрано так, чтобы СРЕДНЯЯ нагрузка превысила канал (3125 байт/с), а не осталась
		// близко к нему: 9 партий, нота+снятие каждая (6 байт) каждые 10 мс - это 5400 байт/с,
		// 173% канала. Это уже не "быстрый барабанный проход", а намеренный избыток - вопрос
		// не "бывает ли так в музыке", а "что происходит с очередью, когда так есть".
		constexpr double kStepMs = 10.0;
		constexpr int kVoices = 9;
		constexpr int kSteps = 200; // 2 секунды потока
		const uint64_t beforeSent = proc.getCore().midiForwarded();
		const auto t1 = std::chrono::steady_clock::now();
		double peakBacklogBytes = 0.0;
		double peakBacklogMs = 0.0;

		for (int step = 0; step < kSteps; ++step) {
			const double targetMs = step * kStepMs;
			while (std::chrono::duration<double, std::milli>(
			           std::chrono::steady_clock::now() - t1)
			           .count() < targetMs)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));

			juce::MidiBuffer hit;
			for (int v = 0; v < kVoices; ++v)
				hit.addEvent(juce::MidiMessage::noteOn(kChannels[v], 36 + v, juce::uint8(100)), 0);
			for (int v = 0; v < kVoices; ++v)
				hit.addEvent(juce::MidiMessage::noteOff(kChannels[v], 36 + v), 0);
			juce::AudioBuffer<float> audio(2, kBlock);
			audio.clear();
			proc.processBlock(audio, hit);

			const uint64_t sent = proc.getCore().midiForwarded();
			const uint64_t got = proc.getCore().midiDelivered();
			const double backlogBytes = double(sent - got);
			const double backlogMs = backlogBytes * 1000.0 / D110Core::kMidiBytesPerSecond;
			peakBacklogBytes = std::max(peakBacklogBytes, backlogBytes);
			peakBacklogMs = std::max(peakBacklogMs, backlogMs);
		}

		render(proc, 2.0); // дать очереди дослить всё, что накопилось
		const uint64_t afterSent = proc.getCore().midiForwarded();
		const uint64_t afterGot = proc.getCore().midiDelivered();
		const double avgBytesPerSec = double(afterSent - beforeSent)
		                            / (kSteps * kStepMs / 1000.0);

		std::printf("  шаг %.1f мс (16-е при 240 уд/мин), %d партий разом, %d шагов\n",
		            kStepMs, kVoices, kSteps);
		std::printf("  средняя нагрузка потока: %.0f байт/с из %.0f байт/с канала (%.0f%%)\n",
		            avgBytesPerSec, D110Core::kMidiBytesPerSecond,
		            100.0 * avgBytesPerSec / D110Core::kMidiBytesPerSecond);
		std::printf("  пик очереди во время потока: %.0f байт (~%.0f мс отставания)\n",
		            peakBacklogBytes, peakBacklogMs);
		std::printf("  очередь после потока и 2с ожидания: %llu байт (%s)\n",
		            (unsigned long long)(afterSent - afterGot),
		            (afterSent - afterGot) == 0 ? "полностью слита" : "ОСТАЛОСЬ ВИСЕТЬ");
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
