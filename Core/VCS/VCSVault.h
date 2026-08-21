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

#include "Core/VCS/VCSInput.h"

// Pulling yourself up onto a ledge - the one traversal move VCS has an animation for and no way
// to ask for.
//
// The game already does this in the water: swim up to a quay whose edge is above your head, and
// the player grabs it and pulls himself up. That is a real animation and a real piece of movement
// code; what it lacks is any way to reach it on land, because nothing on foot ever asks "is there
// a ledge in front of me".
//
// So this asks. Every other frame it drops eight vertical lines through the game's own collision -
// see VCSWorld for how a host-side caller gets to call CWorld::FindGroundZFor3DCoord at all - and
// compares what they hit against where the player is standing. A surface between chest and
// head height, with somewhere to go behind it, is a ledge.
//
// Six of those eight are aimed at the wall, and the count is set by the THINNEST thing worth
// climbing rather than by the widest. A vertical line reports only what it is dropped through: a
// fence rail a hand's breadth deep falls BETWEEN two lines spaced 0.40 apart far more often than
// it falls on one, and a fence that is missed does not read as a fence that was refused - it reads
// as nothing being in front of the player at all. Six lines across the same reach put them 0.16
// apart, which is thinner than anything in this game that a person could climb.
//
// ---------------------------------------------------------------------------------------------
// TWO WAYS OVER, and the far side is what tells them apart.
//
// Climb ONTO a wall and the surface behind its edge is the one you end up standing on: a roof, a
// balcony, the top of a container. Go OVER a fence and there is no top worth standing on - the
// ground behind it is back down at the height you started from.
//
// The landing line decides which of the two it is. Level with the ledge, within a tolerance, and
// this is a pull-up onto it. Below the ledge but not far below your own footing, and it is a
// fence: clear the top and come down the other side. Far below - a parapet at a roof edge with the
// street underneath - and it is neither, and nothing arms.
//
// ---------------------------------------------------------------------------------------------
// EVERYTHING IS MEASURED RELATIVE TO THE PLAYER'S OWN FOOTING, and that is not a detail.
//
// The obvious way to write this is "ledge height = surface Z minus the ped's Z". It would be
// wrong, because nobody knows where in the character the ped's stored position actually sits -
// GTA entities carry their origin somewhere around the hips, not at the feet, and the exact offset
// is a number this fork has never had to measure. Guessing it would put a constant error into
// every height test.
//
// The first sample is therefore fired straight down through the PLAYER, and every height is taken
// against what it hits. The unknown offset cancels: it is in both terms. Nothing here needs to
// know how tall Vic is, and the same code would work on a ped of a different size.
//
// ---------------------------------------------------------------------------------------------
// WHO MOVES THE PLAYER.
//
// Two answers, and the first one is the real thing:
//
//   1. The GAME climbs it. CanClimb and StartClimb - the two functions the swimming climb-out goes
//      through - are called on the game's own thread, and it plays the pull-up, moves the ped and
//      puts him down on top. The animation, the collision and the landing are all its own. Only a
//      pull-up is ever asked of it: "on top of what it found" is the wrong place to finish a hop
//      over a fence, because on top of a fence is a rail.
//   2. When its search declines a ledge this probe was happy with, this writes the ped's world
//      position along a curve for about half a second instead. Which works because of something
//      the free-aim work already established: unlike velocity, nothing in the game recomputes the
//      entity's stored position from its movement state, so a step written each frame accumulates
//      instead of being wiped (see PedVelX in VCSAddresses.h).
//
// The second has no animation - the player slides up the face in whatever pose he was in - so it
// is a fallback rather than a mode. The Vault tab counts the two separately.

namespace VCS {

// The probe's shape. Out here rather than in the .cpp because the debugger draws a row per line and
// has to agree about how many there are. One through the player for the footing, six at the wall,
// one past it for the landing - eight, which is every slot the query block holds
// (kMaxGroundSamples), and therefore why the wall count is six and not more.
inline constexpr int kVaultWallSamples = 6;
inline constexpr int kVaultProbeSamples = kVaultWallSamples + 2;

// Which of the two moves an armed ledge would run - see "TWO WAYS OVER" above.
enum class VaultKind {
	None,
	Onto,  // a pull-up onto the surface, which the game itself can animate
	Over,  // a hop across a fence, landing on the far side - always the written motion
};

struct VCSVaultSettings {
	// On. It was off while the motion was still the written-position fallback - handing someone a
	// jump key that sometimes slid the character up a wall with no animation is not a default -
	// but the climb is the game's own now, animation and all, so there is nothing to opt into.
	bool enabled = true;

	// How far in front of the player to look, in world units (metres, near enough). The three
	// wall samples are spread between these two.
	float reachNear = 0.50f;
	float reachFar = 1.30f;

	// How far PAST the far sample to look for somewhere to go. What is found there is what picks
	// between the two moves; a ledge with NOTHING behind it is refused either way, because it is
	// geometry with no far side and pulling up onto it lands the player inside whatever is beyond.
	float landingDepth = 0.90f;

	// Whether that landing check is required. Turning it off allows narrow ledges, at the price of
	// the failure it exists to prevent - and everything allowed that way is treated as a pull-up,
	// since with no landing there is nothing to come down onto.
	bool requireLanding = true;

	// How far the landing surface may differ from the ledge top and still count as the same
	// surface to stand on - that is, as a pull-up ONTO the ledge rather than a hop OVER it.
	float landingTolerance = 0.40f;

	// And how far below the player's own FOOTING the far side may be before a hop over is refused.
	// This is the number that separates a fence from a parapet: behind a fence is the ground you
	// were already standing on, give or take a kerb or a verge, while behind a roof edge is the
	// street. A drop deeper than this is a fall, and a fall is not a vault.
	//
	// Measured against the footing rather than against the ledge on purpose. Against the ledge it
	// would scale with the height of the thing being climbed, so a taller fence would licence a
	// longer fall - which is backwards.
	float landingDrop = 1.50f;

	// How high above the player's footing the probe lines START. This doubles as the ceiling on
	// what can be climbed, and it does so for a structural reason rather than by a check: the
	// query only ever reports surfaces BELOW its starting point, so a wall taller than this simply
	// does not register as a ledge.
	//
	// It must stay comfortably ABOVE maxHeight or the band lies: a band that allows 5.0 against a
	// ceiling of 3.2 can never see anything past 3.2, and the difference reads as "the probe found
	// nothing" rather than as a setting being in the way.
	float probeCeiling = 5.50f;

	// The pull-up band, measured from the player's footing. The animation this is built around is
	// a genuine pull-up - in the water the edge is above the player's head - so the band starts
	// well above a step and ends where an arm can still reach.
	//
	// Measured references, in the same units: a waist wall reads +0.91, a head-height wall +2.01,
	// and the ped's own origin sits 1.04 above what it stands on.
	//
	// 3.03 is not a guess at the top end and not a round number by accident - it is the boundary,
	// found by walking the band up in play until the climb stopped carrying (2026-08-21). Don't
	// tidy it up to 3.1: that was tried and it is past the edge.
	//
	// It is the ceiling on what the FORCED climb will carry, which is a different and much higher
	// number than what the game's own search would accept - CanClimb declined roughly four of every
	// five walls inside this band before forcing existed.
	float minHeight = 1.50f;
	float maxHeight = 3.03f;

	// The motion, in 60 Hz ticks. Rise first, then step forward onto the surface, which is the
	// shape of the swimming climb-out: hang, pull up, plant a foot.
	int riseTicks = 22;
	int stepTicks = 14;

	// How much of the horizontal distance is covered during the rise. The rest belongs to the step.
	// Zero would rise straight up a wall and then slide forward through it.
	float riseForwardFraction = 0.25f;

	// How far above the ledge to finish, so the landing is a small drop onto the surface rather
	// than a spawn exactly at its height.
	float clearance = 0.12f;

	// Prefer the game's own climb when it exists. Does nothing yet - kept so that finding the
	// native state is a change in one function rather than a change in the design.
	bool preferNative = true;

	// Climb ANYWAY when the game's own search declines - filling the struct CanClimb would have
	// filled and calling StartClimb on it ourselves. See "FORCING IT" in VCSWorld.cpp.
	//
	// ON. It was off while nobody knew whether the game would tolerate the null entity this hands
	// it, and the possible answers included crashing. Confirmed in play 2026-08-21: it animates,
	// and the animation now runs on very nearly every vault instead of one in ten. So 0x0890f6ec
	// null-checks the entity, and a target given with no entity to be relative to is taken as
	// world-absolute - which is exactly what this wants.
	//
	// The written motion stays underneath regardless, for a climb that starts and never engages.
	bool forceNativeClimb = true;
};

VCSVaultSettings &VaultSettings();

// Which part of a vault is running. Idle is also "a ledge may be in front of the player but
// nothing has been asked for".
enum class VaultPhase {
	Idle,
	AskingGame,  // waiting to hear whether the game will climb this itself
	Climbing,    // it is - the game owns the ped until it puts the state back
	Rising,      // it declined: pulling up the face ourselves
	Stepping,    // and planting onto the surface
};

// Probe, arm, trigger and drive. Called once per tick from VCSGame, BEFORE the input mapping -
// the trigger has to be able to consume the jump press on the tick it happens, or the game gets a
// jump as well as a vault.
void VaultTick(VCSInputContext context);

// Whether a vault is in progress. While this is true the input layer sends the game nothing at
// all: the character is being moved by us, and every button it could press is the game trying to
// move him too.
bool VaultInProgress();

// Whether a ledge is currently detected and the jump key would vault rather than jump.
bool VaultArmed();

// Everything the debugger needs to show why a vault did or didn't happen. All of it is the
// last-computed value, so it stays readable while standing still in front of a wall.
struct VCSVaultDebug {
	bool probing = false;         // a query is outstanding or answers are arriving
	bool haveFooting = false;     // the sample under the player found a surface
	float footingZ = 0.0f;        // and what it hit
	float playerZ = 0.0f;         // the ped's stored Z, for comparison - see the note on origins
	// The wall samples near to far, then the landing sample, all as heights above the footing.
	float heights[kVaultWallSamples + 1] = {};
	bool found[kVaultWallSamples + 1] = {};
	bool armed = false;
	VaultKind kind = VaultKind::None;  // and which of the two moves it would be
	float targetHeight = 0.0f;    // the ledge the trigger would use
	float targetDistance = 0.0f;  // how far in front it is
	const char *reject = nullptr; // why the last probe produced no ledge, when it produced none
	VaultPhase phase = VaultPhase::Idle;
	int phaseTicks = 0;
	u64 vaults = 0;               // how many have actually run
	u64 nativeAttempts = 0;       // how many of those got as far as asking the game
	u64 nativeClimbs = 0;         // how many it accepted and animated
	u64 nativeDeclines = 0;       // how many its own search refused
	u64 nativeSilent = 0;         // how many it never answered at all
	u64 nativeForced = 0;         // how many ran on a struct we filled in after it refused
	u64 nativeStillborn = 0;      // and how many of those never engaged the climb at all

	// Why the last vault did or didn't get the game's animation, in words. Sticky, unlike `reject`
	// above it: that one is rewritten by the next probe within a frame of the vault ending, which
	// put the answer out of reach at exactly the moment it was wanted.
	const char *lastNative = nullptr;
};
const VCSVaultDebug &VaultDebugState();

// Drop any vault in progress and disarm. Called when the game state goes away underneath us.
void VaultReset();

}  // namespace VCS
