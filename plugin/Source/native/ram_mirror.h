// The RAM-to-SysEx mirror bridge that feeds mt32emu, ported from D110Core.cpp's
// osdSnapshotRam()/emitRegionSysex()/kMirrorRegions - already pure logic over a plain byte
// buffer with zero MAME dependency (confirmed during this port's own planning), so this is a
// near-verbatim copy, not a reimplementation. Duplicated rather than shared with D110Core.cpp
// on purpose: the plan keeps D110Core.cpp completely untouched until the native core is fully
// proven, and D110Core.cpp is not reachable from a MAME-free target anyway (it pulls in MAME
// headers even though D110Core.h itself doesn't). Once the native core is validated, hoisting
// this into a class both cores share is the natural next cleanup - flagged here so it isn't
// forgotten.
//
// Simplified versus the original in one respect: no ring buffer or atomics. D110Core's queue
// is lock-free because a MAME thread produces and the audio thread consumes; here everything
// runs on one thread (the whole point of this port), so a plain deque is correct and simpler.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

class RamMirror {
public:
	struct Region {
		uint16_t ramOffset;
		uint32_t sysexAddress;
		uint16_t length;
		const char *name;
		bool reassertAfterTimbreTemp = false;
	};
	static const Region kRegions[];
	static const int kNumRegions;
	static constexpr int kMaxSysexBytes = 256;

	RamMirror();

	// Call periodically (any fixed cadence is fine, matching D110Core's own "once per
	// emulated video frame" - see D110CoreNative::runForSeconds()). elapsedSeconds is
	// cumulative emulated time since start(), used only for the boot-settle resync delay.
	void update(const uint8_t *ram, double elapsedSeconds);

	// Forces every region to resend on the next update() - matches D110Core::resyncMirror(),
	// needed after a factory reset or any external RAM rewrite the diff wouldn't otherwise see.
	void forceResync() { resyncPending_ = true; }

	// Drains one queued DT1 SysEx message (F0...F7) into out (must hold kMaxSysexBytes).
	// Returns its length, or 0 if the queue is empty.
	int popSysex(uint8_t *out);

	// Total messages built since construction - D110CoreNative derives sysexEmitted() (the
	// Monitor tab's diagnostic, D110Core::sysexEmitted()'s counterpart) from this.
	uint64_t messagesEmitted() const { return messagesEmitted_; }

private:
	std::vector<std::vector<uint8_t>> prev_;
	bool primed_ = false;
	bool bootResyncPending_ = true;
	bool resyncPending_ = false;
	static constexpr double kBootSettleSeconds = 4.0; // D110Core::kBootSettleMs / 1000
	std::deque<std::vector<uint8_t>> queue_;
	uint64_t messagesEmitted_ = 0;

	void emitRegionSysex(const Region &region, const uint8_t *ramImage);
};
