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
	// ~30% from the original 250 (Alan: tracks were too thin to read comfortably), then by
	// another 22 for the step-recording strip.
	static constexpr float kRefH = 347.0f;

private:
	void timerCallback() override; // repaints the bar readout while the transport rolls

	d110seq::D110SequencerEngine &engine();
	void layout();
	void cycleTimeSignature();
	void showTimeSignatureMenu();
	void cycleRecordMode();
	void showRecordModeMenu();
	void showQuantizeMenu(int track);
	void showMetronomeModeMenu();
	void confirmClearTrack(int track);
	// Right-click any song-slot button: copy the CURRENT song into one of the other 3 slots -
	// see D110SequencerEngine::copyCurrentSongTo().
	void showCopySongMenu();
	void confirmCopySongTo(int destSlot);
	// Right-click LOAD/SAVE: all 4 song slots at once (.d110songs), as opposed to the plain
	// click's single current song (.mid) - see D110AudioProcessor::exportSequencerSongs().
	void showLoadMenu();
	void showSaveMenu();
	void cycleLoopMode();
	void showBarMenu();
	void promptForBar();
	void promptForPunchRange();
	void confirmNewSong();
	// STEP-strip controls - see D110SequencerEngine::setStepDuration()/startStepRecording().
	void cycleStepDuration();
	void showStepDurationMenu();
	// track == -1 means every track at once (from the BAR readout's own menu); track >= 0
	// scopes the operation to just that one track (from a track row's right-click menu). See
	// D110SequencerEngine::deleteBars()/copyBars() for what "every track" vs. "just this one"
	// actually does to bar alignment.
	void promptForDeleteBars(int track);
	void promptForCopyBars(int track);
	// Same track==-1-means-every-track convention as the two above - see
	// D110SequencerEngine::transposeBars().
	void promptForTransposeBars(int track);

	D110AudioProcessor &processor;

	juce::Rectangle<float> stopBounds, playBounds, recBounds;
	juce::Rectangle<float> tempoBounds, timeSigBounds, metronomeBounds, precountBounds, loopBounds;
	juce::Rectangle<float> barPrevBounds, barNextBounds, barReadoutBounds;
	juce::Rectangle<float> loadBounds, saveBounds, recModeBounds, newBounds, undoBounds;
	// Step-recording strip, under the file strip - see D110SequencerEngine's step API.
	// stepInfoBounds is a plain readout (current bar/step while active), not clickable.
	juce::Rectangle<float> stepBounds, stepDurationBounds, stepDotBounds, restBounds, backBounds,
		stepInfoBounds;
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

	// Precount's downbeat-LED flash (see paint()'s own comment) - edge-detected in
	// timerCallback() against D110SequencerEngine::precountBeatsElapsed(), since positionBeats
	// itself is frozen throughout precount and can't be used to time this the way the normal
	// scrolling LED strip is.
	int lastPrecountBeatsElapsed = -1;
	juce::int64 precountFlashUntilMs = 0;

	bool draggingTempo = false;
	float tempoDragStartY = 0.0f;
	double tempoDragStartValue = 0.0;

	bool draggingBar = false;
	float barDragStartY = 0.0f;
	int barDragStartValue = 1;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D110SequencerPanel)
};
