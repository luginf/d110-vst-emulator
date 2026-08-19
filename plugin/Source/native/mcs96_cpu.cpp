// See mcs96_cpu.h for provenance. Everything below is ported near-verbatim from the vendored
// MAME tree's mcs96.cpp + i8x9x.cpp (pure integer/state logic, zero MAME framework calls in
// either source file outside of device boilerplate this port doesn't need), except:
//  - setExtIntLine() clears pending_irq on the falling edge (see class comment, point 1).
//  - logerror()/machine().side_effects_disabled() diagnostics are dropped - they were
//    MAME-only tracing, no behavioural effect.
#include "mcs96_cpu.h"

// --- arithmetic/flag helpers (mcs96.cpp, byte-for-byte) --------------------------------

Mcs96Cpu::u8 Mcs96Cpu::do_addb(u8 v1, u8 v2) {
	uint16_t sum = v1 + v2;
	PSW &= ~(F_Z | F_N | F_C | F_V);
	if (!u8(sum)) PSW |= F_Z;
	else if (int8_t(sum) < 0) PSW |= F_N;
	if (~(v1 ^ v2) & (v1 ^ sum) & 0x80) PSW |= F_V | F_VT;
	if (sum & 0xff00) PSW |= F_C;
	return u8(sum);
}

Mcs96Cpu::u16 Mcs96Cpu::do_add(u16 v1, u16 v2) {
	uint32_t sum = v1 + v2;
	PSW &= ~(F_Z | F_N | F_C | F_V);
	if (!u16(sum)) PSW |= F_Z;
	else if (int16_t(sum) < 0) PSW |= F_N;
	if (~(v1 ^ v2) & (v1 ^ sum) & 0x8000) PSW |= F_V | F_VT;
	if (sum & 0xffff0000) PSW |= F_C;
	return u16(sum);
}

Mcs96Cpu::u8 Mcs96Cpu::do_subb(u8 v1, u8 v2) {
	uint16_t diff = v1 - v2;
	PSW &= ~(F_N | F_V | F_Z | F_C);
	if (!u8(diff)) PSW |= F_Z;
	else if (int8_t(diff) < 0) PSW |= F_N;
	if ((v1 ^ v2) & (v1 ^ diff) & 0x80) PSW |= F_V;
	if (!(diff & 0xff00)) PSW |= F_C;
	return u8(diff);
}

Mcs96Cpu::u16 Mcs96Cpu::do_sub(u16 v1, u16 v2) {
	uint32_t diff = v1 - v2;
	PSW &= ~(F_N | F_V | F_Z | F_C);
	if (!u16(diff)) PSW |= F_Z;
	else if (int16_t(diff) < 0) PSW |= F_N;
	if ((v1 ^ v2) & (v1 ^ diff) & 0x8000) PSW |= F_V;
	if (!(diff & 0xffff0000)) PSW |= F_C;
	return u16(diff);
}

Mcs96Cpu::u8 Mcs96Cpu::do_addcb(u8 v1, u8 v2) {
	uint16_t sum = v1 + v2 + (PSW & F_C ? 1 : 0);
	PSW &= ~(F_Z | F_N | F_C | F_V);
	if (!u8(sum)) PSW |= F_Z;
	else if (int8_t(sum) < 0) PSW |= F_N;
	if (~(v1 ^ v2) & (v1 ^ sum) & 0x80) PSW |= F_V | F_VT;
	if (sum & 0xff00) PSW |= F_C;
	return u8(sum);
}

Mcs96Cpu::u16 Mcs96Cpu::do_addc(u16 v1, u16 v2) {
	uint32_t sum = v1 + v2 + (PSW & F_C ? 1 : 0);
	PSW &= ~(F_Z | F_N | F_C | F_V);
	if (!u16(sum)) PSW |= F_Z;
	else if (int16_t(sum) < 0) PSW |= F_N;
	if (~(v1 ^ v2) & (v1 ^ sum) & 0x8000) PSW |= F_V | F_VT;
	if (sum & 0xffff0000) PSW |= F_C;
	return u16(sum);
}

Mcs96Cpu::u8 Mcs96Cpu::do_subcb(u8 v1, u8 v2) {
	uint16_t diff = v1 - v2 - (PSW & F_C ? 0 : 1);
	PSW &= ~(F_N | F_V | F_Z | F_C);
	if (!u8(diff)) PSW |= F_Z;
	else if (int8_t(diff) < 0) PSW |= F_N;
	if ((v1 ^ v2) & (v1 ^ diff) & 0x80) PSW |= F_V;
	if (!(diff & 0xff00)) PSW |= F_C;
	return u8(diff);
}

Mcs96Cpu::u16 Mcs96Cpu::do_subc(u16 v1, u16 v2) {
	uint32_t diff = v1 - v2 - (PSW & F_C ? 0 : 1);
	PSW &= ~(F_N | F_V | F_Z | F_C);
	if (!u16(diff)) PSW |= F_Z;
	else if (int16_t(diff) < 0) PSW |= F_N;
	if ((v1 ^ v2) & (v1 ^ diff) & 0x8000) PSW |= F_V;
	if (!(diff & 0xffff0000)) PSW |= F_C;
	return u16(diff);
}

void Mcs96Cpu::set_nz8(u8 v) {
	PSW &= ~(F_N | F_V | F_Z | F_C);
	if (!v) PSW |= F_Z;
	else if (int8_t(v) < 0) PSW |= F_N;
}

void Mcs96Cpu::set_nz16(u16 v) {
	PSW &= ~(F_N | F_V | F_Z | F_C);
	if (!v) PSW |= F_Z;
	else if (int16_t(v) < 0) PSW |= F_N;
}

void Mcs96Cpu::check_irq() {
	irq_requested = (PSW & F_I) && (((PSW & pending_irq) != 0) || ((int_mask1 & pending_irq1) != 0));
}

// --- register-file/SFR dispatch (i8x9x_device::internal_regs, transcribed as direct
// address decode instead of a MAME address_map - same effective behaviour) -------------

Mcs96Cpu::u8 Mcs96Cpu::regsRead8(u16 a) {
	switch (a) {
	case 0x00: case 0x01: return 0;                 // "r0", always reads 0
	case 0x02: return ad_result_r(0);
	case 0x03: return ad_result_r(1);
	case 0x04: return u8(hsi_time_r());
	case 0x05: return u8(hsi_time_r() >> 8);
	case 0x06: return hsi_status_r();
	case 0x07: return sbuf_r();
	case 0x08: return int_mask_r();
	case 0x09: return int_pending_r();
	case 0x0a: return u8(timer1_r());
	case 0x0b: return u8(timer1_r() >> 8);
	case 0x0c: return u8(timer2_r());
	case 0x0d: return u8(timer2_r() >> 8);
	case 0x0e: return port0_r();
	case 0x0f: return port1_r();
	case 0x10: return port2_r();
	case 0x11: return sp_stat_r();
	case 0x12: return int_pending1_r();
	case 0x13: return int_mask1_r();
	case 0x15: return ios0_r();
	case 0x16: return ios1_r();
	default:
		if (a >= 0x18) return regFile[a];
		return 0; // 0x14, 0x17: unmapped in internal_regs
	}
}

void Mcs96Cpu::regsWrite8(u16 a, u8 data) {
	switch (a) {
	case 0x00: case 0x01: return; // nopw
	case 0x02: ad_command_w(data); return;
	case 0x03: hsi_mode_w(data); return;
	// 0x04/0x05 (HSO_TIME) is word-native in the real map (hso_time_w takes u16) - a genuine
	// byte-write instruction targeting just one lane still has to merge into the other lane
	// and go through the same commit, matching how MAME's memory system would synthesize a
	// byte access against a word-native handler.
	case 0x04: hso_time_w(u16((hso_time & 0xff00) | data)); return;
	case 0x05: hso_time_w(u16((hso_time & 0x00ff) | (u16(data) << 8))); return;
	case 0x06: hso_command_w(data); return;
	case 0x07: sbuf_w(data); return;
	case 0x08: int_mask_w(data); return;
	case 0x09: int_pending_w(data); return;
	case 0x0a: watchdog_w(data); return;
	case 0x0e: baud_rate_w(data); return;
	case 0x0f: port1_w(data); return;
	case 0x10: port2_w(data); return;
	case 0x11: sp_con_w(data); return;
	case 0x12: int_pending1_w(data); return;
	case 0x13: int_mask1_w(data); return;
	case 0x15: ioc0_w(data); return;
	case 0x16: ioc1_w(data); return;
	case 0x17: pwm_control_w(data); return;
	default:
		if (a >= 0x18) regFile[a] = data;
		return; // 0x0b,0x0c,0x0d (read-only timers), 0x14: writes dropped
	}
}

Mcs96Cpu::u16 Mcs96Cpu::regsRead16(u16 a) {
	switch (a) {
	case 0x00: return 0;
	case 0x04: return hsi_time_r();
	case 0x0a: return timer1_r();
	case 0x0c: return timer2_r();
	default:
		if (a >= 0x18) return u16(regFile[a]) | (u16(regFile[a + 1]) << 8);
		return u16(regsRead8(a)) | (u16(regsRead8(a + 1)) << 8);
	}
}

void Mcs96Cpu::regsWrite16(u16 a, u16 data) {
	switch (a) {
	case 0x00: return;
	case 0x04: hso_time_w(data); return;
	default:
		if (a >= 0x18) { regFile[a] = u8(data); regFile[a + 1] = u8(data >> 8); return; }
		regsWrite8(a, u8(data));
		regsWrite8(a + 1, u8(data >> 8));
		return;
	}
}

void Mcs96Cpu::any_w8(u16 adr, u8 data) {
	if (adr < 0x100) regsWrite8(adr, data);
	else if (busWrite8) busWrite8(adr, data);
}

void Mcs96Cpu::any_w16(u16 adr, u16 data) {
	adr &= 0xfffe;
	if (adr < 0x100) regsWrite16(adr, data);
	else if (busWrite8) { busWrite8(adr, u8(data)); busWrite8(adr + 1, u8(data >> 8)); }
}

Mcs96Cpu::u8 Mcs96Cpu::any_r8(u16 adr) {
	if (adr < 0x100) return regsRead8(adr);
	return busRead8 ? busRead8(adr) : 0;
}

Mcs96Cpu::u16 Mcs96Cpu::any_r16(u16 adr) {
	adr &= 0xfffe;
	if (adr < 0x100) return regsRead16(adr);
	if (!busRead8) return 0;
	return u16(busRead8(adr)) | (u16(busRead8(adr + 1)) << 8);
}

// --- i8x9x peripheral logic (i8x9x.cpp, transcribed) ------------------------------------

Mcs96Cpu::u16 Mcs96Cpu::timer_value(int timer, u64 current_time) const {
	if (timer == 2) current_time -= base_timer2;
	return u16(current_time / TIMER_DIVISOR);
}

Mcs96Cpu::u64 Mcs96Cpu::timer_time_until(int timer, u64 current_time, u16 tval) const {
	u64 timer_base = timer == 2 ? base_timer2 : 0;
	u64 delta = (current_time - timer_base) / TIMER_DIVISOR;
	u32 tdelta = u16(tval - delta);
	if (!tdelta) tdelta = 0x10000;
	return timer_base + ((delta + tdelta) * TIMER_DIVISOR);
}

void Mcs96Cpu::timer2_reset(u64 current_time) {
	base_timer2 = current_time;
	for (int i = 0; i < 8; i++)
		if ((hso_active >> i & 1) && (hso_info[i].command >> 6 & 1))
			hso_info[i].fire_at = timer_time_until(2, current_time, hso_info[i].time);
}

void Mcs96Cpu::commit_hso_cam() {
	for (int i = 0; i < 8; i++)
		if (!(hso_active >> i & 1)) {
			hso_active |= 1 << i;
			if (hso_active == 0xff) ios0 |= 0x40;
			hso_info[i].command = hso_command;
			hso_info[i].time = hso_time;
			hso_info[i].fire_at = timer_time_until((hso_command >> 6 & 1) ? 2 : 1, total_cycles_, hso_time);
			internal_update(total_cycles_);
			return;
		}
	// CAM full - the real part has one hold register; a second overflow before the first
	// is taken destroys a command outright. Matches i8x9x.cpp's own behaviour.
	ios0 |= 0xc0;
	hso_cam_hold.command = hso_command;
	hso_cam_hold.time = hso_time;
}

void Mcs96Cpu::trigger_cam(int id, u64 current_time) {
	HsoCamEntry &cam = hso_info[id];
	if (hso_active == 0xff && !(ios0 >> 7 & 1)) ios0 &= 0xbf;
	hso_active &= ~(1 << id);
	switch (cam.command & 0x0f) {
	case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: case 0x5:
		set_hso(1 << (cam.command & 7), (cam.command >> 5 & 1) != 0);
		break;
	case 0x6: set_hso(0x03, (cam.command >> 5 & 1) != 0); break;
	case 0x7: set_hso(0x0c, (cam.command >> 5 & 1) != 0); break;
	case 0x8: case 0x9: case 0xa: case 0xb: ios1 |= 1 << (cam.command & 3); break;
	case 0xe: timer2_reset(current_time); break;
	case 0xf: ad_start(current_time); break;
	default: break;
	}
	if (cam.command >> 4 & 1) {
		pending_irq |= (cam.command >> 3 & 1) ? IRQ_SOFT : IRQ_HSO;
		check_irq();
	}
}

void Mcs96Cpu::set_hso(u8 mask, bool state) {
	// hso_cb has no D-110 binding (roland_d10.cpp never sets one) - nothing to forward.
	if (state) ios0 |= mask; else ios0 &= ~mask;
}

void Mcs96Cpu::ad_start(u64 current_time) {
	ad_result = 8 | (ad_command & 7);
	// No analog inputs are wired on the D-110 (ach*_cb all unbound) - result stays at its
	// "converted, all-zero sample" value, matching the driver's own unbound-callback default.
	ad_done = current_time + 88;
	internal_update(current_time);
}

void Mcs96Cpu::serial_send(u8 data) {
	serial_send_buf = data;
	serial_send_timer = total_cycles_ + 9600;
}

void Mcs96Cpu::serial_send_done() {
	serial_send_timer = 0;
	if (serialTxCb) serialTxCb(serial_send_buf);
	pending_irq |= IRQ_SERIAL;
	pending_irq1 |= 0x01;
	sp_stat |= 0x20;
	check_irq();
}

void Mcs96Cpu::recompute_bcount(u64 event_time) {
	if (!event_time || event_time >= total_cycles_ + u64(icount)) { bcount = 0; return; }
	bcount = total_cycles_ + u64(icount) - event_time;
}

void Mcs96Cpu::internal_update(u64 current_time) {
	for (int i = 0; i < 8; i++)
		if ((hso_active >> i & 1) && current_time >= hso_info[i].fire_at)
			trigger_cam(i, current_time);

	if (ad_done && current_time >= ad_done) {
		ad_done = 0;
		ad_result &= ~8;
		pending_irq |= IRQ_AD;
		check_irq();
	}

	if (serial_send_timer && current_time >= serial_send_timer)
		serial_send_done();

	u64 event_time = 0;
	for (int i = 0; i < 8; i++) {
		if (!(hso_active >> i & 1) && (ios0 >> 7 & 1)) {
			hso_info[i] = hso_cam_hold;
			hso_info[i].fire_at = timer_time_until((hso_cam_hold.command >> 6 & 1) ? 2 : 1, current_time, hso_cam_hold.time);
			hso_active |= 1 << i;
			ios0 &= 0x7f;
			if (hso_active == 0xff) ios0 |= 0x40;
		}
		if (hso_active >> i & 1) {
			if (!event_time || hso_info[i].fire_at < event_time) event_time = hso_info[i].fire_at;
		}
	}

	if (ad_done && (!event_time || ad_done < event_time)) event_time = ad_done;
	if (serial_send_timer && (!event_time || serial_send_timer < event_time)) event_time = serial_send_timer;

	recompute_bcount(event_time);
}

// --- SFR accessor functions (i8x9x.cpp, transcribed; N8097BH-specific masks inlined:
// p0_mask=0xff, has_p1=true, p2_mask=0xff - the only variant the D-110 ever instantiates) --

Mcs96Cpu::u8 Mcs96Cpu::ad_result_r(int offset) { return u8(ad_result >> (offset ? 8 : 0)); }

void Mcs96Cpu::ad_command_w(u8 data) {
	ad_command = data & 0xf;
	if (ad_command & 8) ad_start(total_cycles_);
}

void Mcs96Cpu::hsi_mode_w(u8 data) { hsi_mode = data; }

void Mcs96Cpu::hso_time_w(u16 data) { hso_time = data; commit_hso_cam(); }

Mcs96Cpu::u16 Mcs96Cpu::hsi_time_r() { return 0x0000; }

void Mcs96Cpu::hso_command_w(u8 data) { hso_command = data; }

Mcs96Cpu::u8 Mcs96Cpu::hsi_status_r() { return hsi_status; }

void Mcs96Cpu::sbuf_w(u8 data) { serial_send(data); }

Mcs96Cpu::u8 Mcs96Cpu::sbuf_r() {
	// Reading SBUF consumes the received byte and clears RI, as on hardware - without this
	// a fast sender could silently overwrite a byte the firmware hadn't picked up yet.
	sp_stat &= ~0x40;
	return sbuf;
}

void Mcs96Cpu::watchdog_w(u8) {}

Mcs96Cpu::u16 Mcs96Cpu::timer1_r() { return timer_value(1, total_cycles_); }
Mcs96Cpu::u16 Mcs96Cpu::timer2_r() { return timer_value(2, total_cycles_); }

void Mcs96Cpu::baud_rate_w(u8 data) {
	if (brh) baud_reg = (baud_reg & 0x00ff) | (u16(data) << 8);
	else baud_reg = (baud_reg & 0xff00) | data;
	brh = !brh;
}

Mcs96Cpu::u8 Mcs96Cpu::port0_r() { return inP0Cb ? inP0Cb() : 0; }

void Mcs96Cpu::port1_w(u8 data) { port1 = data; if (outP1Cb) outP1Cb(data); }
Mcs96Cpu::u8 Mcs96Cpu::port1_r() { return (inP1Cb ? inP1Cb() : u8(0xff)) & port1; }

void Mcs96Cpu::port2_w(u8 data) { data &= 0xe1; port2 = data; if (outP2Cb) outP2Cb(data); }
Mcs96Cpu::u8 Mcs96Cpu::port2_r() {
	u8 in = inP2Cb ? inP2Cb() : u8(0xc2);
	return u8((in | 0x25) & (port2 | (extint ? 0x1e : 0x1a)));
}

void Mcs96Cpu::sp_con_w(u8 data) { sp_con = data & 0x1f; }

Mcs96Cpu::u8 Mcs96Cpu::sp_stat_r() { u8 res = sp_stat; sp_stat &= 0x80; return res; }

void Mcs96Cpu::ioc0_w(u8 data) { ioc0 = data & 0xfd; if (data >> 1 & 1) timer2_reset(total_cycles_); }

Mcs96Cpu::u8 Mcs96Cpu::ios0_r() { return ios0; }

void Mcs96Cpu::ioc1_w(u8 data) { ioc1 = data; }

Mcs96Cpu::u8 Mcs96Cpu::ios1_r() { u8 res = ios1; ios1 = ios1 & 0xc0; return res; }

void Mcs96Cpu::pwm_control_w(u8 data) { pwm_control = data; }

void Mcs96Cpu::int_mask_w(u8 data) { PSW = (PSW & 0xff00) | data; check_irq(); }
Mcs96Cpu::u8 Mcs96Cpu::int_mask_r() { return u8(PSW); }
void Mcs96Cpu::int_pending_w(u8 data) { pending_irq = data; check_irq(); }
Mcs96Cpu::u8 Mcs96Cpu::int_pending_r() { return pending_irq; }
void Mcs96Cpu::int_mask1_w(u8 data) { int_mask1 = data; check_irq(); }
Mcs96Cpu::u8 Mcs96Cpu::int_mask1_r() { return int_mask1; }
void Mcs96Cpu::int_pending1_w(u8 data) { pending_irq1 = data; check_irq(); }
Mcs96Cpu::u8 Mcs96Cpu::int_pending1_r() { return pending_irq1; }

// --- public interface --------------------------------------------------------------------

void Mcs96Cpu::setExtIntLine(bool asserted) {
	// Rising edge: raise IRQ_EXTINT exactly as i8x9x_device::execute_set_input() does.
	// Falling edge: unlike the vendored core (which never clears pending_irq here at all -
	// see the class-comment fix note), clear it here. fetch_full()'s dispatcher explicitly
	// excludes level 7 (EXTINT) from its own auto-clear-on-take logic, so nothing else in
	// this class ever clears this bit; without doing it here the same stale interrupt would
	// be re-taken forever after the first assertion.
	if (!extint && asserted && !(ioc1 >> 1 & 1)) {
		pending_irq |= IRQ_EXTINT;
		check_irq();
	}
	if (extint && !asserted) {
		pending_irq &= ~IRQ_EXTINT;
		check_irq();
	}
	extint = asserted;
}

void Mcs96Cpu::serialWrite(u8 val) {
	if (sp_stat & 0x40) ++serial_overrun_count_; // previous byte never read - see the header's own comment
	sbuf = val;
	sp_stat |= 0x40;
	pending_irq |= IRQ_SERIAL;
	pending_irq1 |= 0x02;
	check_irq();
}

void Mcs96Cpu::reset() {
	PC = 0x2080;
	PPC = PC;
	PSW = 0;
	irq_requested = false;
	inst_state = STATE_FETCH;
	icount = 0;
	bcount = 0;

	hso_active = 0;
	hso_command = 0;
	hso_time = 0;
	timer2_reset(total_cycles_);
	port1 = 0xff;
	port2 = 0xc1; // P2.5 cleared (p2_mask=0xff for N8097BH)
	ios0 = ios1 = 0x00;
	ioc0 &= 0xaa;
	ioc1 = (ioc1 & 0xae) | 0x01;
	ad_result = 0;
	ad_done = 0;
	pwm_control = 0x00;
	sp_con &= 0x17;
	sp_stat &= 0x80;
	serial_send_timer = 0;
	brh = false;
	if (outP1Cb) outP1Cb(0xff);
	if (outP2Cb) outP2Cb(0xc1);
}

int Mcs96Cpu::run(int cycles) {
	icount = cycles;
	const u64 startTotal = total_cycles_;
	internal_update(total_cycles_);
	while (icount > 0) {
		while (icount > int(bcount)) {
			int picount = inst_state >= 0x200 ? -1 : icount;
			bool wasFetch = inst_state >= 0x200;
			execFull();
			if (wasFetch && onFetch) onFetch(PPC);
			if (icount == picount)
				break; // a case fell through without consuming cycles - avoid a hang
		}
		while (bcount && icount <= int(bcount))
			internal_update(total_cycles_ + u64(icount) - bcount);
	}
	return int(total_cycles_ - startTotal);
}

#include "mcs96ops.hxx"
#include "mcs96_exec.hxx"
