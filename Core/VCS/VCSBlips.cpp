// Copyright (c) 2026- PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include <algorithm>
#include <cmath>
#include <vector>

#include "Common/Log.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/MIPS/MIPS.h"
#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSBlips.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSMips.h"
#include "Core/VCS/VCSWorld.h"

namespace VCS {

// The game's own blip calls, straight out of `00C3 add_blip_for_coord`'s handler at 0x08a79228:
//
//     lw   $a0, 0x16dc($gp)   ; the store
//     ori  $a1, $zero, 4      ; kind
//     move $a2, $sp           ; &{x, y, z}
//     ori  $a3, $zero, 5
//     jal  0x0880e03c
//     ori  $t0, $zero, 3      ; the fifth argument, in the delay slot
//     ...
//     jal  0x0880e374         ; (store, handle, 3) - make it show
//
// The fifth argument is why this needs a program at all: hleEnqueueCall sets $a0..$a3 and stops.
static constexpr u32 kVCSBlipCreate = 0x0880e03c;
static constexpr u32 kVCSBlipShow = 0x0880e374;
// (store, handle). From `00BB remove_blip`'s handler at 0x08a78b7c, which loads the store into $a0
// and the handle into $a1 and calls it. Without this the markers were permanent: a batch a frame
// filled the store until every create returned -1, which is exactly what happened the first time
// this ran.
static constexpr u32 kVCSBlipRemove = 0x0880dc1c;
// Checked before the program is written, the way VCSWorld checks its own target. Never the first
// instruction of a function - that is where a JIT block marker lands. `addiu $s2, ...` style
// second words are safe because nothing branches into a prologue.
static constexpr u32 kVCSBlipCreateOp2 = 0x00E01825;   // move $v1, $a3
// Taken from 0x0880e03c itself. The first version of this used 0x0880e4a4's second word -
// a different function entirely, read off the same disassembly listing - so the check never
// matched and the maker sat at "waiting for the game" forever. The prologue it now checks is
// also what confirms the signature: it moves $a3, $a2, $t0, $a0 and $a1 into place, so all
// five arguments are real.

namespace {

constexpr int kMaxBlips = 24;

constexpr u32 kMagic = 0x50494c42;   // "BLIP"
constexpr u32 kMagicOff = 0x00;
constexpr u32 kSeqOff = 0x04;
constexpr u32 kDoneOff = 0x08;
constexpr u32 kCountOff = 0x0c;
constexpr u32 kStoreOff = 0x10;
constexpr u32 kRemCountOff = 0x14;   // handles to give back before making any
constexpr u32 kInOff = 0x20;         // count * 12 floats
constexpr u32 kInStride = 12;
constexpr u32 kOutOff = 0x140;       // count * 4 handles, written by the program
constexpr u32 kRemOff = 0x1a0;       // count * 4 handles, written by the host
// Instrumentation: what the program ACTUALLY passed to the creator, per iteration - the store in
// $a0 and the coordinate pointer in $a2, written immediately before the call. The fault says $a0
// is not the store by the time the game reads it, and this is what tells us whether it left here
// wrong or was corrupted inside. Two words per marker.
constexpr u32 kDbgOff = 0x200;
constexpr u32 kDbgStride = 8;
constexpr u32 kCodeOff = 0x280;
constexpr u32 kBlockSize = 0x480;

u32 g_block = 0;
u32 g_seq = 0;
bool g_pending = false;
// Ticks to wait before another batch. The markers only need to keep up with a car, and without
// this the maker ran a full remove-eight/create-eight cycle every couple of frames - which is what
// `placed` flickering between 0 and 8, and `slots free` between 27 and 35, was showing.
int g_cooldown = 0;
const char *g_status = "not started";
std::vector<u32> g_handles;          // what the game gave back, so it can be given back
std::vector<RoutePoint> g_wanted;    // the points still to be created
std::vector<RoutePoint> g_lastSent;  // what the placed markers are for, for the moved test

// The program.
//
//   $a0  the block, on entry     $s0  the block, across the calls
//   $s1  the index               $s2  the count
//
// The loop guard is an unsigned compare against the maximum, for the reason VCSWorld's is: the
// count lives in PSP memory, a savestate can restore it from another session, and a bad count must
// do nothing rather than loop inside game code forever.
std::vector<u32> BuildProgram() {
	std::vector<u32> code;
	const auto emit = [&code](u32 word) { code.push_back(word); };

	emit(Addiu(kRegSP, kRegSP, -32));
	emit(Sw(kRegRA, kRegSP, 28));
	emit(Sw(kRegS0, kRegSP, 24));
	emit(Sw(kRegS1, kRegSP, 20));
	emit(Sw(kRegS2, kRegSP, 16));
	emit(Move(kRegS0, kRegA0));
	// The block, on OUR frame as well as in $s0.
	//
	// $s0 is callee-saved by the ABI, so keeping the block there across a call ought to be safe -
	// and every other register this program cares about is reloaded from memory anyway. But the
	// fault says otherwise: $a0 arrives at the creator as something that is not the store, and the
	// only thing between loading it and using it is a call into the game. Rather than trust three
	// game functions to honour a convention, the block is reloaded from our own stack after each
	// one. It costs an instruction per call and removes the entire question.
	emit(Sw(kRegA0, kRegSP, 12));

	// --- give back the previous batch, before asking for another -------------------------------
	emit(Lw(kRegS2, kRegS0, kRemCountOff));
	emit(Sltiu(kRegT0, kRegS2, kMaxBlips + 1));
	const size_t remGuard = code.size();
	emit(Beq(kRegT0, kRegZero, 0));      // patched: a bad count removes nothing
	emit(Nop());
	emit(Move(kRegS1, kRegZero));

	const size_t remLoop = code.size();
	const size_t remExit = code.size();
	emit(Beq(kRegS1, kRegS2, 0));        // patched
	emit(Nop());
	emit(Sll(kRegT1, kRegS1, 2));
	emit(Addu(kRegT1, kRegT1, kRegS0));
	emit(Lw(kRegA1, kRegT1, kRemOff));
	emit(Lw(kRegA0, kRegS0, kStoreOff));
	emit(Jal(kVCSBlipRemove));
	emit(Nop());
	emit(Lw(kRegS0, kRegSP, 12));    // see the note at the prologue
	emit(Addiu(kRegS1, kRegS1, 1));
	const size_t remBack = code.size();
	emit(B(0));                          // patched
	emit(Nop());

	const size_t createStart = code.size();
	code[remGuard] = Beq(kRegT0, kRegZero, (int)(createStart - remGuard - 1));
	code[remExit] = Beq(kRegS1, kRegS2, (int)(createStart - remExit - 1));
	code[remBack] = B((int)(remLoop - remBack - 1));

	// --- and now the new ones ------------------------------------------------------------------
	emit(Lw(kRegS2, kRegS0, kCountOff));
	emit(Sltiu(kRegT0, kRegS2, kMaxBlips + 1));

	const size_t guardIndex = code.size();
	emit(Beq(kRegT0, kRegZero, 0));      // patched: skip to the handshake
	emit(Nop());
	emit(Move(kRegS1, kRegZero));

	const size_t loopIndex = code.size();
	const size_t exitIndex = code.size();
	emit(Beq(kRegS1, kRegS2, 0));        // patched
	emit(Nop());

	// &in[i] = block + kInOff + i*12, as three shifts rather than a multiply.
	emit(Sll(kRegT0, kRegS1, 1));
	emit(Addu(kRegT0, kRegT0, kRegS1));
	emit(Sll(kRegT0, kRegT0, 2));
	emit(Addu(kRegT0, kRegT0, kRegS0));
	emit(Addiu(kRegA2, kRegT0, kInOff));

	emit(Lw(kRegA0, kRegS0, kStoreOff));
	emit(Addiu(kRegA1, kRegZero, 4));
	emit(Addiu(kRegA3, kRegZero, 5));

	// Record the arguments as they leave, indexed by i. See kDbgOff.
	emit(Sll(kRegT2, kRegS1, 3));
	emit(Addu(kRegT2, kRegT2, kRegS0));
	emit(Sw(kRegA0, kRegT2, kDbgOff + 0));
	emit(Sw(kRegA2, kRegT2, kDbgOff + 4));

	emit(Jal(kVCSBlipCreate));
	emit(Addiu(kRegT0, kRegZero, 3));    // the fifth argument, in the delay slot - as the game does
	emit(Lw(kRegS0, kRegSP, 12));

	// out[i] = the handle. Stored before anything else can clobber $v0, and stored even when it is
	// -1 so the host can see how many actually landed.
	emit(Sll(kRegT1, kRegS1, 2));
	emit(Addu(kRegT1, kRegT1, kRegS0));
	emit(Sw(kRegV0, kRegT1, kOutOff));

	// A REFUSED HANDLE IS NOT SHOWN, and skipping that is not tidiness.
	//
	// The store runs out of slots - most of them are the game's own safe houses and mission
	// markers - and a create that cannot find one returns -1. The first version handed that -1
	// straight to the display call, which does not validate it: it works out an entry address from
	// -1 and writes there. That is the fault the log filled with,
	//
	//     Bad memory access ... ffffffff at 0880e09c_z_un_0880e03c
	//
	// and 0880e09c is `sw $a2, 0x274($t1)` with $t1 the store pointer - i.e. by the next call the
	// store argument itself had been corrupted by the previous scribble.
	emit(Addiu(kRegT2, kRegZero, -1));
	const size_t skipShow = code.size();
	emit(Beq(kRegV0, kRegT2, 0));        // patched
	emit(Nop());
	emit(Lw(kRegA0, kRegS0, kStoreOff));
	emit(Move(kRegA1, kRegV0));
	emit(Jal(kVCSBlipShow));
	emit(Addiu(kRegA2, kRegZero, 3));
	emit(Lw(kRegS0, kRegSP, 12));

	const size_t afterShow = code.size();
	code[skipShow] = Beq(kRegV0, kRegT2, (int)(afterShow - skipShow - 1));

	emit(Addiu(kRegS1, kRegS1, 1));
	const size_t backIndex = code.size();
	emit(B(0));                          // patched
	emit(Nop());

	const size_t doneIndex = code.size();
	// The handshake last, so the host never sees a sequence number claiming handles that are not
	// written yet.
	emit(Lw(kRegT0, kRegS0, kSeqOff));
	emit(Sw(kRegT0, kRegS0, kDoneOff));
	emit(Lw(kRegRA, kRegSP, 28));
	emit(Lw(kRegS0, kRegSP, 24));
	emit(Lw(kRegS1, kRegSP, 20));
	emit(Lw(kRegS2, kRegSP, 16));
	emit(Jr(kRegRA));
	emit(Addiu(kRegSP, kRegSP, 32));

	// Branch offsets are counted from the instruction AFTER the branch's delay slot.
	code[guardIndex] = Beq(kRegT0, kRegZero, (int)(doneIndex - guardIndex - 1));
	code[exitIndex] = Beq(kRegS1, kRegS2, (int)(doneIndex - exitIndex - 1));
	code[backIndex] = B((int)(loopIndex - backIndex - 1));
	return code;
}

}  // namespace

VCSBlipSettings &BlipSettings() {
	static VCSBlipSettings settings;
	return settings;
}

bool BlipMakerInstalled() {
	return g_block != 0;
}

void InstallBlipMaker() {
	if (g_block) {
		return;
	}
	// The module may not be loaded yet; retried every tick, exactly like the world query's.
	const std::optional<u32> check = ReadU32(kVCSBlipCreate + 4);
	if (!check || *check != kVCSBlipCreateOp2) {
		g_status = "waiting for the game";
		return;
	}

	u32 size = kBlockSize;
	const u32 block = userMemory.Alloc(size, true, "VCS route blips");
	if (block == (u32)-1) {
		g_status = "no room in the game's memory partition";
		return;
	}
	const u32 codeAddr = block + kCodeOff;
	// `jal` reaches 256MB. A program that jumps to the wrong place crashes inside the game rather
	// than returning a wrong answer, so it is worth the comparison.
	if (((codeAddr + 4) & 0xf0000000u) != (kVCSBlipCreate & 0xf0000000u)) {
		userMemory.Free(block);
		g_status = "the block landed out of jal range";
		return;
	}

	const std::vector<u32> code = BuildProgram();
	if (kCodeOff + code.size() * sizeof(u32) > kBlockSize) {
		userMemory.Free(block);
		g_status = "the program outgrew its block";
		return;
	}

	bool ok = WriteU32(block + kSeqOff, 0) && WriteU32(block + kDoneOff, 0) &&
	          WriteU32(block + kCountOff, 0) && WriteU32(block + kStoreOff, 0);
	for (size_t i = 0; ok && i < code.size(); i++) {
		ok = WriteU32(codeAddr + (u32)i * 4, code[i]);
	}
	ok = ok && WriteU32(block + kMagicOff, kMagic);
	if (!ok) {
		userMemory.Free(block);
		g_status = "could not write the block";
		return;
	}
	currentMIPS->InvalidateICache(codeAddr, (u32)code.size() * 4);

	g_block = block;
	g_seq = 0;
	g_pending = false;
	g_status = "installed";
	INFO_LOG(Log::HLE, "VCS: blip maker installed at %08x (%d instructions)", block,
	         (int)code.size());
}

void ClearRouteBlips() {
	// Nothing yet removes them through the game - see BlipTick. Until that exists this only drops
	// our record, which is why ShowRoute refuses to make a second batch while one is outstanding.
	g_handles.clear();
	g_wanted.clear();
	g_lastSent.clear();
}

void ShowRoute(const std::vector<RoutePoint> &route) {
	const VCSBlipSettings &s = BlipSettings();
	if (!s.showRoute || route.empty() || !g_block) {
		// No route any more - but the markers from the last one are still on the radar, and the
		// only thing that can take them off is a batch that removes them. An empty `wanted` with
		// handles outstanding is exactly that: BlipTick sends count=0 with the whole remove list.
		g_wanted.clear();
		g_lastSent.clear();
		return;
	}

	std::vector<RoutePoint> wanted;
	// Only the part ahead, thinned to the budget. The radar shows a few hundred units and a route
	// is thousands, so marking every node would spend the pool where nobody can see it.
	float carried = s.spacing;
	for (size_t i = 1; i < route.size(); i++) {
		const float dx = route[i].x - route[i - 1].x;
		const float dy = route[i].y - route[i - 1].y;
		carried += std::sqrt(dx * dx + dy * dy);
		if (carried >= s.spacing) {
			carried = 0.0f;
			wanted.push_back(route[i]);
			if ((int)wanted.size() >= std::min(s.maxMarkers, kMaxBlips)) {
				break;
			}
		}
	}

	// Only ask for a new batch when the markers would actually land somewhere else.
	//
	// This is what the first version got wrong, and it got it wrong in the most expensive way: it
	// re-queued every tick, so a batch of twelve went out every frame, nothing was ever given back,
	// and the store filled until every create returned -1. Two thousand batches in half a minute.
	//
	// Half the spacing is the threshold because that is the point at which a marker visibly moves;
	// below it the trail would be redrawn to look identical.
	if (wanted.size() == g_lastSent.size()) {
		bool moved = false;
		for (size_t i = 0; i < wanted.size() && !moved; i++) {
			const float dx = wanted[i].x - g_lastSent[i].x;
			const float dy = wanted[i].y - g_lastSent[i].y;
			moved = (dx * dx + dy * dy) > (s.spacing * 0.5f) * (s.spacing * 0.5f);
		}
		if (!moved) {
			return;
		}
	}
	g_wanted = wanted;
}

void BlipTick() {
	InstallBlipMaker();
	if (!g_block) {
		return;
	}
	if (g_cooldown > 0) {
		g_cooldown--;
	}

	// Has the last batch come back?
	if (g_pending) {
		const std::optional<u32> done = ReadU32(g_block + kDoneOff);
		if (done && *done == g_seq) {
			g_pending = false;
			const std::optional<u32> count = ReadU32(g_block + kCountOff);
			const int n = count ? (int)*count : 0;
			g_handles.clear();
			for (int i = 0; i < n && i < kMaxBlips; i++) {
				const std::optional<u32> h = ReadU32(g_block + kOutOff + (u32)i * 4);
				if (h && *h != (u32)-1) {
					g_handles.push_back(*h);
				}
			}
			int bad = 0;
			for (int i = 0; i < n && i < kMaxBlips; i++) {
				const std::optional<u32> store =
					ReadU32(g_block + kDbgOff + (u32)i * kDbgStride);
				const std::optional<u32> coords =
					ReadU32(g_block + kDbgOff + (u32)i * kDbgStride + 4);
				if (!store || !coords) {
					continue;
				}
				const bool storeOk = *store >= 0x08800000 && *store < 0x0a000000;
				const bool coordsOk = *coords >= g_block && *coords < g_block + kBlockSize;
				if (!storeOk || !coordsOk) {
					bad++;
					WARN_LOG(Log::HLE, "VCS: blip %d was passed store=%08x coords=%08x", i,
					         *store, *coords);
				}
			}
			g_status = bad ? "bad arguments - see the log" : "placed";
		}
		return;
	}

	// Nothing to place AND nothing to take away: genuinely idle.
	if (g_wanted.empty() && g_handles.empty()) {
		return;
	}
	// Clearing is urgent - the player just took the waypoint off and expects the markers to go.
	// Moving them along is not, so it waits out the cooldown.
	if (g_cooldown > 0 && !g_wanted.empty()) {
		return;
	}

	const std::optional<u32> store = ReadAddrU32(VCSAddr::BlipManager);
	if (!store || *store == 0) {
		g_status = "no blip store";
		return;
	}

	// Never ask for more than there is room for.
	//
	// A create with no slot left is not free: the store's search walks all 75 entries, and that
	// failure path is where the memory faults clustered. Counting first turns "ask and be refused
	// eight times" into "ask for what fits", and leaves two spare so the game can still make its
	// own - a pickup or a mission marker has a better claim on the last slot than a route dot.
	const int spare = std::max(FreeBlipSlots() - 2, 0);
	const int n = std::min(std::min((int)g_wanted.size(), kMaxBlips), spare);
	// The handles from last time ride along, and the program gives them back before it asks for
	// any more - so a batch replaces its predecessor rather than piling on top of it.
	const int rem = std::min((int)g_handles.size(), kMaxBlips);
	bool ok = WriteU32(g_block + kStoreOff, *store) && WriteU32(g_block + kCountOff, (u32)n) &&
	          WriteU32(g_block + kRemCountOff, (u32)rem);
	for (int i = 0; ok && i < rem; i++) {
		ok = WriteU32(g_block + kRemOff + (u32)i * 4, g_handles[(size_t)i]);
	}
	for (int i = 0; ok && i < n; i++) {
		const u32 at = g_block + kInOff + (u32)i * kInStride;
		ok = WriteFloat(at + 0, g_wanted[(size_t)i].x) &&
		     WriteFloat(at + 4, g_wanted[(size_t)i].y) &&
		     WriteFloat(at + 8, g_wanted[(size_t)i].z);
	}
	if (!ok) {
		g_status = "could not write the request";
		return;
	}
	g_seq++;
	if (!WriteU32(g_block + kSeqOff, g_seq)) {
		return;
	}
	if (!EnqueueGameCall(g_block + kCodeOff, g_block)) {
		g_status = "dispatch busy";
		return;
	}
	g_cooldown = 30;   // about half a second at vblank rate
	g_lastSent = g_wanted;
	g_handles.clear();   // they belong to the program now
	g_wanted.clear();
	g_pending = true;
	g_status = "sent";
}

int FreeBlipSlots() {
	const std::optional<u32> store = ReadAddrU32(VCSAddr::BlipManager);
	if (!store || *store == 0) {
		return 0;
	}
	int free = 0;
	for (int i = 0; i < kVCSBlipMaxEntries; i++) {
		const u32 entry = *store + kVCSBlipArray + (u32)i * kVCSBlipStride;
		const std::optional<u8> flags = ReadU8(entry + kVCSBlipType);
		if (flags && (*flags & kVCSBlipInUseBit) == 0) {
			free++;
		}
	}
	return free;
}

int RouteBlipCount() {
	return (int)g_handles.size();
}

const char *BlipStatus() {
	return g_status;
}

}  // namespace VCS
