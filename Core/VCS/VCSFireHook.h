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

// Free aim, done the way re3 and reVC do it: at the fire site.
//
// Those games aim by RAYCASTING THROUGH THE CROSSHAIR PIXEL rather than along the ped's aim -
// CWeapon::FireInstantHit calls CCamera::Find3rdPersonCamTargetVector, which unprojects that pixel
// into a world ray. The arm IK and the reticle sprite are presentation on top of it. VCS has no
// such branch, but it turns out not to need one: the shot direction is writable at the raycast, so
// the same model can be imposed from outside.
//
// That was established in play, not assumed - see "The path a shot actually takes" in
// docs/VCS_ADDRESSES.md. Rotating the target vector 25 degrees immediately before
// CWorld::ProcessLineOfSight moved the resolved hit point 5.50 units, reproducibly, and the
// rounds visibly hit a different object.
//
// Why this matters more than it looks: every other aiming route in this fork has to fight the
// game's own aim integrator, which squares the axis and smooths it, and `aimResponseModel` in
// VCSCamera.cpp exists solely to invert that. There is no integrator here. The direction we
// write IS the direction the bullet takes.
//
// ---------------------------------------------------------------------------------------------
// THE RAY IS BUILT FROM THE CAMERA'S STORED BASIS, NOT FROM CameraYaw/CameraPitch.
//
// The first build reconstructed a direction as (cos, sin) of `CameraYaw - PI`, with `CameraPitch`
// used as-is, and then carried a hand-tuned `crosshairX` trim on top. Every part of that was a
// symptom of one mistake. Beta and Alpha are the camera's ORBIT angles about its look-at target;
// the camera's actual forward is a separate stored vector, and the two differ by however far the
// player sits from the middle of the screen. Measured in a gameplay savestate: 4.42 degrees, about
// 3.3 of it horizontal and 2.8 vertical.
//
// That difference is not a constant. It is an angular offset, so it grows as the camera closes on
// the player and swings as the camera orbits - which is exactly the "crosshairX that is right at
// one angle is wrong at another" wander recorded below, and why no single trim ever held.
//
// So: read Front, Up and Source out of CCam (see kVCSCamFrontOffset and friends), offset the ray
// by the crosshair's screen position through re3's own formula, and fire it from the camera. The
// remaining knobs describe the CROSSHAIR, which is a real thing with a real position, rather than
// describing an error.

namespace VCS {

// Install the hook. Safe to call more than once, and a no-op unless this is the USA build with
// the compat flag on. Called from VCS::Init.
void InstallFireHook();

// Undo it, restoring the instruction we replaced. Called from VCS::Shutdown.
void RemoveFireHook();

// Whether the hook is currently installed - for the debugger overlay.
bool FireHookInstalled();

// How many raycasts the hook has seen, and how many it actually redirected. A redirect count
// that stays at zero while the seen count climbs means the guard is rejecting our own shots,
// which is the failure worth being able to see at a glance.
void FireHookStats(u64 *seen, u64 *redirected);

// What the last redirect actually solved, so the debugger can show it without recomputing
// anything - and so that a wrong answer can be read off the numbers instead of inferred from
// where the bullets went. Filled in on the emu thread by the hook and read on the same thread by
// ImVCS; see the threading note in CLAUDE.md.
struct VCSFireHookTrace {
	bool valid = false;        // false until a shot has been redirected at least once
	bool haveBasis = false;    // whether Front/Up/Source read back
	float front[3] = {};
	float up[3] = {};
	float camSource[3] = {};
	float fov = 0.0f;
	float origin[3] = {};      // where the ray was finally started from
	float dir[3] = {};         // unit direction it was fired along
	float gunSource[3] = {};   // the game's own source, i.e. the muzzle
	float range = 0.0f;        // the length we used
	float gameRange = 0.0f;    // the length the game's own target implied, for comparison
	int weaponType = -1;       // -1 when the weapon record could not be read
};
const VCSFireHookTrace &FireHookLastTrace();

// The world ray the crosshair currently covers: origin at the camera, `dir` a unit vector, built by
// the same re3 formula the fire hook aims along. Exposed so that the GUN can be pointed down the
// identical line the bullet will take - if these two ever came from separate solves they would
// drift apart, and a gun that points somewhere other than the shot is the exact failure this whole
// effort started from.
//
// Returns false if the camera basis is unreadable, in which case callers must do nothing rather
// than aim at a default. Emu thread only.
bool SolveAimRay(float origin[3], float dir[3]);

struct VCSFireHookSettings {
	// ON. It was opt-in while the shot direction was still wrong often enough to matter; it is the
	// whole feature now, and having to tick it every session was pure friction.
	//
	// Note this also switches free aim over to driving the camera - CameraAimActive is gated on the
	// hook being installed AND enabled, because with the shot still resolved the old way, steering
	// the camera would reintroduce the desync that got mouse look in free aim disproved.
	bool enabled = true;

	// Debug: instead of aiming from the camera, rotate the game's own shot by this many degrees.
	// Non-zero reproduces exactly what the vcsfiretest.py harness did, in-engine, which is the
	// cheapest way to confirm the hook itself works before trusting the camera maths on top.
	float debugDeflectDegrees = 0.0f;

	// How close to the player a ray must start to count as the player's shot.
	//
	// This wrapper is a pure forwarding shim with NO shooter argument - unlike reVC's, which
	// takes `shooter` and lets the branch read `shooter == FindPlayerPed()`. So the hook cannot
	// ask who fired and infers it from geometry instead: the player's shot leaves the player's
	// gun. An NPC firing from inside this radius would be misread, which is why the aim key must
	// also be held.
	float playerRadius = 3.0f;

	// Where the crosshair sits ON SCREEN, as a fraction of width and height, 0.5/0.5 being dead
	// centre. These are re3's m_f3rdPersonCHairMultX / ...Y and they mean the same thing here:
	// Find3rdPersonCamTargetVector unprojects that pixel rather than the middle of the frame.
	//
	// Both default to centre, and that is a PREDICTION worth stating so it can be falsified. The
	// old build needed a 3.2 degree horizontal trim, and the camera's stored Front turns out to sit
	// 3.3 degrees off the angle that trim was correcting - so with Front used directly the trim
	// should be zero, i.e. the crosshair is central. If the shot is consistently off to one side in
	// play, this is the honest knob for it: nudge X, and note that re3 itself uses 0.53 because
	// GTA III's ped stands left of frame.
	float crosshairX = 0.5f;
	float crosshairY = 0.5f;

	// Build the ray from the CAMERA's position and slide its origin forward to the muzzle, as re3
	// does, instead of starting it at the muzzle and only borrowing the camera's direction.
	//
	// This is what removes PARALLAX. Starting at the gun leaves the ray a fixed lateral offset from
	// the line the crosshair actually covers, and because the camera orbits the player, the size of
	// that offset wanders with the angle between them. Measured in play on the old build: the
	// crosshairX needed to compensate swung between 0.5125 and 0.5300 across a 180 degree sweep,
	// non-monotonically. Fitting that curve was the wrong move, because the error also scales with
	// 1/distance, so any fit would be correct at one range only.
	//
	// This shipped OFF before, and for a good reason at the time: the camera position was picked by
	// ranking six candidate vec3s for being "most anti-parallel to the camera's forward vector",
	// the forward vector used to rank them was the one that has since turned out to be 4.4 degrees
	// wrong, +0x210 won, and with it the shots landed nowhere visible. The suspicion recorded
	// alongside - that ranking against a bad vector means little - was correct.
	//
	// It is on now because the camera position was MEASURED instead: +0x020 is 4.63 m from the
	// player on the far side, and it is the only candidate whose bearing to the look-at point
	// reproduces the stored pitch (+0x210 sits at the same height as its target, implying a level
	// camera while Alpha reads -11.9 degrees). See kVCSCamSourceOffset.
	bool useCameraOrigin = true;

	// The old route, kept switchable rather than deleted, because it is what shipped and A/B-ing it
	// in play costs nothing. Rebuilds the direction from CameraYaw/CameraPitch the old way,
	// including the FOV-scaled crosshair trim, instead of reading the stored basis.
	bool legacyAngleRay = false;

	// Residual yaw trim, degrees, applied on top of whichever route is selected. Expected to be
	// zero now that the basis is read rather than reconstructed; kept because "expected" is not
	// "measured in play", and a knob that turns out to want zero is itself a result.
	float aimYawOffsetDeg = 0.0f;

	// Read the weapon's own range out of CWeaponInfo instead of reusing the length of whatever
	// target the game had already computed. See kVCSWeaponInfoTablePtr for why that length cannot
	// be trusted: it is the weapon range on one path, the distance to a locked-on entity on
	// another, and the distance to the free-aim dummy on a third.
	bool useWeaponRange = true;

	// Used when useWeaponRange is off, or when the weapon record cannot be read. Also the floor:
	// a redirected ray is never shorter than this, because a ray that stops short of what the
	// crosshair is on looks exactly like a ray pointed somewhere else.
	float fallbackRange = 60.0f;
};

VCSFireHookSettings &FireHookSettings();

// The hook body itself. Referenced by the replacement table in Core/HLE/ReplaceTables.cpp, which
// is the only reason it is not static.
int Hook_vcs_weapon_raycast();

// Runs on the instruction after the raycast returns, and does exactly one thing: puts the game's
// own shot source back if the hook above moved it. See kVCSWeaponRaycastDone.
int Hook_vcs_weapon_raycast_done();

}  // namespace VCS
