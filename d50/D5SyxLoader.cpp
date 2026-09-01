#include "D5SyxLoader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace d5 {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kPatchBase = 0x8000;  // 02-00-00 in the D-50's 7-bit address space
constexpr int kPatchCountMax = 64;       // the patch area's own 64 slots
constexpr int kBlock = 64;

// ' ', 'A'-'Z', 'a'-'z', '1'-'9', '0', '-' - the panel's own character set.
constexpr char kChars[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz1234567890-";

// (label, block index, offset, maximum) - if the block layout were wrong,
// these would leave their documented ranges at once.
struct Check {
    const char *label;
    int block, offset, hi;
};
constexpr Check kChecks[] = {
    {"structure (upper)", 2, 10, 6},  {"structure (lower)", 5, 10, 6}, {"chorus type", 2, 42, 7},
    {"key mode", 6, 18, 8},           {"reverb type", 6, 30, 31},      {"PCM wave number", 0, 7, 99},
    {"waveform", 0, 6, 1},            {"TVF resonance", 0, 14, 30},
};

std::vector<uint8_t> readFile(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string nameOf(const std::unordered_map<uint32_t, uint8_t> &mem, uint32_t base, int length) {
    std::string out;
    for (int i = 0; i < length; ++i) {
        const auto it = mem.find(base + static_cast<uint32_t>(i));
        const uint8_t v = (it != mem.end() ? it->second : 0) & 0x3F;
        out.push_back(v < sizeof(kChars) - 1 ? kChars[v] : ' ');
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace

SyxLoadResult loadBankFromSyx(const std::string &path) {
    SyxLoadResult result;
    const auto raw = readFile(path);
    if (raw.empty()) {
        result.message = "cannot read " + path;
        return result;
    }

    std::unordered_map<uint32_t, uint8_t> mem;
    size_t i = 0;
    int messages = 0;
    while (i < raw.size()) {
        const auto aIt = std::find(raw.begin() + static_cast<long>(i), raw.end(), uint8_t{0xF0});
        if (aIt == raw.end()) break;
        const auto bIt = std::find(aIt, raw.end(), uint8_t{0xF7});
        if (bIt == raw.end()) break;
        // m mirrors the Python code's raw[a:b+1] exactly, index for index -
        // deliberately not computed from absolute offsets into `raw`, which
        // is easy to get off by one on (the checksum byte is m[-2], not the
        // last byte, and getting that wrong would parse the checksum itself
        // as patch data).
        const std::vector<uint8_t> m(aIt, bIt + 1);
        i = static_cast<size_t>(bIt - raw.begin()) + 1;

        if (m.size() < 10 || m[1] != 0x41 || m[3] != 0x14) continue;  // not a Roland D-50 message
        if (m[4] != 0x12) continue;                                  // DT1 only; a handshake dump would be DAT
        const uint32_t addr = static_cast<uint32_t>(m[5]) * 16384u + static_cast<uint32_t>(m[6]) * 128u + m[7];
        const size_t dataLen = m.size() - 10;  // m[8:-2]
        const uint8_t checksum = m[m.size() - 2];
        uint32_t total = static_cast<uint32_t>(m[5]) + m[6] + m[7];
        for (size_t k = 0; k < dataLen; ++k) total += m[8 + k];
        if ((128 - total % 128) % 128 != checksum) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "checksum mismatch in the message at address %02X-%02X-%02X", m[5], m[6],
                          m[7]);
            result.message = buf;
            return result;
        }
        for (size_t k = 0; k < dataLen; ++k) mem[addr + static_cast<uint32_t>(k)] = m[8 + k];
        ++messages;
    }
    if (messages == 0) {
        result.message = "no D-50 data messages in " + path;
        return result;
    }

    int found = 0;
    for (int p = 0; p < kPatchCountMax; ++p) {
        const uint32_t base = kPatchBase + static_cast<uint32_t>(p) * static_cast<uint32_t>(kSyxPatchBytes);
        bool complete = true;
        for (int b = 0; b < kSyxPatchBytes; ++b) {
            if (mem.find(base + static_cast<uint32_t>(b)) == mem.end()) {
                complete = false;
                break;
            }
        }
        if (!complete) break;
        std::vector<uint8_t> patch(static_cast<size_t>(kSyxPatchBytes));
        for (int b = 0; b < kSyxPatchBytes; ++b) patch[static_cast<size_t>(b)] = mem[base + static_cast<uint32_t>(b)];
        result.patches.push_back(std::move(patch));
        result.names.push_back(nameOf(mem, base + static_cast<uint32_t>(6 * kBlock), 18));
        ++found;
    }
    if (found == 0) {
        result.message = path + " has no patch memory at address 02-00-00";
        return result;
    }

    for (const auto &c : kChecks) {
        for (const auto &p : result.patches) {
            const int v = p[static_cast<size_t>(c.block * kBlock + c.offset)];
            if (v < 0 || v > c.hi) {
                result.message = std::string(c.label) + " is out of range - the block layout does not hold";
                result.patches.clear();
                result.names.clear();
                return result;
            }
        }
    }

    result.ok = true;
    result.message = std::to_string(found) + " patches from " + fs::path(path).filename().string();
    return result;
}

SyxLoadResult loadBankFromSyxFolder(const std::string &dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        SyxLoadResult r;
        r.message = "SysEx folder not found: " + dir;
        return r;
    }
    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext == ".syx") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto &f : files) {
        auto r = loadBankFromSyx(f.string());
        if (r.ok) return r;
    }
    SyxLoadResult r;
    r.message = "no usable .syx bank found in " + dir;
    return r;
}

}  // namespace d5
