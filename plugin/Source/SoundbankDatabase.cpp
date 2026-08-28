#include "SoundbankDatabase.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace d110bank {

namespace {

// Same constants as D110Core.h's kSysexTones/kNumTones/kToneMemRecord, duplicated here rather
// than pulled in from D110Core.h - see this file's own header comment for why.
constexpr juce::uint32 kToneSysexAddress = 0x080000;
constexpr int kNumToneSlots = 64;
constexpr int kToneRecordSize = 256;

// Same three constants as D110Core.h's kSysexPatches/kNumPatches/kPatchRecord - kept for the
// dormant Patch decoder below (see SoundbankDatabase.h's own comment on why it's kept).
constexpr juce::uint32 kPatchSysexAddress = 0x060000;
constexpr int kNumPatches = 64;
constexpr int kPatchRecord = 128;

// Scans raw bytes for concatenated F0...F7 SysEx messages - same logic as
// PluginProcessor.cpp's own (file-local) extractSysexMessagesFromRawBytes(), duplicated
// rather than shared since that one is `static` to its own translation unit.
std::vector<std::vector<juce::uint8>> extractRawSysex(const juce::MemoryBlock &data) {
	std::vector<std::vector<juce::uint8>> messages;
	const auto *bytes = static_cast<const juce::uint8 *>(data.getData());
	const size_t len = data.getSize();

	size_t i = 0;
	while (i < len) {
		if (bytes[i] == 0xF0) {
			size_t j = i + 1;
			while (j < len && bytes[j] != 0xF7) ++j;
			if (j < len) {
				messages.emplace_back(bytes + i, bytes + j + 1);
				i = j + 1;
				continue;
			}
			break; // Unterminated trailing message - ignore it.
		}
		++i;
	}
	return messages;
}

// Same idea as PluginProcessor.cpp's own extractSysexMessagesFromMidiFile() - pulls just the
// SysEx meta-events out of a standard MIDI file, in file order.
std::vector<std::vector<juce::uint8>> extractMidiSysex(const juce::File &file) {
	std::vector<std::vector<juce::uint8>> messages;

	juce::FileInputStream stream(file);
	if (!stream.openedOk()) return messages;

	juce::MidiFile midiFile;
	if (!midiFile.readFrom(stream)) return messages;

	for (int t = 0; t < midiFile.getNumTracks(); ++t) {
		const auto *track = midiFile.getTrack(t);
		for (int e = 0; e < track->getNumEvents(); ++e) {
			const auto &message = track->getEventPointer(e)->message;
			if (!message.isSysEx()) continue;
			const auto *raw = message.getRawData();
			messages.emplace_back(raw, raw + message.getRawDataSize());
		}
	}
	return messages;
}

// Trims a raw name field at the first non-printable byte, shared by both decoders below.
juce::String trimmedName(const juce::uint8 *data, int nameChars) {
	juce::String name;
	for (int c = 0; c < nameChars; ++c) {
		const char ch = char(data[c]);
		if (ch < 32 || ch > 126) break;
		name << ch;
	}
	return name.trim();
}

// Decodes one raw F0...F7 SysEx message into zero or more whole 256-byte Tone Memory records -
// zero if it isn't a Roland DT1 write, isn't addressed inside the internal Tone area, or fails
// the Roland checksum; possibly more than one if the message (a real captured "dump the whole
// bank" capture) carries several consecutive records back to back. Mirrors the address packing
// D110Core::buildDt1Message() (D110Core.cpp) uses to build these messages in the first place,
// inverted to decode one back.
std::vector<DecodedTone> decodeTonesFromMessage(const juce::uint8 *msg, size_t len) {
	std::vector<DecodedTone> result;
	if (len < 11) return result; // F0,41,dev,model,12,a1,a2,a3,<>=1 data byte,checksum,F7
	if (msg[0] != 0xF0 || msg[len - 1] != 0xF7) return result;
	if (msg[1] != 0x41) return result; // Roland
	// msg[2] is the unit's own device ID (0-31 or "all") - varies per captured instrument,
	// not checked.
	if (msg[3] != 0x16) return result; // model: MT-32 family, which the D-110 answers to
	if (msg[4] != 0x12) return result; // DT1 ("Data set 1")

	const size_t dataLen = len - 10; // total - (F0,41,dev,model,12,a1,a2,a3) - (checksum,F7)
	if (dataLen < size_t(kToneRecordSize)) return result;

	const juce::uint8 a1 = msg[5] & 0x7f, a2 = msg[6] & 0x7f, a3 = msg[7] & 0x7f;
	const juce::uint32 target = (juce::uint32(a1) << 14) | (juce::uint32(a2) << 7) | a3;
	const juce::uint32 toneAreaBase = (((kToneSysexAddress >> 16) & 0x7f) << 14)
	                                 | (((kToneSysexAddress >> 8) & 0x7f) << 7)
	                                 | (kToneSysexAddress & 0x7f);
	if (target < toneAreaBase) return result;
	const juce::uint32 offset = target - toneAreaBase;
	if (offset % juce::uint32(kToneRecordSize) != 0
	    || offset >= juce::uint32(kNumToneSlots * kToneRecordSize))
		return result;

	// The checksum covers the address bytes plus every data byte in the message, computed
	// once for the whole message - not per-256-byte record, even if several are packed in.
	juce::uint32 sum = juce::uint32(a1) + a2 + a3;
	for (size_t i = 0; i < dataLen; ++i) sum += (msg[8 + i] & 0x7f);
	const juce::uint8 expectedChecksum = juce::uint8((128 - (sum & 0x7f)) & 0x7f);
	if ((msg[8 + dataLen] & 0x7f) != expectedChecksum) return result;

	// Two INDEPENDENT bounds (see the Patch decoder below's own comment - a real earlier bug
	// here was conflating these): `i` walks the MESSAGE's own data bytes (bounded by dataLen,
	// message-relative), while the tone slot INDEX it lands on (offset/256 + i) is bounded
	// separately by the Tone area's own total size (address-space-relative).
	for (size_t i = 0; (i + 1) * size_t(kToneRecordSize) <= dataLen
	                   && offset + juce::uint32(i) * juce::uint32(kToneRecordSize) + juce::uint32(kToneRecordSize)
	                          <= juce::uint32(kNumToneSlots * kToneRecordSize);
	     ++i) {
		DecodedTone t;
		std::memcpy(t.data, msg + 8 + i * size_t(kToneRecordSize), size_t(kToneRecordSize));
		t.name = trimmedName(t.data, 10);
		result.push_back(t);
	}
	return result;
}

// --- Patch-oriented decoding, ON HOLD (see SoundbankDatabase.h's own comment) --------------
std::vector<DecodedPatch> decodePatchesFromMessage(const juce::uint8 *msg, size_t len) {
	std::vector<DecodedPatch> result;
	if (len < 11) return result;
	if (msg[0] != 0xF0 || msg[len - 1] != 0xF7) return result;
	if (msg[1] != 0x41) return result;
	if (msg[3] != 0x16) return result;
	if (msg[4] != 0x12) return result;

	const size_t dataLen = len - 10;
	if (dataLen < size_t(kPatchRecord)) return result;

	const juce::uint8 a1 = msg[5] & 0x7f, a2 = msg[6] & 0x7f, a3 = msg[7] & 0x7f;
	const juce::uint32 target = (juce::uint32(a1) << 14) | (juce::uint32(a2) << 7) | a3;
	const juce::uint32 patchAreaBase = (((kPatchSysexAddress >> 16) & 0x7f) << 14)
	                                  | (((kPatchSysexAddress >> 8) & 0x7f) << 7)
	                                  | (kPatchSysexAddress & 0x7f);
	if (target < patchAreaBase) return result;
	const juce::uint32 offset = target - patchAreaBase;
	if (offset % juce::uint32(kPatchRecord) != 0
	    || offset >= juce::uint32(kNumPatches * kPatchRecord))
		return result;

	juce::uint32 sum = juce::uint32(a1) + a2 + a3;
	for (size_t i = 0; i < dataLen; ++i) sum += (msg[8 + i] & 0x7f);
	const juce::uint8 expectedChecksum = juce::uint8((128 - (sum & 0x7f)) & 0x7f);
	if ((msg[8 + dataLen] & 0x7f) != expectedChecksum) return result;

	for (size_t i = 0; (i + 1) * size_t(kPatchRecord) <= dataLen
	                   && offset + juce::uint32(i) * juce::uint32(kPatchRecord) + juce::uint32(kPatchRecord)
	                          <= juce::uint32(kNumPatches * kPatchRecord);
	     ++i) {
		DecodedPatch p;
		std::memcpy(p.data, msg + 8 + i * size_t(kPatchRecord), size_t(kPatchRecord));
		p.name = trimmedName(p.data, 10);
		result.push_back(p);
	}
	return result;
}

} // namespace

juce::String letterGroupFor(const juce::String &name) {
	const auto trimmed = name.trim();
	if (trimmed.isEmpty()) return "_";
	const auto c0 = trimmed[0];
	if (juce::CharacterFunctions::isLetter(c0))
		return juce::String::charToString(juce::CharacterFunctions::toUpperCase(c0));
	if (juce::CharacterFunctions::isDigit(c0)) return "0-9";
	return "_";
}

std::vector<DecodedTone> decodeTonesFromFile(const juce::File &file) {
	std::vector<std::vector<juce::uint8>> messages;
	if (file.hasFileExtension("mid") || file.hasFileExtension("smf"))
		messages = extractMidiSysex(file);
	if (messages.empty()) {
		juce::MemoryBlock raw;
		if (file.loadFileAsData(raw)) messages = extractRawSysex(raw);
	}

	std::vector<DecodedTone> result;
	for (const auto &m : messages) {
		auto tones = decodeTonesFromMessage(m.data(), m.size());
		result.insert(result.end(), tones.begin(), tones.end());
	}
	return result;
}

std::vector<DecodedPatch> decodePatchesFromFile(const juce::File &file) {
	std::vector<std::vector<juce::uint8>> messages;
	if (file.hasFileExtension("mid") || file.hasFileExtension("smf"))
		messages = extractMidiSysex(file);
	if (messages.empty()) {
		juce::MemoryBlock raw;
		if (file.loadFileAsData(raw)) messages = extractRawSysex(raw);
	}

	std::vector<DecodedPatch> result;
	for (const auto &m : messages) {
		auto patches = decodePatchesFromMessage(m.data(), m.size());
		result.insert(result.end(), patches.begin(), patches.end());
	}
	return result;
}

juce::File Database::defaultRoot() {
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
		.getChildFile("D-110 Emulator")
		.getChildFile("soundbanks_db");
}

Database::Database(juce::File dbRoot) : root(std::move(dbRoot)) {}

void Database::load() {
	entries.clear();
	if (!indexFile().existsAsFile()) return;

	const auto parsed = juce::JSON::parse(indexFile());
	if (const auto *arr = parsed.getArray()) {
		for (const auto &v : *arr) {
			Entry e;
			e.displayName = v.getProperty("displayName", {}).toString();
			e.rawName = v.getProperty("rawName", {}).toString();
			e.file = root.getChildFile(v.getProperty("file", {}).toString());
			e.sourceFile = v.getProperty("sourceFile", {}).toString();
			entries.push_back(std::move(e));
		}
	}
}

void Database::saveIndex() const {
	juce::Array<juce::var> arr;
	for (const auto &e : entries) {
		auto *obj = new juce::DynamicObject();
		obj->setProperty("displayName", e.displayName);
		obj->setProperty("rawName", e.rawName);
		obj->setProperty("file", e.file.getRelativePathFrom(root));
		obj->setProperty("sourceFile", e.sourceFile);
		arr.add(juce::var(obj));
	}
	root.createDirectory();
	indexFile().replaceWithText(juce::JSON::toString(juce::var(arr)));
}

int Database::rescan(const juce::File &sourceFolder) {
	load(); // start from whatever's already on disk so this merges rather than replaces

	// How many entries already share a given rawName, INCLUDING ones added earlier in this
	// same pass - drives the " (1)"/" (2)"/... suffix. The first entry under a name (n==0)
	// gets no suffix at all.
	std::map<juce::String, int> countPerName;
	for (const auto &e : entries) ++countPerName[e.rawName];

	// A real personal library is usually organised into subfolders (by device, by artist, by
	// pack, ...) - RangedDirectoryIterator's own `isRecursive` flag walks all of them, with the
	// extension check done explicitly here (juce::File::hasFileExtension takes the same
	// semicolon-separated list findChildFiles' wildcard argument would, but without that
	// method's own recursive+wildcard interaction to have to trust).
	// FollowSymlinks::no: nothing about a tone library needs a symlink followed, and a
	// symlink loop under an unbounded recursive walk is a real (if unlikely) way to never
	// return / exhaust memory - not worth the risk for a feature that has no reason to need it.
	juce::Array<juce::File> files;
	for (const auto &entry : juce::RangedDirectoryIterator(sourceFolder, true, "*", juce::File::findFiles,
	                                                        juce::File::FollowSymlinks::no))
		if (entry.getFile().hasFileExtension("syx;mid;midi;smf")) files.add(entry.getFile());

	int added = 0;
	for (const auto &f : files) {
		for (const auto &t : decodeTonesFromFile(f)) {
			const juce::String rawName = t.name.isEmpty() ? juce::String("(Unnamed)") : t.name;

			bool alreadyPresent = false;
			for (const auto &e : entries) {
				if (e.rawName != rawName) continue;
				juce::uint8 existing[kToneRecordSize];
				if (readToneBytes(e, existing) && std::memcmp(existing, t.data, kToneRecordSize) == 0) {
					alreadyPresent = true;
					break;
				}
			}
			if (alreadyPresent) continue;

			const int n = countPerName[rawName]++;
			const juce::String displayName =
				n == 0 ? rawName : rawName + " (" + juce::String(n) + ")";

			const auto letterDir = root.getChildFile(letterGroupFor(rawName));
			letterDir.createDirectory();
			// Just needs to be unique, not derived from content - juce::Uuid avoids pulling in
			// the whole juce_cryptography module (unused elsewhere in this project) for a hash.
			const auto uniqueName = juce::Uuid().toString().substring(0, 12);
			const auto file = letterDir.getChildFile(uniqueName + ".d110tone");
			file.replaceWithData(t.data, size_t(kToneRecordSize));

			entries.push_back({ displayName, rawName, file, f.getFullPathName() });
			++added;
		}
	}

	if (added > 0) saveIndex();
	return added;
}

std::vector<const Entry *> Database::byLetter(const juce::String &group) const {
	std::vector<const Entry *> result;
	for (const auto &e : entries)
		if (letterGroupFor(e.rawName) == group) result.push_back(&e);
	std::sort(result.begin(), result.end(), [](const Entry *a, const Entry *b) {
		return a->displayName.compareIgnoreCase(b->displayName) < 0;
	});
	return result;
}

int Database::countForLetter(const juce::String &group) const {
	int n = 0;
	for (const auto &e : entries)
		if (letterGroupFor(e.rawName) == group) ++n;
	return n;
}

bool Database::readToneBytes(const Entry &entry, juce::uint8 *out256) {
	juce::MemoryBlock mb;
	if (!entry.file.loadFileAsData(mb) || mb.getSize() != size_t(kToneRecordSize)) return false;
	std::memcpy(out256, mb.getData(), size_t(kToneRecordSize));
	return true;
}

juce::File Favorites::defaultFile() {
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
		.getChildFile("D-110 Emulator")
		.getChildFile("soundbank_favorites.json");
}

Favorites::Favorites(juce::File f) : indexFile(std::move(f)) { load(); }

void Favorites::load() {
	favouriteFiles.clear();
	if (!indexFile.existsAsFile()) return;
	const auto parsed = juce::JSON::parse(indexFile);
	if (const auto *arr = parsed.getArray())
		for (const auto &v : *arr) favouriteFiles.emplace_back(v.toString());
}

void Favorites::save() const {
	juce::Array<juce::var> arr;
	for (const auto &f : favouriteFiles) arr.add(f.getFullPathName());
	indexFile.getParentDirectory().createDirectory();
	indexFile.replaceWithText(juce::JSON::toString(juce::var(arr)));
}

bool Favorites::contains(const Entry &entry) const {
	for (const auto &f : favouriteFiles)
		if (f == entry.file) return true;
	return false;
}

void Favorites::toggle(const Entry &entry) {
	for (size_t i = 0; i < favouriteFiles.size(); ++i) {
		if (favouriteFiles[i] != entry.file) continue;
		favouriteFiles.erase(favouriteFiles.begin() + long(i));
		save();
		return;
	}
	favouriteFiles.push_back(entry.file);
	save();
}

} // namespace d110bank
