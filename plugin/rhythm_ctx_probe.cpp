// closed_hat (клавиша 42, тембр 64) звучит в 10-50 раз тише соседей, и рядом с ней в логе
// движка появляется предупреждение "Attempted to play invalid key 1 (velocity 121)" -
// которого больше нигде за весь прогон нет. Совпадение скорости (121 = 0.95*127, ровно то,
// что шлёт наш собственный зонд) говорит, что это НАШ мост породил лишнее событие, а не
// munt сама по себе.
//
// mt32emu::RhythmPart::noteOn получает ev.note напрямую из D110Core::popNoteEvent(), а тот -
// из m_ctxNote[ctx], которое прошивка сама пишет в f400[] (rams 0x3400+ctx). Если "1" туда
// действительно попадает - это пишет прошивка, и вопрос "почему" переносится на её сторону;
// если нет - подмена происходит в НАШЕМ мосте. Единственный способ разделить эти два случая -
// прослушать саму запись, точно как la32_ctx_probe.cpp делает для мелодических партий.
#include "Source/PluginProcessor.h"

#include <cstdio>
#include <map>
#include <thread>

namespace {
constexpr double kSampleRate = 44100.0;
constexpr int kBlock = 512;
constexpr int kRhythmChannel = 10;

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

struct Decoded { const char *array; int index; };
Decoded decode(uint16_t addr) {
	if (addr >= 0x2DC0 && addr < 0x2E00) return {"edc0", (addr - 0x2DC0) / 2};
	if (addr >= 0x2E00 && addr < 0x2E40) return {"ee00", (addr - 0x2E00) / 2};
	if (addr >= 0x2E40 && addr < 0x2E80) return {"ee40", (addr - 0x2E40)};
	if (addr >= 0x2E80 && addr < 0x2EC0) return {"ee80", (addr - 0x2E80) / 2};
	if (addr >= 0x2EC0 && addr < 0x2F00) return {"eec0", (addr - 0x2EC0) / 2};
	if (addr >= 0x2F80 && addr < 0x2FC0) return {"ef80", (addr - 0x2F80) / 2};
	if (addr >= 0x33A0 && addr < 0x33C0) return {"f3a0", addr - 0x33A0}; // часть (part*16)
	if (addr >= 0x33C0 && addr < 0x33E0) return {"f3c0", addr - 0x33C0};
	if (addr >= 0x3400 && addr < 0x3420) return {"f400", addr - 0x3400}; // нота
	if (addr >= 0x3420 && addr < 0x3440) return {"f420", addr - 0x3420}; // скорость
	if (addr >= 0x3440 && addr < 0x3460) return {"f440", addr - 0x3440};
	if (addr >= 0x3460 && addr < 0x3480) return {"f460", addr - 0x3460}; // освобождение
	if (addr >= 0x3480 && addr < 0x34a0) return {"f480", addr - 0x3480};
	return {"?", -1};
}

} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	D110AudioProcessor proc;
	proc.prepareToPlay(kSampleRate, kBlock);
	proc.getCore().setVoiceCtxTap(true);
	proc.setPoweredOn(true);
	render(proc, 9.0);
	if (!proc.getCore().isRunning()) {
		std::printf("прошивка не запустилась: %s\n", proc.getLastError().toRawUTF8());
		return 1;
	}
	proc.setForwardNotesToFirmware(true);
	proc.getCore().takeCtxEvents(); // сбросить шум загрузки

	for (int note : {42, 46, 90, 35}) { // хай-хэты + один заведомо звучащий (kick) для контроля
		proc.getCore().takeCtxEvents();
		juce::MidiBuffer on;
		on.addEvent(juce::MidiMessage::noteOn(kRhythmChannel, note, 0.95f), 0);
		render(proc, 0.4, &on);
		juce::MidiBuffer off;
		off.addEvent(juce::MidiMessage::noteOff(kRhythmChannel, note), 0);
		render(proc, 0.4, &off);

		const auto events = proc.getCore().takeCtxEvents();
		std::printf("\n=== клавиша %d, %d событий ===\n", note, int(events.size()));
		for (const auto &e : events) {
			const Decoded d = decode(e.addr);
			if (d.index < 0) continue;
			std::printf("  PC %04X  %s[%d]  = %d (0x%02X)\n", e.pc, d.array, d.index, e.value,
			            e.value);
		}
	}

	proc.setPoweredOn(false);
	proc.releaseResources();
	return 0;
}
