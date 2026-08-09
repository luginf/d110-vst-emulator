#pragma once

// The whole surface D110SequencerPanel actually needs from whatever it's embedded in -
// six required methods plus two optional ones below, deliberately, so the panel (like the
// engine underneath it) doesn't have to depend on the full D-110 plugin. D110AudioProcessor
// implements this for the plugin; NonetSeqHost implements it for Nonet Sequencer, the
// independent sequencer app, which has no firmware, no ROMs, and nothing else to power on.

#include <juce_core/juce_core.h>

namespace d110seq { class D110SequencerEngine; }

class D110SequencerHost {
public:
	virtual ~D110SequencerHost() = default;

	virtual d110seq::D110SequencerEngine &getSequencer() = 0;

	// All 4 song slots at once, as a standalone .midiseq file - see
	// D110SequencerSongsFile.h, which both implementations delegate to.
	virtual void exportSequencerSongs(const juce::File &file) = 0;
	virtual void importSequencerSongs(const juce::File &file) = 0;

	// Right-click STOP: all-notes-off, everywhere this host can reach - the firmware/sound
	// engine for the plugin, the direct MIDI Out port for both.
	virtual void midiPanic() = 0;

	// Where the panel's own file dialogs (Load/Save .mid, Load/Save .midiseq) should
	// start from and remember afterwards - one shared "last folder" per host, the same one
	// D110AudioProcessor's other dialogs (SysEx bank, memory snapshot) also read/write, so
	// picking a folder anywhere in the app carries over everywhere else instead of each
	// dialog independently defaulting back to $HOME.
	virtual juce::File getLastDialogDir() const = 0;
	virtual void setLastDialogDir(const juce::File &dir) = 0;

	// Optional: whether this host lets a track's MIDI channel be changed from the panel
	// itself (clicking its CH readout). False by default - the plugin's own channel map
	// comes from the live firmware's SYSTEM page instead, so D110AudioProcessor doesn't
	// override either of these. NonetSeqHost does: it has no firmware/SYSTEM page to set
	// channels from, so this is the only way to point a track anywhere but the factory
	// default (Part 1-8 -> channels 2-9, Rhythm -> 10).
	virtual bool supportsTrackChannelEdit() const { return false; }
	virtual void setTrackChannel(int /*track*/, int /*channel*/) {}
};
