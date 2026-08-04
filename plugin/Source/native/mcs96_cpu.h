// Hand-ported from the vendored MAME tree's cpu/mcs96/{mcs96,i8x9x}.{h,cpp}
// (C:/Users/bd260/Projects/MU128-VST/mame-master), folding mcs96_device + i8x9x_device
// into one class since the D-110 only ever instantiates the i8x9x/N8097BH variant - MAME's
// two-level split exists to share code across the wider MCS-96 family, which this doesn't
// need. Runs stepped inline on the audio thread instead of on MAME's own real-time thread,
// which is what eliminates the MIDI-to-sound jitter the MAME-backed D110Core has (see
// note_latency_probe.cpp and the docs referenced from D110Core.h).
//
// Opcode bodies (mcs96ops.hxx) are machine-transcribed - not hand-parsed - from the vendored
// tree's own mcs96ops.lst via its own code generator (mcs96make.py, run unmodified against
// the vendored .lst), to avoid transcription error across ~230 opcode variants. Only the
// "_full" bodies are ported: do_exec_partial() is dead code in the real driver too (MAME's
// own scheduler never calls it - see mcs96.cpp execute_run(), the call is commented out).
//
// Two behavioural differences from the vendored MAME core, both deliberate:
//  1. EXTINT's falling edge now clears the pending-interrupt bit (setExtIntLine(false)).
//     The MAME-backed D110Core has to patch this externally, at every falling edge, via
//     state_int()/set_state_int() pokes into MCS96_INT_PENDING, because it cannot touch
//     i8x9x_device's own execute_set_input(). This port owns that function directly, so it
//     fixes the gap at the source instead of replicating the external workaround.
//  2. No bit-level UART/SCI timing: i8x9x's serial_w() is a single-register write with no
//     FIFO or start/stop-bit timing (unlike DX100's HD6303X SCI), so serialWrite() is called
//     once per elapsed tick from a 3125Hz accumulator in the stepping loop, matching what
//     D110Core.cpp's D110Osd::midiTick() already does today.
#pragma once

#include <cstdint>
#include <functional>

class Mcs96Cpu {
public:
	using u8 = uint8_t;
	using u16 = uint16_t;
	using u32 = uint32_t;
	using u64 = uint64_t;

	// Program-space (>=0x100) bus hooks - ROM banks, rams window, LCD/panel/SO ports, all
	// owned by whatever wires this CPU up (D110Bus in this project). Register-file/SFR space
	// (<0x100) is handled internally, exactly as any_r8/any_w8 route it in the original.
	std::function<u8(u16 addr)> busRead8;
	std::function<void(u16 addr, u8 data)> busWrite8;

	// EXTINT is the only interrupt line the D-110 driver ever pulses from outside (HSI/timer/
	// serial/AD interrupts are all raised internally by this class). Modelled as a method,
	// not a bus access, since on real hardware it's a dedicated interrupt-controller pin.
	void setExtIntLine(bool asserted);

	// Feeds one byte into SBUF as if it had just arrived over the MIDI cable - see class
	// comment, point 2. Mirrors i8x9x_device::serial_w().
	void serialWrite(u8 val);
	bool serialRxReady() const { return !(sp_stat & 0x40); }
	// True once a byte written via serialWrite() has actually been picked up by the CPU's
	// own serial-read code (SBUF read clears the ready flag) - lets a caller pace bytes in
	// without ever clobbering one still waiting to be read.

	void reset();
	// Runs up to `cycles` oscillator periods (the same unit total_cycles()/icount use in the
	// original - state times x cycles_scaling, cycles_scaling=3 for the i8x9x). Returns the
	// number actually consumed, which can slightly exceed `cycles` since an in-progress
	// instruction always finishes before the loop rechecks the budget - the caller's own
	// accumulator absorbs the overshoot, exactly as DX100's stepOneSample() does.
	int run(int cycles);

	u16 pc() const { return PC; }
	u64 totalCycles() const { return total_cycles_; }

	// Fired once per real instruction, right after its opcode byte(s) have been fetched and
	// decoded (i.e. once per PPC-worth of program flow, not once per do_exec_full() dispatch
	// - fetch/fetch_noirq are dispatches too but aren't instructions themselves). Used by
	// native_boot_probe.cpp to build a PC trace for comparison against the MAME-backed core;
	// nullptr costs nothing extra in run()'s hot loop beyond one bool check.
	std::function<void(u16 pc)> onFetch;

	// Full 256-byte register-file/SFR address space, exposed for diagnostics that read fixed
	// register-file offsets directly (D110Core's kWaitIndexReg tap does this against the real
	// core's "register_file" share). Slots 0x00-0x17 are SFRs with dedicated read/write
	// dispatch below and are never touched through this array directly; 0x18-0xff is the
	// genuine general-purpose register file, addressed the same way real MCS-96 code does.
	u8 regFile[0x100] = {};

	// --- SFR/peripheral state (ported from i8x9x_device's private members) ---
	struct HsoCamEntry { u8 command = 0; u16 time = 0; u64 fire_at = 0; };
	HsoCamEntry hso_info[8];
	HsoCamEntry hso_cam_hold;
	u64 base_timer2 = 0, ad_done = 0;
	u8 hsi_mode = 0, hsi_status = 0, hso_command = 0, ad_command = 0, hso_active = 0;
	u16 hso_time = 0, ad_result = 0;
	u8 pwm_control = 0;
	u8 port1 = 0, port2 = 0;
	u8 ios0 = 0, ios1 = 0, ioc0 = 0, ioc1 = 0;
	bool extint = false;
	u8 sbuf = 0, sp_con = 0, sp_stat = 0;
	u8 serial_send_buf = 0;
	u64 serial_send_timer = 0;
	u16 baud_reg = 0;
	bool brh = false;

	// Port 1/2 output callbacks - the D-110 driver never wires anything to port1_cb, and
	// port2_cb only ever logs, so these are optional and unused for now; kept as hooks in
	// case a later phase needs them (e.g. diagnostics).
	std::function<void(u8)> outP1Cb;
	std::function<void(u8)> outP2Cb;
	std::function<u8()> inP0Cb;   // D-110 wires this to its own port0 (battery-ok + sample-clock toggle)
	std::function<u8()> inP1Cb;
	std::function<u8()> inP2Cb;
	std::function<void(u8)> serialTxCb;   // fires once a transmitted byte's timer lands

private:
	// --- mcs96_device core state ---
	int icount = 0;
	u64 bcount = 0;
	int inst_state = 0x200;   // STATE_FETCH
	static constexpr int STATE_FETCH = 0x200;
	static constexpr int STATE_FETCH_NOIRQ = 0x201;
	int cycles_scaling = 3;   // 8x9x: state time = 3 oscillator periods
	u8 pending_irq = 0;
	u8 pending_irq1 = 0, int_mask1 = 0;
	u16 PC = 0, PPC = 0, PSW = 0;
	u16 OP1 = 0;
	u8 OP2 = 0, OP3 = 0, OPI = 0;
	u32 TMP = 0;
	bool irq_requested = false;
	u64 total_cycles_ = 0;

	enum {
		F_ST = 0x0100, F_I = 0x0200, F_C = 0x0800, F_VT = 0x1000,
		F_V = 0x2000, F_N = 0x4000, F_Z = 0x8000
	};
	enum {
		IRQ_TIMER = 0x01, IRQ_AD = 0x02, IRQ_HSI = 0x04, IRQ_HSO = 0x08,
		IRQ_HSI0 = 0x10, IRQ_SOFT = 0x20, IRQ_SERIAL = 0x40, IRQ_EXTINT = 0x80
	};

	inline void next(int cycles) { total_cycles_ += u64(cycles_scaling) * cycles; icount -= cycles_scaling * cycles; inst_state = STATE_FETCH; }
	inline void next_noirq(int cycles) { total_cycles_ += u64(cycles_scaling) * cycles; icount -= cycles_scaling * cycles; inst_state = STATE_FETCH_NOIRQ; }
	void check_irq();
	inline u8 read_pc() { return busRead8 ? busRead8(PC++) : (PC++, u8(0)); }

	// Register-file/SFR dispatch (<0x100 address space, "regs" in the original).
	u8 regsRead8(u16 adr);
	void regsWrite8(u16 adr, u8 data);
	u16 regsRead16(u16 adr);
	void regsWrite16(u16 adr, u16 data);

	void reg_w8(u8 adr, u8 data) { regsWrite8(adr, data); }
	void reg_w16(u8 adr, u16 data) { regsWrite16(adr & 0xfe, data); }
	u8 reg_r8(u8 adr) { return regsRead8(adr); }
	u16 reg_r16(u8 adr) { return regsRead16(adr & 0xfe); }
	void any_w8(u16 adr, u8 data);
	void any_w16(u16 adr, u16 data);
	u8 any_r8(u16 adr);
	u16 any_r16(u16 adr);

	u8 do_addb(u8 v1, u8 v2);
	u16 do_add(u16 v1, u16 v2);
	u8 do_subb(u8 v1, u8 v2);
	u16 do_sub(u16 v1, u16 v2);
	u8 do_addcb(u8 v1, u8 v2);
	u16 do_addc(u16 v1, u16 v2);
	u8 do_subcb(u8 v1, u8 v2);
	u16 do_subc(u16 v1, u16 v2);
	void set_nz8(u8 v);
	void set_nz16(u16 v);

	// --- i8x9x peripheral logic (ported from i8x9x.cpp, unchanged behaviour except where
	// noted in the class comment above) ---
	static constexpr u32 TIMER_DIVISOR = 24; // 8 state times x 3 oscillator periods, see i8x9x.cpp
	u16 timer_value(int timer, u64 current_time) const;
	u64 timer_time_until(int timer, u64 current_time, u16 tval) const;
	void timer2_reset(u64 current_time);
	void commit_hso_cam();
	void trigger_cam(int id, u64 current_time);
	void set_hso(u8 mask, bool state);
	void ad_start(u64 current_time);
	void serial_send(u8 data);
	void serial_send_done();
	void internal_update(u64 current_time);
	void recompute_bcount(u64 event_time);

	u8 ad_result_r(int offset);
	void ad_command_w(u8 data);
	void hsi_mode_w(u8 data);
	u16 hsi_time_r();
	void hso_time_w(u16 data);
	u8 hsi_status_r();
	void hso_command_w(u8 data);
	u8 sbuf_r();
	void sbuf_w(u8 data);
	void int_mask_w(u8 data);
	u8 int_mask_r();
	void int_pending_w(u8 data);
	u8 int_pending_r();
	u16 timer1_r();
	u16 timer2_r();
	void watchdog_w(u8 data);
	u8 port0_r();
	void baud_rate_w(u8 data);
	u8 port1_r();
	void port1_w(u8 data);
	u8 port2_r();
	void port2_w(u8 data);
	u8 sp_stat_r();
	void sp_con_w(u8 data);
	void int_pending1_w(u8 data);
	u8 int_pending1_r();
	void int_mask1_w(u8 data);
	u8 int_mask1_r();
	u8 ios0_r();
	void ioc0_w(u8 data);
	u8 ios1_r();
	void ioc1_w(u8 data);
	void pwm_control_w(u8 data);

	// --- opcode bodies (mcs96ops.hxx, machine-generated - see file header) ---
#include "mcs96ops_decls.hxx"

	void execFull();
};
