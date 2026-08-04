// See msm6222b.h for provenance - transcribed from the vendored MAME tree's
// devices/video/msm6222b.cpp, byte-for-byte except the two touchpoints noted there.
#include "msm6222b.h"

void Msm6222b::reset() {
	std::memset(cgram, 0, sizeof(cgram));
	std::memset(ddram, 0x20, sizeof(ddram));

	cursor_direction = true;
	cursor_blinking = false;
	display_on = false;
	two_line = false;
	cursor_on = false;
	shift_on_write = false;
	double_height = false;
	adc = 0x00;
	shift = 0;
}

void Msm6222b::control_w(u8 data) {
	int cmd;
	for (cmd = 7; cmd >= 0 && !(data & (1 << cmd)); cmd--) {}
	switch (cmd) {
	case 0:
		std::memset(ddram, 0x20, sizeof(ddram));
		adc = 0x00;
		break;
	case 1:
		adc = 0x00;
		shift = 0x00;
		break;
	case 2:
		shift_on_write = data & 1;
		cursor_direction = data & 2;
		break;
	case 3:
		display_on = data & 4;
		cursor_on = data & 2;
		cursor_blinking = data & 1;
		break;
	case 4:
		if (data & 8) shift_step(data & 4);
		else cursor_step(data & 4);
		break;
	case 5:
		two_line = data & 8;
		double_height = (data & 0xc) == 4;
		break;
	case 6:
		adc = data & 0x3f;
		break;
	case 7:
		adc = data;
		break;
	}
}

Msm6222b::u8 Msm6222b::control_r() { return adc & 0x7f; }

void Msm6222b::data_w(u8 data) {
	if (adc & 0x80) {
		int adr = adc & 0x7f;
		if (two_line) {
			if ((adr >= 40 && adr < 64) || adr >= 64 + 40) adr = -1;
			if (adr >= 64) adr += 40 - 64;
		} else {
			if (adr >= 80) adr = -1;
		}
		if (adr != -1) {
			ddram[adr] = data;
			if (shift_on_write) shift_step(cursor_direction);
			else cursor_step(cursor_direction);
		}
	} else {
		if (adc < 8 * 8) {
			cgram[adc] = data;
			cursor_step(cursor_direction);
		}
	}
}

void Msm6222b::cursor_step(bool direction) {
	if (direction) {
		if (adc & 0x80) {
			if (two_line && adc == (0x80 | 39)) adc = 0x80 | 64;
			else if (two_line && adc == (0x80 | (64 + 39))) adc = 0x80;
			else if ((!two_line) && adc == (0x80 | 79)) adc = 0x80;
			else adc++;
		} else {
			if (adc == 8 * 8 - 1) adc = 0x00;
			else adc++;
		}
	} else {
		if (adc & 0x80) {
			if (adc == 0x80) adc = two_line ? 0x80 | (64 + 39) : 0x80 | 79;
			else if (two_line && adc == (0x80 | 64)) adc = 0x80 | 39;
			else adc--;
		} else {
			if (adc == 0x00) adc = 8 * 8 - 1;
			else adc--;
		}
	}
}

void Msm6222b::shift_step(bool direction) {
	if (direction) {
		if (shift == 79) shift = 0;
		else shift++;
	} else {
		if (shift == 0) shift = 79;
		else shift--;
	}
}

bool Msm6222b::blink_on() const {
	if (!cursor_blinking) return false;
	if (double_height) return clockTicks_ % 281600 >= 140800;
	return clockTicks_ % 204800 >= 102400;
}

const Msm6222b::u8 *Msm6222b::render() {
	std::memset(render_buf, 0, 80 * 16);
	if (!display_on) return render_buf;

	int char_height = double_height ? 11 : 8;

	for (int i = 0; i < 80; i++) {
		u8 c = ddram[(i + shift) % 80];
		if (c < 16)
			std::memcpy(render_buf + 16 * i, double_height ? cgram + 8 * (c & 6) : cgram + 8 * (c & 7), char_height);
		else if (cgrom_)
			std::memcpy(render_buf + 16 * i, cgrom_ + 16 * c, char_height);
	}

	if (cursor_on) {
		int cpos = adc & 0x7f;
		if (two_line) {
			if ((cpos >= 40 && cpos < 64) || cpos >= 64 + 40) cpos = -1;
			else if (cpos >= 64) cpos += 40 - 64;
		} else {
			if (cpos >= 80) cpos = -1;
		}
		if (cpos != -1) {
			cpos = (cpos + shift) % 80;
			render_buf[cpos * 16 + (double_height ? 10 : 7)] |= 0x1f;
			if (blink_on())
				for (int i = 0; i < char_height; i++)
					render_buf[cpos * 16 + i] ^= 0x1f;
		}
	}

	return render_buf;
}
