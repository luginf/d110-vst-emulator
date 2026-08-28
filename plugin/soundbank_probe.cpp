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
	// A tone inside a .zip - Alan's request, 2026-08-28 (also the practical stand-in for "pick
	// a whole folder" on Android, where that isn't reliable).
	{
		juce::uint8 tone[256];
		makeTone(tone, "ZIPPED 1");
		juce::uint8 msg[512];
		const int len = buildDt1(kSysexTones, 0, tone, 256, msg);
		const auto innerSyx = tempRoot.getChildFile("_zip_inner.syx");
		innerSyx.replaceWithData(msg, size_t(len));

		juce::ZipFile::Builder zipBuilder;
		zipBuilder.addFile(innerSyx, 0, "inside/zipped.syx");
		const auto zipFile = sourceDir.getChildFile("bundle.zip");
		if (auto out = zipFile.createOutputStream()) zipBuilder.writeToStream(*out, nullptr);
		innerSyx.deleteFile();
	}

	d110bank::Database db(dbDir);
	const int firstScan = db.rescan(sourceDir);
	check(firstScan == 5, "first rescan added exactly 5 new entries (nested + zipped)");
	check(db.size() == 5, "database now holds 5 entries total");

	bool sawNested = false, sawZipped = false;
	for (const auto &e : db.all()) {
		if (e.displayName == "NESTED 1") sawNested = true;
		if (e.displayName == "ZIPPED 1") sawZipped = true;
	}
	check(sawNested, "a tone two subfolders deep was found by the recursive scan");
	check(sawZipped, "a tone inside a .zip was found and decoded");

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
	check(db.size() == 5, "database still holds exactly 5 entries after the second rescan");

	// A brand-new Database instance pointed at the same root loads the persisted index.json
	// alone, no source-file access at all - the "never slow down loading" requirement.
	d110bank::Database freshLoad(dbDir);
	freshLoad.load();
	check(freshLoad.size() == 5, "a fresh Database instance loads all 5 entries from index.json alone");

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

	// Favorites export (SysEx) - Alan's request 2026-08-28: sorted list, split into a file per
	// kNumToneSlots (64) tones once there are more than one file's worth ("mes favoris.syx" ->
	// "mes favoris_01.syx", "_02.syx", ... - his own example).
	{
		const auto exportDir = tempRoot.getChildFile("export_src");
		exportDir.createDirectory();

		std::vector<d110bank::Entry> tones;
		for (int i = 0; i < 70; ++i) {
			juce::uint8 data[256];
			const juce::String name = "FAV " + juce::String(i);
			makeTone(data, name.toRawUTF8());
			const auto file = exportDir.getChildFile("t" + juce::String(i) + ".d110tone");
			file.replaceWithData(data, 256);
			d110bank::Entry e;
			e.displayName = name;
			e.rawName = name;
			e.file = file;
			e.sourceFile = "test";
			tones.push_back(e);
		}

		const auto baseFile = tempRoot.getChildFile("mes favoris.syx");
		const int written = d110bank::exportTonesAsSysex(tones, baseFile);
		check(written == 2, "70 favorites split into exactly 2 files (64 + 6)");
		check(!baseFile.existsAsFile(), "base filename itself is NOT written when more than one file is needed");

		const auto file1 = tempRoot.getChildFile("mes favoris_01.syx");
		const auto file2 = tempRoot.getChildFile("mes favoris_02.syx");
		check(file1.existsAsFile() && file2.existsAsFile(), "both chunk files exist, correctly named/padded");

		const auto decoded1 = d110bank::decodeTonesFromFile(file1);
		const auto decoded2 = d110bank::decodeTonesFromFile(file2);
		check(int(decoded1.size()) == 64, "first chunk file decodes back to exactly 64 tones");
		check(int(decoded2.size()) == 6, "second chunk file decodes back to exactly 6 tones");
		check(!decoded1.empty() && decoded1[0].name == "FAV 0",
		      "first chunk's first tone is FAV 0, preserving the given order");
		check(!decoded2.empty() && decoded2[5].name == "FAV 69", "second chunk's last tone is FAV 69");

		// The <=64 case - Alan's own example filename, written with no suffix at all.
		const std::vector<d110bank::Entry> few(tones.begin(), tones.begin() + 3);
		const auto singleBase = tempRoot.getChildFile("small.syx");
		const int writtenSingle = d110bank::exportTonesAsSysex(few, singleBase);
		check(writtenSingle == 1 && singleBase.existsAsFile(),
		      "3 favorites (<=64) export to exactly one file, with no _01 suffix");
		check(d110bank::decodeTonesFromFile(singleBase).size() == 3, "that single file decodes back to 3 tones");
	}

	tempRoot.deleteRecursively();

	std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d FAILURE(S)\n", failures);
	return failures == 0 ? 0 : 1;
}
