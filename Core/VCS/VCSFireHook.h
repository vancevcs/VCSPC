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
// Those games aim by RAYCASTING FROM THE CAMERA rather than along the ped's aim -
// CWeapon::FireInstantHit calls CCamera::Find3rdPersonCamTargetVector, which unprojects the
// crosshair pixel into a world ray, and the arm IK and crosshair are presentation on top. VCS
// has no such branch, but it turns out not to need one: the shot direction is writable at the
// raycast, so the same model can be imposed from outside.
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

struct VCSFireHookSettings {
	// Off by default. This changes where bullets go; it should be opted into.
	bool enabled = false;

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

	// Residual yaw trim, degrees, on top of the measured convention. Zero is correct as of the
	// in-play calibration - the 90 degrees this used to carry is now folded into the formula, since
	// a permanent slider for a fact that has been measured is just somewhere for it to hide.
	float aimYawOffsetDeg = 0.0f;

	// Horizontal crosshair position, as a fraction of screen width.
	//
	// The shot is offset from the camera's centre line because the CROSSHAIR is, and re3 does
	// exactly this: m_f3rdPersonCHairMultX is 0.53, right of centre because the ped stands in the
	// left of frame, and Find3rdPersonCamTargetVector unprojects that pixel rather than the middle
	// of the screen.
	//
	// Calibrated in play here: a 3.2 degree trim was needed, and re3's formula run at VCS's numbers
	// - (x - 0.5) * 0.9 * FOV * aspect, with FOV 70 and 480/272 - gives 3.36 degrees at 0.53 and
	// 3.2 at 0.529. That agreement is why this is computed rather than stored as a fixed angle:
	// the offset scales with FOV, so it stays correct through sniper zoom, which a constant would
	// not.
	float crosshairX = 0.53f;

	// Build the ray from the CAMERA's position and slide its origin to the muzzle, as re3 does,
	// instead of starting it at the muzzle and only borrowing the camera's direction.
	//
	// This is what removes PARALLAX. Starting at the gun leaves the ray a fixed lateral offset from
	// the line the crosshair actually covers - and because the camera orbits the player, the size
	// of that offset wanders with the angle between them. Measured in play: the crosshairX needed
	// to compensate swung between 0.5125 and 0.5300 across a 180 degree sweep, non-monotonically.
	// Fitting that curve was the wrong move, because the error also scales with 1/distance, so any
	// fit would be correct at one range only.
	bool useCameraOrigin = true;

	// Which field of CCam[0] holds that position. Six position-shaped vec3s sit behind the camera's
	// forward vector at follow-camera distance; +0x210 is the most exactly anti-parallel (177.6 deg
	// against 172.5 for +0x020), which is why it is the default. Tunable because "most
	// anti-parallel" is evidence, not proof - if parallax persists, +0x020, +0x050 and +0x090 are
	// the other candidates.
	u32 camSourceOffset = 0x210;

	// Flips the vertical axis. Also zeroed out by calibration: pitch is now used as-is, which is
	// what aimed correctly, rather than negated as the address notes imply.
	bool aimInvertPitch = false;
};

VCSFireHookSettings &FireHookSettings();

// The hook body itself. Referenced by the replacement table in Core/HLE/ReplaceTables.cpp, which
// is the only reason it is not static.
int Hook_vcs_weapon_raycast();

}  // namespace VCS
