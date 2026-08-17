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
};

VCSFireHookSettings &FireHookSettings();

// The hook body itself. Referenced by the replacement table in Core/HLE/ReplaceTables.cpp, which
// is the only reason it is not static.
int Hook_vcs_weapon_raycast();

}  // namespace VCS
