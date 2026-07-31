#define __STDC_FORMAT_MACROS 1
#define __STDC_CONSTANT_MACROS 1

#include "D110Core.h"

// --- MAME core ---
#include "emu.h"
#include "osd/modules/lib/osdobj_common.h"
#include "osdepend.h"
#include "emuopts.h"
#include "render.h"
#include "ioport.h"
#include "video/msm6222b.h"
#include "cpu/mcs96/i8x9x.h"
#include "frontend/mame/ui/menuitem.h"
#include "frontend/mame/mame.h"
#include "frontend/mame/clifront.h"
#include "frontend/mame/mameopts.h"
#include "drivenum.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>

// MAME binaries embed a Common-Controls v6 manifest via their .rc; without it comctl32
// (imported by ordinal) binds to v5 and LoadLibrary fails with ERROR_INVALID_ORDINAL.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- version + emulator_info symbols the linker needs (the app must supply these) ----
// The d110 subset was built out of the MAME 0.288 tree at MU128-VST/mame-master.
extern const char bare_build_version[] = "0.288";
extern const char bare_vcs_revision[] = "0.288";
extern const char build_version[] = "0.288";

const char *emulator_info::get_appname() { return "mame"; }
const char *emulator_info::get_appname_lower() { return "mame"; }
const char *emulator_info::get_configname() { return "mame"; }
const char *emulator_info::get_copyright() { return "Copyright"; }
const char *emulator_info::get_copyright_info() { return "Copyright"; }
const char *emulator_info::get_bare_build_version() { return bare_build_version; }
const char *emulator_info::get_build_version() { return build_version; }
bool emulator_info::standalone() { return false; }
void emulator_info::periodic_check() {}
bool emulator_info::frame_hook() { return false; }
void emulator_info::sound_hook(const std::map<std::string, std::vector<std::pair<const float *, int>>> &) {}
void emulator_info::layout_script_cb(layout_file &, const char *) {}
bool emulator_info::draw_user_interface(running_machine &) { return false; }
void emulator_info::display_ui_chooser(running_machine &) {}

namespace {

// In two-line mode the MSM6222B's render buffer puts line 1 at character 0 and line 2
// at character 40, 16 bytes per character (msm6222b.h).
constexpr int kRenderLine0 = 0;
constexpr int kRenderLine1 = 40;
constexpr int kRenderStride = 16;

// Headless OSD: injects button presses, and lifts the LCD and the battery RAM out of
// the running machine once per frame.
class D110Osd : public osd_common_t {
	D110Core *core;
	running_machine *m_machine = nullptr;
	render_target *m_target = nullptr;
	msm6222b_device *m_lcd = nullptr;
	i8x9x_device *m_cpu = nullptr;
	emu_timer *m_midiTimer = nullptr;
	emu_timer *m_extIntTimer = nullptr;
	int m_extIntPhase = 0;
	bool m_extIntLevel = false;
	bool m_stuckIntHigh = false;
	int m_stuckIntHighTicks = 0; // watchdog: ticks since ASSERT with no la32NeedClear yet
	// Per-voice-context note state, machine thread only - see noteWatch().
	uint8_t m_ctxNote[D110Core::kNumVoiceContexts] = {};
	uint8_t m_ctxVelocity[D110Core::kNumVoiceContexts] = {};
	uint8_t m_ctxPart[D110Core::kNumVoiceContexts] = {};
	bool m_ctxSounding[D110Core::kNumVoiceContexts] = {};
	bool m_ctxNoteFresh[D110Core::kNumVoiceContexts] = {};
	int m_la32WaitTicks = 0; // how long the firmware has been in the current LA32 wait
	std::chrono::steady_clock::time_point m_ctxOnTime[D110Core::kNumVoiceContexts];
	uint8_t *m_regFile = nullptr;
	memory_passthrough_handler m_la32Tap;
	memory_passthrough_handler m_voiceCtxTap;
	memory_passthrough_handler m_dispatchTap;
	memory_passthrough_handler m_soTap;
	bool m_la32Pending = false;   // a status byte is waiting to be collected
	bool m_la32NeedClear = false; // the handler has taken it; drop the line
	uint8_t m_la32Status = 0xff;
	uint8_t *m_ram = nullptr;
	std::array<ioport_field *, D110Core::kNumButtons> m_buttonField{};
	bool m_resolved = false;
	std::vector<uint8_t> m_lcdScratch;

	// What is currently asserted in the machine, and how many more frames each button
	// must stay closed before it may open again. The minimum hold exists because the
	// panel is polled once per frame: a click shorter than a frame would otherwise be
	// applied and released between the same two polls and never reach the firmware.
	uint32_t m_appliedButtons = 0;
	std::array<int, D110Core::kNumButtons> m_holdFrames{};
	static constexpr int kMinHoldFrames = 3;

	// Resolve the panel ioports and the two devices we read. Retried every update()
	// until they exist: locking in a failed lookup on the first call would silently
	// leave the panel dead for the whole session.
	// Turns the firmware's own writes into note events. Runs on the CPU thread, inside the
	// write tap, so it sees them at exact emulated time rather than a frame later.
	//
	// The note-start routine (ROM 0x278B/0x2790/0x2795) writes note, velocity and part for
	// one context back to back, so the part write is what completes the triple. Release is
	// f460[ctx] bit 6, set by the reclaim path the note-off search at 0x2496 ends in - see
	// the long comment on D110Core::NoteEvent.
	void noteWatch(uint16_t ramsOffset, uint8_t value) {
		if (ramsOffset >= D110Core::kNoteTable &&
		    ramsOffset < D110Core::kNoteTable + D110Core::kNumVoiceContexts) {
			const int ctx = ramsOffset - D110Core::kNoteTable;
			// Bit 7 set means "released but sustained" - a re-marking of a note already
			// sounding, not a new one, so it must not restart it.
			if ((value & 0x80) == 0) {
				// A context can be handed straight to a new note without its previous
				// occupant ever being released - the firmware steals voices, and the
				// reclaim path that sets f460 bit 6 is not on that route. Releasing the
				// old note here is what keeps the two sides balanced; without it a stolen
				// note was left sounding and only stopped when mt32emu happened to steal
				// the voice for itself (measured: 1185 note-ons against 707 note-offs).
				if (m_ctxSounding[ctx] && m_ctxNote[ctx] != value)
					releaseContext(ctx);
				m_ctxNote[ctx] = value;
				m_ctxNoteFresh[ctx] = true; // this context is mid-triple
			}
			return;
		}
		if (ramsOffset >= D110Core::kVelocityTable &&
		    ramsOffset < D110Core::kVelocityTable + D110Core::kNumVoiceContexts) {
			m_ctxVelocity[ramsOffset - D110Core::kVelocityTable] = value;
			return;
		}
		if (ramsOffset >= D110Core::kPartTable &&
		    ramsOffset < D110Core::kPartTable + D110Core::kNumVoiceContexts) {
			const int ctx = ramsOffset - D110Core::kPartTable;
			const uint8_t part = uint8_t(value >> 4); // stored as part * 16
			if (part > 8) return;                     // 0-7 voice parts, 8 rhythm
			m_ctxPart[ctx] = part;
			// Third write of the triple completes a note - but ONLY if this run actually
			// began with a note write. The part byte is also written on its own in other
			// situations, and firing on those emitted note 0 out of a stale context, which
			// mt32emu rejected as "Attempted to play invalid key 0".
			if (m_ctxNoteFresh[ctx] && m_ctxNote[ctx] > 0 && m_ctxNote[ctx] <= 127) {
				core->osdPushNoteEvent({part, m_ctxNote[ctx], m_ctxVelocity[ctx], true});
				m_ctxSounding[ctx] = true;
				m_ctxOnTime[ctx] = std::chrono::steady_clock::now();
			}
			m_ctxNoteFresh[ctx] = false;
			return;
		}
		if (ramsOffset >= D110Core::kReleaseTable &&
		    ramsOffset < D110Core::kReleaseTable + D110Core::kNumVoiceContexts) {
			const int ctx = ramsOffset - D110Core::kReleaseTable;
			if (value & D110Core::kReleasedBit) releaseContext(ctx);
		}
	}

	// Ends whatever this context was sounding, if anything. Two routes reach it: the
	// firmware marking the voice reclaimed (f460 bit 6), and the context being handed
	// straight to a new note without that ever happening, which is how voice stealing
	// looks from here.
	void releaseContext(int ctx) {
		if (!m_ctxSounding[ctx]) return;
		core->osdPushNoteEvent({m_ctxPart[ctx], m_ctxNote[ctx], 0, false});
		m_ctxSounding[ctx] = false;
		core->osdAddNoteDuration(std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - m_ctxOnTime[ctx]).count());
	}

	void resolveDevices() {
		if (m_resolved || !m_machine) return;
		device_t &root = m_machine->root_device();
		if (!root.ioport("SC0")) return;

		static const char *kTags[D110Core::kNumPorts] = {"SC0", "SC1"};
		for (int p = 0; p < D110Core::kNumPorts; ++p) {
			ioport_port *port = root.ioport(kTags[p]);
			if (!port) continue;
			for (int b = 0; b < D110Core::kNumBits; ++b) {
				const ioport_value mask = 1u << b;
				for (ioport_field &f : port->fields())
					if (f.mask() == mask) {
						m_buttonField[D110Core::buttonIndex(p, b)] = &f;
						break;
					}
			}
		}

		m_lcd = root.subdevice<msm6222b_device>("lcd");
		if (memory_share *share = root.memshare("rams"))
			if (share->bytes() >= D110Core::kRamSize)
				m_ram = static_cast<uint8_t *>(share->ptr());

		// The CPU's own serial receiver is the D-110's MIDI IN. The driver drives it the
		// same way for its built-in test note, so this needs nothing MAME does not expose.
		m_cpu = root.subdevice<i8x9x_device>("maincpu");

		// The CPU's own register file, so the voice index the wait loop is using can be
		// read straight out of register 52. internal_regs maps 0x18-0xff as this share -
		// and it belongs to the CPU, not to the root device, so it has to be looked up
		// THROUGH the CPU. Asking root for it silently returns nothing, which is what left
		// the stub's branch never running at all.
		if (m_cpu)
			if (memory_share *regs = m_cpu->memshare("register_file"))
				m_regFile = static_cast<uint8_t *>(regs->ptr());

		// The sound board's status register. A read tap rather than a handler because it
		// can be installed on a running machine and sits on top of whatever the address
		// map does or does not provide - and unlike a plain observer, a tap may replace
		// the value that the read returns, which is the whole point here.
		if (m_cpu) {
			m_la32Tap = m_cpu->space(AS_PROGRAM).install_read_tap(
				0x0c00, 0x0c01, "d110_la32_status",
				[this](offs_t, u16 &data, u16 mem_mask) {
					// Counted unconditionally, so "the tap never fired" can be told apart
					// from "the tap fired but had nothing to give".
					core->osdCountLa32Read();
					if (!m_la32Pending) return;
					if (mem_mask & 0x00ff)
						data = u16((data & 0xff00) | m_la32Status);
					else if (mem_mask & 0xff00)
						data = u16((data & 0x00ff) | (u16(m_la32Status) << 8));
					else
						return;
					m_la32Pending = false;
					m_la32NeedClear = true; // drop the interrupt line on the next tick
					core->osdCountLa32Service();
				});
		}

		// Diagnostic write tap over the voice-context bookkeeping arrays (f3c0..f480) - see
		// D110Core.h's kVoiceCtxTapBase. Unlike the LA32 status tap this range IS claimed by
		// the address map (it is real "rams"), so a plain write tap that never touches the
		// data just observes; it exists to answer "does the firmware's own housekeeping ever
		// reach f460[] at all" instead of guessing further from a static disassembly.
		if (m_cpu) {
			m_voiceCtxTap = m_cpu->space(AS_PROGRAM).install_write_tap(
				D110Core::kVoiceCtxTapBase,
				D110Core::kVoiceCtxTapBase + D110Core::kVoiceCtxTapSpan - 1,
				"d110_voice_ctx",
				[this](offs_t addr, u16 &data, u16 mem_mask) {
					// Normalised to the rams array offset (addr - 0xC000) so events line up
					// with kSlotStateTable/kSlotContextTable and everything else that reads
					// the same array through D110Core::getRam()/m_ram, rather than every
					// caller having to know about the CPU's own address window.
					const uint16_t pc = uint16_t(m_cpu->pc());
					const uint16_t ramsOffset = uint16_t(addr - 0xc000);
					if (mem_mask & 0x00ff) {
						core->osdLogCtxEvent(pc, ramsOffset, uint8_t(data & 0xff));
						noteWatch(ramsOffset, uint8_t(data & 0xff));
					}
					if (mem_mask & 0xff00) {
						core->osdLogCtxEvent(pc, uint16_t(ramsOffset + 1), uint8_t((data >> 8) & 0xff));
						noteWatch(uint16_t(ramsOffset + 1), uint8_t((data >> 8) & 0xff));
					}
				});
			m_dispatchTap = m_cpu->space(AS_PROGRAM).install_write_tap(
				D110Core::kDispatchTapBase,
				D110Core::kDispatchTapBase + D110Core::kDispatchTapSpan - 1,
				"d110_dispatch",
				[this](offs_t addr, u16 &data, u16 mem_mask) {
					const uint16_t pc = uint16_t(m_cpu->pc());
					const uint16_t ramsOffset = uint16_t(addr - 0xc000);
					if (mem_mask & 0x00ff)
						core->osdLogCtxEvent(pc, ramsOffset, uint8_t(data & 0xff));
					if (mem_mask & 0xff00)
						core->osdLogCtxEvent(pc, uint16_t(ramsOffset + 1), uint8_t((data >> 8) & 0xff));
				});
		}

		// The MIDI MESSAGE lamp, straight off the register the hardware drives it from.
		// The driver's own so_w() documents the layout ("bit 0 = led") but discards the
		// value, so a tap is how the panel gets to see it without patching MAME.
		if (m_cpu) {
			m_soTap = m_cpu->space(AS_PROGRAM).install_write_tap(
				D110Core::kSoRegister, D110Core::kSoRegister, "d110_so_led",
				[this](offs_t, u16 &data, u16 mem_mask) {
					if (!(mem_mask & 0x00ff)) return;
					core->osdSetMidiLamp((data & 0x01) != 0);
					core->osdMarkMidiLampSeen();
				});
		}

		// Ask MAME to report every access to an address nothing claims, and capture those
		// reports. The sound board is exactly what is missing from this machine's map, so
		// its register interface shows up here and nowhere else.
		if (m_cpu && core->logUnmappedEnabled()) {
			m_cpu->space(AS_PROGRAM).set_log_unmap(true);
			m_machine->add_logerror_callback(
				[core = this->core](const char *line) { core->osdLogLine(line); });
		}

		m_resolved = true;
	}

public:
	D110Osd(D110Core *c, osd_options &options) : osd_common_t(options), core(c) {}

	virtual void init(running_machine &machine) override {
		osd_common_t::init(machine);
		m_machine = &machine;
		// A render target has to exist even headless, or the machine has nothing to
		// draw into and video update asserts.
		m_target = machine.render().target_alloc();
		m_target->set_bounds(320, 240);
		m_lcdScratch.resize(D110Core::kLcdBytes, 0);

		// The MIDI IN timer has to be allocated HERE and nowhere later: MAME closes save
		// state registration once the machine has finished starting, and a timer created
		// after that is a fatal error ("Attempt to register save state entry after state
		// registration is closed"). The CPU is not resolvable this early, which is fine -
		// the callback simply does nothing until it is.
		const attotime period = attotime::from_hz(D110Core::kMidiBytesPerSecond);
		m_midiTimer = machine.scheduler().timer_alloc(
			timer_expired_delegate(FUNC(D110Osd::midiTick), this));
		m_midiTimer->adjust(period, 0, period);

		// The external interrupt nothing else provides - see D110Core::setExtIntDivider.
		// Allocated here for the same reason as the MIDI timer: after the machine has
		// started, MAME refuses to create timers at all.
		const attotime extPeriod = attotime::from_hz(D110Core::kExtIntTimerHz);
		m_extIntTimer = machine.scheduler().timer_alloc(
			timer_expired_delegate(FUNC(D110Osd::extIntTick), this));
		m_extIntTimer->adjust(extPeriod, 0, extPeriod);
	}

	virtual void osd_exit() override {
		if (m_machine && m_target) {
			m_machine->render().target_free(m_target);
			m_target = nullptr;
		}
		osd_common_t::osd_exit();
	}

	virtual void update(bool) override {
		if (m_machine && core->shouldStop()) {
			m_machine->schedule_exit();
			return;
		}
		if (!m_machine) return;

		resolveDevices();

		// Re-assert the panel's desired state rather than replaying events. Applying a
		// state that is already correct is free, and a switch can never be stranded
		// closed by a lost or reordered release.
		const uint32_t want = core->buttonMask();
		for (int i = 0; i < D110Core::kNumButtons; ++i) {
			if (!m_buttonField[i]) continue;
			const uint32_t bit = 1u << i;
			const bool wantDown = (want & bit) != 0;
			const bool isDown = (m_appliedButtons & bit) != 0;

			if (wantDown && !isDown) {
				// The scan ports are IP_ACTIVE_LOW; set_value/clear_value handle polarity.
				m_buttonField[i]->set_value(m_buttonField[i]->mask());
				m_appliedButtons |= bit;
				m_holdFrames[i] = kMinHoldFrames;
			} else if (!wantDown && isDown) {
				if (m_holdFrames[i] > 0) { --m_holdFrames[i]; continue; }
				m_buttonField[i]->clear_value();
				m_appliedButtons &= ~bit;
			} else if (wantDown) {
				if (m_holdFrames[i] > 0) --m_holdFrames[i];
			}
		}

		if (m_lcd) {
			const uint8_t *rendered = m_lcd->render();
			if (rendered) {
				for (int line = 0; line < D110Core::kLines; ++line) {
					const int first = (line == 0) ? kRenderLine0 : kRenderLine1;
					for (int col = 0; col < D110Core::kCols; ++col) {
						const uint8_t *src = rendered + (size_t)(first + col) * kRenderStride;
						uint8_t *dst = m_lcdScratch.data()
						             + ((size_t)line * D110Core::kCols + col) * D110Core::kRowsPerChar;
						for (int row = 0; row < D110Core::kRowsPerChar; ++row)
							dst[row] = src[row] & 0x1f;
					}
				}
				core->osdSnapshotLcd(m_lcdScratch.data());
			}
		}

		if (m_ram) core->osdSnapshotRam(m_ram);
	}

	// Host MIDI arriving at the emulated MIDI IN, one byte per firing. It has to be a
	// machine timer rather than something driven from update(): the emulated UART is a
	// single register with no FIFO, so two writes without emulated time between them
	// leave only the second byte, and update() runs once a frame - far too coarse and
	// far too bursty. On a timer the bytes arrive spaced exactly as they would down a
	// real MIDI cable, which is also what the driver's own built-in test note does.
	void midiTick(s32) {
		uint8_t byte = 0;
		if (m_cpu && core->popMidiByte(byte))
			m_cpu->serial_w(byte);

		if (!m_cpu) return;
		const uint16_t pc = uint16_t(m_cpu->pc());

		// Piggy-back the PC sample on this timer: it already fires 3125 times a second,
		// which is dense enough to see which loop the firmware is sitting in.
		core->osdSamplePc(pc);

		// One-off diagnostic: ground truth for the actual EXTINT line state (via the
		// public device_execute_interface::input_line_state, since i8x9x's own port2_r()
		// is protected) right around the handler's own pin check, instead of reasoning
		// about MAME's i8x9x source further.
		if (pc >= 0x3138 && pc <= 0x3140)
			core->osdLogPort2Sample(
				pc, uint8_t(m_cpu->input_line_state(i8x9x_device::EXTINT_LINE)),
				m_stuckIntHigh, m_la32Pending);

		// Answer the firmware when it is caught waiting for the sound board. Everything
		// here is conditional on the program counter, so a machine running normally is
		// never touched.
		const bool stuck =
			(pc == D110Core::kStuckLoopPc || pc == D110Core::kStuckLoopPcAlt);

		// Once the handler has collected the status byte, drop the line so it can rise
		// again for the next voice.
		//
		// 2026-07-31: chased what looked like a clear-too-early race (see the superseded
		// comment in git history) before finding the REAL cause with a live EXTINT-state
		// tap (`d110_demo_song_repro`, `Port2Sample`): `m_stuckIntHigh` read false on
		// ~all of 22,951 samples taken while the handler was bailing at 0x313D thousands
		// of times a second - i.e. WE were not holding the line, yet the CPU kept
		// re-entering anyway. The cause is in MAME's own MCS-96 core, not this stub:
		// `mcs96ops.lst`'s interrupt dispatch clears `pending_irq` for the level just
		// taken with `if (level != 7) pending_irq &= ~(1<<level)` - EXTINT is level 7,
		// the ONE level explicitly excluded. Once the rising edge sets that bit
		// (`i8x9x_device::execute_set_input`, `pending_irq |= IRQ_EXTINT`), nothing in
		// the core ever clears it again - not on the falling edge, not on take - so the
		// CPU re-takes the same stale interrupt continuously regardless of what we do
		// with the external line. This, not any timing race, is what produced tens of
		// thousands of handler entries for a handful of real services.
		//
		// Fixed by clearing it ourselves: `MCS96_INT_PENDING` is a public state slot
		// (same debug-state API IOC1 is already read through), so no MAME patch is
		// needed - mask off bit 0x80 (IRQ_EXTINT, private in i8x9x.h, hence the literal)
		// in the same place the line itself is dropped.
		if (m_stuckIntHigh && (m_la32NeedClear || !stuck)) {
			m_cpu->set_input_line(i8x9x_device::EXTINT_LINE, CLEAR_LINE);
			const uint64_t pending = m_cpu->state_int(mcs96_device::MCS96_INT_PENDING);
			m_cpu->set_state_int(mcs96_device::MCS96_INT_PENDING, pending & ~uint64_t(0x80));
			m_stuckIntHigh = false;
			m_la32NeedClear = false;
			m_stuckIntHighTicks = 0;
		}

		// Leaving the wait resets the response-delay counter, so the next voice waits its
		// own full delay rather than inheriting this one's elapsed time.
		if (!stuck) { m_la32WaitTicks = 0; return; }

		// Record whether the CPU is even willing to accept the external interrupt.
		core->osdRecordIoc1(int(m_cpu->state_int(i8x9x_device::I8X9X_IOC1)));

		switch (core->stuckPolicy_()) {
		case D110Core::StuckPolicy::PokeRam:
			if (m_ram) {
				for (int i = 0; i < D110Core::kVoiceFlagSpan; ++i)
					m_ram[D110Core::kVoiceFlagBase + i] |= 0x80;
				core->osdCountStuckRelease();
			}
			break;
		case D110Core::StuckPolicy::PulseExtInt:
			if (!m_stuckIntHigh) {
				m_cpu->set_input_line(i8x9x_device::EXTINT_LINE, ASSERT_LINE);
				m_stuckIntHigh = true;
				m_stuckIntHighTicks = 0;
				core->osdCountStuckRelease();
			} else {
				// Give it a falling edge between pulses even while still stuck.
				m_cpu->set_input_line(i8x9x_device::EXTINT_LINE, CLEAR_LINE);
				m_stuckIntHigh = false;
			}
			break;
		case D110Core::StuckPolicy::La32Stub:
			// Report the hardware voice SLOT that dispatch assigned to the SPECIFIC
			// context the CPU is parked waiting for right now, and let the firmware's own
			// handler do the rest. Answering any busy slot at random was not enough once
			// more than one note is pending (any chord): the handler ran and consumed real
			// status bytes, but if it wasn't the slot backing the context in r52, the loop
			// the CPU is actually sitting in never got its flag set and the panel stayed
			// dead - measured 2026-07-31. Dispatch wrote r52's low byte into ee01[slot]
			// (rams 0x2E01+2*slot, ROM 0x361A), so the reverse lookup is exact: find n
			// with ee01[n] == r52 and edc0[n] still busy.
			// Make the firmware wait as the real chip would, if asked to. Counted in
			// midiTick periods; 0 answers as soon as the wait is noticed.
			if (++m_la32WaitTicks <= core->la32ResponseDelay()) break;

			if (m_ram && m_regFile && !m_la32Pending && !m_la32NeedClear && !m_stuckIntHigh) {
				const int reg = D110Core::kWaitIndexReg - D110Core::kRegFileBase;
				const uint8_t context = m_regFile[reg];
				int slot = -1;
				for (int n = 0; n < D110Core::kNumHardwareVoices; ++n) {
					const uint8_t busy = m_ram[D110Core::kSlotStateTable + 2 * n];
					if ((busy == D110Core::kSlotBusyValue || busy == D110Core::kSlotBusyValueAlt) &&
					    m_ram[D110Core::kSlotContextTable + 2 * n] == context) {
						slot = n;
						break;
					}
				}
				if (slot >= 0) {
					m_la32Status = D110Core::encodeLa32Status(core->la32StatusMode(), uint16_t(slot));
					m_la32Pending = true;
					// The handler refuses to do anything unless the pin is still high when
					// it runs, so raise it and hold it until the status has been collected.
					m_cpu->set_input_line(i8x9x_device::EXTINT_LINE, ASSERT_LINE);
					m_stuckIntHigh = true;
					m_stuckIntHighTicks = 0;
				} else {
					core->osdRecordUnresolvedContext(context);
				}
			}
			break;
		case D110Core::StuckPolicy::Off:
			break;
		}
	}

	// Drives the external interrupt the D-110's sound board would raise and MAME does not.
	// The line is edge triggered - execute_set_input only latches when it goes from low to
	// high - so it is toggled rather than pulsed within one call, which guarantees a clean
	// edge no matter how the scheduler batches things.
	void extIntTick(s32) {
		const int divider = core->extIntDivider();
		if (!m_cpu || divider <= 0) return;
		if (++m_extIntPhase < divider) return;
		m_extIntPhase = 0;

		m_extIntLevel = !m_extIntLevel;
		m_cpu->set_input_line(i8x9x_device::EXTINT_LINE, m_extIntLevel ? ASSERT_LINE : CLEAR_LINE);
		if (m_extIntLevel) core->osdCountExtInt();
	}

	// ---- input / debugger: nothing to do headless ----
	virtual void process_events() override {}
	virtual bool has_focus() const override { return true; }
	virtual void input_update(bool) override {}
	virtual void check_osd_inputs() override {}
	virtual void init_debugger() override {}
	virtual void wait_for_debugger(device_t &, bool) override {}

	// ---- sound: the D-110 driver produces none, and we do not want any ----
	virtual bool no_sound() override { return true; }
	virtual bool sound_external_per_channel_volume() override { return false; }
	virtual bool sound_split_streams_per_source() override { return false; }
	virtual osd::audio_info sound_get_information() override {
		osd::audio_info info;
		osd::audio_info::node_info node;
		node.m_name = "null";
		node.m_display_name = "No Audio";
		node.m_id = 1;
		node.m_rate = {44100u};
		node.m_sinks = 1;
		node.m_sources = 0;
		info.m_nodes.push_back(node);
		info.m_default_sink = 1;
		info.m_generation = 1;
		return info;
	}
	virtual uint32_t sound_stream_sink_open(uint32_t, std::string, uint32_t) override { return 1; }
	virtual void sound_stream_close(uint32_t) override {}
	virtual void add_audio_to_recording(const int16_t *, int) override {}
	virtual uint32_t sound_stream_source_open(uint32_t, std::string, uint32_t) override { return 0; }
	virtual uint32_t sound_get_generation() override { return 1; }
	virtual void sound_stream_source_update(uint32_t, int16_t *, int) override {}
	virtual void sound_stream_set_volumes(uint32_t, const std::vector<float> &) override {}
	virtual void sound_begin_update() override {}
	virtual void sound_end_update() override {}
	virtual void sound_stream_sink_update(uint32_t, const int16_t *, int) override {}

	// ---- UI neutered ----
	virtual void customize_input_type_list(std::vector<input_type_entry> &t) override { t.clear(); }
	virtual std::vector<ui::menu_item> get_slider_list() override { return {}; }
	virtual osd_font::ptr font_alloc() override { return nullptr; }
	virtual bool get_font_families(std::string const &,
	                               std::vector<std::pair<std::string, std::string>> &) override { return false; }
	virtual bool execute_command(const char *) override { return false; }
	virtual void set_verbose(bool) override {}

	// ---- MIDI: the D-110 driver mounts no MIDI image slots, so nothing asks for a
	// port. Feeding host MIDI into the firmware (so its display tracks Program
	// Changes from the DAW) needs a small driver patch and comes later.
	virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view) override { return {}; }
	virtual std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view) override { return {}; }
	virtual std::vector<osd::midi_port_info> list_midi_ports() override { return {}; }

	virtual std::unique_ptr<osd::network_device> open_network_device(int, osd::network_handler &) override { return {}; }
	virtual std::vector<osd::network_device_info> list_network_devices() override { return {}; }
};

} // namespace

// ---------------------------------------------------------------------------

// The blocks of the firmware's RAM that are mirrored into the LA engine, with the
// Roland exclusive address each one lives at. Both bases below were measured, not
// assumed - `plugin/bridge_probe.cpp` walks the firmware's menus and diffs the RAM,
// and the per-part block agreed on three consecutive bytes in the order Roland
// documents (timbreNumber, keyShift, fineTune). See docs/sysex_address_map.md.
//
// Mirroring a whole region rather than individual parameters is deliberate: it means
// nothing here has to know what any given byte means, so every parameter inside the
// block comes across for free.
//
// The Tone Temporary Area is split into one region PER PART rather than one big block,
// for two reasons: a Roland DT1 is limited to 256 bytes and a tone is 246, and sending
// only the part that actually changed keeps the engine from re-caching all eight tones
// on every edit.
//
// Its base was MEASURED, and by comparison rather than inference - `plugin/tone_probe.cpp`
// reads back the tone mt32emu holds for each part and slides those 246 bytes over the
// whole 32 KB of firmware RAM. All eight parts match 246 bytes out of 246, at exactly
// base + part * 246 from 0x21E4:
//
//     part 1 0x21E4 'AcouBass 1'   part 5 0x25BC 'Trombone 1'
//     part 2 0x22DA 'AcouPiano2'   part 6 0x26B2 'Sax 1'
//     part 3 0x23D0 'Guitar 1'     part 7 0x27A8 'Sax 3'
//     part 4 0x24C6 'Trumpet 2'    part 8 0x289E 'Strings 3'
//
// An exact match is also the null test: both halves load the same tone out of the same
// ROM, so mirroring an unedited one demonstrably changes nothing. That is what the
// earlier span-difference method could never establish - it only bounded the base from
// BELOW, because the tail bytes of two tones need not differ. (The "+460 cents" that
// once seemed to condemn 0x21E4 was not the base at all: it was the test harness's
// zero-crossing pitch meter latching onto a different harmonic. The base was right.)
#define D110_TONE(part) \
	{ uint16_t(0x21E4 + (part) * 246), \
	  0x040000u | (uint32_t(((part) * 246) / 128) << 8) | uint32_t(((part) * 246) % 128), \
	  246, "Tone Temporary" }

// The Rhythm Setup Area: 85 entries of 4 bytes (timbre, output level, panpot, reverb
// switch) mapping each drum key to a tone. RAM 0x2090 - immediately after the 144 bytes of
// Timbre Temporary - and 0x2090 + 340 lands exactly on the measured Tone Temporary base
// 0x21E4, which is what makes the base certain rather than assumed. SysEx 0x030110, per
// munt's own MemoryRegion table.
//
// It has to be split because emitRegionSysex builds ONE DT1 into a 256-byte buffer and
// silently truncates anything longer - 340 bytes would lose its tail. 128 is the natural
// chunk: adding 128 to a Roland address is exactly +1 on the middle seven-bit byte and
// leaves the low one alone, so the arithmetic below stays trivially correct.
#define D110_RHYTHM(chunk, len) \
	{ uint16_t(0x2090 + (chunk) * 128), \
	  0x030110u + (uint32_t(chunk) << 8), \
	  (len), "Rhythm Setup" }

const D110Core::MirrorRegion D110Core::kMirrorRegions[] = {
	// 9 parts (8 voice + rhythm) of 16 bytes; SysEx 0x030000, part 2 at 0x030010,
	// rhythm at 0x030100. 144 bytes fits one DT1, so it goes as a single block.
	{ 0x2000, 0x030000, 9 * 16, "Timbre Temporary" },

	// Which drum sits on which key. Without this the engine keeps the MT-32's own default
	// rhythm map, and the D-110's demo songs - which drive the rhythm part hard - log
	// "Attempted to play unmapped key 25" and drop those hits silently.
	D110_RHYTHM(0, 128), D110_RHYTHM(1, 128), D110_RHYTHM(2, 84),

	// The eight tones the parts are actually playing. This is what carries Edit's deep
	// pages - the partials, the waveforms, the pitch/TVF/TVA envelopes - which until now
	// moved the display and nothing else. The generated SysEx addresses reproduce the
	// manual's own part 7 and part 8 entries, 0x040B44 and 0x040D3A.
	D110_TONE(0), D110_TONE(1), D110_TONE(2), D110_TONE(3),
	D110_TONE(4), D110_TONE(5), D110_TONE(6), D110_TONE(7),

	// The System Area, but deliberately only the middle of it. The base is certain -
	// tone_probe's audit reads RAM 0x2D94 as Roland's structure and two fields confirm it
	// beyond coincidence: reserveSettings sums to exactly 32, and chanAssign reads
	// 1 2 3 4 5 6 7 8 9, which is the D-110's documented "Part 1 answers on MIDI channel
	// 2" assignment. Those 18 bytes are mirrored, so the SYSTEM page's partial reserve and
	// channel map become real rather than decorative.
	//
	// The bytes on either side are NOT mirrored, and both would do real damage:
	//   masterVol (offset 22) reads 0, because on a D-110 the volume is a physical knob
	//     and the firmware never fills that byte in. Sending it set the engine's master
	//     volume to zero and dropped the whole instrument by ~30 dB - which is exactly
	//     how this was found.
	//   reverbMode (offset 1) reads 4, outside the 0-3 this engine accepts; the D-110's
	//     reverb settings do not line up with the MT-32's.
	//   masterTune (offset 0) is left out too: the panel reads 442 where Roland's
	//     documented 0-127 -> 432.1-457.6 Hz mapping makes 0x4A about 447, so the two
	//     scales disagree and mirroring it would detune everything against the display.
	{ 0x2D98, 0x100004, 18, "System (reserve + channels)" },
};
#undef D110_TONE
constexpr int D110Core::kNumMirrorRegions =
	int(sizeof(D110Core::kMirrorRegions) / sizeof(D110Core::kMirrorRegions[0]));

// One DT1 carries kMaxSysexBytes minus an 8-byte header and a 2-byte tail. The largest
// thing mirrored here is a 246-byte tone, which fits with EXACTLY no slack.
static constexpr int kMaxRegionBytes = D110Core::kMaxSysexBytes - 10;
static_assert(kMaxRegionBytes >= 246, "a Tone Temporary region no longer fits in one DT1");

D110Core::D110Core()
	: lcd(kLcdBytes, 0), ram(kRamSize, 0),
	  sysexBuf((size_t)kSysexSlots * kMaxSysexBytes, 0), sysexLen(kSysexSlots, 0),
	  midiBuf(kMidiSlots, 0), noteBuf(kNoteSlots) {
	mirrorPrev.resize(kNumMirrorRegions);
	for (int i = 0; i < kNumMirrorRegions; ++i) {
		// A region longer than one DT1 would be truncated by emitRegionSysex and still
		// sent with a valid checksum, so the engine would silently accept a half-written
		// parameter block. Catch it here rather than let it become a mystery.
		assert(kMirrorRegions[i].length <= kMaxRegionBytes);
		mirrorPrev[(size_t)i].assign(kMirrorRegions[i].length, 0);
	}
}

D110Core::~D110Core() {
	stop();
	if (resetThread.joinable()) resetThread.join();
}

std::atomic<bool> D110Core::sMachineLive{false};

bool D110Core::start(const std::string &rom, const std::string &nvram) {
	if (running.load(std::memory_order_acquire)) return true;

	// Claim the process's one machine slot, or refuse. Doing this before the thread is
	// spawned is what makes it a guarantee rather than a race.
	bool expected = false;
	if (!sMachineLive.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return false;
	holdsMachine = true;

	romPath = rom;
	nvramDir = nvram;
	stopFlag.store(false, std::memory_order_release);

	// A fresh machine means a fresh baseline. Without this the previous run's contents
	// survive in mirrorPrev, so the first snapshot after a restart diffs against a state
	// that no longer exists - the "prime silently, then send only changes" rule held only
	// for the very first boot in the process. Instead: prime silently here, then resync
	// everything once the firmware has settled, below in osdSnapshotRam.
	mirrorPrimed = false;
	startedAt = std::chrono::steady_clock::now();
	bootResyncPending = true;

	mameThread = std::thread(&D110Core::threadFunc, this);
	return true;
}

void D110Core::stop() {
	// Open every switch before the machine goes away, so nothing can be left asserted
	// if the panel is still showing a cap as held.
	releaseAllButtons();
	stopFlag.store(true, std::memory_order_release);
	if (mameThread.joinable()) mameThread.join();
	running.store(false, std::memory_order_release);

	// Release the process's machine slot only once the thread is truly gone - MAME's
	// singleton is not free until its destructor has run.
	if (holdsMachine) {
		holdsMachine = false;
		sMachineLive.store(false, std::memory_order_release);
	}
}

void D110Core::threadFunc() {
	running.store(true, std::memory_order_release);

	// No -nothrottle: with no audio ring to apply back-pressure, MAME's own throttle
	// is what keeps the firmware running at real speed, which is what makes key
	// repeat and display timing feel like the hardware.
	std::vector<std::string> args = {
		"d110", "d110",
		"-rompath", romPath,
		"-nvram_directory", nvramDir,
		"-video", "none",
		"-sound", "none",
		"-noreadconfig", "-skip_gameinfo",
		"-keyboardprovider", "none",
		"-mouseprovider", "none",
		"-joystickprovider", "none",
	};

	auto *opts = new osd_options();
	auto *osd = new D110Osd(this, *opts);
	auto *fe = new cli_frontend(*opts, *osd);
	fe->execute(args);
	delete fe;
	delete osd;
	delete opts;

	running.store(false, std::memory_order_release);
}

// The documented cold start. Write/Copy is held from the moment the machine comes up
// rather than pressed afterwards, which the state-based button model makes easy: the
// switch is simply asserted before the machine starts, and the OSD applies it as soon
// as the ioports resolve - exactly like holding the real button while switching on.
void D110Core::factoryReset() {
	if (resetting.exchange(true, std::memory_order_acq_rel)) return; // already running
	if (resetThread.joinable()) resetThread.join();

	resetThread = std::thread([this] {
		const std::string rom = romPath, nv = nvramDir;
		if (isRunning()) stop();

		releaseAllButtons();
		setButton(buttonIndex(0, 0), true); // Write/Copy, held across the reset
		start(rom, nv);

		std::this_thread::sleep_for(std::chrono::seconds(5)); // let the firmware come up
		setButton(buttonIndex(0, 0), false);
		std::this_thread::sleep_for(std::chrono::milliseconds(800));

		setButton(buttonIndex(1, 0), true); // Enter, to confirm
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		setButton(buttonIndex(1, 0), false);

		// Give the firmware time to rewrite its patch and timbre memory before anything
		// else touches it.
		std::this_thread::sleep_for(std::chrono::seconds(4));
		// The whole memory has just been rebuilt from the preset ROM, so tell the engine
		// about all of it rather than waiting for a diff to notice piecemeal.
		resyncMirror();
		resetting.store(false, std::memory_order_release);
	});
}

// ---- buttons --------------------------------------------------------------

void D110Core::setButton(int index, bool down) {
	if (index < 0 || index >= kNumButtons) return;
	const uint32_t bit = 1u << index;
	uint32_t cur = wantButtons.load(std::memory_order_relaxed);
	uint32_t next;
	do {
		next = down ? (cur | bit) : (cur & ~bit);
	} while (!wantButtons.compare_exchange_weak(cur, next, std::memory_order_release,
	                                            std::memory_order_relaxed));
}

// ---- LCD ------------------------------------------------------------------

void D110Core::osdSnapshotLcd(const uint8_t *renderBuf) {
	std::lock_guard<std::mutex> lock(lcdMutex);
	std::memcpy(lcd.data(), renderBuf, kLcdBytes);
	lcdValid.store(true, std::memory_order_release);
}

bool D110Core::getLcd(uint8_t *out) const {
	if (!lcdValid.load(std::memory_order_acquire)) return false;
	std::lock_guard<std::mutex> lock(lcdMutex);
	std::memcpy(out, lcd.data(), kLcdBytes);
	return true;
}

// ---- battery RAM ----------------------------------------------------------

void D110Core::osdSnapshotRam(const uint8_t *src) {
	// Once the firmware has stopped scribbling over its RAM on the way up, push the whole
	// mirrored state across once. The engine booted on its own ROM defaults while the
	// firmware booted on the user's saved memory, so until this happens the panel and the
	// sound disagree about which timbres are loaded.
	if (bootResyncPending
	    && std::chrono::steady_clock::now() - startedAt
	           >= std::chrono::milliseconds(kBootSettleMs)) {
		bootResyncPending = false;
		mirrorResync.store(true, std::memory_order_release);
	}
	const bool resync = mirrorResync.exchange(false, std::memory_order_acq_rel);

	// Only the mirrored regions decide whether anything is sent onward. Diffing the
	// whole 32 KB would fire dozens of times a second on the firmware's scratch area,
	// which has nothing to do with the sound.
	for (int i = 0; i < kNumMirrorRegions; ++i) {
		const auto &region = kMirrorRegions[i];
		auto &prev = mirrorPrev[(size_t)i];
		const uint8_t *now = src + region.ramOffset;
		if (!resync && mirrorPrimed && std::memcmp(prev.data(), now, region.length) == 0)
			continue;
		std::memcpy(prev.data(), now, region.length);
		// The very first snapshot after a start only establishes the baseline; the
		// deliberate catch-up is the resync above, which happens once the firmware is
		// no longer mid-boot.
		if (mirrorPrimed || resync)
			emitRegionSysex(region, src);
	}
	mirrorPrimed = true;

	std::lock_guard<std::mutex> lock(ramMutex);
	if (ramValid.load(std::memory_order_relaxed) && std::memcmp(ram.data(), src, kRamSize) == 0)
		return; // unchanged - leave the generation alone so callers can skip work
	std::memcpy(ram.data(), src, kRamSize);
	ramValid.store(true, std::memory_order_release);
	ramGen.fetch_add(1, std::memory_order_release);
}

// Builds the Roland "Data set 1" message the hardware itself would send for this
// block, and queues it. Format per the D-110 owner's manual and accepted verbatim by
// mt32emu's Synth::playSysexWithoutFraming: F0 41 <dev> 16 12 <a1 a2 a3> <data> <sum> F7,
// manufacturer 0x41 Roland, model 0x16, command 0x12 DT1, device ID 0x10 (the default
// unit). The checksum makes address plus data plus checksum a multiple of 128.
void D110Core::emitRegionSysex(const MirrorRegion &region, const uint8_t *ramImage) {
	uint8_t msg[kMaxSysexBytes];
	int n = 0;
	msg[n++] = 0xF0;
	msg[n++] = 0x41; // Roland
	msg[n++] = 0x10; // device ID
	msg[n++] = 0x16; // model: MT-32 family, which is what the D-110 answers to
	msg[n++] = 0x12; // DT1

	// A Roland address is written as three SEPARATE seven-bit bytes - 0x030000 means the
	// bytes 03 00 00 - so it splits on byte boundaries, not on seven-bit ones. Packing it
	// as a 21-bit number instead sends 0x030000 as 0C 00 00, which mt32emu rejects with
	// "Sysex write to unrecognised address 0c0000". That was the whole reason the first
	// version of this bridge fired correctly and still changed nothing.
	const uint32_t addr = region.sysexAddress;
	const uint8_t a1 = uint8_t((addr >> 16) & 0x7f);
	const uint8_t a2 = uint8_t((addr >> 8) & 0x7f);
	const uint8_t a3 = uint8_t(addr & 0x7f);
	msg[n++] = a1;
	msg[n++] = a2;
	msg[n++] = a3;

	uint32_t sum = a1 + a2 + a3;
	const int payload = std::min<int>(region.length, kMaxSysexBytes - n - 2);
	for (int i = 0; i < payload; ++i) {
		// Data bytes are seven bits on the wire; the firmware's own values already fit,
		// but mask defensively so a stray high bit cannot break the framing.
		const uint8_t v = ramImage[region.ramOffset + i] & 0x7f;
		msg[n++] = v;
		sum += v;
	}
	msg[n++] = uint8_t((128 - (sum & 0x7f)) & 0x7f);
	msg[n++] = 0xF7;

	pushSysex(msg, n);
}

void D110Core::pushSysex(const uint8_t *msg, int len) {
	if (len <= 0 || len > kMaxSysexBytes) return;
	const int w = sW.load(std::memory_order_relaxed);
	const int r = sR.load(std::memory_order_acquire);
	if (((w + 1) & kSysexMask) == r) return; // full: drop rather than stall the machine
	std::memcpy(&sysexBuf[(size_t)w * kMaxSysexBytes], msg, (size_t)len);
	sysexLen[(size_t)w] = uint16_t(len);
	sW.store((w + 1) & kSysexMask, std::memory_order_release);
	sysexCount.fetch_add(1, std::memory_order_release);
}

// ---- host MIDI into the firmware ------------------------------------------

void D110Core::pushMidi(const uint8_t *bytes, int len) {
	int w = mW.load(std::memory_order_relaxed);
	const int r = mR.load(std::memory_order_acquire);
	for (int i = 0; i < len; ++i) {
		const int next = (w + 1) & kMidiMask;
		if (next == r) { // full: drop the rest rather than stall the audio thread
			midiDropCount.fetch_add(uint64_t(len - i), std::memory_order_relaxed);
			break;
		}
		midiBuf[(size_t)w] = bytes[i];
		w = next;
		midiInCount.fetch_add(1, std::memory_order_relaxed);
	}
	mW.store(w, std::memory_order_release);
}

uint8_t D110Core::encodeLa32Status(int mode, uint16_t voice) {
	switch (mode) {
	case 1: return uint8_t(voice & 0x1f);
	case 2: return uint8_t(0x80 | (voice & 0x1f));
	case 3: return uint8_t(0x80 | ((voice + 1) & 0x1f));
	default: return uint8_t((voice + 1) & 0x1f);
	}
}

// ---- capturing MAME's own log --------------------------------------------

void D110Core::osdLogLine(const char *line) {
	if (!logUnmapped.load(std::memory_order_acquire) || line == nullptr) return;
	std::lock_guard<std::mutex> lock(logMutex);
	if (logLines.size() >= kMaxLogLines) return; // stop rather than grow without bound
	logLines.emplace_back(line);
}

std::vector<std::string> D110Core::takeLogLines() {
	std::lock_guard<std::mutex> lock(logMutex);
	std::vector<std::string> out;
	out.swap(logLines);
	return out;
}

// ---- voice-context array write tap -----------------------------------------

void D110Core::osdLogCtxEvent(uint16_t pc, uint16_t addr, uint8_t value) {
	if (!voiceCtxTap.load(std::memory_order_acquire)) return;
	std::lock_guard<std::mutex> lock(ctxMutex);
	if (ctxEvents.size() >= kMaxCtxEvents) return;
	ctxEvents.push_back({pc, addr, value});
}

std::vector<D110Core::CtxEvent> D110Core::takeCtxEvents() {
	std::lock_guard<std::mutex> lock(ctxMutex);
	std::vector<CtxEvent> out;
	out.swap(ctxEvents);
	return out;
}

// ---- notes recovered from the firmware --------------------------------------

void D110Core::osdPushNoteEvent(const NoteEvent &ev) {
	if (noteLoggingOn())
		osdLogNote({noteLogElapsedMs(), ev.part, ev.note, ev.velocity, ev.on});
	const int w = nW.load(std::memory_order_relaxed);
	const int next = (w + 1) & kNoteMask;
	// Full means the audio thread has stalled; dropping is the only safe answer, and a
	// dropped note-off would hang a voice far more audibly than a dropped note-on.
	if (next == nR.load(std::memory_order_acquire)) return;
	noteBuf[(size_t)w] = ev;
	nW.store(next, std::memory_order_release);
	if (ev.on) noteOnCount.fetch_add(1, std::memory_order_relaxed);
}

bool D110Core::popNoteEvent(NoteEvent &out) {
	const int r = nR.load(std::memory_order_relaxed);
	if (r == nW.load(std::memory_order_acquire)) return false;
	out = noteBuf[(size_t)r];
	nR.store((r + 1) & kNoteMask, std::memory_order_release);
	return true;
}

void D110Core::osdLogPort2Sample(uint16_t pc, uint8_t port2, bool stuckIntHigh, bool la32Pending) {
	std::lock_guard<std::mutex> lock(port2Mutex);
	if (port2Samples.size() >= kMaxPort2Samples) return;
	port2Samples.push_back({pc, port2, stuckIntHigh, la32Pending});
}

std::vector<D110Core::Port2Sample> D110Core::takePort2Samples() {
	std::lock_guard<std::mutex> lock(port2Mutex);
	std::vector<Port2Sample> out;
	out.swap(port2Samples);
	return out;
}

// ---- PC sampling ----------------------------------------------------------

void D110Core::osdSamplePc(uint16_t pc) {
	if (!pcSampling.load(std::memory_order_acquire)) return;
	std::lock_guard<std::mutex> lock(pcMutex);
	if (pcHist.empty()) pcHist.assign(0x10000, 0);
	++pcHist[pc];
	pcTotal.fetch_add(1, std::memory_order_release);
}

void D110Core::resetPcHistogram() {
	std::lock_guard<std::mutex> lock(pcMutex);
	pcHist.assign(0x10000, 0);
	pcTotal.store(0, std::memory_order_release);
}

uint64_t D110Core::pcHitsInRange(uint16_t lo, uint16_t hi) const {
	std::lock_guard<std::mutex> lock(pcMutex);
	if (pcHist.empty()) return 0;
	uint64_t total = 0;
	for (int i = lo; i <= int(hi) && i < int(pcHist.size()); ++i) total += pcHist[(size_t)i];
	return total;
}

std::vector<D110Core::PcHit> D110Core::topPcs(int count) const {
	std::lock_guard<std::mutex> lock(pcMutex);
	std::vector<PcHit> all;
	for (size_t i = 0; i < pcHist.size(); ++i)
		if (pcHist[i]) all.push_back({uint16_t(i), pcHist[i]});
	std::sort(all.begin(), all.end(),
	          [](const PcHit &a, const PcHit &b) { return a.hits > b.hits; });
	if (int(all.size()) > count) all.resize(count);
	return all;
}

bool D110Core::popMidiByte(uint8_t &out) {
	const int r = mR.load(std::memory_order_relaxed);
	if (r == mW.load(std::memory_order_acquire)) return false;
	out = midiBuf[(size_t)r];
	mR.store((r + 1) & kMidiMask, std::memory_order_release);
	midiOutCount.fetch_add(1, std::memory_order_relaxed);
	return true;
}

int D110Core::popSysex(uint8_t *out) {
	const int r = sR.load(std::memory_order_relaxed);
	if (r == sW.load(std::memory_order_acquire)) return 0;
	const int len = sysexLen[(size_t)r];
	std::memcpy(out, &sysexBuf[(size_t)r * kMaxSysexBytes], (size_t)len);
	sR.store((r + 1) & kSysexMask, std::memory_order_release);
	return len;
}

bool D110Core::getRam(uint8_t *out) const {
	if (!ramValid.load(std::memory_order_acquire)) return false;
	std::lock_guard<std::mutex> lock(ramMutex);
	std::memcpy(out, ram.data(), kRamSize);
	return true;
}
