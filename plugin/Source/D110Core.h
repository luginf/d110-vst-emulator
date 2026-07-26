// D110Core: runs MAME's real D-110 machine - the actual Roland firmware, its menu
// system, its MSM6222B LCD and its 16 front-panel buttons - inside the plugin, on its
// own thread, with a headless OSD. It produces NO audio: MAME has no LA32 emulation
// for any Roland LA machine, so the sound continues to come from mt32emu. This half
// supplies everything mt32emu lacks, and mt32emu supplies the one thing this half
// lacks. See docs/sysex_address_map.md for how the two are joined.
//
// Architecture mirrors the proven MameWrap in the MU-100R and MU-128 plugins (same
// cli_frontend + custom osd_common_t technique), minus the audio ring and plus a
// window onto the firmware's battery-backed RAM, which is what carries panel edits
// across to mt32emu.
//
// Nothing here needs a patched MAME: the LCD's render(), the "rams" memory share and
// the SC0/SC1 ioports are all reachable through public interfaces.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class D110Core {
public:
	D110Core();
	~D110Core();

	// Boots the machine. `romPath` is a MAME rompath (the d110 romset); `nvramDir` is
	// where the firmware's battery RAM and memory card persist between runs.
	void start(const std::string &romPath, const std::string &nvramDir);
	void stop();
	bool isRunning() const { return running.load(std::memory_order_acquire); }

	// --- LCD -----------------------------------------------------------------
	// The real MSM6222B's rendered dot matrix, straight out of the chip: 2 lines of
	// 16 characters, 8 dot rows per character, and within a row byte bit 4 is the
	// LEFTMOST dot (msm6222b.h's own convention). The glyphs therefore come from the
	// genuine mask CGROM that MAME loads, and the cursor and blink are the
	// controller's own - there is no font table on this path at all.
	static constexpr int kCols = 16;
	static constexpr int kLines = 2;
	static constexpr int kRowsPerChar = 8;
	static constexpr int kLcdBytes = kLines * kCols * kRowsPerChar;
	// Fills `out` (kLcdBytes) with out[(line * kCols + col) * kRowsPerChar + row].
	// Returns false if the machine has not produced a frame yet.
	bool getLcd(uint8_t *out) const;

	// --- front panel ---------------------------------------------------------
	// Two 8-bit scan ports exactly as INPUT_PORTS_START(d110) declares them:
	// port 0 = "SC0" (top row), port 1 = "SC1" (bottom row). Bit masks per button
	// are in docs/panel_reference_notes.md.
	static constexpr int kNumPorts = 2;
	static constexpr int kNumBits = 8;
	static constexpr int kNumButtons = kNumPorts * kNumBits;
	static constexpr int buttonIndex(int port, int bit) { return port * kNumBits + bit; }

	// Buttons are held as a DESIRED STATE, not as a queue of press/release events, and
	// the machine re-applies that state every frame. This is what makes a stuck button
	// impossible: there is no release event that can be dropped or arrive out of order,
	// and re-asserting a state that is already correct costs nothing. (A queue was tried
	// first and could strand a switch closed whenever its release was lost.)
	void setButton(int index, bool down);
	// Opens every switch at once - used when the panel loses track, e.g. on power off.
	void releaseAllButtons() { wantButtons.store(0, std::memory_order_release); }
	uint32_t buttonMask() const { return wantButtons.load(std::memory_order_acquire); }

	// The factory reset the driver's own header documents: hold Write/Copy across a
	// reset, then confirm with Enter. That is what makes the firmware rebuild its patch
	// and timbre memory from the preset ROM - a fresh NVRAM is all zeroes and shows an
	// empty patch until this is done, and it is also the way back from a memory that
	// has been edited into a mess. Restarts the machine, so it returns immediately and
	// finishes on its own; `isRunning()` goes false and true again as it does.
	void factoryReset();
	// Set while a factory reset is in progress, so the UI can say so.
	bool isResetting() const { return resetting.load(std::memory_order_acquire); }

	// --- the firmware's battery-backed RAM ------------------------------------
	// 32 KB at 0x40000 in the D-110's bank map, shared as "rams". This is the whole
	// patch/timbre memory plus the working areas, and its layout is Roland's own
	// SysEx address map with a constant offset per region - RAM 0x2000 is SysEx
	// 0x030000, confirmed both against the manual and by measurement. Mirroring it
	// into mt32emu is what makes a panel edit audible.
	static constexpr int kRamSize = 0x8000;
	// Copies the current RAM into `out` (kRamSize bytes). Returns false before the
	// first snapshot exists.
	bool getRam(uint8_t *out) const;
	// Monotonic counter bumped whenever the snapshot changed, so a caller can skip
	// work when the firmware has been idle.
	uint64_t ramGeneration() const { return ramGen.load(std::memory_order_acquire); }

	// --- the bridge to the LA engine -----------------------------------------
	// Whenever a mirrored region of that RAM changes, the core turns it into a Roland
	// DT1 exclusive message addressed exactly as the hardware would address it, and
	// queues it here. The audio thread drains the queue into mt32emu, which speaks
	// this map natively - so selecting a timbre or retuning a part on the panel
	// reaches the sound engine without anything having to understand the parameters
	// one by one. See docs/sysex_address_map.md.
	static constexpr int kMaxSysexBytes = 256;
	// Copies one pending message into `out` (at least kMaxSysexBytes) and returns its
	// length, or 0 if the queue is empty. Safe to call from the audio thread.
	int popSysex(uint8_t *out);
	// How many mirror messages have been produced since boot - diagnostics, so a
	// "nothing changed" result can be told apart from "the bridge never fired".
	uint64_t sysexEmitted() const { return sysexCount.load(std::memory_order_acquire); }

	// --- called by the internal OSD only --------------------------------------
	void osdSnapshotLcd(const uint8_t *renderBuf);
	void osdSnapshotRam(const uint8_t *ram);
	bool shouldStop() const { return stopFlag.load(std::memory_order_acquire); }

	// One block of the firmware's RAM that is mirrored into the LA engine, and where
	// Roland's own exclusive map says it lives.
	struct MirrorRegion {
		uint16_t ramOffset;
		uint32_t sysexAddress; // 21-bit, seven bits per transmitted byte
		uint16_t length;
		const char *name;
	};
	static const MirrorRegion kMirrorRegions[];
	static const int kNumMirrorRegions;

private:
	void threadFunc();

	std::thread mameThread;
	std::atomic<bool> running{false};
	std::atomic<bool> stopFlag{false};
	std::string romPath, nvramDir;

	// One bit per button, written by the GUI thread and read by the machine thread.
	std::atomic<uint32_t> wantButtons{0};
	std::atomic<bool> resetting{false};
	std::thread resetThread;

	mutable std::mutex lcdMutex;
	std::vector<uint8_t> lcd;
	std::atomic<bool> lcdValid{false};

	mutable std::mutex ramMutex;
	std::vector<uint8_t> ram;
	std::atomic<bool> ramValid{false};
	std::atomic<uint64_t> ramGen{0};

	void emitRegionSysex(const MirrorRegion &region, const uint8_t *ramImage);
	void pushSysex(const uint8_t *msg, int len);

	// Previous contents of each mirrored region, so only real changes are sent.
	std::vector<std::vector<uint8_t>> mirrorPrev;
	bool mirrorPrimed = false;

	// SysEx ring: MAME thread producer, audio thread consumer. Fixed-size slots so
	// the audio thread never allocates or blocks.
	static constexpr int kSysexSlots = 64;
	static constexpr int kSysexMask = kSysexSlots - 1;
	std::vector<uint8_t> sysexBuf;      // kSysexSlots * kMaxSysexBytes
	std::vector<uint16_t> sysexLen;
	std::atomic<int> sW{0}, sR{0};
	std::atomic<uint64_t> sysexCount{0};
};
