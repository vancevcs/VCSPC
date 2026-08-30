// Copyright (c) 2026- PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#pragma once

#include "Common/CommonTypes.h"

// Just enough MIPS to write a program into PSP memory and call the game's own code with it.
//
// Two things in this fork need that, for the same reason: PPSSPP's hleEnqueueCall sets $a0..$a3
// and nothing else, so it cannot express a call that wants floats in $f12..$f14 (the world query)
// or a fifth integer argument in $t0 (the blip creator). Writing a small program and calling THAT
// sidesteps the limitation, and copies the game's own calling convention rather than assuming one.
//
// Encoders only - no state, no allocation. Where the programs live and how they are called is each
// caller's business; see VCSWorld.h for the ground probe and VCSBlips.h for the route markers.

namespace VCS {

// --- A very small MIPS assembler ----------------------------------------------------------------
//
// Written out as encodings rather than as a blob of hex words, because a blob cannot be reviewed.
// Every form below was checked against the game's own disassembly - e.g. `addiu $sp, $sp, -0x20`
// really is 0x27bdffe0 at 0x08a9e434, and `jal 0x8893460` really is 0x0e224d18 - so the encodings
// are confirmed by the same listing that supplied the address.
inline constexpr u32 kRegZero = 0;
inline constexpr u32 kRegA0 = 4;
inline constexpr u32 kRegA1 = 5;
inline constexpr u32 kRegT0 = 8;
inline constexpr u32 kRegT1 = 9;
inline constexpr u32 kRegT2 = 10;
inline constexpr u32 kRegV0 = 2;
inline constexpr u32 kRegA2 = 6;
inline constexpr u32 kRegA3 = 7;
inline constexpr u32 kRegS0 = 16;
inline constexpr u32 kRegS1 = 17;
inline constexpr u32 kRegS2 = 18;
inline constexpr u32 kRegSP = 29;
inline constexpr u32 kRegRA = 31;
inline constexpr u32 kRegF0 = 0;
inline constexpr u32 kRegF12 = 12;
inline constexpr u32 kRegF13 = 13;
inline constexpr u32 kRegF14 = 14;

inline constexpr u32 IType(u32 op, u32 rs, u32 rt, int imm) {
	return (op << 26) | (rs << 21) | (rt << 16) | (u32)(u16)(s16)imm;
}
inline constexpr u32 Addiu(u32 rt, u32 rs, int imm) { return IType(0x09, rs, rt, imm); }
inline constexpr u32 Sltiu(u32 rt, u32 rs, int imm) { return IType(0x0b, rs, rt, imm); }
inline constexpr u32 Lw(u32 rt, u32 rs, int off) { return IType(0x23, rs, rt, off); }
inline constexpr u32 Sw(u32 rt, u32 rs, int off) { return IType(0x2b, rs, rt, off); }
inline constexpr u32 Lbu(u32 rt, u32 rs, int off) { return IType(0x24, rs, rt, off); }
inline constexpr u32 Sb(u32 rt, u32 rs, int off) { return IType(0x28, rs, rt, off); }
inline constexpr u32 Lwc1(u32 ft, u32 rs, int off) { return IType(0x31, rs, ft, off); }
inline constexpr u32 Swc1(u32 ft, u32 rs, int off) { return IType(0x39, rs, ft, off); }
inline constexpr u32 Beq(u32 rs, u32 rt, int words) { return IType(0x04, rs, rt, words); }
inline constexpr u32 B(int words) { return Beq(kRegZero, kRegZero, words); }
inline constexpr u32 Addu(u32 rd, u32 rs, u32 rt) {
	return (rs << 21) | (rt << 16) | (rd << 11) | 0x21;
}
inline constexpr u32 Move(u32 rd, u32 rs) { return Addu(rd, rs, kRegZero); }
inline constexpr u32 Sll(u32 rd, u32 rt, u32 sa) { return (rt << 16) | (rd << 11) | (sa << 6); }
inline constexpr u32 Jal(u32 target) { return (0x03u << 26) | ((target >> 2) & 0x03ffffffu); }
inline constexpr u32 Jr(u32 rs) { return (rs << 21) | 0x08; }
inline constexpr u32 Nop() { return 0; }

}  // namespace VCS
