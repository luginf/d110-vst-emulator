// d110bank::Database in isolation - no firmware, no ROMs, no running D110AudioProcessor at
// all (see SoundbankDatabase.h's own comment for why it's deliberately decoupled from
// D110Core/D110CoreNative). Builds synthetic Roland DT1 "Tone" SysEx messages by hand (same
// address-packing formula as D110Core::buildDt1Message, duplicated here rather than linking
// the whole core just for this one probe - it needs no firmware/ROMs at all otherwise), writes
// them to a temp folder as .syx files, and checks that rescan() finds/dedups/persists them the
// way SoundbankDatabase.h documents.
#include "Source/SoundbankDatabase.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

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

// Mirrors D110Core::buildDt1Message() (D110Core.cpp) exactly - duplicated rather than linked
// in, on purpose (see this file's own header comment).
int buildDt1(juce::uint32 sysexAddress, int offset, const juce::uint8 *data, int length,
             juce::uint8 *out) {
	int n = 0;
	out[n++] = 0xF0;
	out[n++] = 0x41;
	out[n++] = 0x10;
	out[n++] = 0x16;
	out[n++] = 0x12;
	const juce::uint32 linear = (((sysexAddress >> 16) & 0x7f) << 14)
	                           | (((sysexAddress >> 8) & 0x7f) << 7) | (sysexAddress & 0x7f);
	const juce::uint32 target = linear + juce::uint32(offset);
	const juce::uint8 a1 = juce::uint8((target >> 14) & 0x7f);
	const juce::uint8 a2 = juce::uint8((target >> 7) & 0x7f);
	const juce::uint8 a3 = juce::uint8(target & 0x7f);
	out[n++] = a1;
	out[n++] = a2;
	out[n++] = a3;
	juce::uint32 sum = juce::uint32(a1) + a2 + a3;
	for (int i = 0; i < length; ++i) {
		const juce::uint8 v = data[i] & 0x7f;
		out[n++] = v;
		sum += v;
	}
	out[n++] = juce::uint8((128 - (sum & 0x7f)) & 0x7f);
	out[n++] = 0xF7;
	return n;
}

constexpr juce::uint32 kSysexTones = 0x080000;

void makeTone(juce::uint8 *out256, const char *name) {
	std::memset(out256, 0, 256);
	std::memcpy(out256, name, std::min<size_t>(10, std::strlen(name)));
	// Real Roland SysEx data bytes are always 7-bit - `& 0x7f` keeps this filler realistic (and
	// round-trip-safe: an un-masked byte >127 would get silently masked on the wire like any
	// real data byte would, and then correctly no longer match this function's own unmasked
	// "expected" copy - a probe bug, not a Database one, caught by exactly this check).
	for (int i = 10; i < 256; ++i) out256[i] = juce::uint8(i & 0x7f); // arbitrary but deterministic
}

void writeSyx(const juce::File &file, juce::uint32 address, int offset, const juce::uint8 *tone) {
	juce::uint8 msg[512];
	const int len = buildDt1(address, offset, tone, 256, msg);
	file.replaceWithData(msg, size_t(len));
}
} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const auto tempRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
		.getChildFile("d110_soundbank_probe_"
		               + juce::String(juce::int64(juce::Time::getMillisecondCounterHiRes())));
	const auto sourceDir = tempRoot.getChildFile("source");
	const auto dbDir = tempRoot.getChildFile("db");
	sourceDir.createDirectory();

	// PIANO 1 - one clean tone.
	{
		juce::uint8 tone[256];
		makeTone(tone, "PIANO 1");
		writeSyx(sourceDir.getChildFile("bankA.syx"), kSysexTones, 0 * 256, tone);
	}
	// Two DIFFERENT tones both named "STRINGS" - same-name-different-content case, should
	// become "STRINGS" and "STRINGS (1)".
	{
		juce::uint8 tone[256];
		makeTone(tone, "STRINGS");
		tone[20] = 1;
		writeSyx(sourceDir.getChildFile("bankB.syx"), kSysexTones, 1 * 256, tone);
	}
	{
		juce::uint8 tone[256];
		makeTone(tone, "STRINGS");
		tone[20] = 2;
		writeSyx(sourceDir.getChildFile("bankC.syx"), kSysexTones, 2 * 256, tone);
	}
	// Exact duplicate of PIANO 1's bytes, in a different file at a different slot address -
	// must MERGE, not add a 4th entry.
	{
		juce::uint8 tone[256];
		makeTone(tone, "PIANO 1");
		writeSyx(sourceDir.getChildFile("bankD.syx"), kSysexTones, 5 * 256, tone);
	}
	// A message addressed OUTSIDE the Tone area (Timbre Temporary) - must be ignored entirely,
	// not miscounted as a 4th/5th tone.
	{
		juce::uint8 junk[16] = {};
		writeSyx(sourceDir.getChildFile("timbre.syx"), 0x030000, 0, junk);
	}
	// A tone sitting in a NESTED subfolder (real libraries are usually organised into
	// subfolders by device/pack/artist) - Alan's request, 2026-08-28: rescan() must recurse.
	{
		juce::uint8 tone[256];
		makeTone(tone, "NESTED 1");
		const auto nestedDir = sourceDir.getChildFile("pack").getChildFile("subpack");
		nestedDir.createDirectory();
		writeSyx(nestedDir.getChildFile("bankE.syx"), kSysexTones, 0 * 256, tone);
	}

	d110bank::Database db(dbDir);
	const int firstScan = db.rescan(sourceDir);
	check(firstScan == 4, "first rescan added exactly 4 new entries (including the nested one)");
	check(db.size() == 4, "database now holds 4 entries total");

	bool sawNested = false;
	for (const auto &e : db.all())
		if (e.displayName == "NESTED 1") sawNested = true;
	check(sawNested, "a tone two subfolders deep was found by the recursive scan");

	bool sawPiano = false, sawStrings = false, sawStrings1 = false;
	for (const auto &e : db.all()) {
		if (e.displayName == "PIANO 1") sawPiano = true;
		if (e.displayName == "STRINGS") sawStrings = true;
		if (e.displayName == "STRINGS (1)") sawStrings1 = true;
	}
	check(sawPiano, "PIANO 1 present, undisambiguated (the duplicate from bankD merged into it)");
	check(sawStrings, "first STRINGS present, undisambiguated");
	check(sawStrings1, "second (different-content) STRINGS present as \"STRINGS (1)\"");

	const auto letterP = db.byLetter("P");
	check(letterP.size() == 1 && letterP[0]->displayName == "PIANO 1", "byLetter(\"P\") finds PIANO 1 only");
	check(db.countForLetter("S") == 2, "countForLetter(\"S\") == 2 (both STRINGS entries)");
	check(db.countForLetter("Q") == 0, "countForLetter(\"Q\") == 0 (nothing there)");

	juce::uint8 reread[256] = {};
	bool foundPiano = false;
	for (const auto &e : db.all())
		if (e.displayName == "PIANO 1") foundPiano = d110bank::Database::readToneBytes(e, reread);
	juce::uint8 expected[256];
	makeTone(expected, "PIANO 1");
	check(foundPiano && std::memcmp(reread, expected, 256) == 0,
	      "readToneBytes() returns the exact original 256 bytes, byte-for-byte");

	// Re-running rescan on an unchanged source folder must add nothing (merge, not replace).
	const int secondScan = db.rescan(sourceDir);
	check(secondScan == 0, "a second rescan of the same folder adds 0 new entries");
	check(db.size() == 4, "database still holds exactly 4 entries after the second rescan");

	// A brand-new Database instance pointed at the same root loads the persisted index.json
	// alone, no source-file access at all - the "never slow down loading" requirement.
	d110bank::Database freshLoad(dbDir);
	freshLoad.load();
	check(freshLoad.size() == 4, "a fresh Database instance loads all 4 entries from index.json alone");

	// Favorites - a separate persisted list, Alan's request 2026-08-28.
	{
		const auto favFile = tempRoot.getChildFile("favorites.json");
		const d110bank::Entry *piano = nullptr;
		const d110bank::Entry *strings1 = nullptr;
		for (const auto &e : db.all()) {
			if (e.displayName == "PIANO 1") piano = &e;
			if (e.displayName == "STRINGS (1)") strings1 = &e;
		}
		check(piano != nullptr && strings1 != nullptr, "test fixture has the two entries favorites needs");

		d110bank::Favorites fav(favFile);
		check(fav.size() == 0, "a fresh Favorites file starts empty");
		check(!fav.contains(*piano), "PIANO 1 isn't a favorite yet");

		fav.toggle(*piano);
		check(fav.contains(*piano), "toggling PIANO 1 on makes it a favorite");
		check(fav.size() == 1, "favorites now holds exactly 1 entry");

		fav.toggle(*strings1);
		check(fav.size() == 2, "favorites now holds exactly 2 entries after adding a second");

		fav.toggle(*piano);
		check(!fav.contains(*piano), "toggling PIANO 1 again removes it");
		check(fav.contains(*strings1), "STRINGS (1) is untouched by toggling a different entry");
		check(fav.size() == 1, "favorites back down to exactly 1 entry");

		d110bank::Favorites reloaded(favFile);
		check(reloaded.size() == 1 && reloaded.contains(*strings1),
		      "a fresh Favorites instance loads the persisted file correctly");
	}

	tempRoot.deleteRecursively();

	std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d FAILURE(S)\n", failures);
	return failures == 0 ? 0 : 1;
}
