/* Copyright (C) 2013, 2014 Sergey V. Mikayev
 *
 * Licensed under the GNU Lesser General Public License, version 2.1 or later.
 *
 * Microcode-level emulation of the BOSS HG61H20R36F / BOS-007 reverb gate
 * array. The implementation executes the external program ROM used by the
 * hardware instead of approximating it with a hand-tuned algorithm.
 */

#include "BossEmu.h"

#include <cstring>

namespace MT32Emu {

static const int MIN_ROM_SIZE = 0x4000;
static const int MAX_ROM_SIZE = 0x8000;
static const int RAM_SIZE = 0x4000;
static const int PROCESSING_LOOP_CYCLES = 0x100;
static const int MAX_RIGHT_DATA_IN_CYCLE = 0x80;
static const int RIGHT_DATA_OUT_CYCLE = 0xBC;
static const int LEFT_DATA_OUT_CYCLE = 0xE0;
static const int DATA_BIT = 0x01;
static const int WRITE_BIT = 0x02;
static const int SHIFTER_BIT = 0x04;
static const int INVERSION_BIT = 0x08;
static const int ACCUMULATOR_OUT_BIT = 0x10;
static const int ACTIVITY_THRESHOLD = 8;

BossEmu::BossEmu(const unsigned char useROM[], const int length) :
	rom(NULL), romSize(0), extendedModesEnabled(false), ram(NULL), romBaseIx(0), sawBits(0),
	currentPosition(0), accumulator(0), shifter(0), carry(0)
{
	if (useROM == NULL || (length != MIN_ROM_SIZE && length != MAX_ROM_SIZE)) return;
	extendedModesEnabled = length == MAX_ROM_SIZE;
	romSize = static_cast<std::size_t>(length);
	rom = useROM;
	ram = new short[RAM_SIZE];
	reset();
}

BossEmu::~BossEmu() {
	delete[] ram;
}

void BossEmu::reset() {
	if (ram != NULL) std::memset(ram, 0, sizeof(short) * RAM_SIZE);
	currentPosition = 0;
	accumulator = 0;
	shifter = 0;
	carry = 0;
	romBaseIx = 0;
	sawBits = 0;
}

void BossEmu::setParameters(int mode, int time, int level) {
	mode = extendedModesEnabled ? mode & 7 : mode & 3;
	const std::size_t romBase = static_cast<std::size_t>((mode << 12) | ((level & 7) << 9) | ((time & 4) << 6));
	if (romBase >= romSize) {
		romBaseIx = 0;
		sawBits = 0;
		return;
	}
	romBaseIx = static_cast<int>(romBase);
	sawBits = 1 << (time & 3);
}

bool BossEmu::isActive() const {
	if (ram == NULL) return false;
	for (int i = 0; i < RAM_SIZE; ++i) {
		if (ram[i] < -ACTIVITY_THRESHOLD || ACTIVITY_THRESHOLD < ram[i]) return true;
	}
	return false;
}

void BossEmu::process(const short *inLeft, const short *inRight, short *outLeft, short *outRight, int length) {
	if (ram == NULL) {
		if (outLeft != NULL) std::memset(outLeft, 0, sizeof(short) * length);
		if (outRight != NULL) std::memset(outRight, 0, sizeof(short) * length);
		return;
	}

	while (length-- > 0) {
		const short inputLeft = inLeft != NULL ? static_cast<short>(*inLeft >> 1) : 0;
		const short inputRight = inRight != NULL ? static_cast<short>(*inRight >> 1) : 0;
		for (int cycleIx = 0; cycleIx < PROCESSING_LOOP_CYCLES; cycleIx += 4) {
			if (cycleIx == RIGHT_DATA_OUT_CYCLE && outRight != NULL) *outRight = accumulator;
			else if (cycleIx == LEFT_DATA_OUT_CYCLE && outLeft != NULL) *outLeft = accumulator;
			processingCycle(cycleIx, cycleIx < MAX_RIGHT_DATA_IN_CYCLE ? inputRight : inputLeft);
		}
		if (inLeft != NULL) ++inLeft;
		if (inRight != NULL) ++inRight;
		if (outLeft != NULL) ++outLeft;
		if (outRight != NULL) ++outRight;
		currentPosition = (currentPosition + 1) & (RAM_SIZE - 1);
	}
}

void BossEmu::processingCycle(const int cycleIx, const short inputData) {
	const int romCycleIx = romBaseIx + cycleIx;
	const int ramIx = (currentPosition + (rom[romCycleIx] | (rom[romCycleIx + 2] << 8))) & (RAM_SIZE - 1);
	unsigned char controlBits = rom[romCycleIx + 1];
	short newShifterValue = static_cast<short>(shifter >> 1);
	short newCarry = shifter < 0 ? static_cast<short>(shifter & 1) : 0;

	if ((controlBits & WRITE_BIT) == 0) {
		short value = accumulator;
		if ((controlBits & DATA_BIT) != 0) value = inputData;
		ram[ramIx] = value;
		if ((controlBits & SHIFTER_BIT) == 0) {
			newShifterValue = value;
			carry = newCarry = 0;
		}
	} else if ((controlBits & SHIFTER_BIT) == 0) {
		newShifterValue = ram[ramIx];
		carry = newCarry = 0;
	}

	int shifterMask = ((controlBits >> 4) & 0x0E) | (rom[romCycleIx + 2] >> 7);
	updateAccumulator(controlBits, shifterMask);
	carry = newCarry;
	shifter = newShifterValue;

	controlBits = rom[romCycleIx + 3];
	shifterMask = ((controlBits >> 4) & 0x0E) | ((rom[romCycleIx + 2] >> 6) & 1);
	updateAccumulator(controlBits, shifterMask);
	carry = shifter < 0 ? static_cast<short>(shifter & 1) : 0;
	shifter = static_cast<short>(shifter >> 1);
}

void BossEmu::updateAccumulator(const unsigned char controlBits, const int shifterMask) {
	if ((controlBits & ACCUMULATOR_OUT_BIT) == 0) accumulator = 0;
	if ((shifterMask & sawBits) != 0) {
		const int sum = static_cast<int>(accumulator) + static_cast<int>(shifter) + carry;
		accumulator = static_cast<short>(sum < -0x8000 ? -0x8000 : (0x7FFF < sum ? 0x7FFF : sum));
	}
	if ((controlBits & INVERSION_BIT) != 0) accumulator = static_cast<short>(~accumulator);
}

} // namespace MT32Emu
