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

// The camera the player looks through on foot, driving, sailing and flying.
//
// WHY THIS REPLACES MOUSE LOOK RATHER THAN FIXING IT
//
// Mouse look used to write the game's orbit angles, CameraYaw and CameraPitch, and let the game's
// own camera modes build the view from them. Everything that was wrong with it was the game's camera
// doing what it was built to do with a number it was handed:
//
//   on foot    mode 4 holds its distance and steers around walls, so the camera sat on the ground
//              when you looked up and would not swing toward a wall you stood beside
//   driving    mode 18 integrates pitch, and a written pitch wound that integrator up until the
//              view pinned to the roof
//   flying     mode 18 follows the aircraft's own pitch, so tipping a helicopter forward tipped the
//              view down into its rotor
//
// None of those is a tuning problem, so this does not write the angles into those modes at all. It
// lets the game compute its camera, and then REPLACES THE RESULT, inside the game's own frame:
//
//   1. At 0x08a23a60 - a HOOKENTER replacement - the game's camera for this frame is in three stack
//      vectors. The prepare hook reads them, works out the pivot (the player, or the vehicle), the
//      direction the player is looking, and how far back the camera wants to be, and writes a fan
//      of rays from the pivot out past that point into a block of PSP memory.
//   2. At 0x08a23be8 the game is about to build CCamera's matrix. That `jal` is pointed at a small
//      program in the same block, which calls the game's own CWorld::ProcessLineOfSight once per
//      ray, and then reaches a second HOOKENTER - the finish hook - which pulls the camera in to
//      the nearest hit, writes Source, Front and Up over the game's three, and lets the jal carry
//      on to the function it was meant to call.
//
// So the collision is the game's, the timing is the game's - one pass per logic frame, with no
// vblank writes racing it - and everything downstream of the matrix, the renderer, the frustum, the
// streamer, the radar, sees the camera the player sees.
//
// What it is modelled on is San Andreas on PC: the camera orbits a point above the character, looks
// at it from wherever the mouse put it, and when something is in the way it moves IN along that line
// instead of going somewhere else. Look up and it slides down behind the player's legs; stand in an
// alley and it comes in close rather than refusing to turn.
//
// WHAT IT LEAVES TO THE GAME
//
// Any other camera mode: aiming, scopes, cutscenes, first person, the mounted gun, the drive-by. It
// owns modes 4, 18 and 22 in the on-foot, vehicle and aircraft contexts and nothing else. Handing a
// view over is a short blend each way, so aiming or a cutscene does not arrive as a cut.
//
// How far back it sits is its own, and that is a measured result rather than a preference. The first
// build learned the distance from the game's camera, and mode 4 turned out to be a camera on a string:
// it starts every frame from wherever the last camera was and only enforces a band, so it hands back
// whatever distance it was given. A vehicle is framed from its collision box instead - the same data
// mode 18 reads - so a bus is still framed like a bus, and Select's zoom level still scales it.
//
// Threading: both hooks run on the emu thread inside the game's frame; ChaseCamAddLook runs on the
// emu thread from CameraTick. Nothing here is touched from anywhere else.

namespace VCS {

struct VCSChaseCamSettings {
	// Off restores the game's own instruction at both points, and mouse look falls back to writing
	// the angles, the way it did before this existed.
	bool enabled = true;

	// --- On foot ---

	// Height of the pivot above the player's origin, which itself sits about 1.04 above the ground.
	// 0.6 is where the game's own on-foot camera looks (CCam+0x190).
	float footPivotHeight = 0.6f;
	// How far back from the pivot the camera sits with nothing in the way. 4.5 is where VCS's own
	// on-foot camera sits in the open: 4.63 from the player in ULUS10160_1.03_1.ppst.
	//
	// A setting and not read from the game, and that is a finding. Mode 4 is a camera on a string -
	// it starts each frame from wherever the last camera was and only enforces a band - so asked what
	// distance it wants, it answers with the one it was given. Measured: a learner fed from it sat at
	// 2.29 for a whole session, because the boot load had left the camera close.
	float footDistance = 4.5f;
	// How far up and down the view may tilt, radians. Up is limited only by what stays readable:
	// the camera is below the pivot there, and the ground brings it in.
	float footLookUp = 1.20f;
	float footLookDown = 1.35f;
	// Keep the camera out of parked cars. The player's own vehicle is never in play on foot.
	bool footCollideVehicles = true;

	// --- Vehicles and aircraft ---

	// Pivot height as a fraction of the collision box's top. 0.95 is where mode 18 looks on a bike.
	float vehiclePivotScale = 0.95f;
	// How far back: (base + perLength * box length) at the middle zoom level, scaled by the near and
	// far factors at the other two, then by the overall scale. Fitted so the motorbike fixture - a box
	// 1.98 long - gets the 6.26 its own camera measured at zoom 2.
	float vehicleDistanceBase = 5.0f;
	float vehicleDistancePerLength = 0.65f;
	float vehicleZoomNear = 0.8f;   // CCamera+0x798 reads 1
	float vehicleZoomFar = 1.35f;   // CCamera+0x798 reads 3
	float vehicleDistanceScale = 1.0f;
	float vehicleLookUp = 0.90f;
	float vehicleLookDown = 1.30f;
	// The pitch the view settles to behind a moving vehicle: slightly down, as the game's is.
	float vehicleRestPitch = -0.12f;
	// Swing back behind the vehicle once the mouse has been left alone and the vehicle is moving.
	bool vehicleRecentre = true;
	float recentreDelay = 1.5f;     // seconds of no look input
	float recentreRate = 2.0f;      // 1/seconds, at full urgency
	float recentreMinSpeed = 3.0f;  // world units a second
	// Q / E / both, and the bumpers, look left, right and behind the car - InVehicle only, since
	// they yaw an aircraft.
	bool glances = true;

	// --- Collision ---

	// 1 casts along the view only; 5 adds four rays spread to coneRadius at the camera, so the near
	// plane cannot clip into a wall the centre ray just misses.
	int rays = 5;
	float coneRadius = 0.25f;
	// How far short of a hit the camera stops, and the closest it will ever come.
	float collisionMargin = 0.2f;
	float minDistance = 0.35f;
	// A hit pulls the camera in at once. Everything else eases at this rate - coming back out past a
	// railing, or the wanted distance changing between a player and a car - or the view pumps.
	float easeOutRate = 3.0f;
	bool collideObjects = true;

	// --- Keeping the lens out of the player and the car ---
	//
	// The game's near clip plane is 0.9, set for a camera four metres out. This one comes in to a
	// metre or less, and at 0.9 the near plane cuts straight through whatever is in front of it - you
	// see the inside of the character and the car. So each frame the near plane is lowered to a
	// fraction of the camera's clearance from the player's body or the car's box, through the game's
	// own RwCameraSetNearClipPlane, and left at the game's value whenever that is already smaller.
	bool nearClip = true;
	float nearClipFactor = 0.6f;  // of the clearance; a 70 degree frustum's corners reach ~1.5x near
	float nearClipMin = 0.05f;
	// The player as a capsule for that clearance: this radius around his axis, from his feet to just
	// over his head. The car is its collision box.
	float footBodyRadius = 0.35f;
	// If the camera still ends up inside the player or the car - backed into a corner - it is lifted
	// out over the top, by this much, rather than left looking at the inside of a model.
	float subjectMargin = 0.12f;
	// In a vehicle the camera's POSITION may only orbit this far below the pivot. Looking further up
	// tilts the view instead of swinging the camera down under the car; the car slides down the screen,
	// as San Andreas does it.
	//
	// 0.35 rather than tight. At 0.08 - the first value - a bike was off the bottom of the screen by a
	// 0.6 look-up, which is a worse view than the one it replaced. On flat ground 0.35 still stops the
	// camera well clear: a car's pivot is ~1.4 above the ground, so the ground pulls it in to ~3.9 behind
	// the car's centre, 1.6 past its rear. Anything that gets closer is what the near plane and the lift
	// out are for.
	float vehicleOrbitUp = 0.35f;

	// --- Handing the view over ---

	float blendIn = 0.25f;          // seconds, from the game's camera to ours
	float blendOut = 0.15f;         // seconds, from ours back to whatever the game switched to
	float pivotSettleRate = 6.0f;   // 1/seconds, for the pivot jumping between a player and a car
	// Carry the game's camera shake - explosions, rough ground - over onto this camera.
	bool keepShake = true;
};

VCSChaseCamSettings &ChaseCamSettings();

// Put both hooks in place, or take them out when the setting is off. Called every tick from
// VCSGame, like the fire hook's install, and cheap once installed: it re-checks that the patch is
// still there, because loading a savestate replaces PSP memory underneath it.
void InstallChaseCam();

// Put the game's instructions back and give the block back. Called from VCS::Shutdown.
void RemoveChaseCam();

// Forget the view: the next frame it owns re-anchors to wherever the game has the camera.
void ChaseCamReset();

// Whether look input in this context belongs to the chase camera. False when it is not installed
// or is switched off, in the aiming context, and on a mounted gun - all places CameraTick keeps.
bool ChaseCamTakesLook(VCSInputContext context);

// Turn the view, in radians. Yaw is the direction the camera looks, counter-clockwise; pitch is up.
void ChaseCamAddLook(float yawRadians, float pitchRadians);

// The two replacement entries in ReplaceTables.cpp.
int Hook_vcs_camera_prepare();
int Hook_vcs_camera_finish();

// For the debugger.
struct VCSChaseCamStats {
	const char *status = "";
	bool installed = false;
	bool owning = false;
	const char *kind = "";
	int mode = -1;
	u64 frames = 0;
	u64 ownedFrames = 0;
	float yaw = 0.0f;
	float pitch = 0.0f;
	float gameDistance = 0.0f;
	float wantDistance = 0.0f;
	float allowedDistance = 0.0f;
	float distance = 0.0f;
	float clearance = 0.0f;
	float nearClip = 0.0f;
	int rays = 0;
	int hits = 0;
	float pivot[3] = {};
	float speed = 0.0f;
	float sinceLook = 0.0f;
	bool glancing = false;
	bool blendingOut = false;
	float blendIn = 0.0f;
	u32 block = 0;
};
void ChaseCamGetStats(VCSChaseCamStats *out);

}  // namespace VCS
