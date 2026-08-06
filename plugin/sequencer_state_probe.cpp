// Does a project actually recall the D-20-style sequencer? getStateInformation packs each
// track's MidiMessageSequence, tempo/time-signature and per-track mute/solo/quantize
// alongside the firmware NVRAM already covered by state_test.cpp; this checks that round
// trip in isolation - none of it depends on the machine being powered on, so no boot wait
// is needed here.
#include "Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check(bool condition, const char *what) {
	if (condition) {
		std::printf("  ok   %s\n", what);
	} else {
		std::printf("  FAIL %s\n", what);
		++failures;
	}
}
} // namespace

int main() {
	juce::ScopedJuceInitialiser_GUI juceInit;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	juce::MemoryBlock savedState;
	{
		D110AudioProcessor proc;
		proc.prepareToPlay(44100.0, 512);
		auto &eng = proc.getSequencer();

		eng.setTempo(97.0);
		eng.setTimeSignature(3, 4);
		eng.setMetronomeEnabled(false);
		eng.setPrecountBars(0);

		eng.armTrack(2);
		eng.startRecording();
		eng.captureEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
		eng.captureEvent(juce::MidiMessage::noteOff(1, 60), 0.5);
		eng.stopRecording();
		eng.setTrackMuted(2, true);
		eng.setTrackSoloed(4, true);
		eng.quantizeTrack(2, d110seq::QuantizeGrid::eighth);

		// D110Keyboard's own config - stored on the processor precisely so this round-trips
		// without an editor ever having existed (see PluginProcessor.h's own comment).
		proc.setKeyboardMidiChannel(7);
		proc.setKeyboardOmni(true);
		proc.setKeyboardPcInputEnabled(true);
		proc.setKeyboardPcLayout(1); // AZERTY

		// Utility tab's own settings - see PluginProcessor.h's comments on both for why they
		// live here rather than on the editor component itself.
		proc.setUiThemeLight(true);
		proc.setEditorPaneRefH(920.0f);

		proc.getStateInformation(savedState);
	}
	check(savedState.getSize() > 0, "getStateInformation produced data");

	D110AudioProcessor proc2;
	proc2.prepareToPlay(44100.0, 512);
	proc2.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));
	auto &eng2 = proc2.getSequencer();

	check(std::abs(eng2.getTempo() - 97.0) < 0.01, "tempo restored");
	check(eng2.getTimeSigNumerator() == 3 && eng2.getTimeSigDenominator() == 4, "time signature restored");
	check(!eng2.getMetronomeEnabled(), "metronome flag restored");
	check(eng2.getPrecountBars() == 0, "precount bars restored");
	check(eng2.trackHasEvents(2), "track 2 events restored");
	check(!eng2.trackHasEvents(0), "untouched track 0 stayed empty");
	check(eng2.isTrackMuted(2), "track 2 mute restored");
	check(eng2.isTrackSoloed(4), "track 4 solo restored");
	check(eng2.getTrackQuantize(2) == d110seq::QuantizeGrid::eighth, "track 2 quantize restored");
	check(proc2.getKeyboardMidiChannel() == 7, "keyboard MIDI channel restored");
	check(proc2.getKeyboardOmni(), "keyboard omni restored");
	check(proc2.getKeyboardPcInputEnabled(), "keyboard PC-input-enabled restored");
	check(proc2.getKeyboardPcLayout() == 1, "keyboard PC layout (AZERTY) restored");
	check(proc2.getUiThemeLight(), "UI theme (light) restored");
	check(std::abs(proc2.getEditorPaneRefH() - 920.0f) < 0.01f, "editor pane height restored");

	// All 4 song slots must round-trip, not just whichever one is current when the project
	// is saved - see D110SequencerEngine's slot* accessors and their use in get/
	// setStateInformation.
	juce::MemoryBlock slottedState;
	{
		D110AudioProcessor proc;
		proc.prepareToPlay(44100.0, 512);
		auto &eng = proc.getSequencer();
		eng.setPrecountBars(0);

		eng.setTempo(80.0);
		eng.armTrack(0);
		eng.startRecording();
		eng.captureEvent(juce::MidiMessage::noteOn(1, 61, (juce::uint8)90), 0.0);
		eng.captureEvent(juce::MidiMessage::noteOff(1, 61), 0.5);
		eng.stopRecording();

		eng.selectSongSlot(2);
		eng.setTempo(150.0);
		eng.setTimeSignature(6, 8);
		eng.armTrack(5);
		eng.startRecording();
		eng.captureEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8)90), 0.0);
		eng.captureEvent(juce::MidiMessage::noteOff(1, 72), 0.5);
		eng.stopRecording();

		eng.selectSongSlot(1); // leave a non-zero, non-content slot as the "current" one
		proc.getStateInformation(slottedState);
	}

	D110AudioProcessor proc3;
	proc3.prepareToPlay(44100.0, 512);
	proc3.setStateInformation(slottedState.getData(), static_cast<int>(slottedState.getSize()));
	auto &eng3 = proc3.getSequencer();

	check(eng3.getCurrentSongSlot() == 1, "the slot that was current when saved is current again");
	check(eng3.songSlotHasContent(0) && eng3.songSlotHasContent(2) && !eng3.songSlotHasContent(1),
	      "all 4 slots' content states came back correctly, not just the current one");
	check(std::abs(eng3.slotTempo(0) - 80.0) < 0.01 && std::abs(eng3.slotTempo(2) - 150.0) < 0.01,
	      "slot 0 and slot 2 kept their own separate tempo");
	check(eng3.slotTimeSigNumerator(2) == 6 && eng3.slotTimeSigDenominator(2) == 8,
	      "slot 2 kept its own time signature");
	eng3.selectSongSlot(0);
	check(eng3.trackHasEvents(0), "slot 0's track is playable after switching to it post-load");
	eng3.selectSongSlot(2);
	check(eng3.trackHasEvents(5), "slot 2's track is playable after switching to it post-load");

	// A project saved before the 4-slot feature existed has no "seqCurrentSlot" attribute,
	// and its song lives under the old flat names ("seqTempo", "seqTrack2", ...) rather than
	// "...Slot0...". Synthesize one from savedState's own slot-0 data (getStateInformation
	// itself only ever writes the new format now, so there's no other way to get a genuine
	// old-format blob) and check it still loads, landing in slot 0 rather than being
	// silently dropped.
	std::unique_ptr<juce::XmlElement> legacyXml(
		juce::AudioProcessor::getXmlFromBinary(savedState.getData(), static_cast<int>(savedState.getSize())));
	check(legacyXml != nullptr, "sanity: could parse the saved project's own XML");
	legacyXml->setAttribute("seqTempo", legacyXml->getDoubleAttribute("seqTempoSlot0"));
	legacyXml->setAttribute("seqTimeSigNum", legacyXml->getIntAttribute("seqTimeSigNumSlot0"));
	legacyXml->setAttribute("seqTimeSigDen", legacyXml->getIntAttribute("seqTimeSigDenSlot0"));
	for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t) {
		const juce::String slot0Suffix = "Slot0" + juce::String(t);
		legacyXml->setAttribute("seqMute" + juce::String(t), legacyXml->getIntAttribute("seqMute" + slot0Suffix));
		legacyXml->setAttribute("seqSolo" + juce::String(t), legacyXml->getIntAttribute("seqSolo" + slot0Suffix));
		legacyXml->setAttribute("seqQuantize" + juce::String(t),
		                        legacyXml->getIntAttribute("seqQuantize" + slot0Suffix));
		legacyXml->setAttribute("seqTrack" + juce::String(t),
		                        legacyXml->getStringAttribute("seqTrack" + slot0Suffix));
	}
	for (int i = legacyXml->getNumAttributes() - 1; i >= 0; --i) {
		const auto name = legacyXml->getAttributeName(i);
		if (name == "seqCurrentSlot" || name.contains("Slot")) legacyXml->removeAttribute(name);
	}
	check(!legacyXml->hasAttribute("seqCurrentSlot"), "sanity: synthetic legacy project has no seqCurrentSlot");

	juce::MemoryBlock legacyState;
	juce::AudioProcessor::copyXmlToBinary(*legacyXml, legacyState);

	D110AudioProcessor legacyProc;
	legacyProc.prepareToPlay(44100.0, 512);
	legacyProc.setStateInformation(legacyState.getData(), static_cast<int>(legacyState.getSize()));
	auto &engLegacy = legacyProc.getSequencer();
	check(engLegacy.getCurrentSongSlot() == 0, "a pre-slot project loads into slot 0");
	check(engLegacy.trackHasEvents(2) && std::abs(engLegacy.getTempo() - 97.0) < 0.01,
	      "a pre-slot project's song is intact after loading through the new slot-aware code");

	if (failures == 0) {
		std::printf("\nALL PASSED\n");
		return 0;
	}
	std::printf("\n%d CHECK(S) FAILED\n", failures);
	return 1;
}
