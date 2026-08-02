#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
// MidiInput/MidiOutput and MidiDeviceInfo live here, not in juce_audio_processors: the
// panel opens a port itself so an external editor can reach the module the way it would
// reach the hardware, beside whatever the host routes in.
#include <juce_audio_devices/juce_audio_devices.h>
#include <mt32emu/mt32emu.h>
#include "D110Core.h"
#include <array>
#include <memory>
#include <thread>

class D110AudioProcessor : public juce::AudioProcessor,
                           private juce::MidiInputCallback,
                           private juce::Timer {
public:
	D110AudioProcessor();
	~D110AudioProcessor() override;

	// --- MIDI ports chosen on the panel, independently of the host -------------
	// A hardware module is driven from whatever is plugged into its MIDI IN, and an editor
	// like MIDI Quest expects to reach it over a port rather than through the DAW's plugin
	// routing. Opening a port directly is what makes that possible; the host's own MIDI
	// keeps working alongside it, so this adds a route rather than replacing one.
	static juce::Array<juce::MidiDeviceInfo> midiInputs() {
		return juce::MidiInput::getAvailableDevices();
	}
	static juce::Array<juce::MidiDeviceInfo> midiOutputs() {
		return juce::MidiOutput::getAvailableDevices();
	}
	void setMidiInputDevice(const juce::String &identifier);
	void setMidiOutputDevice(const juce::String &identifier);
	juce::String getMidiInputId() const { return selInputId; }
	juce::String getMidiOutputId() const { return selOutputId; }

	void prepareToPlay(double sampleRate, int samplesPerBlock) override;
	void releaseResources() override;
	void processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) override;

	juce::AudioProcessorEditor *createEditor() override;
	bool hasEditor() const override { return true; }

	const juce::String getName() const override { return "D-110 Emulator"; }
	bool acceptsMidi() const override { return true; }
	bool producesMidi() const override { return false; }
	bool isMidiEffect() const override { return false; }
	double getTailLengthSeconds() const override { return 3.0; }

	int getNumPrograms() override { return 1; }
	int getCurrentProgram() override { return 0; }
	void setCurrentProgram(int) override {}
	const juce::String getProgramName(int) override { return {}; }
	void changeProgramName(int, const juce::String &) override {}

	void getStateInformation(juce::MemoryBlock &destData) override;
	void setStateInformation(const void *data, int sizeInBytes) override;

	// Remembers the chosen path and (re)opens the synth once BOTH paths are known.
	// Safe to call with just one path set - it will not error until the other is also provided.
	void setControlRomPath(const juce::String &path);
	void setPcmRomPath(const juce::String &path);

	bool isSynthReady() const { return synth != nullptr; }
	juce::String getControlRomPath() const { return controlRomPath; }
	juce::String getPcmRomPath() const { return pcmRomPath; }
	juce::String getControlRomDescription() const { return controlRomDescription; }
	juce::String getPcmRomDescription() const { return pcmRomDescription; }
	juce::String getLastError() const { return lastError; }
	static juce::File getAutoRomFolder();

	// Snapshot of everything the virtual front panel needs to redraw itself each frame.
	// Built entirely from stable, always-current getters (not mt32emu's own getDisplayState() text,
	// which is designed to flash transient messages and revert after a couple of seconds, and whose
	// internal buffer turned out to leave stale bytes behind when a shorter message follows a longer
	// one - see the "info overlapping" bug this replaced).
	//
	// `text` holds raw character codes rather than a juce::String because the panel renders a real
	// 16x2 dot matrix and needs codes, not glyph-agnostic text: code 0x01 is the CGRAM full-block
	// the hardware substitutes for a Part number while that Part is sounding, which no string can
	// carry. Layout copies the real Patch Play screen photographed in docs/lcd_reference.png:
	//
	//     12345678R RomPly
	//     1:Macho Memory
	struct LcdSnapshot {
		static constexpr int kCols = 16;
		static constexpr int kLines = 2;
		static constexpr juce::uint8 kActivePartBlock = 0x01; // matches Display.cpp's ACTIVE_PART_INDICATOR

		bool midiLedOn = false;
		juce::uint8 text[kLines][kCols] = {};

		bool operator==(const LcdSnapshot &other) const {
			if (midiLedOn != other.midiLedOn) return false;
			for (int line = 0; line < kLines; ++line)
				for (int col = 0; col < kCols; ++col)
					if (text[line][col] != other.text[line][col]) return false;
			return true;
		}
		bool operator!=(const LcdSnapshot &other) const { return !(*this == other); }
	};
	LcdSnapshot getLcdSnapshot() const;

	// Mirrors pressing "Master Volume" on real hardware to return the LCD to its default view.
	void resetDisplayToMainMode();

	// The plugin opens powered OFF, and clicking POWER boots the real Roland firmware live,
	// in real time, exactly as the hardware does - the D110Core machine is started here and
	// the front panel then shows its own LCD coming up. Switching off stops the machine.
	bool isPoweredOn() const { return poweredOn; }
	void setPoweredOn(bool shouldBePoweredOn);
	void togglePower() { setPoweredOn(!poweredOn.load()); }

	// The emulated D-110 control board: the firmware, its menus, its MSM6222B display and
	// its 16 panel buttons. Supplies everything mt32emu has no notion of.
	D110Core &getCore() { return core; }

	// Reads the LA engine's own copy of a parameter area back out. `packedAddress` is a
	// Roland address in mt32emu's packed form (three 7-bit bytes squeezed together, so
	// 0x040000 on the wire is 0x010000 here), NOT the three separate bytes emitRegionSysex
	// transmits. Returns false when no synth is open.
	//
	// Diagnostics only - the plugin never needs it - but it is what lets a test check the
	// firmware-to-engine bridge byte for byte instead of guessing from the sound. Both
	// halves load their tones from the same ROM, so where the bridge is correct the two
	// copies must be identical.
	bool readEngineMemory(juce::uint32 packedAddress, juce::uint32 length, juce::uint8 *out);

	// Where the firmware's battery RAM and memory card persist: beside the ROMs in the
	// plugin's own data folder, as with the other synths in this series. That folder sits
	// under Program Files and is not writable everywhere, so a write is tried once and the
	// user's app data stands in when it fails - nvramIsBesideRoms() says which is in use.
	//
	// ONE memory, shared and permanent, exactly as the instrument has one set of batteries.
	// It survives the host being closed, so the unit is found as it was left; a project
	// additionally carries its own copy, which is restored over this when it loads.
	static juce::File getNvramRoot();
	juce::File getNvramFolder() const;
	// False when the data folder turned out not to be writable and app data is being used
	// instead, so the panel's menu can say so rather than leave it a mystery.
	static bool nvramIsBesideRoms();

	// Set when a power-on was refused because another instance already holds the emulated
	// machine. Only one can run per process - see D110Core::start.
	bool isPowerBlocked() const { return powerBlocked; }
	// MAME rompath for the d110 romset: the plugin's own data folder first, then the
	// machine's standing MAME ROM folders so a development box works without copying.
	static juce::String getMameRomPath();

	// The panel draws its own VOLUME knob out of the reference photograph, so it talks to the
	// parameter directly rather than through a SliderAttachment.
	float getMasterVolume() const;
	void setMasterVolume(float newValue); // 0..1, with host automation gestures

	// Extracts SysEx messages from a .syx (raw concatenated dumps) or .mid/.smf (SysEx meta-events
	// only - notes are ignored) file and queues them to be sent to the synth. Safe to call from the
	// message thread: the messages are picked up and actually sent from processBlock on the audio thread.
	void importSysexBank(const juce::File &file);
	juce::String getLastImportMessage() const { return lastImportMessage; }

	// Patch browsing, matching the real D-110's PART + VALUE/NUMBER workflow: PART selects which
	// of the 8 Parts you're browsing, VALUE/NUMBER steps that Part's Patch/Program up or down.
	// Assumes the default factory MIDI channel assignment (Part i -> channel i+1), since the
	// SYSTEM page that would let you reassign channels isn't implemented yet.
	void selectNextPart();
	void selectPreviousPart();
	void stepPatch(int direction); // direction: +1 or -1

	// --- the extended editor ---------------------------------------------------
	// Everything the drawer edits goes to the INSTRUMENT, as a Roland DT1 into its own
	// MIDI IN - exactly what an external editor would send to the hardware. The firmware
	// changes its memory, the mirror carries that to the sound engine, and the panel's own
	// display shows it too. Nothing here writes to the sound engine directly, so an edit
	// made in the drawer and an edit made on the panel are the same event.
	//
	// Measured, not assumed: plugin/editor_write_probe.cpp sends one write into each area
	// and checks which byte of the firmware's battery RAM moved. All of them land where
	// Roland's map says, and Mem Protect does not stand in the way of exclusive writes.
	void sendAreaData(juce::uint32 sysexAddress, int offset, const juce::uint8 *data, int length);
	void sendTimbreTempParam(int part, int field, juce::uint8 value);
	void sendToneTempParam(int part, int offset, juce::uint8 value);
	void sendRhythmParam(int slot, int field, juce::uint8 value);
	void sendSystemParam(int field, juce::uint8 value);
	void sendTimbreMemoryParam(int slot, int field, juce::uint8 value);
	void sendPatchMemoryParam(int patch, int field, juce::uint8 value);
	// Имена - те же десять знаков, что показывает индикатор: только печатные ASCII, добито
	// пробелами. Прибор других не знает, и в эксклюзивном сообщении байт выше 0x7F невозможен.
	void sendName(juce::uint32 sysexAddress, int offset, const juce::String &name);
	// Надпись на индикаторе прибора - штатная команда Roland по адресу 0x200000.
	void sendDisplayMessage(const juce::String &text);

	// Смена тембра партии - обычная смена программы на её собственном MIDI-канале, как с
	// внешней клавиатуры. Канал берётся из карты прошивки, а не считается по формуле:
	// заводская раскладка «партия N на канале N+1» изменяема, и формула промахнулась бы.
	void selectTimbreForPart(int part, int timbre);
	// Переход на патч НАЖАТИЯМИ САМОЙ ПАНЕЛИ: Patch, затем Bank+ и Number+ столько раз,
	// сколько нужно. Патч выбирает прошивка - она при этом раскладывает его по временным
	// областям, пишет своё на индикатор и поднимает зеркало, - а не мы за неё.
	//
	// Bank+ двигает номер на 8, Number+ на 1 (измерено editor_write_probe), поэтому до
	// любого из 64 патчей не больше четырнадцати нажатий.
	void selectPatch(int patch);
	bool isSelectingPatch() const { return patchSteps > 0; }
	// Номер патча, который прибор играет сейчас (0..63), или -1, если память ещё не читалась.
	int currentPatchNumber() const;

	// Правка поля ХРАНИМОГО патча, слышная сразу - если правится тот патч, который прибор
	// сейчас и играет.
	//
	// На приборе это две разные вещи: играет он из временных областей, а память патча -
	// только слепок, который туда попадает при выборе патча. Поэтому правка одной лишь
	// памяти беззвучна, и редактор, который делает вид, будто это не так, врёт дважды: он
	// молчит там, где ждёшь звука, и меняет то, чего не слышно. Здесь пишутся ОБЕ копии,
	// когда речь о текущем патче, и только память - когда о любом другом.
	void editPatchField(int patch, int field, juce::uint8 value);

	// Прослушать тон из памяти: его 246 байт уходят во временную область партии, и партия
	// начинает играть им немедленно. Это ровно то, что делает «Recall», просто без кнопки.
	void auditionTone(int part, int slot);
	// И обратно: тон, которым партия играет сейчас, кладётся в ячейку памяти.
	void storeToneFromPart(int part, int slot);

	// Лента принятых сообщений для вкладки MONITOR - кольцо на 64 записи, без блокировок.
	struct MidiLogEntry {
		juce::uint8 status = 0, data1 = 0, data2 = 0;
		juce::uint16 size = 0;
	};
	int getMidiLog(MidiLogEntry *out, int max) const;

	juce::AudioProcessorValueTreeState parameters;

private:
	void closeSynth();
	void rebuildSampleRateConverter();
	void handleIncomingMidiMessage(const juce::MidiMessage &message);
	// Hands a host MIDI message to the emulated control board as well as to the sound
	// engine, so the firmware's own display tracks what is being played.
	void forwardMidiToFirmware(const juce::MidiMessage &message);

public:
	// Доходят ли note on/off до платы управления. Всегда да; выключение осталось только
	// для испытательных стендов и в интерфейс не выведено.
	//
	// Включено - это и есть поведение прибора: прошивка применяет свои диапазоны клавиш,
	// раскладку по партиям и распределение голосов, зажигает индикаторы в верхней строке
	// ЖКИ и сообщает обратно, какую ноту на какой партии она действительно взяла; только
	// после этого нота попадает в звуковой движок.
	//
	// Выключение когда-то было обходным путём: MAME не эмулирует LA32 ни для одной машины
	// Roland LA, и, дойдя до распределения голосов, прошивка переставала опрашивать
	// переднюю панель - звук шёл, а кнопки и экран умирали. Это чинит
	// D110Core::StuckPolicy::La32Stub, безусловно включаемый в setPoweredOn(); подробности
	// в docs/la32_interface.md. Стенды, которым нужна прошивка без нот
	// (plugin/longrun_test.cpp, plugin/hang_probe.cpp), пользуются этим сеттером напрямую.
	void setForwardNotesToFirmware(bool shouldForward) { forwardNotes = shouldForward; }
	bool getForwardNotesToFirmware() const { return forwardNotes; }

	// Sounds one part directly, with no firmware involvement - diagnostics only. It is the
	// only way to ask what the ENGINE does with a part, separately from whether the
	// firmware is sending it anything. Safe to call from a test's own thread because
	// nothing else is writing to the synth at the time; not for use from the plugin.
	void playNoteOnPartForTest(uint8_t part, uint8_t note, uint8_t velocity) {
		if (synth) synth->playMsgOnPart(part, 0x9, note, velocity);
	}
	void playNoteOffOnPartForTest(uint8_t part, uint8_t note) {
		if (synth) synth->playMsgOnPart(part, 0x8, note, 0);
	}
	// Which parts the ENGINE currently has sounding, as a bitmask - the counterpart to the
	// firmware's own indicators. Where the two disagree, the note reached the engine and
	// the engine chose not to sound it, which is a different fault from losing the note.
	// What the engine holds for a part, so it can be compared against what the firmware
	// holds for the same part. Diagnostics: a difference here is the bridge failing to
	// carry something across, which is invisible from either side alone.
	void engineReadMemory(uint32_t sysexAddr, uint32_t len, uint8_t *out) const {
		if (synth) synth->readMemory(sysexAddr, len, out);
	}
	// Пишет в память движка так же, как это делает мост, но без прошивки за спиной -
	// только для диагностики. Прочитанное значение само по себе не доказывает, что путь
	// чтения работает; доказывает записанное известное значение, прочитанное обратно, -
	// ради этого контрольного прогона метод и существует. Правило потоков то же, что у
	// playNoteOnPartForTest: собственный поток теста, и больше в синтезатор никто не пишет.
	void engineWriteSysexForTest(const uint8_t *data, int len) {
		if (synth) synth->playSysex(data, static_cast<MT32Emu::Bit32u>(len));
	}
	// Открылся ли звуковой движок вообще. Любое показание вида "в движке нули" обязано
	// сначала исключить это: readMemory() на закрытом синтезаторе возвращается, не тронув
	// буфер, и это читается как данные, если буфер и так был обнулён.
	bool engineIsOpen() const { return synth != nullptr; }
	uint32_t enginePartStates() const { return synth ? synth->getPartStates() : 0u; }
	uint32_t enginePartialCount() const { return synth ? synth->getPartialCount() : 0u; }
	// How many partials are busy right now. If this sits at the ceiling while a part is
	// silent, the part is being starved rather than ignored.
	int engineActivePartials() const {
		if (!synth) return -1;
		const uint32_t total = synth->getPartialCount();
		std::vector<MT32Emu::Bit8u> states(total, 0);
		synth->getPartialStates(states.data());
		int busy = 0;
		for (uint32_t i = 0; i < total; ++i)
			if (states[i] != 0) ++busy; // 0 is INACTIVE
		return busy;
	}

private:
	std::atomic<bool> forwardNotes{true};

	// Messages arriving on the directly-opened port. They are queued here and merged into
	// the next processBlock rather than acted on straight away, because that call arrives
	// on the OS's own MIDI thread and both the engine and the control board expect their
	// input from one place.
	void handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &) override;
	std::unique_ptr<juce::MidiInput> osMidiIn;
	std::unique_ptr<juce::MidiOutput> osMidiOut;
	juce::String selInputId, selOutputId;
	juce::CriticalSection osMidiLock;
	juce::MidiMessageCollector osMidiCollector;

	// Opens the synth from whatever is currently in controlRomData/pcmRomData.
	bool openSynthIfReady();
	// Scans getAutoRomFolder() for a Control ROM and PCM ROM by content (not filename) and
	// loads them automatically if both are found - so the user doesn't have to pick files by hand.
	bool tryAutoLoadRoms();
	// Second chance for the auto-scan: mt32emu wants one combined Control image and one
	// combined PCM image, but a MAME romset ships the individual chips instead. This looks
	// for those chips - loose in the folder or inside a d110 .zip - and joins them in memory
	// into exactly the images mt32emu expects. Returns whether both were assembled.
	bool tryAssembleRomsFromChipDumps(const juce::File &folder);
	// Reads `file` and hands it to mt32emu, returning its ROMInfo type via `typeOut`.
	bool identifyRomData(const juce::MemoryBlock &data, MT32Emu::ROMInfo::Type &typeOut) const;

	juce::MemoryBlock controlRomData, pcmRomData;
	std::unique_ptr<MT32Emu::ArrayFile> controlRomFile;
	std::unique_ptr<MT32Emu::ArrayFile> pcmRomFile;
	const MT32Emu::ROMImage *controlROMImage = nullptr;
	const MT32Emu::ROMImage *pcmROMImage = nullptr;
	std::unique_ptr<MT32Emu::Synth> synth;
	std::unique_ptr<MT32Emu::SampleRateConverter> sampleRateConverter;

	juce::String controlRomPath, pcmRomPath;
	juce::String controlRomDescription, pcmRomDescription;
	juce::String lastError;

	double currentSampleRate = 44100.0;
	std::vector<float> interleavedScratch;

	std::atomic<float> *masterVolumeParam = nullptr;
	std::atomic<float> *reverbEnabledParam = nullptr;
	std::atomic<float> *superModeParam = nullptr;
	bool lastSuperModeApplied = false;
	std::atomic<bool> poweredOn{false};

	// Guards both pending queues below. Synth's MIDI queue only tolerates a single writer thread
	// (the audio thread, via processBlock), so anything triggered from the message thread (button
	// clicks, file loads) is queued here and drained/applied from processBlock instead of calling
	// synth->playMsg()/playSysex() directly.
	juce::CriticalSection engineActionLock;
	std::vector<std::vector<MT32Emu::Bit8u>> pendingSysexImports;
	std::vector<MT32Emu::Bit32u> pendingShortMessages;
	juce::String lastImportMessage;

	D110Core core;

	bool powerBlocked = false;

	// Writes the saved firmware memory into this instance's folder, ready for the machine
	// to pick up next time it starts, and reads it back out again.
	void writeNvramFiles(const juce::MemoryBlock &rams, const juce::MemoryBlock &memcs) const;
	juce::MemoryBlock readNvramFile(const juce::String &name) const;

	// Where MAME keeps `rams` and `memcs` - one folder, shared, persistent.
	static juce::File getMachineNvramFolder();

	std::atomic<int> selectedPartIndex{0};
	std::array<int, 8> currentProgramPerPart{};

	// --- переход на патч кнопками панели --------------------------------------
	// Очередь кнопок, которые осталось нажать, и фаза текущего нажатия (0 - опустить,
	// 1 - отпустить). Живёт на таймере сообщений: между нажатиями обязано пройти
	// эмулируемое время, иначе матрица опроса просто не увидит вторую половину.
	void timerCallback() override;
	std::vector<int> patchQueue;
	int patchSteps = 0;
	int patchPhase = 0;

	// --- лента принятых сообщений ---------------------------------------------
	// Кольцо фиксированного размера, один писатель (аудиопоток) и один читатель (интерфейс),
	// поэтому без блокировок: писатель кладёт запись и только потом двигает счётчик.
	static constexpr juce::uint32 kMidiLogSize = 64;
	void logIncomingMidi(const juce::MidiMessage &message);
	std::array<MidiLogEntry, kMidiLogSize> midiLog{};
	std::atomic<juce::uint32> midiLogWrite{0};

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110AudioProcessor)
};
