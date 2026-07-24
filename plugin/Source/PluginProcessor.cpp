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

	params.push_back(std::make_unique<juce::AudioParameterFloat>(
		juce::ParameterID{"masterVolume", 1}, "Master Volume",
		juce::NormalisableRange<float>(0.0f, 1.5f), 1.0f));

	params.push_back(std::make_unique<juce::AudioParameterBool>(
		juce::ParameterID{"reverbEnabled", 1}, "Reverb", true));

	params.push_back(std::make_unique<juce::AudioParameterBool>(
		juce::ParameterID{"superMode", 1}, "Super Mode (unofficial)", false));

	return {params.begin(), params.end()};
}

D110AudioProcessor::D110AudioProcessor()
	: AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
	  parameters(*this, nullptr, "PARAMS", createParameterLayout()) {
	masterVolumeParam = parameters.getRawParameterValue("masterVolume");
	reverbEnabledParam = parameters.getRawParameterValue("reverbEnabled");
	superModeParam = parameters.getRawParameterValue("superMode");

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
	controlRomPath = path;
	openSynthIfReady();
}

void D110AudioProcessor::setPcmRomPath(const juce::String &path) {
	pcmRomPath = path;
	openSynthIfReady();
}

juce::File D110AudioProcessor::getAutoRomFolder() {
	// Colocated with the platform's standard shared VST3 folder (see VST3_COPY_DIR in
	// plugin/CMakeLists.txt) - not AppData/etc, which was only ever a leftover from this
	// project's original CMakeLists template.
#if JUCE_WINDOWS
	return juce::File("C:/Program Files/Common Files/VST3/D-110 Data");
#elif JUCE_MAC
	return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
		.getChildFile("Library/Audio/Plug-Ins/VST3/D-110 Data");
#else
	return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
		.getChildFile(".vst3/D-110_Data");
#endif
}

bool D110AudioProcessor::tryAutoLoadRoms() {
	auto folder = getAutoRomFolder();
	if (!folder.isDirectory()) return false;

	juce::String foundControl, foundPcm;
	for (const auto &entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFiles)) {
		auto file = entry.getFile();
		MT32Emu::FileStream probe;
		if (!probe.open(file.getFullPathName().toRawUTF8())) continue;

		const MT32Emu::ROMImage *image = MT32Emu::ROMImage::makeROMImage(&probe);
		const MT32Emu::ROMInfo *info = image->getROMInfo();
		if (info != nullptr) {
			if (info->type == MT32Emu::ROMInfo::Control && foundControl.isEmpty())
				foundControl = file.getFullPathName();
			else if (info->type == MT32Emu::ROMInfo::PCM && foundPcm.isEmpty())
				foundPcm = file.getFullPathName();
		}
		MT32Emu::ROMImage::freeROMImage(image);
		probe.close();
	}

	if (foundControl.isEmpty() || foundPcm.isEmpty()) return false;

	controlRomPath = foundControl;
	pcmRomPath = foundPcm;
	return openSynthIfReady();
}

bool D110AudioProcessor::openSynthIfReady() {
	if (controlRomPath.isEmpty() || pcmRomPath.isEmpty()) {
		lastError = "Waiting for both Control ROM and PCM ROM to be selected.";
		return false;
	}

	closeSynth();
	lastError.clear();

	auto newControlFile = std::make_unique<MT32Emu::FileStream>();
	if (!newControlFile->open(controlRomPath.toRawUTF8())) {
		lastError = "Could not open control ROM file: " + controlRomPath;
		return false;
	}
	auto newPcmFile = std::make_unique<MT32Emu::FileStream>();
	if (!newPcmFile->open(pcmRomPath.toRawUTF8())) {
		lastError = "Could not open PCM ROM file: " + pcmRomPath;
		return false;
	}

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

juce::AudioProcessorEditor *D110AudioProcessor::createEditor() {
	return new D110AudioProcessorEditor(*this);
}

D110AudioProcessor::LcdSnapshot D110AudioProcessor::getLcdSnapshot() const {
	LcdSnapshot snapshot;
	snapshot.selectedPartNumber = selectedPartIndex.load() + 1;

	if (!synth) {
		snapshot.patchLine = "(no ROMs loaded)";
		return snapshot;
	}

	// Only used for its boolean return value (whether the MIDI LED is lit) - the text buffer
	// this also fills isn't used, since mt32emu's own text flashes transiently and reverts, and
	// its internal buffer can leave stale bytes behind between different message lengths.
	char unusedBuffer[21] = {};
	snapshot.midiLedOn = synth->getDisplayState(unusedBuffer, false);
	snapshot.partStatesBitmask = synth->getPartStates();

	// Row 1 already says which Part is selected, so row 2 doesn't repeat "PartN" - that would be
	// the same duplication the real LCD doesn't have. We show the patch number we track ourselves
	// (honest, since we don't know the real unit's exact bank-letter addressing scheme) plus name.
	const int part = selectedPartIndex.load();
	const char *name = synth->getPatchName(static_cast<MT32Emu::Bit8u>(part));
	const int program = currentProgramPerPart[static_cast<size_t>(part)];
	snapshot.patchLine = "Patch " + juce::String(program + 1).paddedLeft('0', 3) + ": "
		+ (name != nullptr ? juce::String(name).trim() : juce::String("(unknown)"));

	return snapshot;
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

	// D-110 factory default chanAssign maps Part i directly to MIDI channel nibble i (Part 1 ->
	// channel 1), unlike the MT-32's Part i -> channel i+1 (see the isD110ControlROM special case
	// in munt/mt32emu/src/Synth.cpp's Synth::open()).
	const int channelNibble = part;
	const MT32Emu::Bit32u message = static_cast<MT32Emu::Bit32u>(0xC0 | channelNibble)
		| (static_cast<MT32Emu::Bit32u>(newProgram) << 8);

	const juce::ScopedLock sl(engineActionLock);
	pendingShortMessages.push_back(message);
}

void D110AudioProcessor::sendPatchTempByteWrite(int part, int fieldOffset, MT32Emu::Bit8u value) {
	// PatchTemp for Part N (0-7) lives at Roland address 0x030000 + N*0x10, one 8-byte PatchParam
	// record per part (see PatchTempMemoryRegion in munt/mt32emu/src/MemoryRegion.h); byte 0 of
	// that record is timbreGroup, byte 1 is timbreNum. Offsets stay well under the 0x80 (7-bit)
	// boundary for parts 0-7, so plain hex addition of the offset is safe here - no address
	// nibblization/carry handling needed (contrast the 9th, rhythm-only entry at 0x030080, which
	// would need it and isn't targeted by this function).
	const MT32Emu::Bit8u addrHi = 0x03;
	const MT32Emu::Bit8u addrMid = 0x00;
	const auto addrLo = static_cast<MT32Emu::Bit8u>(part * 0x10 + fieldOffset);

	std::vector<MT32Emu::Bit8u> message = {0xF0, 0x41, 0x10, 0x16, 0x12, addrHi, addrMid, addrLo, value};
	unsigned int checksum = 0;
	for (MT32Emu::Bit8u b : {addrHi, addrMid, addrLo, value}) checksum -= b;
	message.push_back(static_cast<MT32Emu::Bit8u>(checksum & 0x7fu));
	message.push_back(0xF7);

	const juce::ScopedLock sl(engineActionLock);
	pendingSysexImports.push_back(std::move(message));
}

void D110AudioProcessor::changeGroup(int direction) {
	const int part = selectedPartIndex.load();
	int newGroup = (currentTimbreGroupPerPart[static_cast<size_t>(part)] + direction) % 4;
	if (newGroup < 0) newGroup += 4;
	currentTimbreGroupPerPart[static_cast<size_t>(part)] = newGroup;
	sendPatchTempByteWrite(part, 0, static_cast<MT32Emu::Bit8u>(newGroup));
}

void D110AudioProcessor::changeBank(int direction) {
	const int part = selectedPartIndex.load();
	int newNum = (currentTimbreNumPerPart[static_cast<size_t>(part)] + direction) % 64;
	if (newNum < 0) newNum += 64;
	currentTimbreNumPerPart[static_cast<size_t>(part)] = newNum;
	sendPatchTempByteWrite(part, 1, static_cast<MT32Emu::Bit8u>(newNum));
}

void D110AudioProcessor::getStateInformation(juce::MemoryBlock &destData) {
	auto state = parameters.copyState();
	std::unique_ptr<juce::XmlElement> xml(state.createXml());
	xml->setAttribute("controlRomPath", controlRomPath);
	xml->setAttribute("pcmRomPath", pcmRomPath);
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
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
	return new D110AudioProcessor();
}
