#include "PluginProcessor.h"
#include "PluginEditor.h"

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

	// 0..2 rather than 0..1.5 so that unity gain - the default - is the exact middle of the range,
	// and therefore sits dead centre on the panel knob's printed scale (which the reference photo
	// shows is centred on straight-up to within 0.03 degrees). The default loudness is unchanged;
	// the knob simply now points at 12 o'clock at rest instead of leaning right.
	params.push_back(std::make_unique<juce::AudioParameterFloat>(
		juce::ParameterID{"masterVolume", 1}, "Master Volume",
		juce::NormalisableRange<float>(0.0f, 2.0f), 1.0f));

	params.push_back(std::make_unique<juce::AudioParameterBool>(
		juce::ParameterID{"reverbEnabled", 1}, "Reverb", true));

	params.push_back(std::make_unique<juce::AudioParameterBool>(
		juce::ParameterID{"superMode", 1}, "Super Mode (unofficial)", false));

	return {params.begin(), params.end()};
}

// Instance ids in use right now. Guarded because a host may build and tear down plugin
// instances from more than one thread.
static juce::CriticalSection gInstanceIdLock;
static juce::StringArray gInstanceIdsInUse;

bool D110AudioProcessor::claimInstanceId(const juce::String &id) {
	const juce::ScopedLock sl(gInstanceIdLock);
	if (gInstanceIdsInUse.contains(id)) return false;
	gInstanceIdsInUse.add(id);
	return true;
}

void D110AudioProcessor::releaseInstanceId(const juce::String &id) {
	const juce::ScopedLock sl(gInstanceIdLock);
	gInstanceIdsInUse.removeString(id);
}

D110AudioProcessor::D110AudioProcessor()
	: AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
	  parameters(*this, nullptr, "PARAMS", createParameterLayout()) {
	masterVolumeParam = parameters.getRawParameterValue("masterVolume");
	reverbEnabledParam = parameters.getRawParameterValue("reverbEnabled");
	superModeParam = parameters.getRawParameterValue("superMode");

	claimInstanceId(instanceId.toDashedString());
	tryAutoLoadRoms();
}

D110AudioProcessor::~D110AudioProcessor() {
	releaseInstanceId(instanceId.toDashedString());
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

		// A brand-new instance has no firmware memory at all, and a D-110 with blank
		// battery RAM boots to an empty patch - it reads as broken rather than as new.
		// Each instance now having its OWN memory makes this the normal case rather than
		// a once-per-install oddity, so do what the manual tells an owner to do with a
		// fresh unit: the documented cold start, performed automatically. Only when there
		// is genuinely nothing to lose - a restored project brings its own memory, and it
		// is written out before this point.
		const bool virgin = !nvram.getChildFile("d110").getChildFile("rams").existsAsFile();

		if (!core.start(getMameRomPath().toStdString(), nvram.getFullPathName().toStdString())) {
			// Another instance already holds the emulated control board. Stay off rather
			// than start a second machine, which crashes the host outright.
			powerBlocked = true;
			lastError = "Another D-110 Emulator instance is already switched on. "
			            "Only one can run at a time - switch that one off first.";
			return;
		}
		powerBlocked = false;
		if (virgin) core.factoryReset();
	} else {
		core.stop();
	}
	poweredOn = shouldBePoweredOn;
}

juce::File D110AudioProcessor::getNvramRoot() {
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
		.getChildFile("D-110 Emulator")
		.getChildFile("nvram");
}

juce::File D110AudioProcessor::getNvramFolder() const {
	return getNvramRoot().getChildFile(instanceId.toDashedString());
}

// MAME keeps each nvram_device in its own file under <nvram_directory>/<machine>/, so the
// D-110's battery RAM is "rams" and its memory card is "memcs", both raw 32 KB images.
void D110AudioProcessor::writeNvramFiles(const juce::MemoryBlock &rams,
                                         const juce::MemoryBlock &memcs) const {
	const auto dir = getNvramFolder().getChildFile("d110");
	dir.createDirectory();
	if (rams.getSize() > 0) dir.getChildFile("rams").replaceWithData(rams.getData(), rams.getSize());
	if (memcs.getSize() > 0) dir.getChildFile("memcs").replaceWithData(memcs.getData(), memcs.getSize());
}

juce::MemoryBlock D110AudioProcessor::readNvramFile(const juce::String &name) const {
	juce::MemoryBlock block;
	const auto file = getNvramFolder().getChildFile("d110").getChildFile(name);
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
	const auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
		.getChildFile("D-110 Emulator").getChildFile("romset");
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
	// Real DAW plugin folder on this machine - see the vst_data_folder_location
	// memory note: all plugin data (ROMs, NVRAM, etc.) lives under here, not
	// AppData (that was only ever a leftover from this project's original
	// CMakeLists template).
	return juce::File("C:/Program Files/Common Files/VST3/D-110 Data");
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
	// whose per-chip dumps have to be joined before mt32emu will know them.
	if (controlRomData.getSize() == 0 || pcmRomData.getSize() == 0)
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
			}
			continue;
		}

		juce::MemoryBlock data;
		if (file.loadFileAsData(data)) consider(data);
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

bool D110AudioProcessor::openSynthIfReady() {
	if (controlRomData.getSize() == 0 || pcmRomData.getSize() == 0) {
		lastError = "Waiting for both Control ROM and PCM ROM to be selected.";
		return false;
	}

	closeSynth();
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
	bool useSuper = superModeParam != nullptr && superModeParam->load() > 0.5f;
	if (!newSynth->open(*newControlImage, *newPcmImage, MT32Emu::DEFAULT_MAX_PARTIALS,
						 MT32Emu::AnalogOutputMode_COARSE, useSuper)) {
		lastError = "Synth failed to open with these ROM files.";
		MT32Emu::ROMImage::freeROMImage(newControlImage);
		MT32Emu::ROMImage::freeROMImage(newPcmImage);
		return false;
	}

	controlRomFile = std::move(newControlFile);
	pcmRomFile = std::move(newPcmFile);
	controlROMImage = newControlImage;
	pcmROMImage = newPcmImage;
	synth = std::move(newSynth);
	lastSuperModeApplied = useSuper;

	controlRomDescription = controlROMImage->getROMInfo()->description;
	pcmRomDescription = pcmROMImage->getROMInfo()->description;

	synth->setReverbEnabled(reverbEnabledParam == nullptr || reverbEnabledParam->load() > 0.5f);

	rebuildSampleRateConverter();
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
	if (synth) rebuildSampleRateConverter();
}

void D110AudioProcessor::releaseResources() {
}

void D110AudioProcessor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) {
	juce::ScopedNoDenormals noDenormals;
	const int numSamples = buffer.getNumSamples();
	buffer.clear();

	if (!synth || !sampleRateConverter || !poweredOn.load()) {
		return;
	}

	// Super Mode can only be applied when the synth is (re)opened, not live. This reopen
	// happens on the audio thread for simplicity - acceptable for a hobby project where the
	// toggle is rarely touched, but not real-time safe. A future revision should move this to
	// the message thread instead.
	bool wantSuper = superModeParam->load() > 0.5f;
	if (wantSuper != lastSuperModeApplied) {
		openSynthIfReady();
		if (!synth || !sampleRateConverter) return;
	}

	synth->setReverbEnabled(reverbEnabledParam->load() > 0.5f);

	// Drain anything queued from the UI thread (SysEx bank imports, patch-browsing program
	// changes, stub-button LCD messages) and actually apply it here on the audio thread, since
	// Synth's MIDI queue only tolerates a single writer thread.
	std::vector<std::vector<MT32Emu::Bit8u>> pendingImportsToSend;
	std::vector<MT32Emu::Bit32u> pendingShortMessagesToSend;
	{
		const juce::ScopedLock sl(engineActionLock);
		pendingImportsToSend.swap(pendingSysexImports);
		pendingShortMessagesToSend.swap(pendingShortMessages);
	}
	for (auto &message : pendingImportsToSend)
		synth->playSysex(message.data(), static_cast<MT32Emu::Bit32u>(message.size()));
	for (auto message : pendingShortMessagesToSend)
		synth->playMsg(message);

	// The bridge: whenever the control board's own parameter memory changes - because a
	// button was pressed on the panel - the core hands us the Roland exclusive message
	// the hardware would have used, and the LA engine takes it natively. This is what
	// makes an edit made on the panel audible.
	{
		MT32Emu::Bit8u sysex[D110Core::kMaxSysexBytes];
		while (const int len = core.popSysex(sysex))
			synth->playSysex(sysex, static_cast<MT32Emu::Bit32u>(len));
	}

	if (static_cast<int>(interleavedScratch.size()) < numSamples * 2)
		interleavedScratch.resize(static_cast<size_t>(numSamples) * 2);

	const float masterVolume = masterVolumeParam->load();
	const int numOutChannels = buffer.getNumChannels();

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
			sampleRateConverter->getOutputSamples(interleavedScratch.data(),
												   static_cast<unsigned int>(samplesToRender));
			float *left = buffer.getWritePointer(0, samplePos);
			float *right = numOutChannels > 1 ? buffer.getWritePointer(1, samplePos) : nullptr;
			for (int i = 0; i < samplesToRender; ++i) {
				const float l = interleavedScratch[static_cast<size_t>(i) * 2] * masterVolume;
				const float r = interleavedScratch[static_cast<size_t>(i) * 2 + 1] * masterVolume;
				left[i] = l;
				if (right != nullptr) right[i] = r;
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
}

void D110AudioProcessor::handleIncomingMidiMessage(const juce::MidiMessage &message) {
	// The control board gets its own copy first. On the real instrument one MIDI cable
	// feeds both the CPU and the voice circuitry; here the CPU is MAME and the voice
	// circuitry is mt32emu, so both have to be told. Without this the firmware never
	// learns a note was played - which is why the top LCD row did not light the playing
	// parts, and why the display drifted away from the host's program changes.
	forwardMidiToFirmware(message);

	if (!synth) return;

	if (message.isSysEx()) {
		synth->playSysex(message.getRawData(), static_cast<MT32Emu::Bit32u>(message.getRawDataSize()));
		return;
	}

	const auto *raw = message.getRawData();
	const auto size = message.getRawDataSize();
	if (size <= 0) return;

	const juce::uint8 status = raw[0];
	const juce::uint8 data1 = size > 1 ? raw[1] : 0;
	const juce::uint8 data2 = size > 2 ? raw[2] : 0;

	const MT32Emu::Bit32u msg = static_cast<MT32Emu::Bit32u>(status)
		| (static_cast<MT32Emu::Bit32u>(data1) << 8)
		| (static_cast<MT32Emu::Bit32u>(data2) << 16);
	synth->playMsg(msg);
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

	// Only used for its boolean return value (whether the MIDI LED is lit) - the text buffer
	// this also fills isn't used, since mt32emu's own text flashes transiently and reverts, and
	// its internal buffer can leave stale bytes behind between different message lengths.
	char unusedBuffer[21] = {};
	snapshot.midiLedOn = synth->getDisplayState(unusedBuffer, false);

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
	lastImportMessage = "Queued " + juce::String(messages.size()) + " SysEx message(s) from "
		+ file.getFileName();
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
	xml->setAttribute("instanceId", instanceId.toDashedString());

	// The firmware's own memory - its patches, its timbres, its edits - so that a project
	// recalls the sounds it was saved with instead of whatever the shared folder happens
	// to hold now.
	//
	// While the machine is running the FILE on disk is stale: MAME only writes its NVRAM
	// out when the machine exits. So take the live image straight from the core, and fall
	// back to the file only when powered off.
	juce::MemoryBlock rams;
	if (core.isRunning()) {
		rams.setSize(D110Core::kRamSize);
		if (!core.getRam(static_cast<juce::uint8 *>(rams.getData()))) rams.reset();
	}
	if (rams.getSize() == 0) rams = readNvramFile("rams");

	// The memory card has no live window, so it can only come off disk.
	const juce::MemoryBlock memcs = readNvramFile("memcs");

	xml->setAttribute("nvramRams", packBlock(rams));
	xml->setAttribute("nvramMemcs", packBlock(memcs));

	copyXmlToBinary(*xml, destData);
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

	// Take back the folder this project was saved with - unless another live instance is
	// already using it, which is what happens when a plugin is copied and pasted rather
	// than newly added. In that case keep the fresh id minted in the constructor, so the
	// copy starts as a copy instead of fighting the original over one folder.
	const auto savedId = xml->getStringAttribute("instanceId");
	if (savedId.isNotEmpty() && savedId != instanceId.toDashedString()
	    && claimInstanceId(savedId)) {
		releaseInstanceId(instanceId.toDashedString());
		instanceId = juce::Uuid(savedId);
	}

	// Land the saved firmware memory in this instance's folder now, while the machine is
	// certainly not running - the plugin always comes up powered off, and MAME reads these
	// files once at start. Hitting POWER then boots the project's own instrument.
	const auto rams = unpackBlock(xml->getStringAttribute("nvramRams"));
	const auto memcs = unpackBlock(xml->getStringAttribute("nvramMemcs"));
	if (rams.getSize() > 0 || memcs.getSize() > 0)
		writeNvramFiles(rams, memcs);
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
	return new D110AudioProcessor();
}
