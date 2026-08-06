#pragma once

#include <array>

#include <juce_gui_basics/juce_gui_basics.h>

#include "D110SequencerEngine.h"

class D110AudioProcessor;

// D-20-style sequencer drawer: a transport strip plus one row per track (D-110 parts
// 1-8, then rhythm). A third independently-foldable drawer, alongside D110EditorPane
// and D110Keyboard - see D110AudioProcessorEditor, which owns and positions this the
// same way it does those two. Talks to the processor only through getSequencer(), so
// this UI - like the engine itself - doesn't know anything about firmware RAM.
class D110SequencerPanel : public juce::Component, private juce::Timer {
public:
	explicit D110SequencerPanel(D110AudioProcessor &);
	~D110SequencerPanel() override;

	void paint(juce::Graphics &) override;
	void resized() override;
	void mouseDown(const juce::MouseEvent &) override;
	void mouseDrag(const juce::MouseEvent &) override;
	void mouseUp(const juce::MouseEvent &) override;
	void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

	// Reference height, in the same units as D110Panel::kRefH - what the owning editor
	// adds to its own layout, exactly as it already does for D110Keyboard::kRefH. Raised
	// ~30% from the original 250 (Alan: tracks were too thin to read comfortably).
	static constexpr float kRefH = 325.0f;

private:
	void timerCallback() override; // repaints the bar readout while the transport rolls

	d110seq::D110SequencerEngine &engine();
	void layout();
	void cycleTimeSignature();
	void showTimeSignatureMenu();
	void cycleRecordMode();
	void showRecordModeMenu();
	void showQuantizeMenu(int track);
	void confirmClearTrack(int track);
	void cycleLoopMode();
	void showBarMenu();
	void promptForBar();
	void promptForPunchRange();
	void confirmNewSong();

	D110AudioProcessor &processor;

	juce::Rectangle<float> stopBounds, playBounds, recBounds;
	juce::Rectangle<float> tempoBounds, timeSigBounds, metronomeBounds, precountBounds, loopBounds;
	juce::Rectangle<float> barPrevBounds, barNextBounds, barReadoutBounds;
	juce::Rectangle<float> loadBounds, saveBounds, recModeBounds, newBounds;
	// One button per song slot (see D110SequencerEngine::kNumSongSlots) - click to switch,
	// highlighted on whichever is current, with a small dot for slots that have content.
	std::array<juce::Rectangle<float>, d110seq::D110SequencerEngine::kNumSongSlots> slotBounds;
	// Visual metronome: one LED per metronome click in the bar (see
	// D110SequencerEngine::clicksPerBar/currentClickInBar), shown under the transport strip
	// whenever METRO is on.
	juce::Rectangle<float> metroLedBounds;

	struct TrackRow {
		// rowBounds spans the whole row - used only to catch a right-click anywhere on the
		// row for the quantize menu, regardless of which column it lands on.
		juce::Rectangle<float> rowBounds;
		juce::Rectangle<float> label, channelReadout, muteBounds, soloBounds, armBounds, activityBounds;
	};
	std::array<TrackRow, d110seq::D110SequencerEngine::kNumTracks> rows;

	bool draggingTempo = false;
	float tempoDragStartY = 0.0f;
	double tempoDragStartValue = 0.0;

	bool draggingBar = false;
	float barDragStartY = 0.0f;
	int barDragStartValue = 1;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110SequencerPanel)
};
