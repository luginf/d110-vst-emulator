// Hand-ported from the vendored MAME tree's devices/video/msm6222b.cpp (the D-110's real LCD
// controller, MSM6222B-01 variant - HD44780-compatible with a fixed CGROM). Near-verbatim:
// the original has exactly two MAME touchpoints, both replaced here -
//   1. optional_region_ptr<uint8_t> m_cgrom -> a plain pointer set via setCgrom().
//   2. machine().time().as_ticks(250000) in blink_on() -> an explicit tick count the caller
//      advances (see setClockTicks()), since this class no longer has its own notion of
//      wall-clock time.
// Everything else - control_w/data_w/cursor_step/shift_step/render() - is pure state-machine
// logic over ddram[80]/cgram[64]/render_buf[80*16], unchanged.
#pragma once

#include <cstdint>
#include <cstring>

class Msm6222b {
public:
	using u8 = uint8_t;
	using u64 = uint64_t;

	Msm6222b() { reset(); }
	void reset();

	// cgrom must point at >= 16*128 bytes (0x1000 for the -01 variant) and outlive this
	// object - matches msm6222b-01.bin (SHA1 e108b520e6d20459a7bbd5958bbfa1d551a690bd),
	// already present beside the other ROMs in D-110 Data.
	void setCgrom(const u8 *cgrom) { cgrom_ = cgrom; }

	void control_w(u8 data);
	u8 control_r();
	void data_w(u8 data);

	// 250kHz tick count, used only for the cursor-blink phase - purely cosmetic, no effect
	// on displayed text. Advance it however is convenient (e.g. from a sample counter).
	void setClockTicks(u64 ticks) { clockTicks_ = ticks; }

	// Character n's 8 (or 11, double-height) rows live at bytes n*16..n*16+7(+10). Only the
	// low 5 bits of each row byte are used. One-line mode: n = 0..79. Two-line: first line
	// 0..39, second 40..79.
	const u8 *render();

private:
	void cursor_step(bool direction);
	void shift_step(bool direction);
	bool blink_on() const;

	const u8 *cgrom_ = nullptr;
	u64 clockTicks_ = 0;

	u8 cgram[8 * 8] = {};
	u8 ddram[80] = {};
	u8 render_buf[80 * 16] = {};
	bool cursor_direction = false, cursor_blinking = false, two_line = false,
	     shift_on_write = false, double_height = false, cursor_on = false, display_on = false;
	u8 adc = 0, shift = 0;
};
