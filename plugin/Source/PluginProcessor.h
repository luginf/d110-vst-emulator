#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <mt32emu/mt32emu.h>
#include "D110Core.h"
#include <array>
#include <memory>

class D110AudioProcessor : public juce::AudioProcessor {
public:
	D110AudioProcessor();
	~D110AudioProcessor() override;

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

	// Where the firmware's battery RAM and memory card persist. Must be writable, so it is
	// under the user's app data rather than beside the ROMs in Program Files.
	static juce::File getNvramFolder();
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

	std::atomic<int> selectedPartIndex{0};
	std::array<int, 8> currentProgramPerPart{};

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110AudioProcessor)
};
