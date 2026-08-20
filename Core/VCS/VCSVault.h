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
// So this asks. Every other frame it drops four vertical lines through the game's own collision -
// see VCSWorld for how a host-side caller gets to call CWorld::FindGroundZFor3DCoord at all - and
// compares what they hit against where the player is standing. A surface between chest and
// head height, with somewhere to stand behind it, is a ledge.
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
//      puts him down on top. The animation, the collision and the landing are all its own.
//   2. When its search declines a ledge this probe was happy with, this writes the ped's world
//      position along a curve for about half a second instead. Which works because of something
//      the free-aim work already established: unlike velocity, nothing in the game recomputes the
//      entity's stored position from its movement state, so a step written each frame accumulates
//      instead of being wiped (see PedVelX in VCSAddresses.h).
//
// The second has no animation - the player slides up the face in whatever pose he was in - so it
// is a fallback rather than a mode. The Vault tab counts the two separately.

namespace VCS {

struct VCSVaultSettings {
	// On. It was off while the motion was still the written-position fallback - handing someone a
	// jump key that sometimes slid the character up a wall with no animation is not a default -
	// but the climb is the game's own now, animation and all, so there is nothing to opt into.
	bool enabled = true;

	// How far in front of the player to look, in world units (metres, near enough). The three
	// wall samples are spread between these two.
	float reachNear = 0.50f;
	float reachFar = 1.30f;

	// How far PAST the far sample to check for somewhere to stand. A ledge with nothing behind it
	// is a fence rail, and pulling up onto one lands the player inside the geometry beyond it.
	float landingDepth = 0.90f;

	// Whether that landing check is required. Turning it off allows narrow ledges, at the price of
	// the failure it exists to prevent.
	bool requireLanding = true;

	// How far the landing surface may differ from the ledge top and still count as the same
	// surface to stand on.
	float landingTolerance = 0.40f;

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
	float minHeight = 1.50f;
	float maxHeight = 2.90f;

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
	float heights[4] = {};        // wall samples 1..3 then the landing sample, above the footing
	bool found[4] = {};
	bool armed = false;
	float targetHeight = 0.0f;    // the ledge the trigger would use
	float targetDistance = 0.0f;  // how far in front it is
	const char *reject = nullptr; // why the last probe produced no ledge, when it produced none
	VaultPhase phase = VaultPhase::Idle;
	int phaseTicks = 0;
	u64 vaults = 0;               // how many have actually run
	u64 nativeAttempts = 0;       // how many of those asked the game to do it
	u64 nativeClimbs = 0;         // and how many the game accepted and animated
};
const VCSVaultDebug &VaultDebugState();

// Drop any vault in progress and disarm. Called when the game state goes away underneath us.
void VaultReset();

}  // namespace VCS
