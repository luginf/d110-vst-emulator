/* Copyright (C) 2013 Sergey V. Mikayev
 *
 * Licensed under the GNU Lesser General Public License, version 2.1 or later.
 */

#ifndef MT32EMU_BOSS_EMU_H
#define MT32EMU_BOSS_EMU_H

#include <cstddef>

namespace MT32Emu {

// Microcode-level emulation of the HG61H20R36F / BOS-007 reverb gate array.
class BossEmu {
public:
	BossEmu(const unsigned char rom[], int length);
	~BossEmu();

	void setParameters(int mode, int time, int level);
	void reset();
	bool isActive() const;
	void process(const short *inLeft, const short *inRight, short *outLeft, short *outRight, int length);

private:
	const unsigned char *rom;
	std::size_t romSize;
	bool extendedModesEnabled;
	short *ram;
	int romBaseIx;
	int sawBits;
	int currentPosition;
	short accumulator;
	short shifter;
	short carry;

	void processingCycle(int cycleIx, short inputData);
	void updateAccumulator(unsigned char controlBits, int shifterMask);
};

} // namespace MT32Emu

#endif
