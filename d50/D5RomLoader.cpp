#include "D5RomLoader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace d5 {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kPcmACrc = 0x1461C0FB;         // TC532000P-7469, IC30, lower 128K words
constexpr uint32_t kPcmBCrc = 0xE50599BF;         // TC532000P-7470, IC29, upper 128K words
constexpr uint32_t kPcmCombinedCrc = 0xE2AED2D9;  // TC534000P-7477, late boards, A+B in one

std::vector<uint8_t> readFile(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Standard reflected CRC-32 (polynomial 0xEDB88320), matching Python's zlib.crc32.
uint32_t crc32(const std::vector<uint8_t> &data) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// A 256 KB mask ROM read out as a 512 KB (or larger) dump repeats its content
// - fold it down to one copy, exactly d5_rom.py's own _fold().
std::vector<uint8_t> fold(std::vector<uint8_t> data) {
    while (data.size() >= 2 && data.size() % 2 == 0) {
        const size_t half = data.size() / 2;
        if (!std::equal(data.begin(), data.begin() + static_cast<long>(half), data.begin() + static_cast<long>(half)))
            break;
        data.resize(half);
    }
    return data;
}

// The PCM ROMs hold 16-bit big-endian words whose bits are permuted by the
// board routing, and whose value is a sign bit plus a 15-bit log2 magnitude -
// see d5_rom.py's own comment (VOGONS thread on LA-synth sample ROMs,
// t=77094; this is an independent implementation of that description).
uint32_t reorderBits(uint32_t raw) {
    uint32_t o = raw & 0x8000u;
    o |= (raw << 8) & 0x4000u;
    o |= (raw >> 1) & 0x3F80u;
    o |= (raw << 1) & 0x007Eu;
    o |= (raw >> 7) & 0x0001u;
    return o;
}

// double, not float: d5_rom.py's own LUT is built with Python's native
// (double-precision) float, and int(v * 32767) truncates - matching its
// precision exactly avoids an off-by-one-LSB rounding mismatch on samples
// that land within a hair of an integer boundary (~0.02% of them, verified
// against the Python output when this was float: 44 samples off by 1 LSB).
const std::array<double, 65536> &decodeLut() {
    static const auto lut = [] {
        std::array<double, 65536> l{};
        for (uint32_t raw = 0; raw < 65536; ++raw) {
            const uint32_t o = reorderBits(raw);
            const uint32_t mag = o & 0x7FFFu;
            const double amp = std::pow(2.0, (static_cast<double>(mag) - 32767.0) / 2048.0);
            l[raw] = (o & 0x8000u) ? -amp : amp;
        }
        return l;
    }();
    return lut;
}

// Decodes raw ROM bytes to the same signed-16-bit quantization
// tools/d5_extract/d5_make_blob.py writes into d5_pcm.bin, so a ROM dump
// decoded here or by that script produce byte-identical audio.
std::vector<int16_t> decodePcm(const std::vector<uint8_t> &data) {
    const auto &lut = decodeLut();
    std::vector<int16_t> out;
    out.reserve(data.size() / 2);
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        const uint32_t raw = (static_cast<uint32_t>(data[i]) << 8) | data[i + 1];
        const double v = lut[raw];
        int sample = static_cast<int>(v * 32767.0);
        sample = std::clamp(sample, -32767, 32767);
        out.push_back(static_cast<int16_t>(sample));
    }
    return out;
}

}  // namespace

RomLoadResult loadPcmFromRomFolder(const std::string &dir) {
    RomLoadResult result;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        result.message = "ROM folder not found: " + dir;
        return result;
    }

    std::vector<uint8_t> pcmA, pcmB, pcmCombined;
    std::string pcmAPath, pcmBPath, pcmCombinedPath;

    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        // ROM dumps only - large unrelated files (patch banks, this app's own
        // executable if it ever lived in the same folder) are skipped by size,
        // same guard instrument.cmake's own ROM glob relies on implicitly by
        // only ever matching *.bin in a ROM folder.
        std::error_code sizeEc;
        const auto sz = fs::file_size(entry.path(), sizeEc);
        if (sizeEc || sz > (2u << 20)) continue;

        auto data = fold(readFile(entry.path()));
        if (data.empty()) continue;
        const uint32_t crc = crc32(data);
        if (crc == kPcmACrc) {
            pcmA = std::move(data);
            pcmAPath = entry.path().filename().string();
        } else if (crc == kPcmBCrc) {
            pcmB = std::move(data);
            pcmBPath = entry.path().filename().string();
        } else if (crc == kPcmCombinedCrc) {
            pcmCombined = std::move(data);
            pcmCombinedPath = entry.path().filename().string();
        }
    }

    if (pcmA.empty() && !pcmCombined.empty()) {
        const size_t half = pcmCombined.size() / 2;
        pcmA.assign(pcmCombined.begin(), pcmCombined.begin() + static_cast<long>(half));
        pcmB.assign(pcmCombined.begin() + static_cast<long>(half), pcmCombined.end());
        pcmAPath = pcmBPath = pcmCombinedPath;
    }

    if (pcmA.empty() || pcmB.empty()) {
        result.message = "No D-50 PCM ROM pair found in " + dir + " - needs the two PCM ROMs (IC30/IC29), "
                          "any dump including a doubled 512 KB read-out; see d50/roms/README.md";
        return result;
    }

    result.pcm = decodePcm(pcmA);
    const auto pcmBDecoded = decodePcm(pcmB);
    result.pcm.insert(result.pcm.end(), pcmBDecoded.begin(), pcmBDecoded.end());
    result.sourceA = pcmAPath;
    result.sourceB = pcmBPath;
    result.ok = true;
    result.message = "decoded " + pcmAPath + " + " + pcmBPath;
    return result;
}

}  // namespace d5
