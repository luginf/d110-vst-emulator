#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

#include "D110KeyboardHost.h"

// On-screen test keyboard: a two-octave mouse piano plus optional tracker-style PC
// keyboard input (QWERTY/AZERTY, FastTracker2/Impulse Tracker convention), MIDI
// channel/omni set via right-click. Talks to its owner only through D110KeyboardHost,
// so it works identically inside the plugin's own drawer and inside the independent
// Nonet Sequencer app's window.
//
// Two ways to strike a key: the mouse, on the drawn keys, and - opt-in, right-click to
// enable - the computer keyboard, in the layout every tracker (FastTracker2, Impulse
// Tracker, OpenMPT, Renoise...) has used since the 1990s: two overlapping rows, the lower
// one ZSXDCVGBHNJM,L.;/ starting at the current base octave, the upper one Q2W3ER5T6Y7UI9O0P
// one octave above it. QWERTY and AZERTY differ only in which CHARACTER a given physical
// key sends, not in the note it plays, so trackerKeys() carries both and the chosen layout
// just picks which column to compare incoming key text against.
class D110Keyboard : public juce::Component {
public:
	explicit D110Keyboard(D110KeyboardHost &);
	~D110Keyboard() override;

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseExit(const juce::MouseEvent &) override;
	bool keyStateChanged(bool isKeyDown) override;
	void focusLost(juce::Component::FocusChangeType) override;

	// Reference height, in the same units as D110Panel::kRefH - what the owning window
	// adds to its own layout for this drawer/strip.
	static constexpr float kRefH = 130.0f;

private:
	static constexpr int kOctaves = 2;
	static constexpr int kLowestNote = 48; // C3

	enum class PcLayout { qwerty, azerty };

	struct KeyRect { juce::Rectangle<float> bounds; int note; bool black; };

	// One physical key of the tracker layout: the note it plays, relative to the keyboard's
	// current base octave, and which character it sends under each PC layout this offers.
	struct TrackerKey { int semitoneFromBase; juce::juce_wchar qwerty; juce::juce_wchar azerty; };
	static const std::vector<TrackerKey> &trackerKeys();

	void rebuildKeys();
	int keyAt(juce::Point<float>) const;
	void setHeldNote(int note); // -1 releases (mouse/touch - only one held note at a time)
	void changeOctave(int delta);
	void sendNote(int note, float velocity, bool on); // honours channel/omni
	void showContextMenu();
	void releaseAllPcNotes();

	D110KeyboardHost &host;
	int octaveShift = 0;
	int heldNote = -1;
	bool draggingKey = false; // mouse went down on a key, not on OCT-/OCT+

	int midiChannel = 1;        // 1..16 - which channel injectTestNote() targets
	bool omni = false;          // when on, every note goes out on all 16 channels at once
	bool pcKeyboardEnabled = false;
	PcLayout pcLayout = PcLayout::qwerty;
	// One flag per trackerKeys() entry, so keyStateChanged() (a single "something changed"
	// callback with no indication of which key) can diff against last-known state and fire
	// note-on/off only for the keys that actually moved. Polling juce::KeyPress rather than
	// overriding keyPressed(): that only fires on press, and a tracker needs release too.
	std::vector<bool> pcKeyDown;

	juce::Rectangle<float> captionBounds, octaveDownBounds, octaveUpBounds, keysBounds;
	std::vector<KeyRect> whiteKeys, blackKeys;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110Keyboard)
};
