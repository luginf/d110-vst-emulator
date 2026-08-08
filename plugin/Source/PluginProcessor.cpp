#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// Scans raw bytes for concatenated F0...F7 SysEx messages, e.g. the contents of a bare .syx file.
static std::vector<std::vector<MT32Emu::Bit8u>> extractSysexMessagesFromRawBytes(const juce::MemoryBlock &data) {
	std::vector<std::vector<MT32Emu::Bit8u>> messages;
	const auto *bytes = static_cast<const MT32Emu::Bit8u *>(data.getData());
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

// Pulls out just the SysEx meta-events from a standard MIDI file, in file order. Note/CC/etc.
// events are intentionally ignored - this is for bank/patch dumps distributed as .mid files,
// not for playing the file as a song.
static std::vector<std::vector<MT32Emu::Bit8u>> extractSysexMessagesFromMidiFile(const juce::File &file) {
	std::vector<std::vector<MT32Emu::Bit8u>> messages;

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

static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout() {
	std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

	// 0..2 with unity as the default, so unity is the exact middle of the range and the
	// panel knob therefore rests pointing straight up - the reference photo's printed scale
	// is centred on vertical to within 0.03 degrees, and the knob draws whatever fraction of
	// its travel the parameter is at. With a 0..1 range the default of unity WAS the maximum,
	// so the knob sat hard clockwise: correct arithmetic, wrong-looking instrument.
	//
	// The consequence, stated plainly rather than avoided: the top half of the travel is
	// boost, and running it wide open is +6 dB and will clip a busy patch. That is what a
	// hardware volume control does too, and it is the player's choice; the default loudness
	// is unchanged either way. (An earlier revert to 0..1 traded the knob's rest position
	// away to prevent that, which fixed nothing that was broken.)
	//
	// Safe for existing projects: the value tree stores the real value, not a normalised
	// one, so a session saved at 1.0 still loads as unity.
	params.push_back(std::make_unique<juce::AudioParameterFloat>(
		juce::ParameterID{"masterVolume", 1}, "Master Volume",
		juce::NormalisableRange<float>(0.0f, 2.0f), 1.0f));

	params.push_back(std::make_unique<juce::AudioParameterBool>(
		juce::ParameterID{"reverbEnabled", 1}, "Reverb", true));

	params.push_back(std::make_unique<juce::AudioParameterBool>(
		juce::ParameterID{"superMode", 1}, "Super Mode (unofficial)", false));

	return {params.begin(), params.end()};
}

juce::AudioProcessor::BusesProperties D110AudioProcessor::createBuses() {
	// Bus 0 keeps its original name and stays default-on, unchanged from before individual
	// outputs existed - a project or host state that only knows this one bus is not affected
	// by there now being six more.
	BusesProperties buses;
	buses = buses.withOutput("Output", juce::AudioChannelSet::stereo(), true);
	for (int output = 1; output <= 6; ++output)
		buses = buses.withOutput("INDIVIDUAL " + juce::String(output), juce::AudioChannelSet::mono(), false);
	return buses;
}

bool D110AudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
	if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
	for (int bus = 1; bus < layouts.outputBuses.size(); ++bus) {
		const auto set = layouts.getChannelSet(false, bus);
		if (!set.isDisabled() && set != juce::AudioChannelSet::mono()) return false;
	}
	return true;
}

D110AudioProcessor::D110AudioProcessor()
	: AudioProcessor(createBuses()),
	  parameters(*this, nullptr, "PARAMS", createParameterLayout()) {
	masterVolumeParam = parameters.getRawParameterValue("masterVolume");
	reverbEnabledParam = parameters.getRawParameterValue("reverbEnabled");
	superModeParam = parameters.getRawParameterValue("superMode");

	// Reads sequencerLiveChannels rather than the core directly - see that array's own
	// comment. Set once here rather than per-block: the callback itself never changes,
	// only the array it closes over, which processBlock refreshes each block.
	sequencerLiveChannels.fill(-1);
	sequencerLiveChannels[d110seq::D110SequencerEngine::kRhythmTrack] = 10;
	sequencerEngine.setChannelSource(
		[this](int track) { return sequencerLiveChannels[static_cast<size_t>(track)]; });

	sequencerLivePrograms.fill(-1);
	sequencerEngine.setProgramSource(
		[this](int track) { return sequencerLivePrograms[static_cast<size_t>(track)]; });

	tryAutoLoadRoms();
}

D110AudioProcessor::~D110AudioProcessor() {
	closeSynth();
}

void D110AudioProcessor::closeSynth() {
	// Order matters: the synth must be destroyed before the ROM images it references,
	// and the ROM images must be destroyed before the file streams backing them.
	sampleRateConverter.reset();
	synth.reset();
	if (controlROMImage != nullptr) {
		MT32Emu::ROMImage::freeROMImage(controlROMImage);
		controlROMImage = nullptr;
	}
	if (pcmROMImage != nullptr) {
		MT32Emu::ROMImage::freeROMImage(pcmROMImage);
		pcmROMImage = nullptr;
	}
	controlRomFile.reset();
	pcmRomFile.reset();
}

void D110AudioProcessor::setControlRomPath(const juce::String &path) {
	if (!juce::File(path).loadFileAsData(controlRomData)) {
		lastError = "Could not read control ROM file: " + path;
		return;
	}
	controlRomPath = path;
	openSynthIfReady();
}

void D110AudioProcessor::setPcmRomPath(const juce::String &path) {
	if (!juce::File(path).loadFileAsData(pcmRomData)) {
		lastError = "Could not read PCM ROM file: " + path;
		return;
	}
	pcmRomPath = path;
	openSynthIfReady();
}

void D110AudioProcessor::setPoweredOn(bool shouldBePoweredOn) {
	if (poweredOn.load() == shouldBePoweredOn) return;

	// Booting is the real thing: the machine starts here and the firmware comes up on the
	// panel's own display in real time, not fast-forwarded.
	if (shouldBePoweredOn) {
		const auto nvram = getNvramFolder();
		nvram.getChildFile("d110").createDirectory();

		// The very first time this plugin is ever used there is no memory at all, and a
		// D-110 with blank battery RAM boots to an empty patch - it reads as broken rather
		// than as new. So perform the documented cold start, once. After that the file is
		// there and every later run, in this host session or any other, picks up exactly
		// where the last one left off.
		const bool virgin = !getMachineNvramFolder().getChildFile("rams").existsAsFile();

		// D110CoreNative wants the plain folder the ROM files sit loose in (same one
		// getAutoRomFolder() already points every other ROM-loading path at); D110Core wants
		// a MAME rompath, which is why getMameRomPath() mirrors those same files into a
		// MAME-shaped romset directory first. Two different search conventions for the same
		// underlying files, not two different ROM sets.
#ifdef D110_NATIVE_CORE
		materializeNativeRomFiles();
		const auto romArg = getAutoRomFolder().getFullPathName().toStdString();
#else
		const auto romArg = getMameRomPath().toStdString();
#endif
		if (!core.start(romArg, nvram.getFullPathName().toStdString())) {
			powerBlocked = true;
#ifdef D110_NATIVE_CORE
			// Unlike the MAME-backed core, D110CoreNative has no process-wide "only one
			// machine" lock - every instance is fully independent, so several can run at
			// once. A false return here can only mean one thing: it couldn't read one of
			// the three ROM files it needs from romArg.
			lastError = "Could not start the emulator: " + core.lastStartError()
			            + ". Check that the Control ROM, PCM ROM and character-generator ROM "
			              "are present in the data folder shown on the Utility tab.";
#else
			// The MAME-backed core does hold a single process-wide machine slot (see
			// D110Core::sMachineLive) - a second D110Emulator instance really can't run
			// alongside a first.
			lastError = "Another D-110 Emulator instance is already switched on. "
			            "Only one can run at a time - switch that one off first.";
#endif
			return;
		}
		powerBlocked = false;
		// The real sound-board interface, measured and fixed 2026-07-31 (see
		// docs/la32_interface.md): a wrong voice number, a MAME MCS-96 core bug (EXTINT's
		// pending-interrupt bit never clearing on take), and a too-narrow busy-value scan
		// were the three things standing between this and working audio-driven playback.
		// All three fixed; validated against a stress-chord test (30s, was 0-3s), a
		// realistic single-note test (120 notes over 60s) and the real demo song (three
		// songs back to back, panel responsive throughout). Safe to leave on unconditionally
		// - it only ever acts while the firmware is genuinely parked waiting for it.
		// La32Ramps (the real amplitude/filter envelope model - see D110CoreNative.h's own
		// comment on La32RampState) measurably fixes La32Stub's voice-starvation problem
		// (native_polyphony_stress_probe: 31/32 hardware slots stuck busy under La32Stub vs
		// 2/32 under La32Ramps after a 60-note run). A live DAW session previously hit a real
		// freeze under fast overlapping notes/chords that offline tests didn't reproduce at
		// the time; root-caused (native_ramp_edge_stress_probe.cpp - a ramp landing only
		// answers a voice's own envelope completion, never the separate per-note DISPATCH wait,
		// so a newly dispatched voice under heavy overlap could starve forever) and fixed by
		// also answering the dispatch handshake through the same channel.
		//
		// Native-only, deliberately: the same conceptual fix ported to D110Core.cpp (the
		// MAME-backed core) introduced a NEW, 100% reproducible regression there - the very
		// first note-off after every cold boot never completes (measured:
		// mame_stuck_note_repro.cpp; confirmed absent under La32Stub, present only under
		// La32Ramps, on identical boot/tone/note sequences; the new dispatch-ack code was
		// confirmed NOT to be the cause - it never even triggers in the failing runs, and
		// landed-ramp events look identical between a failing first note and a succeeding
		// later one). Root cause not yet isolated - suspected interaction between La32Ramps'
		// faster/more eager EXTINT responses and something in the MAME machine's own real-time
		// boot sequence, not reproducible on the native core's synchronous, thread-free
		// stepping. Reported by the owner: D110EmulatorNative plays cleanly, D110Emulator
		// (MAME-backed) sustains notes forever and freezes the LCD on the very first notes of
		// a session. Until root-caused, only the native core gets La32Ramps by default; the
		// MAME-backed core stays on the safe, long-proven La32Stub.
#ifdef D110_NATIVE_CORE
		core.setStuckPolicy(D110CoreType::StuckPolicy::La32Ramps);
#else
		core.setStuckPolicy(D110CoreType::StuckPolicy::La32Stub);
#endif
		if (virgin) core.factoryReset();
	} else {
		// Stopping is what makes MAME write its NVRAM out, so this is where the state
		// actually reaches disk and survives the host being closed.
		core.stop();
	}
	poweredOn = shouldBePoweredOn;
}

// Accepts both the underscore and space spellings of a plugin data folder name - some
// platforms/older builds used one or the other (see getAutoRomFolder() below) - so neither
// convention silently gets ignored. Prefers whichever actually has something in it - an
// empty folder under the "wrong" spelling (e.g. left behind by an older install, or created
// by hand while guessing at the name) must not shadow a populated one under the other
// spelling. Among two empty (or two populated) candidates, prefers the space variant; if
// neither exists at all, also returns the space variant, to create, matching this project's
// own established naming (D-110 Emulator, D-110 Data).
static bool folderIsPopulated(const juce::File &f) {
	return f.isDirectory() && f.getNumberOfChildFiles(juce::File::findFilesAndDirectories) > 0;
}

static juce::File resolveNamedFolder(const juce::File &parent, const juce::String &spaceName) {
	const auto spaced = parent.getChildFile(spaceName);
	const auto underscored = parent.getChildFile(spaceName.replaceCharacter(' ', '_'));
	const auto &isPopulated = folderIsPopulated;
	if (isPopulated(spaced)) return spaced;
	if (isPopulated(underscored)) return underscored;
	if (spaced.isDirectory()) return spaced;
	if (underscored.isDirectory()) return underscored;
	return spaced;
}

// The firmware's memory lives beside the ROMs, in the plugin's own data folder - so the
// D-110's battery RAM is simply `D-110 Data\nvram\d110\rams`.
//
// Named `mame_nvram` until 2026-08-06, matching a convention shared with this project's own
// MAME-based sibling emulators (MU-100R's `mame_nvram\mu100r\nvram`, QS300's
// `mame_nvram\qs300\ram`, etc.) - no longer apt now that D110EmulatorNative (MAME-free) is
// the default/shipping backend, and it was already inconsistent with the plain `nvram` name
// the app-data fallback below has always used. Renamed, with a one-time migration so an
// existing install's saved patches/timbres/memory card aren't silently orphaned under the old
// name.
juce::File D110AudioProcessor::getNvramRoot() {
	const auto romFolder = getAutoRomFolder();
	const auto preferred = romFolder.getChildFile("nvram");
	const auto legacyName = romFolder.getChildFile("mame_nvram");
	if (!preferred.isDirectory() && legacyName.isDirectory()) legacyName.moveFileTo(preferred);

	static const bool writable = [preferred] {
		preferred.createDirectory();
		const auto probe = preferred.getChildFile(".write-test");
		if (!probe.replaceWithText("x")) return false;
		probe.deleteFile();
		return true;
	}();
	if (writable) return preferred;

	return resolveNamedFolder(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory),
	                          "D-110 Emulator")
		.getChildFile("nvram");
}

bool D110AudioProcessor::nvramIsBesideRoms() {
	return getNvramRoot().isAChildOf(getAutoRomFolder());
}

// ONE memory, shared by every instance and persistent for good - the same arrangement as
// every other synth in this series, and the same as the instrument itself, which has one
// set of batteries and remembers what you left in it.
//
// It used to be one folder per instance, keyed by a fresh id. That was wrong twice over:
// a plugin loaded anew got a new id and therefore the factory state, so quitting the host
// and coming back lost everything, and the ids piled up as abandoned folders. Nothing was
// gained by it either, because only one instance can be switched on at a time anyway.
juce::File D110AudioProcessor::getNvramFolder() const {
	return getNvramRoot();
}

// Where MAME actually puts the two files, and therefore where a restored project's memory
// has to be written for the machine to pick it up.
juce::File D110AudioProcessor::getMachineNvramFolder() {
	return getNvramRoot().getChildFile("d110");
}

// MAME keeps each nvram_device in its own file under <nvram_directory>/<machine>/, so the
// D-110's battery RAM is "rams" and its memory card is "memcs", both raw 32 KB images.
void D110AudioProcessor::writeNvramFiles(const juce::MemoryBlock &rams,
                                         const juce::MemoryBlock &memcs) const {
	const auto dir = getMachineNvramFolder();
	dir.createDirectory();
	if (rams.getSize() > 0) dir.getChildFile("rams").replaceWithData(rams.getData(), rams.getSize());
	if (memcs.getSize() > 0) dir.getChildFile("memcs").replaceWithData(memcs.getData(), memcs.getSize());
}

juce::MemoryBlock D110AudioProcessor::readNvramFile(const juce::String &name) const {
	juce::MemoryBlock block;
	const auto file = getMachineNvramFolder().getChildFile(name);
	if (file.existsAsFile()) file.loadFileAsData(block);
	return block;
}

// The ROM files sit loose in the plugin's data folder, the same as every other synth in
// this series. The emulated control board, though, insists on MAME's own convention: a
// romset is either `d110.zip` or a directory literally named `d110`. Rather than push
// that layout onto the data folder, the plugin quietly mirrors the ROMs into a private
// working folder shaped the way the machine wants. Nothing about the data folder
// changes, and the copy is about 1.2 MB.
juce::String D110AudioProcessor::getMameRomPath() {
	const auto root = resolveNamedFolder(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory),
	                                     "D-110 Emulator").getChildFile("romset");
	const auto setDir = root.getChildFile("d110");
	setDir.createDirectory();

	// Only the sizes a D-110 chip can actually have, so nothing unrelated in the data
	// folder gets dragged along. Sub-folders are searched too, in case the user unpacked
	// the archive rather than emptying it out.
	auto isChipSize = [](juce::int64 n) {
		return n == 4096 || n == 32768 || n == 131072 || n == 524288;
	};

	const auto source = getAutoRomFolder();
	if (source.isDirectory())
		for (const auto &entry : juce::RangedDirectoryIterator(source, true, "*", juce::File::findFiles)) {
			const auto file = entry.getFile();
			if (!isChipSize(file.getSize())) continue;
			const auto target = setDir.getChildFile(file.getFileName());
			// Refresh only when it actually differs, so start-up is not a needless copy.
			if (!target.existsAsFile() || target.getSize() != file.getSize()
			    || target.getLastModificationTime() < file.getLastModificationTime())
				file.copyFileTo(target);
		}

	return root.getFullPathName();
}

juce::File D110AudioProcessor::getAutoRomFolder() {
	// Colocated with the platform's standard shared VST3 folder (see VST3_COPY_DIR in
	// plugin/CMakeLists.txt) - makes sense for the plugin, since every DAW scans there anyway,
	// and is the default location for a fresh install (checked first, below).
#if JUCE_WINDOWS
	const auto vst3Colocated = resolveNamedFolder(juce::File("C:/Program Files/Common Files/VST3"), "D-110 Data");
#elif JUCE_MAC
	const auto vst3Colocated = resolveNamedFolder(
		juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Audio/Plug-Ins/VST3"),
		"D-110 Data");
#else
	const auto vst3Colocated = resolveNamedFolder(
		juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".vst3"), "D-110 Data");
#endif

	// A second, per-OS "app data" location - the same root getNvramRoot()'s own fallback and
	// the Standalone's settings file already use - because asking a Standalone user (no VST3
	// host installed, possibly no ~/.vst3 at all) to put ROMs inside a VST3-specific folder
	// makes no sense for them. Only actually used if that is where the ROMs turn out to be;
	// the VST3-colocated folder above stays the default for a fresh install either way, so
	// nothing changes for anyone already using it.
	const auto appData = resolveNamedFolder(
		juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("D-110 Emulator"),
		"D-110 Data");

	if (folderIsPopulated(vst3Colocated)) return vst3Colocated;
	if (folderIsPopulated(appData)) return appData;
	return vst3Colocated;
}

bool D110AudioProcessor::identifyRomData(const juce::MemoryBlock &data,
                                         MT32Emu::ROMInfo::Type &typeOut) const {
	if (data.getSize() == 0) return false;
	MT32Emu::ArrayFile probe(static_cast<const MT32Emu::Bit8u *>(data.getData()), data.getSize());
	const MT32Emu::ROMImage *image = MT32Emu::ROMImage::makeROMImage(&probe);
	const MT32Emu::ROMInfo *info = image->getROMInfo();
	const bool recognised = info != nullptr;
	if (recognised) typeOut = info->type;
	MT32Emu::ROMImage::freeROMImage(image);
	return recognised;
}

bool D110AudioProcessor::tryAutoLoadRoms() {
	auto folder = getAutoRomFolder();
	if (!folder.isDirectory()) return false;

	// Recursive: an unpacked MAME romset sits in a sub-folder named after the set
	// (d110/), which is the layout the plugin's data folder normally has.
	for (const auto &entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles)) {
		auto file = entry.getFile();
		juce::MemoryBlock data;
		if (!file.loadFileAsData(data)) continue;

		MT32Emu::ROMInfo::Type type;
		if (!identifyRomData(data, type)) continue;

		if (type == MT32Emu::ROMInfo::Control && controlRomData.getSize() == 0) {
			controlRomData = std::move(data);
			controlRomPath = file.getFullPathName();
		} else if (type == MT32Emu::ROMInfo::PCM && pcmRomData.getSize() == 0) {
			pcmRomData = std::move(data);
			pcmRomPath = file.getFullPathName();
		}
	}

	// Nothing recognised as a whole image? The folder may still hold a MAME romset,
	// whose per-chip dumps have to be joined before mt32emu will know them. Also runs
	// whenever the BOSS ROM specifically is still missing: it is never a "whole image" as
	// far as identifyRomData() is concerned (it isn't Control or PCM), so the loop above
	// can never find it even when Control/PCM already came from whole-image files.
	if (controlRomData.getSize() == 0 || pcmRomData.getSize() == 0 || bossRomData.getSize() == 0)
		tryAssembleRomsFromChipDumps(folder);

	if (controlRomData.getSize() == 0 || pcmRomData.getSize() == 0) return false;
	return openSynthIfReady();
}

// A MAME d110 romset holds the D-110's chips separately, exactly as they sit on the
// board, while mt32emu wants the two images the board presents to the CPU. The joins
// below were verified byte for byte against the ROMs this plugin was already using:
//
//   Control = firmware (IC19, 32K) ++ presets (IC12, 128K)   -> SHA1 8d549f33...
//   PCM     = wave IC8 (512K)      ++ wave IC7 (512K)        -> SHA1 8eb2e385...
//
// Note the PCM order - IC8 first, then IC7. The other way round is not what mt32emu
// recognises. Chips are matched on content, so their filenames do not matter, and
// they may be loose in the folder or still inside the romset's .zip.
bool D110AudioProcessor::tryAssembleRomsFromChipDumps(const juce::File &folder) {
	struct Chip { const char *sha1; size_t size; };
	static const Chip kFirmwareV110 = { "28635510f30d6c1fb88e00da03e5b4e045c380cb", 32768 };
	static const Chip kFirmwareV106 = { "73b155fb0a8adc2362e73cb0803dafba9ccfb508", 32768 };
	static const Chip kPresets      = { "05587a0542b01625dcde37de5bb339880e47eb93", 131072 };
	static const Chip kWaveIc7      = { "6760d14900161b8715c2bfd4ebe997877087c90c", 524288 };
	static const Chip kWaveIc8      = { "9c59f50518a070461b2ec6cb4e43ee7cc1e905b6", 524288 };
	// IC6, the BOSS reverb chip's own program ROM (HG61H20R36F / BOS-007). Unlike the other
	// four chips this one is not folded into a Control/PCM image mt32emu already knows how
	// to identify - it goes to setBossReverbROM() separately, in openSynthIfReady() below.
	static const Chip kBoss         = { "17bd2887711c5c5458aba6d3be5972b2096eb450", 32768 };

	juce::MemoryBlock firmwareV110, firmwareV106, presets, waveIc7, waveIc8;

	auto consider = [&](const juce::MemoryBlock &data) {
		// mt32emu already carries a SHA1 implementation, so the chips are identified
		// with that rather than by adding another one here.
		MT32Emu::ArrayFile probe(static_cast<const MT32Emu::Bit8u *>(data.getData()), data.getSize());
		const juce::String digest(probe.getSHA1());
		auto take = [&](const Chip &chip, juce::MemoryBlock &into) {
			if (into.getSize() == 0 && data.getSize() == chip.size && digest == chip.sha1)
				into = data;
		};
		take(kFirmwareV110, firmwareV110);
		take(kFirmwareV106, firmwareV106);
		take(kPresets, presets);
		take(kWaveIc7, waveIc7);
		take(kWaveIc8, waveIc8);
		take(kBoss, bossRomData);
	};

	// Some ROM sets distribute two adjacent chips already concatenated into a single file -
	// most commonly the two PCM wave chips as one ~1MB dump, since that is the standard shape
	// PCM ROM images circulate in for MT-32/CM-32L (same physical chips this D-110 uses), quite
	// unlike MAME's own romsets, which always keep chips separate. Such a file matches neither
	// a whole-image checksum (identifyRomData(), above in tryAutoLoadRoms) nor a single chip's
	// size/SHA1 (consider(), above), so it silently went unrecognised. Splitting at every
	// boundary where the two halves' sizes match two known chips and re-running consider() on
	// each half recognises it exactly like MAME's separate dumps would - whichever order the
	// two chips were joined in, since consider() itself checks a half against every known chip
	// rather than assuming which one it must be.
	static const Chip *const kAllChips[] = {&kFirmwareV110, &kFirmwareV106, &kPresets,
	                                         &kWaveIc7,      &kWaveIc8,      &kBoss};
	auto considerConcatenated = [&](const juce::MemoryBlock &data) {
		for (const Chip *first : kAllChips) {
			if (first->size >= data.getSize()) continue;
			const size_t remainder = data.getSize() - first->size;
			for (const Chip *second : kAllChips) {
				if (second->size != remainder) continue;
				consider(juce::MemoryBlock(data.getData(), first->size));
				consider(juce::MemoryBlock(static_cast<const char *>(data.getData()) + first->size, remainder));
				return;
			}
		}
	};

	for (const auto &entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles)) {
		const auto file = entry.getFile();

		if (file.hasFileExtension("zip")) {
			juce::ZipFile zip(file);
			for (int i = 0; i < zip.getNumEntries(); ++i) {
				std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(i));
				if (stream == nullptr) continue;
				juce::MemoryBlock data;
				stream->readIntoMemoryBlock(data);
				consider(data);
				considerConcatenated(data);
			}
			continue;
		}

		juce::MemoryBlock data;
		if (file.loadFileAsData(data)) {
			consider(data);
			considerConcatenated(data);
		}
	}

	// v1.10 is the later firmware, so prefer it when both are present.
	const juce::MemoryBlock &firmware = firmwareV110.getSize() != 0 ? firmwareV110 : firmwareV106;

	if (controlRomData.getSize() == 0 && firmware.getSize() != 0 && presets.getSize() != 0) {
		juce::MemoryBlock joined(firmware);
		joined.append(presets.getData(), presets.getSize());
		MT32Emu::ROMInfo::Type type;
		if (identifyRomData(joined, type) && type == MT32Emu::ROMInfo::Control) {
			controlRomData = std::move(joined);
			controlRomPath = "(assembled from MAME chip dumps: firmware + presets)";
		}
	}

	if (pcmRomData.getSize() == 0 && waveIc7.getSize() != 0 && waveIc8.getSize() != 0) {
		juce::MemoryBlock joined(waveIc8);
		joined.append(waveIc7.getData(), waveIc7.getSize());
		MT32Emu::ROMInfo::Type type;
		if (identifyRomData(joined, type) && type == MT32Emu::ROMInfo::PCM) {
			pcmRomData = std::move(joined);
			pcmRomPath = "(assembled from MAME chip dumps: wave IC8 + IC7)";
		}
	}

	return controlRomData.getSize() != 0 && pcmRomData.getSize() != 0;
}

void D110AudioProcessor::materializeNativeRomFiles() {
	// controlRomData is always firmware (32768 bytes) followed by presets (131072 bytes) in
	// that order, whichever of the two paths above put it there - see the "Control = firmware
	// ++ presets" comment on tryAssembleRomsFromChipDumps.
	if (controlRomData.getSize() != 32768 + 131072) return;

	const auto folder = getAutoRomFolder();
	const auto firmwareFile = folder.getChildFile("d-110.v1.10.ic19.bin");
	const auto presetsFile = folder.getChildFile("r15179873-lh5310-97.ic12.bin");
	const auto *bytes = static_cast<const char *>(controlRomData.getData());
	if (!firmwareFile.existsAsFile()) firmwareFile.replaceWithData(bytes, 32768);
	if (!presetsFile.existsAsFile()) presetsFile.replaceWithData(bytes + 32768, 131072);
}

bool D110AudioProcessor::openSynthIfReady() {
	if (controlRomData.getSize() == 0 || pcmRomData.getSize() == 0) {
		lastError = "Waiting for both Control ROM and PCM ROM to be selected.";
		return false;
	}

	lastError.clear();

	// The ROM bytes are held in memory rather than read through a FileStream, because a
	// Control or PCM image may have been assembled from a MAME romset's separate chip
	// dumps and so never exists as a file on disk in the form mt32emu wants.
	auto newControlFile = std::make_unique<MT32Emu::ArrayFile>(
		static_cast<const MT32Emu::Bit8u *>(controlRomData.getData()), controlRomData.getSize());
	auto newPcmFile = std::make_unique<MT32Emu::ArrayFile>(
		static_cast<const MT32Emu::Bit8u *>(pcmRomData.getData()), pcmRomData.getSize());

	const MT32Emu::ROMImage *newControlImage = MT32Emu::ROMImage::makeROMImage(newControlFile.get());
	const MT32Emu::ROMImage *newPcmImage = MT32Emu::ROMImage::makeROMImage(newPcmFile.get());

	if (newControlImage->getROMInfo() == nullptr) {
		lastError = "Control ROM not recognised (unexpected size/checksum).";
		MT32Emu::ROMImage::freeROMImage(newControlImage);
		MT32Emu::ROMImage::freeROMImage(newPcmImage);
		return false;
	}
	if (newPcmImage->getROMInfo() == nullptr) {
		lastError = "PCM ROM not recognised (unexpected size/checksum).";
		MT32Emu::ROMImage::freeROMImage(newControlImage);
		MT32Emu::ROMImage::freeROMImage(newPcmImage);
		return false;
	}

	auto newSynth = std::make_unique<MT32Emu::Synth>();
	// Must happen before open() - initReverbModels() decides right there whether to build
	// the engine's own four modes or all eight of the real chip's, and open() is what calls
	// it. Absent or the wrong size, this is silently a no-op and the engine falls back to
	// its own four modes exactly as it did before the BOSS ROM was wired in.
	if (bossRomData.getSize() != 0)
		newSynth->setBossReverbROM(static_cast<const MT32Emu::Bit8u *>(bossRomData.getData()),
		                            static_cast<MT32Emu::Bit32u>(bossRomData.getSize()));
	const bool useSuper = superModeParam != nullptr && superModeParam->load() > 0.5f;
	if (!newSynth->open(*newControlImage, *newPcmImage, kExtendedPartialCount,
						 MT32Emu::AnalogOutputMode_COARSE, useSuper)) {
		lastError = "Synth failed to open with these ROM files.";
		MT32Emu::ROMImage::freeROMImage(newControlImage);
		MT32Emu::ROMImage::freeROMImage(newPcmImage);
		return false;
	}
	newSynth->setReverbEnabled(reverbEnabledParam == nullptr || reverbEnabledParam->load() > 0.5f);

	// Built here rather than via rebuildSampleRateConverter(), so the fully-formed engine -
	// synth AND converter together - is what gets handed over below, not the synth alone
	// followed by a gap before the converter catches up.
	auto newSampleRateConverter = std::make_unique<MT32Emu::SampleRateConverter>(
		*newSynth, currentSampleRate, MT32Emu::SamplerateConversionQuality_GOOD);

	const auto newControlRomDescription = newControlImage->getROMInfo()->description;
	const auto newPcmRomDescription = newPcmImage->getROMInfo()->description;

	// Everything above - ROM checksum/parsing, MT32Emu::Synth::open(), the sample-rate
	// converter - is the slow part, and none of it needs a lock: it's all local until here.
	// Only the handover does, because processBlock() (see synthAccessLock's declaration)
	// reads these same members under the same lock, for the whole of one block. Swap the new
	// engine in and the old one out here, then destroy the old one AFTER releasing the lock,
	// so a call arriving from the message thread (a live Super Mode toggle, see
	// superModeReopenPending in processBlock()) never makes the audio thread wait on
	// anything but the swap itself - not on freeing the engine it's replacing.
	std::unique_ptr<MT32Emu::Synth> oldSynth;
	std::unique_ptr<MT32Emu::SampleRateConverter> oldSampleRateConverter;
	std::unique_ptr<MT32Emu::ArrayFile> oldControlRomFile, oldPcmRomFile;
	const MT32Emu::ROMImage *oldControlImage = nullptr;
	const MT32Emu::ROMImage *oldPcmImage = nullptr;
	{
		const juce::ScopedLock sl(synthAccessLock);
		oldSynth = std::move(synth);
		oldSampleRateConverter = std::move(sampleRateConverter);
		oldControlRomFile = std::move(controlRomFile);
		oldPcmRomFile = std::move(pcmRomFile);
		oldControlImage = controlROMImage;
		oldPcmImage = pcmROMImage;

		controlRomFile = std::move(newControlFile);
		pcmRomFile = std::move(newPcmFile);
		controlROMImage = newControlImage;
		pcmROMImage = newPcmImage;
		synth = std::move(newSynth);
		sampleRateConverter = std::move(newSampleRateConverter);
		lastSuperModeApplied = useSuper;
		controlRomDescription = newControlRomDescription;
		pcmRomDescription = newPcmRomDescription;
	}

	// Same order as closeSynth(): the synth before the ROM images it references, the images
	// before the file streams backing them. A side effect worth calling out: unlike the old
	// closeSynth()-first sequence, a failed reopen above (bad ROM data, synth->open()
	// failing) never reaches here, so the previous, working engine is left running rather
	// than torn down for nothing.
	oldSampleRateConverter.reset();
	oldSynth.reset();
	if (oldControlImage != nullptr) MT32Emu::ROMImage::freeROMImage(oldControlImage);
	if (oldPcmImage != nullptr) MT32Emu::ROMImage::freeROMImage(oldPcmImage);
	oldControlRomFile.reset();
	oldPcmRomFile.reset();

	return true;
}

void D110AudioProcessor::rebuildSampleRateConverter() {
	if (!synth) {
		sampleRateConverter.reset();
		return;
	}
	sampleRateConverter = std::make_unique<MT32Emu::SampleRateConverter>(
		*synth, currentSampleRate, MT32Emu::SamplerateConversionQuality_GOOD);
}

void D110AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
	currentSampleRate = sampleRate;
	interleavedScratch.resize(static_cast<size_t>(samplesPerBlock) * 2);
	osMidiCollector.reset(sampleRate);
	if (synth) rebuildSampleRateConverter();

	// Switched on by default, at the user's request: a rack unit you have to reach for and
	// flip on every time you open the project is a step nobody wants between loading a
	// session and hearing it. setPoweredOn(true) is idempotent - already-on is a no-op - so
	// calling it on every prepareToPlay (a host may call this more than once) is harmless,
	// and it fails exactly as a manual click would if the ROMs are missing or another
	// instance already holds the machine: poweredOn stays false, the panel shows POWER up,
	// and the reason is on the right-click menu, same as before.
	setPoweredOn(true);

	// A note reaching the firmware is not the same event as a note reaching the engine
	// directly: it goes processBlock -> D110CoreType::pushMidi()'s ring buffer -> the machine
	// thread's own 3125Hz timer, shifted in exactly as a real MIDI cable would, then the
	// firmware's own dispatch code before the sound is actually audible. Measured
	// (plugin/note_latency_probe.cpp, isolated single notes, no MIDI congestion): 0-18ms,
	// close to uniformly spread rather than clustered around one number - the audio thread
	// and the machine thread are genuinely two different clocks, and this plugin was
	// reporting zero latency to the host regardless, so a DAW's own delay compensation
	// never accounted for any of it.
	//
	// setLatencySamples() cannot make the jitter itself disappear - PDC shifts everything
	// else by a FIXED amount, and the true delay here varies note to note. What it can fix
	// is the SYSTEMATIC part: without it, this plugin's audio was always emitted late
	// relative to a perfectly compensated track, never early. Reporting one block's worth
	// centres that jitter on the grid instead of trailing behind it - one block because
	// that quantisation (host hands us MIDI once per callback; the firmware's clock is not
	// the audio thread's clock) is the one piece of this delay that is not itself random.
	setLatencySamples(samplesPerBlock);
}

void D110AudioProcessor::releaseResources() {
}

void D110AudioProcessor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) {
	juce::ScopedNoDenormals noDenormals;
	const int numSamples = buffer.getNumSamples();
	buffer.clear();

	// Rechannelize host-fed external MIDI (a DAW routing a real controller to this plugin's
	// MIDI input, in VST3) onto the on-screen keyboard's own selected channel - Alan's own USB
	// keyboard is hardwired to channel 1, so this is what lets him record other sequencer
	// tracks/parts just by changing the on-screen keyboard's channel, without a controller
	// that can actually transmit on other channels. Omni intentionally passes this through
	// unchanged - there is no single target channel to remap to. Only channel-voice messages
	// (getChannel() > 0) are touched, matching the same idiom the sequencer's own
	// channelForTrack rechannelling already uses. This has to run before anything below adds
	// the on-screen keyboard's OWN notes (injectTestNote, merged in from osMidiCollector just
	// below) into midiMessages - those are already explicitly channelized (or looped across
	// all 16 channels for Omni) at the point of injection and must not be touched here. The
	// Standalone app's own directly-opened MIDI port is handled the same way, but earlier -
	// see handleIncomingMidiMessage(), which rechannelizes before a message ever reaches
	// osMidiCollector in the first place.
	if (!keyboardOmni) {
		juce::MidiBuffer rechannelized;
		for (const auto meta : midiMessages) {
			auto msg = meta.getMessage();
			if (msg.getChannel() > 0) msg.setChannel(keyboardMidiChannel);
			rechannelized.addEvent(msg, meta.samplePosition);
		}
		midiMessages.swapWith(rechannelized);
	}

	// Anything that arrived on the directly-opened port joins the host's own stream here,
	// so from this point on there is one queue and the rest of the method cannot tell the
	// two apart - which is right, since the hardware cannot either.
	{
		juce::MidiBuffer fromPort;
		osMidiCollector.removeNextBlockOfMessages(fromPort, numSamples);
		for (const auto meta : fromPort)
			midiMessages.addEvent(meta.getMessage(), meta.samplePosition);
	}

	// --- D-20-style sequencer: capture into the armed track, then layer in playback -----
	// Done here, ahead of the synth/power guard just below, so the transport's own clock
	// keeps ticking regardless of power state - matching real hardware, where a sequencer
	// isn't gated by anything else. Recorded and played-back notes are added straight into
	// midiMessages, so the streaming loop further down (and the firmware it feeds) cannot
	// tell them apart from host MIDI - same reasoning as the osMidiCollector merge above.
	// sequencerClicks is filled here but mixed into the OUTPUT buffer much further down,
	// after the synth has actually rendered into it.
	std::vector<d110seq::D110SequencerEngine::MetronomeClick> sequencerClicks;
	{
		// Refreshed once per block, not once per note - see sequencerLiveChannels' comment.
		if (core.isRunning()) {
			if (sequencerRamScratch.size() != static_cast<size_t>(D110CoreType::kRamSize))
				sequencerRamScratch.resize(static_cast<size_t>(D110CoreType::kRamSize));
			if (core.getRam(sequencerRamScratch.data())) {
				for (int t = 0; t < d110seq::D110SequencerEngine::kRhythmTrack; ++t) {
					const size_t chAddr = static_cast<size_t>(D110CoreType::kRamSystem + 13 + t);
					const int raw = chAddr < sequencerRamScratch.size()
					                    ? static_cast<int>(sequencerRamScratch[chAddr])
					                    : -1;
					// 0..15 = channel 1..16; 16 = the part is OFF, and anything else means
					// the read didn't land where expected - both fall back to the engine's
					// own default (channelForTrack) by staying -1.
					sequencerLiveChannels[static_cast<size_t>(t)] = (raw >= 0 && raw <= 15) ? raw + 1 : -1;

					// TimbreTemp field 1 = the part's live tone NUMBER (0-63), the same byte
					// D110EditorPane's Parts tab shows in its own TONE column - see
					// sequencerLivePrograms' own comment for why this, not the tone GROUP, is
					// what gets embedded as a Program Change.
					const size_t toneAddr = static_cast<size_t>(D110CoreType::kRamTimbreTemp)
					                       + static_cast<size_t>(t) * D110CoreType::kTimbreTempRecord + 1;
					const int tone = toneAddr < sequencerRamScratch.size()
					                      ? static_cast<int>(sequencerRamScratch[toneAddr])
					                      : -1;
					sequencerLivePrograms[static_cast<size_t>(t)] = (tone >= 0 && tone <= 63) ? tone : -1;
				}
			}
		}

		const double beatsPerSample = (sequencerEngine.getTempo() / 60.0) / currentSampleRate;
		const double windowStartBeats = sequencerEngine.getPositionBeats();
		const int armed = sequencerEngine.isRecording() ? sequencerEngine.getArmedTrack() : -1;
		if (armed >= 0) {
			const int armedChannel = sequencerEngine.channelForTrack(armed);
			for (const auto meta : midiMessages) {
				const auto &msg = meta.getMessage();
				if (msg.isNoteOnOrOff() && msg.getChannel() == armedChannel)
					sequencerEngine.captureEvent(
						msg, windowStartBeats + static_cast<double>(meta.samplePosition) * beatsPerSample);
			}
		}

		// Step recording doesn't care about beat position (see stepNoteOn()'s own comment), only
		// which notes were played and released - same armed-channel gating as real-time capture
		// above, just routed to the step API instead. Notes stay in midiMessages either way, so
		// they're still audible through the firmware while being entered, same as a real-time take.
		const int stepArmed = sequencerEngine.isStepRecording() ? sequencerEngine.getArmedTrack() : -1;
		if (stepArmed >= 0) {
			const int armedChannel = sequencerEngine.channelForTrack(stepArmed);
			for (const auto meta : midiMessages) {
				const auto &msg = meta.getMessage();
				if (msg.getChannel() != armedChannel) continue;
				if (msg.isNoteOn()) sequencerEngine.stepNoteOn(msg.getNoteNumber(), msg.getVelocity());
				else if (msg.isNoteOff()) sequencerEngine.stepNoteOff(msg.getNoteNumber());
			}
		}

		sequencerEngine.renderInto(midiMessages, numSamples, currentSampleRate,
		                            sequencerEngine.getMetronomeEnabled() ? &sequencerClicks : nullptr);
	}

	// Held for the rest of this block: openSynthIfReady() (below) can run concurrently on the
	// message thread and swaps `synth`/`sampleRateConverter` out from under us when Super
	// Mode is toggled live. It only ever costs this an O(1) pointer handover, never the
	// (slow) rebuild itself - see synthAccessLock's declaration.
	const juce::ScopedLock sl(synthAccessLock);

	if (!synth || !sampleRateConverter || !poweredOn.load()) {
		return;
	}

	// Super Mode can only be applied when the synth is (re)opened, not live. Rebuilding it -
	// ROM checksum/parsing plus a full MT32Emu::Synth::open() - is far too slow for the audio
	// thread's deadline: doing it right here, synchronously, is what actually produced the
	// xrun-then-the-host-kills-the-plugin failure this comment used to just warn about. Only
	// the mismatch is detected here; the rebuild itself runs on the message thread via
	// openSynthIfReady(), guarded by superModeReopenPending so a run already in flight isn't
	// queued a second time. This block, and any others before it lands, keep rendering with
	// whichever engine is currently installed - a toggle takes a few blocks to catch up
	// rather than landing instantly, which is the same "not live" limitation this always had.
	const bool wantSuper = superModeParam->load() > 0.5f;
	if (wantSuper != lastSuperModeApplied && !superModeReopenPending.exchange(true)) {
		std::weak_ptr<char> weakLife = lifeToken;
		juce::MessageManager::callAsync([this, weakLife] {
			if (weakLife.expired()) return; // the processor was destroyed before this ran
			openSynthIfReady();
			superModeReopenPending = false;
		});
	}

	synth->setReverbEnabled(reverbEnabledParam->load() > 0.5f);

	// Drain anything queued from the UI thread (SysEx bank imports, patch-browsing program
	// changes, stub-button LCD messages) and actually apply it here on the audio thread, since
	// Synth's MIDI queue only tolerates a single writer thread.
	std::vector<std::vector<MT32Emu::Bit8u>> pendingImportsToSend;
	std::vector<MT32Emu::Bit32u> pendingShortMessagesToSend;
	std::vector<juce::uint8> pendingPanicBytesToSend;
	{
		const juce::ScopedLock slEngine(engineActionLock);
		pendingImportsToSend.swap(pendingSysexImports);
		pendingShortMessagesToSend.swap(pendingShortMessages);
		pendingPanicBytesToSend.swap(pendingPanicBytes);
	}
	if (!pendingPanicBytesToSend.empty())
		core.pushMidi(pendingPanicBytesToSend.data(), static_cast<int>(pendingPanicBytesToSend.size()));
	// Эти две очереди намеренно идут через очередь движка с отметкой времени, а не через
	// «немедленные» формы, которыми пользуется мост ниже: их источник - действие
	// пользователя на панели, оно отстоит от любой ноты на секунды, и обгон в один блок
	// здесь ничего не решает. У моста иначе - там параметр и нота приходят в одну и ту же
	// миллисекунду, и порядок между ними значим.
	for (auto &message : pendingImportsToSend) {
		synth->playSysex(message.data(), static_cast<MT32Emu::Bit32u>(message.size()));
		// And to the control board, which is the half that actually owns this data. Sending
		// it only to the sound engine looked like it worked and did nothing: the firmware
		// never learned of the new patches, so the next RAM mirror overwrote them with the
		// firmware's unchanged state and the display never moved either. Pushed from here
		// rather than from importSysexBank because the ring has one producer by design,
		// and that producer is this thread.
		core.pushMidi(message.data(), static_cast<int>(message.size()));
	}
	for (auto message : pendingShortMessagesToSend)
		synth->playMsg(message);

	// The bridge: whenever the control board's own parameter memory changes - because a
	// button was pressed on the panel - the core hands us the Roland exclusive message
	// the hardware would have used, and the LA engine takes it natively. This is what
	// makes an edit made on the panel audible.
	//
	// playSysexNow, а не playSysex: последний ставит сообщение в собственную очередь
	// движка с отметкой времени, и применяется оно только когда расчёт звука до неё
	// дойдёт, - тогда как ноты ниже уходят через playMsgOnPart, который действует
	// НЕМЕДЛЕННО. Ноты обгоняли параметры, от которых зависят. В начале демо-песни это
	// стоило двух ударов: прошивка загружала карту ритма и почти сразу стучала по
	// клавишам 25 и 27, а движок применял удары раньше карты, видел там тембр 127 (OFF)
	// и молча их отбрасывал ("Attempted to play unmapped key"). Здесь оба пути
	// действуют в момент разбора очередей, и порядок этого цикла - параметры, потом
	// ноты - становится настоящим.
	{
		MT32Emu::Bit8u sysex[D110CoreType::kMaxSysexBytes];
		while (const int len = core.popSysex(sysex))
			synth->playSysexNow(sysex, static_cast<MT32Emu::Bit32u>(len));
	}

	// The other half of the same idea, for notes rather than parameters: the firmware tells
	// us which note it started on which part, and the engine plays it. This is what makes
	// the instrument's OWN demo songs audible - it generates those internally from ROM and
	// never transmits them, so before this they moved the part indicators in silence. It
	// also means host notes are voiced by the same path the panel is showing, with the
	// firmware's own key ranges, part assignment and voice allocation applied, instead of
	// the two halves being fed separately and drifting apart.
	//
	// playMsgOnPart addresses the part directly (0-7 voice, 8 rhythm), which is exactly
	// what the firmware hands over, so no channel-to-part mapping has to be re-derived here.
	// --- diagnostic instrumentation (2026-08-07), gated by debugModeEnabled (Utility tab ->
	// DEBUG) - off by default so nothing runs or hits disk on the audio thread unless a user
	// deliberately turns it on to help chase a real issue. Originally written to find the
	// note-dropout bug (see project_note_dropout_root_cause memory) and kept, rather than
	// deleted, in case a similar issue needs the same kind of measurement again. Two earlier
	// versions of the tracking below got the measurement wrong in opposite directions: v1 only
	// logged FAILURES (busy>0 while activePartials==0), which made a roughly-50%-of-notes
	// problem look like a permanent stuck state (successes were never written down at all);
	// v2 fixed that but checked the GLOBAL activePartials count, which meant ANY other
	// part/note still ringing at the same moment masked a specific note's own failure. This
	// version tracks each individual note-on PER PART (right where ev.part is already known,
	// below) and checks THAT part's own bit in enginePartStates() a few blocks later, so one
	// part's success can no longer hide another's failure.
	{
		D110CoreType::NoteEvent ev;
		while (core.popNoteEvent(ev)) {
			if (ev.part > 8) continue;
			synth->playMsgOnPart(ev.part, ev.on ? 0x9 : 0x8, ev.note, ev.velocity);
			if (debugModeEnabled && ev.on) diagPendingChecks.push_back({ ev.part, 3 });
		}
	}
	if (debugModeEnabled && core.isRunning()) {
		const uint32_t partStates = enginePartStates();
		for (size_t i = 0; i < diagPendingChecks.size();) {
			if (--diagPendingChecks[i].checksLeft <= 0) {
				++diagAttempts;
				if ((partStates >> diagPendingChecks[i].part) & 1u) ++diagHits; else ++diagMisses;
				diagPendingChecks.erase(diagPendingChecks.begin() + static_cast<long>(i));
			} else {
				++i;
			}
		}

		const juce::int64 nowMs = juce::Time::getMillisecondCounter();
		if (diagAttempts > 0 && nowMs - diagLastFlushMs > 3000) {
			diagLastFlushMs = nowMs;
			juce::File logFile = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
			                          .getChildFile("d110_diagnostic_log.txt");
			juce::String line;
			line << juce::Time::getCurrentTime().toString(true, true, true, true)
			     << "  TALLY (per-part)  attempts=" << diagAttempts << " hits=" << diagHits
			     << " misses=" << diagMisses << " ("
			     << juce::String(diagAttempts > 0 ? 100.0 * diagMisses / diagAttempts : 0.0, 1)
			     << "% missed)  enginePartStates=0x" << juce::String::toHexString(int(partStates))
			     << juce::newLine;
			MidiLogEntry log[16];
			const int n = getMidiLog(log, 16);
			for (int i = 0; i < n; ++i)
				line << "    recent MIDI: status=0x" << juce::String::toHexString(log[i].status)
				     << " data1=" << int(log[i].data1) << " data2=" << int(log[i].data2) << " size="
				     << log[i].size << juce::newLine;
			logFile.appendText(line);
		}
	}
	// --- end diagnostic instrumentation ---

	if (static_cast<int>(interleavedScratch.size()) < numSamples * 2)
		interleavedScratch.resize(static_cast<size_t>(numSamples) * 2);

	const float masterVolume = masterVolumeParam->load();
	const int numOutChannels = buffer.getNumChannels();

	// The six INDIVIDUAL buses are disabled by default (createBuses()), so most hosts and
	// most projects never pay for this at all: MULTI_OUTPUT_STREAMS below only gets built,
	// and renderD110MultiOutput() only gets called, when a host has actually connected at
	// least one of them. Otherwise this takes the exact code path it always did.
	bool multiOutputActive = false;
	float *individualChannel[6] = {};
	for (int output = 0; output < 6; ++output) {
		auto bus = getBusBuffer(buffer, false, output + 1);
		if (bus.getNumChannels() > 0) {
			individualChannel[output] = bus.getWritePointer(0);
			multiOutputActive = true;
		}
	}

	auto midiIterator = midiMessages.cbegin();
	const auto midiEnd = midiMessages.cend();
	int samplePos = 0;

	while (samplePos < numSamples) {
		while (midiIterator != midiEnd && (*midiIterator).samplePosition <= samplePos) {
			handleIncomingMidiMessage((*midiIterator).getMessage());
			++midiIterator;
		}

		int nextEventSample = numSamples;
		if (midiIterator != midiEnd)
			nextEventSample = juce::jmin(numSamples, (*midiIterator).samplePosition);

		const int samplesToRender = nextEventSample - samplePos;
		if (samplesToRender > 0) {
			if (multiOutputActive) {
				MT32Emu::D110MultiOutputStreams streams = {};
				streams.mixStereo = interleavedScratch.data();
				for (int output = 0; output < 6; ++output)
					streams.individualMono[output] =
						individualChannel[output] != nullptr ? individualChannel[output] + samplePos : nullptr;
				synth->renderD110MultiOutput(streams, static_cast<MT32Emu::Bit32u>(samplesToRender));
			} else {
				sampleRateConverter->getOutputSamples(interleavedScratch.data(),
													   static_cast<unsigned int>(samplesToRender));
			}
			float *left = buffer.getWritePointer(0, samplePos);
			float *right = numOutChannels > 1 ? buffer.getWritePointer(1, samplePos) : nullptr;
			// Насыщение ЦАПа, и только потом ручка громкости - в приборе они стоят
			// именно в таком порядке: ЦАП шестнадцатибитный и физически не может выдать
			// больше полной шкалы, а регулятор громкости аналоговый и стоит за ним.
			//
			// Само насыщение принадлежит движку: Synth::clipSampleEx(Bit32s) прижимает
			// сумму голосов к +-32767. Но у той же функции есть перегрузка для float, и
			// она НАМЕРЕННО ничего не делает - `return sampleEx;`. Наша сборка идёт по
			// float-пути, поэтому модель ЦАПа до выхода не доезжала, и плотные места
			// демо-песни выходили за шкалу примерно на 1.5 дБ. Это расхождение с
			// прибором, а не его характер: на «Macho Memory» за полную шкалу выходили
			// 69 отсчётов из 6 614 016, то есть одна тысячная процента, - ЦАП срезал бы
			// их неслышно.
			//
			// Ручка на панели идёт 0..2 с единичным усилением ровно посередине, где она
			// и стоит по умолчанию, так что в состоянии покоя выход теперь не может
			// превысить полную шкалу. Всё, что правее середины, - это уже запрошенное
			// пользователем усиление, как фейдер на пульте, а не поведение прибора.
			for (int i = 0; i < samplesToRender; ++i) {
				const float l = juce::jlimit(-1.0f, 1.0f,
				                            interleavedScratch[static_cast<size_t>(i) * 2]);
				const float r = juce::jlimit(-1.0f, 1.0f,
				                            interleavedScratch[static_cast<size_t>(i) * 2 + 1]);
				left[i] = l * masterVolume;
				if (right != nullptr) right[i] = r * masterVolume;
			}
		}
		samplePos = nextEventSample;
	}

	// Any remaining events exactly at the end of the block still need to be delivered so
	// they're not lost before the next processBlock call.
	while (midiIterator != midiEnd) {
		handleIncomingMidiMessage((*midiIterator).getMessage());
		++midiIterator;
	}

	// Sequencer metronome: mixed straight into the output rather than routed through the
	// firmware/LA32 engine, since it's a transport aid and not part of the instrument's own
	// sound - deliberately outside the DAC-saturation/VOLUME-knob scaling above. State
	// (phase, remaining samples) is carried in member variables because a click's short
	// decay can outlast the block it started in.
	// Skipped entirely when the metronome is routed through the rhythm channel instead (see
	// D110SequencerEngine::getMetronomeUseChannel10()) - that real MIDI note IS the click
	// sound in that mode, and mixing this synthesized one in too would just double it.
	//
	// The metronomeSamplesRemaining > 0 half of this condition matters even on blocks with
	// no NEW click (sequencerClicks empty): the click's ~30ms decay is longer than a typical
	// audio block (512 samples is ~11ms at 44.1kHz), so it almost always outlives the block
	// it started in. Gating this purely on "a click started this block" - the previous
	// version - meant the decay's countdown simply froze on every block that had no new
	// click of its own, then resumed (and finished, all at once) whenever the NEXT beat's
	// click came in and reopened this branch - i.e. the tail end of one beat's click played
	// back-to-back with the very start of the next beat's, audible as the first beat's sound
	// also sounding on the second beat. Rendering (and counting down) on every block once a
	// click is in progress, not just blocks that start one, is what actually lets it decay
	// smoothly across block boundaries instead of pausing and dumping the rest late.
	if ((!sequencerClicks.empty() || metronomeSamplesRemaining > 0) && !sequencerEngine.getMetronomeUseChannel10()) {
		constexpr double kClickSeconds = 0.03;
		const int clickTotalSamples = juce::jmax(1, static_cast<int>(currentSampleRate * kClickSeconds));
		const float volume = sequencerEngine.getMetronomeVolume();
		int cursor = 0;
		auto ringUpTo = [&](int endSample) {
			for (int i = cursor; i < endSample; ++i) {
				if (metronomeSamplesRemaining <= 0) continue;
				const float amp = static_cast<float>(metronomeSamplesRemaining) / static_cast<float>(clickTotalSamples);
				const float s = std::sin(metronomePhase) * amp * 0.25f * volume;
				metronomePhase += juce::MathConstants<double>::twoPi * metronomeFreq / currentSampleRate;
				for (int ch = 0; ch < numOutChannels; ++ch) buffer.addSample(ch, i, s);
				--metronomeSamplesRemaining;
			}
		};
		for (const auto &click : sequencerClicks) {
			ringUpTo(click.samplePosition);
			cursor = click.samplePosition;
			metronomeSamplesRemaining = clickTotalSamples;
			metronomeFreq = click.downbeat ? 1500.0 : 1000.0;
			metronomePhase = 0.0;
		}
		ringUpTo(numSamples);
	}

#ifdef D110_NATIVE_CORE
	// The MAME-backed D110Core advances on its own real-time thread regardless of whether
	// processBlock() is even being called, which is exactly the two-clocks problem this whole
	// port exists to remove - so nothing in the original code ever had to step it forward from
	// here. D110CoreNative has no thread of its own: it only advances when explicitly told to,
	// and this is that call. Placed at the END of the block, after every host MIDI event above
	// has already reached core.pushMidi() (inside forwardMidiToFirmware(), called from
	// handleIncomingMidiMessage()) - so this step is what actually processes everything this
	// block just queued, and the results (popSysex/popNoteEvent, drained at the TOP of this
	// function) surface on the NEXT processBlock call. That's the measured, fixed one-block
	// latency (native_note_latency_probe.cpp: 11.61ms, zero jitter across 15 trials) -
	// setLatencySamples(samplesPerBlock) in prepareToPlay() already reports and compensates
	// for exactly this.
	core.runForSeconds(double(numSamples) / currentSampleRate);
#endif
}

void D110AudioProcessor::handleIncomingMidiMessage(const juce::MidiMessage &message) {
	// The control board gets its own copy first. On the real instrument one MIDI cable
	// feeds both the CPU and the voice circuitry; here the CPU is MAME and the voice
	// circuitry is mt32emu, so both have to be told. Without this the firmware never
	// learns a note was played - which is why the top LCD row did not light the playing
	// parts, and why the display drifted away from the host's program changes.
	forwardMidiToFirmware(message);

	// И в ленту, которую показывает вкладка MONITOR расширенного редактора: это ровно то,
	// что прибор получил, до всякого разбора.
	logIncomingMidi(message);

	if (!synth) return;

	if (message.isSysEx()) {
		synth->playSysex(message.getRawData(), static_cast<MT32Emu::Bit32u>(message.getRawDataSize()));
		return;
	}

	const auto *raw = message.getRawData();
	const auto size = message.getRawDataSize();
	if (size <= 0) return;

	// When the firmware is voicing the notes, it must not be done twice. The firmware has
	// already been handed this message above; it will allocate a voice and report the note
	// back through popNoteEvent(), which is where it reaches the engine. Sending it here as
	// well would play every note twice - once with the firmware's own key range, transpose
	// and part assignment applied, once without. Everything that is NOT a note still goes
	// straight through, because the engine needs controllers, bend and program changes in
	// its own right.
	if (forwardNotes.load(std::memory_order_relaxed) && core.isRunning() &&
	    (message.isNoteOnOrOff() || message.isAftertouch()))
		return;

	const juce::uint8 status = raw[0];
	const juce::uint8 data1 = size > 1 ? raw[1] : 0;
	const juce::uint8 data2 = size > 2 ? raw[2] : 0;

	const MT32Emu::Bit32u msg = static_cast<MT32Emu::Bit32u>(status)
		| (static_cast<MT32Emu::Bit32u>(data1) << 8)
		| (static_cast<MT32Emu::Bit32u>(data2) << 16);
	synth->playMsg(msg);
}

// ---- MIDI ports opened directly, beside the host's own routing ---------------

void D110AudioProcessor::setMidiInputDevice(const juce::String &id) {
	if (osMidiIn) { osMidiIn->stop(); osMidiIn.reset(); }
	selInputId = {};
	if (id.isEmpty()) return;
	osMidiIn = juce::MidiInput::openDevice(id, this);
	if (osMidiIn) { osMidiIn->start(); selInputId = id; }
}

void D110AudioProcessor::setMidiOutputDevice(const juce::String &id) {
	std::unique_ptr<juce::MidiOutput> opened;
	if (id.isNotEmpty()) opened = juce::MidiOutput::openDevice(id);
	const juce::ScopedLock lock(osMidiLock);
	osMidiOut = std::move(opened);
	selOutputId = osMidiOut ? id : juce::String();
}

void D110AudioProcessor::handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &m) {
	// Handed to the collector rather than acted on here: this runs on the OS MIDI thread,
	// and everything downstream - the engine's single-writer queue and the control board's
	// serial input - is fed from processBlock.
	//
	// Rechannelized onto the on-screen keyboard's own selected channel first - see
	// processBlock()'s own comment (the equivalent rechannelling for a DAW-hosted external
	// controller) for why. This is the Standalone app's directly-opened MIDI port, the other
	// place real external MIDI enters.
	if (!keyboardOmni && m.getChannel() > 0) {
		auto msg = m;
		msg.setChannel(keyboardMidiChannel);
		osMidiCollector.addMessageToQueue(msg);
		return;
	}
	osMidiCollector.addMessageToQueue(m);
}

void D110AudioProcessor::injectTestNote(int channel, int note, float velocity, bool on) {
	auto message = on ? juce::MidiMessage::noteOn(channel, note, velocity)
	                   : juce::MidiMessage::noteOff(channel, note, velocity);
	// addMessageToQueue times messages against Time::getMillisecondCounter()'s base, same as
	// the real MidiInput callback above stamps its own messages before handing them here.
	message.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
	osMidiCollector.addMessageToQueue(message);
}

void D110AudioProcessor::forwardMidiToFirmware(const juce::MidiMessage &message) {
	if (!core.isRunning()) return;

	// Clock and active sensing arrive constantly and say nothing the panel can show. At
	// 3125 bytes a second they would crowd out the messages that matter, so they stop
	// here. Everything else - notes, controllers, program changes, bend, exclusive - goes
	// through, because the firmware is meant to be the authority on what it is playing.
	const auto *raw = message.getRawData();
	const int size = message.getRawDataSize();
	if (size <= 0) return;
	if (raw[0] == 0xF8 || raw[0] == 0xFE) return;

	if (!forwardNotes.load(std::memory_order_relaxed)
	    && (message.isNoteOnOrOff() || message.isAftertouch() || message.isChannelPressure()))
		return;

	// See D110CoreType::hintRhythmKey(): a handful of rhythm timbres make the firmware's own
	// note-completion write carry a small placeholder instead of the real key, and this is
	// the one place that still knows what the real key was. Channel 10 is the factory
	// rhythm channel, the same assumption the rest of this project already makes (rhythm
	// probes, docs) rather than something tracked per chanAssign here.
	//
	// Gated to the three measured-broken timbres (64, 65, 66) specifically - NOT every
	// rhythm hit. Queuing a hint for every drum, working ones included, was the bug a
	// session found by ear: the queue is a plain FIFO with no link back to which key it was
	// for, so an ordinary kick or snare played earlier sat in it and later got handed to an
	// unrelated hi-hat hit whose own completion happened to need a substitute, printing the
	// wrong drum onto that key until the backlog cleared. Checking the CURRENT Rhythm Setup
	// tembr keeps the queue holding only entries a broken completion could plausibly be
	// asking for.
	if (message.isNoteOn() && message.getChannel() == 10) {
		const int note = message.getNoteNumber();
		if (note >= D110CoreType::kRhythmFirstKey && note < D110CoreType::kRhythmFirstKey + D110CoreType::kNumRhythmKeys) {
			std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
			if (core.getRam(ram.data())) {
				const size_t at = size_t(D110CoreType::kRamRhythmTemp)
				                + size_t(note - D110CoreType::kRhythmFirstKey) * D110CoreType::kRhythmRecord;
				const uint8_t tembr = ram[at];
				if (tembr == 64 || tembr == 65 || tembr == 66)
					core.hintRhythmKey(juce::uint8(note));
			}
		}
	}

	core.pushMidi(static_cast<const juce::uint8 *>(raw), size);
}

juce::AudioProcessorEditor *D110AudioProcessor::createEditor() {
	return new D110AudioProcessorEditor(*this);
}

D110AudioProcessor::LcdSnapshot D110AudioProcessor::getLcdSnapshot() const {
	LcdSnapshot snapshot;

	// Both rows start as spaces, so anything shorter than 16 characters is padded rather than
	// leaving whatever the previous, longer message put there.
	for (int line = 0; line < LcdSnapshot::kLines; ++line)
		for (int col = 0; col < LcdSnapshot::kCols; ++col)
			snapshot.text[line][col] = ' ';

	auto write = [&snapshot](int line, int col, const char *s) {
		for (int i = 0; s[i] != '\0' && col + i < LcdSnapshot::kCols; ++i)
			snapshot.text[line][col + i] = static_cast<juce::uint8>(s[i]);
	};

	if (!synth) {
		write(0, 0, "D-110  No ROMs");
		write(1, 0, "loaded");
		return snapshot;
	}

	// The lamp comes off the firmware's own SO register when the control board is running -
	// bit 0 of 0x0200, which is what drives it on the hardware. mt32emu's getDisplayState()
	// is only the fallback for a powered-off or not-yet-booted machine: it answers a
	// different question (has a MIDI message just arrived, is any voice part sounding)
	// that merely looks similar.
	if (core.midiLampValid()) {
		snapshot.midiLedOn = core.midiLampOn();
	} else {
		char unusedBuffer[21] = {};
		snapshot.midiLedOn = synth->getDisplayState(unusedBuffer, false);
	}

	// Row 1, exactly as the real unit draws it (see docs/lcd_reference.png): the eight Part slots
	// and the Rhythm slot, then the mode word. A Part that currently has a partial playing in a
	// non-releasing phase has its number REPLACED by the full-block character - the hardware's own
	// convention, documented in munt's Display.cpp, not a dimmed digit.
	const MT32Emu::Bit32u partStates = synth->getPartStates();
	for (int i = 0; i < 8; ++i)
		snapshot.text[0][i] = (partStates & (1u << i)) ? LcdSnapshot::kActivePartBlock
		                                               : static_cast<juce::uint8>('1' + i);
	snapshot.text[0][8] = (partStates & (1u << 8)) ? LcdSnapshot::kActivePartBlock
	                                               : static_cast<juce::uint8>('R');
	write(0, 10, "RomPly");

	// Row 2: "<part>:<patch name>", again matching the photographed screen.
	const int part = selectedPartIndex.load();
	snapshot.text[1][0] = static_cast<juce::uint8>('1' + part);
	snapshot.text[1][1] = ':';
	if (const char *name = synth->getPatchName(static_cast<MT32Emu::Bit8u>(part)))
		write(1, 2, juce::String(name).trim().toRawUTF8());

	return snapshot;
}

bool D110AudioProcessor::readEngineMemory(juce::uint32 packedAddress, juce::uint32 length,
                                          juce::uint8 *out) {
	if (synth == nullptr) return false;
	synth->readMemory(static_cast<MT32Emu::Bit32u>(packedAddress),
	                  static_cast<MT32Emu::Bit32u>(length),
	                  static_cast<MT32Emu::Bit8u *>(out));
	return true;
}

float D110AudioProcessor::getMasterVolume() const {
	if (auto *p = parameters.getParameter("masterVolume"))
		return p->getValue();
	return 1.0f;
}

void D110AudioProcessor::setMasterVolume(float newValue) {
	if (auto *p = parameters.getParameter("masterVolume")) {
		p->beginChangeGesture();
		p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, newValue));
		p->endChangeGesture();
	}
}

void D110AudioProcessor::resetDisplayToMainMode() {
	if (synth) synth->setMainDisplayMode();
}

void D110AudioProcessor::importSysexBank(const juce::File &file) {
	std::vector<std::vector<MT32Emu::Bit8u>> messages;

	if (file.hasFileExtension("mid") || file.hasFileExtension("smf")) {
		messages = extractSysexMessagesFromMidiFile(file);
	}

	if (messages.empty()) {
		// Either a genuine .syx file, or a .mid JUCE couldn't parse - fall back to scanning raw bytes.
		juce::MemoryBlock raw;
		if (file.loadFileAsData(raw)) messages = extractSysexMessagesFromRawBytes(raw);
	}

	if (messages.empty()) {
		lastImportMessage = "No SysEx data found in: " + file.getFileName();
		return;
	}

	{
		const juce::ScopedLock sl(engineActionLock);
		for (auto &message : messages) pendingSysexImports.push_back(std::move(message));
	}
	size_t bytes = 0;
	for (const auto &m : messages) bytes += m.size();
	// It goes into the control board down an emulated MIDI cable at the real 31250 baud,
	// so a big bank genuinely takes a few seconds to land, exactly as on the hardware.
	const int seconds = int(double(bytes) / D110CoreType::kMidiBytesPerSecond + 0.5);
	lastImportMessage = "Sending " + juce::String(messages.size()) + " SysEx message(s) from "
		+ file.getFileName()
		+ (seconds >= 2 ? " (about " + juce::String(seconds) + "s at MIDI speed)" : juce::String());
}

namespace {
// "D1SN" + format version, so a stray file (or a future incompatible layout) is refused
// rather than fed to the instrument as garbage.
constexpr char kSnapshotMagic[4] = { 'D', '1', 'S', 'N' };
constexpr juce::uint8 kSnapshotVersion = 1;
} // namespace

void D110AudioProcessor::exportMemorySnapshot(const juce::File &file) {
	// Same rule as getStateInformation: while the machine is running the file on disk is
	// stale (MAME/the native core only flush NVRAM out on power-off), so the live image in
	// the core is authoritative whenever it's available, and the file is the fallback.
	juce::MemoryBlock rams;
	if (core.isRunning()) {
		rams.setSize(D110CoreType::kRamSize);
		if (!core.getRam(static_cast<juce::uint8 *>(rams.getData()))) rams.reset();
	}
	if (rams.getSize() == 0) rams = readNvramFile("rams");

	if (rams.getSize() != size_t(D110CoreType::kRamSize)) {
		lastImportMessage = "Nothing to snapshot yet - switch the instrument on at least once first.";
		return;
	}

	juce::MemoryBlock memcs;
	if (core.isRunning()) {
		memcs.setSize(D110CoreType::kCardSize);
		if (!core.getCardImage(static_cast<juce::uint8 *>(memcs.getData()))) memcs.reset();
	}
	if (memcs.getSize() == 0) memcs = readNvramFile("memcs");
	memcs.setSize(size_t(D110CoreType::kCardSize), true); // zero-pad if the card was never imaged

	juce::MemoryBlock out;
	out.append(kSnapshotMagic, sizeof(kSnapshotMagic));
	const juce::uint8 header[3] = { kSnapshotVersion,
	                                juce::uint8(core.cardInserted() ? 1 : 0),
	                                juce::uint8(core.cardWriteProtect() ? 1 : 0) };
	out.append(header, sizeof(header));
	out.append(rams.getData(), rams.getSize());
	out.append(memcs.getData(), memcs.getSize());

	if (!file.replaceWithData(out.getData(), out.getSize())) {
		lastImportMessage = "Could not write snapshot: " + file.getFullPathName();
		return;
	}

	lastImportMessage = "Saved memory snapshot: " + file.getFileName();
}

void D110AudioProcessor::importMemorySnapshot(const juce::File &file) {
	constexpr int kHeaderSize = 4 + 1 + 1 + 1;
	juce::MemoryBlock data;
	if (!file.loadFileAsData(data)
	    || data.getSize() != size_t(kHeaderSize) + size_t(D110CoreType::kRamSize) + size_t(D110CoreType::kCardSize)
	    || memcmp(data.getData(), kSnapshotMagic, sizeof(kSnapshotMagic)) != 0
	    || static_cast<const juce::uint8 *>(data.getData())[4] != kSnapshotVersion) {
		lastImportMessage = "Not a D-110 memory snapshot: " + file.getFileName();
		return;
	}

	const auto *bytes = static_cast<const juce::uint8 *>(data.getData());
	const bool cardInserted = bytes[5] != 0;
	const bool cardWriteProtect = bytes[6] != 0;
	juce::MemoryBlock rams(bytes + kHeaderSize, size_t(D110CoreType::kRamSize));
	juce::MemoryBlock memcs(bytes + kHeaderSize + D110CoreType::kRamSize, size_t(D110CoreType::kCardSize));

	// The core only reads NVRAM off disk at power-on, so a snapshot loaded while the
	// instrument is already running needs a power cycle to actually take - done here, rather
	// than left for the user, so "load snapshot" is felt immediately.
	const bool wasOn = core.isRunning();
	if (wasOn) setPoweredOn(false);
	writeNvramFiles(rams, memcs);
	core.setCardInserted(cardInserted);
	core.setCardWriteProtect(cardWriteProtect);
	if (wasOn) setPoweredOn(true);

	lastImportMessage = "Loaded memory snapshot: " + file.getFileName();
}

namespace {
// One region of the documented map, built as one or more DT1 messages (buildDt1Message caps
// a single message's data at kMaxSysexBytes - 12 bytes, which is smaller than a 246-byte Tone
// Temporary record or a 256-byte Tone record, so a region longer than that is split across
// several messages - the same thing kMirrorRegions/D110_RHYTHM already do for Rhythm Setup,
// and just as harmless here: each DT1 carries its own address, so the firmware does not care
// how many messages a region arrived as).
// `sysexBaseAddress` is the region's fixed base (e.g. kSysexToneTemp) and never changes per
// record - exactly as sendToneTempParam/sendPatchMemoryParam/etc. already call buildDt1Message
// elsewhere in this file. The record's position is folded into `sysexOffsetBase` instead, which
// buildDt1Message adds in the SEVEN-BIT packed address space (see its own comment): the base
// address's hex digits are NOT plain-integer-addable, since a byte offset crossing 0x80 has to
// carry into the next seven-bit digit rather than the next hex nibble.
void appendDt1Region(juce::MemoryBlock &out, juce::uint32 sysexBaseAddress, int sysexOffsetBase,
                     const juce::uint8 *ram, int ramOffset, int length) {
	constexpr int kChunk = 200;
	juce::uint8 msg[D110CoreType::kMaxSysexBytes];
	for (int sent = 0; sent < length; sent += kChunk) {
		const int n = std::min(kChunk, length - sent);
		const int written = D110CoreType::buildDt1Message(sysexBaseAddress, sysexOffsetBase + sent,
		                                                   ram + ramOffset + sent, n, msg);
		if (written > 0) out.append(msg, size_t(written));
	}
}
} // namespace

void D110AudioProcessor::exportSysexBank(const juce::File &file) {
	juce::MemoryBlock ramsBlock;
	if (core.isRunning()) {
		ramsBlock.setSize(D110CoreType::kRamSize);
		if (!core.getRam(static_cast<juce::uint8 *>(ramsBlock.getData()))) ramsBlock.reset();
	}
	if (ramsBlock.getSize() == 0) ramsBlock = readNvramFile("rams");
	if (ramsBlock.getSize() != size_t(D110CoreType::kRamSize)) {
		lastImportMessage = "Nothing to export yet - switch the instrument on at least once first.";
		return;
	}
	const auto *ram = static_cast<const juce::uint8 *>(ramsBlock.getData());

	juce::MemoryBlock out;

	for (int i = 0; i < D110CoreType::kNumPatches; ++i)
		appendDt1Region(out, D110CoreType::kSysexPatches, i * D110CoreType::kPatchRecord,
		                ram, D110CoreType::kRamPatches + i * D110CoreType::kPatchRecord,
		                D110CoreType::kPatchRecord);

	appendDt1Region(out, D110CoreType::kSysexTimbreTemp, 0, ram, D110CoreType::kRamTimbreTemp,
	                D110CoreType::kNumParts * D110CoreType::kTimbreTempRecord);

	appendDt1Region(out, D110CoreType::kSysexRhythmTemp, 0, ram, D110CoreType::kRamRhythmTemp,
	                D110CoreType::kNumRhythmKeys * D110CoreType::kRhythmRecord);

	// Rhythm has no Tone Temporary record of its own, so only the 8 voice parts.
	for (int i = 0; i < 8; ++i)
		appendDt1Region(out, D110CoreType::kSysexToneTemp, i * D110CoreType::kToneRecord,
		                ram, D110CoreType::kRamToneTemp + i * D110CoreType::kToneRecord,
		                D110CoreType::kToneRecord);

	for (int i = 0; i < D110CoreType::kNumTimbres; ++i)
		appendDt1Region(out, D110CoreType::kSysexTimbres, i * D110CoreType::kTimbreRecord,
		                ram, D110CoreType::kRamTimbres + i * D110CoreType::kTimbreRecord,
		                D110CoreType::kTimbreRecord);

	// Only the 23 documented System Area bytes - the rest of the RAM up to Tone Memory is
	// undocumented firmware working state, not patch/tone data (see kMirrorRegions' own
	// comment on the same boundary).
	appendDt1Region(out, D110CoreType::kSysexSystem, 0, ram, D110CoreType::kRamSystem, 23);

	for (int i = 0; i < D110CoreType::kNumTones; ++i)
		appendDt1Region(out, D110CoreType::kSysexTones, i * D110CoreType::kToneMemRecord,
		                ram, D110CoreType::kRamTones + i * D110CoreType::kToneMemRecord,
		                D110CoreType::kToneMemRecord);

	if (!file.replaceWithData(out.getData(), out.getSize())) {
		lastImportMessage = "Could not write SysEx bank: " + file.getFullPathName();
		return;
	}

	lastImportMessage = "Saved SysEx bank: " + file.getFileName();
}

void D110AudioProcessor::selectNextPart() {
	selectedPartIndex = (selectedPartIndex.load() + 1) % 8;
}

void D110AudioProcessor::selectPreviousPart() {
	selectedPartIndex = (selectedPartIndex.load() + 7) % 8;
}

void D110AudioProcessor::stepPatch(int direction) {
	const int part = selectedPartIndex.load();
	int newProgram = (currentProgramPerPart[static_cast<size_t>(part)] + direction) % 128;
	if (newProgram < 0) newProgram += 128;
	currentProgramPerPart[static_cast<size_t>(part)] = newProgram;

	// Default factory chanAssign maps Part i to MIDI channel nibble i+1 (Part 1 -> channel 2, etc).
	const int channelNibble = part + 1;
	const MT32Emu::Bit32u message = static_cast<MT32Emu::Bit32u>(0xC0 | channelNibble)
		| (static_cast<MT32Emu::Bit32u>(newProgram) << 8);

	const juce::ScopedLock sl(engineActionLock);
	pendingShortMessages.push_back(message);
}

void D110AudioProcessor::midiPanic() {
	std::vector<juce::uint8> firmwareBytes;
	std::vector<MT32Emu::Bit32u> engineMessages;
	firmwareBytes.reserve(16 * 2 * 3);
	engineMessages.reserve(16 * 2);
	for (int channel = 0; channel < 16; ++channel) {
		const juce::uint8 status = juce::uint8(0xB0 | channel);
		for (juce::uint8 controller : { juce::uint8(64), juce::uint8(123) }) { // hold pedal off, all notes off
			firmwareBytes.push_back(status);
			firmwareBytes.push_back(controller);
			firmwareBytes.push_back(0);
			engineMessages.push_back(MT32Emu::Bit32u(status) | (MT32Emu::Bit32u(controller) << 8));
		}
	}

	const juce::ScopedLock sl(engineActionLock);
	pendingPanicBytes.insert(pendingPanicBytes.end(), firmwareBytes.begin(), firmwareBytes.end());
	pendingShortMessages.insert(pendingShortMessages.end(), engineMessages.begin(), engineMessages.end());
}

// ---- the extended editor -----------------------------------------------------
//
// Every one of these builds a Roland "Data set 1" and hands it to the CONTROL BOARD's own
// MIDI input, which is the same door an external editor knocks on. Nothing goes to the
// sound engine from here: the firmware writes its memory, and the mirror
// (D110CoreType::emitRegionSysex) carries that across on its next snapshot - so a drawer edit
// and a panel edit are the same event, and the instrument's own display shows both.
//
// That the firmware accepts them, and where each one lands, is MEASURED - see
// plugin/editor_write_probe.cpp, which sends one write per area and reports which byte of
// the battery RAM moved. Mem Protect, which is ON from the factory, does not block them.

void D110AudioProcessor::sendAreaData(juce::uint32 sysexAddress, int offset,
                                      const juce::uint8 *data, int length) {
	if (!core.isRunning() || data == nullptr || length <= 0) return;
	juce::uint8 msg[D110CoreType::kMaxSysexBytes];
	const int n = D110CoreType::buildDt1Message(sysexAddress, offset, data, length, msg);
	if (n > 0) core.pushMidi(msg, n);
}

void D110AudioProcessor::sendTimbreTempParam(int part, int field, juce::uint8 value) {
	if (part < 0 || part >= D110CoreType::kNumParts) return;
	if (field < 0 || field >= D110CoreType::kTimbreTempRecord) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexTimbreTemp, part * D110CoreType::kTimbreTempRecord + field, &v, 1);
}

void D110AudioProcessor::sendToneTempParam(int part, int offset, juce::uint8 value) {
	if (part < 0 || part > 7 || offset < 0 || offset >= D110CoreType::kToneRecord) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexToneTemp, part * D110CoreType::kToneRecord + offset, &v, 1);
}

void D110AudioProcessor::sendRhythmParam(int slot, int field, juce::uint8 value) {
	if (slot < 0 || slot >= D110CoreType::kNumRhythmKeys) return;
	if (field < 0 || field >= D110CoreType::kRhythmRecord) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexRhythmTemp, slot * D110CoreType::kRhythmRecord + field, &v, 1);
}

void D110AudioProcessor::sendSystemParam(int field, juce::uint8 value) {
	// 23 байта, как их описывает Roland: подстройка, ревербератор, резерв партиалов, карта
	// каналов и громкость. Последняя у D-110 - физическая ручка, и прошивка её не
	// заполняет, поэтому редактор её не предлагает; предел здесь всё равно стоит по длине
	// области, а не по тому, что редактор рисует.
	if (field < 0 || field > 22) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexSystem, field, &v, 1);
}

void D110AudioProcessor::sendTimbreMemoryParam(int slot, int field, juce::uint8 value) {
	if (slot < 0 || slot >= D110CoreType::kNumTimbres) return;
	if (field < 0 || field >= D110CoreType::kTimbreRecord) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexTimbres, slot * D110CoreType::kTimbreRecord + field, &v, 1);
}

void D110AudioProcessor::sendPatchMemoryParam(int patch, int field, juce::uint8 value) {
	if (patch < 0 || patch >= D110CoreType::kNumPatches) return;
	if (field < 0 || field >= D110CoreType::kPatchRecord) return;
	const juce::uint8 v = value & 0x7f;
	sendAreaData(D110CoreType::kSysexPatches, patch * D110CoreType::kPatchRecord + field, &v, 1);
}

void D110AudioProcessor::sendName(juce::uint32 sysexAddress, int offset,
                                  const juce::String &name) {
	juce::uint8 data[D110CoreType::kNameChars];
	for (int i = 0; i < D110CoreType::kNameChars; ++i) {
		const juce::juce_wchar ch = (i < name.length()) ? name[i] : ' ';
		data[i] = juce::uint8((ch >= 32 && ch < 127) ? ch : ' ');
	}
	sendAreaData(sysexAddress, offset, data, D110CoreType::kNameChars);
}

void D110AudioProcessor::sendDisplayMessage(const juce::String &text) {
	// Двадцать знаков - столько несёт команда Roland. Индикатор D-110 шириной шестнадцать,
	// так что четыре последних он просто не покажет; сообщение от этого не перестаёт быть
	// правильным, и обрезать его здесь значило бы посылать не то, что посылает прибор.
	juce::uint8 data[20];
	for (int i = 0; i < 20; ++i) {
		const juce::juce_wchar ch = (i < text.length()) ? text[i] : ' ';
		data[i] = juce::uint8((ch >= 32 && ch < 127) ? ch : ' ');
	}
	sendAreaData(D110CoreType::kSysexDisplay, 0, data, 20);
}

void D110AudioProcessor::selectTimbreForPart(int part, int timbre) {
	if (part < 0 || part > 7 || timbre < 0 || timbre > 127) return;
	if (!core.isRunning()) return;

	// Канал берётся из карты самой прошивки (System Area, chanAssign), а не считается по
	// заводской формуле "партия N на канале N+1": эту карту можно изменить, и тогда
	// формула отправила бы смену программы мимо.
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return;
	const int chan = ram[(size_t)D110CoreType::kRamSystem + 13 + (size_t)part];
	if (chan > 15) return;   // партия выключена - слать некуда

	const juce::uint8 bytes[2] = { juce::uint8(0xC0 | chan), juce::uint8(timbre & 0x7f) };
	core.pushMidi(bytes, 2);
	// Смена программы - обычное MIDI-сообщение, поэтому она должна дойти и до звукового
	// движка, как дошла бы с внешней клавиатуры.
	const juce::ScopedLock sl(engineActionLock);
	pendingShortMessages.push_back(juce::uint32(bytes[0]) | (juce::uint32(bytes[1]) << 8));
}

// Патч выбирается НАЖАТИЯМИ, а не записью в память: на D-110 патч - это не один
// параметр, а целая раскладка, и раскладывает её по временным областям сама прошивка.
// Записать её за прошивку значило бы держать вторую, свою реализацию смены патча, которая
// однажды разойдётся с настоящей; нажать кнопки - значит попросить сделать это ту
// программу, которая на приборе за это отвечает, и получить заодно её индикатор, её
// зеркало и её же поведение.
//
// Номер текущего патча лежит в ОЗУ по 0x2DB9 (0..63), Bank+ двигает его на 8, Number+ на 1
// - измерено plugin/editor_write_probe.cpp. Отсюда путь до любого патча: не больше семи
// нажатий Bank+ и семи Number+.
void D110AudioProcessor::selectPatch(int patch) {
	if (patch < 0 || patch >= D110CoreType::kNumPatches) return;
	if (!core.isRunning() || patchSteps > 0) return;

	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return;
	const int current = ram[(size_t)D110CoreType::kRamPatchNumber];
	if (current < 0 || current >= D110CoreType::kNumPatches) return;

	patchQueue.clear();
	// Сперва на страницу выбора патча - оттуда Bank и Number значат номер патча, а не
	// что-нибудь ещё. Нажимается всегда, даже если патч уже нужный: пользователь щёлкнул
	// по патчу и вправе увидеть его на индикаторе.
	patchQueue.push_back(D110CoreType::buttonIndex(0, 6));   // Patch

	// Ход считается СО ЗНАКОМ, и вниз идут кнопки со стрелкой вниз. Кольцевая арифметика
	// здесь не работает, и это измерено: Bank+ у прибора НЕ перекатывается с восьмого банка
	// на первый, а упирается. Расчёт «пять раз вперёд вместо трёх назад» просил патч I-14 и
	// оставлял прибор на I-84 - ровно там, где кончился банк (plugin/editor_test.cpp).
	const int bankStep = (patch / 8) - (current / 8);
	const int numberStep = (patch % 8) - (current % 8);
	const int bankButton = D110CoreType::buttonIndex(bankStep >= 0 ? 0 : 1, 2);     // Bank+ / Bank-
	const int numberButton = D110CoreType::buttonIndex(numberStep >= 0 ? 0 : 1, 1); // Number+ / Number-
	for (int i = 0; i < std::abs(bankStep); ++i) patchQueue.push_back(bankButton);
	for (int i = 0; i < std::abs(numberStep); ++i) patchQueue.push_back(numberButton);

	patchSteps = int(patchQueue.size());
	patchPhase = 0;
	// 60 мс на полунажатие: две фазы на кнопку, то есть восьмая доля секунды на нажатие -
	// быстрее, чем это делает рука, и заметно медленнее, чем матрица опроса, которая
	// читается каждый кадр прошивки.
	startTimer(60);
}

void D110AudioProcessor::timerCallback() {
	if (patchQueue.empty()) {
		patchSteps = 0;
		stopTimer();
		return;
	}
	const int button = patchQueue.front();
	if (patchPhase == 0) {
		core.setButton(button, true);
		patchPhase = 1;
		return;
	}
	core.setButton(button, false);
	patchPhase = 0;
	patchQueue.erase(patchQueue.begin());
	patchSteps = int(patchQueue.size());
	if (patchQueue.empty()) stopTimer();
}

int D110AudioProcessor::currentPatchNumber() const {
	if (!core.isRunning()) return -1;
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return -1;
	const int n = ram[(size_t)D110CoreType::kRamPatchNumber];
	return (n >= 0 && n < D110CoreType::kNumPatches) ? n : -1;
}

// Правка патча, слышная сразу. Память патча пишется всегда, а если правится тот патч,
// который прибор играет, то же значение уходит и в живую область - туда, откуда прибор
// действительно берёт звук.
//
// Раскладка записи патча измерена (docs/sysex_address_map.md): имя 0-9, ревербератор 10-12,
// резерв партиалов 13-21, карта каналов 22-30, дальше восемь записей партий по 12 байт с
// 31-го. Живые двойники у них разные: у партий это Timbre Temporary, у ревербератора,
// резерва и каналов - системная область.
void D110AudioProcessor::editPatchField(int patch, int field, juce::uint8 value) {
	sendPatchMemoryParam(patch, field, value);
	if (patch != currentPatchNumber()) return;

	if (field >= 31 && field < 31 + 8 * 12) {
		const int part = (field - 31) / 12;
		const int offset = (field - 31) % 12;

		// «Какой тон играет партия» - это ПАРА байтов, группа и номер, и переносить её
		// надо парой. Перенос одного байта оставляет обе стороны при своих: в записи патча
		// b01, в живой области a01 - номер сходится, группа нет, и ящик с индикатором
		// называют разные тона. Измерено: развели группы, покрутили номер, получили патч
		// (1,0) против живой области (0,0) (plugin/editor_test.cpp, раздел 5).
		if (offset == 0 || offset == 1) {
			std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
			if (!core.getRam(ram.data())) return;
			const size_t at = size_t(D110CoreType::kRamPatches)
			                + size_t(patch) * D110CoreType::kPatchRecord + 31 + size_t(part) * 12;
			// Второй байт пары берётся из самой записи патча: та, что в ОЗУ, ещё не знает о
			// правке, которая только что ушла эксклюзивным сообщением, поэтому правимый байт
			// подставляется вручную.
			juce::uint8 pair[2] = { ram[at], ram[at + 1] };
			pair[offset] = value & 0x7f;
			sendAreaData(D110CoreType::kSysexTimbreTemp, part * D110CoreType::kTimbreTempRecord, pair, 2);
			return;
		}
		sendTimbreTempParam(part, offset, value);
		return;
	}
	// Тип ревербератора в движок не переносится (восемь типов на четыре режима не ложатся),
	// но в прошивку он уходит: на индикаторе прибора значение обязано измениться.
	if (field >= 10 && field <= 12) { sendSystemParam(field - 9, value); return; }
	if (field >= 13 && field <= 21) { sendSystemParam(field - 9, value); return; }
	if (field >= 22 && field <= 30) { sendSystemParam(field - 9, value); return; }
}

// Тон целиком - 246 байт, а в одно эксклюзивное сообщение у Roland помещается 244, поэтому
// он идёт двумя кусками, ровно как его послал бы внешний библиотекарь.
static void sendToneBlock(D110AudioProcessor &proc, juce::uint32 address, int offset,
                          const uint8_t *from) {
	constexpr int kChunk = 123;
	for (int off = 0; off < D110CoreType::kToneRecord; off += kChunk) {
		const int len = juce::jmin(kChunk, D110CoreType::kToneRecord - off);
		proc.sendAreaData(address, offset + off, from + off, len);
	}
}

void D110AudioProcessor::auditionTone(int part, int slot) {
	if (part < 0 || part > 7 || slot < 0 || slot >= D110CoreType::kNumTones) return;
	if (!core.isRunning()) return;
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return;
	sendToneBlock(*this, D110CoreType::kSysexToneTemp, part * D110CoreType::kToneRecord,
	              ram.data() + D110CoreType::kRamTones + slot * D110CoreType::kToneMemRecord);
}

void D110AudioProcessor::storeToneFromPart(int part, int slot) {
	if (part < 0 || part > 7 || slot < 0 || slot >= D110CoreType::kNumTones) return;
	if (!core.isRunning()) return;
	std::vector<uint8_t> ram(D110CoreType::kRamSize, 0);
	if (!core.getRam(ram.data())) return;
	sendToneBlock(*this, D110CoreType::kSysexTones, slot * D110CoreType::kToneMemRecord,
	              ram.data() + D110CoreType::kRamToneTemp + part * D110CoreType::kToneRecord);
}

void D110AudioProcessor::logIncomingMidi(const juce::MidiMessage &message) {
	const auto *raw = message.getRawData();
	const int size = message.getRawDataSize();
	if (size <= 0) return;
	// Тактовые импульсы и active sensing идут потоком - их сотни в секунду, и они вытеснили
	// бы из ленты всё осмысленное.
	if (raw[0] == 0xF8 || raw[0] == 0xFE) return;

	MidiLogEntry e;
	e.status = raw[0];
	e.data1 = size > 1 ? raw[1] : 0;
	e.data2 = size > 2 ? raw[2] : 0;
	e.size = juce::uint16(juce::jmin(size, 65535));
	const juce::uint32 w = midiLogWrite.load(std::memory_order_relaxed);
	midiLog[(size_t)(w % kMidiLogSize)] = e;
	midiLogWrite.store(w + 1, std::memory_order_release);
}

int D110AudioProcessor::getMidiLog(MidiLogEntry *out, int max) const {
	const juce::uint32 w = midiLogWrite.load(std::memory_order_acquire);
	const int have = int(juce::jmin<juce::uint32>(w, kMidiLogSize));
	const int n = juce::jmin(have, max);
	for (int i = 0; i < n; ++i)
		out[i] = midiLog[(size_t)((w - 1 - juce::uint32(i)) % kMidiLogSize)];
	return n;
}

// Compressing before base64 is worth the few lines: the firmware's 32 KB battery RAM is
// mostly repeated patch and timbre records, so it packs down hard, and a project file
// should not carry ~88 KB of text per instance for something this repetitive.
static juce::String packBlock(const juce::MemoryBlock &raw) {
	if (raw.getSize() == 0) return {};
	juce::MemoryOutputStream compressed;
	{
		juce::GZIPCompressorOutputStream gzip(compressed);
		gzip.write(raw.getData(), raw.getSize());
	}
	return juce::Base64::toBase64(compressed.getData(), compressed.getDataSize());
}

static juce::MemoryBlock unpackBlock(const juce::String &text) {
	juce::MemoryBlock out;
	if (text.isEmpty()) return out;
	juce::MemoryOutputStream compressed;
	if (!juce::Base64::convertFromBase64(compressed, text)) return out;
	juce::MemoryInputStream source(compressed.getData(), compressed.getDataSize(), false);
	juce::GZIPDecompressorInputStream gzip(source);
	juce::MemoryOutputStream expanded;
	expanded.writeFromInputStream(gzip, -1);
	out.append(expanded.getData(), expanded.getDataSize());
	return out;
}

void D110AudioProcessor::getStateInformation(juce::MemoryBlock &destData) {
	auto state = parameters.copyState();
	std::unique_ptr<juce::XmlElement> xml(state.createXml());
	xml->setAttribute("controlRomPath", controlRomPath);
	xml->setAttribute("pcmRomPath", pcmRomPath);
	// Ports are stored by identifier, not by name: names repeat across machines and change
	// when a device is renamed, whereas the identifier is what openDevice actually wants.
	xml->setAttribute("midiIn", selInputId);
	xml->setAttribute("midiOut", selOutputId);

	// The firmware's own memory - its patches, its timbres, its edits - so that a project
	// recalls the sounds it was saved with instead of whatever the shared folder happens
	// to hold now.
	//
	// While the machine is running the FILE on disk is stale: MAME only writes its NVRAM
	// out when the machine exits. So take the live image straight from the core, and fall
	// back to the file only when powered off.
	juce::MemoryBlock rams;
	if (core.isRunning()) {
		rams.setSize(D110CoreType::kRamSize);
		if (!core.getRam(static_cast<juce::uint8 *>(rams.getData()))) rams.reset();
	}
	if (rams.getSize() == 0) rams = readNvramFile("rams");

	// Карта - по тому же правилу, что и батарейное ОЗУ: пока машина работает, файл на диске
	// отстал, а живое содержимое лежит в ядре. Оно же отдаётся и для извлечённой карты, у
	// которой в разделяемой памяти машины сейчас одни 0xFF.
	juce::MemoryBlock memcs;
	if (core.isRunning()) {
		memcs.setSize(D110CoreType::kCardSize);
		if (!core.getCardImage(static_cast<juce::uint8 *>(memcs.getData()))) memcs.reset();
	}
	if (memcs.getSize() == 0) memcs = readNvramFile("memcs");

	xml->setAttribute("nvramRams", packBlock(rams));
	xml->setAttribute("nvramMemcs", packBlock(memcs));
	// Гнездо и движок защиты от записи - это положение вещей на приборе, такое же, как
	// содержимое памяти: проект, сохранённый с вынутой картой, должен открыться с вынутой.
	xml->setAttribute("cardInserted", core.cardInserted() ? 1 : 0);
	xml->setAttribute("cardWriteProtect", core.cardWriteProtect() ? 1 : 0);

	// The on-screen test keyboard's own config - see D110AudioProcessor::getKeyboardMidiChannel()
	// and friends for why this lives on the processor rather than the UI component.
	xml->setAttribute("kbChannel", keyboardMidiChannel);
	xml->setAttribute("kbOmni", keyboardOmni ? 1 : 0);
	xml->setAttribute("kbPcInput", keyboardPcInput ? 1 : 0);
	xml->setAttribute("kbPcLayout", keyboardPcLayout);

	// Utility tab's THEME toggle - see getUiThemeLight().
	xml->setAttribute("uiThemeLight", uiThemeLight ? 1 : 0);
	// Editor drawer's own height, drag-resized via the keyboard handle band - see
	// getEditorPaneRefH().
	xml->setAttribute("editorPaneRefH", editorPaneRefH);

	// The D-20-style sequencer's own tracks and transport settings, packed the same way
	// the firmware NVRAM is above - so a project brings the whole sequencer back exactly
	// as left, not just the instrument. The playhead position and armed/record/play state
	// are deliberately NOT saved: like most sequencers, a reopened project starts stopped
	// at bar 1 rather than mid-transport.
	// Metronome, precount, record mode and loop/punch read as workspace preferences rather
	// than song content (see D110SequencerEngine::newSong()'s own comment), so they're saved
	// once, not per slot.
	xml->setAttribute("seqMetronome", sequencerEngine.getMetronomeEnabled() ? 1 : 0);
	xml->setAttribute("seqMetronomeMode", static_cast<int>(sequencerEngine.getMetronomeMode()));
	xml->setAttribute("seqMetronomeChannel10", sequencerEngine.getMetronomeUseChannel10() ? 1 : 0);
	xml->setAttribute("seqMetronomeRecordOnly", sequencerEngine.getMetronomeRecordOnly() ? 1 : 0);
	xml->setAttribute("seqMetronomeVolume", double(sequencerEngine.getMetronomeVolume()));
	xml->setAttribute("seqPrecountBars", sequencerEngine.getPrecountBars());
	xml->setAttribute("seqRecordMode", static_cast<int>(sequencerEngine.getRecordMode()));
	xml->setAttribute("seqStepGrid", static_cast<int>(sequencerEngine.getStepDuration()));
	xml->setAttribute("seqStepDotted", sequencerEngine.getStepDotted() ? 1 : 0);
	xml->setAttribute("seqLoopMode", static_cast<int>(sequencerEngine.getLoopMode()));
	xml->setAttribute("seqPunchIn", sequencerEngine.getPunchIn());
	xml->setAttribute("seqPunchOut", sequencerEngine.getPunchOut());

	// Tempo, time signature and the 9 tracks ARE song content, so all kNumSongSlots songs
	// are saved, not just the one currently selected - see writeSequencerSongsXml()'s own
	// comment. "seqCurrentSlot" being present at all is what tells setStateInformation this
	// is the new, slotted format rather than an older single-song project - see its own
	// comment.
	writeSequencerSongsXml(*xml);

	copyXmlToBinary(*xml, destData);
}

void D110AudioProcessor::writeSequencerSongsXml(juce::XmlElement &xml) const {
	xml.setAttribute("seqCurrentSlot", sequencerEngine.getCurrentSongSlot());
	for (int slot = 0; slot < d110seq::D110SequencerEngine::kNumSongSlots; ++slot) {
		const juce::String slotSuffix = "Slot" + juce::String(slot);
		xml.setAttribute("seqTempo" + slotSuffix, sequencerEngine.slotTempo(slot));
		xml.setAttribute("seqTimeSigNum" + slotSuffix, sequencerEngine.slotTimeSigNumerator(slot));
		xml.setAttribute("seqTimeSigDen" + slotSuffix, sequencerEngine.slotTimeSigDenominator(slot));
		for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t) {
			const juce::String suffix = slotSuffix + juce::String(t);
			xml.setAttribute("seqMute" + suffix, sequencerEngine.slotTrackMuted(slot, t) ? 1 : 0);
			xml.setAttribute("seqSolo" + suffix, sequencerEngine.slotTrackSoloed(slot, t) ? 1 : 0);
			xml.setAttribute("seqQuantize" + suffix, static_cast<int>(sequencerEngine.slotTrackQuantize(slot, t)));
			xml.setAttribute("seqTrack" + suffix, packBlock(sequencerEngine.slotTrackToBytes(slot, t)));
		}
	}
}

// selectSongSlot() also stops/rewinds/disarms, which is fine here: setStateInformation's
// caller already expects the transport to come up stopped, and exportSequencerSongs'
// caller is a deliberate, explicit load the user asked for.
void D110AudioProcessor::readSequencerSongsXml(const juce::XmlElement &xml) {
	sequencerEngine.selectSongSlot(juce::jlimit(
		0, d110seq::D110SequencerEngine::kNumSongSlots - 1, xml.getIntAttribute("seqCurrentSlot", 0)));
	for (int slot = 0; slot < d110seq::D110SequencerEngine::kNumSongSlots; ++slot) {
		const juce::String slotSuffix = "Slot" + juce::String(slot);
		sequencerEngine.setSlotTempo(slot, xml.getDoubleAttribute("seqTempo" + slotSuffix,
		                                                          sequencerEngine.slotTempo(slot)));
		sequencerEngine.setSlotTimeSignature(
			slot, xml.getIntAttribute("seqTimeSigNum" + slotSuffix, sequencerEngine.slotTimeSigNumerator(slot)),
			xml.getIntAttribute("seqTimeSigDen" + slotSuffix, sequencerEngine.slotTimeSigDenominator(slot)));
		for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t) {
			const juce::String suffix = slotSuffix + juce::String(t);
			const auto trackBytes = unpackBlock(xml.getStringAttribute("seqTrack" + suffix));
			sequencerEngine.slotTrackFromBytes(slot, t, trackBytes.getData(), trackBytes.getSize());
			sequencerEngine.setSlotTrackMuted(slot, t, xml.getIntAttribute("seqMute" + suffix, 0) != 0);
			sequencerEngine.setSlotTrackSoloed(slot, t, xml.getIntAttribute("seqSolo" + suffix, 0) != 0);
			sequencerEngine.setSlotTrackQuantize(
				slot, t, static_cast<d110seq::QuantizeGrid>(xml.getIntAttribute("seqQuantize" + suffix, 0)));
		}
	}
}

void D110AudioProcessor::exportSequencerSongs(const juce::File &file) {
	juce::XmlElement xml("D110SequencerSongs");
	xml.setAttribute("version", 1);
	writeSequencerSongsXml(xml);
	if (!xml.writeTo(file)) {
		lastImportMessage = "Could not write songs file: " + file.getFullPathName();
		return;
	}
	lastImportMessage = "Saved sequencer songs: " + file.getFileName();
}

void D110AudioProcessor::importSequencerSongs(const juce::File &file) {
	std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(file));
	if (xml == nullptr || !xml->hasTagName("D110SequencerSongs")) {
		lastImportMessage = "Not a D-110 sequencer songs file: " + file.getFileName();
		return;
	}
	readSequencerSongsXml(*xml);
	lastImportMessage = "Loaded sequencer songs: " + file.getFileName();
}

void D110AudioProcessor::setStateInformation(const void *data, int sizeInBytes) {
	std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
	if (xml == nullptr || !xml->hasTagName(parameters.state.getType())) return;

	parameters.replaceState(juce::ValueTree::fromXml(*xml));

	const auto crp = xml->getStringAttribute("controlRomPath");
	const auto prp = xml->getStringAttribute("pcmRomPath");
	if (crp.isNotEmpty() && prp.isNotEmpty()) {
		controlRomPath = crp;
		pcmRomPath = prp;
		openSynthIfReady();
	}

	// Атрибут forwardNotes из старых проектов намеренно игнорируется. Пока он был
	// переключателем в меню, его можно было сохранить выключенным; теперь переключателя
	// нет, и восстановленное «выключено» оставило бы инструмент без индикации партий и без
	// собственных диапазонов клавиш прошивки, причём включить обратно было бы нечем.
	forwardNotes = true;

	// Reopening by identifier: if the device is not present on this machine the call simply
	// finds nothing and the plugin stays on host MIDI alone, which is the right outcome for
	// a project carried to a different computer.
	setMidiInputDevice(xml->getStringAttribute("midiIn"));
	setMidiOutputDevice(xml->getStringAttribute("midiOut"));

	// Put the project's own firmware memory back, while the machine is certainly not
	// running - the plugin always comes up powered off, and MAME reads these files once at
	// start. Hitting POWER then boots the instrument the project was saved with.
	//
	// This overwrites the shared memory, and that is the intent: there is one instrument,
	// so loading a project sets it to what that project had, exactly as loading a bank into
	// the hardware would. A plugin loaded WITHOUT a project touches none of this and simply
	// finds the memory as it was last left.
	const auto rams = unpackBlock(xml->getStringAttribute("nvramRams"));
	const auto memcs = unpackBlock(xml->getStringAttribute("nvramMemcs"));
	if (rams.getSize() > 0 || memcs.getSize() > 0)
		writeNvramFiles(rams, memcs);

	// Проект старше этой возможности карту не вынимал, поэтому по умолчанию она на месте.
	// Содержимое её при этом уже лежит в файле выше, и ядро подхватит его при включении даже
	// с вынутой картой - см. D110CoreType::osdApplyCard.
	core.setCardInserted(xml->getIntAttribute("cardInserted", 1) != 0);
	core.setCardWriteProtect(xml->getIntAttribute("cardWriteProtect", 0) != 0);

	// The on-screen test keyboard's own config - a project saved before this existed simply
	// has none of these attributes, and every getAttribute call below already has this
	// instance's own current (default) value as its fallback.
	setKeyboardMidiChannel(xml->getIntAttribute("kbChannel", keyboardMidiChannel));
	setKeyboardOmni(xml->getIntAttribute("kbOmni", keyboardOmni ? 1 : 0) != 0);
	setKeyboardPcInputEnabled(xml->getIntAttribute("kbPcInput", keyboardPcInput ? 1 : 0) != 0);
	setKeyboardPcLayout(xml->getIntAttribute("kbPcLayout", keyboardPcLayout));

	setUiThemeLight(xml->getIntAttribute("uiThemeLight", uiThemeLight ? 1 : 0) != 0);
	setEditorPaneRefH(float(xml->getDoubleAttribute("editorPaneRefH", double(editorPaneRefH))));

	// The sequencer's own tracks and transport settings - see the matching comment in
	// getStateInformation. A project saved before this existed simply has none of these
	// attributes, and every getAttribute call below already has the engine's own default
	// as its fallback, so an old project loads with a fresh, empty sequencer rather than
	// an error.
	sequencerEngine.setMetronomeEnabled(xml->getIntAttribute("seqMetronome", 1) != 0);
	sequencerEngine.setMetronomeMode(static_cast<d110seq::MetronomeMode>(
	    xml->getIntAttribute("seqMetronomeMode", static_cast<int>(d110seq::MetronomeMode::both))));
	sequencerEngine.setMetronomeUseChannel10(xml->getIntAttribute("seqMetronomeChannel10", 0) != 0);
	sequencerEngine.setMetronomeRecordOnly(xml->getIntAttribute("seqMetronomeRecordOnly", 0) != 0);
	sequencerEngine.setMetronomeVolume(float(xml->getDoubleAttribute("seqMetronomeVolume", 1.0)));
	// "seqPrecountBars" is the current attribute; "seqPrecount" (a plain 0/1 toggle) is
	// what an older project saved before precount became a 0/1/2-bar cycle - read as 1
	// bar when it was on, matching that toggle's old meaning.
	if (xml->hasAttribute("seqPrecountBars"))
		sequencerEngine.setPrecountBars(xml->getIntAttribute("seqPrecountBars", 1));
	else
		sequencerEngine.setPrecountBars(xml->getIntAttribute("seqPrecount", 1) != 0 ? 1 : 0);
	sequencerEngine.setRecordMode(static_cast<d110seq::RecordMode>(
		xml->getIntAttribute("seqRecordMode", static_cast<int>(d110seq::RecordMode::replaceRange))));
	sequencerEngine.setStepDuration(static_cast<d110seq::QuantizeGrid>(
		xml->getIntAttribute("seqStepGrid", static_cast<int>(d110seq::QuantizeGrid::eighth))));
	sequencerEngine.setStepDotted(xml->getIntAttribute("seqStepDotted", 0) != 0);
	sequencerEngine.setPunchIn(xml->getIntAttribute("seqPunchIn", sequencerEngine.getPunchIn()));
	sequencerEngine.setPunchOut(xml->getIntAttribute("seqPunchOut", sequencerEngine.getPunchOut()));
	sequencerEngine.setLoopMode(static_cast<d110seq::LoopMode>(
		xml->getIntAttribute("seqLoopMode", static_cast<int>(d110seq::LoopMode::off))));

	// "seqCurrentSlot" only exists in projects saved after the 4-song-slot feature - an
	// older project has a single, unslotted song under the plain "seqTempo"/"seqTrack0"/...
	// names, which lands in slot 0 here; slots 1-3 simply stay empty.
	if (xml->hasAttribute("seqCurrentSlot")) {
		readSequencerSongsXml(*xml);
	} else {
		sequencerEngine.setTempo(xml->getDoubleAttribute("seqTempo", sequencerEngine.getTempo()));
		sequencerEngine.setTimeSignature(
			xml->getIntAttribute("seqTimeSigNum", sequencerEngine.getTimeSigNumerator()),
			xml->getIntAttribute("seqTimeSigDen", sequencerEngine.getTimeSigDenominator()));
		for (int t = 0; t < d110seq::D110SequencerEngine::kNumTracks; ++t) {
			const juce::String suffix(t);
			const auto trackBytes = unpackBlock(xml->getStringAttribute("seqTrack" + suffix));
			sequencerEngine.trackFromBytes(t, trackBytes.getData(), trackBytes.getSize());
			sequencerEngine.setTrackMuted(t, xml->getIntAttribute("seqMute" + suffix, 0) != 0);
			sequencerEngine.setTrackSoloed(t, xml->getIntAttribute("seqSolo" + suffix, 0) != 0);
			sequencerEngine.quantizeTrack(
				t, static_cast<d110seq::QuantizeGrid>(xml->getIntAttribute("seqQuantize" + suffix, 0)));
		}
	}
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
	return new D110AudioProcessor();
}
