void Mcs96Cpu::skip_immed_1b_full()
{
	OP1 = read_pc();
	next(4);
}

void Mcs96Cpu::clr_direct_1w_full()
{
	OP1 = read_pc();
	reg_w16(OP1, 0x0000);
	next(4);
}

void Mcs96Cpu::not_direct_1w_full()
{
	OP1 = read_pc();
	TMP = ~reg_r16(OP1);
	set_nz16(TMP);
	reg_w16(OP1, TMP);
	next(4);
}

void Mcs96Cpu::neg_direct_1w_full()
{
	OP1 = read_pc();
	TMP = reg_r16(OP1);
	reg_w16(OP1, do_sub(0, TMP));
	next(4);
}

void Mcs96Cpu::xch_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
}

void Mcs96Cpu::dec_direct_1w_full()
{
	OP1 = read_pc();
	TMP = reg_r16(OP1);
	reg_w16(OP1, do_sub(TMP, 1));
	next(4);
}

void Mcs96Cpu::ext_direct_1w_full()
{
	OP1 = read_pc();
	OP1 &= 0xfc;
	TMP = int16_t(reg_r16(OP1));
	set_nz16(TMP);
	reg_w16(OP1+2, TMP >> 16);
	next(4);
}

void Mcs96Cpu::inc_direct_1w_full()
{
	OP1 = read_pc();
	TMP = reg_r16(OP1);
	reg_w16(OP1, do_add(TMP, 1));
	next(4);
}

void Mcs96Cpu::shr_immed_or_reg_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	if(OP1 >= 0x10) {
		OP1 = reg_r8(OP1) & 0x1f;
	}
	TMP = reg_r16(OP2);
	PSW &= ~(F_Z|F_N|F_C|F_V|F_ST);
	if(OP1 >= 2 && (TMP & (0xffff >> (OP1 <= 16 ? 17-OP1 : 0))))
		PSW |= F_ST;
	if(OP1 >= 1 && OP1 <= 16 && (TMP & (0x0001 << (OP1-1))))
		PSW |= F_C;
	TMP = uint16_t(TMP) >> OP1;
	if(!TMP)
		PSW |= F_Z;
	else if(int16_t(TMP) < 0)
		PSW |= F_N;
	reg_w16(OP2, TMP);
	next(OP1 ? 7+OP1 : 8);
}

void Mcs96Cpu::shl_immed_or_reg_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	if(OP1 >= 0x10) {
		OP1 = reg_r8(OP1) & 0x1f;
	}
	TMP = reg_r16(OP2);
	PSW &= ~(F_Z|F_N|F_C|F_V|F_ST);
	if(OP1 >= 2 && (TMP & (0xffff << (OP1 <= 16 ? 17-OP1 : 0))))
		PSW |= F_ST;
	if(OP1 >= 1 && OP1 <= 16 && (TMP & (0x8000 >> (OP1-1))))
		PSW |= F_C;
	TMP = uint16_t(TMP << OP1);
	if(!TMP)
		PSW |= F_Z;
	else if(int16_t(TMP) < 0)
		PSW |= F_N;
	reg_w16(OP2, TMP);
	next(OP1 ? 7+OP1 : 8);
}

void Mcs96Cpu::shra_immed_or_reg_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	if(OP1 >= 0x10) {
		OP1 = reg_r8(OP1) & 0x1f;
	}
	TMP = reg_r16(OP2);
	PSW &= ~(F_Z|F_N|F_C|F_V|F_ST);
	if(OP1 >= 2 && (TMP & (0xffff >> (OP1 <= 16 ? 17-OP1 : 0))))
		PSW |= F_ST;
	if(OP1 >= 1 && OP1 <= 16 && (TMP & (0x0001 << (OP1-1))))
		PSW |= F_C;
	TMP = int16_t(TMP) >> OP1;
	if(!TMP)
		PSW |= F_Z;
	else if(int16_t(TMP) < 0)
		PSW |= F_N;
	reg_w16(OP2, TMP);
	next(OP1 ? 7+OP1 : 8);
}

void Mcs96Cpu::shrl_immed_or_reg_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	if(OP1 >= 0x10) {
		OP1 = reg_r8(OP1) & 0x1f;
	}
	OP2 &= 0xfc;
	TMP = reg_r16(OP2);
	TMP |= reg_r16(OP2+2) << 16;
	PSW &= ~(F_Z|F_N|F_C|F_V|F_ST);
	if(OP1 >= 2 && (TMP & (0xffffffff >> (33-OP1))))
		PSW |= F_ST;
	if(OP1 >= 1 && (TMP & (0x00000001 << (OP1-1))))
		PSW |= F_C;
	TMP = TMP >> OP1;
	if(!TMP)
		PSW |= F_Z;
	else if(int32_t(TMP) < 0)
		PSW |= F_N;
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	next(OP1 ? 7+OP1 : 8);
}

void Mcs96Cpu::shll_immed_or_reg_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	if(OP1 >= 0x10) {
		OP1 = reg_r8(OP1) & 0x1f;
	}
	OP2 &= 0xfc;
	TMP = reg_r16(OP2);
	TMP |= reg_r16(OP2+2) << 16;
	PSW &= ~(F_Z|F_N|F_C|F_V|F_ST);
	if(OP1 >= 2 && (TMP & (0xffffffff << (33-OP1))))
		PSW |= F_ST;
	if(OP1 >= 1 && (TMP & (0x80000000 >> (OP1-1))))
		PSW |= F_C;
	TMP = TMP << OP1;
	if(!TMP)
		PSW |= F_Z;
	else if(int32_t(TMP) < 0)
		PSW |= F_N;
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	next(OP1 ? 7+OP1 : 8);
}

void Mcs96Cpu::shral_immed_or_reg_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	if(OP1 >= 0x10) {
		OP1 = reg_r8(OP1) & 0x1f;
	}
	OP2 &= 0xfc;
	TMP = reg_r16(OP2);
	TMP |= reg_r16(OP2+2) << 16;
	PSW &= ~(F_Z|F_N|F_C|F_V|F_ST);
	if(OP1 >= 2 && (TMP & (0xffffffff >> (33-OP1))))
		PSW |= F_ST;
	if(OP1 >= 1 && (TMP & (0x00000001 << (OP1-1))))
		PSW |= F_C;
	TMP = int32_t(TMP) >> OP1;
	if(!TMP)
		PSW |= F_Z;
	else if(int32_t(TMP) < 0)
		PSW |= F_N;
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	next(OP1 ? 7+OP1 : 8);
}

void Mcs96Cpu::norml_direct_2e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP2 &= 0xfc;
	TMP = reg_r16(OP2);
	TMP |= reg_r16(OP2+2) << 16;
	for(OP3 = 0; OP3 < 31 && int32_t(TMP) >= 0; OP3++);
	PSW &= ~(F_Z|F_N|F_C);
	if(!TMP)
		PSW |= F_Z;
	reg_w8(OP1, OP3);
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	next(11+OP3);
}

void Mcs96Cpu::clrb_direct_1b_full()
{
	OP1 = read_pc();
	reg_w8(OP1, 0x00);
	next(4);
}

void Mcs96Cpu::notb_direct_1b_full()
{
	OP1 = read_pc();
	TMP = ~reg_r8(OP1);
	set_nz8(TMP);
	reg_w8(OP1, TMP);
	next(4);
}

void Mcs96Cpu::negb_direct_1b_full()
{
	OP1 = read_pc();
	TMP = reg_r8(OP1);
	reg_w8(OP1, do_subb(0, TMP));
	next(4);
}

void Mcs96Cpu::xchb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
}

void Mcs96Cpu::decb_direct_1b_full()
{
	OP1 = read_pc();
	TMP = reg_r8(OP1);
	reg_w8(OP1, do_subb(TMP, 1));
	next(4);
}

void Mcs96Cpu::extb_direct_1b_full()
{
	OP1 = read_pc();
	OP1 &= 0xfe;
	TMP = int8_t(reg_r8(OP1));
	set_nz8(TMP);
	reg_w16(OP1, TMP);
	next(4);
}

void Mcs96Cpu::incb_direct_1b_full()
{
	OP1 = read_pc();
	TMP = reg_r8(OP1);
	reg_w8(OP1, do_addb(TMP, 1));
	next(4);
}

void Mcs96Cpu::shrb_immed_or_reg_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	if(OP1 >= 0x10) {
		OP1 = reg_r8(OP1) & 0x1f;
	}
	TMP = reg_r8(OP2);
	PSW &= ~(F_Z|F_N|F_C|F_V|F_ST);
	if(OP1 >= 2 && (TMP & (0xff >> (OP1 <= 8 ? 9-OP1 : 0))))
		PSW |= F_ST;
	if(OP1 >= 1 && OP1 <= 8 && (TMP & (0x01 << (OP1-1))))
		PSW |= F_C;
	TMP = uint8_t(TMP) >> OP1;
	if(!TMP)
		PSW |= F_Z;
	else if(int8_t(TMP) < 0)
		PSW |= F_N;
	reg_w8(OP2, TMP);
	next(OP1 ? 7+OP1 : 8);
}

void Mcs96Cpu::shlb_immed_or_reg_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	if(OP1 >= 0x10) {
		OP1 = reg_r8(OP1) & 0x1f;
	}
	TMP = reg_r8(OP2);
	PSW &= ~(F_Z|F_N|F_C|F_V|F_ST);
	if(OP1 >= 2 && (TMP & (0xff << (OP1 <= 8 ? 9-OP1 : 0))))
		PSW |= F_ST;
	if(OP1 >= 1 && OP1 <= 8 && (TMP & (0x80 >> (OP1-1))))
		PSW |= F_C;
	TMP = uint8_t(TMP << OP1);
	if(!TMP)
		PSW |= F_Z;
	else if(int8_t(TMP) < 0)
		PSW |= F_N;
	reg_w8(OP2, TMP);
	next(OP1 ? 7+OP1 : 8);
}

void Mcs96Cpu::shrab_immed_or_reg_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	if(OP1 >= 0x10) {
		OP1 = reg_r8(OP1) & 0x1f;
	}
	TMP = reg_r8(OP2);
	PSW &= ~(F_Z|F_N|F_C|F_V|F_ST);
	if(OP1 >= 2 && (TMP & (0xff >> (OP1 <= 8 ? 9-OP1 : 0))))
		PSW |= F_ST;
	if(OP1 >= 1 && OP1 <= 8 && (TMP & (0x01 << (OP1-1))))
		PSW |= F_C;
	TMP = uint8_t(int8_t(TMP) >> OP1);
	if(!TMP)
		PSW |= F_Z;
	else if(int8_t(TMP) < 0)
		PSW |= F_N;
	reg_w8(OP2, TMP);
	next(OP1 ? 7+OP1 : 8);
}

void Mcs96Cpu::sjmp_rel11_full()
{
	OP1 = read_pc();
	OP1 = OP1 | ((inst_state & 7) << 8);
	if(OP1 & 0x400)
		OP1 |= 0xfc00;
	PC += OP1;
	next(8);
}

void Mcs96Cpu::scall_rel11_full()
{
	OP1 = read_pc();
	OP1 = OP1 | ((inst_state & 7) << 8);
	if(OP1 & 0x400)
		OP1 |= 0xfc00;
	TMP = reg_r16(0x18);
	TMP -= 2;
	reg_w16(0x18, TMP);
	any_w16(TMP, PC);
	PC += OP1;
	next(13); // real is 13/16 depending on sp's position
}

void Mcs96Cpu::jbc_brrel8_full()
{
	OP2 = read_pc();
	OP1 = int8_t(read_pc());
	TMP = reg_r8(OP2);
	if(!((TMP >> (inst_state & 7)) & 1)) {
		PC += OP1;
		next(9);
	} else {
		next(5);
	}
}

void Mcs96Cpu::jbs_brrel8_full()
{
	OP2 = read_pc();
	OP1 = int8_t(read_pc());
	TMP = reg_r8(OP2);
	if((TMP >> (inst_state & 7)) & 1) {
		PC += OP1;
		next(9);
	} else {
		next(5);
	}
}

void Mcs96Cpu::and_direct_3w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = reg_r16(OP1);
	TMP &= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP3, TMP);
	next(5);
}

void Mcs96Cpu::and_immed_3w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = OP1 & reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP3, TMP);
	next(6);
}

void Mcs96Cpu::and_indirect_3w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r16(OP1);
	TMP &= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP3, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(8); // +4 when external
	} else {
		next(7); // +4 when external
	}
}

void Mcs96Cpu::and_indexed_3w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r16(OP1);
	TMP &= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP3, TMP);
	next(OPI & 0x01 ? 8 : 7);
}

void Mcs96Cpu::add_direct_3w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = reg_r16(OP1);
	TMP = do_add(reg_r16(OP2), TMP);
	reg_w16(OP3, TMP);
	next(5);
}

void Mcs96Cpu::add_immed_3w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = do_add(reg_r16(OP2), OP1);
	reg_w16(OP3, TMP);
	next(6);
}

void Mcs96Cpu::add_indirect_3w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_add(reg_r16(OP2), TMP);
	reg_w16(OP3, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(8); // +4 when external
	} else {
		next(7); // +4 when external
	}
}

void Mcs96Cpu::add_indexed_3w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_add(reg_r16(OP2), TMP);
	reg_w16(OP3, TMP);
	next(OPI & 0x01 ? 8 : 7);
}

void Mcs96Cpu::sub_direct_3w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = reg_r16(OP1);
	TMP = do_sub(reg_r16(OP2), TMP);
	reg_w16(OP3, TMP);
	next(5);
}

void Mcs96Cpu::sub_immed_3w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = do_sub(reg_r16(OP2), OP1);
	reg_w16(OP3, TMP);
	next(6);
}

void Mcs96Cpu::sub_indirect_3w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_sub(reg_r16(OP2), TMP);
	reg_w16(OP3, TMP);
	next(OPI & 0x01 ? 8 : 7);
}

void Mcs96Cpu::sub_indexed_3w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_sub(reg_r16(OP2), TMP);
	reg_w16(OP3, TMP);
	next(OPI & 0x01 ? 8 : 7);
}

void Mcs96Cpu::mulu_direct_3w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = reg_r16(OP1);
	TMP *= reg_r16(OP2);
	OP3 &= 0xfc;
	reg_w16(OP3, TMP);
	reg_w16(OP3+2, TMP >> 16);
	next(26);
}

void Mcs96Cpu::mulu_immed_3w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = OP1 * reg_r16(OP2);
	OP3 &= 0xfc;
	reg_w16(OP3, TMP);
	reg_w16(OP3+2, TMP >> 16);
	next(27);
}

void Mcs96Cpu::mulu_indirect_3w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r16(OP1);
	TMP *= reg_r16(OP2);
	OP3 &= 0xfc;
	reg_w16(OP3, TMP);
	reg_w16(OP3+2, TMP >> 16);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(29); // +4 when external
	} else {
		next(28); // +4 when external
	}
}

void Mcs96Cpu::mulu_indexed_3w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r16(OP1);
	TMP *= reg_r16(OP2);
	OP3 &= 0xfc;
	reg_w16(OP3, TMP);
	reg_w16(OP3+2, TMP >> 16);
	next(OPI & 0x01 ? 29 : 28);
}

void Mcs96Cpu::mul_direct_3w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = reg_r16(OP1);
	TMP = int16_t(reg_r16(OP2)) * int16_t(TMP);
	OP3 &= 0xfc;
	reg_w16(OP3, TMP);
	reg_w16(OP3+2, TMP >> 16);
	next(30);
}

void Mcs96Cpu::mul_immed_3w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = int16_t(OP1) * int16_t(reg_r16(OP2));
	OP3 &= 0xfc;
	reg_w16(OP3, TMP);
	reg_w16(OP3+2, TMP >> 16);
	next(31);
}

void Mcs96Cpu::mul_indirect_3w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r16(OP1);
	TMP = int16_t(reg_r16(OP2)) * int16_t(TMP);
	OP3 &= 0xfc;
	reg_w16(OP3, TMP);
	reg_w16(OP3+2, TMP >> 16);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(33); // +4 when external
	} else {
		next(32); // +4 when external
	}
}

void Mcs96Cpu::mul_indexed_3w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r16(OP1);
	TMP = int16_t(reg_r16(OP2)) * int16_t(TMP);
	OP3 &= 0xfc;
	reg_w16(OP3, TMP);
	reg_w16(OP3+2, TMP >> 16);
	next(OPI & 0x01 ? 33 : 32);
}

void Mcs96Cpu::andb_direct_3b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = reg_r8(OP1);
	TMP &= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP3, TMP);
	next(5);
}

void Mcs96Cpu::andb_immed_3b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = OP1 & reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP3, TMP);
	next(5);
}

void Mcs96Cpu::andb_indirect_3b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r8(OP1);
	TMP &= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP3, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(8); // +4 when external
	} else {
		next(7); // +4 when external
	}
}

void Mcs96Cpu::andb_indexed_3b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r8(OP1);
	TMP &= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP3, TMP);
	next(OPI & 0x01 ? 8 : 7);
}

void Mcs96Cpu::addb_direct_3b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = reg_r8(OP1);
	TMP = do_addb(reg_r8(OP2), TMP);
	reg_w8(OP3, TMP);
	next(5);
}

void Mcs96Cpu::addb_immed_3b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = do_addb(reg_r8(OP2), OP1);
	reg_w8(OP3, TMP);
	next(5);
}

void Mcs96Cpu::addb_indirect_3b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_addb(reg_r8(OP2), TMP);
	reg_w8(OP3, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(8); // +4 when external
	} else {
		next(7); // +4 when external
	}
}

void Mcs96Cpu::addb_indexed_3b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_addb(reg_r8(OP2), TMP);
	reg_w8(OP3, TMP);
	next(OPI & 0x01 ? 8 : 7);
}

void Mcs96Cpu::subb_direct_3b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = reg_r8(OP1);
	TMP = do_subb(reg_r8(OP2), TMP);
	reg_w8(OP3, TMP);
	next(5);
}

void Mcs96Cpu::subb_immed_3b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = do_subb(reg_r8(OP2), OP1);
	reg_w8(OP3, TMP);
	next(5);
}

void Mcs96Cpu::subb_indirect_3b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_subb(reg_r8(OP2), TMP);
	reg_w8(OP3, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(8); // +4 when external
	} else {
		next(7); // +4 when external
	}
}

void Mcs96Cpu::subb_indexed_3b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_subb(reg_r8(OP2), TMP);
	reg_w8(OP3, TMP);
	next(OPI & 0x01 ? 8 : 7);
}

void Mcs96Cpu::mulub_direct_3e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = reg_r8(OP1);
	TMP *= reg_r8(OP2);
	reg_w16(OP3, TMP);
	next(18);
}

void Mcs96Cpu::mulub_immed_3e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = OP1 * reg_r8(OP2);
	reg_w16(OP3, TMP);
	next(18);
}

void Mcs96Cpu::mulub_indirect_3e_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r8(OP1);
	TMP *= reg_r8(OP2);
	reg_w16(OP3, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(21); // +4 when external
	} else {
		next(20); // +4 when external
	}
}

void Mcs96Cpu::mulub_indexed_3e_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r8(OP1);
	TMP = reg_r8(OP2);
	reg_w16(OP3, TMP);
	next(OPI & 0x01 ? 21 : 20);
}

void Mcs96Cpu::mulb_direct_3e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = reg_r8(OP1);
	TMP = int8_t(reg_r8(OP2)) * int8_t(TMP);
	reg_w16(OP3, TMP);
	next(22);
}

void Mcs96Cpu::mulb_immed_3e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = int8_t(OP1) * int8_t(reg_r8(OP2));
	reg_w16(OP3, TMP);
	next(22);
}

void Mcs96Cpu::mulb_indirect_3e_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r8(OP1);
	TMP = int8_t(reg_r8(OP2)) * int8_t(TMP);
	reg_w16(OP3, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(25); // +4 when external
	} else {
		next(24); // +4 when external
	}
}

void Mcs96Cpu::mulb_indexed_3e_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP3 = read_pc();
	TMP = any_r8(OP1);
	TMP = int8_t(reg_r8(OP2)) * int8_t(TMP);
	reg_w16(OP3, TMP);
	next(OPI & 0x01 ? 25 : 24);
}

void Mcs96Cpu::and_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r16(OP1);
	TMP &= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	next(4);
}

void Mcs96Cpu::and_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	TMP = OP1 & reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	next(5);
}

void Mcs96Cpu::and_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP &= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::and_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP &= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::add_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r16(OP1);
	TMP = do_add(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	next(4);
}

void Mcs96Cpu::add_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	TMP = do_add(reg_r16(OP2), OP1);
	reg_w16(OP2, TMP);
	next(5);
}

void Mcs96Cpu::add_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_add(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::add_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_add(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::sub_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r16(OP1);
	TMP = do_sub(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	next(4);
}

void Mcs96Cpu::sub_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	TMP = do_sub(reg_r16(OP2), OP1);
	reg_w16(OP2, TMP);
	next(5);
}

void Mcs96Cpu::sub_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_sub(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::sub_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_sub(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::mulu_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP2 &= 0xfc;
	TMP = reg_r16(OP1);
	TMP *= reg_r16(OP2);
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	next(25);
}

void Mcs96Cpu::mulu_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	OP2 &= 0xfc;
	TMP = OP1 * reg_r16(OP2);
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	next(26);
}

void Mcs96Cpu::mulu_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP2 &= 0xfc;
	TMP = any_r16(OP1);
	TMP *= reg_r16(OP2);
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(28); // +4 when external
	} else {
		next(27); // +4 when external
	}
}

void Mcs96Cpu::mulu_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP2 &= 0xfc;
	TMP = any_r16(OP1);
	TMP *= reg_r16(OP2);
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	next(OPI & 0x01 ? 28 : 27);
}

void Mcs96Cpu::mul_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP2 &= 0xfc;
	TMP = reg_r16(OP1);
	TMP = int16_t(reg_r16(OP2)) * int16_t(TMP);
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	next(29);
}

void Mcs96Cpu::mul_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	OP2 &= 0xfc;
	TMP = int16_t(OP1) * int16_t(reg_r16(OP2));
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	next(30);
}

void Mcs96Cpu::mul_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP2 &= 0xfc;
	TMP = any_r16(OP1);
	TMP = int16_t(reg_r16(OP2)) * int16_t(TMP);
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(32); // +4 when external
	} else {
		next(31); // +4 when external
	}
}

void Mcs96Cpu::mul_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP2 &= 0xfc;
	TMP = any_r16(OP1);
	TMP = int16_t(reg_r16(OP2)) * int16_t(TMP);
	reg_w16(OP2, TMP);
	reg_w16(OP2+2, TMP >> 16);
	next(OPI & 0x01 ? 32 : 31);
}

void Mcs96Cpu::andb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r8(OP1);
	TMP &= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::andb_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = OP1 & reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::andb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP &= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::andb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP &= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::addb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r8(OP1);
	TMP = do_addb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::addb_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = do_addb(reg_r8(OP2), OP1);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::addb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_addb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::addb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_addb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::subb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r8(OP1);
	TMP = do_subb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::subb_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = do_subb(reg_r8(OP2), OP1);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::subb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_subb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::subb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_subb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::mulub_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP2 &= 0xfe;
	TMP = reg_r8(OP1);
	TMP *= reg_r8(OP2);
	reg_w16(OP2, TMP);
	next(17);
}

void Mcs96Cpu::mulub_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP2 &= 0xfe;
	TMP = OP1 * reg_r8(OP2);
	reg_w16(OP2, TMP);
	next(17);
}

void Mcs96Cpu::mulub_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP2 &= 0xfe;
	TMP = any_r8(OP1);
	TMP *= reg_r8(OP2);
	reg_w16(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(20); // +4 when external
	} else {
		next(19); // +4 when external
	}
}

void Mcs96Cpu::mulub_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP2 &= 0xfe;
	TMP = any_r8(OP1);
	TMP *= reg_r8(OP2);
	reg_w16(OP2, TMP);
	next(OPI & 0x01 ? 20 : 19);
}

void Mcs96Cpu::mulb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP2 &= 0xfe;
	TMP = reg_r8(OP1);
	TMP = int8_t(reg_r16(OP2)) * int8_t(TMP);
	reg_w16(OP2, TMP);
	next(21);
}

void Mcs96Cpu::mulb_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP2 &= 0xfe;
	TMP = int8_t(OP1) * int8_t(reg_r8(OP2));
	reg_w16(OP2, TMP);
	next(21);
}

void Mcs96Cpu::mulb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP2 &= 0xfe;
	TMP = any_r8(OP1);
	TMP = int8_t(reg_r8(OP2)) * int8_t(TMP);
	reg_w16(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(24); // +4 when external
	} else {
		next(23); // +4 when external
	}
}

void Mcs96Cpu::mulb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP2 &= 0xfe;
	// BYTE operand: any_r16 masks the address down to even (adr &= 0xfffe), so an
	// operand at an ODD address silently became its even neighbour. Every sibling
	// form reads a byte here - 7f mulub indexed_2b, fe7e mulb indirect_2b and
	// fe5f mulb indexed_3e all use any_r8; this one line was the odd man out.
	// Found on the Roland D-70, where 83EE computes
	//   reg91 = 96 + (curve(velocity) - 96) * [patch+0x0C] / 127
	// with `mulb bl, 0c[7a]`. The patch record base is odd-aligned, so the V.Sens
	// multiplier at +0x0C was read as the curve selector at +0x0B = 0, the whole
	// deviation term collapsed and every note came out at the neutral 96 - i.e.
	// the instrument had no velocity response at all.
	TMP = any_r8(OP1);
	TMP = int8_t(reg_r8(OP2)) * int8_t(TMP);
	reg_w16(OP2, TMP);
	next(OPI & 0x01 ? 24 : 23);
}

void Mcs96Cpu::or_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r16(OP1);
	TMP |= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	next(4);
}

void Mcs96Cpu::or_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	TMP = OP1 | reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	next(5);
}

void Mcs96Cpu::or_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP |= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::or_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP |= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::xor_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r16(OP1);
	TMP ^= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	next(4);
}

void Mcs96Cpu::xor_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	TMP = OP1 ^ reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	next(5);
}

void Mcs96Cpu::xor_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP ^= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::xor_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP ^= reg_r16(OP2);
	set_nz16(TMP);
	reg_w16(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::cmp_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r16(OP1);
	do_sub(reg_r16(OP2), TMP);
	next(4);
}

void Mcs96Cpu::cmp_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	do_sub(reg_r16(OP2), OP1);
	next(5);
}

void Mcs96Cpu::cmp_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r16(OP1);
	do_sub(reg_r16(OP2), TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::cmp_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r16(OP1);
	do_sub(reg_r16(OP2), TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::divu_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP2 &= 0xfc;
	PSW &= ~F_V;
	OP1 = reg_r16(OP1);
	if(OP1) {
		TMP = reg_r16(OP2);
		TMP |= reg_r16(OP2+2);
		uint32_t TMP2 = TMP / OP1;
		if(TMP2 > 65535)
			PSW |= F_V|F_VT;
		TMP = TMP % OP1;
		TMP = (TMP2 & 0xffff) | ((TMP & 0xffff) << 16);
		reg_w16(OP2, TMP);
		reg_w16(OP2+2, TMP >> 16);
	}
	next(25);
}

void Mcs96Cpu::divu_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	OP2 &= 0xfc;
	PSW &= ~F_V;
	if(OP1) {
		TMP = reg_r16(OP2);
		TMP |= reg_r16(OP2+2);
		uint32_t TMP2 = TMP / OP1;
		if(TMP2 > 65535)
			PSW |= F_V|F_VT;
		TMP = TMP % OP1;
		TMP = (TMP2 & 0xffff) | ((TMP & 0xffff) << 16);
		reg_w16(OP2, TMP);
		reg_w16(OP2+2, TMP >> 16);
	}
	next(26);
}

void Mcs96Cpu::divu_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP2 &= 0xfc;
	PSW &= ~F_V;
	uint32_t d = any_r16(OP1);
	if(d) {
		TMP = reg_r16(OP2);
		TMP |= reg_r16(OP2+2);
		uint32_t TMP2 = TMP / d;
		if(TMP2 > 65535)
			PSW |= F_V|F_VT;
		TMP = TMP % d;
		TMP = (TMP2 & 0xffff) | ((TMP & 0xffff) << 16);
		reg_w16(OP2, TMP);
		reg_w16(OP2+2, TMP >> 16);
	}
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(29); // +4 when external
	} else {
		next(28); // +4 when external
	}
}

void Mcs96Cpu::divu_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP2 &= 0xfc;
	PSW &= ~F_V;
	uint32_t d = any_r16(OP1);
	if(d) {
		TMP = reg_r16(OP2);
		TMP |= reg_r16(OP2+2);
		uint32_t TMP2 = TMP / d;
		if(TMP2 > 65535)
			PSW |= F_V|F_VT;
		TMP = TMP % d;
		TMP = (TMP2 & 0xffff) | ((TMP & 0xffff) << 16);
		reg_w16(OP2, TMP);
		reg_w16(OP2+2, TMP >> 16);
	}
	next(OPI & 0x01 ? 29 : 28);
}

void Mcs96Cpu::div_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	OP2 &= 0xfc;
	PSW &= ~F_V;
	OP1 = reg_r16(OP1);
	if(OP1) {
		TMP = reg_r16(OP2);
		TMP |= reg_r16(OP2+2);
		int32_t TMP2 = int32_t(TMP) / int16_t(OP1);
		if(TMP2 > 32767 || TMP2 < -32768)
			PSW |= F_V|F_VT;
		TMP = TMP % int16_t(OP1);
		TMP = (TMP2 & 0xffff) | ((TMP & 0xffff) << 16);
		reg_w16(OP2, TMP);
		reg_w16(OP2+2, TMP >> 16);
	}
	next(30);
}

void Mcs96Cpu::div_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	OP2 &= 0xfc;
	PSW &= ~F_V;
	if(OP1) {
		TMP = reg_r16(OP2);
		TMP |= reg_r16(OP2+2);
		int32_t TMP2 = int32_t(TMP) / int16_t(OP1);
		if(TMP2 > 32767 || TMP2 < -32768)
			PSW |= F_V|F_VT;
		TMP = TMP % int16_t(OP1);
		TMP = (TMP2 & 0xffff) | ((TMP & 0xffff) << 16);
		reg_w16(OP2, TMP);
		reg_w16(OP2+2, TMP >> 16);
	}
	next(30);
}

void Mcs96Cpu::div_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	OP2 &= 0xfc;
	PSW &= ~F_V;
	int32_t d = int16_t(any_r16(OP1));
	if(d) {
		TMP = reg_r16(OP2);
		TMP |= reg_r16(OP2+2);
		int32_t TMP2 = int32_t(TMP) / d;
		if(TMP2 > 32767 || TMP2 < -32768)
			PSW |= F_V|F_VT;
		TMP = TMP % d;
		TMP = (TMP2 & 0xffff) | ((TMP & 0xffff) << 16);
		reg_w16(OP2, TMP);
		reg_w16(OP2+2, TMP >> 16);
	}
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(33); // +4 when external
	} else {
		next(32); // +4 when external
	}
}

void Mcs96Cpu::div_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	OP2 &= 0xfc;
	PSW &= ~F_V;
	int32_t d = int16_t(any_r16(OP1));
	if(d) {
		TMP = reg_r16(OP2);
		TMP |= reg_r16(OP2+2);
		int32_t TMP2 = int32_t(TMP) / d;
		if(TMP2 > 32767 || TMP2 < -32768)
			PSW |= F_V|F_VT;
		TMP = TMP % d;
		TMP = (TMP2 & 0xffff) | ((TMP & 0xffff) << 16);
		reg_w16(OP2, TMP);
		reg_w16(OP2+2, TMP >> 16);
	}
	next(OPI & 0x01 ? 33 : 32);
}

void Mcs96Cpu::orb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r8(OP1);
	TMP |= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::orb_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = OP1 | reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::orb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP |= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::orb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP |= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::xorb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r8(OP1);
	TMP ^= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::xorb_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = OP1 ^ reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::xorb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP ^= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::xorb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP ^= reg_r8(OP2);
	set_nz8(TMP);
	reg_w8(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::cmpb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r8(OP1);
	do_subb(reg_r8(OP2), TMP);
	next(4);
}

void Mcs96Cpu::cmpb_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	do_subb(reg_r8(OP2), OP1);
	next(4);
}

void Mcs96Cpu::cmpb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r8(OP1);
	do_subb(reg_r8(OP2), TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::cmpb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r8(OP1);
	do_subb(reg_r8(OP2), TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::divub_direct_2e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	PSW &= ~F_V;
	OP1 = reg_r8(OP1);
	if(OP1) {
		TMP = reg_r16(OP2);
		uint32_t TMP2 = TMP / OP1;
		if(TMP2 > 255)
			PSW |= F_V|F_VT;
		TMP = TMP % OP1;
		TMP = (TMP2 & 0xff) | ((TMP & 0xff) << 8);
		reg_w16(OP2, TMP);
	}
	next(17);
}

void Mcs96Cpu::divub_immed_2e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	PSW &= ~F_V;
	if(OP1) {
		TMP = reg_r16(OP2);
		uint32_t TMP2 = TMP / OP1;
		if(TMP2 > 255)
			PSW |= F_V|F_VT;
		TMP = TMP % OP1;
		TMP = (TMP2 & 0xff) | ((TMP & 0xff) << 8);
		reg_w16(OP2, TMP);
	}
	next(17);
}

void Mcs96Cpu::divub_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	PSW &= ~F_V;
	uint32_t d = any_r8(OP1);
	if(d) {
		TMP = reg_r16(OP2);
		uint32_t TMP2 = TMP / d;
		if(TMP2 > 255)
			PSW |= F_V|F_VT;
		TMP = TMP % d;
		TMP = (TMP2 & 0xff) | ((TMP & 0xff) << 8);
		reg_w16(OP2, TMP);
	}
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(21); // +4 when external
	} else {
		next(20); // +4 when external
	}
}

void Mcs96Cpu::divub_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	PSW &= ~F_V;
	uint32_t d = any_r8(OP1);
	if(d) {
		TMP = reg_r16(OP2);
		uint32_t TMP2 = TMP / d;
		if(TMP2 > 255)
			PSW |= F_V|F_VT;
		TMP = TMP % d;
		TMP = (TMP2 & 0xff) | ((TMP & 0xff) << 8);
		reg_w16(OP2, TMP);
	}
	next(OPI & 0x01 ? 21 : 20);
}

void Mcs96Cpu::divb_direct_2e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	PSW &= ~F_V;
	OP1 = reg_r8(OP1);
	if(OP1) {
		TMP = reg_r16(OP2);
		uint32_t TMP2 = int16_t(TMP) / int8_t(OP1);
		if(int16_t(TMP2) > 127 || int16_t(TMP2) < -128)
			PSW |= F_V|F_VT;
		TMP = int16_t(TMP) % int8_t(OP1);
		TMP = (TMP2 & 0xff) | ((TMP & 0xff) << 8);
		reg_w16(OP2, TMP);
	}
	next(21);
}

void Mcs96Cpu::divb_immed_2e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	PSW &= ~F_V;
	if(OP1) {
		TMP = reg_r16(OP2);
		uint32_t TMP2 = int16_t(TMP) / int8_t(OP1);
		if(int16_t(TMP2) > 127 || int16_t(TMP2) < -128)
			PSW |= F_V|F_VT;
		TMP = int16_t(TMP) % int8_t(OP1);
		TMP = (TMP2 & 0xff) | ((TMP & 0xff) << 8);
		reg_w16(OP2, TMP);
	}
	next(21);
}

void Mcs96Cpu::divb_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	PSW &= ~F_V;
	int32_t d = int8_t(any_r8(OP1));
	if(d) {
		TMP = reg_r16(OP2);
		uint32_t TMP2 = int16_t(TMP) / d;
		if(int16_t(TMP2) > 127 || int16_t(TMP2) < -128)
			PSW |= F_V|F_VT;
		TMP = int16_t(TMP) % d;
		TMP = (TMP2 & 0xff) | ((TMP & 0xff) << 8);
		reg_w16(OP2, TMP);
	}
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(25); // +4 when external
	} else {
		next(24); // +4 when external
	}
}

void Mcs96Cpu::divb_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	PSW &= ~F_V;
	int32_t d = int8_t(any_r8(OP1));
	if(d) {
		TMP = reg_r16(OP2);
		uint32_t TMP2 = int16_t(TMP) / d;
		if(int16_t(TMP2) > 127 || int16_t(TMP2) < -128)
			PSW |= F_V|F_VT;
		TMP = int16_t(TMP) % d;
		TMP = (TMP2 & 0xff) | ((TMP & 0xff) << 8);
		reg_w16(OP2, TMP);
	}
	next(OPI & 0x01 ? 24 : 1);
}

void Mcs96Cpu::ld_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	reg_w16(OP2, reg_r16(OP1));
	next(4);
}

void Mcs96Cpu::ld_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	reg_w16(OP2, OP1);
	next(5);
}

void Mcs96Cpu::ld_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	reg_w16(OP2, any_r16(OP1));
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::ld_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	reg_w16(OP2, any_r16(OP1));
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::addc_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r16(OP1);
	TMP = do_addc(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	next(4);
}

void Mcs96Cpu::addc_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	TMP = do_addc(reg_r16(OP2), OP1);
	reg_w16(OP2, TMP);
	next(5);
}

void Mcs96Cpu::addc_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_addc(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::addc_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_addc(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::subc_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r16(OP1);
	TMP = do_subc(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	next(4);
}

void Mcs96Cpu::subc_immed_2w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	OP2 = read_pc();
	TMP = do_subc(reg_r16(OP2), OP1);
	reg_w16(OP2, TMP);
	next(5);
}

void Mcs96Cpu::subc_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_subc(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::subc_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r16(OP1);
	TMP = do_subc(reg_r16(OP2), TMP);
	reg_w16(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::ldbze_direct_2e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	reg_w16(OP2, uint8_t(reg_r8(OP1)));
	next(4);
}

void Mcs96Cpu::ldbze_immed_2e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	reg_w16(OP2, uint8_t(OP1));
	next(4);
}

void Mcs96Cpu::ldbze_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	reg_w16(OP2, uint8_t(any_r8(OP1)));
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::ldbze_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	reg_w16(OP2, uint8_t(any_r8(OP1)));
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::ldb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	reg_w8(OP2, reg_r8(OP1));
	next(4);
}

void Mcs96Cpu::ldb_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	reg_w8(OP2, OP1);
	next(4);
}

void Mcs96Cpu::ldb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	reg_w8(OP2, any_r8(OP1));
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::ldb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	reg_w8(OP2, any_r8(OP1));
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::addcb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r8(OP1);
	TMP = do_addcb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::addcb_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = do_addcb(reg_r8(OP2), OP1);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::addcb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_addcb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::addcb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_addcb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::subcb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = reg_r8(OP1);
	TMP = do_subcb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::subcb_immed_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	TMP = do_subcb(reg_r8(OP2), OP1);
	reg_w8(OP2, TMP);
	next(4);
}

void Mcs96Cpu::subcb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_subcb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::subcb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	TMP = any_r8(OP1);
	TMP = do_subcb(reg_r8(OP2), TMP);
	reg_w8(OP2, TMP);
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::ldbse_direct_2e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	reg_w16(OP2, int8_t(reg_r8(OP1)));
	next(4);
}

void Mcs96Cpu::ldbse_immed_2e_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	reg_w16(OP2, int8_t(OP1));
	next(4);
}

void Mcs96Cpu::ldbse_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	reg_w16(OP2, int8_t(any_r8(OP1)));
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(7); // +4 when external
	} else {
		next(6); // +4 when external
	}
}

void Mcs96Cpu::ldbse_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	reg_w16(OP2, int8_t(any_r8(OP1)));
	next(OPI & 0x01 ? 7 : 6);
}

void Mcs96Cpu::st_direct_2w_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	reg_w16(OP1, reg_r16(OP2));
	next(4);
}

void Mcs96Cpu::st_indirect_2w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	any_w16(OP1, reg_r16(OP2));
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(8); // +4 when external
	} else {
		next(7); // +4 when external
	}
}

void Mcs96Cpu::st_indexed_2w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	any_w16(OP1, reg_r16(OP2));
	next(OPI & 0x01 ? 8 : 7);
}

void Mcs96Cpu::stb_direct_2b_full()
{
	OP1 = read_pc();
	OP2 = read_pc();
	reg_w8(OP1, reg_r8(OP2));
	next(4);
}

void Mcs96Cpu::stb_indirect_2b_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	OP2 = read_pc();
	any_w8(OP1, reg_r8(OP2));
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 1);
		next(8); // +4 when external
	} else {
		next(7); // +4 when external
	}
}

void Mcs96Cpu::stb_indexed_2b_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else if(OP1 & 0x80)
		OP1 |= 0xff00;
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	OP2 = read_pc();
	any_w8(OP1, reg_r8(OP2));
	next(OPI & 0x01 ? 8 : 7);
}

void Mcs96Cpu::push_direct_1w_full()
{
	OP1 = read_pc();
	TMP = reg_r16(0x18);
	TMP -= 2;
	reg_w16(0x18, TMP);
	OP1 = reg_r16(OP1);
	any_w16(TMP, OP1);
	next(8); // +4 is external sp
}

void Mcs96Cpu::push_immed_1w_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	TMP = reg_r16(0x18);
	TMP -= 2;
	reg_w16(0x18, TMP);
	any_w16(TMP, OP1);
	next(8); // +4 is external sp
}

void Mcs96Cpu::push_indirect_1w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	TMP = reg_r16(0x18);
	TMP -= 2;
	reg_w16(0x18, TMP);
	OP1 = any_r16(OP1);
	any_w16(TMP, OP1);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(12); // +4 when external
	} else {
		next(11); // +4 when external
	}
}

void Mcs96Cpu::push_indexed_1w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else
		OP1 = int8_t(OP1);
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	TMP = reg_r16(0x18);
	TMP -= 2;
	reg_w16(0x18, TMP);
	OP1 = any_r16(OP1);
	any_w16(TMP, OP1);
	next(OPI & 0x01 ? 12 : 11);
}

void Mcs96Cpu::pop_direct_1w_full()
{
	OP1 = read_pc();
	TMP = reg_r16(0x18);
	reg_w16(0x18, TMP+2);
	TMP = any_r16(TMP);
	reg_w16(OP1, TMP);
	next(12); // +2 when external sp
}

void Mcs96Cpu::pop_indirect_1w_full()
{
	OPI = read_pc();
	OP1 = reg_r16(OPI);
	TMP = reg_r16(0x18);
	reg_w16(0x18, TMP+2);
	TMP = any_r16(TMP);
	if((OPI & 0xfe) == 0x18)
		OP1 += 2;
	any_w16(OP1, TMP);
	if(OPI & 0x01) {
		reg_w16(OPI, OP1 + 2);
		next(14); // +4 when external
	} else {
		next(14); // +4 when external
	}
}

void Mcs96Cpu::pop_indexed_1w_full()
{
	OPI = read_pc();
	OP1 = read_pc();
	if(OPI & 0x01) {
		OPI &= 0xfe;
		OP1 |= read_pc() << 8;
	} else
		OP1 = int8_t(OP1);
	if(OPI) {
		OP1 += reg_r16(OPI);
	}
	TMP = reg_r16(0x18);
	reg_w16(0x18, TMP+2);
	TMP = any_r16(TMP);
	any_w16(OP1, TMP);
	next(OPI & 0x01 ? 14 : 14);
}

void Mcs96Cpu::jnst_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(!(PSW & F_ST)) {
		PC += OP1;
		next(8);
	} else {
		PSW &= ~F_VT;
		next(4);
	}
}

void Mcs96Cpu::jnh_rel8_full()
{
	OP1 = int8_t(read_pc());
	if((PSW & (F_C|F_Z)) != F_C) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jgt_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(!(PSW & (F_Z|F_N))) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jnc_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(!(PSW & F_C)) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jnvt_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(!(PSW & F_VT)) {
		PC += OP1;
		next(8);
	} else {
		PSW &= ~F_VT;
		next(4);
	}
}

void Mcs96Cpu::jnv_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(!(PSW & F_V)) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jge_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(!(PSW & F_N)) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jne_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(!(PSW & F_Z)) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jst_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(PSW & F_ST) {
		PC += OP1;
		next(8);
	} else {
		PSW &= ~F_VT;
		next(4);
	}
}

void Mcs96Cpu::jh_rel8_full()
{
	OP1 = int8_t(read_pc());
	if((PSW & (F_C|F_Z)) == F_C) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jle_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(PSW & (F_Z|F_N)) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jc_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(PSW & F_C) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jvt_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(PSW & F_VT) {
		PSW &= ~F_VT;
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jv_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(PSW & F_V) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::jlt_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(PSW & F_N) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::je_rel8_full()
{
	OP1 = int8_t(read_pc());
	if(PSW & F_Z) {
		PC += OP1;
		next(8);
	} else {
		next(4);
	}
}

void Mcs96Cpu::djnz_rrel8_full()
{
	OP2 = read_pc();
	OP1 = int8_t(read_pc());
	TMP = reg_r8(OP2);
	TMP = uint8_t(TMP-1);
	reg_w8(OP2, TMP);
	if(TMP) {
		PC += OP1;
		next(9);
	} else {
		next(5);
	}
}

void Mcs96Cpu::br_indirect_1n_full()
{
	OPI = read_pc() & 0xfe;
	OP1 = reg_r16(OPI);
	PC = OP1;
	next(8);
}

void Mcs96Cpu::ljmp_rel16_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	PC += OP1;
	next(8);
}

void Mcs96Cpu::lcall_rel16_full()
{
	OP1 = read_pc();
	OP1 |= read_pc() << 8;
	TMP = reg_r16(0x18);
	TMP -= 2;
	reg_w16(0x18, TMP);
	any_w16(TMP, PC);
	PC += OP1;
	next(13); // +3 for external sp
}

void Mcs96Cpu::ret_none_full()
{
	TMP = reg_r16(0x18);
	reg_w16(0x18, TMP+2);
	PC = any_r16(TMP);
	next(12); // +4 for external sp
}

void Mcs96Cpu::pushf_none_full()
{
	TMP = reg_r16(0x18);
	TMP -= 2;
	reg_w16(0x18, TMP);
	any_w16(TMP, PSW);
	PSW = 0x0000;
	check_irq();
	next_noirq(8); // +4 for external sp
}

void Mcs96Cpu::popf_none_full()
{
	TMP = reg_r16(0x18);
	reg_w16(0x18, TMP+2);
	PSW = any_r16(TMP);
	check_irq();
	next_noirq(9); // +4 for external sp
}

void Mcs96Cpu::trap_none_full()
{
	TMP = reg_r16(0x18);
	TMP -= 2;
	reg_w16(0x18, TMP);
	any_w16(TMP, PC);
	PC  = any_r16(0x2010);
	next_noirq(21); // +3 for external sp
}

void Mcs96Cpu::clrc_none_full()
{
	PSW &= ~F_C;
	next(4);
}

void Mcs96Cpu::setc_none_full()
{
	PSW |= F_C;
	next(4);
}

void Mcs96Cpu::di_none_full()
{
	PSW &= ~F_I;
	check_irq();
	next_noirq(4);
}

void Mcs96Cpu::ei_none_full()
{
	PSW |= F_I;
	check_irq();
	next_noirq(4);
}

void Mcs96Cpu::clrvt_none_full()
{
	PSW &= ~F_VT;
	next(4);
}

void Mcs96Cpu::nop_none_full()
{
	next(4);
}

void Mcs96Cpu::rst_none_full()
{
	PC = 0x2080;
	next(4);
}

void Mcs96Cpu::fetch_full()
{
	if(irq_requested) {
		int level;
		// Extended bank (INT_PEND1/INT_MASK1, vectors 0x2030) is checked FIRST.
		// Its sources are edge-like and time-critical - serial receive lives here
		// on the Roland D-70 - whereas the classic bank carries the periodic timer
		// interrupts, which are pending so often that checking them first would
		// starve the extended ones almost completely.
		for(level = 7; level >= 0 && !(int_mask1 & pending_irq1 & (1<<level)); level--);
		if(level >= 0) {
			pending_irq1 &= ~(1<<level);
			OP1 = level;
			TMP = reg_r16(0x18);
			TMP -= 2;
			reg_w16(0x18, TMP);
			any_w16(TMP, PC);
			PC = any_r16(0x2030+2*OP1);
		} else {
			for(level = 7; level >= 0 && !(PSW & pending_irq & (1<<level)); level--);
			if(level >= 0) {
				if(level != 7)
					pending_irq &= ~(1<<level);
				OP1 = level;
				TMP = reg_r16(0x18);
				TMP -= 2;
				reg_w16(0x18, TMP);
				any_w16(TMP, PC);
				PC = any_r16(0x2000+2*OP1);
			}
		}
		check_irq();
	}
	PPC = PC;
	OP1 = read_pc();
	if(OP1 == 0xfe) {
		OP1 = read_pc();
		inst_state = 0x100 | OP1;
	} else
		inst_state = OP1;
}

void Mcs96Cpu::fetch_noirq_full()
{
	PPC = PC;
	OP1 = read_pc();
	if(OP1 == 0xfe) {
		OP1 = read_pc();
		inst_state = 0x100 | OP1;
	} else
		inst_state = OP1;
}

