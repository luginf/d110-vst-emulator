#include "D50AudioProcessor.h"

#include "D50Editor.h"

#include <cmath>
#include <utility>

#include "../../../d50/D5RomLoader.h"
#include "../../../d50/D5SyxLoader.h"

D50AudioProcessor::D50AudioProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
}

D50AudioProcessor::~D50AudioProcessor() = default;

bool D50AudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

// Looks next to the running binary first, matching how the D-110 backends'
// firmware ROMs are found (see PluginProcessor.cpp's getAutoRomFolder()). No
// Utility-tab folder override yet - this is the first playable cut, not the
// full ROM-management UI the D-110 side grew over time.
//
// Preferred path: the user's own raw D-50 PCM ROM dump(s) (IC30/IC29, any
// dump - see d5RomLoader's own comment), decoded right here at startup via
// d5::loadPcmFromRomFolder() - no Python/CMake toolchain needed, the same
// "drop your ROM, the app finds it" experience the D-110 backends already
// give their firmware ROMs. Verified byte-identical to
// tools/d5_extract/d5_make_blob.py's own output against Alan's ROM dump.
//
// Fallback: an already-converted d5_pcm.bin, for advanced/CI use (or a build
// from before this loader existed) - never required, but harmless to keep.
bool D50AudioProcessor::loadPcmBlob() {
    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();

    for (const auto &romDir : {exeDir, exeDir.getChildFile("d50-roms")}) {
        auto result = d5::loadPcmFromRomFolder(romDir.getFullPathName().toStdString());
        if (!result.ok) continue;
        pcmBlob = std::move(result.pcm);
        bridge.init(pcmBlob.data());
        bridge.setVolume(volumePercent);
        status = "D-50 ready (" + juce::String(result.message) + ", " + juce::String(bridge.patchCount()) +
                  " patches)";
        return true;
    }

    const juce::Array<juce::File> preConverted{
        exeDir.getChildFile("d5_pcm.bin"),
        exeDir.getChildFile("d50").getChildFile("d5_pcm.bin"),
    };
    for (const auto &f : preConverted) {
        if (!f.existsAsFile()) continue;
        juce::MemoryBlock mb;
        if (!f.loadFileAsData(mb) || mb.getSize() < 2) continue;
        const auto *samples = static_cast<const int16_t *>(mb.getData());
        pcmBlob.assign(samples, samples + mb.getSize() / 2);
        bridge.init(pcmBlob.data());
        bridge.setVolume(volumePercent);
        status = "D-50 ready (" + f.getFullPathName() + ", " + juce::String(bridge.patchCount()) + " patches)";
        return true;
    }

    status = "No D-50 PCM ROM found next to the app (or in d50-roms/) - see d50/roms/README.md";
    return false;
}

// Looks in the same two places as loadPcmBlob() for a real SysEx patch bank
// (any .syx there - see D5SyxLoader.h). Not required: without one, the
// engine keeps whatever it already has (a compiled-in bank if this build was
// made with one in d50/roms/, or the 8 hand-built presets otherwise) - see
// D5_Bridge::loadBank()'s own comment on why a runtime bank always wins when
// one is found. This is also the real fix for the licensing problem noted in
// plugin/CMakeLists.txt: a shipped binary no longer needs anyone's patch data
// compiled in at all, once nobody relies on that build-time bank existing.
void D50AudioProcessor::loadPatchBank() {
    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (const auto &dir : {exeDir, exeDir.getChildFile("d50-roms")}) {
        auto result = d5::loadBankFromSyxFolder(dir.getFullPathName().toStdString());
        if (!result.ok) continue;
        bridge.loadBank(std::move(result.patches), std::move(result.names));
        status += " + " + juce::String(result.message);
        return;
    }
}

juce::String D50AudioProcessor::importSyxBank(const juce::File &file) {
    auto result = d5::loadBankFromSyx(file.getFullPathName().toStdString());
    if (!result.ok) return "Import failed: " + juce::String(result.message);
    bridge.loadBank(std::move(result.patches), std::move(result.names));
    status = "D-50 ready (bank replaced: " + juce::String(result.message) + ")";
    return juce::String(result.message);
}

namespace {
// One Roland DT1 message: F0 41 00 14 12 <addr 3x7-bit> <448 data> <checksum> F7.
// `addr` is the flat address (aa*16384 + bb*128 + cc) - see D5SyxLoader.cpp's
// own parse_sysex() for the matching decode.
void appendDt1Patch(juce::MemoryOutputStream &out, uint32_t addr, const uint8_t *patch) {
    out.writeByte(static_cast<char>(0xF0));
    out.writeByte(0x41);  // Roland
    out.writeByte(0x00);  // device ID
    out.writeByte(0x14);  // D-50 model ID
    out.writeByte(0x12);  // DT1
    const uint8_t aa = static_cast<uint8_t>((addr >> 14) & 0x7F);
    const uint8_t bb = static_cast<uint8_t>((addr >> 7) & 0x7F);
    const uint8_t cc = static_cast<uint8_t>(addr & 0x7F);
    out.writeByte(static_cast<char>(aa));
    out.writeByte(static_cast<char>(bb));
    out.writeByte(static_cast<char>(cc));
    uint32_t sum = aa + bb + cc;
    for (int i = 0; i < D5_Bridge::kPatchBytes; ++i) {
        out.writeByte(static_cast<char>(patch[i]));
        sum += patch[i];
    }
    out.writeByte(static_cast<char>((128 - sum % 128) % 128));
    out.writeByte(static_cast<char>(0xF7));
}
constexpr uint32_t kPatchBaseAddr = 0x8000;  // 02-00-00, internal memory slot 0
// 00-00-00: the D-50's own "temporary area" - the 448 bytes actually
// sounding, same address D5_Midi.cpp's own `kTempBase` writes through for a
// live parameter edit. Writing here (not kPatchBaseAddr) is what makes
// sendPatchToExternalMidi() take effect on a real unit at once without
// touching any of its 64 stored patches.
constexpr uint32_t kTempAreaAddr = 0x000000;
}  // namespace

bool D50AudioProcessor::exportCurrentPatch(const juce::File &file) const {
    if (!bridgeReady) return false;
    juce::MemoryOutputStream out;
    // Address 02-00-00: internal memory slot 0 - the same address a real
    // bulk dump's first patch uses, so this file loads back in through
    // either this app's own importSyxBank()/ROM-folder auto-load or a real
    // D-50's bulk receive.
    appendDt1Patch(out, kPatchBaseAddr, bridge.tempPatch());
    return file.replaceWithData(out.getData(), out.getDataSize());
}

bool D50AudioProcessor::exportBank(const juce::File &file) const {
    if (!bridgeReady) return false;
    const int count = bridge.patchCount();
    if (count <= 0) return false;
    juce::MemoryOutputStream out;
    for (int i = 0; i < count; ++i) {
        // The sounding slot's live edits only ever land in tempPatch() -
        // sysexWriteTemp() never writes them back into the bank/overlay - so
        // without this substitution a bank export would silently drop
        // whatever the Tone tab just changed for the patch actually playing.
        const uint8_t *src = (i == bridge.patch()) ? bridge.tempPatch() : bridge.storedPatch(i);
        if (src == nullptr) continue;
        appendDt1Patch(out, kPatchBaseAddr + static_cast<uint32_t>(i) * D5_Bridge::kPatchBytes, src);
    }
    return file.replaceWithData(out.getData(), out.getDataSize());
}

void D50AudioProcessor::setMidiOutputDevice(const juce::String &identifier) {
    std::unique_ptr<juce::MidiOutput> opened;
    if (identifier.isNotEmpty()) opened = juce::MidiOutput::openDevice(identifier);
    const juce::ScopedLock lock(osMidiLock);
    osMidiOut = std::move(opened);
    selOutputId = osMidiOut ? identifier : juce::String();
}

bool D50AudioProcessor::sendPatchToExternalMidi() const {
    if (!bridgeReady) return false;
    const juce::ScopedLock lock(osMidiLock);
    if (osMidiOut == nullptr) return false;
    juce::MemoryOutputStream out;
    appendDt1Patch(out, kTempAreaAddr, bridge.tempPatch());
    osMidiOut->sendMessageNow(juce::MidiMessage(out.getData(), static_cast<int>(out.getDataSize())));
    return true;
}

void D50AudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/) {
    hostSampleRate = sampleRate;
    resamplerL.reset();
    resamplerR.reset();
    pendingL.clear();
    pendingR.clear();
    injectedMidi.reset(sampleRate);
    if (!bridgeReady) {
        bridgeReady = loadPcmBlob();
        if (bridgeReady) loadPatchBank();
    }
}

void D50AudioProcessor::releaseResources() {
}

void D50AudioProcessor::setVolumePercent(int v) {
    volumePercent = juce::jlimit(0, 100, v);
    if (bridgeReady) bridge.setVolume(volumePercent);
}

void D50AudioProcessor::handleMidiMessage(const juce::MidiMessage &m) {
    if (!bridgeReady) return;
    if (m.isNoteOn()) {
        bridge.noteOn(static_cast<uint8_t>(m.getNoteNumber()), static_cast<uint8_t>(m.getVelocity()));
        noteActiveFlags[static_cast<size_t>(m.getNoteNumber())].store(true, std::memory_order_relaxed);
    } else if (m.isNoteOff()) {
        bridge.noteOff(static_cast<uint8_t>(m.getNoteNumber()));
        noteActiveFlags[static_cast<size_t>(m.getNoteNumber())].store(false, std::memory_order_relaxed);
    } else if (m.isAllNotesOff() || m.isAllSoundOff()) {
        bridge.allNotesOff();
        for (auto &flag : noteActiveFlags) flag.store(false, std::memory_order_relaxed);
    } else if (m.isPitchWheel()) {
        const float wheel = (static_cast<float>(m.getPitchWheelValue()) - 8192.0f) / 8192.0f;
        bridge.setPitchBendSemis(wheel * static_cast<float>(bridge.bendRangeSemis()));
    } else if (m.isChannelPressure()) {
        bridge.setAftertouch(static_cast<float>(m.getChannelPressureValue()) / 127.0f);
    } else if (m.isProgramChange()) {
        bridge.selectPatch(m.getProgramChangeNumber());
    } else if (m.isController()) {
        const int cc = m.getControllerNumber();
        const int v = m.getControllerValue();
        switch (cc) {
            case 1: bridge.setModWheel(static_cast<float>(v) / 127.0f); break;
            case 5: bridge.setPortamentoTime(v * 100 / 127); break;
            case 6:  // RPN 0 data entry MSB: the bender range, in semitones
                if (rpnMsb == 0 && rpnLsb == 0) bridge.setBendRange(v);
                break;
            case 7: setVolumePercent(v * 100 / 127); break;
            case 64: bridge.setSustain(v >= 64); break;
            case 65: bridge.setPortamentoSwitch(v >= 64); break;
            case 100: rpnLsb = v; break;
            case 101: rpnMsb = v; break;
            case 123: bridge.allNotesOff(); break;
            default: break;
        }
    }
}

void D50AudioProcessor::renderInternal(int numOutSamples, float *outL, float *outR) {
    if (!bridgeReady) {
        std::fill(outL, outL + numOutSamples, 0.0f);
        std::fill(outR, outR + numOutSamples, 0.0f);
        return;
    }

    const double ratio = static_cast<double>(bridge.sampleRate()) / hostSampleRate;
    const int needed = static_cast<int>(std::ceil(numOutSamples * ratio)) + 4;
    const int have = static_cast<int>(pendingL.size());
    if (have < needed) {
        const int toRender = needed - have;
        if (static_cast<int>(renderScratch.size()) < toRender * 2) renderScratch.resize(static_cast<size_t>(toRender) * 2);
        bridge.fillBufferI32(renderScratch.data(), toRender);
        pendingL.reserve(pendingL.size() + static_cast<size_t>(toRender));
        pendingR.reserve(pendingR.size() + static_cast<size_t>(toRender));
        for (int i = 0; i < toRender; ++i) {
            // fillBufferI32 packs the sample in the upper 16 bits of each word
            // (see D5_Bridge.h's own comment) - an arithmetic right shift
            // recovers the signed 16-bit value.
            pendingL.push_back(static_cast<float>(renderScratch[static_cast<size_t>(2 * i)] >> 16) / 32767.0f);
            pendingR.push_back(static_cast<float>(renderScratch[static_cast<size_t>(2 * i + 1)] >> 16) / 32767.0f);
        }
    }

    const int usedL = resamplerL.process(ratio, pendingL.data(), outL, numOutSamples);
    const int usedR = resamplerR.process(ratio, pendingR.data(), outR, numOutSamples);
    const int used = juce::jmax(usedL, usedR);
    pendingL.erase(pendingL.begin(), pendingL.begin() + used);
    pendingR.erase(pendingR.begin(), pendingR.begin() + used);
}

void D50AudioProcessor::injectTestNote(int channel, int note, float velocity, bool on) {
    // D110Keyboard::sendNote() broadcasts each key across all 16 MIDI channels
    // when "MIDI Remap" is off (its default), because on the multitimbral
    // D-110 it cannot know which channel the part it should reach listens on.
    // The D-50 bridge is monotimbral and ignores the channel entirely (see
    // this class's own D110KeyboardHost comment), so 15 of those 16 are pure
    // duplicates: each one used to consume another of the engine's 16 voice
    // slots, so one key could empty the pool and the rest of a chord fell
    // silent. Collapsed here, at the point the duplicates are created, rather
    // than by making a repeat note-on a no-op down in D5_Bridge::noteOn() -
    // that cure cost the retrigger a held sustain pedal depends on.
    if (!midiRemap && channel != 1) return;
    auto message = on ? juce::MidiMessage::noteOn(channel, note, velocity)
                       : juce::MidiMessage::noteOff(channel, note, velocity);
    // Timestamped against Time::getMillisecondCounter()'s base, what
    // MidiMessageCollector expects (same pattern as the D-110 plugin's own
    // injectTestNote() - see PluginProcessor.cpp).
    message.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
    injectedMidi.addMessageToQueue(message);
}

void D50AudioProcessor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages) {
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    // The on-screen keyboard's own notes join the host's MIDI stream here, so
    // from this point on the rest of this method cannot tell them apart.
    {
        juce::MidiBuffer fromKeyboard;
        injectedMidi.removeNextBlockOfMessages(fromKeyboard, numSamples);
        for (const auto metadata : fromKeyboard) midiMessages.addEvent(metadata.getMessage(), metadata.samplePosition);
    }

    for (const auto metadata : midiMessages) handleMidiMessage(metadata.getMessage());

    if (buffer.getNumChannels() >= 2) {
        renderInternal(numSamples, buffer.getWritePointer(0), buffer.getWritePointer(1));
    } else if (buffer.getNumChannels() == 1) {
        std::vector<float> scratchR(static_cast<size_t>(numSamples));
        renderInternal(numSamples, buffer.getWritePointer(0), scratchR.data());
    }
}

juce::AudioProcessorEditor *D50AudioProcessor::createEditor() { return new D50Editor(*this); }

void D50AudioProcessor::getStateInformation(juce::MemoryBlock &destData) {
    juce::MemoryOutputStream out(destData, true);
    out.writeInt(4);  // format version
    out.writeInt(bridgeReady ? bridge.patch() : 0);
    out.writeInt(volumePercent);
    out.writeInt(keyboardMidiChannel);
    out.writeBool(midiRemap);
    out.writeBool(keyboardPcInputEnabled);
    out.writeInt(keyboardPcLayout);
    out.writeString(selOutputId);  // by identifier, not display name - see setMidiOutputDevice()
    out.writeBool(D5_Bridge::tvfKeyfollowFixed());
}

void D50AudioProcessor::setStateInformation(const void *data, int sizeInBytes) {
    juce::MemoryInputStream in(data, static_cast<size_t>(sizeInBytes), false);
    const int version = in.readInt();
    const int patch = in.readInt();
    volumePercent = in.readInt();
    if (version >= 2) {
        keyboardMidiChannel = in.readInt();
        midiRemap = in.readBool();
        keyboardPcInputEnabled = in.readBool();
        keyboardPcLayout = in.readInt();
    }
    if (version >= 3) {
        // If this identifier isn't present on the current machine,
        // openDevice() just returns null and we silently fall back to no
        // external output - same as D110AudioProcessor's own handling.
        setMidiOutputDevice(in.readString());
    }
    if (version >= 4) {
        // Older states predate the switch and were all written by a build
        // that had the corrected direction hard-coded, so the default the
        // engine already holds is the right one to keep for them.
        D5_Bridge::setTvfKeyfollowFixed(in.readBool());
    }
    if (bridgeReady) {
        bridge.selectPatch(patch);
        bridge.setVolume(volumePercent);
    }
}

// This file is only ever built into the D50Emulator plugin target (see
// plugin/CMakeLists.txt) - createPluginFilter() is JUCE's standard plugin
// entry point, expected by the VST3/Standalone wrapper code it links.
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() { return new D50AudioProcessor(); }
