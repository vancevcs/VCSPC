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
	// OFF: still experimental and it does not behave the way it reads. The vehicle camera is
	// the spring this fork spent a long time learning not to fight, and pitching it is the
	// part of that fight still unresolved - so the default is the behaviour that works.
	//
	// Worth being deliberate about because the Mouse page no longer has a row for it: with
	// the row gone, this default IS the setting for every player.
	bool pitchInVehicle = false;

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

	// --- Handing the camera back ---
	//
	// Mouse look does not persuade the game's camera, it OVERRIDES it: `CameraYaw` is written every
	// tick for a window after the last movement, and the on-foot follow camera (CCam mode 15,
	// 0x08999f60) computes its own Beta from the player's heading regardless - it never reads a look
	// axis at all. So for the length of the window the player sees our value, and the game sees its
	// own, and the two are free to disagree by as much as the player turned.
	//
	// Which made the END of the window a cliff. Full authority on one tick, none on the next, and
	// whatever the game had been quietly computing underneath arrives in a single frame. That is the
	// "it snaps back about a second after I stop" report, offset and all: the offset is how far the
	// game's own follow logic had drifted while it was covered up.
	//
	// The fix is to stop dropping the camera and hand it over instead. Once the hold expires the
	// written value walks toward the game's live value across `lookReleaseFrames`, on a smoothstep,
	// and the last frame of the walk writes exactly what it read - so the tick where we stop writing
	// changes nothing by construction. There is no snap left to feel because there is no step left
	// anywhere in the sequence.
	//
	// It is deliberately blind to WHY the two values differ, and that is the useful property. If the
	// game had in fact adopted our value, the gap is zero, every step of the walk is a no-op, and the
	// camera simply stays where the player put it. If the game kept its own, the walk covers the gap
	// smoothly. Both readings of the follow camera's behaviour give a smooth handback, so this did
	// not have to wait on reverse-engineering mode 15's target field to be correct.
	//
	// The proper fix, for when that field IS known, is to write the game's own follow TARGET so it
	// converges on the player's view rather than being painted over. Then the handback is free.

	// Hand the camera back to the game AT ALL. Off keeps the view exactly where the player put it,
	// indefinitely: no hold expiry, no walk, no return.
	//
	// The master switch over everything else in this section, and the honest end of the road the
	// last three changes were walking down. The handback exists to give the game its camera back
	// politely; if the player would rather it never took it back, there is nothing left to be polite
	// about and every setting below stops meaning anything.
	//
	// What it costs is the follow camera entirely, which is the point but is worth stating: the view
	// no longer swings behind you as you walk, and IN A VEHICLE it no longer swings behind the car -
	// so a turn leaves you looking at the side of it until you move the mouse. That is what "never
	// return" means rather than a defect, and it is one click back.
	//
	// The write discipline is unchanged, which is what makes an indefinite hold safe rather than a
	// longer exposure to an old bug: pitch is still asserted once per GAME logic frame in a vehicle
	// and still clamped to the anchor window, so nothing is being written faster or further than it
	// was during the hold's first three quarters of a second.
	//
	// OFF by default, which is a decision about this game rather than a general preference. The
	// return was chased through four builds - a fade, a heading target, a learner, an idle hold -
	// and every one of them landed on the same wall: the follow camera has a resting position while
	// walking and a different one at a standstill, so a handback at rest is a value the game will
	// always disagree with. Not returning at all is the only version with nothing left to snap, and
	// see recenterOnGlance for how the default view is asked for on purpose instead.
	bool returnLook = false;

	// Ticks of full authority after the last look movement. These are ~60Hz emulator ticks, so 45 is
	// about three quarters of a second - long enough to cover the gaps between mouse events without
	// the camera feeling stuck to the player's last flick.
	//
	// Ignored entirely when returnLook is off - the hold simply never expires.
	int lookHoldFrames = 45;

	// Ticks the handback takes once that expires. Raise it for a longer, gentler return; 0 goes back
	// to the old cliff, which is only useful for confirming what this setting is for.
	//
	// The walk advances once per GAME logic frame rather than once per tick - the game runs its
	// camera at 30fps against our 60Hz, and stepping twice per frame would be the same double-write
	// that wound up the vehicle pitch integrator - so 45 ticks is about 22 steps.
	int lookReleaseFrames = 45;

	// End the handback exactly BEHIND the character, rather than wherever the game's camera
	// happens to be sitting when the walk starts.
	//
	// The two are not the same thing, and which one is which is measured rather than assumed:
	// reported in play, pitch always comes back to -4.6 deg and yaw does not come back to behind
	// the player at all. So the game returns pitch to a baseline by itself and leaves yaw where it
	// finds it - which means a handback that fades toward the game's LIVE yaw is fading toward the
	// value we ourselves last wrote, and correctly does nothing. Smooth, and not what anyone wants
	// the camera to do after they stop looking around.
	//
	// The target is the player's own heading, converted with the same quarter turn PedAimTick
	// inverts:
	//
	//     heading = camYaw + PI/2        (PedAimTick, shipped and confirmed in play)
	//     camYaw  = heading - PI/2       (therefore, and this is what the walk aims at)
	//
	// ON FOOT ONLY. In a vehicle the camera already returns behind the car on its own, `PedHeading`
	// belongs to a ped who is sitting in a seat rather than facing where the car points, and the
	// vehicle camera is the one with the spring that this fork has been careful not to fight. There
	// the walk keeps fading to the game's live value, which is the behaviour that was already right.
	//
	// The heading is re-read on every step, so a player who is walking or turning during the walk is
	// tracked rather than aimed at once and missed.
	bool returnBehindPlayer = true;

	// LEARN where behind-the-player actually is, instead of trusting the quarter turn.
	//
	// `camYaw = heading - PI/2` is derived from PedAimTick's own inverse and it is not wrong, but it
	// describes a camera sitting exactly opposite the character's facing - and the game's follow
	// camera does not sit there. Reported in play: the walk lands on that value and the game then
	// snaps 15-35 degrees off it. The size is the evidence. The known geometric gap between Beta and
	// the camera's Front is 4.42 degrees, so a constant this much larger is not that gap, and a range
	// that wide is not one constant being slightly wrong either.
	//
	// So measure it rather than pick it. While the player WALKS IN A STRAIGHT LINE and we are not
	// driving the camera, the game has the camera wherever it wants it - so `CameraYaw - PedHeading`
	// is, by definition, the offset the follow camera is holding. Sample it there, low-pass it, and
	// aim the handback at `PedHeading + offset`.
	//
	// The three gates on a sample are all doing work:
	//
	//   MOVING          the follow camera recentres while you walk and leaves the view alone when
	//                   you stand still - which is what the first round of this fix established, and
	//                   why a sample taken standing still would only measure where the player last
	//                   left the camera pointing.
	//   NOT TURNING     the camera trails a turn, so a sample mid-turn measures that lag rather than
	//                   the offset. Steady-state lag is proportional to turn rate, and a straight
	//                   line has a rate of zero.
	//   CAMERA SETTLED  the same test from the other end. Both must be still for the pair to be a
	//                   resting relationship rather than two things in motion.
	//
	// Off falls back to the derived quarter turn, which is also what runs until enough samples have
	// been collected. Either way `returnBehindTrimDeg` is added on top.
	bool learnFollowOffset = true;

	// Hand trim on the handback target, in degrees. Added to the learned offset, or to the quarter
	// turn when learning is off or has not run yet.
	//
	// Here so the number can be dialled in from play without a rebuild if the learner cannot get a
	// clean sample - watch the live and learned values on the Camera tab and put the difference in
	// here. Zero is correct when the learner is working.
	float returnBehindTrimDeg = 0.0f;

	// Don't hand the camera back at all while the player is standing still. Wait for them to move.
	//
	// This is the answer to a measurement rather than a preference. The learner settled at exactly
	// -90 degrees over 1671 samples, so `heading - PI/2` IS where the follow camera sits WHILE
	// WALKING - the derived quarter turn was right all along. But the camera read -59.2 degrees off
	// the heading once the walk had finished and the player was standing still, 30.8 degrees from
	// where the handback left it, which is the whole of the reported 15-35 degree snap.
	//
	// So the game has a resting position while walking and a different one while idle, and no value
	// we hand over standing still is going to be the one it wants. Two ways out, and this is the
	// cheaper: don't let go until the player is moving, at which point the game's own recentring is
	// running and is provably heading for the same -90 the walk aims at. The camera stays exactly
	// where the player left it in the meantime, which is also what GTA San Andreas does.
	//
	// The expensive way out, for when it is needed, is finding mode 15's own stored Beta and writing
	// THAT - then the handback stops being a negotiation.
	//
	// Costs a permanently asserted write while standing still, which is what the hold already does
	// for its first three quarters of a second and the game does not fight.
	bool holdUntilMoving = true;

	// The vehicle glance keys - Q, E, or both - put the view back behind the car.
	//
	// This is what makes `returnLook = false` liveable in a vehicle rather than merely quiet. With
	// no automatic return there has to be a deliberate one, and the glance keys are already the
	// "look somewhere else for a moment" control, so asking them for the default view back costs no
	// new binding and reads as the same gesture.
	//
	// It also fixes something that was broken the moment the hold became indefinite: a glance is the
	// GAME's own look mechanic - L trigger plus a stick direction, see GlanceDirection - so it moves
	// the game's camera, and a permanently asserted CameraYaw simply painted over it. The glance
	// appeared to do nothing. Now it ends the hold: the walk takes the view back to the car's default
	// bearing and then stops writing, which is both the recentre being asked for and the camera
	// being handed over so the glance itself works from the position it expects.
	//
	// The walk targets the heading directly rather than the game's live yaw - the same
	// `heading + offset` the on-foot return uses, which is -90 degrees - so it lands on the value
	// the game agrees with instead of negotiating with a camera that is holding our own number.
	//
	// Independent of returnLook: with the return on, this just brings the handback forward from
	// whenever the hold would have expired. It only MATTERS with it off, because that is the
	// configuration where nothing else would ever give the camera back.
	bool recenterOnGlance = true;

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
	// Zero: strictly linear. Extra gain on fast movement is the first thing a PC player turns
	// off, and the row that tuned it is no longer on the Aiming page - so the default has to
	// be the value someone would have chosen, not the one the aim model happened to be
	// measured at. Still bound in the debugger's Camera tab for anyone who wants it back.
	float aimAccel = 0.0f;

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

	// Aim the passenger drive-by with the mouse, by giving it the nub.
	//
	// A THIRD aim mechanism, and neither of the two above. There is no aim control to hold - the
	// game puts you in it for as long as you are riding shotgun with a weapon - and both aim flags
	// stay 0 the whole time, so every gate written against them reads "not aiming" and the mouse
	// goes to the camera. Measured in the passenger seat during a mission: the camera turned and
	// the gun did not, and the only thing that moved the gun was A and D, which are the stick.
	//
	// The nub IS the aim here, exactly as it is in free aim, so the fix is the same one - spend
	// the delta on the stick rather than on the camera angle. See DriveByAimActive for how the
	// state is recognised, and note the response model already knew this camera: mode 11 has been
	// in its table, with its own alpha and rate, since it was first measured.
	bool driveByMouseAim = true;

	// Deflection per count of mouse movement in a drive-by. Proportional, NOT through the response
	// model, and that is a correction rather than a shortcut.
	//
	// The model inverts one specific mechanism: the weapon camera's smoothed increment, CCam+0x130
	// and +0x124, which it reads back every frame to cancel the momentum it predicted. Mode 11 was
	// in its table of modes that own those fields, so this looked settled. Measured instead, across
	// 551 samples of an actual drive-by: BOTH increments read exactly 0.000000 the entire time.
	// The mode-11 aim does not go through that integrator at all, so there was nothing there to
	// invert and the solver was answering a question the game never asked.
	//
	// What that cost was an axis imbalance, and the numbers say why it landed where it did. The
	// model's rate goes as the SQUARE of the game's axis scale, and the drive-by runs 2.5 on X
	// against 0.5 on Y - a 25x ratio, so it asked for 5x less deflection on X than on Y for the
	// same rotation. Reported exactly that way: Y smooth, X "possible but hard".
	//
	// Proportional has no such term. The per-axis scale is still divided out below, so a
	// horizontal sweep and a vertical one cover the same distance per count.
	float driveBySensitivity = 0.045f;

	// Divide the game's own aim axis scale back out, so the two axes match.
	//
	// The drive-by is set up by one `03E9 2.5 0.5` in the retail script - wide horizontally,
	// damped vertically - and those land on the axis before the game consumes it. Left in, the
	// same mouse movement travels five times further sideways than up.
	//
	// On by default because a mouse has no reason to inherit a thumbstick's asymmetry, and worth
	// keeping switchable because it assumes the consumer is LINEAR in the scaled axis, which is
	// the one thing here that is reasoned rather than measured. Off gives the game's own balance.
	bool driveByMatchAxes = true;

	// Aim a mounted cannon's ELEVATION with the mouse - the fire truck's water cannon.
	//
	// A second in-vehicle state where the stick is an aim, and it looks nothing like the drive-by:
	// no weapon camera at all (`WeaponCamMode` reads 0), the camera stays in one mode throughout,
	// and the axis scale is left at its default. The game's own hint says it outright - "while it
	// is spraying, use the analog stick to adjust the cannon's aim".
	//
	// Only the Y axis is taken, and that is the whole safety argument rather than a limitation.
	// Vertical is the half that is missing - A and D already reach the cannon's yaw, because they
	// are the steering row and steering is the stick's X - while X is never touched, so driving
	// this truck cannot break no matter what state the mission is in. Measured: with the stick
	// held at full Y, exactly one field in the whole vehicle moved, the cannon's pitch.
	//
	// There is deliberately NO "is it spraying" test, because no trustworthy one exists.
	// `IsFreeAiming` was the obvious candidate and was measured oscillating 0/1 on a ~15 second
	// timer while merely driving, with the cannon stationary - gating on it would have taken the
	// stick away from steering twice a minute. The vehicle model is the gate instead: it cannot
	// be wrong, and being wrong here costs the player their steering.
	bool cannonMouseAim = true;

	// Deflection per count of mouse movement for the cannon's elevation. Proportional, like the
	// drive-by and for the same reason - there is no camera increment here to model.
	//
	// Small because the travel is small: the pitch moved 0.25 in two seconds at FULL deflection
	// and appears to clamp around 0.05, so this is a fine adjustment rather than a sweep.
	float cannonSensitivity = 0.02f;

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
	// ZERO, settled in play, and it is the fix for the dead zone that outlived three builds of
	// looking for it somewhere else: hold aim, sweep, stop without releasing, sweep again - and the
	// view refuses to follow until the mouse has spent a chunk of travel on nothing.
	//
	// The argument for having a threshold was that mouse deltas flip sign between frames constantly
	// inside an ordinary stroke - a hand is not a stepper motor - and flipping the lead on each of
	// those would inject 2*kick every time. That is a real mechanism. What it does not do is justify
	// THIS shape of guard, because of a detail that only shows up when you follow what the lever is
	// doing during the spend:
	//
	//   The threshold does not prevent the reversal cost. It DELAYS it. The 2*kick flip happens
	//   either way - all the travel buys is when. And during the spend the lever is still moving,
	//   because yawStep is added regardless of what YawKickStep returns; it is the AIM that is not
	//   moving, since an aim that has come to rest has no lead in front of it and the lever has to
	//   cross a whole deadband before the game will follow. So the player pushes, the lever moves,
	//   nothing happens on screen, and then it all arrives at once.
	//
	// Watched in the debugger, at default: 0.04 of intent goes in and 0.34 comes out - the settings
	// read back exactly, 0.04 being this and 0.332 being 2*aimYawKick. Not a coincidence to explain,
	// an arithmetic identity to act on.
	//
	// At zero the flip lands on the first frame of the reversal, which is a frame the player is
	// already moving on, and 2*kick is exactly what an aim at rest needs to break loose. That is the
	// header comment above aimYawKick predicting its own best case and the guard preventing it.
	//
	// If the chatter the threshold was built for ever does appear - it would be a low-speed
	// phenomenon, a slow sweep wobbling around the sign - do not bring this back. A travel threshold
	// taxes every genuine reversal to catch a wobble; filter the sign, or require the reversal to be
	// sustained in TIME, and leave the deliberate one free.
	float aimYawKickHysteresis = 0.0f;

	// How many ticks THE GAME'S OWN AIM must sit still before the kick is re-armed, so the next
	// stroke pays for it again.
	//
	// ZERO - THIS WAS NOT THE FAULT, and it is here as the negative result rather than as a feature.
	// The dead zone it was built for is the aimYawKickHysteresis spend above, and setting that to 0
	// fixes it outright with this switched off.
	//
	// The kick is one-shot per stroke direction and its sign then sits there for as long as aim is
	// held. That is right for the frames inside a stroke and wrong the moment the player pauses
	// without letting go, because the lead the sign claims to be holding does not survive the pause -
	// "it releases the instant the view breaks loose" is the measurement this was built on, and by
	// the time the aim has stopped the lead has been spent. The sign outlives the radians.
	//
	// Both halves of the reported fault come out of that one staleness, and they are worth writing
	// down together because they look like two different bugs:
	//
	//   Push the SAME way again. `dir == sign`, so YawKickStep takes its early return and adds
	//   nothing. There is no lead standing in front of an aim that has come to rest, so the player
	//   spends a whole deadband getting it moving. That is the dead zone.
	//
	//   Push the OTHER way. The hysteresis eats 0.04 rad of intent, and then the flip costs
	//   `want - lead` = 2 * kick = 0.332 in one frame. The aim needed one kick and got two.
	//   MEASURED IN PLAY at 0.04 in and 0.34 out, both directions, which is what turned this from a
	//   theory into an arithmetic check.
	//
	// So the test is whether the AIM has stopped, not whether the mouse has. An earlier build counted
	// still mouse ticks and was worse, for a reason worth keeping: a slow sweep produces whole ticks
	// with no mouse counts in them, so it re-armed mid-stroke and added a second kick on top of the
	// one already working. Front moving is the difference between "the player is being gentle" and
	// "the aim has arrived", and only the game can answer that.
	//
	// Reading Front here is not the closed loop this file warns about. Nothing is solved from it and
	// nothing is written: it is a DIFFERENCE between consecutive ticks, used once, to answer a yes/no
	// question. That also makes it immune to the quadrant constant between Beta's space and Front's,
	// which an absolute offset would have to get right.
	//
	// Three ticks would be the value if it were ever wanted - a settle confirmation rather than a
	// guess about the physics, since one tick of Front under the epsilon can happen mid-glide.
	//
	// TWO BUILDS WENT INTO THIS AND BOTH WERE REPORTED WORSE, which is worth recording in order:
	//
	//   The first counted still MOUSE ticks. Worse immediately, and the reason generalises: a slow
	//   sweep produces whole ticks with no mouse counts in it, so it re-armed mid-stroke and stacked
	//   a second kick on the one already working.
	//
	//   The second is what is written above - Front not moving AND the mouse still. That is the right
	//   test if this question is ever the right question. It was not; the delay was never a missing
	//   kick, so a better way of deciding when to add one could not help.
	//
	// The reasoning that produced it still looks sound, and that is exactly why it is being kept
	// switchable instead of deleted: a parked sign genuinely does outlive the radians behind it. It
	// simply is not what anyone was feeling. Do not rebuild it from scratch on the strength of that
	// argument alone - it has already been built twice.
	int aimYawKickRearmTicks = 0;

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

// Converts a wanted rotation, in radians, into a stick deflection in -1..1. Emu thread only - it
// reads the game's
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

// The largest deflection the live channel can actually carry to the game - 1.0, since the nub is
// the only channel and sceCtrl clamps it there. The model must solve against this rather than
// against the setting, or its mirror records rotation that never happened.
float AimChannelLimit();

// Which side the open-loop yaw kick is parked on (-1, 0, +1), how far the mouse has travelled
// against it since the last flip, and how many ticks the game's aim has been at rest. Exported so
// the debugger can show the chatter the reversal threshold exists to prevent, rather than leaving it
// to be felt in play - and, with the rest count, whether the re-arm is firing when the aim actually
// stops or partway through a slow stroke.
void YawKickState(float *side, float *againstIt, int *restTicks);

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

// Writes the gun direction directly during free aim. Emu thread only, once per frame, and must
// run before CameraTick so it gets the mouse delta first. Every frame is not optional - the game
// recomputes this value and an unasserted write is undone almost immediately.
void AimTick(VCSInputContext context);

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

// Steps left in the handback, or 0 when it is not running. Watch it against the live yaw: the
// walk is working when the two converge, and pointless when the gap was zero to begin with.
int CameraReleaseFrames();

// The yaw the handback is currently walking toward, and whether that came from the player's
// heading (returnBehindPlayer) rather than from the game's own camera. Only meaningful while
// CameraReleaseFrames() is nonzero.
float CameraHandbackTargetYaw(bool *behindPlayer);

// What the follow camera's own offset looks like: the value being used (radians, CameraYaw minus
// PedHeading), the raw instantaneous one for comparison, how many clean samples went into it, and
// the player's current speed - which is the gate that decides whether a sample counts at all, so
// seeing it is how you tell "not learning" from "never moving".
void FollowOffsetState(float *used, float *live, u64 *samples, float *speed);

// The last handback's aftermath: what the camera did on the game frames after we stopped writing,
// as offsets from the player's heading in radians. `written` is the offset we handed over, `count`
// how many frames were captured, and `trace` points at them oldest-first.
//
// This is the measurement that says WHAT KIND of thing the game does when it gets the camera back.
// A single frame of movement is a stored value being restored; a slide over several is a spring
// easing toward a target; no movement at all means the snap is not here and the next place to look
// is what happens BEFORE we stop, not after.
void ReleaseTrace(const float **trace, int *count, float *written);

// Why the hold is parked open rather than expiring, or nullptr when it isn't. Distinguishes the two
// reasons a camera can sit still forever, which look identical from the outside and are not.
const char *CameraParkedReason();

// How many ticks actually pushed a new yaw into the game, and what the last one was.
void CameraWriteStats(u64 *writes, u64 *fails, float *lastWritten);

}  // namespace VCS
