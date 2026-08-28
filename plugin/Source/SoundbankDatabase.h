#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <vector>

// A persistent, deduplicated database of individual D-110 TONE records (256 bytes each, the
// shape of one internal Tone Memory slot - see D110Core.h's
// kSysexTones/kRamTones/kNumTones/kToneMemRecord/kNameChars), built by scanning a folder of
// Roland SysEx dumps for real. Alan's request, 2026-08-28: browse a big personal library by
// SOUND name (a Tone is the actual instrument sound - what "AC PIANO"/"Metal Harp" etc. name -
// not a Patch, which only combines/arranges existing Tones across parts and has no sound of
// its own; Alan corrected an earlier Patch-based version of this feature to target Tones
// instead, once he realised the SysEx files carry both and he wanted the sounds, not the
// arrangements), and inject one chosen tone either into one chosen internal Tone Memory slot,
// or straight into a part's live working tone for instant, non-destructive audition.
//
// Deliberately decoupled from D110Core/D110CoreNative - this class never touches a running
// instrument, only bytes on disk, so it's equally usable from the desktop plugin, Android,
// and plugin/soundbank_probe.cpp with no firmware/ROMs involved at all. The one thing it
// needs from the SysEx map is duplicated here rather than shared (kToneAreaBase/kToneRecordSize/
// kNumToneSlots in SoundbankDatabase.cpp) - same call already made for nameAt()-style name
// trimming elsewhere in this codebase (see D110EditorPane::nameAt(), PluginEditor.cpp).
namespace d110bank {

// One database entry. displayName may carry a "(1)"/"(2)" disambiguation suffix that the
// tone's own embedded 10-character name (rawName) never does - see Database::rescan()'s own
// comment for when that happens. The suffix exists only here, for browsing; the 256 raw bytes
// on disk (and therefore what gets injected) are never touched by it.
struct Entry {
	juce::String displayName;
	juce::String rawName;
	juce::File file;
	juce::String sourceFile; // which scanned file this tone came from - shown for provenance only
};

// Groups/sorts by the first letter of `name`: 'A'-'Z' for a leading letter (case-folded),
// "0-9" for a leading digit, "_" for anything else (symbols, or an empty/unnamed tone) -
// exactly the on-disk subfolder layout too (see Database::rescan()).
juce::String letterGroupFor(const juce::String &name);

// One decoded 256-byte Tone Memory record (10-byte name at offset 0, 246-byte tone body at
// offset 10 - the same body shape as Tone Temporary, D110Core.h's kToneRecord, which is what
// SoundbankBrowser's "audition" path writes straight into), with the name already trimmed -
// the output of decodeTonesFromFile(), exposed separately so plugin/soundbank_probe.cpp can
// also feed synthetic DT1 bytes straight to decodeTonesFromMessage() with no filesystem
// involved.
struct DecodedTone {
	juce::String name;
	juce::uint8 data[256];
};

// Scans one file (.syx, or .mid/.smf with embedded SysEx meta-events) for Roland DT1 messages
// addressed inside the Tone area (SysEx 08 00 00, 64 x 256 bytes = the internal Tone Memory
// group - Roland calls this Tone Group "i", the only one of the four groups a SysEx write can
// actually reach; groups a/b are ROM presets, group r is rhythm) and decodes every whole
// 256-byte record found - one DT1 message may carry one tone or, for a real "dump the whole
// bank" capture, many consecutive ones in a single message. Messages addressed outside the
// Tone area, or that fail the Roland checksum, are silently skipped - real capture files mix
// Patch/Timbre/Tone/System writes together and only Tone matters here.
std::vector<DecodedTone> decodeTonesFromFile(const juce::File &file);

// --- Patch-oriented decoding, ON HOLD ------------------------------------------------------
// The feature's first cut scanned PATCHES (128-byte Patch Memory records, SysEx 06 00 00) -
// Alan corrected this 2026-08-28 ("je veux les sons (tones), pas les patch"), but asked to
// keep this code rather than delete it, for when Patches get their own pass later. Not called
// by Database::rescan() below, and not otherwise wired into the UI - dead code, kept
// deliberately.
struct DecodedPatch {
	juce::String name;
	juce::uint8 data[128];
};
std::vector<DecodedPatch> decodePatchesFromFile(const juce::File &file);

class Database {
public:
	// Fixed, not user-configurable (only the SOURCE folder to scan is - see
	// D110AudioProcessor::getSoundbankSourceFolder()): this is an internal database, not
	// something Alan manages the location of directly. Same "D-110 Emulator" app-data
	// folder every other machine-wide setting already uses (getCustomRomPathFile() et al,
	// PluginProcessor.h).
	static juce::File defaultRoot();

	explicit Database(juce::File root = defaultRoot());

	// Reads index.json if present - one JSON parse, no folder walk, no source-file access at
	// all. Safe to call every time a browser/tab opens; this is the ONLY thing that runs on a
	// normal open, which is the whole point (Alan's explicit requirement: never slow down
	// loading, only rescan the source folder when asked).
	void load();

	// Recursively scans `sourceFolder` for *.syx/*.mid/*.smf, decodes every tone found, and
	// MERGES the result into whatever's already in the database (calls load() first) - an
	// existing entry with the exact same name AND byte content is left alone (already have
	// it); same name but different content becomes a new entry with a " (1)"/" (2)"/...
	// display suffix; a different name is always its own entry even if the bytes happen to
	// match another one already stored under a different name. Writes each newly-added
	// tone's raw 256 bytes to its own small file under the right A-Z/0-9/_ subfolder and
	// rewrites index.json once at the end. Never called automatically - only from an explicit
	// Rescan action. Slow for a big source folder (recursive file scan + a checksum decode
	// per SysEx message) - call this off the message thread.
	// Returns how many NEW entries were added, so the caller can report a count (Alan's
	// request, 2026-08-28: show how many sounds are available after a scan).
	int rescan(const juce::File &sourceFolder);

	int size() const { return int(entries.size()); }
	const std::vector<Entry> &all() const { return entries; }

	// Entries grouped under one letterGroupFor() key, alphabetical by displayName - what the
	// browser's per-letter jump list shows, along with countForLetter()'s own count (Alan's
	// request: show how many sounds are in each alphabetical subfolder when picking).
	std::vector<const Entry *> byLetter(const juce::String &group) const;
	int countForLetter(const juce::String &group) const;

	// Reads one entry's raw 256-byte record off disk - what the browser hands to
	// D110AudioProcessor::injectSoundbankTone()/auditionToneBytes(). False if the file is
	// missing or the wrong size (e.g. the database folder was tampered with outside the app).
	static bool readToneBytes(const Entry &entry, juce::uint8 *out256);

private:
	juce::File root;
	std::vector<Entry> entries;

	juce::File indexFile() const { return root.getChildFile("index.json"); }
	void saveIndex() const;
};

// A small list of favourited tones, Alan's request 2026-08-28: kept in its OWN file, entirely
// separate from Database's index.json and never touched by rescan()/saveIndex() - so it
// survives even a full database rebuild, and is meant to eventually support exporting the
// favourited tones as a standalone SysEx bank (not built yet - this class only tracks
// membership; export can reuse Database::readToneBytes() on each favourite's own `file` once
// that's wanted). Identity is a favourite's absolute on-disk path (Entry::file) - stable
// across rescans, since rescan() only ever ADDS entries, never moves or deletes one already
// on disk.
class Favorites {
public:
	static juce::File defaultFile();

	explicit Favorites(juce::File file = defaultFile());

	// Re-reads the favourites file - cheap, safe to call whenever a browser opens (mirrors
	// Database::load()'s own contract), in case another process (Android vs. the desktop
	// plugin, say) changed it.
	void load();

	bool contains(const Entry &entry) const;
	// Adds `entry` if it wasn't already a favourite, removes it if it was. Saves immediately.
	void toggle(const Entry &entry);

	const std::vector<juce::File> &files() const { return favouriteFiles; }
	int size() const { return int(favouriteFiles.size()); }

private:
	juce::File indexFile;
	std::vector<juce::File> favouriteFiles;

	void save() const;
};

} // namespace d110bank
