#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Ported from tools/d5_extract/d5_rom.py's bit-reorder/log2 PCM decode and
// D5RomSet's ROM identification - so an end user (not just Alan, with the
// Python/CMake toolchain) can point this app at their own raw D-50 PCM ROM
// dump(s) and get sound, the same "drop your files, the app finds them"
// experience the D-110 backends already give their firmware ROMs.
//
// Deliberately narrower than d5_rom.py's own D5RomSet: only the two PCM ROMs
// (audio DATA, decoded here) are needed for playback. The program EPROM's own
// name table is not read at all - the sample geometry and names ship compiled
// in (d5_pcm_table.h, from tools/d5_extract's frozen d5_sample_table.json),
// not derived from any specific user's EPROM dump - so this loader has one
// job, decoding audio, not identifying a whole ROM set.
namespace d5 {

struct RomLoadResult {
    bool ok = false;
    std::string message;       // human-readable outcome, success or failure
    std::string sourceA, sourceB;  // which files were used, for the status line
    std::vector<int16_t> pcm;  // chip A's samples then chip B's - ready for D5_Bridge::init()
};

// Scans every regular file directly inside `dir` (non-recursive) for a PCM
// ROM A/B pair, identified by CRC32 after folding a doubled dump (a 256 KB
// chip read out as 512 KB) down to one copy - exactly d5_rom.py's own
// _fold()/CRC table, so any dump that tool accepts is accepted here too,
// regardless of filename.
RomLoadResult loadPcmFromRomFolder(const std::string &dir);

}  // namespace d5
