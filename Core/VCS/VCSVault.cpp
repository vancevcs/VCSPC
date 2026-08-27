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

#include <cmath>

#include "Core/VCS/VCSVault.h"

#include "Core/VCS/VCSAddresses.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSWorld.h"

namespace VCS {

// The probe fires eight lines: one through the player for the footing, six at the wall, and one
// past it for somewhere to go. Sample 0 is the footing; 1..kWallSamples are the wall, near to far;
// the last is the landing. The wall count lives in the header - see the note there on why it is
// six and not three.
static constexpr int kSelfSample = 0;
static constexpr int kWallSamples = kVaultWallSamples;
static constexpr int kLandSample = kWallSamples + 1;
static constexpr int kProbeSamples = kVaultProbeSamples;

static_assert(kProbeSamples <= kMaxGroundSamples, "the probe asks for more than the block holds");

// How old an answer may be before the trigger refuses it, and how far the player may have walked
// since it was fired. Two ticks is the normal age - request on one tick, collect on the next - so
// these are both generous, and both exist for the same case: a query that went out and never came
// back must not leave a ledge armed forever.
static constexpr u64 kMaxAnswerAgeTicks = 12;
static constexpr float kMaxDriftSq = 0.45f * 0.45f;

static VCSVaultSettings g_settings;
static VCSVaultDebug g_debug;

struct Vec2 {
	float x = 0.0f, y = 0.0f;
};

// What the outstanding query was fired from, kept so its answers can still be interpreted after
// the player has moved. Without this the heights would be read against wherever the player is NOW,
// which is not where the lines were dropped.
static Vec2 g_probeOrigin;
static Vec2 g_probeForward;
static float g_probeDistances[kProbeSamples] = {};
static bool g_probeInFlight = false;

// The armed ledge, in world terms rather than as an offset, so nothing has to be recomputed at
// the moment of the press.
static bool g_armed = false;
static Vec2 g_ledgeOrigin;      // where the player was when this was solved
static Vec2 g_ledgeForward;
static float g_ledgeZ = 0.0f;   // world Z of the edge to get over
static float g_landingZ = 0.0f; // and of the surface to finish standing on, which for a pull-up
                                // IS the ledge and for a hop over a fence is the ground behind it
static float g_footingZ = 0.0f; // the one being left
static float g_ledgeDistance = 0.0f;
static VaultKind g_ledgeKind = VaultKind::None;

// The motion.
static VaultPhase g_phase = VaultPhase::Idle;
static int g_phaseTicks = 0;
static Vec2 g_startXY;
static float g_startZ = 0.0f;
static Vec2 g_riseEndXY;
static Vec2 g_landXY;
static float g_topZ = 0.0f;      // where the rise finishes: clear of the ledge, whichever move
static float g_settleZ = 0.0f;   // where the step finishes: standing on the landing surface
static float g_vaultHeading = 0.0f;

// The jump key has to be RELEASED between vaults. Without this, holding it through a vault starts
// the next one the instant the first ends, which on a staircase of ledges is a lift.
static bool g_triggerLatched = false;

static u64 g_vaults = 0;
static u64 g_nativeAttempts = 0;
static u64 g_nativeClimbs = 0;
static u64 g_nativeDeclines = 0;
static u64 g_nativeSilent = 0;
static u64 g_nativeForced = 0;
static u64 g_nativeStillborn = 0;

// Whether the climb we are watching is one we forced, and whether the ped ever actually entered
// the climb state. Both exist for the same case: a forced struct the game accepts at the call and
// then does nothing with. Without them that reads as a vault which simply never happened, and the
// player is left standing at the wall having pressed jump.
static bool g_climbWasForced = false;
static bool g_sawClimbState = false;

// Why the last vault did or didn't get the game's animation. A string rather than a flag, for the
// same reason `reject` is one: these are not degrees of one thing, they are unrelated failures, and
// the difference between "never asked" and "asked and refused" is the whole diagnosis.
//
// It has to outlive the vault. `reject` does not - the probe restarts the instant the phase goes
// back to Idle and overwrites it within a frame, so the reason a vault had no animation was gone
// by the time anyone could read it.
static const char *g_lastNative = nullptr;

// How long to wait for the game to answer whether it will climb. The query turns around in a frame
// or two; this is the point at which waiting longer is worse than falling back.
static constexpr int kNativeAnswerTicks = 20;

// A ceiling on watching the game climb, in case the state never goes back - without it a stuck
// state would hold the player's controls for the rest of the session. The real climb takes about
// two seconds.
static constexpr int kNativeClimbMaxTicks = 300;

// How long to let the ped state stay at something other than "climbing" at the START of a climb
// before concluding the climb is not going to happen. The game's own climb is already in state 44
// by the time we hear back, so this only ever fires on a forced one that StartClimb accepted and
// then quietly dropped - the exact failure the hand-written state 44 produced.
static constexpr int kClimbEngageTicks = 8;

// What the ped state reads while the game is running its climb-out. Measured, not guessed - see
// kVCSPedStateOffset in VCSAddresses.h.
static constexpr u32 kVCSPedStateClimbing = 44;

VCSVaultSettings &VaultSettings() {
	return g_settings;
}

const VCSVaultDebug &VaultDebugState() {
	return g_debug;
}

bool VaultInProgress() {
	return g_phase != VaultPhase::Idle;
}

bool VaultArmed() {
	return g_armed;
}

void VaultReset() {
	g_phase = VaultPhase::Idle;
	g_phaseTicks = 0;
	g_armed = false;
	g_ledgeKind = VaultKind::None;
	g_probeInFlight = false;
	g_debug = VCSVaultDebug();
	g_debug.vaults = g_vaults;
	g_debug.nativeAttempts = g_nativeAttempts;
	g_debug.nativeClimbs = g_nativeClimbs;
	g_debug.nativeDeclines = g_nativeDeclines;
	g_debug.nativeSilent = g_nativeSilent;
	g_debug.nativeForced = g_nativeForced;
	g_debug.nativeStillborn = g_nativeStillborn;
	g_debug.lastNative = g_lastNative;
}

static float SmoothStep(float t) {
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;
	return t * t * (3.0f - 2.0f * t);
}

static float Lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

// The player's entity, and its stored world position. Both go through the same offsets every
// other part of this fork uses - see kVCSEntityPositionOffset.
static bool PlayerEntity(u32 *ped) {
	const std::optional<u32> p = ReadAddrU32(VCSAddr::PlayerBase);
	if (!p || !*p) {
		return false;
	}
	*ped = *p;
	return true;
}

static bool ReadPlayerPos(u32 ped, Vec2 *xy, float *z) {
	const std::optional<float> x = ReadFloat(ped + kVCSEntityPositionOffset + 0);
	const std::optional<float> y = ReadFloat(ped + kVCSEntityPositionOffset + 4);
	const std::optional<float> zz = ReadFloat(ped + kVCSEntityPositionOffset + 8);
	if (!x || !y || !zz) {
		return false;
	}
	xy->x = *x;
	xy->y = *y;
	*z = *zz;
	return true;
}

static bool WritePlayerPos(u32 ped, const Vec2 &xy, float z) {
	return WriteFloat(ped + kVCSEntityPositionOffset + 0, xy.x) &&
	       WriteFloat(ped + kVCSEntityPositionOffset + 4, xy.y) &&
	       WriteFloat(ped + kVCSEntityPositionOffset + 8, z);
}

// Which way the player faces. The convention is the one PedHeading's own note records: forward is
// (-sin, cos), which is also what the free-aim heading write assumes, so the two can never end up
// disagreeing about which way is forwards.
static bool PlayerForward(Vec2 *out, float *heading) {
	const std::optional<float> h = ReadAddrFloat(VCSAddr::PedHeading);
	if (!h) {
		return false;
	}
	out->x = -sinf(*h);
	out->y = cosf(*h);
	if (heading) {
		*heading = *h;
	}
	return true;
}

// Fire the lines. Nothing here decides anything - it only asks.
static void RequestProbe(const Vec2 &at, const Vec2 &forward, float playerZ, float footingZ,
                         bool haveFooting) {
	// The lines start above the player's FOOTING when we know it, and above the ped's own origin
	// before the first answer has arrived. Starting from the footing is what makes probeCeiling
	// mean "how high a ledge may be" rather than "how high above some unknown point in the
	// character's body".
	const float base = haveFooting ? footingZ : playerZ;
	const float ceiling = base + g_settings.probeCeiling;

	// Not `near` and `far`: windows.h defines both as empty macros, and a local of either name is
	// a compile error the moment anything in this translation unit pulls it in.
	const float reachNear = g_settings.reachNear;
	const float reachFar = g_settings.reachFar;

	// The wall lines are spread evenly across the reach, both ends included. Evenly rather than
	// clustered anywhere, because a thin obstacle is equally likely at any distance in it - and the
	// gap between two neighbours is exactly the thickness this can still miss.
	g_probeDistances[kSelfSample] = 0.0f;
	for (int i = 0; i < kWallSamples; i++) {
		const float t = kWallSamples > 1 ? (float)i / (float)(kWallSamples - 1) : 0.0f;
		g_probeDistances[1 + i] = Lerp(reachNear, reachFar, t);
	}
	g_probeDistances[kLandSample] = reachFar + g_settings.landingDepth;

	VCSGroundSample samples[kProbeSamples];
	for (int i = 0; i < kProbeSamples; i++) {
		samples[i].x = at.x + forward.x * g_probeDistances[i];
		samples[i].y = at.y + forward.y * g_probeDistances[i];
		// The player's own line starts a little above the ped rather than at the ceiling, so a low
		// roof or an awning overhead cannot be mistaken for the floor being stood on.
		samples[i].z = (i == kSelfSample) ? playerZ + 0.20f : ceiling;
	}

	if (RequestGroundZ(samples, kProbeSamples)) {
		g_probeOrigin = at;
		g_probeForward = forward;
		g_probeInFlight = true;
	}
}

// Turn the last set of answers into a ledge, or into a reason there isn't one. Runs on the tick
// the answers arrive; g_probeOrigin is where they were fired from, which is not necessarily where
// the player is now.
static void SolveProbe() {
	VCSGroundResult r[kMaxGroundSamples];
	const int n = LatestGroundZ(r, kMaxGroundSamples);
	if (n < kProbeSamples) {
		return;
	}

	g_debug.haveFooting = r[kSelfSample].found;
	g_debug.footingZ = r[kSelfSample].z;
	// One row per line, wall samples then the landing - which is r[kLandSample], and therefore the
	// last iteration of the same loop.
	for (int i = 0; i <= kWallSamples; i++) {
		g_debug.found[i] = r[i + 1].found;
		g_debug.heights[i] = r[i + 1].z - r[kSelfSample].z;
	}

	g_armed = false;
	g_debug.armed = false;
	g_debug.kind = VaultKind::None;
	g_ledgeKind = VaultKind::None;
	g_debug.targetHeight = 0.0f;
	g_debug.targetDistance = 0.0f;

	if (!r[kSelfSample].found) {
		// No ground under the player at all. In the air, in the water, or somewhere the collision
		// has not streamed in - none of which is a place to start climbing from.
		g_debug.reject = "no footing";
		return;
	}
	const float footing = r[kSelfSample].z;

	// Nearest first: the wall you are standing against is the one you meant, not the one behind it.
	int hit = -1;
	for (int i = 1; i <= kWallSamples; i++) {
		if (!r[i].found) {
			continue;
		}
		const float h = r[i].z - footing;
		if (h >= g_settings.minHeight && h <= g_settings.maxHeight) {
			hit = i;
			break;
		}
	}
	if (hit < 0) {
		g_debug.reject = "nothing in the pull-up band";
		return;
	}

	const float ledgeZ = r[hit].z;

	// What is behind the edge picks the move. Level with it and there is a surface to stand on, so
	// this is a pull-up ONTO it. Below it, but not far below the footing, and the thing in front is
	// a fence: go OVER, and come down on the far side. Far below is a parapet at a roof edge and
	// the street underneath, and HIGHER is a second wall with no room between the two.
	VaultKind kind = VaultKind::Onto;
	float landingZ = ledgeZ;
	const char *landingReject = nullptr;
	if (!r[kLandSample].found) {
		landingReject = "nothing on the far side";
	} else {
		const float landZ = r[kLandSample].z;
		if (std::fabs(landZ - ledgeZ) <= g_settings.landingTolerance) {
			kind = VaultKind::Onto;
		} else if (landZ > ledgeZ) {
			landingReject = "the far side is higher than the ledge";
		} else if (footing - landZ <= g_settings.landingDrop) {
			kind = VaultKind::Over;
			landingZ = landZ;
		} else {
			landingReject = "the far side is a drop, not a landing";
		}
	}
	if (landingReject) {
		if (g_settings.requireLanding) {
			g_debug.reject = landingReject;
			return;
		}
		// Allowed anyway, which is the whole point of the setting. It stays a pull-up: with no
		// landing to speak of there is nothing to come down onto, so the top is the only target.
		kind = VaultKind::Onto;
		landingZ = ledgeZ;
	}

	g_armed = true;
	g_ledgeOrigin = g_probeOrigin;
	g_ledgeForward = g_probeForward;
	g_ledgeZ = ledgeZ;
	g_landingZ = landingZ;
	g_footingZ = footing;
	g_ledgeDistance = g_probeDistances[hit];
	g_ledgeKind = kind;

	g_debug.armed = true;
	g_debug.kind = kind;
	g_debug.reject = nullptr;
	g_debug.targetHeight = ledgeZ - footing;
	g_debug.targetDistance = g_ledgeDistance;
}

// The game's own climb-out - the animated one, which is what this whole feature wanted.
//
// Asking is two of the game's own functions, and VCSWorld runs both in one call: CanClimb fills a
// struct with whatever it found, StartClimb consumes it, plays the pull-up and moves the ped. So
// this does not reproduce the climb, it REQUESTS it - the game's search, the game's geometry, the
// game's animation.
//
// The answer arrives a frame or two later, which is why this returns "asked" rather than
// "climbing": the state machine waits in AskingGame, and falls back to the written motion if the
// game declines.
//
// Two dead ends behind this, both recorded in CLAUDE.md so nobody re-walks them: writing the ped
// state (44) by hand engages the climb and then aborts, because nothing told it WHAT to climb; and
// the in-water flag cannot be forced, because the game recomputes it from the world every frame.
static bool TryStartNativeClimb(u32 ped, const Vec2 &landXY) {
	if (!g_settings.preferNative) {
		g_lastNative = "not asked - the game's own climb is switched off";
		return false;
	}

	// Where we want him to finish, as a point on the surface rather than as a ped origin - the
	// game moves its own ped and knows its own offsets. A guess at the convention, and the first
	// thing to calibrate if a forced climb runs but lands somewhere wrong.
	VCSClimbForce force;
	force.entity = 0;
	force.target[0] = landXY.x;
	force.target[1] = landXY.y;
	force.target[2] = g_landingZ;
	// Counted only once the question is actually on its way. A request that never went out is not
	// the game refusing anything - it means the query layer is unavailable, which is a fault on
	// this side rather than a judgement on the game's, and lumping the two together hides it.
	if (!RequestNativeClimb(ped, g_settings.forceNativeClimb ? &force : nullptr)) {
		g_lastNative = "could not ask - the world query is not available";
		return false;
	}
	g_nativeAttempts++;
	return true;
}

static void StartVault(u32 ped, const Vec2 &pos, float z) {
	// Everything is captured now, at the press, and nothing is re-read during the motion. A vault
	// that re-solved its target each frame would chase the player it is itself moving.
	g_startXY = pos;
	g_startZ = z;

	const float standOffset = z - g_footingZ;
	const float landDistance = g_settings.reachFar + g_settings.landingDepth;

	g_landXY.x = pos.x + g_ledgeForward.x * landDistance;
	g_landXY.y = pos.y + g_ledgeForward.y * landDistance;

	const float riseDistance = landDistance * g_settings.riseForwardFraction;
	g_riseEndXY.x = pos.x + g_ledgeForward.x * riseDistance;
	g_riseEndXY.y = pos.y + g_ledgeForward.y * riseDistance;

	// The ped's origin is not at its feet, and this is where that matters: the height it has to
	// finish at is the surface it lands on plus however far the origin sat above the surface it was
	// standing on a moment ago. Same cancellation the whole file is built on.
	//
	// The rise always clears the LEDGE, whichever move this is - going over a fence means getting
	// above the rail first, and only where it comes DOWN differs. For a pull-up the landing is the
	// ledge, so the two heights collapse back into the one this used to compute.
	g_settleZ = g_landingZ + standOffset;
	g_topZ = g_ledgeZ + standOffset + g_settings.clearance;

	g_vaultHeading = atan2f(-g_ledgeForward.x, g_ledgeForward.y);

	g_phaseTicks = 0;
	g_armed = false;
	g_vaults++;

	// Ask the game first, and do nothing at all while it decides. Writing a position on the frames
	// its climb is starting would be two things moving one ped.
	//
	// Only for a pull-up, though. The game's climb-out finishes standing on top of whatever its own
	// search found, and on top of a fence is a rail - the one place a hop over it is meant not to
	// end. So going over always runs the written motion, whatever preferNative says.
	// Going over is only excluded while the game picks its own destination. Once we are filling the
	// struct in we choose the target too, so a fence can be asked for as readily as a wall - it is
	// the far side we hand it rather than the rail.
	if (g_ledgeKind == VaultKind::Over && !g_settings.forceNativeClimb) {
		g_lastNative = "not asked - going over it, not onto it";
	} else if (TryStartNativeClimb(ped, g_landXY)) {
		g_phase = VaultPhase::AskingGame;
		return;
	}
	g_phase = VaultPhase::Rising;
}

// One tick of the written motion. Rise up the face, then step onto the surface.
static void DriveVault(u32 ped) {
	// Velocity is zeroed every tick rather than once at the start, because the game recomputes it
	// from the movement state - the same property that makes position writable makes velocity not.
	// Leaving it alone means the run that carried the player into the wall is still being applied
	// underneath the climb.
	WriteAddrFloat(VCSAddr::PedVelX, 0.0f);
	WriteAddrFloat(VCSAddr::PedVelY, 0.0f);
	// Hold the facing at the wall for the whole climb. Both fields, or the game eases the heading
	// back toward its own target while we are holding the other one.
	WriteAddrFloat(VCSAddr::PedHeading, g_vaultHeading);
	WriteAddrFloat(VCSAddr::PedHeadingTarget, g_vaultHeading);

	g_phaseTicks++;

	if (g_phase == VaultPhase::Rising) {
		const int total = g_settings.riseTicks > 0 ? g_settings.riseTicks : 1;
		const float e = SmoothStep((float)g_phaseTicks / (float)total);
		const Vec2 xy{Lerp(g_startXY.x, g_riseEndXY.x, e), Lerp(g_startXY.y, g_riseEndXY.y, e)};
		if (!WritePlayerPos(ped, xy, Lerp(g_startZ, g_topZ, e))) {
			VaultReset();
			return;
		}
		if (g_phaseTicks >= total) {
			g_phase = VaultPhase::Stepping;
			g_phaseTicks = 0;
		}
		return;
	}

	const int total = g_settings.stepTicks > 0 ? g_settings.stepTicks : 1;
	const float e = SmoothStep((float)g_phaseTicks / (float)total);
	const Vec2 xy{Lerp(g_riseEndXY.x, g_landXY.x, e), Lerp(g_riseEndXY.y, g_landXY.y, e)};
	if (!WritePlayerPos(ped, xy, Lerp(g_topZ, g_settleZ, e))) {
		VaultReset();
		return;
	}
	if (g_phaseTicks >= total) {
		g_phase = VaultPhase::Idle;
		g_phaseTicks = 0;
	}
}

void VaultTick(VCSInputContext context) {
	g_debug.phase = g_phase;
	g_debug.phaseTicks = g_phaseTicks;
	g_debug.vaults = g_vaults;
	g_debug.nativeAttempts = g_nativeAttempts;
	g_debug.nativeClimbs = g_nativeClimbs;
	g_debug.nativeDeclines = g_nativeDeclines;
	g_debug.nativeSilent = g_nativeSilent;
	g_debug.nativeForced = g_nativeForced;
	g_debug.nativeStillborn = g_nativeStillborn;
	g_debug.lastNative = g_lastNative;
	g_debug.probing = g_probeInFlight;

	if (!g_settings.enabled) {
		if (g_phase != VaultPhase::Idle || g_armed) {
			VaultReset();
		}
		return;
	}

	u32 ped = 0;
	if (!PlayerEntity(&ped)) {
		VaultReset();
		return;
	}

	// Waiting on the game's answer. Nothing is written to the ped here on purpose - see
	// TryStartNativeClimb.
	if (g_phase == VaultPhase::AskingGame) {
		g_phaseTicks++;
		bool found = false;
		if (NativeClimbAnswered(&found)) {
			// Forced is not a kind of found. The game declined either way, and `found` keeps
			// meaning what it has always meant: whether its own search agreed.
			const bool forced = NativeClimbForced();
			if (!found) {
				g_nativeDeclines++;
			}
			if (found || forced) {
				// The climb is running. It owns the ped now, animation and all; all this has left
				// to do is stay out of the way until its state goes back to normal.
				g_phase = VaultPhase::Climbing;
				g_phaseTicks = 0;
				g_climbWasForced = forced;
				g_sawClimbState = false;
				if (forced) {
					g_nativeForced++;
					g_lastNative = "the game declined - forced onto our own target";
				} else {
					g_nativeClimbs++;
					g_lastNative = "the game animated it";
				}
				g_debug.reject = nullptr;
			} else {
				// Its ledge search declined and nothing overrode it - our probe is happy with this
				// wall and the game is not. Fall back to moving the player ourselves.
				g_phase = VaultPhase::Rising;
				g_phaseTicks = 0;
				g_lastNative = "the game's own search declined it";
				g_debug.reject = "the game declined to climb it";
			}
		} else if (g_phaseTicks > kNativeAnswerTicks) {
			g_phase = VaultPhase::Rising;
			g_phaseTicks = 0;
			g_nativeSilent++;
			g_lastNative = "the game never answered";
			g_debug.reject = "no answer from the game";
		}
		g_debug.phase = g_phase;
		g_debug.phaseTicks = g_phaseTicks;
		return;
	}

	// The game is climbing. Watch its state rather than a timer: the animation is however long the
	// animation is, and guessing at it would either cut the player loose mid-pull or hold the
	// controls after he is already standing.
	if (g_phase == VaultPhase::Climbing) {
		g_phaseTicks++;
		const std::optional<u32> state = ReadU32(ped + kVCSPedStateOffset);
		const bool stillClimbing = state.value_or(0) == kVCSPedStateClimbing;
		if (stillClimbing) {
			g_sawClimbState = true;
		}
		if (!g_sawClimbState) {
			// It has not started yet. The game's own climb is already in state 44 by the time we
			// hear about it, so this is a forced one that StartClimb took and then did nothing
			// with - the hand-written state 44 all over again, and the reason a fallback still
			// has to exist under a climb we asked for.
			if (g_phaseTicks > kClimbEngageTicks) {
				g_nativeStillborn++;
				g_lastNative = g_climbWasForced ? "forced, and the climb never engaged"
				                               : "the game accepted it, then never started";
				// Start the written motion from where the abandoned climb LEFT him, not from
				// where he was at the press. The state-44 experiment dropped the ped 0.57 into a
				// hang before giving up, and lerping from the stale start would snap that back.
				Vec2 now;
				float nowZ = 0.0f;
				if (ReadPlayerPos(ped, &now, &nowZ)) {
					g_startXY = now;
					g_startZ = nowZ;
				}
				g_phase = VaultPhase::Rising;
				g_phaseTicks = 0;
			}
			g_debug.phase = g_phase;
			g_debug.phaseTicks = g_phaseTicks;
			return;
		}
		if (!stillClimbing || g_phaseTicks > kNativeClimbMaxTicks) {
			g_phase = VaultPhase::Idle;
			g_phaseTicks = 0;
		}
		g_debug.phase = g_phase;
		g_debug.phaseTicks = g_phaseTicks;
		return;
	}

	// A vault in progress owns the character and finishes on its own clock. It is deliberately not
	// cancellable by the player: half a climb leaves the ped inside a wall.
	if (g_phase != VaultPhase::Idle) {
		DriveVault(ped);
		g_debug.phase = g_phase;
		g_debug.phaseTicks = g_phaseTicks;
		return;
	}

	// On foot only. In a vehicle there is nothing to climb with, and while aiming the player is
	// doing something else with the same hands - and with the same key, since Space is the block
	// button in a fistfight.
	if (context != VCSInputContext::OnFoot) {
		g_armed = false;
		g_debug.armed = false;
		g_debug.reject = "not on foot";
		return;
	}

	Vec2 pos;
	float z = 0.0f;
	Vec2 forward;
	float heading = 0.0f;
	if (!ReadPlayerPos(ped, &pos, &z) || !PlayerForward(&forward, &heading)) {
		g_armed = false;
		g_debug.reject = "no player position";
		return;
	}
	g_debug.playerZ = z;

	// Collect an answer if one landed, then ask again. The two are in this order so that a fresh
	// question is always in flight: the query is asynchronous, and a probe fired only when the
	// player wants one would arrive a frame after the press.
	//
	// "Still waiting" comes from the query layer, not from a flag of our own. The first version
	// kept its own bool and cleared it only on a successful answer - so the first request that
	// went missing left it set forever, and no probe was ever fired again. Measured in play: seq
	// stuck at 1, done at 0, and a ledge that could never arm.
	if (g_probeInFlight && !GroundZOutstanding()) {
		g_probeInFlight = false;
		if (GroundZReady()) {
			SolveProbe();
		} else {
			g_debug.reject = "the probe went unanswered";
		}
	}
	if (!g_probeInFlight) {
		RequestProbe(pos, forward, z, g_debug.haveFooting ? g_debug.footingZ : z,
		             g_debug.haveFooting);
	}
	g_debug.probing = g_probeInFlight;

	// An armed ledge goes stale two ways: the answer gets old, or the player walks away from where
	// it was solved. Both matter, because the alternative is vaulting onto a wall that is now
	// behind you.
	if (g_armed) {
		const float dx = pos.x - g_ledgeOrigin.x;
		const float dy = pos.y - g_ledgeOrigin.y;
		if (GroundZAgeTicks() > kMaxAnswerAgeTicks || dx * dx + dy * dy > kMaxDriftSq) {
			g_armed = false;
			g_debug.armed = false;
			g_debug.reject = "stale";
		}
	}

	// The trigger. Jump when there is nothing to climb, vault when there is - so nothing is taken
	// away from the player, and there is no new key to learn.
	const bool jumpDown = JumpHeld();
	if (!jumpDown) {
		g_triggerLatched = false;
	} else if (!g_triggerLatched) {
		g_triggerLatched = true;
		if (g_armed) {
			StartVault(ped, pos, z);
			g_debug.phase = g_phase;
			g_debug.phaseTicks = g_phaseTicks;
		}
	}
}

}  // namespace VCS
