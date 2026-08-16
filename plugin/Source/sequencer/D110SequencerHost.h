#pragma once

// The whole surface D110SequencerPanel actually needs from whatever it's embedded in -
// six required methods plus six optional ones below, deliberately, so the panel (like the
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

	// Optional: whether this host lets the panel offer D110SequencerEngine's extra 7 tracks
	// (see D110SequencerEngine::kMaxTracks) at all. False by default - the D-110 plugin's
	// sequencer is always exactly the 9 D-110-shaped tracks (Parts 1-8 + Rhythm); the actual
	// enable/disable flag and track data live on the engine itself
	// (setExtraTracksEnabled()/getExtraTracksEnabled(), reachable via getSequencer()), so
	// this is purely a capability gate for the panel's own "activate extra tracks" right-click
	// menu and page-switch buttons - NonetSeqHost is the only override, returning true.
	virtual bool supportsExtraTracks() const { return false; }

	// Optional: whether this host lets a track have its own MIDI Program Change (0-127,
	// -1 = none), sent once over MIDI Out at the moment PLAY/REC starts - lets an external,
	// non-D-110 synth on that track's channel pick the right patch without a separate manual
	// step. False by default, same reasoning as supportsTrackChannelEdit(): the plugin's
	// tracks feed the live firmware directly, which already has its own patch per part, so
	// there's nothing for a Program Change to usefully select. NonetSeqHost is the only
	// override; the values themselves are real per-track state there (see setTrackProgram()),
	// not something read live off anything else.
	virtual bool supportsProgramChange() const { return false; }
	virtual int getTrackProgram(int /*track*/) const { return -1; }
	virtual void setTrackProgram(int /*track*/, int /*program*/) {}

	// Optional narrowing of supportsProgramChange() to specific tracks - default mirrors the
	// blanket flag. The D-110 plugin overrides this to exclude the Rhythm track: Program
	// Change picks one of a melodic Part's 128 stored Timbres, and Rhythm has no equivalent
	// single-number selection (each key already has its own sound) - see
	// D110AudioProcessor::supportsProgramChangeForTrack().
	virtual bool supportsProgramChangeForTrack(int track) const { return supportsProgramChange(); }

	// Bank Select (CC0, MSB only), sent immediately before the Program Change above, same
	// 1-128 musician-facing numbering. Only meaningful once a Program Change is actually set
	// (see getTrackProgram()) - defaults to bank 1 rather than "none" since most synths treat
	// bank 0/MSB-absent as bank 1 anyway, so there's no useful "unset" state to represent.
	virtual int getTrackBank(int /*track*/) const { return 1; }
	virtual void setTrackBank(int /*track*/, int /*bank*/) {}

	// Optional convenience hint for the Program Change dialog's pre-fill: -1 = no hint (leave
	// the field blank when no override is set yet - Nonet-Seq's original behaviour, since it
	// has no sound engine of its own to read a "current" value from). The D-110 plugin
	// overrides this to report the Part's own live tone number, so the dialog defaults to
	// "whatever this Part is playing right now" - see
	// D110AudioProcessor::getTrackProgramHint().
	virtual int getTrackProgramHint(int /*track*/) const { return -1; }

	// Per-song sound snapshot: the instrument's whole memory (every Patch, Timbre, Tone and
	// System setting - see docs/sysex_address_map.md), captured/restored per song slot so
	// switching which song is loaded can also recall which sounds it used, instead of the
	// Program Change override above (which is one global value regardless of song). Plugin-
	// only - Nonet-Seq has no firmware/memory of its own to snapshot, so these stay false/
	// no-op there, same reasoning as supportsTrackChannelEdit(). See
	// D110AudioProcessor::storeSoundSnapshotForSlot()/loadSoundSnapshotForSlot() and
	// D110SequencerPanel::showCopySongMenu(), which offers both from the song-slot buttons'
	// right-click menu.
	virtual bool supportsSoundSnapshots() const { return false; }
	virtual bool hasSoundSnapshot(int /*slot*/) const { return false; }
	virtual void storeSoundSnapshotForSlot(int /*slot*/) {}
	virtual void loadSoundSnapshotForSlot(int /*slot*/) {}
};
