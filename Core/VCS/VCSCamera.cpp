// Copyright (c) 2026- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <cmath>
#include <mutex>

#include "Core/MIPS/MIPS.h"
#include "Core/MemMap.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSFireHook.h"
#include "Core/VCS/VCSChaseCam.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSMemory.h"

namespace VCS {

static const float kTwoPi = 6.28318530718f;

static VCSCameraSettings g_settings;

// Written on the input thread, drained on the emu thread.
static std::mutex g_deltaMutex;
static float g_pendingDx = 0.0f;
static float g_pendingDy = 0.0f;
// What the last TakeMouseDelta handed out, so a second consumer in the same frame gets the same
// movement rather than zero. Guarded by the same mutex.
static float g_lastTakenDx = 0.0f;
static float g_lastTakenDy = 0.0f;

static bool g_driving = false;

// Diagnostics for the debugger window. Plain counters, incremented from the input thread;
// exact ordering doesn't matter for what they're used for.
static u64 g_mouseSeen = 0;
static u64 g_mouseClaimed = 0;
static u64 g_mouseRejectedContext = 0;
static float g_maxAbsDx = 0.0f;      // largest single delta ever seen
static float g_sumAbsDx = 0.0f;      // total movement, to tell 'tiny' from 'none'
static u64 g_writes = 0;             // ticks that actually wrote a new yaw
static u64 g_writeFails = 0;         // ticks that tried to write and the write was rejected
static float g_lastWritten = 0.0f;

// The yaw and pitch this file asserts into the game's own angles - while aiming, on a mounted gun, and
// as plain mouse look when the chase camera is switched off. Everywhere else the view belongs to the
// chase camera, which never goes through these. Emu thread only.
static float g_desiredYaw = 0.0f;
static float g_desiredPitch = 0.0f;
// Where the game had the pitch when we ENTERED THIS CONTEXT. The clamp window is centred on this,
// so no baseline has to be known in advance.
//
// Captured once per context, NOT once per stroke. Re-anchoring per stroke makes the anchor follow its
// own output, and the window ratchets 0.7 rad at a time until it hits the game's own limit.
static float g_anchorPitch = 0.0f;
static bool g_haveAnchorPitch = false;

// Whether the player is strapped to a vehicle with a weapon - a mission's mounted gun.
// See the block over PedAttachedTo in VCSAddresses.h: it is the one state where CameraYaw
// is measured from the VEHICLE rather than from the world, so it is a different camera in
// every way that matters here, and the context cannot say so - the game reports no vehicle
// at all and the fork sees an ordinary on-foot player.
static bool g_wasAttachedGun = false;
// Last context we ran in, so a change (e.g. getting into a car) forces a fresh anchor.
static VCSInputContext g_lastContext = VCSInputContext::Unknown;
// Ticks left to keep asserting. Aiming and the mounted gun park it open for as long as they last;
// plain look lets it run out.
static int g_holdFrames = 0;
// ~60Hz ticks, so about three quarters of a second after the last movement.
static const int kLookHoldFrames = 45;

// The game stores yaw in [0, 2PI), and feeding it anything outside that makes the camera snap.
static float WrapYaw(float yaw) {
	yaw = std::fmod(yaw, kTwoPi);
	return yaw < 0.0f ? yaw + kTwoPi : yaw;
}

// How far pitch may travel from wherever the game had it when the look started - about 40
// degrees each way. Deliberately RELATIVE, so no baseline has to be known or assumed; an
// absolute limit once pinned the view where it could not be brought back down.
//
// This comment used to claim the vehicle baseline was about -1.55. Measured 2026-08-17: it is
// -0.11861 rad (-6.80 deg), close to the on-foot value. See the clamp in CameraTick.
static const float kPitchRange = 0.7f;

// An anchor is only believed if it looks like a pitch at all. A pitch is physically within +/-PI/2 -
// straight down to straight up - so anything past this is not one.
//
// This is a VALIDITY test on the anchor's source, not an absolute limit on where the player may
// look, and the difference matters: absolute look limits were the original vehicle bug. Reported
// 2026-08-17: stepping into a vehicle can leave this field holding a positive value of 5 rad or more
// (286 deg, impossible as a pitch - the entry transition evidently reuses it). Capturing that as the
// anchor puts the vehicle ceiling absurdly high, which lets pitch climb past the real baseline and
// brings the spring runaway straight back. Until the field looks like a pitch again, pitch is simply
// not driven.
static const float kMaxPlausiblePitch = 1.6f;

// The aim pitch the player has ASKED for, in Front's space rather than CameraPitch's, plus how
// close to it counts as arrived. See the solve in CameraTick.
static float g_intentPitch = 0.0f;
static bool g_haveIntentPitch = false;
// The lead to hold over the aim's real angle, for a given error.
//
// A hard +/-D bias is bang-bang control, and it chatters. Front chases a lever that leads it by D,
// so it eventually passes the intent; the error crosses zero, the sign flips, and the lever moves by
// TWO deadbands in one frame. Measured in play: yaw desired went +0.5798 -> +0.2398, a jump of
// 0.340 rad, against 2 * 0.164 = 0.328 predicted. That is the snap, and it is a limit cycle rather
// than a bug in any one value.
//
// Blending the bias to zero across a narrow band around zero error removes it by construction: the
// lever is continuous through the crossing, so the worst jump becomes 2 * blend instead of 2 * D.
// Outside the band the full lead still applies, so nothing is lost where it matters - which is the
// whole reason not to simply shrink D.
static float LeadBias(float error, float deadband, float blend) {
	if (blend <= 0.0f) {
		return error > 0.0f ? deadband : (error < 0.0f ? -deadband : 0.0f);
	}
	float t = error / blend;
	if (t > 1.0f) t = 1.0f;
	if (t < -1.0f) t = -1.0f;
	return deadband * t;
}

static float g_lastFrontYaw = 0.0f;
static float g_lastFrontPitch = 0.0f;
static bool g_haveLastFront = false;
// How much the aim must move between frames to count as moving rather than stalled. Well under a
// frame of ordinary aiming, well over float noise.
static const float kFrontMovedEps = 0.0008f;
static bool g_aimLeadApplied = false;
// Which side of the aim the open-loop yaw kick is currently parked on: -1, 0 or +1. Zero means no
// kick is applied, which is both the resting state and what a fresh stroke starts from.
static float g_yawKickSign = 0.0f;
// Intent travelled against that side since the last flip, for the hysteresis. Reset on every flip
// and on every stroke, so it only ever measures the CURRENT attempt to reverse.
static float g_yawKickAgainst = 0.0f;
// The radians actually folded into g_desiredYaw right now, rather than what the setting currently
// reads. Those differ the moment the slider is moved mid-stroke, and giving back a different amount
// than was added would leave the difference in the lever permanently - a slow drift that would look
// like the kick being wrong rather than like the bookkeeping being wrong.
static float g_yawKickLead = 0.0f;
// Front's yaw as the kick last saw it, and how many ticks it has sat still. Deliberately NOT the
// g_lastFrontYaw below: that one belongs to the closed-loop deadband solve and is only updated on
// ticks where that solve runs at all, so it is stale exactly when this needs it fresh.
static float g_kickFrontYaw = 0.0f;
static bool g_haveKickFront = false;
static int g_kickRestTicks = 0;
static float g_intentYaw = 0.0f;
static bool g_haveIntentYaw = false;
static const float kIntentEpsilon = 0.002f;




// The FOV the look sensitivity is tuned AT, in degrees.
//
// re3 and reVC divide by 80 here, and 80 is NOT a measurement of anything - III's own gameplay FOV
// is 70 too. It is just the constant their sensitivity defaults were tuned against, so their 0.0025
// already has the resulting 0.875 folded into it. Copying the 80 would therefore make every
// sensitivity in this fork 0.875x slower on a value that was settled in play, which is a retune
// wearing a feature's clothes.
//
// Dividing by what the game actually runs at makes the scale exactly 1.0 in ordinary play and only
// moves when something moves the FOV - which is the entire point of the setting.
static const float kBaseFOV = 70.0f;
// Bounds on a believable FOV read, for the same reason kMaxPlausiblePitch exists: a transient, a
// menu, or a frame caught mid-transition must cost one frame of unscaled input rather than a mouse
// that stops working.
static const float kMinPlausibleFOV = 5.0f;
static const float kMaxPlausibleFOV = 150.0f;
// ...and on the scale itself, so even an FOV inside those bounds cannot take the mouse away.
static const float kFOVScaleMin = 0.15f;
static const float kFOVScaleMax = 2.0f;

VCSCameraSettings &CameraSettings() {
	return g_settings;
}

// ---------------------------------------------------------------------------------------------
// The game's aim response model
// ---------------------------------------------------------------------------------------------
//
// This is the piece that makes free aim feel like a mouse rather than a thumbstick, and it is a
// direct transcription of what the game does with the look axis, taken out of its own code.
//
// The weapon-aim camera is CCam mode 45, dispatched from the table at 0x08b7ed88 into the Process
// function at 0x089a341c. Once per frame it does this, with `axis` being the value the pad
// accessor at 0x0898bb4c returns - i.e. the nub, or the d-pad pair, depending on CameraInputMode:
//
//     n     = axis / 128                                          0x089a37d4
//     d     = n * |n| * (FOV / FOVref) * 0.04 * timeStep          0x089a37f0 .. 0x089a3804
//     alpha = pow(|axis| < 2 ? 0.5 : 0.9, timeStep)               0x089a38cc .. 0x089a3920
//     inc   = alpha * inc + (1 - alpha) * d * 0.8                 0x089a3924 .. 0x089a3940
//     Beta += inc                                                 0x089a3960 .. 0x089a3968
//
// (Mode 11, the scoped modes, is the same shape with different constants - a signed square, a
// smoother, the same saturation. One model covers both; the difference lands as a mild sensitivity
// change between scoped and unscoped aim, which is welcome anyway.)
//
// Three things in there are what "feels like a controller stick" actually means:
//
//   n * |n|     A SQUARE. Aim travel goes with the sum of the squares of the per-frame mouse
//               deltas, not with total mouse travel. Slow, careful movement produces almost
//               nothing; a flick produces far too much and then clips. This is a shape, so no
//               sensitivity setting has ever been able to fix it - and that is exactly the
//               history this code has: every previous attempt retuned a scale.
//   alpha       A LAG, and worse, a GLIDE. At a typical timestep alpha is about 0.84, so the
//               increment needs six frames to reach the rate asked for, and coasts for six more
//               after the mouse stops. Mice do not coast.
//   +/-1        SATURATION, which truncates fast movements outright.
//
// Inverting all three turns the axis into a position control - the same thing writing CameraYaw
// directly achieves for mouse look, but reached through the mechanic the game actually reads,
// which is the only way that works here. There is no stored aim angle to write; see the note on
// AimYaw in VCSAddresses.h for how that was settled.
//
// The FOV term is deliberately NOT cancelled. Wanting the rotation to scale with FOV is what
// every mouse-aimed shooter does - zooming in should aim finer - and because we want the applied
// rotation to be proportional to FOV as well, the term divides out of the solve entirely. That is
// why nothing here reads the FOV.

// The game has TWO aim responses, and which one is live changes the numbers enough to matter.
// The mode jump table at 0x08b7ed88 decides:
//
//   modes 7, 8, 34, 45, 46, 47  ->  Process 0x089a341c
//       d     = (axis/128) * |axis/128| * (FOV/80) * 0.04 * timeStep     0x089a37f0
//       inc   = alpha*inc + (1-alpha) * d * 0.8                          0x089a3938, gp-0x3448
//       alpha = 0.9 normally, 0.5 for |axis| < 2                         gp-0x3450, gp-0x344c
//
//   modes 11, 28                ->  Process 0x0899c6a0
//       d     = looksens^2 * axis * |axis| * (FOV/80) / 14 * timeStep    0x0899d2e4 .. 0x0899d2fc
//       inc   = alpha*inc + (1-alpha) * d                                no extra gain here
//       alpha = 0.8 normally, 0.5 for |axis| < 2                         gp-0x34b0, gp-0x34ac
//
// Modelling the wrong alpha is not a rounding error. 0.9 where the game uses 0.8 makes our idea of
// its momentum decay too slowly, so the anti-glide correction pushes harder than it should - and
// over-cancelling is the aim moving BACKWARDS at the end of a movement.
static const float kAimAlphaA = 0.9f;      // modes 7/8/34/45/46/47
static const float kAimAlphaB = 0.8f;      // modes 11/28
static const float kAimAlphaSmall = 0.5f;  // both, for |axis| < 2
static const float kAimSmallAxis = 2.0f / 128.0f;

// Rate for the first family: 0.04 (0x089a37ec) times the 0.8 increment gain (gp-0x3448).
static const float kAimRateA = 0.032f;

// The second family folds no gain in, but its axis is NOT pre-divided by 128 - it squares the raw
// axis - so converting to our -1..1 deflection means multiplying by 127^2.
static const float kAimRateBAxisSq = 16129.0f / 14.0f;

// What the game does to our deflection before it reaches any of the above. sceCtrl quantises
// -1..1 onto the 0..255 byte, which lands full deflection on 127 of a possible 128; the game then
// scales by AimAxisScale, but only in a weapon camera mode.
static const float kNubQuantise = 127.0f / 128.0f;

// Read 1.668 in an in-game savestate. Only used if TimeStep is unset or reads as non-positive,
// which it does outside gameplay - see the entry in VCSAddresses.h.
static const float kAimDefaultTimeStep = 1.668f;

// How much undelivered rotation may be held over. Past this the movement is dropped instead of
// queued, because a long queue IS the drifting-after-you-stop feel this whole model exists to
// remove - better to under-travel on an impossible flick than to keep turning for a second
// afterwards. In frames' worth of full-deflection rotation.
static const float kMaxCarryFrames = 3.0f;

static VCSAimAxis g_aimX;
static VCSAimAxis g_aimY;
static float g_aimTimeStep = kAimDefaultTimeStep;

// The model must step exactly once per GAME frame, and neither part of that is automatic.
//
// One caller asks for a deflection - ApplyAnalog, writing the nub. It was two while the d-pad
// pair carried a second aim channel, which is why the answer is cached per tick rather than
// simply computed: a second asker in the same tick has to get the same answer, not a second step.
//
// And a tick is NOT a game frame. We run from hleEnterVblank at ~60Hz; this game runs its logic
// at 30fps (TimeStep reads 1.668, which is 50/30 in GTA's 50fps-reference units). Left alone, that
// breaks the model twice over:
//
//   the mirror advances twice per game frame, decaying as alpha^2 where the game's own increment
//   decays as alpha, so our idea of what the game is carrying drifts away from the truth - and
//   that mirror is exactly what the anti-glide correction is computed from;
//
//   worse, the mouse movement of the first tick is solved, written, and then overwritten by the
//   second tick's write before the game ever samples the pad. Half the input, silently gone.
//
// So the model runs on the game's clock instead: FrameCounter says when a logic frame has actually
// happened, and between them the accumulated mouse movement is left alone and the last deflection
// is simply re-asserted, which is what the game will read anyway.
//
// With FrameCounter unset this degrades to solving every tick, which is what it did before the
// frame counter was found - wrong in the ways above, but working.
static u64 g_aimFrame = 0;
static u64 g_aimSolvedFrame = (u64)-1;
static float g_aimOutX = 0.0f;
static float g_aimOutY = 0.0f;
// Last acceleration multiplier applied, for the debugger.
static float g_aimAccelGain = 1.0f;

// Consecutive GAME frames with no mouse movement at all.
//
// This is what decides whether a stroke has ended, and it is deliberately a property of the mouse
// rather than of either axis: the player stopping is one physical event, not two. Tracking it
// per-axis would call a pure sideways stroke "still" vertically and brake an axis mid-movement,
// which is the exact failure this exists to remove.
static int g_aimStillFrames = 0;

static u32 g_lastGameFrame = 0;
static bool g_haveGameFrame = false;
static bool g_aimReady = true;

// Last camera mode seen, so a change can be noticed. See AimModelBeginFrame.
static u32 g_lastCamMode = 0;
static bool g_haveCamMode = false;
// Diagnostics: ticks that were a real game frame, versus ticks that were not.
static u64 g_gameFrames = 0;
static u64 g_skippedTicks = 0;

// The largest deflection the live channel can carry to the game.
//
// Always full deflection now: the nub is the only aim channel, and sceCtrl clamps it there before
// the game ever sees it. It was a variable while the d-pad pair could carry more - those fields
// are int16 and the game's accessor never clamps them - and the model still asks, because the
// question is a real one and the answer being constant is a property of the channel rather than
// of the model.
float AimChannelLimit() {
	return 1.0f;
}

static float AimTimeStep() {
	const std::optional<float> t = ReadAddrFloat(VCSAddr::TimeStep);
	// Non-positive means we are not in gameplay (it reads 0.0 at the menu), and a wild value means
	// we read the wrong thing. Either way, fall back rather than divide by it.
	if (t && *t > 0.01f && *t < 100.0f) {
		g_aimTimeStep = *t;
	}
	return g_aimTimeStep;
}

// Everything about the game's current aim response, read rather than assumed.
//
// `rate` is radians of increment per frame at full deflection, i.e. the coefficient on n*|n| once
// the deflection has been through sceCtrl's quantisation and the game's own axis scale. `alphaBase`
// is the smoothing factor before it is raised to the timestep.
struct AimGameResponse {
	float alphaBase = kAimAlphaA;
	float rate = kAimRateA;
	// FOV / 80, the factor both responses scale their angular rate by. Kept separately because the
	// WANTED rotation has to be scaled by it too - see the note in AimAxisStep.
	float fovScale = 1.0f;
	bool live = false;      // false = could not read the game; the caller falls back
};

// KNOWN GAP, deliberately left: this uses the X axis scale for BOTH axes, and the game does not.
// The accessors at 0x0898de90 and 0x0898df08 multiply by CPad+0xd0 and CPad+0xd4 respectively,
// under the same weapon-mode test, and on foot those read 1.05 and 0.50 - so Y is modelled about
// twice as responsive as it really is.
//
// Not fixed here because it is not what the drive-by needed in the end (that bypasses this
// function entirely, see driveBySensitivity) and because correcting it moves on-foot vertical aim
// by a factor of two, which is a retune of a path that currently works rather than a bug fix.
static AimGameResponse ReadAimResponse(float timeStep) {
	AimGameResponse r;

	const std::optional<u32> mode = ReadAddrU32(VCSAddr::CamMode);
	const std::optional<float> fov = ReadAddrFloat(VCSAddr::CamFOV);
	if (!mode || !fov || *fov <= 1.0f) {
		return r;   // not in gameplay, or the addresses are unset
	}

	// The camera must be in a mode that actually integrates the look axis, or none of this applies.
	//
	// This matters far more than it looks. CCam+0x130 only means "smoothed aim increment" in these
	// modes; the on-foot follow camera is mode 15, which never reads the look axis at all, so
	// whatever sits in that field then is not ours to interpret. Reading it anyway meant that on
	// the first frame of an aim - before the camera had switched modes - the model could find a
	// stale non-zero value, call it momentum, and shove the stick hard to cancel it. With a
	// run-and-gun weapon that deflection swings the character, which is the snap on aim entry.
	//
	// Diagnosed from the outside first, and precisely: the bug appeared whenever the increment
	// readout was anything other than exactly 0.00000 as aim began, and never when it was zero.
	switch (*mode) {
	case 7: case 8: case 11: case 28: case 34: case 45: case 46: case 47:
		break;
	default:
		return r;   // live stays false - the caller keeps its own mirror and cancels nothing
	}

	// FOV is divided by 80 in both responses - f26 at 0x089a3528 and the 0x42a00000 at 0x0899d2d0.
	const float fovScale = *fov / 80.0f;
	r.fovScale = fovScale;

	// How much of our deflection actually reaches the squaring step. The weapon-mode axis scale is
	// the one that bit: unmodelled, the game's response came out about 10% stronger than predicted,
	// which the anti-glide correction turned into a visible backwards snap.
	float axisScale = kNubQuantise;
	const std::optional<u32> weaponMode = ReadAddrU32(VCSAddr::WeaponCamMode);
	if (weaponMode && (*weaponMode == 45 || *weaponMode == 11)) {
		const std::optional<float> s = ReadAddrFloat(VCSAddr::AimAxisScale);
		if (s && *s > 0.01f && *s < 10.0f) {
			axisScale *= *s;
		}
	}
	// It is squared, because the response is.
	const float axisSq = axisScale * axisScale;

	if (*mode == 11 || *mode == 28) {
		const std::optional<float> look = ReadAddrFloat(VCSAddr::LookSensitivity);
		const float s = (look && *look > 0.0f) ? *look : 0.007f;
		r.alphaBase = kAimAlphaB;
		r.rate = s * s * kAimRateBAxisSq * fovScale * axisSq * timeStep;
	} else {
		r.alphaBase = kAimAlphaA;
		r.rate = kAimRateA * fovScale * axisSq * timeStep;
	}
	r.live = r.rate > 1e-9f;
	return r;
}

float AimAxisStep(VCSAimAxis *axis, float want, VCSAddr incAddr) {
	const float timeStep = AimTimeStep();

	const AimGameResponse resp = ReadAimResponse(timeStep);
	// Full-deflection rotation for one frame - the unit the carry is bounded in, and the scale
	// everything below works against.
	const float perFrame = resp.live ? resp.rate : (kAimRateA * timeStep);

	// The game's ACTUAL smoothed increment, not our prediction of it.
	//
	// This is the whole reason the aim used to snap back. Predicting it meant every small error -
	// the wrong alpha for the live camera mode, an unmodelled axis scale, a deflection that got
	// clamped after we recorded it - accumulated into a number that was then used to decide how
	// hard to push back when the mouse stopped. Over-estimate the game's momentum and the
	// correction overshoots, and overshoot is the aim travelling backwards.
	//
	// Reading it means the correction is anchored to the truth every single frame, and no error can
	// accumulate at all. The mirror survives only as a fallback for when this cannot be read.
	// Only while the camera is in a mode that owns this field - see ReadAimResponse. Outside those
	// modes it holds something else entirely, and treating it as momentum is how the aim snapped.
	if (resp.live) {
		const std::optional<float> trueInc = ReadAddrFloat(incAddr);
		if (trueInc) {
			axis->mirror = *trueInc;
		}
	}

	// Whether the mouse moved at all this frame, before the carry muddies it. Decides at the
	// bottom whether undelivered rotation is still wanted.
	const bool moving = want != 0.0f;

	// Scale the WANTED rotation by FOV, so that a mouse count moves the crosshair the same distance
	// ACROSS THE SCREEN whatever the zoom - which is what every mouse-aimed shooter does, and what
	// makes a scope usable.
	//
	// This was the sniper and RPG bug. The game's own rate already scales with FOV, and the model
	// divides that out when solving; scaling the target by it too is what makes the two cancel. Get
	// it wrong and the ANGULAR rate is held constant instead, so scoping in - which drops the FOV
	// from 70 to a fraction of that - multiplies the on-screen speed by the zoom factor. The
	// deflection then saturates, the carry piles up behind it, and the scope takes off on its own.
	//
	// It reads as acceleration rather than as plain over-sensitivity because of that carry: the
	// movement the model cannot deliver this frame is still owed, so it keeps arriving after the
	// mouse has stopped asking for it.
	want *= resp.fovScale;

	want += axis->carry;

	// Solve for the deflection. The game will compute
	//     inc = alpha * inc + (1 - alpha) * rate * n * |n|
	// and add inc to the angle, so the n that makes inc equal `want` is found by rearranging and
	// then undoing the square.
	//
	// Note q can come out with the opposite sign to `want`, and that is the point rather than a
	// bug: when the mouse stops, want is 0 while inc is not, so the correct deflection is
	// backwards - it cancels the smoother's momentum in one frame instead of letting it coast.
	const float alphaGuess = std::pow(resp.alphaBase, timeStep);
	const float denom = (1.0f - alphaGuess) * perFrame;

	// How much of that momentum to cancel - and, just as importantly, WHETHER TO CANCEL AT ALL YET.
	//
	// Cancelling is only correct once the stroke has actually ended. A single frame with no mouse
	// movement does not mean that: a game frame is 33ms, and slow careful aiming genuinely produces
	// zero-count frames in the middle of a stroke. Braking on those is what made the aim stutter,
	// and it is a timing error, so no amount of calibration helps - the first version of this had
	// to be detuned to a tenth of its strength to be usable, which was treating the symptom.
	//
	// Waiting for a few consecutive still frames separates the two cases and lets the cancellation
	// run at full strength where it belongs, at the end of a movement.
	float cancel = g_settings.aimCancel;
	if (cancel < 0.0f) cancel = 0.0f;
	if (cancel > 1.0f) cancel = 1.0f;
	if (g_aimStillFrames < g_settings.aimStopDelay) {
		cancel = 0.0f;
	}
	// Nothing to cancel if we could not confirm the camera is in a mode whose increment we
	// understand - the mirror is then our own prediction, not the game's state.
	if (!resp.live) {
		cancel = 0.0f;
	}

	float n = 0.0f;
	if (denom > 1e-9f) {
		const float q = (want - alphaGuess * axis->mirror * cancel) / denom;
		n = std::sqrt(q < 0.0f ? -q : q);
		if (q < 0.0f) {
			n = -n;
		}
	}

	// Saturate at what the LIVE CHANNEL can actually deliver.
	//
	// Getting this wrong is not a small error, because the clamped value is what the mirror is
	// then built from. Solve to 3.0, have ApplyAnalog clamp the nub to 1.0, and the mirror records
	// a rotation NINE times the one that happened - the square sees to that. The next frame the
	// model faithfully corrects for movement the game never made, and the correction is backwards.
	// It reads as the aim snapping back a pixel whenever you move, and it is worth recognising
	// because the cause is nowhere near the symptom.
	//
	// Only the d-pad channel can exceed full deflection. The nub goes through sceCtrl, which
	// clamp_u8's it long before the game sees it, so on the nub the ceiling is 1 no matter what
	// the slider says.
	const float limit = AimChannelLimit();
	if (n > limit) n = limit;
	if (n < -limit) n = -limit;

	// Now work out what the game will ACTUALLY do with the deflection we settled on, and carry the
	// difference. Done against the clamped value rather than the ideal one, because the shortfall
	// from saturation is exactly what the carry is for.
	//
	// alpha is picked here rather than above because it depends on the deflection, which we did
	// not know yet. Guessing the normal branch and correcting here costs at most one frame of
	// error on the boundary, and the carry takes that back on the next frame anyway.
	const float absN = n < 0.0f ? -n : n;
	const float alpha = std::pow(absN < kAimSmallAxis ? kAimAlphaSmall : resp.alphaBase, timeStep);
	float applied = alpha * axis->mirror + (1.0f - alpha) * perFrame * n * absN;

	// Hard floor under the whole thing: never predict - and therefore never aim for - a rotation
	// that reverses the aim when the mouse did not ask it to.
	//
	// Everything above is now read from the game rather than assumed, so this should not trigger.
	// It is here because over-cancellation is the one failure with a distinctive and unpleasant
	// signature - the aim walking backwards at the end of every movement - and because the cause
	// sits several layers away from the symptom, which cost a lot of time to track down twice.
	// Coasting slightly is always the better error.
	if (!moving && applied * axis->mirror < 0.0f) {
		applied = 0.0f;
	}

	// Only fall back to carrying our own prediction when the game's real value could not be read;
	// otherwise the next call overwrites this from memory anyway.
	axis->mirror = applied;

	float carry = want - applied;
	const float maxCarry = kMaxCarryFrames * perFrame;
	if (carry > maxCarry) carry = maxCarry;
	if (carry < -maxCarry) carry = -maxCarry;

	// On a frame where the mouse did not move, the carry gets read for its SIGN rather than kept
	// or dropped wholesale, because the two signs mean opposite things:
	//
	//   same sign as the rotation      rotation still owed, that saturation prevented delivering.
	//                                  The player has stopped, so they no longer want it - paying
	//                                  it out is exactly the coasting this model exists to remove.
	//                                  A mouse is self-correcting anyway: a flick that lands short
	//                                  gets finished by the next movement.
	//   opposite sign                  we delivered too MUCH, and this is the correction. Still
	//                                  wanted, and dropping it leaves the overshoot standing.
	//
	// Simulated against the game's own formula, degrees of rotation continuing after the mouse
	// stops - keep-everything / drop-everything / this rule:
	//
	//     fast, 20 counts/frame, 1x     9.17   3.67   3.59
	//     hard flick, 40/frame, 1x      9.17   2.53   2.18
	//     hard flick, 40/frame, 3x      3.63   5.02   4.80
	//
	// It wins or ties everywhere except a hard flick at full boost, and that case is dominated by
	// the mirror cancellation rather than by the carry.
	if (moving || carry * applied <= 0.0f) {
		axis->carry = carry;
	} else {
		axis->carry = 0.0f;
	}

	axis->lastAxis = n;
	return n;
}

float AimAccelGain() {
	return g_aimAccelGain;
}

void AimModelState(const VCSAimAxis **x, const VCSAimAxis **y, float *timeStep) {
	*x = &g_aimX;
	*y = &g_aimY;
	*timeStep = g_aimTimeStep;
}

void AimModelReset() {
	g_aimX.Reset();
	g_aimY.Reset();
	g_aimOutX = 0.0f;
	g_aimOutY = 0.0f;
	// Reset high, not to zero: a fresh aim starts with the mouse already at rest, so the next
	// stroke must not open inside the grace period and skip its own stop.
	g_aimStillFrames = 1000;
}

void AimModelBeginFrame() {
	// Only open a new frame for the model when the GAME has advanced one. Between logic frames the
	// pending mouse movement is deliberately left to accumulate and the previous deflection stands.
	const std::optional<u32> frame = ReadAddrU32(VCSAddr::FrameCounter);
	if (!frame) {
		// Address unset, or not in gameplay. Fall back to solving every tick.
		g_haveGameFrame = false;
		g_aimReady = true;
		g_aimFrame++;
		return;
	}

	if (!g_haveGameFrame) {
		// First sight of the counter. Treat it as a frame so aiming can start immediately rather
		// than waiting for the next one.
		g_haveGameFrame = true;
		g_lastGameFrame = *frame;
		g_aimReady = true;
		g_aimFrame++;
		g_gameFrames++;
		return;
	}

	// != rather than > because the counter resets across a load, and a reset is a new frame just
	// as much as an increment is.
	g_aimReady = *frame != g_lastGameFrame;
	g_lastGameFrame = *frame;
	if (g_aimReady) {
		g_aimFrame++;
		g_gameFrames++;
	} else {
		g_skippedTicks++;
	}

	// A camera mode change means the field the model treats as momentum has just changed owner, so
	// whatever is in it right now describes the camera we are leaving, not the one we are entering.
	// Drop the accumulated state and hold the stop off until the mouse has genuinely been still
	// again, rather than braking against a number inherited from the previous mode.
	const std::optional<u32> mode = ReadAddrU32(VCSAddr::CamMode);
	if (mode && (!g_haveCamMode || *mode != g_lastCamMode)) {
		g_haveCamMode = true;
		g_lastCamMode = *mode;
		g_aimX.Reset();
		g_aimY.Reset();
		g_aimStillFrames = 0;
	}
}

bool AimModelReady() {
	return g_aimReady;
}

void AimLastDeflection(float *x, float *y) {
	*x = g_aimOutX;
	*y = g_aimOutY;
}

void AimFrameStats(u64 *gameFrames, u64 *skippedTicks) {
	*gameFrames = g_gameFrames;
	*skippedTicks = g_skippedTicks;
}

int AimStillFrames() {
	return g_aimStillFrames;
}

// Turns this frame's mouse movement into a pair of deflections, applying the model when it is on
// and the old proportional mapping when it is not. `scale` is the legacy deflection-per-count.
//
// Shared by both aim channels on purpose. They write different memory - the nub at CPad+0x2, the
// d-pad pair at CPad+0x12..0x18 - but the game funnels both through the same accessor and the same
// response, so there is exactly one model and one place to change it.
void AimDeflectionFromMouse(float dx, float dy, float scale, bool invertY,
		float *outX, float *outY) {
	if (g_aimSolvedFrame == g_aimFrame) {
		*outX = g_aimOutX;
		*outY = g_aimOutY;
		return;
	}

	const float ySign = invertY ? -1.0f : 1.0f;

	// Sniper and RPG bypass the model entirely - see aimScopedLinear. Keyed on the weapon camera
	// mode rather than on the active camera mode, because this is a property of the WEAPON and it
	// is known the moment one is equipped, not only once aiming has begun.
	bool scoped = false;
	if (g_settings.aimScopedLinear) {
		const std::optional<u32> weaponMode = ReadAddrU32(VCSAddr::WeaponCamMode);
		scoped = weaponMode && (*weaponMode == 7 || *weaponMode == 8);
	}

	// The drive-by bypasses the model for a reason the scoped weapons do not share, so it gets its
	// own test rather than joining theirs: they skip it because they were already linear, this
	// skips it because the mechanism the model inverts is not running. Both camera modes, matching
	// DriveByAimActive - see driveBySensitivity for the 551 samples of zeroed increments that put
	// this here.
	bool driveBy = false;
	if (g_settings.driveByMouseAim) {
		const std::optional<u32> camMode = ReadAddrU32(VCSAddr::CamMode);
		const std::optional<u32> weaponMode = ReadAddrU32(VCSAddr::WeaponCamMode);
		driveBy = camMode && weaponMode && *camMode == 11 && *weaponMode == 11;
	}

	if (driveBy) {
		g_aimX.Reset();
		g_aimY.Reset();
		const float k = g_settings.driveBySensitivity;

		// Divide the game's own axis scale back out, so a sideways sweep and a vertical one cover
		// the same ground. Guarded rather than trusted: these are read from the game and a zero
		// would divide the aim into infinity, which is a stuck stick rather than a fast one.
		float sx = 1.0f, sy = 1.0f;
		if (g_settings.driveByMatchAxes) {
			const std::optional<float> rx = ReadAddrFloat(VCSAddr::AimAxisScale);
			const std::optional<float> ry = ReadAddrFloat(VCSAddr::AimAxisScaleY);
			if (rx && *rx > 0.01f && *rx < 10.0f) sx = *rx;
			if (ry && *ry > 0.01f && *ry < 10.0f) sy = *ry;
		}

		g_aimOutX = dx * k / sx;
		g_aimOutY = -dy * k * ySign / sy;
		if (g_aimOutX > 1.0f) g_aimOutX = 1.0f;
		if (g_aimOutX < -1.0f) g_aimOutX = -1.0f;
		if (g_aimOutY > 1.0f) g_aimOutY = 1.0f;
		if (g_aimOutY < -1.0f) g_aimOutY = -1.0f;
		g_aimSolvedFrame = g_aimFrame;
		*outX = g_aimOutX;
		*outY = g_aimOutY;
		return;
	}

	if (scoped) {
		g_aimX.Reset();
		g_aimY.Reset();
		const float k = g_settings.aimScopedSensitivity;
		g_aimOutX = dx * k;
		g_aimOutY = -dy * k * ySign;
		if (g_aimOutX > 1.0f) g_aimOutX = 1.0f;
		if (g_aimOutX < -1.0f) g_aimOutX = -1.0f;
		if (g_aimOutY > 1.0f) g_aimOutY = 1.0f;
		if (g_aimOutY < -1.0f) g_aimOutY = -1.0f;
		g_aimSolvedFrame = g_aimFrame;
		*outX = g_aimOutX;
		*outY = g_aimOutY;
		return;
	}

	if (!g_settings.aimResponseModel) {
		// Legacy path: deflection straight in proportion to the movement. Kept only so the two can
		// be compared in play. Clear the model's state while it isn't running, or turning it back
		// on mid-aim would resume from a stale mirror.
		g_aimX.Reset();
		g_aimY.Reset();
		g_aimOutX = dx * scale;
		g_aimOutY = -dy * scale * ySign;
	} else {
		// Count still frames before stepping either axis, so both see the same view of whether the
		// stroke has ended. Done here rather than per-axis on purpose - see g_aimStillFrames.
		if (dx != 0.0f || dy != 0.0f) {
			g_aimStillFrames = 0;
		} else if (g_aimStillFrames < 1000) {
			g_aimStillFrames++;
		}
		// Mouse acceleration: fast movements get extra gain, slow ones are left alone.
		//
		// Worth being clear about what this is, because the model exists to REMOVE a nonlinearity.
		// The game's own response is a signed square - an acceleration curve nobody chose, that
		// crushes slow movement and blows up fast movement, and cannot be tuned because it is a
		// shape rather than a scale. The model cancels it. This adds a curve back, but a chosen
		// one, bounded, and applied to the wanted ROTATION rather than to the stick - so precision
		// at low speed is untouched and only deliberate movements are amplified.
		//
		// Gain is computed from the whole 2D speed, not per axis, so a diagonal flick accelerates
		// the same as a horizontal one and the direction is preserved exactly.
		float gain = 1.0f;
		if (g_settings.aimAccel > 0.0f) {
			const float speed = std::sqrt(dx * dx + dy * dy);
			gain = 1.0f + g_settings.aimAccel * speed;
			if (gain > g_settings.aimAccelMax) {
				gain = g_settings.aimAccelMax;
			}
		}
		const float sens = g_settings.aimSensitivity * gain;
		g_aimAccelGain = gain;

		g_aimOutX = AimAxisStep(&g_aimX, dx * sens, VCSAddr::CamAimIncX);
		g_aimOutY = AimAxisStep(&g_aimY, -dy * sens * ySign, VCSAddr::CamAimIncY);
	}

	g_aimSolvedFrame = g_aimFrame;
	*outX = g_aimOutX;
	*outY = g_aimOutY;
}

// Whether to CLAIM the mouse at all - i.e. take the delta away from PPSSPP's own mouse-to-analog
// handling and accumulate it here. Only makes sense while the player is actually controlling a
// character or vehicle. In menus the mouse should be left alone, and with an Unknown context we
// don't know enough to act at all - same rule the button mapping follows.
//
// Aiming is included even though it doesn't move the camera: the delta is still ours, it just
// gets spent on the reticle instead. Dropping the claim there would hand the movement to
// PPSSPP's mouse-to-analog path, which drives the same stick we do and would fight us.
static bool ContextWantsMouse(VCSInputContext context) {
	switch (context) {
	case VCSInputContext::OnFoot:
	case VCSInputContext::InVehicle:
	case VCSInputContext::InAircraft:
	case VCSInputContext::Aiming:
		return true;
	case VCSInputContext::Menu:
		// Claimed so the map can be DRAGGED with it - see MapDragTick. Claiming is not steering:
		// ContextDrivesCamera says no for this context, so the delta is taken away from PPSSPP's
		// own mouse-to-analog path and spent on the map instead of on a view nobody can see.
		return true;
	default:
		return false;
	}
}

// Whether the claimed delta becomes a camera rotation, as opposed to something else.
//
// Free aim is the one exception, and it is the whole point of the aiming model. There the nub
// stops being "where the player walks" and becomes "where the reticle points", and the reticle is
// what decides the direction of the shot. Rotating the camera instead would move the view while
// leaving the gun pointing wherever the game last put it - so mouse look during free aim isn't
// merely stylistically wrong, it cannot aim at all.
//
// This is the same lesson vehicle glances already taught: go through the mechanic the game
// actually reads, not the camera angle that happens to look similar.
//
// Note this asks ReticleActive rather than testing for the Aiming context. Holding aim normally
// gives lock-on, not free aim, and there the camera should keep working exactly as on foot -
// there is no reticle to hand the mouse to.
//
// The lock-on TOGGLE is the exception to the paragraph above, and the last piece of that mode.
// Ordinary lock-on is something the player passes through on the way to free aim, so the camera
// keeping its mouse look there is right. The toggle is the opposite: it says "leave this to the
// game", and while it is on the game is steering the camera around the target it picked - so a
// written yaw fights the follow logic every frame and the view judders between the two. Both
// axes go, not just yaw: pitch is equally the game's while it frames a target.
//
// The delta is still CLAIMED here - see ContextWantsMouse - and CameraTick drains it whether it
// drives anything or not, so nothing accumulates and dumps into the camera on the way out.
static bool ContextDrivesCamera(VCSInputContext context) {
	if (context == VCSInputContext::Aiming && LockOnModeActive()) {
		return false;
	}
	// The game's own menu is up: there is no view to turn, and the delta belongs to the map drag.
	if (context == VCSInputContext::Menu) {
		return false;
	}
	// The drive-by stands the camera down for exactly the reason free aim does, and it is worth
	// saying plainly because the context here is InVehicle, where mouse look is otherwise right:
	// the nub is the aim in that seat, so a delta spent on the camera turns the view and leaves
	// the gun behind. That is the reported bug, in one line.
	return ContextWantsMouse(context) && !ReticleActive(context) && !DriveByAimActive(context);
}

bool HandleMouseDelta(float dx, float dy) {
	// Counted before any gating, so the debugger can tell "no mouse events arrive at all" apart
	// from "they arrive but something rejects them" - two completely different bugs.
	g_mouseSeen++;
	if (dx < 0.0f ? -dx > g_maxAbsDx : dx > g_maxAbsDx) {
		g_maxAbsDx = dx < 0.0f ? -dx : dx;
	}
	g_sumAbsDx += dx < 0.0f ? -dx : dx;

	// Cheapest gates first, and the ones that guarantee no effect on any other game.
	if (!IsActive() || !g_settings.enabled) {
		return false;
	}
	// Aiming needs no address at all - it spends the delta on the stick, not on memory - so it
	// must not be gated behind CameraYaw the way actual mouse look is. This matters for the
	// "works with any subset of the address table" rule: on a build where CameraYaw was never
	// found, aiming should still work.
	const VCSInputContext context = GetCurrentContext();
	if (!IsAddrSet(VCSAddr::CameraYaw) && context != VCSInputContext::Aiming) {
		return false;
	}
	if (!ContextWantsMouse(context)) {
		g_mouseRejectedContext++;
		return false;
	}

	g_mouseClaimed++;

	std::lock_guard<std::mutex> guard(g_deltaMutex);
	g_pendingDx += dx;
	g_pendingDy += dy;
	return true;
}

void AddLookDelta(float dx, float dy) {
	std::lock_guard<std::mutex> guard(g_deltaMutex);
	g_pendingDx += dx;
	g_pendingDy += dy;
}

void TakeMouseDelta(float *dx, float *dy) {
	std::lock_guard<std::mutex> guard(g_deltaMutex);
	*dx = g_pendingDx;
	*dy = g_pendingDy;
	g_pendingDx = 0.0f;
	g_pendingDy = 0.0f;
	// Remembered so a second consumer in the same frame can have the same movement instead of
	// finding an empty buffer - see PeekMouseDelta.
	g_lastTakenDx = *dx;
	g_lastTakenDy = *dy;
}

void PeekMouseDelta(float *dx, float *dy) {
	std::lock_guard<std::mutex> guard(g_deltaMutex);
	*dx = g_lastTakenDx;
	*dy = g_lastTakenDy;
}

// What to scale a mouse count by, given how far the game currently has the view zoomed.
//
// FOR THE DIRECT-WRITE PATHS ONLY - mouse look, camera-driven aim, and free aim's AimYaw. Those
// write an angle into memory, so nothing else scales them and this is the only FOV term in play.
//
// The RETICLE path must never call this, and the reason is worth stating rather than trusting a
// naming convention to convey. There, the mouse becomes a stick deflection and the GAME turns that
// into a rotation with a rate that already carries FOV/80 (the f26 at 0x089a3528). AimAxisStep
// deliberately keeps that term instead of cancelling it - `want *= resp.fovScale` - precisely so a
// count moves the crosshair a constant distance on screen. Calling this there too would square the
// factor and halve the sniper's sensitivity again on top of the halving the game already did, which
// is the sniper/RPG bug that note was written about, arrived at from the other direction.
float FOVLookScale() {
	if (!g_settings.scaleByFOV) {
		return 1.0f;
	}
	const std::optional<float> fov = ReadAddrFloat(VCSAddr::CamFOV);
	if (!fov || *fov < kMinPlausibleFOV || *fov > kMaxPlausibleFOV) {
		return 1.0f;
	}
	float scale = *fov / kBaseFOV;
	if (scale < kFOVScaleMin) scale = kFOVScaleMin;
	if (scale > kFOVScaleMax) scale = kFOVScaleMax;
	return scale;
}

// Drives the gun direction directly in free aim, instead of nudging the nub.
//
// The nub is a rate control with the game's acceleration on it, so aiming through it feels like
// a thumbstick at any sensitivity - that is the mechanism showing, not the tuning. AimYaw is the
// stored direction, so writing it gives 1:1 mouse feel.
//
// It MUST be written every frame. The game recomputes this value continuously, so a write that
// isn't re-asserted is undone within a frame or two - exactly the trap CameraYaw has, and the
// reason probing it over the WebSocket debugger only ever produced a stutter. Anchor on entry,
// integrate the mouse into our own copy, and assert that copy every tick.
static bool g_aimHeld = false;
static float g_desiredAim = 0.0f;

void AimTick(VCSInputContext context) {
	const bool want = g_settings.enabled && g_settings.mouseLookInFreeAim &&
		FreeAimActive(context) && IsAddrSet(VCSAddr::AimYaw);

	if (!want) {
		g_aimHeld = false;
		return;
	}

	float dx, dy;
	TakeMouseDelta(&dx, &dy);

	if (!g_aimHeld) {
		// Anchor to wherever the game currently has the gun, so aiming never snaps on entry.
		const std::optional<float> cur = ReadAddrFloat(VCSAddr::AimYaw);
		if (!cur) {
			return;
		}
		g_desiredAim = *cur;
		g_aimHeld = true;
	}

	// Same sign convention as the camera: the game's angles grow counter-clockwise, so moving
	// the mouse right has to decrease this for the gun to follow the mouse.
	g_desiredAim += dx * g_settings.sensitivity * FOVLookScale() *
		(g_settings.invertX ? 1.0f : -1.0f);

	WriteAddrFloat(VCSAddr::AimYaw, g_desiredAim);
}

// How much to add to the yaw lever on top of the mouse, to get the aim MOVING.
//
// The open-loop half of the deadband story - see aimYawKick for the measurement and for why this is
// shaped nothing like aimYawDeadband despite carrying the same number. Returns the CHANGE to apply
// this frame, which is zero on all but the two frames that matter: the first push of a stroke, and a
// deliberate reversal inside one.
//
// Nothing here reads the game. That is the entire safety argument: with no measured error there is
// no sign to flip on its own, so the limit cycle that the closed-loop version produced is not merely
// tuned away, it is unrepresentable.
static float YawKickStep(VCSInputContext context, float yawStep) {
	if (context != VCSInputContext::Aiming || g_settings.aimYawKick <= 0.0f) {
		// Ordinary mouse look writes CameraYaw into a camera with no integrator in front of it -
		// mode 15 never reads the look axis at all - so there is no stiction to break and a lead
		// would be a plain 9.5 degree error.
		return 0.0f;
	}
	// Not for the scoped weapons, for the same reason pitch's lead is not: modes 7 and 8 drive the
	// reticle directly and linearly, so there is nothing to break loose and a lead is just an
	// offset. Feeding one to a camera that has none is how sniper and RPG got the twitch.
	if (ScopedWeaponActive()) {
		return 0.0f;
	}
	if (yawStep == 0.0f) {
		return 0.0f;
	}

	const float dir = yawStep > 0.0f ? 1.0f : -1.0f;
	if (dir == g_yawKickSign) {
		// Already leading the right way. Hold it, and forget any partial attempt to reverse - the
		// hysteresis measures ONE sustained push against the lead, not the net of a wobble.
		g_yawKickAgainst = 0.0f;
		return 0.0f;
	}

	if (g_yawKickSign != 0.0f) {
		// Pushing against the lead. Spend the travel first, and only move the lead once the player
		// has genuinely committed to the other direction - see aimYawKickHysteresis.
		g_yawKickAgainst += yawStep > 0.0f ? yawStep : -yawStep;
		if (g_yawKickAgainst < g_settings.aimYawKickHysteresis) {
			return 0.0f;
		}
	}

	// Move the lead to this side. From rest that is one kick; from the other side it is two, which
	// is the real cost of a reversal and is paid on a frame the player is already moving.
	//
	// Expressed as "what it should be, minus what is already in there" rather than as a multiple of
	// the kick, so a slider moved mid-stroke lands correctly on the next push instead of stacking a
	// stale lead under a fresh one.
	const float want = dir * g_settings.aimYawKick;
	const float step = want - g_yawKickLead;
	g_yawKickLead = want;
	g_yawKickSign = dir;
	g_yawKickAgainst = 0.0f;
	return step;
}


void CameraTick(VCSInputContext context) {
	g_driving = false;

	// Always drain, even when we're not going to use it. Otherwise movement made while the player
	// was in a menu would be applied in one jump on the way out. In the Aiming context this comes
	// back as zero because ApplyAnalog already took it for the reticle, which is exactly what we
	// want - the delta is spent once, on one thing.
	//
	// The exception is a tick where the aim owns the delta and is deliberately HOLDING it until
	// the game's next logic frame. Draining it here would be the same input loss the holding
	// exists to prevent, just moved one function along.
	// The drive-by holds the delta on the same terms the reticle does - it goes through the same
	// model, so it has the same gap between our tick rate and the game's logic frame to bridge.
	float dx = 0.0f, dy = 0.0f;
	if ((!ReticleActive(context) && !DriveByAimActive(context)) || AimModelReady()) {
		TakeMouseDelta(&dx, &dy);
	}

	// "Is any device driving the camera", not "is the mouse enabled".
	//
	// This used to read g_settings.enabled alone, which was the same question while the mouse was
	// the only thing that could look around. Now the pad's right stick fills the same accumulator,
	// and a player on a pad has every reason to turn Mouse control off - it is presented as a
	// mouse setting and its help talks about mouse look - which would have left the right stick
	// silently dead.
	//
	// It cannot let a delta through that nobody asked for: each device gates its own contribution
	// at the entry point (HandleMouseDelta on the mouse's flag, ApplyPadLook on the pad's), so
	// with both off nothing fills the accumulator and there is nothing here to apply.
	if ((!g_settings.enabled && !PadSettings().enabled) || !ContextDrivesCamera(context)) {
		g_holdFrames = 0;
		g_lastContext = context;
		return;
	}

	// Read ONCE per tick and shared by both axes, so a frame in which the game happens to move
	// the FOV mid-tick cannot scale yaw and pitch by different amounts and skew a diagonal.
	const float fovScale = FOVLookScale();

	// On foot, driving and flying, the view is the chase camera's - see VCSChaseCam.h. It takes the
	// turn in radians and does everything else inside the game's own frame, so nothing below runs and
	// nothing is written into the game's angles from here.
	//
	// The mapping is the one mouse look has always had. The game's yaw grows counter-clockwise
	// (measured: mouse right raised the value, which turned the view LEFT), so the natural mapping
	// needs a negative sign and invertX flips away from it. Pitch is up-positive and invertY flips it.
	if (ChaseCamTakesLook(context)) {
		const float yawStep = dx * g_settings.sensitivity * fovScale *
			(g_settings.invertX ? 1.0f : -1.0f);
		const float pitchStep = dy * g_settings.sensitivity * g_settings.verticalGain * fovScale *
			(g_settings.invertY ? -1.0f : 1.0f);
		ChaseCamAddLook(yawStep, pitchStep);
		// Whatever this path was asserting belongs to a camera the player has left. The next aim
		// anchors afresh, to where the chase camera has been keeping the game's angles.
		g_holdFrames = 0;
		g_haveAnchorPitch = false;
		g_lastContext = context;
		return;
	}

	// What is left for this path: aiming, the mounted gun, and - when the chase camera is switched off
	// or could not install - plain mouse look, which writes the game's own angles.

	// Force a re-anchor when the context changes, so the desired angles never carry from one camera
	// into another.
	if (context != g_lastContext) {
		g_holdFrames = 0;
		g_lastContext = context;
		// A new camera means a new baseline, and it is the only thing that may move the anchor.
		g_haveAnchorPitch = false;
		// The kick belongs to the weapon camera's stiction, so it does not cross into another
		// camera. Dropping the SIGN rather than retracting is deliberate: whatever is folded into
		// Beta is the old camera's business, and the new context re-anchors to live below anyway.
		g_yawKickSign = 0.0f;
		g_yawKickAgainst = 0.0f;
		g_yawKickLead = 0.0f;
		g_haveKickFront = false;
		g_kickRestTicks = 0;
	}

	// Being strapped to a vehicle is a camera change that no context change announces, so it gets
	// the same treatment as one. Without this the desired yaw carries across the boundary, and it
	// carries across as the WRONG KIND OF NUMBER: a world heading on the way in, a small signed
	// offset from the vehicle's nose on the way out. Either way the first write asserts a value
	// the receiving camera cannot use, which is how a mounted gun came to sit pinned at its limit
	// however far the mouse moved.
	//
	// It matters more than the context version because there is no expiry to rescue it: on a
	// mounted gun the hold parks open, so the anchor taken on the last stroke before boarding
	// would be asserted for as long as the ride lasts.
	const bool attachedGun = ReadAddrU32(VCSAddr::PedAttachedTo).value_or(0) != 0;
	if (attachedGun != g_wasAttachedGun) {
		g_wasAttachedGun = attachedGun;
		g_holdFrames = 0;
		g_haveAnchorPitch = false;
		g_haveIntentPitch = false;
		g_haveIntentYaw = false;
		g_haveLastFront = false;
		g_yawKickSign = 0.0f;
		g_yawKickAgainst = 0.0f;
		g_yawKickLead = 0.0f;
		g_haveKickFront = false;
		g_kickRestTicks = 0;
	}

	// Aiming and the mounted gun hold the angles for as long as they last. The moment this stops
	// asserting, the gun's aim point comes off the live camera again - see AimIntentRay - and that is
	// the loop that walks the view.
	const bool holdsForever = context == VCSInputContext::Aiming || attachedGun;

	// Pitch is never written into a vehicle camera. Mode 18 integrates it, and a written pitch winds
	// that integrator up until the view pins to the roof - the chase camera exists partly to be rid of
	// that, so the fallback does not bring it back.
	const bool inVehicleOfAnyKind = context == VCSInputContext::InVehicle ||
		context == VCSInputContext::InAircraft;
	const bool havePitch = IsAddrSet(VCSAddr::CameraPitch) && !inVehicleOfAnyKind;

	// A NOTE ON THE SNAP AFTER A STROKE, so the obvious fix is not tried again.
	//
	// The solve leads the game by the deadband, and that lead is only retracted on frames where the
	// error is recomputed - i.e. frames with movement. Stop the mouse and the last solved value is
	// re-asserted for the rest of the hold window, about 9.4 degrees past where the player stopped,
	// so the aim drifts on and then jumps back when the window releases.
	//
	// Running the same solve on still frames with a zero step LOOKS like the fix and is a runaway.
	// The lever is written directly, so `desired = live + error + bias` means live advances by at
	// least `bias` every single frame for as long as the error survives - 9.4 degrees a frame, sixty
	// times a second. On movement frames that is bounded because Front chases the value we just
	// jumped it to and the error falls under the epsilon within a frame or two; with nothing else
	// moving, it just accumulates. Tried, and it made aiming unusable.
	//
	// Whatever fixes the snap has to retract the lead ONCE, not re-apply it in a loop.
	// Free aim is asserted from its first frame, not from the first mouse count - the RMB half of the
	// drift. With nothing asserted, the gun's aim point comes off the live camera (see AimIntentRay),
	// which is exactly the loop that walks the view, so pressing aim and leaving the mouse alone walked
	// it as well. A zero-length stroke anchors to wherever the game has the camera and starts the hold;
	// with no step and no kick in it, it writes back exactly what it read.
	//
	// Re-taken once if the camera MODE changes before the mouse has moved. Pressing aim hands the
	// follow camera to the aim camera over a couple of frames - measured in the trace: mode 4 to 11,
	// yaw moving 0.1 rad across two frames - and an anchor taken mid-handover would hold the view
	// where the old camera left it.
	static bool s_aimAnchorOnly = false;
	static u32 s_aimAnchorCamMode = 0;
	const std::optional<u32> camModeNow = ReadAddrAsU32(VCSAddr::CamMode);
	if (context == VCSInputContext::Aiming && s_aimAnchorOnly && g_holdFrames > 0 &&
			camModeNow && *camModeNow != s_aimAnchorCamMode) {
		g_holdFrames = 0;
	}
	const bool aimAnchor = context == VCSInputContext::Aiming && g_holdFrames == 0;
	if (dx != 0.0f || (dy != 0.0f && havePitch) || aimAnchor) {
		if (dx != 0.0f || dy != 0.0f) {
			s_aimAnchorOnly = false;
		}
		if (g_holdFrames == 0) {
			if (dx == 0.0f && dy == 0.0f) {
				s_aimAnchorOnly = true;
				s_aimAnchorCamMode = camModeNow.value_or(0);
			}
			// Starting a fresh look - anchor to wherever the game currently has the camera, so
			// we never snap from a stale value.
			const std::optional<float> current = ReadAddrFloat(VCSAddr::CameraYaw);
			if (!current) {
				return;
			}
			g_desiredYaw = *current;
			// A new stroke re-reads where the aim actually is, so the intent never starts from a
			// stale value the game has since moved.
			g_haveIntentPitch = false;
			g_haveIntentYaw = false;
			g_haveLastFront = false;
			// The kick belongs to a stroke, not to the camera. A fresh stroke re-anchors desired to
			// live above, so any kick still folded into the old value is gone with it - carrying the
			// SIGN across would apply the next one relative to a lead that no longer exists.
			g_yawKickSign = 0.0f;
			g_yawKickAgainst = 0.0f;
			g_yawKickLead = 0.0f;
			g_haveKickFront = false;
			g_kickRestTicks = 0;

			// Desired always re-syncs to live, so a stroke never snaps from a stale value. The
			// ANCHOR does not: it is the fixed origin of the clamp window for as long as we stay in
			// this camera. Both are gated on the value actually looking like a pitch - see
			// kMaxPlausiblePitch.
			const std::optional<float> pitch = ReadAddrFloat(VCSAddr::CameraPitch);
			if (pitch && std::fabs(*pitch) <= kMaxPlausiblePitch) {
				g_desiredPitch = *pitch;
				if (!g_haveAnchorPitch) {
					g_anchorPitch = g_desiredPitch;
					g_haveAnchorPitch = true;
				}
			}
		}

		// DISPROVEN, and recorded rather than deleted because the reasoning was sound and the
		// conclusion was not. Mode 45 really does clamp Alpha every logic frame (0x089a39b8..
		// 0x089a3a08), and integrating past a limit the game will not honour really does buy dead
		// travel. So this adopted the game's value whenever it differed from ours by more than 0.6
		// degrees.
		//
		// In play it made the Y axis need a hard flick to move at all, while X stayed immediate. The
		// premise was wrong in one word: the clamp is not the ONLY thing that moves Alpha behind our
		// back. The aim camera adjusts it every frame, so the adopt fired constantly rather than only
		// at the limits, and each frame it threw away the movement just added. Pitch could only move
		// when one frame of mouse beat one frame of the game's correction - which is a flick.
		//
		// That failure is worth more than the fix was: it PROVES the game touches Alpha every frame in
		// the aim camera and does not touch Beta the same way. That asymmetry is a much better
		// candidate for circles coming out square than the clamp ever was, because it makes vertical
		// inherently laggier than horizontal. The next thing to try is asserting aim pitch once per
		// GAME LOGIC FRAME instead of once per vblank - the same fix, for the same reason, that settled
		// the vehicle pitch runaway - rather than reading the game's value back.


		// No believable anchor means no clamp window, and without the window the vehicle ceiling
		// does not exist - so pitch stays undriven rather than driven unbounded.
		if (havePitch && g_haveAnchorPitch) {
			// Stored negated relative to the camera matrix: raising this value tilts the view UP.
			// Default is INVERTED pitch by preference - mouse up looks down, flight-sim style.
			// invertY switches to the conventional mouse-up-looks-up mapping.
			//
			// The two vertical multipliers are EXCLUSIVE, not composed: aiming is a measured channel
			// whose gain was settled in play at 1.0 with the deadband solve in place, and ordinary
			// look now carries re3's 1.9. Multiplying them would hand aiming a 1.9x nobody asked for.
			const float pitchGain = (context == VCSInputContext::Aiming) ?
				g_settings.aimPitchGain : g_settings.verticalGain;
			const float pitchStep = dy * g_settings.sensitivity * pitchGain * fovScale *
				(g_settings.invertY ? -1.0f : 1.0f);

			// SOLVE FOR THE LEAD instead of scaling the input, which is the whole difference between
			// this and the hypersensitive version it replaces.
			//
			// What the player sees is Front. Front does not move until CameraPitch leads it by about
			// 0.164 rad, and then it follows. Scaling the mouse up crosses that gap sooner but makes
			// everything past it twice as fast - the deadband is a constant, so paying for it with
			// gain overcharges every movement that was never near it.
			//
			// So integrate the INTENT in Front's own space, at 1:1 with the mouse exactly like yaw,
			// then work out what CameraPitch has to be for the game to deliver it: the intent, plus
			// the deadband, in the direction we are asking for. The lead appears in full on the first
			// frame of a stroke, so pitch starts immediately, and it collapses back to nothing the
			// moment Front catches up - so it never adds speed, only removes the delay.
			//
			// Anchored to the LIVE Front every frame, so nothing accumulates and a wrong deadband
			// costs one frame of lead rather than compounding. Same reasoning as the aim response
			// model reading CamAimInc instead of predicting it.
			// NOT for the scoped weapons. Sniper and RPG are weapon camera modes 7 and 8, and this
			// file already records what is different about them: they are manually aimed and their
			// reticle is driven DIRECTLY and LINEARLY, with no integrator between the angle and the
			// aim. So there is no stiction for the lead to break - the camera follows the moment it
			// is written.
			//
			// Feeding a lead to a camera that has none is the yaw limit cycle again, one axis over:
			// push 0.165 ahead, Front arrives immediately, the error flips sign, push back. Reported
			// in play as sniper and RPG twitching like crazy, and it is the same shape as the snap
			// that cost four builds on yaw - which is exactly why it is worth naming rather than
			// re-deriving.
			//
			// The general rule this is the third instance of: a correction shaped like the game's
			// response is only valid where that response is. aimResponseModel had to learn it,
			// aimScopedCamera had to learn it, and now so does the deadband.
			std::optional<float> frontPitch;
			if (context == VCSInputContext::Aiming && g_settings.aimPitchDeadband > 0.0f &&
				!ScopedWeaponActive()) {
				const std::optional<float> fz = ReadFloat(kVCSCam0 + kVCSCamFrontOffset + 8);
				if (fz && *fz >= -1.0f && *fz <= 1.0f) {
					frontPitch = std::asin(*fz);
				}
			}
			if (!frontPitch) {
				g_haveIntentPitch = false;
				g_desiredPitch += pitchStep;
			}

			// Pitch does not wrap - it is a look angle, so clamp rather than wrap.
			//
			// The limits are RELATIVE TO THE ANCHOR, never absolute. A fixed +/-0.9 pinned the view
			// where the clamp overrode every attempt to pitch back down; anchoring the window to
			// wherever the game had the camera is correct in every mode without needing to know any
			// baseline, which is the point.
			float lo, hi;
			if (attachedGun) {
				// The mounted gun's own arc, read from the ped rather than derived from an anchor:
				// mode 45 clamps pitch to [-PedAimPitchDown, +PedAimPitchUp] whatever we write, so
				// matching it is what stops the intent running away past a stop it cannot pass.
				// Asymmetric on purpose - a door gunner looks DOWN. GON_C4 asks for 10 up and 55
				// down, and the script sets both in degrees (04CF, 04D0).
				const float up = ReadAddrFloat(VCSAddr::PedAimPitchUp).value_or(0.0f);
				const float down = ReadAddrFloat(VCSAddr::PedAimPitchDown).value_or(0.0f);
				hi = (up > 0.001f && up < 3.2f) ? up : kPitchRange;
				lo = (down > 0.001f && down < 3.2f) ? -down : -kPitchRange;
			} else {
				lo = g_anchorPitch - kPitchRange;
				hi = g_anchorPitch + kPitchRange;
			}
			if (frontPitch) {
				// The window belongs to the INTENT here - it is the thing that represents where the
				// player is asking to look. Clamping the lever instead would cap the lead and bring
				// back exactly the dead travel this exists to remove.
				if (!g_haveIntentPitch) {
					g_intentPitch = *frontPitch;
					g_haveIntentPitch = true;
				}
				g_intentPitch += pitchStep;
				if (g_intentPitch > hi) g_intentPitch = hi;
				if (g_intentPitch < lo) g_intentPitch = lo;

				const float error = g_intentPitch - *frontPitch;
				// The epsilon stops a settled aim from sitting on a permanent lead and jittering
				// around it; below it, ask for exactly what the game already has.
				// THE LEAD IS STICTION, NOT AN OFFSET - and that correction came from the numbers.
				//
				// Measured across a snap: Front moved +0.165 while CameraPitch/Yaw moved -0.340, i.e.
				// exactly one deadband against two. So Front does not settle a deadband SHORT of the
				// lever, it converges onto it - which means a lever parked at `intent + D` lands the
				// aim at `intent + D`, overshooting by a full deadband, and the correction then
				// swings back by 2D. That is the snap, and holding the lead is what causes it.
				//
				// D is what it takes to get the aim MOVING. Once it is moving, the honest command is
				// the intent itself. So the kick is applied only while the aim is stalled, and drops
				// the moment it is under way.
				const bool moving = g_haveLastFront &&
					std::fabs(*frontPitch - g_lastFrontPitch) > kFrontMovedEps;
				const float kick = LeadBias(error, g_settings.aimPitchDeadband, g_settings.aimLeadBlend);
				const float bias = (g_settings.aimLeadOnStallOnly && moving) ? 0.0f : kick;
				g_desiredPitch = g_intentPitch + bias;
				if (bias != 0.0f) g_aimLeadApplied = true;
				g_lastFrontPitch = *frontPitch;
			} else {
				if (g_desiredPitch > hi) g_desiredPitch = hi;
				if (g_desiredPitch < lo) g_desiredPitch = lo;
			}

			// LEASH the desired pitch to what the aim camera actually delivered.
			//
			// Measured in play, and it is the whole "Y needs a hard flick" problem:
			//
			//     before pitching   Front pitch -0.0442   CameraPitch -0.0442   off  0.00 deg
			//     still no movement Front pitch -0.0442   CameraPitch +0.1198   off -9.40 deg
			//     first movement    Front pitch -0.0390   CameraPitch +0.1278   off -9.56 deg
			//
			// The two start IDENTICAL, so they are normally one number. Then Alpha ran 9.4 degrees
			// while Front did not move at all. Mode 45 clamps Alpha every logic frame (0x089a39b8..
			// 0x089a3a08) against bounds it keeps on its own stack, and builds Front from the
			// CLAMPED value - then we overwrite the field again at vblank, so the number we read
			// back is ours and the direction the player sees is the game's. Every count spent past
			// the limit had to be paid back before anything moved again, which is the dead travel.
			//
			// Front is the readable truth here: `Front.z` is the aim's real pitch, and in this
			// camera it equals Alpha exactly until the clamp separates them. So rather than guess
			// the game's bounds - they are stack locals, not memory we can read - keep our value
			// from getting more than `aimPitchLeash` ahead of what actually happened.
			//
			// This is a CLAMP, not an adopt, and that distinction is the entire lesson from the
			// version of this that had to be reverted. Adopting the game's value every frame threw
			// away the movement just added and left pitch needing a flick to move at all. A clamp
			// never fires while the two agree - which is the normal case, measured at 0.00 degrees
			// apart - and only trims the runaway that the player cannot see anyway.
			if (context == VCSInputContext::Aiming && g_settings.aimPitchLeash > 0.0f) {
				const std::optional<float> fz = ReadFloat(kVCSCam0 + kVCSCamFrontOffset + 8);
				if (fz && *fz >= -1.0f && *fz <= 1.0f) {
					const float frontPitch = std::asin(*fz);
					const float leashLo = frontPitch - g_settings.aimPitchLeash;
					const float leashHi = frontPitch + g_settings.aimPitchLeash;
					if (g_desiredPitch > leashHi) g_desiredPitch = leashHi;
					if (g_desiredPitch < leashLo) g_desiredPitch = leashLo;
				}
			}
		}
		// The game's yaw grows counter-clockwise (measured: mouse right raised the value, which
		// turned the view LEFT), so the natural mapping needs a negative sign. invertX flips
		// away from that correct default, it isn't the default itself.
		const float yawStep = dx * g_settings.sensitivity * fovScale *
			(g_settings.invertX ? 1.0f : -1.0f);

		// Yaw carries the SAME deadband as pitch - reported at the same 9.4 degrees - so it gets the
		// same treatment: integrate the intent in Front's space at 1:1, then solve for the Beta that
		// makes the game deliver it.
		//
		// One structural difference, and it is what makes this simpler rather than harder. Front's
		// pitch happens to EQUAL CameraPitch, so pitch could solve in absolute terms. Front's yaw and
		// CameraYaw differ by some offset this fork has measured inconsistently (4.4 degrees in a
		// savestate, 59 in a stale trace) and never pinned down. So work in DIFFERENCES: the error is
		// measured in Front's space, where it is unambiguous, and applied as an increment to the live
		// CameraYaw. Whatever the constant between the two spaces is, it cancels - which is a better
		// answer than measuring it, because it cannot go stale.
		//
		// Anchoring the LEVER to the live value each frame would normally be the read-modify-write
		// this file warns about, but it is not one here: the accumulator is the INTENT, which holds
		// the player's cumulative request across frames. The lever is re-solved from it, so anything
		// the game does to Beta shows up as error and gets corrected rather than lost.
		std::optional<float> frontYaw;
		if (context == VCSInputContext::Aiming && g_settings.aimYawDeadband > 0.0f) {
			const std::optional<float> fx = ReadFloat(kVCSCam0 + kVCSCamFrontOffset);
			const std::optional<float> fy = ReadFloat(kVCSCam0 + kVCSCamFrontOffset + 4);
			if (fx && fy && (*fx != 0.0f || *fy != 0.0f)) {
				frontYaw = std::atan2(*fy, *fx);
			}
		}
		if (frontYaw) {
			const std::optional<float> liveYaw = ReadAddrFloat(VCSAddr::CameraYaw);
			if (liveYaw) {
				if (!g_haveIntentYaw) {
					g_intentYaw = *frontYaw;
					g_haveIntentYaw = true;
				}
				g_intentYaw += yawStep;
				// Yaw is circular, so the error has to be the SHORT way round or a stroke across the
				// wrap point would ask for most of a turn in the wrong direction.
				float error = g_intentYaw - *frontYaw;
				error = std::fmod(error + kTwoPi * 0.5f, kTwoPi);
				if (error < 0.0f) error += kTwoPi;
				error -= kTwoPi * 0.5f;
				// Same stiction model as pitch - see the note there.
				const bool moving = g_haveLastFront &&
					std::fabs(*frontYaw - g_lastFrontYaw) > kFrontMovedEps;
				const float kick = LeadBias(error, g_settings.aimYawDeadband, g_settings.aimLeadBlend);
				const float bias = (g_settings.aimLeadOnStallOnly && moving) ? 0.0f : kick;
				g_desiredYaw = *liveYaw + error + bias;
				g_aimLeadApplied = bias != 0.0f;
				g_lastFrontYaw = *frontYaw;
				g_haveLastFront = true;
			} else {
				g_desiredYaw += yawStep;
			}
		} else {
			// The plain accumulator, which is what the default configuration runs: aimYawDeadband is
			// 0, so frontYaw above is never populated and yaw always lands here. The kick rides on
			// THIS branch only - if the closed-loop deadband is ever turned back on it owns yaw
			// outright, and two leads on one lever would be the snap plus a constant.
			g_haveIntentYaw = false;
			g_desiredYaw += yawStep + YawKickStep(context, yawStep);
		}
		g_holdFrames = kLookHoldFrames;
	}

	// RETRACT THE LEAD ONCE, on the first still frame after a stroke.
	//
	// The lead is not an overshoot - the game moves Front only while the lever leads it by more than
	// the deadband, so a lever at `intent + D` lands Front on `intent` and stops. That part works.
	// What does not is what we leave in the field afterwards: when the hold window expires we stop
	// asserting, and CameraYaw is still parked a deadband past the aim. The game picks that stale
	// Beta up as its own and the view jumps.
	//
	// So drop the bias exactly once, the frame the mouse goes still, and let the ordinary re-assert
	// hold the honest value for the rest of the window. ONCE is the whole point: re-solving this
	// every frame is a runaway, because the lever is written directly and each pass adds another
	// deadband to it - see the note above the movement block. A one-shot has no loop to diverge.
	if (g_aimLeadApplied && dx == 0.0f && dy == 0.0f && g_holdFrames > 0 &&
		context == VCSInputContext::Aiming && g_settings.aimLeadRetract) {
		if (g_haveIntentPitch) {
			// Front's pitch IS CameraPitch, so the honest lever value is just the intent.
			g_desiredPitch = g_intentPitch;
		}
		if (g_haveIntentYaw) {
			const std::optional<float> fx = ReadFloat(kVCSCam0 + kVCSCamFrontOffset);
			const std::optional<float> fy = ReadFloat(kVCSCam0 + kVCSCamFrontOffset + 4);
			const std::optional<float> liveYaw = ReadAddrFloat(VCSAddr::CameraYaw);
			if (fx && fy && liveYaw && (*fx != 0.0f || *fy != 0.0f)) {
				float error = g_intentYaw - std::atan2(*fy, *fx);
				error = std::fmod(error + kTwoPi * 0.5f, kTwoPi);
				if (error < 0.0f) error += kTwoPi;
				error -= kTwoPi * 0.5f;
				g_desiredYaw = *liveYaw + error;
			}
		}
		g_aimLeadApplied = false;
	}

	// The open-loop kick's own one-shot retract. Separate from the block above on purpose: that one
	// belongs to the Front-space solve and only runs when an intent exists, and the kick deliberately
	// has no intent to key on. Same shape, same guarantee - it fires once, because applying it clears
	// the sign that armed it.
	//
	// GATED ON THE AIMING CONTEXT, and that was missing for one build. Without it, releasing aim
	// mid-stroke and then stopping retracted a lead that belongs to the weapon camera out of the
	// ON-FOOT one - 9.5 degrees, arriving a moment after the player stopped moving, in a camera that
	// never had the stiction the lead exists for. The block above has always carried this test; the
	// omission was in the copy, not in the reasoning.
	if (g_yawKickSign != 0.0f && dx == 0.0f && dy == 0.0f && g_holdFrames > 0 &&
		context == VCSInputContext::Aiming && g_settings.aimYawKickRetract) {
		g_desiredYaw -= g_yawKickLead;
		g_yawKickSign = 0.0f;
		g_yawKickAgainst = 0.0f;
		g_yawKickLead = 0.0f;
		g_haveKickFront = false;
		g_kickRestTicks = 0;
	}

	// RE-ARM THE KICK when the game's own aim comes to rest, so the next stroke breaks it loose
	// again instead of inheriting a sign with nothing behind it.
	//
	// The lead releases the moment the view breaks loose - that is the measurement the kick is built
	// on - so once the aim has stopped there is no lead in front of it. The SIGN outlives the radians,
	// and a stroke that finds a stale sign either gets no kick at all (same direction: `dir == sign`
	// takes the early return, and the player spends a deadband) or two of them (reversal: the flip
	// costs `want - lead` = 2*kick, measured in play at 0.34 out for 0.04 of hysteresis in). One
	// staleness, both faults. See aimYawKickRearmTicks.
	//
	// FRONT NOT MOVING is the test, not the mouse being still, and that distinction is the whole
	// correction over the build that counted still mouse ticks and made this worse: a slow sweep has
	// whole ticks with no mouse counts in it, and re-arming on one of those stacks a second kick on
	// the one already working. Front stops when the AIM has arrived, which is the actual question.
	//
	// A DIFFERENCE between consecutive ticks, used once, to answer yes or no. Nothing is solved from
	// the read and nothing is written, so this is not the closed loop the file warns about - and
	// because it never uses Front's absolute value, the quadrant constant between Beta's space and
	// Front's cannot get it wrong.
	if (context == VCSInputContext::Aiming && g_yawKickSign != 0.0f &&
		g_settings.aimYawKickRearmTicks > 0) {
		const std::optional<float> fx = ReadFloat(kVCSCam0 + kVCSCamFrontOffset);
		const std::optional<float> fy = ReadFloat(kVCSCam0 + kVCSCamFrontOffset + 4);
		if (fx && fy && (*fx != 0.0f || *fy != 0.0f)) {
			const float frontYaw = std::atan2(*fy, *fx);
			// Short way round, or a stroke across the wrap point reads as most of a turn and the
			// aim never looks still.
			float moved = std::fmod(frontYaw - g_kickFrontYaw + kTwoPi * 0.5f, kTwoPi);
			if (moved < 0.0f) moved += kTwoPi;
			moved -= kTwoPi * 0.5f;
			const bool aimStill = g_haveKickFront && std::fabs(moved) <= kFrontMovedEps;
			g_kickFrontYaw = frontYaw;
			g_haveKickFront = true;
			// Both halves: the player has stopped asking AND the aim has stopped arriving. Mouse
			// alone is the mistake described above; Front alone would count the ticks at the start
			// of a stroke where the fresh kick has not broken the aim loose yet, and re-arm on top
			// of the very kick it is waiting for.
			if (aimStill && dx == 0.0f) {
				g_kickRestTicks++;
			} else {
				g_kickRestTicks = 0;
			}
			if (g_kickRestTicks >= g_settings.aimYawKickRearmTicks) {
				// Bookkeeping only. The radians already folded into the lever stay exactly where
				// they are - moving them is the retract, and the retract is the one thing here
				// measured to snap.
				g_yawKickSign = 0.0f;
				g_yawKickAgainst = 0.0f;
				g_yawKickLead = 0.0f;
				g_kickRestTicks = 0;
			}
		}
	} else {
		g_kickRestTicks = 0;
		g_haveKickFront = false;
	}

	// The game stores yaw in [0, 2PI). Feeding it a value outside that range makes the camera
	// snap, so wrap rather than clamp - this is a heading, it genuinely is circular.
	//
	// Except on a mounted gun, where it is not a heading at all but a signed offset from the
	// vehicle's nose, and the game clamps it to an arc instead of normalising it (mode 45 skips
	// its own wrap at 0x089a3980 on that branch). Wrapping there is actively destructive: half of
	// a +/-70 degree window is NEGATIVE, and -0.4 wrapped to 5.88 is not a small left offset, it
	// is two and a half turns past the stop. Clamp to the game's own limit instead, so the aim
	// stops where the gun stops and comes straight back the moment the mouse comes back - rather
	// than winding up an angle the player has to unwind before anything moves.
	if (attachedGun) {
		// Its own field, because the limit is the mission's: attach_ped_to_car carries it and
		// GON_C4 asks for 70 degrees. Refused rather than guessed if it reads implausibly - a
		// zero would pin the gun dead ahead, which is worse than the wrap this replaces.
		const float limit = ReadAddrFloat(VCSAddr::PedAimYawLimit).value_or(0.0f);
		if (limit > 0.01f && limit < 3.2f) {
			if (g_desiredYaw > limit) g_desiredYaw = limit;
			if (g_desiredYaw < -limit) g_desiredYaw = -limit;
		}
	} else {
		g_desiredYaw = WrapYaw(g_desiredYaw);
	}
	if (g_holdFrames == 0) {
		return;
	}
	if (holdsForever) {
		// Parked open - see holdsForever.
		g_holdFrames = kLookHoldFrames;
	} else {
		g_holdFrames--;
	}

	// Pitch has to be re-asserted every tick too - more so than yaw: a one-shot pitch write is undone
	// within a few frames (measured: 0.55 written, back to -0.05 within 0.4s).
	if (havePitch && g_haveAnchorPitch) {
		WriteAddrFloat(VCSAddr::CameraPitch, g_desiredPitch);
	}

	g_driving = WriteAddrFloat(VCSAddr::CameraYaw, g_desiredYaw);
	if (g_driving) {
		g_writes++;
		g_lastWritten = g_desiredYaw;
	} else {
		g_writeFails++;
	}
}



// --- Turning the character with the aim ---

static bool g_pedAimDriving = false;
static float g_pedAimDesired = 0.0f;
static u64 g_pedAimWrites = 0;
static u32 g_pedGunTarget = 0;
static int g_pedGunTargetType = -1;
static u64 g_pedGunWrites = 0;

void PedAimStats(bool *driving, float *desiredHeading, u64 *writes) {
	if (driving) *driving = g_pedAimDriving;
	if (desiredHeading) *desiredHeading = g_pedAimDesired;
	if (writes) *writes = g_pedAimWrites;
}

void YawKickState(float *side, float *againstIt, int *restTicks) {
	if (side) *side = g_yawKickSign;
	if (againstIt) *againstIt = g_yawKickAgainst;
	if (restTicks) *restTicks = g_kickRestTicks;
}

void AimAxisStats(float *desiredYaw, float *liveYaw, float *desiredPitch, float *livePitch) {
	if (desiredYaw) *desiredYaw = g_desiredYaw;
	if (desiredPitch) *desiredPitch = g_desiredPitch;
	if (liveYaw) *liveYaw = ReadAddrFloat(VCSAddr::CameraYaw).value_or(0.0f);
	if (livePitch) *livePitch = ReadAddrFloat(VCSAddr::CameraPitch).value_or(0.0f);
}

void PedGunStats(u32 *target, int *entityType, u64 *writes) {
	if (target) *target = g_pedGunTarget;
	if (entityType) *entityType = g_pedGunTargetType;
	if (writes) *writes = g_pedGunWrites;
}

// Point the GUN where the camera points, by moving the thing the ped is aiming at.
//
// The arm IK aims at a world POSITION, not at an angle - which is what "the gun stays put while the
// body turns" actually is. Nothing moves that position once the stick stops being fed, so the hand
// holds its world bearing and the body rotates underneath it.
//
// Moving that entity drives the NATIVE shot as well, not just the animation: for any non-ped target
// FireInstantHit reads `entity + 0x30` straight into its raycast target (0x08a48a6c). So this and
// the fire-site hook agree by construction rather than by coincidence.
//
// Measured in play, and it corrected the first guess: the entity is type 4, an OBJECT, not the
// type 5 dummy that FireInstantHit's special-case branch implied. The guard is therefore a
// blocklist rather than an allowlist - what actually matters is never writing the position of
// something the world owns.
static bool CanMoveAimTarget(int type) {
	// A ped is an NPC and a vehicle is a car; writing either one's position teleports it. A
	// building is static world geometry. Everything else is a placeholder as far as this is
	// concerned, and doing nothing to it helps nobody.
	return type != kVCSEntityTypePed && type != kVCSEntityTypeVehicle &&
	       type != kVCSEntityTypeBuilding && type != 0;
}

// Where to put the gun's aim point: along the aim the player has ASKED for, not along the camera's
// live Front. This is the whole of the free-aim drift.
//
// Measured with the drift trace on 2026-09-15, then read out of the game. While the ped has a gun
// target - and in free aim it always has, the placeholder this file moves - the free-aim camera does
// not integrate anything. Every game frame it works out the angles that would frame that target,
// adds the crosshair offsets to them (0x0899d0e8), and turns Beta and Alpha toward the result by at
// most [gp-0x3584] * TimeStep = 0.1 * 1.668 = 0.1668 rad (0x0899d14c..0x0899d278). That rate is the
// "9.5 degree deadband" the yaw kick and the pitch lead were built for: in every capture the game's
// angle sat exactly 0.1668 from the one we wrote, on whichever side the target was.
//
// Put the target down the live Front and a loop closes through that step. The game turns by the
// crosshair offset to frame the target, Front turns with it, the target moves with Front, and the
// game turns again - a walk that only stops at the far edge of the 0.1668 window. The offsets have a
// fixed sign, so it only ever walks one way: right, never left, in all fourteen captures. A rightward
// stroke left the kick parked on the far side, so the walk crossed the whole window - up to 19
// degrees - after the mouse had stopped. A leftward one left the walk already pinned at its end.
//
// Built from the angles this tick asserted, nothing the game does can move the target, so there is
// no loop: the game lands within one frame on the intent plus a small constant (the crosshair offset,
// and parallax between the camera and the point the game measures from) and stays there. Front is
// exactly (cos(Beta - PI), sin(Beta - PI)) * cos(Alpha), sin(Alpha): the trace read frontYaw ==
// CameraYaw - PI and frontPitch == CameraPitch to four decimals.
//
// Only on ticks that asserted both angles. Before the first assert, mid-handback, and on a mounted gun
// - where CameraYaw is an offset from the vehicle's nose rather than a heading, and the camera's
// attached branch never reads the target - the live ray is the one to use.
static bool AimIntentRay(float origin[3], float dir[3]) {
	if (!g_driving || g_wasAttachedGun || !g_haveAnchorPitch) {
		return false;
	}
	const float yaw = g_desiredYaw - kTwoPi * 0.5f;
	const float cosPitch = std::cos(g_desiredPitch);
	const float front[3] = { std::cos(yaw) * cosPitch, std::sin(yaw) * cosPitch, std::sin(g_desiredPitch) };
	static bool announced = false;
	if (!announced) {
		announced = true;
		NOTICE_LOG(Log::System, "VCS: the gun's aim point is placed along the asserted aim, not the live camera");
	}
	return SolveAimRayAlong(front, origin, dir);
}

static void PedAimGunTick() {
	g_pedGunTarget = 0;
	g_pedGunTargetType = -1;

	if (!g_settings.pedAimGun || !IsAddrSet(VCSAddr::PedPointGunAt))
		return;

	const std::optional<u32> target = ReadAddrU32(VCSAddr::PedPointGunAt);
	if (!target || !*target)
		return;
	g_pedGunTarget = *target;

	const std::optional<u32> flags = ReadU32(*target + kVCSEntityFlagsOffset);
	if (!flags)
		return;
	g_pedGunTargetType = (int)((*flags & kVCSEntityTypeMask) >> kVCSEntityTypeShift);
	if (!CanMoveAimTarget(g_pedGunTargetType))
		return;

	// The crosshair ray the bullet takes - but built along the aim we assert rather than the live
	// Front, wherever that is possible. See AimIntentRay: the live Front here is the drift.
	float origin[3], dir[3];
	if (!AimIntentRay(origin, dir) && !SolveAimRay(origin, dir))
		return;

	const float d = g_settings.pedAimGunDistance;
	const bool wrote =
		WriteFloat(*target + kVCSEntityPositionOffset + 0, origin[0] + dir[0] * d) &&
		WriteFloat(*target + kVCSEntityPositionOffset + 4, origin[1] + dir[1] * d) &&
		WriteFloat(*target + kVCSEntityPositionOffset + 8, origin[2] + dir[2] * d);
	if (wrote) {
		g_pedGunWrites++;
	}
}

void PedAimTick(VCSInputContext context) {
	g_pedAimDriving = false;

	if (!g_settings.enabled)
		return;
	// Only where the mouse is genuinely the aim.
	//
	// FreeAimActive is the load-bearing half of this and was missing at first, which cost a wrong
	// conclusion rather than just a bug. Without it this also ran during MELEE LOCK-ON - where the
	// game steers the player toward the target it has locked - so the heading write fought the
	// game's own facing logic, and with the sign briefly inverted that came out as reversed
	// movement. FreeAimActive already excludes both melee and lock-on, which is exactly the set of
	// states where the player's facing is not ours to drive.
	if (!ContextDrivesCamera(context) || context != VCSInputContext::Aiming)
		return;

	// THE GUN RUNS WHETHER OR NOT THE GAME THINKS IT IS LOCKED ON, and that ordering is the fix
	// for a long-standing complaint rather than a tidy-up.
	//
	// VCS re-acquires a lock-on by itself when a target wanders into range, mid-free-aim, and
	// nothing the player did asked for it. IsAiming goes to 1, FreeAimActive turns false, and
	// this function used to return right here - so the gun stopped being pointed while the camera
	// carried on following the mouse and the bullet carried on following the camera. What the
	// player sees is the character keeping some stale pose while the crosshair moves away from
	// him, shots still landing on the crosshair, and the whole thing fixing itself when the game
	// loses the target again.
	//
	// Escaping the lock-on instead was tried first - re-press the game's own Free Aim button on
	// the rising edge of IsAiming - and it does not work: the press that enters free aim from a
	// standing start does not break a lock the game has already taken.
	//
	// Running the gun anyway is safe because PedAimGunTick is not the write the gate was guarding.
	// That gate protects the HEADING write below, which fought the game's own facing logic during
	// melee lock-on and once came out as reversed movement. The gun works by moving the entity the
	// ped points at, and it already declines any target the world owns - CanMoveAimTarget refuses
	// peds and vehicles, so a genuine lock-on onto a person is left entirely alone. The case this
	// rescues is the one the debugger showed: the target is still the free-aim placeholder object,
	// still movable, and simply stopped being moved.
	PedAimGunTick();

	// The heading write keeps the stricter gate, unchanged and for the reason above.
	if (!FreeAimActive(context))
		return;
	if (!g_settings.pedFollowAim)
		return;
	if (!IsAddrSet(VCSAddr::PedHeading) || !IsAddrSet(VCSAddr::CameraYaw))
		return;

	// Deliberately the LIVE camera yaw rather than g_desiredYaw. Those agree while we are actively
	// asserting, and the live value is the right one in between - the character should stay pointed
	// where the view is even after the hold window has released and the mouse has stopped.
	const std::optional<float> camYaw = ReadAddrFloat(VCSAddr::CameraYaw);
	if (!camYaw)
		return;

	// CameraYaw is the camera's orbit angle; the direction it looks is `camYaw - PI` read as
	// (cos, sin). The ped stores a GTA heading, whose forward is (-sin, cos) - a quarter turn the
	// other way - so the conversion is a straight `+ PI/2`:
	//
	//     atan2(-cos(camYaw - PI), sin(camYaw - PI))  ==  camYaw - PI/2 - PI  ==  camYaw + PI/2
	//
	// Checked against the savestate: camYaw 4.88071 gives 0.168, and the ped was actually facing
	// 0.064 - a 6 degree difference, which is just the follow camera trailing the player rather
	// than a convention error. The two are forced into agreement the moment this starts writing.
	float heading = (g_settings.pedHeadingInvert ? -*camYaw : *camYaw) + kTwoPi * 0.25f
		+ g_settings.pedHeadingOffsetDeg * kTwoPi / 360.0f;

	// Wrap to [-PI, PI], not [0, 2PI). The game keeps this pair in a signed range - the clamp at
	// 0x089499a0 reads both fields, works out how far the heading sits from a target angle, and
	// subtracts the same correction from each, which only behaves for a signed delta. Writing 5.5
	// where it expects -0.78 is the same facing but not the same number, and everything that
	// compares the two fields sees a full turn of difference.
	heading = std::fmod(heading + kTwoPi * 0.5f, kTwoPi);
	if (heading < 0.0f) {
		heading += kTwoPi;
	}
	heading -= kTwoPi * 0.5f;
	g_pedAimDesired = heading;

	// Dest is the game's own "please turn to face this" channel, so it keeps the turn animation
	// and the game's own rate. Cur is the heading itself; writing it as well makes the character
	// track the view exactly, which is what a mouse asks for. See pedSnapHeading.
	bool wrote = false;
	if (IsAddrSet(VCSAddr::PedHeadingTarget)) {
		wrote = WriteAddrFloat(VCSAddr::PedHeadingTarget, heading) || wrote;
	}
	if (g_settings.pedSnapHeading) {
		wrote = WriteAddrFloat(VCSAddr::PedHeading, heading) || wrote;
	}
	if (wrote) {
		g_pedAimDriving = true;
		g_pedAimWrites++;
	}
}

void FreeAimMoveTick(VCSInputContext context) {
	// Patch the branch that makes aiming and moving mutually exclusive.
	//
	// The player control function processes aiming at 0x0894b85c and then branches straight over
	// the movement call at 0x0894b888. That single `b` is the entire reason free aim freezes the
	// player. Turning it into a nop lets the aim path fall through into the movement path.
	//
	// Found by breakpointing all four call sites of the movement applier and diffing walking
	// against free-aiming: 0x0894b888 fired on 100% of walking frames and 24% of aiming ones,
	// while every other site never ran at all.
	//
	// This is applied on CHANGE, not every frame, because it is code. Data this fork writes is
	// re-asserted per frame because the game recomputes it; an instruction stays written.
	//
	// The JIT is the hazard. PPSSPP overwrites the first instruction of each compiled block with a
	// block marker (0x68xxxxxx), so the live word here is NOT the original instruction. Invalidate
	// the icache first - that destroys the block and restores the real opcode - then write, then
	// invalidate again so the JIT recompiles from what we wrote. Skipping the first invalidate
	// means comparing against, and clobbering, a JIT pointer.
	static bool applied = false;
	static bool haveOriginal = false;
	static u32 original = 0;

	// The patch is a BRAKE, not an accelerator, and that inversion is the whole trick.
	//
	// Entering free aim while already running latches the movement: the game keeps applying it,
	// with the correct running-with-weapon animation, because the movement call is skipped and so
	// nothing ever re-evaluates it. Movement in free aim was never impossible - only *changing* it
	// was. So there is nothing to start.
	//
	// What was missing is a way to stop. Applying the patch lets that call run again, which
	// re-reads the stick and finds it centred, and the player halts. So: hold W and stay patched
	// out, so the latch keeps carrying you; release W and patch in for as long as it takes the
	// game to notice.
	//
	// Only while free-aiming, so ordinary play is never running patched code.
	const bool want = g_settings.moveInFreeAim && FreeAimActive(context) &&
	                  !MovementKeysHeld();
	if (want == applied || !IsAddrSet(VCSAddr::MoveGateBranch)) {
		return;
	}

	const u32 addr = LookupAddr(VCSAddr::MoveGateBranch).address;
	if (!Memory::IsValid4AlignedAddress(addr) || !currentMIPS) {
		return;
	}

	currentMIPS->InvalidateICache(addr, 4);

	if (!haveOriginal) {
		original = Memory::ReadUnchecked_U32(addr);
		// Refuse to touch anything that isn't the branch we identified. A wrong address here
		// crashes the game rather than misbehaving, which is not true of anything else this fork
		// writes, so this check is the difference between a bad setting and a bad afternoon.
		if (original != 0x1000000a) {
			WARN_LOG(Log::System, "VCS: move gate at %08x reads %08x, expected 1000000a - not patching",
				addr, original);
			g_settings.moveInFreeAim = false;
			return;
		}
		haveOriginal = true;
	}

	Memory::WriteUnchecked_U32(want ? 0x00000000u : original, addr);
	currentMIPS->InvalidateICache(addr, 4);
	applied = want;
	INFO_LOG(Log::System, "VCS: move gate at %08x %s", addr, want ? "patched to nop" : "restored");
}


void CameraReset() {
	AimModelReset();
	g_aimTimeStep = kAimDefaultTimeStep;

	{
		std::lock_guard<std::mutex> guard(g_deltaMutex);
		g_pendingDx = 0.0f;
		g_pendingDy = 0.0f;
	}
	g_driving = false;
	g_holdFrames = 0;
	g_desiredYaw = 0.0f;
	g_desiredPitch = 0.0f;
	g_yawKickSign = 0.0f;
	g_yawKickAgainst = 0.0f;
	g_yawKickLead = 0.0f;
	g_kickFrontYaw = 0.0f;
	g_haveKickFront = false;
	g_kickRestTicks = 0;
	g_anchorPitch = 0.0f;
	g_haveAnchorPitch = false;
	g_lastContext = VCSInputContext::Unknown;
	ChaseCamReset();
}

bool CameraIsDriving() {
	return g_driving;
}

void CameraHoldState(float *desiredYaw, float *desiredPitch, float *anchorPitch,
		int *holdFrames, const char **contextName) {
	*desiredYaw = g_desiredYaw;
	*desiredPitch = g_desiredPitch;
	*anchorPitch = g_anchorPitch;
	*holdFrames = g_holdFrames;
	*contextName = VCSInputContextName(g_lastContext);
}


void CameraWriteStats(u64 *writes, u64 *fails, float *lastWritten) {
	*writes = g_writes;
	*fails = g_writeFails;
	*lastWritten = g_lastWritten;
}

void CameraDiagnostics(u64 *seen, u64 *claimed, u64 *rejectedContext,
		float *maxAbsDx, float *sumAbsDx) {
	*seen = g_mouseSeen;
	*claimed = g_mouseClaimed;
	*rejectedContext = g_mouseRejectedContext;
	*maxAbsDx = g_maxAbsDx;
	*sumAbsDx = g_sumAbsDx;
}

float CameraPendingDeltaX() {
	std::lock_guard<std::mutex> guard(g_deltaMutex);
	return g_pendingDx;
}

}  // namespace VCS
