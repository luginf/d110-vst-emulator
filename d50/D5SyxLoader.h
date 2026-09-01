#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Ported from tools/d5_extract/d5_syx_to_patches.py's parse_sysex()/name_of() -
// same reasoning as D5RomLoader.h: an end user's own D-50 SysEx bulk dump (a
// real factory or user bank, 64 patches, DT1 messages from address 02-00-00
// upward) is parsed and validated right here at startup, no Python/CMake
// toolchain needed. See D5_Bridge::loadBank() for how this feeds the engine.
namespace d5 {

inline constexpr int kSyxPatchBytes = 448;

struct SyxLoadResult {
    bool ok = false;
    std::string message;                        // outcome, success or failure, for the status line
    std::vector<std::vector<uint8_t>> patches;   // kSyxPatchBytes each
    std::vector<std::string> names;              // same length as patches
};

// Parses one Roland D-50 SysEx bulk dump. Every checksum and eight
// documented parameter ranges are checked (same CHECKS table as the Python
// converter), so a file that is not really a D-50 bank is rejected rather
// than silently producing garbage patches.
SyxLoadResult loadBankFromSyx(const std::string &path);

// Scans `dir` (non-recursive, case-insensitive .syx) and returns the first
// file that parses and validates.
SyxLoadResult loadBankFromSyxFolder(const std::string &dir);

}  // namespace d5
