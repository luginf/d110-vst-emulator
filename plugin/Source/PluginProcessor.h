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
                           private juce::MidiInputCallback {
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

	// Where the firmware's battery RAM and memory card persist. Must be writable, so it is
	// under the user's app data rather than beside the ROMs in Program Files.
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

	juce::AudioProcessorValueTreeState parameters;

private:
	void closeSynth();
	void rebuildSampleRateConverter();
	void handleIncomingMidiMessage(const juce::MidiMessage &message);
	// Hands a host MIDI message to the emulated control board as well as to the sound
	// engine, so the firmware's own display tracks what is being played.
	void forwardMidiToFirmware(const juce::MidiMessage &message);

public:
	// Whether note on/off reach the control board.
	//
	// OFF by default, and that default is not timidity - it is measured. Notes send the
	// firmware down its voice-allocation path, and that path ends at the LA32, which MAME
	// does not emulate for any Roland LA machine. The firmware does not crash; it stays
	// alive (its RAM keeps changing) but never returns to scanning the front panel, so
	// within a few seconds of playing every button and the whole display go dead while the
	// sound carries on. plugin/longrun_test.cpp reproduced it in about four seconds and
	// showed the panel surviving indefinitely once notes were withheld.
	//
	// 2026-07-31: fixed for real (docs/la32_interface.md) - D110Core::StuckPolicy::La32Stub
	// (set unconditionally in setPoweredOn()) supplies the sound board's missing side of
	// the handshake. Validated against a stress-chord test (30s survived, was 0-3s), a
	// realistic single-note test (120 notes over 60s, panel responsive throughout) and the
	// real firmware demo song (three songs back to back). Default is ON: lights the part
	// indicators, tracks Program Changes, and keeps the panel usable while playing - what
	// this flag traded away before now costs nothing. Left as a switch, not removed, in
	// case a host or a future ROM revision needs the old behaviour.
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

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110AudioProcessor)
};
