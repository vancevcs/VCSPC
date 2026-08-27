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

	// Vertical sensitivity as a MULTIPLE of horizontal, for ordinary mouse look.
	//
	// 1.9 is not taste, it is what GTA III and Vice City do on PC. Both compute their offsets as
	//
	//     BetaOffset  = -2.5f*MouseX * m_fMouseAccelHorzntl(0.0025) * FOV/80
	//     AlphaOffset = +4.0f*MouseY * m_fMouseAccelVertical(0.003) * FOV/80
	//
	// so vertical runs at 0.012 against horizontal's 0.00625 - a ratio of 1.92 - even though both
	// axes are exposed to the player as one sensitivity slider. Rockstar shipped the asymmetry
	// deliberately, and the reason is geometric rather than arbitrary: the vertical range is about
	// 135 degrees of the game's own clamp against 360 of yaw, and the screen is wider than it is
	// tall, so an equal-gain vertical axis feels slower than the horizontal one it is matched to.
	//
	// ORDINARY LOOK ONLY. While aiming, aimPitchGain is the vertical multiplier and this one stands
	// aside - see the note there. That is a deliberate split rather than an oversight: aiming here
	// is a separate, measured channel with a deadband solve on it, and aimPitchGain was settled in
	// play at 1.0 with that solve in place. Silently multiplying it by 1.9 would retune an answer
	// somebody already worked for. Set aimPitchGain to 1.9 by hand if parity is wanted.
	float verticalGain = 1.9f;

	// Scale sensitivity with the camera's FOV, so a mouse count moves the view the same distance ON
	// SCREEN whatever the game has zoomed to.
	//
	// Also from re3/reVC, where it is the FOV/80 term above, and it is the one part of their mouse
	// path that has no equivalent anywhere in this file. Without it, scoping in narrows the view
	// while the mouse keeps turning the camera at the same ANGULAR rate, so the sensitivity
	// effectively multiplies by however far the scope zoomed - which is the wrong direction for the
	// one situation that needs precision most.
	//
	// This applies to the paths that write an angle DIRECTLY - mouse look, camera-driven aim, free
	// aim's AimYaw. It must never be applied to the reticle path: the game's own aim rate already
	// carries a FOV/80 term, and AimAxisStep keeps rather than cancels it. See FOVLookScale.
	//
	// What it does is MEASURED; where it fires is only partly so. Known FOV values, from the arsenal
	// sweep and the sniper zoom trace in CLAUDE.md:
	//
	//     ordinary play              70.00   ->  1.00x, i.e. nothing happens
	//     assault rifle raised       50.00   ->  0.71x
	//     sniper, zoomed out         64.40   ->  0.92x
	//     sniper, zoom level 2       33.07   ->  0.47x
	//
	// UNMEASURED, and stated as the expectation it is: the GTA vehicle cameras generally widen the
	// FOV with speed, which would make the look slightly faster the faster you drive. Whether VCS
	// does that, and by how much, has not been checked here - the Camera tab prints the live FOV and
	// the scale it produces, so watch it at speed rather than trusting this sentence.
	bool scaleByFOV = true;

	// Vertical look while driving. ON by default as of 2026-08-17, confirmed usable in play.
	//
	// It was off for a long time because asserting pitch against the vehicle camera ran the view away
	// to the -89 degree limit and left it there. That is now understood and fixed: the cause was
	// writing the field every vblank against a game that updates it at its 30fps logic rate, so two
	// of our writes landed per game frame and wound up the game's own camera integrator. Pitch is now
	// asserted once per game logic frame in a vehicle - see the FrameCounter check in CameraTick.
	//
	// **Known residual, accepted deliberately.** Forcing pitch hard down *through* the vehicle entry
	// and then continuing to force it down once seated can still provoke the runaway. Normal play does
	// not do this; reported as working "99% of the time", and enabled on that basis. The complete fix
	// is to drive the game's own control input rather than the position, as aimResponseModel does for
	// aiming, which needs mode 18's pitch writers at 0x089a1xxx reverse-engineered. See CLAUDE.md.
	bool pitchInVehicle = true;

	// How far below the entry angle, NUMERICALLY, vehicle pitch may travel - in radians. Note the
	// sign convention: more negative is looking UP, so this is the upward-look allowance, and the
	// entry angle is the limit in the other direction. (-6.8 deg is level-ish, -89 deg is the roof.)
	//
	// Both halves of that come from measurement, not taste. Vehicle pitch is spring-controlled, and
	// the spring's correction scales with how far we drag pitch off its target - so the fix is to not
	// drag it far. From a 7379-frame trace, mean correction per frame by region:
	//
	//     above the entry angle      0.4069 rad   (max 1.5044)  <- 3.2x worse, so forbidden entirely
	//     0.02 .. 0.15 below         0.0244 rad   (max 0.1280)  <- this band
	//     0.15 .. 0.30 below         0.0527 rad
	//     0.50 .. 0.75 below         0.2623 rad
	//
	// 1.45 rad is about -89 deg of look-up from the -6.8 deg baseline, i.e. effectively the full
	// range, and confirmed good in play.
	//
	// It was expected to shudder at this depth and it does not, which corrected the diagnosis. The
	// per-frame fight numbers above that appear to grow with depth were measured in a trace where
	// ABOVE-entry excursions were happening in the same session, so what looked like a
	// depth-dependent fight was largely the spring recovering from being pumped by those. Depth is
	// not the problem; going numerically above the entry angle is. **The ceiling is the fix**, and
	// this band can be as wide as the game's own range allows.
	float pitchVehicleDown = 1.45f;

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

	// Mouse acceleration for aiming: extra gain per count of mouse movement in a frame.
	//
	// 0 disables it entirely and the response stays purely linear. At 0.012, a slow 5-count frame
	// gets 1.06x - imperceptible - while a 40-count sweep gets 1.5x, so small corrections keep
	// their precision and deliberate movements cover ground.
	//
	// This deliberately adds back a nonlinearity that aimResponseModel exists to remove, and the
	// distinction matters: the game's own curve is a signed square nobody chose, which crushes slow
	// movement and cannot be tuned because it is a shape. This one is chosen, bounded, and applied
	// to the wanted rotation rather than to the stick, so it never eats the low end.
	float aimAccel = 0.012f;

	// Ceiling on that multiplier. The aim channel saturates, and past that point extra gain only
	// fills the carry - which arrives late and reads as the aim running away. 2.5x is about where
	// the nub runs out on ordinary sensitivities.
	float aimAccelMax = 2.5f;

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
	bool aimScopedLinear = false;

	// Aim scoped weapons by moving the CAMERA - the same direct write that makes mouse look smooth.
	//
	// For a scope this is not merely nicer, it is correct: looking down the sights means the camera
	// direction IS the firing direction, so there is no separate ped aim state to desynchronise from.
	// Sniper and RPG aim beautifully this way, confirmed in play.
	//
	// Which is exactly why the same write FAILS for every other weapon - in third person the
	// crosshair is drawn from the camera while the shot comes from the ped, so steering the camera
	// alone splits them. Same mechanism, opposite result, decided entirely by whether the weapon has
	// a scope. See mouseLookInFreeAim for the disproof.
	//
	// This supersedes aimScopedLinear, which gave those weapons a plain proportional stick mapping.
	// That was correct as far as it went - they were already linear, so the model's sqrt was
	// amplifying them - but the stick was never the best channel available for them.
	bool aimScopedCamera = true;

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
	bool moveInFreeAim = true;




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
	// game's own Free Aim button. The lock-on key (L) toggles the pulse off and leaves ordinary
	// lock-on, with the mouse standing down from the camera as well - see ContextDrivesCamera.
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
	// DISPROVEN, and left here only so nobody spends another evening on it.
	//
	// Writes CameraYaw/CameraPitch directly in free aim, the same path that makes mouse look smooth
	// on foot. It does move the crosshair, and it is beautifully smooth - and the shot goes
	// somewhere else entirely. Confirmed in play.
	//
	// So the crosshair is DRAWN from the camera while the shot is resolved from the ped's own aim
	// state, and the two are only kept in agreement because the stick normally drives both. Steering
	// the camera alone desynchronises them, which is worse than doing nothing: it looks correct and
	// misses.
	//
	// This is the same claim docs/VCS_ADDRESSES.md recorded years of work ago - "swings the VIEW,
	// gun keeps pointing where it was" - which was doubted here on the grounds that the note predated
	// knowing the camera modes. It did predate it, and it was right anyway. Re-testing it was still
	// worth doing; assuming it was wrong was not.
	//
	// The consequence worth internalising: the stick is the ONLY path to the gun, so the game's
	// integrator cannot be removed from aiming, only inverted. aimResponseModel is not a workaround
	// for lacking a better route - it IS the better route.
	bool mouseLookInFreeAim = false;

	// --- Turning the character with the aim ---

	// OFF, because THE GAME ALREADY DOES THIS and we were racing it.
	//
	// The weapon-aim camera writes the ped's heading itself, every frame. In mode 45's Process
	// (0x089a341c), at 0x089a3e20..0x089a3e5c:
	//
	//     angle = atan2(Front.y, Front.x)      ; the camera's own look vector, +0x10
	//     if (angle < 0) angle += 2*PI
	//     angle -= PI/2                        ; 0x3fc90fdb, loaded at 0x089a3984
	//     ped->m_fRotationCur  = angle         ; +0x8d0
	//     ped->m_fRotationDest = angle         ; +0x8d4
	//
	// That is the same formula this file worked out independently, which is a pleasant confirmation
	// and a completely redundant one. Two writers of one field at different rates - ours at vblank,
	// the game's at its 30fps logic rate - is the exact shape of the CameraPitch runaway documented
	// above, and here it shows up as the crosshair being unable to move horizontally until the body
	// has finished turning. Turning ours off leaves the game's, which is correct and free.
	//
	// Kept switchable rather than deleted because it is the direct test: if the character stops
	// turning with this off, the game's write is gated on something we do not satisfy, and that is
	// worth knowing rather than assuming.
	//
	// Note which vector the game uses - `Front`, not Beta/Alpha. Independent evidence that reading
	// the stored basis was the right call for the fire hook too; the game's own ped aiming has
	// always resolved through it.
	bool pedFollowAim = false;

	// Snap the heading rather than only asking the game to turn toward it.
	//
	// m_fRotationDest is the game's own request-a-turn channel and using it alone keeps the turn
	// animation and its rate. That rate is a thumbstick's rate, though, and a mouse is a position
	// control - lag between the crosshair and the gun is the same complaint the aim response model
	// exists to remove, one layer further out. So both fields are written by default and the
	// character tracks the view exactly.
	bool pedSnapHeading = true;

	// OFF. It was briefly on, and the round trip is worth keeping because of what misled it.
	//
	// `camYaw + PI/2` was checked four ways - the ped's own matrix in a savestate, the camera's
	// stored `Front`, `Source - LookAt`, and the in-play note at the yaw write in this file. All
	// agreed on slope +1. Play then said the character turned the wrong way, inverting appeared to
	// fix it, and it was made the default on the principle that play beats static reasoning.
	//
	// **Play was reporting a real symptom and the diagnosis was wrong.** The body was turning
	// correctly the whole time; what looked backwards was the GUN, which stays pinned to a
	// world-space aim point while the body rotates under it - "like a chicken's head". With one
	// part correctly tracking and another part world-locked, which one is "going the wrong way" is
	// genuinely ambiguous from the outside, and inverting made the wrong half agree with the frozen
	// half. See pedAimGun, which fixes the actual cause.
	//
	// What settled it was a case neither the static checks nor the free-aim testing covered: with
	// invert on, MELEE LOCK-ON movement came out reversed. A fix that breaks a neighbouring feature
	// is not a fix, and the neighbouring feature is often where a wrong sign shows up honestly -
	// free aim was too ambiguous to judge it, melee was not.
	//
	// The lesson is narrower than "play wins" and more useful: play beats static reasoning about
	// what is HAPPENING, not about what is CAUSING it. Four agreeing derivations were not wrong;
	// the inference from one symptom to one cause was.
	// DISPROVEN, kept at 0 so the result is not rediscovered by reasoning to it again.
	//
	// The idea: the aim camera clamps its own pitch against bounds we cannot read, so keep our
	// desired value from running more than this far ahead of the pitch it actually delivered
	// (readable as Front.z). Bounded dead travel instead of unbounded windup.
	//
	// In play, ANY non-zero value makes vertical aiming impossible. That falsifies the premise, and
	// the falsification is the useful part: Front does not CHASE Alpha, it has to be LED. Getting
	// Alpha well ahead is the only thing that makes Front move at all, so a leash that forbids
	// exactly that forbids pitching. Front is a follower with a deadband, not a clamped copy - which
	// is a different mechanism from the one this was built for, and the reason it could not work.
	//
	// See aimPitchGain for what the measurement actually supports.
	float aimPitchLeash = 0.0f;

	// Extra gain on the mouse's VERTICAL movement while aiming, on top of `sensitivity`.
	//
	// The aim camera's Front has a measured deadband of about 9.4 degrees: Alpha must lead it by that
	// much before the view moves at all, and then Front tracks at roughly 0.65 of Alpha's rate. At
	// the look sensitivity of 0.004 rad/count that is ~41 mouse counts of nothing before vertical
	// starts, which is the "Y needs a hard flick" complaint stated in units.
	//
	// This does not remove the deadband - it is the game's, and it lives behind the composed Front in
	// mode 45 - it just crosses it in fewer counts. Honest workaround rather than a fix, and it is a
	// safe one in a way the last two attempts were not: it only ever scales input UP, so it cannot
	// swallow movement the way the leash and the adopt both did.
	//
	// Only applied in the Aiming context, so ordinary mouse look is untouched - that half is
	// verticalGain's, and the two never compose. One vertical multiplier per channel.
	float aimPitchGain = 1.0f;

	// The lead, in radians, that CameraPitch must hold over the aim's real pitch before the game
	// moves it at all. This is a MEASURED property of the game, not a taste setting.
	//
	//     before pitching   Front pitch -0.0442   CameraPitch -0.0442   off  0.00 deg
	//     still no movement Front pitch -0.0442   CameraPitch +0.1198   off -9.40 deg
	//     first movement    Front pitch -0.0390   CameraPitch +0.1278   off -9.56 deg
	//
	// 9.4 degrees is 0.164 rad, hence the default. It is applied as an instant OFFSET at the start of
	// a stroke rather than as gain, which is the difference between "pitch starts immediately" and
	// "pitch is twice as fast forever" - the deadband is a constant, so paying for it with gain
	// overcharges every movement that was never near it.
	//
	// Too small and the delay comes back; too large and the aim overshoots slightly before settling,
	// because the lead is larger than the gap it is cancelling. Set to 0 to go back to driving
	// CameraPitch directly.
	float aimPitchDeadband = 0.165f;

	// ZERO, settled in play, and the most useful negative result of the lot: YAW DOES NOT NEED THE
	// LEAD. Only pitch does.
	//
	// It was added on the reasonable-looking grounds that the horizontal deadband was reported at the
	// same 9.4 degrees. It is also the change that introduced the snap - reported the same build it
	// landed in - and every subsequent attempt to cure that snap failed, because they were all
	// treating a mechanism that only existed because of this setting. Turning it off removes the snap
	// and costs nothing: horizontal aiming was already smooth on the plain accumulator.
	//
	// Worth keeping the asymmetry in mind rather than explaining it away: the two axes reach Front by
	// genuinely different routes in mode 45 - pitch through SetRotateX(Alpha), yaw through a Z-rotate
	// whose offset is picked from four quadrants by ped+0x780 - so there is no reason to expect one
	// number to describe both. The 9.4 degrees they appeared to share was measured on pitch.
	float aimYawDeadband = 0.0f;

	// The SAME lead as pitch, in the direction of the stroke, applied WITHOUT reading Front.
	//
	// Measured 2026-08-21, in free aim, off the LIVE Front yaw row: Beta leads Front's yaw by
	// 9.56 deg (0.166 rad) and Front does not move at all, symmetrically - push right and the offset
	// is +9.56, push left and it is -9.56 - and it releases the instant the view breaks loose. A
	// constant between the two spaces would hold ONE signed value both ways rather than mirroring, so
	// this is stiction, and it is within noise of pitch's 0.165.
	//
	// THAT FALSIFIES THE HEADING ABOVE aimYawDeadband, and the correction is worth stating plainly:
	// "yaw does not need the lead" was never measured. It was inferred from the lead making things
	// worse, which it did. Both halves were true and the conclusion joining them was not - yaw needs
	// the same lead pitch does, and what failed was the MECHANISM that delivered it.
	//
	// So the lead is the same and the structure is different, which is the whole design:
	//
	//   aimYawDeadband  desired = liveYaw + error + bias, error measured against Front's yaw.
	//                   A closed loop through a read-modify-write, on a lever written directly. The
	//                   bias flips when the error crosses zero and the lever moves 2D in one frame -
	//                   the measured 0.340 rad snap against 0.328 predicted.
	//
	//   aimYawKick      desired = accumulator + kick * strokeDirection.
	//                   Nothing is read back, so there is no error, no crossing, and nothing to
	//                   re-solve. The lever cannot move further than the mouse asked for plus one
	//                   kick, ever. That is why this can carry the full 0.166 where the other could
	//                   not carry a fifth of it.
	//
	// The direction comes from the MOUSE, not from an error term, and only reverses once the player
	// has genuinely pushed the other way - see aimYawKickHysteresis. A reversal costs 2*kick in one
	// frame, which is the same arithmetic as the snap; the difference is that it happens only when
	// the player asks for it, on a frame they are already moving, rather than spontaneously at a zero
	// crossing in the middle of a steady stroke.
	//
	// Set to 0 to go back to driving CameraYaw with the plain accumulator.
	float aimYawKick = 0.166f;

	// How far, in radians of intent, the mouse must travel AGAINST the current lead before it moves
	// to the other side.
	//
	// This is the one thing standing between the kick and the snap it replaces. Mouse deltas flip
	// sign between frames constantly inside an ordinary stroke - a hand is not a stepper motor - and
	// flipping the lead on each of those would inject 2*kick every time, which is the limit cycle
	// again with a different trigger. Requiring sustained travel the other way means only a
	// deliberate reversal pays it.
	//
	// A quarter of the kick, so a reversal is recognised well before the player has spent a whole
	// deadband wondering why the view stopped following.
	float aimYawKickHysteresis = 0.04f;

	// Drop the kick once, on the first still frame after a yaw stroke.
	//
	// OFF, and it shipped ON for exactly one build. Reported immediately: the aim snapped somewhere a
	// moment after each stroke ended, and unchecking this made it perfect. So the bookkeeping argument
	// below was wrong, and the way it was wrong is the interesting part.
	//
	// The argument was: the hold window expires 45 ticks after the last movement and the game inherits
	// whatever is in Beta, so leaving the lever parked a deadband past the aim hands the game a value
	// the player never asked for. Every clause of that is true. What it assumed without saying so is
	// that moving the lever back by D is INVISIBLE - that the deadband is a standing gap Front sits
	// inside, so a step of one deadband stays within it.
	//
	// It is not a standing gap. THE DEADBAND IS STATIC FRICTION: it gates the ONSET of movement and
	// does not persist once the aim is at rest. A full-deadband step applied to a resting lever is
	// therefore exactly the step that breaks it loose again, and the view follows - which is the snap,
	// and it is the retract working as designed rather than misfiring.
	//
	// That also explains, from a second direction, why pitch has run with its lead held constantly and
	// aimLeadRetract off since the beginning: there is nothing to retract, because a lead that is
	// never moved never moves anything.
	//
	// Kept switchable rather than deleted. The mechanism it exposes is worth more than the setting.
	bool aimYawKickRetract = false;

	// Drop the deadband lead once, on the first still frame after an aim stroke.
	//
	// Without it, the hold window expires with CameraPitch/CameraYaw still parked a deadband past
	// where the player stopped, the game inherits that as its own angle, and the view snaps.
	//
	// ON, but switchable, because the last two attempts at this both shipped broken and a rebuild is
	// a poor way to find that out. If aiming misbehaves at the END of a movement, turn this off first
	// - it is the only thing that touches that moment.
	//
	// Note what it deliberately does NOT do: re-solve every still frame. The lever is written
	// directly, so each pass would add another deadband to it - about 9.4 degrees per frame, sixty
	// times a second. That was tried and made aiming unusable.
	// ZERO. Width of the band over which the deadband lead would blend to zero as the aim arrives.
	//
	// Built to stop the lead chattering when its sign flips at the crossing, which was a real
	// mechanism with a matching measurement - a 0.340 rad jump against 0.328 predicted for 2*D. It did
	// not fix the snap, because the chatter it addressed was in the YAW solve, and the answer there
	// turned out to be not running that solve at all. Pitch never crossed zero mid-stroke in any
	// capture, so it never chattered and never needed this.
	//
	// Kept because the reasoning is sound and would apply again if the yaw lead is ever revived.
	float aimLeadBlend = 0.0f;

	// OFF. Would apply the deadband lead only while the aim is stalled, dropping it once moving.
	//
	// The reasoning came from a measured snap - Front moved one deadband while the camera angle moved
	// two - and read as Front converging ONTO the lever rather than settling short of it, which would
	// make a held lead overshoot by D. Plausible, and it did not help either, for the same reason as
	// the blend: the snap lived in the yaw solve, not in how the lead was held.
	//
	// Pitch works with the lead held constantly, which is what ships. That is evidence against the
	// "converges onto the lever" reading, so treat it as unproven rather than as background fact.
	bool aimLeadOnStallOnly = false;

	// OFF - it did not fix the snap, because the snap was the bias flipping sign mid-stroke rather
	// than anything left behind at the end of one. Superseded by aimLeadBlend, which addresses the
	// actual mechanism. Kept switchable rather than deleted since it is harmless and the reasoning
	// behind it still holds for what it was aimed at.
	bool aimLeadRetract = false;

	bool pedHeadingInvert = false;
	float pedHeadingOffsetDeg = 0.0f;

	// Move the ped's point-gun-at target to the crosshair, so the GUN follows the view and not just
	// the body. This is the half of "the character aims where I aim" that the heading cannot do.
	//
	// The ped aims its arms at a world POSITION, which is why the gun reads as world-locked once
	// the stick stops being fed: nothing moves that position any more, so the hand holds its
	// bearing while the body turns under it. Writing the position every frame is the same discipline
	// every other value in this fork follows.
	//
	// Guarded hard, because this writes an ENTITY's position. It only ever moves an entity of type
	// 7 (DUMMY) - the placeholder the game parks at the free-aim point - and never a real ped, which
	// would teleport an NPC. That is the same test the game's own FireInstantHit makes before
	// trusting the cached position at CPed+0xC80.
	bool pedAimGun = true;

	// How far along the crosshair ray to park it. Far enough that the offset between the camera and
	// the muzzle stops mattering for the resulting bearing; near enough to stay inside the world.
	float pedAimGunDistance = 40.0f;

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

// The acceleration multiplier applied on the last solve, for the debugger.
float AimAccelGain();

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

// Which side the open-loop yaw kick is parked on (-1, 0, +1) and how far the mouse has travelled
// against it since the last flip. Exported so the debugger can show the chatter the reversal
// threshold exists to prevent, rather than leaving it to be felt in play.
void YawKickState(float *side, float *againstIt);

// The factor scaleByFOV is currently applying to the direct angle writes, clamps included, or 1.0
// when it is off or the FOV does not read as a believable one. Exported so the debugger can show
// what is actually being applied rather than recompute the formula and drift from it.
float FOVLookScale();

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

// Adds look movement from something that is not a mouse - today the pad's right stick, via
// ApplyPadLook.
//
// Deliberately NOT HandleMouseDelta with a different caller. That one is a claim: it answers
// whether mouse look wants the movement, and it is gated on the mouse being enabled and on the
// context wanting it. A stick has already been claimed by the time it gets here, its caller has
// already decided the context is one that looks around, and it must keep working for a player
// who has mouse control switched off. What is shared is everything downstream - one accumulator,
// one sensitivity, one aim solver - which is the point.
//
// Input thread or emu thread; the accumulator is mutex-guarded either way.
void AddLookDelta(float dx, float dy);

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

// Turns the CHARACTER to face where the camera is aimed, during camera-driven free aim.
//
// The bullet is redirected at the fire site, but the man holding the gun is not: his gun points
// along his heading, and free aim normally turns that because the stick turns it. Once the mouse
// writes CameraYaw directly the stick is no longer fed, and - because free aim also skips the
// movement call, so nothing re-evaluates the heading either - he stands frozen aiming wherever he
// happened to be pointing when aim went down. Same latch that makes movement in free aim possible;
// here it is the failure rather than the feature.
//
// Emu thread only, once per frame. Reports whether it wrote, for the debugger.
void PedAimTick(VCSInputContext context);

// Whether PedAimTick is currently steering the character, and the heading it last asked for.
void PedAimStats(bool *driving, float *desiredHeading, u64 *writes);

// The entity the gun is currently aimed at, its type (3 = ped, 7 = dummy), and how many times the
// aim point has been moved. A target that is never a dummy means free aim is not using the
// placeholder this assumes, and pedAimGun is doing nothing.
void PedGunStats(u32 *target, int *entityType, u64 *writes);

// Both aim axes at once: what we asked for, what the game currently has, and how often the game
// overrode our pitch. Watching live against desired WHILE CIRCLING THE MOUSE is what separates the
// candidate causes of a circle coming out square - a pitch that sticks while yaw keeps moving shows
// up here as the two diverging, and a view that lags both equally does not.
void AimAxisStats(float *desiredYaw, float *liveYaw, float *desiredPitch, float *livePitch);



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
