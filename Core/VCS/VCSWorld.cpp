// Copyright (c) 2026- PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include <vector>

#include "Core/VCS/VCSWorld.h"

#include "Core/HLE/HLE.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSMemory.h"

namespace VCS {

// --- The block ---------------------------------------------------------------------------------
//
// One allocation holds everything: the handshake words, the sample points, the answers, and the
// program itself. One pointer therefore describes the whole query, which is what lets the program
// take a single argument and lets the host verify the whole thing with one read.
//
//   +0x00  magic          identity, checked every tick - see the savestate note in Install
//   +0x04  requestSeq     bumped by the host when it asks
//   +0x08  doneSeq        copied from requestSeq by the program when it has finished
//   +0x0c  count          how many samples this request carries
//   +0x10  found          scratch byte the game function writes its "hit anything" flag into
//   +0x14  dispatches     host-written: how many requests have actually been sent
//   +0x18  mainThread     host-written: the thread id the game reads its pad on, or -1
//   +0x1c  host           host-written: four-character tag of the syscall that carried the last one
//   +0x1a0 refusedThread  host-written: the thread a refused dispatch attempt was made from
//   +0x1a4 refusedWhy     host-written: four-character tag of why it was refused
//   +0x1a8 refusals       host-written: how many attempts were turned away while a question waited
//   +0x1b0 climbSeq       host-written: bumped to ask the game to climb
//   +0x1b4 climbDone      program-written: copied from climbSeq when the attempt has been made
//   +0x1b8 climbPed       host-written: which ped to climb with
//   +0x1bc climbFound     program-written: whether the game found anything to climb
//   +0x1c0 climbResult    the game's own ClimbResult, filled by CanClimb and consumed by StartClimb
//   +0x200 force          host-written: climb anyway when CanClimb declines
//   +0x204 forced         program-written: whether it did
//   +0x208 forceEntity    host-written: the entity to name as the thing being climbed
//   +0x20c forceTarget    host-written: where to climb to, three floats
//   +0x240 climb code     the second program
//
// Those three are diagnostics rather than mechanism, and they are in PSP MEMORY on purpose: the
// same counters live on the host side for the debugger window, but only what is in the block can
// be read from outside the emulator - which is how this handshake gets debugged without asking
// someone to read numbers off a screen.
//   +0x20  in[8]          x, y, z per sample, 12 bytes each
//   +0x80  out[8]         z, found per sample, 8 bytes each
//   +0xc0  code           the program
static constexpr u32 kBlockMagicOff = 0x00;
static constexpr u32 kBlockSeqOff = 0x04;
static constexpr u32 kBlockDoneOff = 0x08;
static constexpr u32 kBlockCountOff = 0x0c;
static constexpr u32 kBlockFoundOff = 0x10;
static constexpr u32 kBlockDispatchesOff = 0x14;
static constexpr u32 kBlockThreadOff = 0x18;
static constexpr u32 kBlockHostOff = 0x1c;
static constexpr u32 kBlockInOff = 0x20;
static constexpr u32 kBlockInStride = 12;
static constexpr u32 kBlockOutOff = 0x80;
static constexpr u32 kBlockOutStride = 8;
static constexpr u32 kBlockCodeOff = 0xc0;
// The climb request: its own handshake, its own program, and the struct the game fills in.
static constexpr u32 kBlockClimbSeqOff = 0x1b0;
static constexpr u32 kBlockClimbDoneOff = 0x1b4;
static constexpr u32 kBlockClimbPedOff = 0x1b8;
static constexpr u32 kBlockClimbFoundOff = 0x1bc;
static constexpr u32 kBlockClimbResultOff = 0x1c0;   // 0x40 bytes: found at +0, entity +8, target +0x10
// Forcing the climb: the same struct, filled by us instead of by the game's own search.
static constexpr u32 kBlockForceOff = 0x200;
static constexpr u32 kBlockForcedOff = 0x204;
static constexpr u32 kBlockForceEntityOff = 0x208;
static constexpr u32 kBlockForceTargetOff = 0x20c;   // three floats
static constexpr u32 kBlockClimbCodeOff = 0x240;

// The three offsets inside the game's ClimbResult that matter. Named rather than spelled inline,
// because the whole forcing path is these three fields and nothing else.
static constexpr u32 kClimbResultFoundOff = 0x00;
static constexpr u32 kClimbResultEntityOff = 0x08;
static constexpr u32 kClimbResultTargetOff = 0x10;
// Past the program, which is about 40 instructions. Diagnostics only.
static constexpr u32 kBlockRefusedThreadOff = 0x1a0;
static constexpr u32 kBlockRefusedWhyOff = 0x1a4;
static constexpr u32 kBlockRefusalsOff = 0x1a8;
static constexpr u32 kBlockSize = 0x400;

// Spells VCSW. Any value would do; the point is that it is ours and nothing else's, so a block
// that no longer reads it back is a block we no longer own.
static constexpr u32 kBlockMagic = 0x56435357;

static_assert(kBlockInOff + kMaxGroundSamples * kBlockInStride <= kBlockOutOff,
              "sample points overrun the results area");
static_assert(kBlockOutOff + kMaxGroundSamples * kBlockOutStride <= kBlockCodeOff,
              "results overrun the program");

// How long to wait for an answer before giving up on it. Generous - the normal turnaround is one
// tick - because the only thing this protects against is a request that will never be answered at
// all, and abandoning a slow one early would put two questions in flight at once.
static constexpr u64 kRequestTimeoutTicks = 120;

static u32 g_block = 0;
// Why there is no block, for the debugger. "Not installed" with no reason cost a round trip
// already: every branch below that declines to install now says which one it was.
static const char *g_status = "not started";
static u32 g_seq = 0;          // last request we made
static int g_pending = 0;      // sample count of the outstanding request
static u64 g_requests = 0;
static u64 g_answers = 0;
static u64 g_abandoned = 0;
static u64 g_requestTick = 0;
static u64 g_answerTick = 0;

// A prepared request waiting for a syscall to carry it into the game. See the comment on
// WorldQueryDispatch for why a request cannot simply be sent when it is made.
static bool g_wantDispatch = false;
static u32 g_dispatchCodeOff = 0;      // which program the pending call runs
static u32 g_climbSeq = 0;
static bool g_climbPending = false;
static bool g_climbAnswered = false;
static bool g_climbFound = false;
static bool g_climbForced = false;
static u64 g_climbRequests = 0;
static u64 g_prepareTick = 0;
static u64 g_dispatches = 0;
static u64 g_refusals = 0;
static const char *g_lastHost = nullptr;
static const char *g_lastRefusal = nullptr;

// The thread the game runs its main loop on, learned from the pad read. Nothing is dispatched
// until this is known, and nothing is dispatched from any other thread.
static SceUID g_mainThread = -1;
static int g_resultCount = 0;
static VCSGroundResult g_results[kMaxGroundSamples];

// --- A very small MIPS assembler ----------------------------------------------------------------
//
// Written out as encodings rather than as a blob of hex words, because a blob cannot be reviewed.
// Every form below was checked against the game's own disassembly - e.g. `addiu $sp, $sp, -0x20`
// really is 0x27bdffe0 at 0x08a9e434, and `jal 0x8893460` really is 0x0e224d18 - so the encodings
// are confirmed by the same listing that supplied the address.
static constexpr u32 kRegZero = 0;
static constexpr u32 kRegA0 = 4;
static constexpr u32 kRegA1 = 5;
static constexpr u32 kRegT0 = 8;
static constexpr u32 kRegT1 = 9;
static constexpr u32 kRegT2 = 10;
static constexpr u32 kRegS0 = 16;
static constexpr u32 kRegS1 = 17;
static constexpr u32 kRegS2 = 18;
static constexpr u32 kRegSP = 29;
static constexpr u32 kRegRA = 31;
static constexpr u32 kRegF0 = 0;
static constexpr u32 kRegF12 = 12;
static constexpr u32 kRegF13 = 13;
static constexpr u32 kRegF14 = 14;

static constexpr u32 IType(u32 op, u32 rs, u32 rt, int imm) {
	return (op << 26) | (rs << 21) | (rt << 16) | (u32)(u16)(s16)imm;
}
static constexpr u32 Addiu(u32 rt, u32 rs, int imm) { return IType(0x09, rs, rt, imm); }
static constexpr u32 Sltiu(u32 rt, u32 rs, int imm) { return IType(0x0b, rs, rt, imm); }
static constexpr u32 Lw(u32 rt, u32 rs, int off) { return IType(0x23, rs, rt, off); }
static constexpr u32 Sw(u32 rt, u32 rs, int off) { return IType(0x2b, rs, rt, off); }
static constexpr u32 Lbu(u32 rt, u32 rs, int off) { return IType(0x24, rs, rt, off); }
static constexpr u32 Sb(u32 rt, u32 rs, int off) { return IType(0x28, rs, rt, off); }
static constexpr u32 Lwc1(u32 ft, u32 rs, int off) { return IType(0x31, rs, ft, off); }
static constexpr u32 Swc1(u32 ft, u32 rs, int off) { return IType(0x39, rs, ft, off); }
static constexpr u32 Beq(u32 rs, u32 rt, int words) { return IType(0x04, rs, rt, words); }
static constexpr u32 B(int words) { return Beq(kRegZero, kRegZero, words); }
static constexpr u32 Addu(u32 rd, u32 rs, u32 rt) {
	return (rs << 21) | (rt << 16) | (rd << 11) | 0x21;
}
static constexpr u32 Move(u32 rd, u32 rs) { return Addu(rd, rs, kRegZero); }
static constexpr u32 Sll(u32 rd, u32 rt, u32 sa) { return (rt << 16) | (rd << 11) | (sa << 6); }
static constexpr u32 Jal(u32 target) { return (0x03u << 26) | ((target >> 2) & 0x03ffffffu); }
static constexpr u32 Jr(u32 rs) { return (rs << 21) | 0x08; }
static constexpr u32 Nop() { return 0; }

// The program itself.
//
//   $a0  the block, on entry
//   $s0  the block, kept across the call - the game function is free to clobber $t* and $a*
//   $s1  the sample index
//   $s2  the sample count
//
// A note on the loop guard: `count` lives in PSP memory, which a savestate can restore from
// another session, so it is not trusted. An unsigned compare against the maximum rejects both a
// too-large count and a negative one, and a bad block therefore does nothing rather than looping
// inside game code forever - the one failure here that would be a hang rather than a wrong number.
static std::vector<u32> BuildProgram() {
	std::vector<u32> code;
	const auto emit = [&code](u32 word) { code.push_back(word); };

	emit(Addiu(kRegSP, kRegSP, -32));
	emit(Sw(kRegRA, kRegSP, 28));
	emit(Sw(kRegS0, kRegSP, 24));
	emit(Sw(kRegS1, kRegSP, 20));
	emit(Sw(kRegS2, kRegSP, 16));
	emit(Move(kRegS0, kRegA0));
	emit(Lw(kRegS2, kRegS0, kBlockCountOff));
	emit(Sltiu(kRegT0, kRegS2, kMaxGroundSamples + 1));

	// Patched once the rest of the program is laid out, because both of these jump forward.
	const size_t guardIndex = code.size();
	emit(Beq(kRegT0, kRegZero, 0));
	emit(Nop());
	emit(Move(kRegS1, kRegZero));

	const size_t loopIndex = code.size();
	const size_t exitIndex = code.size();
	emit(Beq(kRegS1, kRegS2, 0));   // patched below
	emit(Nop());

	// in[i] sits at block + kBlockInOff + i * 12, and 12 is (i*2 + i) * 4 - three instructions
	// instead of a multiply, which is how the game's own code does this too.
	emit(Sll(kRegT0, kRegS1, 1));
	emit(Addu(kRegT0, kRegT0, kRegS1));
	emit(Sll(kRegT0, kRegT0, 2));
	emit(Addu(kRegT0, kRegT0, kRegS0));
	emit(Lwc1(kRegF12, kRegT0, kBlockInOff + 0));
	emit(Lwc1(kRegF13, kRegT0, kBlockInOff + 4));
	emit(Lwc1(kRegF14, kRegT0, kBlockInOff + 8));

	// The out-parameter, cleared first: a callee that returns without touching it then reads as
	// "found nothing" rather than as whatever the previous sample left there.
	emit(Sb(kRegZero, kRegS0, kBlockFoundOff));
	emit(Addiu(kRegA0, kRegS0, kBlockFoundOff));
	emit(Jal(kVCSFindGroundZFor3DCoord));
	emit(Nop());

	// out[i] = block + kBlockOutOff + i * 8. Recomputed after the call rather than kept in a
	// register, because $t* does not survive a call.
	emit(Sll(kRegT1, kRegS1, 3));
	emit(Addu(kRegT1, kRegT1, kRegS0));
	emit(Swc1(kRegF0, kRegT1, kBlockOutOff + 0));
	emit(Lbu(kRegT2, kRegS0, kBlockFoundOff));
	emit(Sw(kRegT2, kRegT1, kBlockOutOff + 4));

	emit(Addiu(kRegS1, kRegS1, 1));
	emit(B(0));   // patched below
	emit(Nop());

	const size_t doneIndex = code.size();
	// The handshake. Written LAST, after every answer is in place, so the host can never see a
	// sequence number that claims results which are not there yet.
	emit(Lw(kRegT0, kRegS0, kBlockSeqOff));
	emit(Sw(kRegT0, kRegS0, kBlockDoneOff));
	emit(Lw(kRegRA, kRegSP, 28));
	emit(Lw(kRegS0, kRegSP, 24));
	emit(Lw(kRegS1, kRegSP, 20));
	emit(Lw(kRegS2, kRegSP, 16));
	emit(Jr(kRegRA));
	emit(Addiu(kRegSP, kRegSP, 32));

	// Branch offsets are in words, counted from the instruction AFTER the delay slot.
	code[guardIndex] = Beq(kRegT0, kRegZero, (int)(doneIndex - (guardIndex + 1)));
	code[exitIndex] = Beq(kRegS1, kRegS2, (int)(doneIndex - (exitIndex + 1)));
	const size_t backIndex = doneIndex - 2;
	code[backIndex] = B((int)(loopIndex - (backIndex + 1)));
	return code;
}

// The second program: ask the game whether there is anything to climb, and if there is, climb it.
//
// Both calls in ONE dispatch, deliberately. CanClimb fills a struct that StartClimb consumes, and
// putting a frame boundary between them would mean holding the game's own answer across a frame in
// which the player can move - so the ledge it found and the ledge it climbs could differ.
//
//   $a0  the block, on entry
//   $s0  the block, kept across both calls
//
// The result struct is the game's, not ours: its first byte is the found flag, the entity sits at
// +0x08 and the target at +0x10. We only read the flag; StartClimb reads the rest.
//
// ---------------------------------------------------------------------------------------------
// FORCING IT, when the game's own search says no.
//
// StartClimb never asks where its struct came from. CanClimb only FILLS one - found, entity,
// target - and the two are separate functions with a plain pointer between them. So when the
// search declines a ledge our own probe was happy with, this fills the struct itself and calls
// StartClimb on it anyway. The animation, the ped motion and the landing are still the game's.
//
// This is the "fallback if the search ever refuses a ledge our own probe is happy with" that
// kVCSPedSetClimbTarget was written down for.
//
// The ENTITY is the honest weak point. StartClimb hands entity and target to 0x0890f6ec, which
// takes a reference on the entity and stores the target RELATIVE TO IT - so a real entity is what
// the game expects, and getting one means calling ProcessVerticalLine (which reports what it hit,
// not just how high) rather than the FindGroundZFor3DCoord wrapper the probe uses. That call takes
// eleven arguments, three of them past the eight this build passes in registers, and its stack
// layout is not something to guess at - see the corrupt-stack crash in docs/VCS_ADDRESSES.md.
//
// So the entity is a HOST-SUPPLIED field. Today the host passes 0, which tests the thing actually
// worth testing first - whether a forced StartClimb animates at all - and costs nothing to change
// later: when a real entity can be fetched, only the host changes and this program does not.
static std::vector<u32> BuildClimbProgram() {
	std::vector<u32> code;
	const auto emit = [&code](u32 word) { code.push_back(word); };

	emit(Addiu(kRegSP, kRegSP, -32));
	emit(Sw(kRegRA, kRegSP, 28));
	emit(Sw(kRegS0, kRegSP, 24));
	emit(Move(kRegS0, kRegA0));

	// CanClimb(ped, &result)
	emit(Lw(kRegA0, kRegS0, kBlockClimbPedOff));
	emit(Addiu(kRegA1, kRegS0, kBlockClimbResultOff));
	emit(Jal(kVCSPedCanClimb));
	emit(Nop());

	// Report what it found before acting on it, so a "no" is still an answer rather than silence.
	emit(Lbu(kRegT0, kRegS0, kBlockClimbResultOff));
	emit(Sw(kRegT0, kRegS0, kBlockClimbFoundOff));

	const size_t skipIndex = code.size();
	emit(Beq(kRegT0, kRegZero, 0));   // patched below, to the forcing block
	emit(Nop());

	// StartClimb(ped, &result) - the game found it, so the struct is entirely its own.
	emit(Lw(kRegA0, kRegS0, kBlockClimbPedOff));
	emit(Jal(kVCSPedStartClimb));
	emit(Addiu(kRegA1, kRegS0, kBlockClimbResultOff));   // delay slot: computed before the jump

	const size_t overIndex = code.size();
	emit(B(0));   // patched below: over the forcing block, which is not for this path
	emit(Nop());

	// --- forcing ------------------------------------------------------------------------------
	// Reached only when CanClimb declined. Everything above has already reported that it did, so
	// the host still learns the game's own answer whether or not this then overrides it.
	const size_t forceIndex = code.size();
	emit(Lw(kRegT1, kRegS0, kBlockForceOff));
	const size_t noForceIndex = code.size();
	emit(Beq(kRegT1, kRegZero, 0));   // patched below
	emit(Nop());

	// Fill the struct the game would have filled: found, then the entity and the target. Only
	// these three are touched - whatever CanClimb left in the rest of it is left alone, since it
	// is the shape the game itself hands to StartClimb.
	emit(Addiu(kRegT2, kRegZero, 1));
	emit(Sb(kRegT2, kRegS0, kBlockClimbResultOff + kClimbResultFoundOff));
	emit(Sw(kRegT2, kRegS0, kBlockForcedOff));
	emit(Lw(kRegT2, kRegS0, kBlockForceEntityOff));
	emit(Sw(kRegT2, kRegS0, kBlockClimbResultOff + kClimbResultEntityOff));
	emit(Lw(kRegT2, kRegS0, kBlockForceTargetOff + 0));
	emit(Sw(kRegT2, kRegS0, kBlockClimbResultOff + kClimbResultTargetOff + 0));
	emit(Lw(kRegT2, kRegS0, kBlockForceTargetOff + 4));
	emit(Sw(kRegT2, kRegS0, kBlockClimbResultOff + kClimbResultTargetOff + 4));
	emit(Lw(kRegT2, kRegS0, kBlockForceTargetOff + 8));
	emit(Sw(kRegT2, kRegS0, kBlockClimbResultOff + kClimbResultTargetOff + 8));

	emit(Lw(kRegA0, kRegS0, kBlockClimbPedOff));
	emit(Jal(kVCSPedStartClimb));
	emit(Addiu(kRegA1, kRegS0, kBlockClimbResultOff));   // delay slot

	const size_t doneIndex = code.size();
	emit(Lw(kRegT0, kRegS0, kBlockClimbSeqOff));
	emit(Sw(kRegT0, kRegS0, kBlockClimbDoneOff));
	emit(Lw(kRegRA, kRegSP, 28));
	emit(Lw(kRegS0, kRegSP, 24));
	emit(Jr(kRegRA));
	emit(Addiu(kRegSP, kRegSP, 32));

	code[skipIndex] = Beq(kRegT0, kRegZero, (int)(forceIndex - (skipIndex + 1)));
	code[overIndex] = B((int)(doneIndex - (overIndex + 1)));
	code[noForceIndex] = Beq(kRegT1, kRegZero, (int)(doneIndex - (noForceIndex + 1)));
	return code;
}

bool WorldQueryReady() {
	return g_block != 0;
}

void InstallWorldQuery() {
	if (!IsActive()) {
		return;
	}
	if (g_block) {
		// Cheap identity check, every tick. A savestate carries the game's memory AND the
		// allocator that handed us the block, but not this file's idea of where the block is - so
		// loading a state saved before the block existed leaves g_block pointing at whatever now
		// occupies that address. Reading our own magic back is what tells the two apart, and it
		// is one compare.
		if (ReadU32(g_block + kBlockMagicOff).value_or(0) == kBlockMagic) {
			return;
		}
		g_block = 0;
		g_status = "the block was lost - reinstalling";
	}

	// Is the game code actually there, and is it the build these addresses were measured on? Same
	// gate the fire hook uses, and for the same reason: VCS::Init runs before the EBOOT is loaded,
	// so everything installs from Tick and has to be able to say "not yet".
	//
	// Read at +4 and +8, never at +0 - see kVCSFindGroundZOp2 for why the function's own first
	// instruction is the one word here that cannot be trusted.
	if (ReadU32(kVCSFindGroundZFor3DCoord + 4).value_or(0) != kVCSFindGroundZOp2 ||
	    ReadU32(kVCSFindGroundZFor3DCoord + 8).value_or(0) != kVCSFindGroundZOp3) {
		g_status = "waiting for the game's collision code";
		return;
	}
	if (ReadU32(kVCSPedCanClimb + 4).value_or(0) != kVCSPedCanClimbOp2 ||
	    ReadU32(kVCSPedStartClimb + 4).value_or(0) != kVCSPedStartClimbOp2) {
		g_status = "waiting for the game's climb code";
		return;
	}

	// Alloc takes the size by reference and rounds it up to the allocator's grain; we only ever
	// need the address back.
	u32 size = kBlockSize;
	const u32 block = userMemory.Alloc(size, true, "VCS world query");
	if (block == (u32)-1) {
		g_status = "no room in the game's memory partition";
		return;
	}

	// `jal` reaches 256 MB, and everything on the PSP's user side shares one such region - but a
	// program that jumps to the wrong place is a crash inside the game rather than a bad answer,
	// so it is worth one comparison to be sure instead of one comment to say it should be fine.
	const u32 codeAddr = block + kBlockCodeOff;
	if (((codeAddr + 4) & 0xf0000000u) != (kVCSFindGroundZFor3DCoord & 0xf0000000u)) {
		userMemory.Free(block);
		g_status = "the block landed out of jal range";
		return;
	}

	const std::vector<u32> code = BuildProgram();
	const std::vector<u32> climbCode = BuildClimbProgram();
	if (kBlockCodeOff + code.size() * sizeof(u32) > kBlockClimbSeqOff ||
	    kBlockClimbCodeOff + climbCode.size() * sizeof(u32) > kBlockSize) {
		userMemory.Free(block);
		g_status = "the program outgrew its block";
		return;
	}

	bool ok = WriteU32(block + kBlockSeqOff, 0) && WriteU32(block + kBlockDoneOff, 0) &&
	          WriteU32(block + kBlockCountOff, 0) && WriteU32(block + kBlockDispatchesOff, 0) &&
	          WriteU32(block + kBlockThreadOff, (u32)-1) && WriteU32(block + kBlockHostOff, 0) &&
	          WriteU32(block + kBlockRefusedThreadOff, (u32)-1) &&
	          WriteU32(block + kBlockRefusedWhyOff, 0) && WriteU32(block + kBlockRefusalsOff, 0);
	for (size_t i = 0; ok && i < code.size(); i++) {
		ok = WriteU32(codeAddr + (u32)i * 4, code[i]);
	}
	ok = ok && WriteU32(block + kBlockClimbSeqOff, 0) && WriteU32(block + kBlockClimbDoneOff, 0) &&
	     WriteU32(block + kBlockClimbPedOff, 0) && WriteU32(block + kBlockClimbFoundOff, 0);
	// The forcing terms too. Every request writes all of them before it bumps the sequence number,
	// so this is belt and braces - but a block whose program could run against terms nobody set is
	// exactly the state the magic word exists to make impossible, and half-initialising it here
	// would leave the other half to chance.
	ok = ok && WriteU32(block + kBlockForceOff, 0) && WriteU32(block + kBlockForcedOff, 0) &&
	     WriteU32(block + kBlockForceEntityOff, 0) &&
	     WriteFloat(block + kBlockForceTargetOff + 0, 0.0f) &&
	     WriteFloat(block + kBlockForceTargetOff + 4, 0.0f) &&
	     WriteFloat(block + kBlockForceTargetOff + 8, 0.0f);
	for (size_t i = 0; ok && i < climbCode.size(); i++) {
		ok = WriteU32(block + kBlockClimbCodeOff + (u32)i * 4, climbCode[i]);
	}
	// The magic goes down last, so a half-written block is never mistaken for a usable one.
	ok = ok && WriteU32(block + kBlockMagicOff, kBlockMagic);
	if (!ok) {
		userMemory.Free(block);
		g_status = "could not write the block";
		return;
	}

	// The JIT may already have compiled whatever was in this memory before we owned it. Same
	// hazard the fire hook documents at its patch site, and the same fix.
	currentMIPS->InvalidateICache(codeAddr, (u32)code.size() * 4);
	currentMIPS->InvalidateICache(block + kBlockClimbCodeOff, (u32)climbCode.size() * 4);

	g_block = block;
	g_status = "installed";
	g_seq = 0;
	g_pending = 0;
	g_resultCount = 0;
	g_climbSeq = 0;
	g_climbPending = false;
	g_climbAnswered = false;
	g_climbFound = false;
	g_climbForced = false;
	INFO_LOG(Log::HLE, "VCS: world query installed at %08x (%d + %d instructions)", block,
	         (int)code.size(), (int)climbCode.size());
}

void RemoveWorldQuery() {
	if (!g_block) {
		return;
	}
	// Wipe the magic before handing the memory back, so a stale g_block in some other copy of
	// this state cannot recognise it.
	WriteU32(g_block + kBlockMagicOff, 0);
	userMemory.Free(g_block);
	g_block = 0;
	g_status = "not started";
	g_seq = 0;
	g_pending = 0;
	g_requests = 0;
	g_answers = 0;
	g_abandoned = 0;
	g_dispatches = 0;
	g_refusals = 0;
	g_lastRefusal = nullptr;
	g_wantDispatch = false;
	g_mainThread = -1;
	g_lastHost = nullptr;
	g_resultCount = 0;
}

bool RequestGroundZ(const VCSGroundSample *samples, int count) {
	if (!g_block || count <= 0 || count > kMaxGroundSamples) {
		return false;
	}
	// One at a time. Overwriting the inputs of a call that has not run yet would answer the new
	// question with the old points, or worse, a mix of the two.
	if (g_pending > 0) {
		return false;
	}

	for (int i = 0; i < count; i++) {
		const u32 at = g_block + kBlockInOff + (u32)i * kBlockInStride;
		if (!WriteFloat(at + 0, samples[i].x) || !WriteFloat(at + 4, samples[i].y) ||
		    !WriteFloat(at + 8, samples[i].z)) {
			return false;
		}
	}
	if (!WriteU32(g_block + kBlockCountOff, (u32)count)) {
		return false;
	}
	// The sequence number is the request. It goes down after the points, for the same reason the
	// program writes it after the answers.
	const u32 seq = g_seq + 1;
	if (!WriteU32(g_block + kBlockSeqOff, seq)) {
		return false;
	}

	g_seq = seq;
	g_pending = count;
	g_wantDispatch = true;
	g_dispatchCodeOff = kBlockCodeOff;
	g_prepareTick = GetTickCount();
	g_requestTick = g_prepareTick;
	g_requests++;
	return true;
}

void WorldQueryNoteMainThread() {
	if (!g_block) {
		return;
	}
	const SceUID cur = __KernelGetCurThread();
	if (cur != g_mainThread) {
		g_mainThread = cur;
		WriteU32(g_block + kBlockThreadOff, (u32)cur);
	}
}

// Hand the prepared request to the game. Called from a HANDFUL OF SYSCALLS, and that is the whole
// point of this function existing separately from RequestGroundZ.
//
// THIS COST A CRASH, so the rule is written out rather than assumed. hleEnqueueCall means "call
// this after THIS HLE call finishes" - it sets a flag that hleFinishSyscall acts on. Called from
// anywhere that is not inside a syscall, the flag survives until some unrelated syscall finishes,
// and the call is then set up on whatever thread happened to make it, at whatever point that
// thread had reached. The first version of this enqueued from VCS::Tick, which runs on the vblank
// timing event and is not a syscall at all. The result, in play, was:
//
//     E[MemMap]: Bad memory access detected and ignored: a4c8ba03 at 089be2d8
//     E[HLE]:    Corrupt stack on HLE mips call return: 28fefefe
//
// - 0xfefefefe being the fill pattern of a stack nobody had used yet, i.e. the call had been
// planted on a thread that was not running the game.
//
// So: only from inside a syscall, only from a syscall that neither blocks nor reschedules (a
// queued call set up immediately before a context switch is the same class of problem), and only
// on the thread the game runs its main loop on. The hosts are chosen from the game's own import
// table: sceDisplayIsVblank, sceDisplayGetCurrentHcount and sceKernelPowerTick are all pure reads
// or stubs, and VCS imports all three. Whichever fires first in a frame carries the request; the
// others find nothing to do.
// Four characters, so a tag reads as text in a memory dump.
static u32 Tag(const char *s) {
	u32 tag = 0;
	for (int i = 0; i < 4 && s[i]; i++) {
		tag |= (u32)(u8)s[i] << (i * 8);
	}
	return tag;
}

void WorldQueryDispatch(const char *host) {
	if (!g_block || !g_wantDispatch) {
		return;   // nothing waiting: the overwhelmingly common case, and not worth recording
	}

	// Everything from here on happens with a question waiting, so a refusal is worth writing down.
	const SceUID cur = __KernelGetCurThread();
	const char *refusal = nullptr;

	// AN INTERRUPT IS NOT A THREAD. A game that flips inside its vblank handler - VCS imports
	// sceKernelRegisterSubIntrHandler, so this is not hypothetical - would otherwise have the call
	// planted on the interrupt's own context, which is the fresh-stack signature the first crash
	// left behind (0xfefefefe). PPSSPP's own flip path checks the same thing before it dares
	// delay a thread.
	if (__IsInInterrupt()) {
		refusal = "intr";
	} else if (g_mainThread == -1) {
		refusal = "noth";
	} else if (cur != g_mainThread) {
		refusal = "thrd";
	}
	if (refusal) {
		g_refusals++;
		g_lastRefusal = refusal;
		WriteU32(g_block + kBlockRefusedThreadOff, (u32)cur);
		WriteU32(g_block + kBlockRefusedWhyOff, Tag(refusal));
		WriteU32(g_block + kBlockRefusalsOff, (u32)g_refusals);
		return;
	}

	const u32 arg = g_block;
	hleEnqueueCall(g_block + g_dispatchCodeOff, 1, &arg);

	g_wantDispatch = false;
	g_requestTick = GetTickCount();
	g_dispatches++;
	g_lastHost = host;

	// Mirror the two facts that matter into the block, where a tool outside the emulator can see
	// them.
	WriteU32(g_block + kBlockDispatchesOff, (u32)g_dispatches);
	WriteU32(g_block + kBlockHostOff, Tag(host));
}

bool RequestNativeClimb(u32 ped, const VCSClimbForce *force) {
	if (!g_block || !ped || g_climbPending) {
		return false;
	}
	// A climb takes the dispatch slot off any probe that was waiting for it. The probe is a
	// question asked every other frame and asking it again costs nothing; the climb is a press.
	if (g_pending > 0) {
		g_pending = 0;
		g_resultCount = 0;
	}
	// What to do if the game's own search declines. Written before the sequence number, like every
	// other input here, so the program can never see a request whose terms are half in place.
	const bool wantForce = force != nullptr;
	if (!WriteU32(g_block + kBlockForceOff, wantForce ? 1 : 0) ||
	    !WriteU32(g_block + kBlockForcedOff, 0) ||
	    !WriteU32(g_block + kBlockForceEntityOff, wantForce ? force->entity : 0) ||
	    !WriteFloat(g_block + kBlockForceTargetOff + 0, wantForce ? force->target[0] : 0.0f) ||
	    !WriteFloat(g_block + kBlockForceTargetOff + 4, wantForce ? force->target[1] : 0.0f) ||
	    !WriteFloat(g_block + kBlockForceTargetOff + 8, wantForce ? force->target[2] : 0.0f)) {
		return false;
	}

	const u32 seq = g_climbSeq + 1;
	if (!WriteU32(g_block + kBlockClimbPedOff, ped) ||
	    !WriteU32(g_block + kBlockClimbFoundOff, 0) ||
	    !WriteU32(g_block + kBlockClimbSeqOff, seq)) {
		return false;
	}
	g_climbSeq = seq;
	g_climbPending = true;
	g_climbAnswered = false;
	g_climbFound = false;
	g_climbForced = false;
	g_climbRequests++;
	g_wantDispatch = true;
	g_dispatchCodeOff = kBlockClimbCodeOff;
	g_prepareTick = GetTickCount();
	g_requestTick = g_prepareTick;
	return true;
}

bool NativeClimbPending() {
	return g_climbPending;
}

bool NativeClimbForced() {
	return g_climbForced;
}

bool NativeClimbAnswered(bool *found) {
	if (found) {
		*found = g_climbFound;
	}
	return g_climbAnswered;
}

void NativeClimbStats(u64 *requests, bool *lastFound) {
	if (requests) *requests = g_climbRequests;
	if (lastFound) *lastFound = g_climbFound;
}

void WorldQueryTick() {
	// The climb answer, checked before the probe's - it is the one somebody is waiting on.
	if (g_block && g_climbPending) {
		if (ReadU32(g_block + kBlockClimbDoneOff).value_or(0) == g_climbSeq) {
			g_climbFound = ReadU32(g_block + kBlockClimbFoundOff).value_or(0) != 0;
			// Reported separately from `found`: the game still said no, and this records that we
			// went ahead regardless. Collapsing the two would lose the only evidence of which
			// climbs were the game's own choice.
			g_climbForced = ReadU32(g_block + kBlockForcedOff).value_or(0) != 0;
			g_climbPending = false;
			g_climbAnswered = true;
		} else if (GetTickCount() - g_requestTick > kRequestTimeoutTicks) {
			g_climbPending = false;
			g_climbAnswered = true;
			g_climbFound = false;
			g_climbForced = false;
			g_abandoned++;
		}
	}

	// A request nobody carried. Dropping it lets the next probe through, instead of leaving the
	// one-at-a-time rule blocking every future question on a request that never left the building.
	if (g_wantDispatch && GetTickCount() - g_prepareTick > kRequestTimeoutTicks) {
		g_wantDispatch = false;
		g_pending = 0;
		g_resultCount = 0;   // an abandoned question has no answer, not a stale one
		g_abandoned++;
	}

	if (!g_block || g_pending <= 0) {
		return;
	}

	// Does the block still describe the question we asked? A savestate is how it stops doing so:
	// PSP memory and the queued call are both restored with the state, but this file's idea of
	// which request is outstanding is not, so loading a state taken before the request leaves us
	// waiting for an answer to a question the world has never been asked. Left unhandled that is
	// permanent - the request never completes, so no further one is ever allowed, and vaulting
	// quietly stops working for the rest of the session.
	if (ReadU32(g_block + kBlockSeqOff).value_or(0) != g_seq) {
		g_pending = 0;
		g_resultCount = 0;
		g_abandoned++;
		return;
	}

	if (ReadU32(g_block + kBlockDoneOff).value_or(0) != g_seq) {
		// Still queued, or running. Unless it has been so long that it plainly is not coming, in
		// which case let the next question through rather than blocking on this one forever.
		if (GetTickCount() - g_requestTick > kRequestTimeoutTicks) {
			g_pending = 0;
			g_resultCount = 0;
			g_abandoned++;
		}
		return;
	}

	for (int i = 0; i < g_pending; i++) {
		const u32 at = g_block + kBlockOutOff + (u32)i * kBlockOutStride;
		g_results[i].z = ReadFloat(at + 0).value_or(0.0f);
		g_results[i].found = ReadU32(at + 4).value_or(0) != 0;
	}
	g_resultCount = g_pending;
	g_pending = 0;
	g_answers++;
	g_answerTick = GetTickCount();
}

bool GroundZOutstanding() {
	return g_wantDispatch || g_pending > 0;
}

bool GroundZReady() {
	return g_resultCount > 0 && g_pending == 0;
}

int LatestGroundZ(VCSGroundResult *out, int max) {
	const int n = g_resultCount < max ? g_resultCount : max;
	for (int i = 0; i < n; i++) {
		out[i] = g_results[i];
	}
	return n;
}

u64 GroundZAgeTicks() {
	if (!g_answers) {
		return 0;
	}
	const u64 now = GetTickCount();
	return now > g_answerTick ? now - g_answerTick : 0;
}

const char *WorldQueryStatus() {
	return g_status;
}

void WorldQueryStats(u64 *requests, u64 *dispatches, u64 *answers, u64 *abandoned,
                     u32 *blockAddr, const char **host) {
	if (requests) *requests = g_requests;
	if (dispatches) *dispatches = g_dispatches;
	if (answers) *answers = g_answers;
	if (abandoned) *abandoned = g_abandoned;
	if (blockAddr) *blockAddr = g_block;
	if (host) *host = g_lastHost;
}

}  // namespace VCS
