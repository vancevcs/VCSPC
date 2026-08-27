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

#pragma once

#include "Common/CommonTypes.h"

// Asking the GAME about its own world, instead of guessing at it from outside.
//
// Everything else in this fork reads variables and writes variables. Vaulting cannot be built
// that way: "is there a ledge in front of the player, and how high is it" is a question about
// COLLISION GEOMETRY, and collision geometry is not a variable anywhere - it is a sector list of
// entities each carrying a collision model, walked by code. Reimplementing that walk on the host
// would mean reverse-engineering the whole collision format, and it would be a second
// implementation to keep in step with the first.
//
// So this calls the game's own function instead. VCS has exactly the primitive a ledge probe
// wants:
//
//     float CWorld::FindGroundZFor3DCoord(float x, float y, float z, bool *found)   0x08893460
//
// "Drop a vertical line from this point and tell me the height of the first surface under it."
// Found from the script command table rather than by correlation: opcode 01BB
// (get_ground_z_for_3d_coord) dispatches to 0x08a9e434, which collects three floats and calls
// 0x08893460 with them in $f12/$f13/$f14 and a pointer in $a0 - and that function is itself a
// wrapper that builds a vertical line and calls CWorld::ProcessVerticalLine (0x08891dd4). See
// "Asking the world a question" in docs/VCS_ADDRESSES.md for the disassembly.
//
// ---------------------------------------------------------------------------------------------
// HOW A HOST-SIDE CALLER GETS INTO GAME CODE, and why there is a program in PSP memory.
//
// PPSSPP can run a MIPS function on the game's own thread: hleEnqueueCall() pushes a call frame
// with a return trampoline, which is the same machinery that delivers PSP callbacks. It sets
// $a0..$a3 from the arguments and nothing else - so it cannot express a call that takes its
// arguments in $f12..$f14, which is exactly the call we need.
//
// Rather than fight that, this writes a ~30 instruction program into PSP memory and calls THAT.
// The program takes one pointer argument - the block it lives in - loads the floats out of the
// block into the FPU registers the game's own call site uses, calls the game function, and stores
// the results back into the block. Three things fall out of doing it this way:
//
//   - The calling convention is copied from the game's own code rather than assumed. This build
//     passes eight integer arguments in $a0-$a3 and $t0-$t3 (see the ProcessLineOfSight note in
//     docs/VCS_ADDRESSES.md), which is not the convention a host-side caller would guess.
//   - One enqueued call answers SEVERAL sample points, because the program loops. A ledge probe
//     wants every slot this block has - eight - per frame, and one call per frame is the whole
//     budget.
//   - Nothing has to be read back at a particular moment. The program writes its answers into the
//     block and stamps a sequence number; the host reads them whenever it next looks.
//
// The cost is that a query is ASYNCHRONOUS. Requested during one vblank tick, the call runs when
// the game's thread next finishes a syscall, and the answer is there on the following tick. For a
// ledge probe that is not a compromise: the player cannot arrive at a wall in under a frame.
//
// Threading: emu thread only, like everything else that touches PSP memory.

namespace VCS {

// The most sample points one request can carry. The block is sized for this and the program
// loops over exactly as many as are asked for.
inline constexpr int kMaxGroundSamples = 8;

struct VCSGroundSample {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;      // where to drop the vertical line FROM - only surfaces below this count
};

struct VCSGroundResult {
	float z = 0.0f;      // height of the first surface under the sample point
	bool found = false;  // false means the line hit nothing at all, and z is meaningless
};

// Put the program in memory. Idempotent, cheap when already done, and safe to call before the
// game module has loaded - it simply does nothing and can be retried. Called every tick from
// VCSGame, exactly like the fire hook's install, and for the same reason: VCS::Init runs before
// the EBOOT exists.
void InstallWorldQuery();

// Give the block back and forget it. Called from VCS::Shutdown.
void RemoveWorldQuery();

// Collect an answer if one has arrived. Called once per tick from VCSGame, before anything that
// reads results - a query answered between two ticks is only visible once this has looked.
void WorldQueryTick();

// Whether the program is installed and callable.
bool WorldQueryReady();

// One line saying why it is not, when it is not. Every branch of the install that declines to
// proceed sets this, because "not installed" on its own says nothing about which of half a dozen
// reasons applies.
const char *WorldQueryStatus();

// Remember which thread the game runs its main loop on. Called from the pad read, which is the
// game's own once-per-frame call and therefore identifies that thread by construction.
void WorldQueryNoteMainThread();

// Carry a prepared request into the game. Called only from syscalls that neither block nor
// reschedule - see the comment on the definition, which records the crash that established the
// rule. Safe and free to call from several of them; the first one each frame does the work.
void WorldQueryDispatch(const char *host);

// Ask for up to kMaxGroundSamples heights. Returns false if the query layer isn't ready, if a
// previous request is still outstanding, or if the arguments could not be written.
//
// This only PREPARES the question - it writes the sample points into the block and marks it ready
// to send. The sending happens in WorldQueryDispatch, from a syscall, for reasons that cost a
// crash to learn.
//
// Only one request may be in flight. A second one would race the first for the block, and the
// answers would be a mix of the two.
bool RequestGroundZ(const VCSGroundSample *samples, int count);

// Whether the outstanding request has been answered.
bool GroundZReady();

// --- The game's own climb -----------------------------------------------------------------------
//
// Ask the game whether the player can climb something where he is standing, and if he can, to do
// it. Both questions are one call, because the second consumes what the first found and a frame
// between them is a frame the player can move in.
//
// This is the animated climb - the game plays its own pull-up and moves the ped itself. It is the
// swimming climb-out, reached by the two functions that swimming reaches it through, rather than
// by faking the state (which engages the climb and then aborts, having nothing to climb).
//
// Pass `force` to climb ANYWAY when the game's own search declines - see "FORCING IT" on
// BuildClimbProgram. CanClimb still runs and its answer is still reported; this only says what to
// do with a no. Pass nullptr to take the game's answer as final, which is the original behaviour.
struct VCSClimbForce {
	// What to name as the thing being climbed. StartClimb takes a reference on it and stores the
	// target RELATIVE to it, so the game expects a real entity here - but fetching one needs
	// ProcessVerticalLine rather than the wrapper the probe uses, so 0 is what goes in today and
	// whether the game tolerates that is the first thing worth finding out.
	u32 entity = 0;

	// Where to end up, in world coordinates.
	float target[3] = {};
};
bool RequestNativeClimb(u32 ped, const VCSClimbForce *force);

// Whether the last answered climb was one we forced - that is, the game declined and it ran
// anyway. Always false when the game accepted on its own.
bool NativeClimbForced();

// Whether it has been answered, and whether the game found anything to climb. An answer of "not
// found" is a real answer: it means the game's own ledge search declined, and the caller should do
// whatever it does instead.
bool NativeClimbAnswered(bool *found);

// Whether a question is still in the air - prepared, sent, or running. False the moment there is
// nothing outstanding, INCLUDING when a request was abandoned unanswered. Callers must key their
// own "am I waiting" state on this rather than on having asked: a probe that keys on the answer
// alone waits forever the first time one goes missing, which is exactly what happened.
bool GroundZOutstanding();

// The results of the last ANSWERED request. Returns the number of samples it carried, 0 if there
// has never been one. Reading them does not clear them - they stay valid until the next answer
// lands, which is what lets a caller probe every few frames and still have an answer every frame.
int LatestGroundZ(VCSGroundResult *out, int max);

// How many ticks ago that answer arrived, so a caller can refuse to act on a stale one.
u64 GroundZAgeTicks();

// Counters for the debugger. requests is what we asked for, answers what came back, abandoned
// what was given up on; requests running away from answers means the call is not reaching the game,
// which is the failure this whole path is most likely to have and the one that is invisible
// without a number.
void WorldQueryStats(u64 *requests, u64 *dispatches, u64 *answers, u64 *abandoned,
                     u32 *blockAddr, const char **host);

}  // namespace VCS
