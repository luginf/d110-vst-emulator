#pragma once

// (De)serializes the sequencer's 4 song slots (tempo, time signature, all 9 tracks each)
// to/from an XmlElement, and the standalone .midiseq file format built on top of it - an
// XML wrapper around a gzip+base64-packed standard MIDI file per track (see
// D110SequencerEngine::serializeTrack), not a raw memory/NVRAM-style snapshot, hence the
// name (renamed from .d110songs - importSongsFile() still reads that extension's files,
// the format itself never changed, only what this project calls it).
// Deliberately free of any D110AudioProcessor/firmware dependency - see the engine's own
// header comment ("could be lifted into a different project without a rewrite") - shared
// between D110AudioProcessor::getStateInformation/setStateInformation (a sub-block of the
// plugin's own project save) and D110AudioProcessor::exportSequencerSongs/
// importSequencerSongs (the whole document), and by the independent sequencer standalone
// app's own host, which has no firmware state to save alongside it.

#include <juce_core/juce_core.h>

#include "D110SequencerEngine.h"

namespace d110seq {

void writeSongsXml(const D110SequencerEngine &engine, juce::XmlElement &xml);
void readSongsXml(D110SequencerEngine &engine, const juce::XmlElement &xml);

// Returns a short status string suitable for showing the user either way (success or
// failure), the same wording D110AudioProcessor::lastImportMessage already used.
juce::String exportSongsFile(const D110SequencerEngine &engine, const juce::File &file);
juce::String importSongsFile(D110SequencerEngine &engine, const juce::File &file);

} // namespace d110seq
