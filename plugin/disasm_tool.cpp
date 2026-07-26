// Disassembles the D-110's firmware around an address, using MAME's own MCS-96
// disassembler - which is already linked in, so nothing extra is needed.
//
// This exists because hang_probe.cpp found the firmware sitting in a two-instruction loop
// at 0x29E9/0x29EE whenever notes reach it, and the only way to learn what that loop is
// waiting for is to read it.
//
// CPU address == firmware ROM offset here: d110_map sends 0x1000-0x7fff to the bank at
// the same address, and bank 0x00000-0x0ffff is the firmware region.
#include "emu.h"
#include "cpu/mcs96/i8x9xd.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

class RomBuffer : public util::disasm_interface::data_buffer {
public:
	explicit RomBuffer(const std::vector<uint8_t> &r) : rom(r) {}
	u8 r8(offs_t pc) const override { return pc < rom.size() ? rom[pc] : 0xff; }
	u16 r16(offs_t pc) const override { return u16(r8(pc)) | (u16(r8(pc + 1)) << 8); }
	u32 r32(offs_t pc) const override { return u32(r16(pc)) | (u32(r16(pc + 2)) << 16); }
	u64 r64(offs_t pc) const override { return u64(r32(pc)) | (u64(r32(pc + 4)) << 32); }

private:
	const std::vector<uint8_t> &rom;
};

} // namespace

int main(int argc, char **argv) {
	const char *path = (argc > 1)
		? argv[1]
		: "C:\\Program Files\\Common Files\\VST3\\D-110 Data\\d-110.v1.10.ic19.bin";
	const offs_t from = (argc > 2) ? offs_t(strtoul(argv[2], nullptr, 16)) : 0x29d0;
	const offs_t to = (argc > 3) ? offs_t(strtoul(argv[3], nullptr, 16)) : 0x2a10;

	std::ifstream in(path, std::ios::binary);
	if (!in) {
		std::printf("cannot open %s\n", path);
		return 1;
	}
	std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)),
	                         std::istreambuf_iterator<char>());
	std::printf("%s  (%d bytes)\n\n", path, int(rom.size()));

	RomBuffer buf(rom);
	i8x9x_disassembler dasm;

	for (offs_t pc = from; pc < to;) {
		std::ostringstream text;
		const offs_t result = dasm.disassemble(text, pc, buf, buf);
		const int len = int(result & util::disasm_interface::LENGTHMASK);

		std::string bytes;
		for (int i = 0; i < len && i < 8; ++i) {
			char b[4];
			std::snprintf(b, sizeof(b), "%02X ", buf.r8(pc + i));
			bytes += b;
		}
		std::printf("%04X:  %-24s %s\n", pc, bytes.c_str(), text.str().c_str());
		pc += (len > 0) ? len : 1;
	}
	return 0;
}
