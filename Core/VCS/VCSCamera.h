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

#pragma once

#include "Common/CommonTypes.h"
#include "Core/VCS/VCSInput.h"

// Mouse look for VCS.
//
// The PSP has no mouse, so this doesn't go through sceCtrl like the button mapping does - there
// is no analog input that could express "turn the camera by exactly this many radians". Instead
// it writes VCSAddr::CameraYaw directly, once per frame. That works because the game reads that
// value to build the camera matrix and does not fight a written value.
//
// EXCEPT while aiming, where the mouse means something else entirely. See ApplyAnalog in
// VCSInput.cpp: with the aim key held the mouse drives the analog stick (the reticle) rather
// than the camera address, because on the PSP the reticle is what decides where the shot goes.
// This file still owns the raw delta buffer in that case - it just hands it over via
// TakeMouseDelta instead of writing angles with it.
//
// Threading: mouse deltas arrive on the input thread and are accumulated under a mutex; the
// accumulated delta is consumed and applied on the emu thread from VCSGame::Tick, which is the
// only place PSP memory may be written.

namespace VCS {

struct VCSCameraSettings {
	bool enabled = true;

	// Radians of camera rotation per unit of raw mouse movement. Raw mouse units are roughly
	// "dots", so this is small. Tune from the debugger window.
	float sensitivity = 0.004f;

	bool invertX = false;
	bool invertY = false;

	// Vertical look while driving, off by default.
	//
	// Yaw works fine in a vehicle, but asserting PITCH against the vehicle follow-camera leaves
	// it in a bad state: after we release, the camera settles steeply downward (observed at
	// -0.99 rad looking down at the bike from above) instead of returning to normal. The cause
	// is not understood yet, so this is off until it is - driving is usable without vertical
	// look, and unusable with it.
	bool pitchInVehicle = false;

	// --- Aiming ---
	//
	// These are not camera settings at all; they belong to the reticle, which is a stick
	// deflection rather than an angle. They live here because this struct is really "what the
	// mouse does", and it is what the debugger's tuning UI already binds to.

	// Radians of AIM rotation per unit of raw mouse movement.
	//
	// Same units as `sensitivity` above, but deliberately about 8x SMALLER, which is not a matter
	// of taste - it is the aim channel's rate ceiling.
	//
	// This defaulted to 0.004, matching the look sensitivity, on the reasoning that free aim and
	// looking around are one gesture on a PC. True as far as it goes, and wrong in practice: the
	// game's aim rate tops out at kAimRate * timeStep, about 0.053 rad per frame, and on the nub
	// the deflection cannot exceed 1. At 0.004 the model's solve therefore saturates at 13 counts
	// in a frame - reached constantly by ordinary mouse movement - so the response flattened again
	// right where it was supposed to be linear. At 0.0005 it saturates around 107 counts, which is
	// a hard flick, leaving the whole usable range linear.
	//
	// Found by tuning in play. It went 0.004 -> 0.0005 -> 0.00128 as the model got more honest:
	// the 0.0005 was compensating for a build that discarded half the mouse movement, so it rose
	// again once the frame sync landed and the input actually arrived.
	//
	// aimRangeBoost is what buys back the ceiling, but only on the d-pad channel. If you turn that
	// on, this can go back up.
	//
	// Only meaningful with aimResponseModel on - the model is what turns a wanted rotation back
	// into a deflection. With the model off it degrades to a raw deflection scale, which is what
	// this setting used to be (0.015 stick/count).
	float aimSensitivity = 0.00128f;

	// Aim by inverting the game's own axis response, rather than pushing the stick proportionally
	// to the mouse. This is what makes free aim feel like a mouse instead of a thumbstick.
	//
	// The game does not store an aim direction anywhere - it integrates the look axis into the
	// camera every frame, and there are three separate distortions between the axis and the
	// resulting rotation. All three are in CCam mode 45's Process function at 0x089a341c:
	//
	//   a signed SQUARE   rate is proportional to n*|n|, not n. Moving the mouse 10 counts in one
	//                     frame rotates 100x as far as 1 count in each of ten frames.
	//   a SMOOTHER        the increment is low-passed at ~0.84 per frame, so the aim takes about
	//                     six frames to reach the rate asked for and keeps gliding after you stop.
	//   SATURATION        the axis stops at full deflection, so a fast flick is simply truncated.
	//
	// The square is the important one: it is a shape, not a scale, so no value of aimSensitivity
	// has ever been able to fix it - which is exactly why this felt like a stick at every setting
	// anyone tried. Inverting all three turns the same channel into a position control, the same
	// thing writing CameraYaw does for mouse look.
	//
	// On by default. Turn it off to get the old proportional mapping back for comparison.
	bool aimResponseModel = true;

	// Extra deflection range for the d-pad channel, beyond what a real stick can reach.
	//
	// The game's aim rate maxes out at about 0.05 rad per frame, which is a good deal slower than
	// mouse look, so fast aim movements queue up instead of landing at once. The d-pad fields are
	// int16 and the accessor at 0x0898bb4c only halves and sign-extends them - it does not clamp -
	// so writing past 255 produces an axis past 127 and, through the square, a much higher rate.
	// (CLAUDE.md used to say the axis was clamped to +/-127. It is not; that is just the largest
	// value a real d-pad can produce.)
	//
	// This is what makes FAST aim movements linear, and the model needs it to do its job. Simulated
	// against the game's own formula, for a fixed total mouse travel spread across frames:
	//
	//     counts per frame       1x        2x        3x        4x     (target 13.75 deg)
	//     1                  13.75     13.75     13.75     13.75
	//     5                  13.75     13.75     13.75     13.75
	//     10                  7.98     13.75     13.75     13.75
	//     20                  2.65     10.61     13.75     13.75
	//     30                  1.40      5.60     12.59     13.75
	//
	// At 30fps, 20 counts per frame is an ordinary aiming movement, not a flick - so 1.0 leaves
	// the common case still collapsing. 3.0 covers everything up to a deliberate flick, which is
	// why it is the default despite being past what the hardware could produce.
	//
	// Set it to 1.0 for hardware-faithful range. Does nothing on the nub channel, where sceCtrl
	// clamps to full deflection before the game ever sees it - it needs usePadStick.
	//
	// Defaults to 1.0 because usePadStick is not used in practice, which makes this inert. Left in
	// place rather than removed: the finding that the axis is not clamped is real and worth
	// keeping, and this is the only lever on the game's aim rate ceiling if that channel is ever
	// revisited.
	float aimRangeBoost = 1.0f;

	// How much of the game's smoothing momentum to cancel when the mouse stops, 0..1.
	//
	// The model's headline trick is pushing the stick BACKWARDS as a movement ends, which stops the
	// aim dead instead of letting the game's smoother coast it onward. Cancel too much and the aim
	// does not just stop, it reverses - a visible snap back at the end of every movement, which is
	// worse than the coasting it was meant to fix.
	//
	// Back to full strength now that aimStopDelay decides *when* it fires.
	//
	// It was briefly defaulted to 0.10 - mostly off - because the cancellation fired on any frame
	// with no mouse movement, including the zero-count frames that slow aiming produces mid-stroke.
	// That braking-during-a-movement read as stutter, and detuning the strength was the only lever
	// available at the time. It was treating the symptom: the error was in the timing, and a
	// perfectly calibrated cancellation stutters exactly as badly if it fires at the wrong moment.
	//
	// With the delay in place, strength and timing are separate questions again and this can do
	// its job. Lower it only if the aim still travels backwards once a stroke has genuinely ended.
	float aimCancel = 1.0f;

	// Use the plain proportional mapping for the manually-aimed weapons - sniper and RPG, which
	// report weapon camera mode 7 and 8.
	//
	// Those two never had lock-on and never went through the camera's squared response; their
	// reticle is driven directly, and the relationship to the axis is linear. The model's whole job
	// is to invert a square, so pointing it at something that was already linear does not cancel
	// anything - it applies a sqrt that nothing undoes, and a sqrt makes SMALL inputs
	// disproportionately large. The symptom is unmistakable once you know it: the slightest mouse
	// movement sends the scope off on its own.
	//
	// They aimed correctly before the model existed, which is the evidence that matters. This puts
	// them back on that mapping and leaves everything else on the model.
	bool aimScopedLinear = true;

	// Deflection per count of mouse movement for the above. Not radians - this path does not model
	// the game's response, it just pushes the stick in proportion, so the units are the old ones.
	//
	// 0.015 was the pre-model value and it aimed correctly, but it is slow: a brisk 10 counts in a
	// frame asks for only 15% deflection, so ordinary aiming never used more than a slice of the
	// range. 0.045 puts a normal movement mid-range and still needs about 22 counts in one frame
	// to reach the stop.
	//
	// The game reduces its own angular rate with FOV, so scoping in slows the aim without this
	// having to know anything about zoom - which is the behaviour a scope wants anyway.
	float aimScopedSensitivity = 0.045f;

	// EXPERIMENTAL: walk during free aim, by writing the ped's velocity directly.
	//
	// The game gives free aim no movement channel at all - the single analog axis is the crosshair
	// and the d-pad is inert there, both confirmed in play. So this does not route input, it
	// overrides the game's own physics, which is a heavier thing than anything else in this file.
	// Expect the walk animation not to play; the character may slide.
	//
	// Off by default because it is unproven. See FreeAimMoveTick.
	bool moveInFreeAim = false;

	// World units per frame while doing so. The game's own walking speed measured about 0.10, but
	// this defaults well below it: a translation has no collision sweep, so a smaller step gives
	// the physics more chance to resolve a wall before the next one lands, and a slow creep reads
	// far better against an animation that is not actually a walk cycle.
	float freeAimMoveSpeed = 0.035f;

	// Walk during free aim by TRANSLATING the player directly - a slow, collision-resolved noclip
	// rather than an attempt to make the game move them.
	//
	// Independent of moveInFreeAim above: this needs neither the code patch nor the game's own
	// movement path, so the two approaches can be judged separately. See FreeAimTranslateTick for
	// what it inherits from being a position write - no animation, no slopes, and collision only
	// as far as the physics resolves interpenetration after the fact.
	bool freeAimTranslate = false;

	// Turn the character to face the camera while moving in free aim, so they travel where you are
	// pointing rather than the way they happened to be facing when the aim latched.
	//
	// OFF: it does not work. Writing the entity matrix does not redirect the latched movement -
	// most likely the game rewrites the orientation from its own animation state after we do, the
	// same way it owns velocity. Kept because the addresses are correct and useful, and because
	// knowing this route fails is worth as much as the route itself.
	bool freeAimFaceCamera = false;

	// Consecutive still GAME frames before a stroke counts as over and the stop is allowed to fire.
	//
	// At 30fps each frame is 33ms, so 2 is about 66ms - longer than a gap in the middle of real
	// mouse movement, shorter than a coast anyone would notice. Raise it if movement still feels
	// interrupted; lower it for a snappier stop.
	//
	// The cost of waiting is small: the game's smoother has decayed to about 70% over two frames,
	// so most of the coast is still there to be cancelled when the stop does fire.
	int aimStopDelay = 2;

	// Unlike the camera, the reticle is NOT inverted by default. Pushing the mouse away from you
	// raises the crosshair, which is what every mouse-aimed shooter does. The camera's invertY
	// defaults the other way round on purpose (flight-sim style look), so the two deliberately do
	// not share a setting - inheriting the camera's inversion here felt broken in testing.
	bool aimInvertY = false;

	// Send the aim delta to the PSP's RIGHT analog stick instead of the left one, for the CLEO
	// plugin. Requires memstick/PSP/PLUGINS/cleo - off by default because without the plugin
	// nothing reads that stick and aiming would silently do nothing.
	//
	// The right stick is a channel VCS itself never reads. On real hardware it doesn't exist;
	// PPSSPP carries it in the spare bytes of SceCtrlData (see CtrlData in sceCtrl.cpp, "the PSP
	// has only one stick, but has space for more info"). That's what makes it a safe side channel
	// - writing to it cannot disturb the game, only a plugin listening for it.
	//
	// Turning this on also disables the left-stick reticle, so the two can never fight over the
	// same aim, and stops the camera being driven while aim is held, since the plugin owns the
	// view then.
	bool aimViaRightStick = false;

	// Drop straight into free aim when the aim key goes down, instead of starting in lock-on and
	// requiring the player to break out. On by default: free aim is the point on a PC, and
	// lock-on is the special case here, the reverse of how the PSP scheme is arranged.
	//
	// Implemented by pulsing d-pad DOWN shortly after entering the Aiming context, which is the
	// game's own Free Aim button. Holding the lock-on key (CapsLock) suppresses the pulse and
	// leaves ordinary lock-on.
	bool autoFreeAim = true;

	// Ticks to wait after the aim trigger before pressing Free Aim.
	//
	// This is what decides whether lock-on is visible at all. The game begins hunting for a target
	// as soon as the trigger registers, so every tick of delay is a tick in which it can find
	// someone and snap to them - the "it locks onto the nearest NPC first" complaint is entirely
	// this number. These are 60Hz ticks against a 30fps game, so the old value of 9 was about
	// 150ms of open season.
	//
	// 0 presses Free Aim on the very same tick the trigger goes down. That is the least lock-on
	// the game can be given, and the value to start from; raise it only if free aim stops engaging
	// reliably, which would mean the trigger genuinely needs longer to register first.
	int aimFreeAimDelay = 0;

	// Aim with the direct camera writes rather than through the nub.
	//
	// The nub is a RATE control with the game's own acceleration on it, so aiming through it
	// feels like a thumbstick no matter how the sensitivity is tuned - that is the mechanism
	// showing through, not a tuning problem. In free aim the camera IS the aim, so writing
	// CameraYaw/CameraPitch moves the crosshair with the same 1:1 mouse feel that mouse look
	// already has, and the nub can be left alone entirely.
	//
	// INERT, and kept only so the reasoning stays attached to the code that embodies it.
	//
	// The idea was to write a stored gun direction the way mouse look writes CameraYaw. There is
	// no such value: the weapon camera integrates the look axis into its own angle and the gun is
	// resolved from that, so nothing holds an aim direction to be written. See the AimYaw note in
	// VCSAddresses.h for how that was established and what was ruled out.
	//
	// Everything gated on this is therefore unreachable - AimTick and the ReticleActive branch
	// both also require IsAddrSet(AimYaw), which is false and will stay false. aimResponseModel
	// is what actually solved the problem this was aimed at.
	bool mouseLookInFreeAim = true;

	// --- The pad's synthesised second stick ---
	//
	// VCS wants two analog sticks and the PSP has one, so the game builds the second from the
	// d-pad: camera X is (DPadRight - DPadLeft) / 2, Y is (DPadDown - DPadUp) / 2, in the pad
	// functions at 0x0898bb4c / 0x0898bb8c. Those fields are int16 but the real d-pad only puts
	// 0 or 255 in them, so a player can only ever produce -127, 0 or +127. Writing intermediate
	// values makes it a true analog axis - which is the game's OWN camera and aim input, so it
	// drives both, through the code path the game already has.
	//
	// Off by default: it is a different mechanism from the direct CameraYaw writes, and the two
	// should be compared before either becomes the default.
	bool usePadStick = false;

	// Stick deflection per unit of raw mouse movement, for the LEGACY path only - it is unused
	// while aimResponseModel is on, which is the default.
	//
	// The history here is worth keeping, because it is a clean example of a correct measurement
	// producing a wrong conclusion. This was lowered to 0.008 to leave headroom, which made the
	// path do nothing at all; it was then raised to 0.05 on the theory that "free aim reads these
	// fields as a button, not as an analog axis" - the evidence being that arrow keys put a clean
	// 255 in them and moved the aim, while writes of 26-50 into the very same fields did not.
	//
	// There is no button threshold. The fields are a real analog axis and always were; the game
	// squares it (see aimResponseModel), so an axis of 13-25 out of 127 produces about 1/400th of
	// the rotation 255 does - invisible, and indistinguishable from being ignored. Saturating was
	// treating the symptom. The model removes the reason to.
	float padStickSensitivity = 0.05f;
};

VCSCameraSettings &CameraSettings();

// --- The game's aim response model ---
//
// One axis worth of state for turning "rotate the aim by this many radians this frame" into the
// stick deflection that will actually do it. See AimAxisStep and the long comment above it in
// VCSCamera.cpp for the formula and where each constant came from.
//
// Two pieces of state, and both are needed:
//
//   mirror  our copy of the smoothed increment the game is carrying. The game's next increment
//           depends on it, so the deflection that produces a wanted rotation depends on it too.
//           This is also what lets a stop be a stop: when the mouse stops, the correct deflection
//           is NEGATIVE, cancelling the glide the smoother would otherwise coast through.
//   carry   rotation that was asked for and could not be delivered, because the axis ran out of
//           range. Spent on following frames, so a fast flick still travels the right distance
//           instead of being quietly truncated. Bounded - see kMaxCarryFrames.
struct VCSAimAxis {
	float mirror = 0.0f;
	float carry = 0.0f;
	float lastAxis = 0.0f;   // last deflection returned, for the debugger window

	void Reset() {
		mirror = 0.0f;
		carry = 0.0f;
		lastAxis = 0.0f;
	}
};

// Converts a wanted rotation, in radians, into a stick deflection in -1..1 (or wider, if
// aimRangeBoost allows and the channel can carry it). Emu thread only - it reads the game's
// timestep. Call exactly once per axis per frame, including with want == 0: a frame with no mouse
// movement is what tells the model to cancel the glide.
float AimAxisStep(VCSAimAxis *axis, float want, VCSAddr incAddr);

// The two axes the aim uses, exposed so the debugger can show what the model is doing.
void AimModelState(const VCSAimAxis **x, const VCSAimAxis **y, float *timeStep);

// Turns one frame's mouse movement into the pair of stick deflections that should be written,
// through the model when aimResponseModel is on and proportionally when it is not. `scale` is the
// legacy deflection-per-count used only in the second case.
//
// Both aim channels go through this. They write different memory - the nub at CPad+0x2, the d-pad
// pair at CPad+0x12..0x18 - but the game funnels both through the same accessor and applies the
// same response to them, so there is one model and one place to change it.
void AimDeflectionFromMouse(float dx, float dy, float scale, bool invertY,
		float *outX, float *outY);

// Drops the model's accumulated state. Call when aiming stops, so the next aim starts clean
// instead of inheriting a mirror value from a previous one.
void AimModelReset();

// Opens a new frame for the model, if the GAME has advanced one. Both aim channels ask it for a
// deflection and only the first ask per frame may step it, so something has to say where a frame
// begins - VCSGame's Tick does, before either channel runs.
void AimModelBeginFrame();

// Whether the model wants this tick's mouse movement now.
//
// False means the game has not run a logic frame since the last solve - we tick at ~60Hz and this
// game runs at 30fps - so the movement must be LEFT ACCUMULATING rather than drained, and the
// previous deflection re-asserted. Draining it would solve for movement the game then never reads,
// because the next tick's write overwrites ours before it samples the pad.
bool AimModelReady();

// The deflection last solved, for re-asserting on a tick that isn't a game frame.
void AimLastDeflection(float *x, float *y);

// The largest deflection the live channel can actually carry to the game - 1.0 on the nub, since
// sceCtrl clamps it, and aimRangeBoost on the d-pad channel. The model must solve against this
// rather than against the setting, or its mirror records rotation that never happened.
float AimChannelLimit();

// Ticks that were a real game frame, and ticks that were not. The ratio should sit near 1:1 on
// this game; anything else means the frame counter isn't what we think it is.
void AimFrameStats(u64 *gameFrames, u64 *skippedTicks);

// Consecutive still game frames. Once this reaches aimStopDelay the stroke counts as over and the
// stop fires; below it, the mouse is treated as merely between movements.
int AimStillFrames();

// Called from NativeKey's mouse sibling on the input thread. Returns true if mouse look claimed
// the movement, in which case PPSSPP's own mouse handling must not also act on it.
//
// Returns false - changing nothing for any other game - whenever VCS is inactive, mouse look is
// disabled, CameraYaw isn't known, or the player is in a menu.
bool HandleMouseDelta(float dx, float dy);

// Drains the mouse movement accumulated since the last call, and returns it. Emu thread only.
//
// Exists so the aiming path in VCSInput can take the delta for the reticle instead of leaving it
// to become a camera rotation. CameraTick calls this too, so the buffer is emptied every frame
// either way - the two callers never both consume a delta, because they are selected by context
// (Aiming versus everything else) and ApplyAnalog always runs first, leaving CameraTick nothing.
void TakeMouseDelta(float *dx, float *dy);

// What the last TakeMouseDelta handed out, without consuming anything. For the second consumer
// in a frame: during free aim the nub and the d-pad carry different axes of the same aim, so
// both need this frame's movement and only the first of them can drain it.
void PeekMouseDelta(float *dx, float *dy);

// Whether all four pad d-pad field addresses are known, so the second stick can be driven.
bool PadStickAvailable();

// Whether the mouse is currently driving that stick. When true, the camera is not written
// directly - the game moves its own camera in response to the axis we feed it.
bool PadStickActive(VCSInputContext context);

// Writes the synthesised second stick for this frame. Emu thread only, once per frame, and must
// run after ApplyAnalog/ApplyAimStick and before CameraTick - see the ordering note in Tick.
void PadStickTick(VCSInputContext context);

// Writes the gun direction directly during free aim. Emu thread only, once per frame, and must
// run before CameraTick so it gets the mouse delta first. Every frame is not optional - the game
// recomputes this value and an unasserted write is undone almost immediately.
void AimTick(VCSInputContext context);

// The stick position written on the most recent tick, for the debugger window.
void GetPadStick(float *x, float *y);

// Cumulative evidence that this path is alive, because the live x/y above cannot provide it:
// the axis is a rate control, so it self-centres the moment the mouse stops and reads zero in
// every screenshot. frames counts ticks where the path ran at all, nonZero counts ticks that
// actually pushed a deflection, and peak is the largest magnitude ever written.
//
// nonZero staying at 0 while frames climbs means the path is running but no mouse movement is
// reaching it - the usual cause being the cursor sitting over the ImGui debugger window, which
// takes the mouse before NativeMouseDelta can hand it here.
void PadStickStats(u64 *frames, u64 *nonZero, float *peak, bool *modeFlagSet);

// Walks the player during free aim by writing the ped's velocity. Emu thread only, once per frame,
// and it must run every frame - the game rewrites the field. Experimental; see moveInFreeAim.
void FreeAimMoveTick(VCSInputContext context);

// Walks the player during free aim by adding a step to their world position each frame. Emu thread
// only. Independent of the code patch above - see freeAimTranslate.
void FreeAimTranslateTick(VCSInputContext context);

// Turns the character to face the camera while moving in free aim, so the latched movement follows
// where you point. Emu thread only, every frame - the game rewrites the matrix.
void FreeAimFaceTick(VCSInputContext context);

// Applies the accumulated mouse movement to the game's camera. Emu thread only, once per frame.
// Does not touch the camera in the Aiming context - there the mouse belongs to the reticle.
void CameraTick(VCSInputContext context);

// Drops any pending movement. Called on init/shutdown so a delta from a previous session can't
// be applied to a fresh one.
void CameraReset();

// --- For the debugger window ---

// Whether the last tick actually wrote to the game.
bool CameraIsDriving();

// Mouse movement accumulated since the last tick, for showing that input is arriving.
float CameraPendingDeltaX();

// How many mouse deltas reached this layer, how many it claimed, and how many were rejected
// purely because of the input context. Distinguishes 'no events arrive' from 'events arrive
// but get rejected', which are completely different problems.
void CameraDiagnostics(u64 *seen, u64 *claimed, u64 *rejectedContext,
		float *maxAbsDx, float *sumAbsDx);

// What the layer is currently asserting, versus what it anchored to. If desiredPitch drifts
// away from the game's live pitch while holdFrames stays high, we are fighting the game.
void CameraHoldState(float *desiredYaw, float *desiredPitch, float *anchorPitch,
		int *holdFrames, const char **contextName);

// How many ticks actually pushed a new yaw into the game, and what the last one was.
void CameraWriteStats(u64 *writes, u64 *fails, float *lastWritten);

}  // namespace VCS
