#pragma once

// Abstract interface D110Keyboard talks to its owner through - same decoupling pattern
// as D110SequencerHost (see sequencer/D110SequencerHost.h) uses for D110SequencerPanel.
// D110AudioProcessor implements this for the plugin's own keyboard drawer, NonetSeqHost
// for the independent Nonet Sequencer app's keyboard.
class D110KeyboardHost {
public:
	virtual ~D110KeyboardHost() = default;

	virtual void injectTestNote(int channel, int note, float velocity, bool on) = 0;

	// D110Keyboard's own config (MIDI channel/omni, PC-keyboard tracker input,
	// QWERTY/AZERTY) - read once at construction and written back on every change, so it
	// survives as long as the host does.
	virtual int getKeyboardMidiChannel() const = 0;
	virtual void setKeyboardMidiChannel(int channel) = 0;
	virtual bool getKeyboardOmni() const = 0;
	virtual void setKeyboardOmni(bool omni) = 0;
	virtual bool getKeyboardPcInputEnabled() const = 0;
	virtual void setKeyboardPcInputEnabled(bool enabled) = 0;
	// 0 = QWERTY, 1 = AZERTY.
	virtual int getKeyboardPcLayout() const = 0;
	virtual void setKeyboardPcLayout(int layout) = 0;
};
