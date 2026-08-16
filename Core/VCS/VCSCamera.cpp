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

// The synthesised second stick we last wrote into CPad. Emu thread only.
static bool g_padStickHeld = false;
static float g_padStickX = 0.0f;
static float g_padStickY = 0.0f;
// Cumulative, never reset while a game runs - see PadStickStats for why.
static bool g_padStickModeSet = false;
static u64 g_padStickFrames = 0;
static u64 g_padStickNonZero = 0;
static float g_padStickPeak = 0.0f;

// Defined further down, next to the explanation of what it writes and why.
static void WritePadStick(float x, float y);

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

// The yaw we are asserting, and how many more frames to keep asserting it for. Emu thread only.
static float g_desiredYaw = 0.0f;
static float g_desiredPitch = 0.0f;
// Where the game had the pitch when this look started. The clamp window is centred on this,
// because the game's pitch baseline is wildly different per camera mode.
static float g_anchorPitch = 0.0f;
// Last context we ran in, so a change (e.g. getting into a car) forces a fresh anchor.
static VCSInputContext g_lastContext = VCSInputContext::Unknown;
static int g_holdFrames = 0;

// About three quarters of a second at 60fps. Long enough to cover the gaps between mouse
// events without leaving the camera locked after the player stops looking around.
static const int kHoldFrames = 45;

// How far pitch may travel from wherever the game had it when the look started - about 40
// degrees each way. Deliberately RELATIVE: the game's pitch baseline differs hugely per
// camera (about -0.05 on foot, about -1.55 in a vehicle), so any absolute limit is wrong in
// one mode or the other. See the clamp in CameraTick for what that cost.
static const float kPitchRange = 0.7f;

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
// Two callers ask for a deflection. In free aim both aim channels run: ApplyAnalog writes the nub
// and PadStickTick writes the d-pad pair, and which one the game reads is decided by
// CameraInputMode, not by us. So the first caller in a tick solves and the second gets the same
// answer.
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

// The largest deflection the channel that is actually live can carry to the game.
//
// usePadStick decides which channel that is - it sets CameraInputMode, and the flag is what makes
// the game read the d-pad pair instead of the nub. So it also decides whether aimRangeBoost means
// anything at all.
float AimChannelLimit() {
	if (g_settings.usePadStick && PadStickAvailable() && g_settings.aimRangeBoost > 1.0f) {
		return g_settings.aimRangeBoost;
	}
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

	// Saturate at what the LIVE CHANNEL can actually deliver, which is not the same as what
	// aimRangeBoost permits.
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
		g_aimOutX = AimAxisStep(&g_aimX, dx * g_settings.aimSensitivity, VCSAddr::CamAimIncX);
		g_aimOutY = AimAxisStep(&g_aimY, -dy * g_settings.aimSensitivity * ySign, VCSAddr::CamAimIncY);
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
	case VCSInputContext::Aiming:
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
// PluginAimActive is the other way the mouse gets spoken for: with the CLEO plugin driving aim
// from the right stick, the plugin owns the view while aim is held, so writing camera angles
// underneath it would fight whatever it does.
static bool ContextDrivesCamera(VCSInputContext context) {
	return ContextWantsMouse(context) && !ReticleActive(context) && !PluginAimActive(context) &&
	       !PadStickActive(context);
}

// Whether the mode is wanted at all. This governs the FLAG, which must stay set for as long as
// the player is in this mode - including while free-aiming, when the delta goes elsewhere.
static bool PadStickModeWanted(VCSInputContext context) {
	return g_settings.usePadStick && g_settings.enabled && PadStickAvailable() &&
	       ContextWantsMouse(context);
}

bool PadStickActive(VCSInputContext context) {
	// Only during FREE AIM, and deliberately alongside the reticle rather than instead of it.
	//
	// In free aim the two channels carry different axes, confirmed in game: the nub turns the
	// yaw, and d-pad up/down moves the PITCH. So the reticle path (nub) and this one (d-pad)
	// are complementary, and driving only one gives you half an aim. This previously stood aside
	// whenever the reticle was active, which is exactly backwards - it meant pitch was only ever
	// reachable by pressing the arrow keys by hand.
	//
	// Everywhere else it stays off, because these fields double as the d-pad BUTTONS and the
	// meaning is destructive: left/right is previous/next target while locked on (a trace caught
	// us writing 255 and cycling through NPCs) and previous/next weapon on foot. Free aim is the
	// one state where the buttons have nothing worse to do.
	// Keyed on FreeAimActive, NOT ReticleActive. It used to key on the reticle, which became
	// circular the moment the reticle had to stand down for this: the reticle stops so the nub is
	// free for movement, which would have stopped the d-pad aim as well, which would have left free
	// aim with nothing driving it at all.
	return PadStickModeWanted(context) && FreeAimActive(context);
}

void PadStickTick(VCSInputContext context) {
	// The flag follows the MODE, not the delta routing - otherwise entering free aim would clear
	// it, and clearing it is what takes free aim away again.
	if (PadStickModeWanted(context)) {
		if (!g_padStickModeSet) {
			WriteAddrU8(VCSAddr::CameraInputMode, 1);
			g_padStickModeSet = true;
		}
	} else if (g_padStickModeSet) {
		WriteAddrU8(VCSAddr::CameraInputMode, 0);
		g_padStickModeSet = false;
	}

	if (!PadStickActive(context)) {
		// Release once. After that the game's own pad update owns these fields again, and writing
		// zeroes every frame would be pointless work that also fights a real d-pad press.
		if (g_padStickHeld) {
			WritePadStick(0.0f, 0.0f);
			g_padStickHeld = false;
			g_padStickX = 0.0f;
			g_padStickY = 0.0f;
		}
		// Deliberately NOT resetting the model here. PadStickActive is a subset of ReticleActive,
		// so this also fires on every frame where the nub is aiming and the d-pad is not - and
		// wiping the model then would destroy the state the nub channel is mid-way through using.
		// ApplyAnalog owns the reset, at the one point that really means "aiming stopped".
		return;
	}

	// The flag itself is handled at the top of this function, once per mode change rather than
	// once per frame. The game owns it: there is a setter at 0x089c6f08 (takes a byte in a1,
	// stores to gp-0x3F00, returns the old value) and 0x08ab6898 copies it into a settings struct
	// alongside other options, so it is a game SETTING the game changes as modes come and go.
	// Re-asserting it every frame fought that, and the symptom was aim "locking on for a split
	// second and then nothing" - the game set the flag for its aim mode and we stomped it back.
	g_padStickFrames++;

	float dx, dy;
	// Take or peek, depending on whether anyone drained the delta before us.
	//
	// This used to always PEEK, on the reasoning that ApplyAnalog's reticle branch had already
	// drained the frame's movement for the nub, so draining again would find nothing. That held
	// only while the reticle ran alongside this. Now the reticle stands down whenever the d-pad is
	// the aim channel - so that the nub is free for WASD - and with it went the only consumer that
	// drained anything. Peeking then returns a stale value, and the mouse stops aiming entirely.
	if (ReticleActive(context)) {
		PeekMouseDelta(&dx, &dy);
	} else {
		TakeMouseDelta(&dx, &dy);
	}

	// The deflection is no longer proportional to the mouse movement - it is whatever the model
	// says will move the aim by the distance the mouse moved. See AimAxisStep.
	//
	// This has to run on every frame, including frames with no movement, because a stop is an
	// instruction: it is what makes the model push back against the game's smoother instead of
	// letting the aim coast. The old code returned early on a zero delta, which is precisely the
	// glide that made this feel like a stick.
	float x = 0.0f, y = 0.0f;
	AimDeflectionFromMouse(dx, dy, g_settings.padStickSensitivity, g_settings.invertY, &x, &y);
	if (g_settings.invertX) {
		x = -x;
	}

	// Only touch the fields while actually deflecting them. These ARE the d-pad - the same four
	// fields the real buttons set - so writing zeroes every idle frame stomps genuine d-pad input
	// 60 times a second: Q/E target cycling, weapon switching, radio, horn. That is the "only
	// release what you pressed" rule from ApplyMapping, and it applies here for the same reason.
	//
	// Not writing when idle is safe: the game's own pad update rewrites these every frame from the
	// real buttons, so letting go hands them straight back rather than leaving a stale deflection.
	// One zeroing write on the release frame clears whatever we last put there.
	if (x == 0.0f && y == 0.0f) {
		if (g_padStickHeld) {
			WritePadStick(0.0f, 0.0f);
			g_padStickHeld = false;
			g_padStickX = 0.0f;
			g_padStickY = 0.0f;
		}
		return;
	}

	WritePadStick(x, y);
	g_padStickHeld = true;
	g_padStickX = x;
	g_padStickY = y;

	// Persistent evidence. The live x/y self-centres the instant the mouse stops, so it reads
	// zero in every screenshot and can neither confirm nor deny that this works. These don't
	// decay, so they answer "has the mouse EVER driven this axis" at a glance.
	if (x != 0.0f || y != 0.0f) {
		g_padStickNonZero++;
		const float mag = (x < 0.0f ? -x : x) > (y < 0.0f ? -y : y)
			? (x < 0.0f ? -x : x) : (y < 0.0f ? -y : y);
		if (mag > g_padStickPeak) {
			g_padStickPeak = mag;
		}
	}
}

void PadStickStats(u64 *frames, u64 *nonZero, float *peak, bool *modeFlagSet) {
	*frames = g_padStickFrames;
	*nonZero = g_padStickNonZero;
	*peak = g_padStickPeak;
	const std::optional<u32> mode = ReadAddrU32(VCSAddr::CameraInputMode);
	*modeFlagSet = mode.has_value() && *mode != 0;
}

void GetPadStick(float *x, float *y) {
	*x = g_padStickX;
	*y = g_padStickY;
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

bool PadStickAvailable() {
	// CameraInputMode is required, not optional: with it unset the d-pad fields are never read,
	// so the whole mechanism is inert and offering it would just be a switch that does nothing.
	return IsAddrSet(VCSAddr::PadDPadLeft) && IsAddrSet(VCSAddr::PadDPadRight) &&
	       IsAddrSet(VCSAddr::PadDPadUp) && IsAddrSet(VCSAddr::PadDPadDown) &&
	       IsAddrSet(VCSAddr::CameraInputMode);
}

// Drives the second analog stick VCS thinks it has.
//
// The game synthesises that stick from the d-pad: the camera X axis is computed as
// (DPadRight - DPadLeft) / 2 and Y as (DPadDown - DPadUp) / 2, in the pad functions at
// 0x0898bb4c and 0x0898bb8c. Those fields are int16 and the real d-pad only ever puts 0 or 255
// in them, so the axis a player can actually produce is -127, 0 or +127 - three positions. But
// nothing in the arithmetic requires that, and writing intermediate values yields a genuine
// analog axis, which is what makes mouse input expressible at all.
//
// Writing here rather than pressing buttons is the whole point: no PSP input can express a
// partial d-pad. It is the same thing PSPRecomp does by replacing those two functions, and the
// same thing the CLEO plugin does by writing into CPad - reached with a plain memory write.
//
// x and y are -1..1, or wider when aimRangeBoost allows. Only one side of each pair is ever
// nonzero, exactly as a real d-pad can never be pressed left and right at once.
static void WritePadStick(float x, float y) {
	const float scale = 254.0f;   // halved by the game, so 254 lands on the +/-127 a d-pad reaches

	// The ceiling is the int16 the field actually is, not 255. 255 is only the largest value a
	// real d-pad produces - the accessor at 0x0898bb4c subtracts the pair, halves it and sign
	// extends, with no clamp anywhere - so a larger value is a larger axis, which is what
	// aimRangeBoost is for. The model is the limiter; this just avoids wrapping the sign bit.
	const int kMax = 32767;
	int ix = (int)(x * scale);
	int iy = (int)(y * scale);
	if (ix > kMax) ix = kMax;
	if (ix < -kMax) ix = -kMax;
	if (iy > kMax) iy = kMax;
	if (iy < -kMax) iy = -kMax;

	WriteAddrU16(VCSAddr::PadDPadRight, (u16)(ix > 0 ? ix : 0));
	WriteAddrU16(VCSAddr::PadDPadLeft,  (u16)(ix < 0 ? -ix : 0));
	// Y is inverted relative to the pair order: the game computes (Down - Up), so a positive
	// requested y (look up) has to land in the Up field.
	WriteAddrU16(VCSAddr::PadDPadDown,  (u16)(iy < 0 ? -iy : 0));
	WriteAddrU16(VCSAddr::PadDPadUp,    (u16)(iy > 0 ? iy : 0));
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
	g_desiredAim += dx * g_settings.sensitivity * (g_settings.invertX ? 1.0f : -1.0f);

	WriteAddrFloat(VCSAddr::AimYaw, g_desiredAim);
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
	float dx = 0.0f, dy = 0.0f;
	if (!ReticleActive(context) || AimModelReady()) {
		TakeMouseDelta(&dx, &dy);
	}

	if (!g_settings.enabled || !ContextDrivesCamera(context)) {
		g_holdFrames = 0;
		g_lastContext = context;
		return;
	}

	// Force a re-anchor when the context changes. On foot and in a vehicle both want mouse look,
	// so without this the desired pitch/yaw carries straight from one camera into the other and
	// the first write after getting into a car asserts a value aimed at the on-foot camera -
	// whose pitch baseline is about 1.5 radians away from the vehicle one.
	if (context != g_lastContext) {
		g_holdFrames = 0;
		g_lastContext = context;
	}

	// The game runs its own camera smoothing: it eases yaw back toward wherever its follow logic
	// wants it. Writing only on frames where the mouse moved therefore does almost nothing - the
	// game undoes each nudge on the frames in between. (Measured: 135 successful writes produced
	// no net rotation at all.)
	//
	// So hold a desired yaw of our own and re-assert it EVERY frame for a short while after the
	// last movement. That outpaces the smoothing while the player is actively looking around,
	// and then releases so the normal follow-camera behaviour comes back when they stop.
	// Pitch is only driven where it behaves. In a vehicle, asserting pitch against the follow
	// camera leaves it settled steeply downward once we release, so it is opt-in there.
	const bool havePitch = IsAddrSet(VCSAddr::CameraPitch) &&
		(context != VCSInputContext::InVehicle || g_settings.pitchInVehicle);

	if (dx != 0.0f || (dy != 0.0f && havePitch)) {
		if (g_holdFrames == 0) {
			// Starting a fresh look - anchor to wherever the game currently has the camera, so
			// we never snap from a stale value.
			const std::optional<float> current = ReadAddrFloat(VCSAddr::CameraYaw);
			if (!current) {
				return;
			}
			g_desiredYaw = *current;

			const std::optional<float> pitch = ReadAddrFloat(VCSAddr::CameraPitch);
			g_desiredPitch = pitch.value_or(0.0f);
			g_anchorPitch = g_desiredPitch;
		}

		if (havePitch) {
			// Stored negated relative to the camera matrix: raising this value tilts the view UP.
			// Default is INVERTED pitch by preference - mouse up looks down, flight-sim style.
			// invertY switches to the conventional mouse-up-looks-up mapping.
			g_desiredPitch += dy * g_settings.sensitivity * (g_settings.invertY ? -1.0f : 1.0f);

			// Pitch does not wrap - it is a look angle, so clamp rather than wrap.
			//
			// The limits are RELATIVE TO THE ANCHOR, never absolute. The game uses completely
			// different pitch baselines per camera: on foot it rests around -0.05, in a vehicle
			// around -1.55. A fixed +/-0.9 was the cause of the vehicle bug - it yanked the
			// anchor from -1.55 up to -0.9 on the first write (a 37 degree jump to the roof) and
			// then pinned it there, because the clamp overrode every attempt to pitch back down.
			// Widening the limits to merely admit the anchor was not enough either: it then let
			// pitch climb to +0.89 while the game sat at -0.92, which is well past vertical.
			//
			// Anchoring the window to wherever the game had the camera makes this correct in
			// both modes without knowing either baseline.
			const float lo = g_anchorPitch - kPitchRange;
			const float hi = g_anchorPitch + kPitchRange;
			if (g_desiredPitch > hi) g_desiredPitch = hi;
			if (g_desiredPitch < lo) g_desiredPitch = lo;
		}
		// The game's yaw grows counter-clockwise (measured: mouse right raised the value, which
		// turned the view LEFT), so the natural mapping needs a negative sign. invertX flips
		// away from that correct default, it isn't the default itself.
		g_desiredYaw += dx * g_settings.sensitivity * (g_settings.invertX ? 1.0f : -1.0f);
		g_holdFrames = kHoldFrames;
	}

	if (g_holdFrames <= 0) {
		return;
	}
	g_holdFrames--;

	// The game stores yaw in [0, 2PI). Feeding it a value outside that range makes the camera
	// snap, so wrap rather than clamp - this is a heading, it genuinely is circular.
	g_desiredYaw = std::fmod(g_desiredYaw, kTwoPi);
	if (g_desiredYaw < 0.0f) {
		g_desiredYaw += kTwoPi;
	}

	// Pitch has to be re-asserted every frame too - more so than yaw, in fact: a one-shot pitch
	// write is undone within a few frames (measured: 0.55 written, back to -0.05 within 0.4s).
	if (havePitch) {
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

// Walk the player during free aim, by writing the ped's velocity directly.
//
// EXPERIMENTAL, and off by default. The PSP has one analog axis, so free aim genuinely cannot both
// place the crosshair and steer - the stick is the crosshair, and the d-pad is inert there
// (confirmed in play with the arrow keys). The game therefore exposes no input channel for
// movement while free-aiming, which makes this the only route that does not involve patching the
// game's own decision.
//
// PedVelX/Y were found by diffing the ped struct between walking and free-aiming with the stick
// deflected in both cases: they are alive in the first and dead in the second, and nothing further
// upstream is. The game rewrites them every frame, so like CameraYaw this has to be re-asserted on
// every tick or it is wiped - a one-shot write from the WebSocket debugger is erased before the
// game reads it, which is why this could only be settled by building it.
//
// The direction is camera-relative, which is what a mouse-look game wants: W goes where you are
// looking. Sign conventions here are a first guess and may need swapping.
// Walk during free aim by TRANSLATING the player, rather than by asking the game to move them.
//
// Velocity was tried first and cannot work: the ped movement update zeroes it at the start of
// every frame and recomputes it from the movement intent, which free aim is what suppresses. So a
// written velocity is wiped before anything reads it - measured, with the game verifiably running.
//
// Position has no such owner. Nothing recomputes it from the movement state, so a small step added
// each frame simply accumulates. It is closer to a noclip than to walking, and it inherits that
// approach's weaknesses honestly:
//
//   - No animation. The character slides in whatever pose free aim holds them in.
//   - Collision is whatever the game's physics does about finding the player inside geometry
//     AFTER the fact. It resolves interpenetration, so walls may well push back - but this does
//     not sweep, so a fast enough step could pass through something thin.
//   - Height is not touched, so slopes and stairs are not followed.
//
// Deliberately independent of the MoveGateBranch patch: this does not need the game's movement
// path to run at all, so the two can be judged separately.
void FreeAimTranslateTick(VCSInputContext context) {
	if (!g_settings.freeAimTranslate || !FreeAimActive(context)) {
		return;
	}
	if (!IsAddrSet(VCSAddr::PedPosX) || !IsAddrSet(VCSAddr::CameraYaw)) {
		return;
	}

	// Forward only, on W. Strafing was tried first and is not worth the complication: this is a
	// translation, not a walk, so every extra direction is another way to slide somewhere the
	// animation and the collision were not expecting.
	if (!IsHostKeyDown(NKCODE_W)) {
		return;   // don't touch the position while the player isn't asking to move
	}

	const std::optional<float> yaw = ReadAddrFloat(VCSAddr::CameraYaw);
	const std::optional<float> px = ReadAddrFloat(VCSAddr::PedPosX);
	const std::optional<float> py = ReadAddrFloat(VCSAddr::PedPosY);
	if (!yaw || !px || !py) {
		return;
	}

	// Heading is CameraYaw - PI. The docs give CameraYaw as the matrix yaw plus PI/2, so undoing
	// that alone should have been right - it was not, and it showed up as A walking the player
	// forwards while W did nothing useful. A quarter turn more lines it up. Measured, not derived:
	// whatever the extra offset means, the game's convention is not the one the note implies.
	const float h = *yaw - 3.14159265f;
	const float step = g_settings.freeAimMoveSpeed;

	WriteAddrFloat(VCSAddr::PedPosX, *px + std::cos(h) * step);
	WriteAddrFloat(VCSAddr::PedPosY, *py + std::sin(h) * step);
}

// Turn the character to face where the camera is pointing, while moving in free aim.
//
// Free aim latches a movement direction when it engages and never revisits it, so the body keeps
// travelling the way it was pointed while only the arm tracks the aim - "he only goes in one
// direction but stretches his hand around". Turning the body turns the movement with it, because
// the latched motion is along the ped's own forward.
//
// Only while actually moving. Standing still in free aim should leave the aiming pose alone; the
// arm already tracks the camera perfectly well on its own.
void FreeAimFaceTick(VCSInputContext context) {
	if (!g_settings.freeAimFaceCamera || !g_settings.moveInFreeAim) {
		return;
	}
	if (!FreeAimActive(context) || !IsHostKeyDown(NKCODE_W)) {
		return;
	}
	if (!IsAddrSet(VCSAddr::PedFwdX) || !IsAddrSet(VCSAddr::CameraYaw)) {
		return;
	}

	const std::optional<float> yaw = ReadAddrFloat(VCSAddr::CameraYaw);
	if (!yaw) {
		return;
	}

	// Same heading convention the translation path had to be corrected to - CameraYaw - PI, found
	// by trying it rather than derived, since the documented "matrix yaw + PI/2" was a quarter turn
	// out. Observed matrix with the player facing world +X: forward (1,0,0), right (0,-1,0), which
	// fixes the right vector as (sin, -cos) for a forward of (cos, sin).
	const float h = *yaw - 3.14159265f;
	const float c = std::cos(h);
	const float s = std::sin(h);

	WriteAddrFloat(VCSAddr::PedFwdX, c);
	WriteAddrFloat(VCSAddr::PedFwdY, s);
	WriteAddrFloat(VCSAddr::PedRightX, s);
	WriteAddrFloat(VCSAddr::PedRightY, -c);
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
	                  !IsHostKeyDown(NKCODE_W);
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

	std::lock_guard<std::mutex> guard(g_deltaMutex);
	g_pendingDx = 0.0f;
	g_pendingDy = 0.0f;
	g_driving = false;
	g_holdFrames = 0;
	g_desiredYaw = 0.0f;
	g_desiredPitch = 0.0f;
	g_anchorPitch = 0.0f;
	g_lastContext = VCSInputContext::Unknown;
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
