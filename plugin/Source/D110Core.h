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
#include <chrono>
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
	//
	// Returns false, having done nothing, if another machine is already running anywhere
	// in this process. ONLY ONE IS POSSIBLE, and the reason is structural rather than
	// something this code can arrange around: MAME reaches its machine through
	// mame_machine_manager::instance(), which caches a process-wide
	// `static mame_machine_manager *s_manager` and hands the same one to every later
	// caller, so a second cli_frontend attaches to the first machine's OSD and options.
	// Measured, not assumed - plugin/two_instance_test.cpp starts a second core while the
	// first is up and the process dies of heap corruption within seconds. Refusing is
	// therefore the only safe answer until MAME's singleton is dealt with.
	bool start(const std::string &romPath, const std::string &nvramDir);
	void stop();
	bool isRunning() const { return running.load(std::memory_order_acquire); }
	// Whether some other instance in this process already holds the machine.
	static bool machineHeldElsewhere() { return sMachineLive.load(std::memory_order_acquire); }

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
	// Sends every mirrored region on the next snapshot, whether or not it changed. Used
	// wherever the engine's idea of the state cannot be trusted to match the firmware's:
	// once the machine has finished booting (the engine came up on its own ROM defaults
	// while the firmware came up on the user's saved memory) and after a factory reset
	// (the firmware has just rebuilt that memory from scratch). The firmware is the
	// master here, so it is always the engine that gets brought into line.
	void resyncMirror() { mirrorResync.store(true, std::memory_order_release); }
	// How many mirror messages have been produced since boot - diagnostics, so a
	// "nothing changed" result can be told apart from "the bridge never fired".
	uint64_t sysexEmitted() const { return sysexCount.load(std::memory_order_acquire); }

	// --- host MIDI into the firmware ------------------------------------------
	// The firmware has to SEE the notes, not just the sound engine. Its MIDI IN is the
	// CPU's serial port, and the driver already drives it that way for its own test note.
	// Without this the control board never learns a key was pressed, so the top LCD row
	// never lights the playing parts and the display drifts away from the host's own
	// program changes - the panel would be showing a machine that is not being played.
	//
	// Queues bytes to be shifted in at MIDI's own rate. Safe to call from the audio
	// thread; the machine thread consumes them.
	void pushMidi(const uint8_t *bytes, int len);

	// --- the missing external interrupt ---------------------------------------
	// When a note reaches it, the firmware enables the CPU's external interrupt and then
	// spins at 0x29E9 waiting for a flag in its own RAM to have bit 7 set:
	//
	//     29E6  orb   int_mask, #80        enable EXTINT
	//     29E9  ldbze 54, f440[52]         read the flag
	//     29EE  jbc   54, 7, 29e9          keep waiting while bit 7 is clear
	//
	// Only an interrupt handler can set that flag, and NOTHING in MAME's roland_d10
	// drives EXTINT - the one interrupt the driver wires up is HSI0 from the key scanner,
	// which the D-110 configuration then removes outright. So the wait never ends and the
	// firmware never returns to scanning the front panel.
	//
	// This supplies the missing edge. The rate is settable because the hardware's real
	// source is unknown - it is somewhere on the sound board, which is exactly the part
	// MAME does not emulate. 0 disables it, restoring the old behaviour.
	// Driving EXTINT turned out to be the wrong lever: in MAME the line also feeds what
	// port 2 reads back (see i8x9x_device::port2_r), so raising it corrupts a port the
	// firmware relies on, and the panel dies even at 62 Hz with no notes at all. Kept
	// because the measurement is worth preserving, but it defaults to off.
	static constexpr int kExtIntTimerHz = 32000;
	// Rising edges arrive at kExtIntTimerHz / (2 * divider). 0 means never.
	void setExtIntDivider(int divider) { extIntDiv.store(divider, std::memory_order_release); }
	int extIntDivider() const { return extIntDiv.load(std::memory_order_acquire); }
	uint64_t extIntEdges() const { return extIntCount.load(std::memory_order_acquire); }

	// --- releasing the stuck wait ---------------------------------------------
	// The loop at 0x29E9 polls a byte in battery RAM (0xF440 goes through fixed_r into the
	// "rams" share at offset 0x3440) and spins until bit 7 of it is set. On the hardware
	// the sound board's interrupt sets that flag; in MAME nothing ever does.
	//
	// So set it - but ONLY while the firmware is demonstrably sitting in that loop, which
	// is checked by looking at the program counter. Outside those two addresses this
	// touches nothing at all, so it cannot disturb a machine that is running normally.
	static constexpr uint16_t kStuckLoopPc = 0x29E9;
	static constexpr uint16_t kStuckLoopPcAlt = 0x29EE;
	// f440[] is one array of per-voice flags; f460[] immediately after it is a DIFFERENT
	// array, walked by the voice-chain loop at 0x268A. Writing across both corrupts the
	// chain and merely moves the hang, which is what a 64-byte span did. 32 bytes covers
	// f440[] alone.
	static constexpr uint16_t kVoiceFlagBase = 0x3440;
	static constexpr int kVoiceFlagSpan = 32;
	// How to answer the firmware when it is caught waiting.
	//   Off       - do nothing; the wait never ends and the panel dies.
	//   PokeRam   - set the flag byte directly. Releases that loop, but bypasses the
	//               interrupt handler, so everything else the handler would have updated
	//               stays stale and the hang simply moves to the voice-chain walk.
	//   PulseExtInt - raise the CPU's external interrupt, which is what the sound board
	//               does on the hardware, and let the firmware's OWN handler run. Fires
	//               only while the firmware is demonstrably in the wait loop: driving that
	//               line continuously kills the panel by itself, because on this CPU the
	//               EXTINT pin is also port 2 bit 2.
	//   La32Stub  - the real answer. Supplies the bookkeeping half of the sound board:
	//               when the firmware is waiting on a voice, raise the interrupt AND hand
	//               its handler the status byte it reads from 0x0C00, encoding the very
	//               voice it is waiting for. The handler then does its own work, which is
	//               what the two cruder policies skipped. See docs/la32_interface.md.
	enum class StuckPolicy { Off, PokeRam, PulseExtInt, La32Stub };
	void setStuckPolicy(StuckPolicy p) { stuckPolicy.store(int(p), std::memory_order_release); }
	StuckPolicy stuckPolicy_() const {
		return StuckPolicy(stuckPolicy.load(std::memory_order_acquire));
	}
	uint64_t stuckReleases() const { return stuckCount.load(std::memory_order_acquire); }
	// IOC1 as it stood the last time the firmware was caught waiting. Bit 1 of it decides
	// whether the CPU accepts the external interrupt at all: i8x9x_device sets the pending
	// flag only `if(!extint && state && !BIT(ioc1, 1))`. If that bit is set, no amount of
	// driving the line can ever be noticed, and EXTINT is a dead end.
	int stuckIoc1Value() const { return stuckIoc1.load(std::memory_order_acquire); }
	void osdRecordIoc1(int v) { stuckIoc1.store(v, std::memory_order_release); }
	// How many times the stub has handed the firmware's handler a status byte.
	uint64_t la32Services() const { return la32Count.load(std::memory_order_acquire); }
	void osdCountLa32Service() { la32Count.fetch_add(1, std::memory_order_relaxed); }
	// Every read of the status register that reached the tap, whether or not there was
	// anything to hand over. Distinguishes "the tap is not installed" from "the firmware
	// never got as far as reading".
	uint64_t la32Reads() const { return la32ReadCount.load(std::memory_order_acquire); }
	void osdCountLa32Read() { la32ReadCount.fetch_add(1, std::memory_order_relaxed); }
	// The CPU register holding the voice index the wait loop is indexing with: `ldbze 54,
	// f440[52]` uses word register 52, and the register file share starts at 0x18.
	static constexpr int kWaitIndexReg = 0x52;
	static constexpr int kRegFileBase = 0x18;

	// How to encode the voice number in the status byte. The handler treats bit 7 as an
	// event class and derives the index differently on each path - with bit 7 clear it
	// doubles the low bits and subtracts two, with bit 7 set and bit 5 clear it doubles
	// them and does not - so the meaning of a given byte is not obvious from the code
	// alone. Rather than guess, the small space is enumerated and measured.
	//   0  (v+1) & 0x1F        bit 7 clear, index comes out as v
	//   1  v & 0x1F            bit 7 clear, index comes out as v-1
	//   2  0x80 | (v & 0x1F)   bit 7 set, bit 5 clear, index v
	//   3  0x80 | ((v+1)&0x1F) bit 7 set, bit 5 clear, index v+1
	void setLa32StatusMode(int mode) { la32Mode.store(mode, std::memory_order_release); }
	int la32StatusMode() const { return la32Mode.load(std::memory_order_acquire); }
	static uint8_t encodeLa32Status(int mode, uint16_t voice);

	// --- diagnostics: what does the firmware talk to that is not there? --------
	// The D-110's address map leaves nearly all of the low I/O page unmapped - only the
	// bank register, the SO register, the two panel scan ports and the LCD are claimed.
	// The sound board lives in the gaps. Turning on unmapped-access logging and capturing
	// MAME's own log lines shows exactly which addresses the firmware writes and reads
	// when it plays a note, which IS the LA32's register interface as the firmware sees it.
	void setLogUnmapped(bool on) { logUnmapped.store(on, std::memory_order_release); }
	bool logUnmappedEnabled() const { return logUnmapped.load(std::memory_order_acquire); }
	void osdLogLine(const char *line);
	// Takes everything captured so far and empties the buffer.
	std::vector<std::string> takeLogLines();

	// --- diagnostics: where is the CPU actually spending its time? -------------
	// Samples the program counter densely while the machine runs. When the firmware
	// stops servicing the front panel the PC collapses onto a handful of addresses, and
	// those addresses say exactly which loop it is stuck in - which is the only way to
	// find out what it is waiting for from the LA32 that MAME does not emulate.
	void setPcSampling(bool on) { pcSampling.store(on, std::memory_order_release); }
	void resetPcHistogram();
	// Fills `out` with the `count` most-sampled addresses, most frequent first.
	struct PcHit { uint16_t pc; uint64_t hits; };
	std::vector<PcHit> topPcs(int count) const;
	uint64_t pcSampleTotal() const { return pcTotal.load(std::memory_order_acquire); }
	// Samples that landed anywhere in [lo, hi]. Used to answer a plain question: is the
	// interrupt handler being entered at all?
	uint64_t pcHitsInRange(uint16_t lo, uint16_t hi) const;
	void osdSamplePc(uint16_t pc);
	void osdCountExtInt() { extIntCount.fetch_add(1, std::memory_order_relaxed); }
	void osdCountStuckRelease() { stuckCount.fetch_add(1, std::memory_order_relaxed); }

	// --- called by the internal OSD only --------------------------------------
	void osdSnapshotLcd(const uint8_t *renderBuf);
	void osdSnapshotRam(const uint8_t *ram);
	// Takes the next byte for the CPU's serial receiver, or returns false if none is
	// waiting. Exactly one byte per call by design: the emulated UART has no FIFO, so a
	// second write before the firmware's interrupt handler has read the first simply
	// overwrites it. The caller is a MAME timer running at the MIDI byte rate, which is
	// what puts real emulated time between one byte and the next.
	bool popMidiByte(uint8_t &out);
	// MIDI's own byte rate: 31250 baud, one start bit and one stop bit around each byte.
	static constexpr double kMidiBytesPerSecond = 31250.0 / 10.0;
	bool shouldStop() const { return stopFlag.load(std::memory_order_acquire); }
	// Diagnostics: bytes accepted, bytes actually shifted into the CPU, and bytes dropped
	// because the queue was full.
	uint64_t midiForwarded() const { return midiInCount.load(std::memory_order_acquire); }
	uint64_t midiDelivered() const { return midiOutCount.load(std::memory_order_acquire); }
	uint64_t midiDropped() const { return midiDropCount.load(std::memory_order_acquire); }

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
	// Process-wide: set while ANY instance holds the machine. See start().
	static std::atomic<bool> sMachineLive;
	bool holdsMachine = false;
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
	std::atomic<bool> mirrorResync{false};
	// The firmware scribbles all over its RAM while it boots, so the one-shot resync that
	// brings the engine into line waits for it to settle rather than firing on the first
	// snapshot. Measured from the moment the machine is started.
	static constexpr int kBootSettleMs = 4000;
	std::chrono::steady_clock::time_point startedAt;
	bool bootResyncPending = false;

	// SysEx ring: MAME thread producer, audio thread consumer. Fixed-size slots so
	// the audio thread never allocates or blocks.
	static constexpr int kSysexSlots = 64;
	static constexpr int kSysexMask = kSysexSlots - 1;
	std::vector<uint8_t> sysexBuf;      // kSysexSlots * kMaxSysexBytes
	std::vector<uint16_t> sysexLen;
	std::atomic<int> sW{0}, sR{0};
	std::atomic<uint64_t> sysexCount{0};

	// MIDI going the other way - host to firmware. A plain byte ring, because MIDI is a
	// byte stream and a message may legitimately be split across calls. Sized to swallow a
	// whole imported SysEx bank in one go: it drains at MIDI's real 3125 bytes a second,
	// so a large bank sits here for several seconds and must not be truncated meanwhile.
	static constexpr int kMidiSlots = 65536;
	static constexpr int kMidiMask = kMidiSlots - 1;
	std::vector<uint8_t> midiBuf;
	std::atomic<int> mW{0}, mR{0};
	std::atomic<uint64_t> midiInCount{0}, midiOutCount{0}, midiDropCount{0};

	std::atomic<int> extIntDiv{0};
	std::atomic<uint64_t> extIntCount{0};
	std::atomic<int> stuckPolicy{0};
	std::atomic<uint64_t> stuckCount{0};
	std::atomic<int> stuckIoc1{-1};
	std::atomic<uint64_t> la32Count{0}, la32ReadCount{0};
	std::atomic<int> la32Mode{0};

	std::atomic<bool> logUnmapped{false};
	mutable std::mutex logMutex;
	std::vector<std::string> logLines;
	static constexpr size_t kMaxLogLines = 200000;

	// PC histogram: written only by the machine thread, read under the mutex.
	std::atomic<bool> pcSampling{false};
	mutable std::mutex pcMutex;
	std::vector<uint64_t> pcHist;
	std::atomic<uint64_t> pcTotal{0};
};
