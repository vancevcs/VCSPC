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

#include <optional>
#include <string>
#include <vector>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/Input/InputState.h"
#include "Common/Input/KeyCodes.h"
#include "Core/VCS/VCSState.h"

// Context-aware keyboard mapping for VCS.
//
// The idea is that the same physical key means different things depending on what the player is
// doing - Space is Jump on foot and Handbrake in a car, F is Enter Vehicle on foot and Exit
// Vehicle while driving. That's the bit a normal static pad mapping can't express, and it's why
// this lives here rather than in PPSSPP's ControlMapper.
//
// Status: the table, the resolution logic and the host key path are all live. The layer still
// does nothing until the address table can tell on-foot from in-vehicle, because until then
// ResolveContext returns Unknown and Unknown means full passthrough.
//
// Threading: unlike the rest of Core/VCS, this is touched from TWO threads. HandleHostKey and
// SetHostKeyDown run on whichever thread delivers input - on Windows that's the message pump,
// a genuinely different OS thread from the one running the CPU (see AGENTS.md). Everything
// else runs on the emu thread. The held-key set is therefore mutex-guarded and the current
// context is atomic. sceCtrl does the same thing with its own ctrlMutex, for the same reason.

namespace VCS {

enum class VCSInputContext {
	// We can't tell yet - either the compat flag is off, no game is running, or (the common
	// case right now) the address table doesn't have enough filled in to distinguish contexts.
	// Always maps to full passthrough.
	Unknown,

	OnFoot,
	InVehicle,

	// Helicopters and planes. Split from InVehicle because VCS genuinely controls them
	// differently - L/R yaw instead of glance/handbrake, and the nub is pitch and roll rather
	// than steering - and because the car bindings cannot fly at all: they drive the stick's X
	// axis only, and pitch is what makes an aircraft move forward. Entered on the vehicle's
	// model id, see VehicleClassForModel.
	InAircraft,

	// Weapon aiming. Entered by HOLDING the aim key, not by the game's own aim flag - see
	// ResolveContext for why. This context is about BINDINGS (Q/E cycle targets rather than
	// weapons, the aim trigger stays held); it does NOT by itself mean the mouse becomes a
	// reticle. Whether it does is a separate question with a separate answer - see
	// ReticleActive, and "Aiming is a stick, not a camera" in CLAUDE.md.
	Aiming,

	Menu,
};

const char *VCSInputContextName(VCSInputContext context);

// The host key that means "aim". Named because three separate places need to agree on it: the
// mapping rows that hold the PSP aim button down, and ResolveContext, which enters the Aiming
// context while it is held.
extern const InputKeyCode kVCSAimKey;

// The host key that means "jump", named for the same reason: the mapping row that presses the
// PSP's jump button and VCSVault's trigger have to agree on it. A vault happens INSTEAD of a jump
// when there is a ledge in front of the player, so the day these two disagree is the day the game
// gets a jump and a climb at once.
extern const InputKeyCode kVCSJumpKey;

// The pad's aim and jump controls, named for exactly the reason the two above are: more than one
// place has to agree about them, and a pad row is now the other half of every agreement the
// keyboard rows already had. The jump one is the concrete lesson - vaulting asked whether the
// JUMP KEY was down, which a pad has no way to answer, so a pad could jump and never climb.
extern const InputKeyCode kVCSPadAimButton;
extern const InputKeyCode kVCSPadJumpButton;

// Recruit a gang member, on both devices, and named for exactly the reason the four above are:
// the mapping rows press it and the auto-free-aim gate has to ask whether it is held, so the two
// must not be able to drift. See RecruitHeld.
extern const InputKeyCode kVCSRecruitKey;
extern const InputKeyCode kVCSPadRecruitButton;

// Which page of the read-only controls listing a row appears on, if any.
//
// Deliberately not the same thing as VCSInputContext, and the Aiming context is why: its rows
// split between two pages. Holding aim on foot is an on-foot action and belongs with the rest of
// them, exactly as the game's own Controls screen puts Aim Weapon and the zoom keys on its On
// Foot page - while the face buttons that only mean anything mid-fight get their own.
// Flags rather than a plain enum, because a few rows genuinely belong on two pages: cycling
// locked-on targets with Q and E is something you do on foot and something you do mid-fight, and
// listing it on only one of them would leave the other page lying by omission.
enum class VCSKeyList {
	None = 0,  // not shown: suppression claims, the spawner's debug rows, the dead Menu context
	OnFoot = 1 << 0,
	InVehicle = 1 << 1,
	Aircraft = 1 << 2,
	Melee = 1 << 3,
};
ENUM_CLASS_BITOPS(VCSKeyList);

// A column of the controls listing. The card shows all three of them at once, side by side.
//
// Two tables, three columns. The keyboard's rows come from kVCSKeyMappings and both pad columns
// from kVCSPadMappings, so every column is built from the table that actually drives that device
// and cannot drift from what pressing the thing does - the only property here worth protecting.
// The two pad columns are one table named twice: a DualSense and an Xbox pad differ in what is
// printed on the plastic and in nothing else, so a second pad table would be a second copy of
// the same scheme waiting to disagree with the first.
//
// Side by side rather than behind a switch, and the difference is not cosmetic. A switch answers
// "what would I press on the device I am not holding" one device at a time; four columns answer
// "what do I press" for whoever is reading, and they show the one thing a switch structurally
// cannot - which actions exist on one device and not on another. WASD against a blank pad cell,
// or the lock-on toggle against two blank ones, says more than either card said alone.
enum class VCSListDevice {
	Keyboard,
	Xbox,
	PlayStation,
};
constexpr size_t kVCSListDeviceCount = 3;

// One row of the mapping table: in this context, this host key produces these PSP button bits.
// psp is a mask of the CTRL_* defines from Core/HLE/sceCtrl.h, so a single key can produce a
// combination if we ever need that.
struct VCSKeyMapping {
	VCSInputContext context;
	InputKeyCode key;
	u32 psp;

	// Engineering note, for the debugger's mapping table. Says why the row exists, and is free to
	// say things like "Suppressed (blocks PPSSPP rapid-fire)" that no player should ever read.
	const char *description;

	// Player-facing name, for the controls listing, and the page it appears on. Both left unset
	// on a row that should not be listed - which is most of the interesting ones, since a
	// suppression claim does nothing and a debug spawner is not a control. Rows sharing a
	// listName merge into one line with several keys against it, which is how the listing shows
	// that Fire is both the left mouse button and Backspace.
	const char *listName;
	VCSKeyList list;
};

// The mapping table. Rows are matched in order; the first row whose context and key both match
// wins. A row with context Unknown would apply everywhere, which we deliberately never use.
extern const VCSKeyMapping kVCSKeyMappings[];
extern const size_t kVCSKeyMappingCount;

// --- The gamepad scheme -----------------------------------------------------------------------
//
// A second scheme beside the keyboard one, laid out the way a modern console game is rather than
// the way the PSP was: the triggers aim and fire, the right stick looks, the bumpers glance and
// zoom. It is not a re-labelling of the PSP's buttons - it moves them, which is the whole point,
// and that is why a pad has to come THROUGH this layer instead of past it as it used to.
//
// The face buttons are the one part that did not move. Xbox A/B/X/Y sit where the PSP's
// Cross/Circle/Square/Triangle do, and VCS already puts the right actions on them, so on foot and
// in a fight the modern layout and the handheld's agree by position. Everything else differs.
//
// Everything the pad sends must be CLAIMED, mapped or not - the inverse of the Escape trap the
// keyboard table documents. PPSSPP's XInput defaults put the pause menu on the left trigger and
// fast-forward on the right one, so a trigger this table failed to claim would not fall through
// harmlessly, it would open a menu mid-firefight.

// Tunables for the pad. Separate from VCSCameraSettings because these describe a stick, not a
// mouse: a stick reports a POSITION that has to be integrated into movement, where a mouse
// reports the movement itself. One sensitivity number cannot mean both.
struct VCSPadSettings {
	// Off hands the pad back to PPSSPP's own mapper, which is the PSP's layout button for button.
	// Worth keeping reachable: this scheme is an opinion, and the handheld's is the other one.
	bool enabled = true;

	// Right stick look, in virtual mouse counts per tick at full deflection.
	//
	// Expressed in MOUSE counts deliberately. The stick's delta is pushed into the same
	// accumulator the mouse fills, so everything downstream - the sensitivity, the FOV scale, the
	// free-aim solver, the vehicle pitch rules - is the code that was measured against a mouse
	// rather than a second copy of it that would drift from the first.
	float lookSpeed = 14.0f;

	bool invertLookY = false;

	// Below this the stick reads as centred. Sticks rest off-centre, and a resting stick that
	// still turns the camera is the most obvious way for this to feel broken.
	float deadzone = 0.18f;

	// How far a trigger travels before it counts as a press, and the lower value it has to fall
	// back past to count as a release. Two numbers rather than one because a trigger held near a
	// single line chatters the button it is bound to, and this trigger is the fire button.
	float triggerPress = 0.45f;
	float triggerRelease = 0.35f;
};

VCSPadSettings &PadSettings();

// One row of the gamepad table.
//
// Deliberately not a VCSKeyMapping. A pad row needs a gate the keyboard's does not - see
// scopedOnly - and the two are matched on different things: a keyboard row on a key, a pad row on
// a button that may be a trigger this layer synthesised. Sharing the struct to save a declaration
// would mean one table carrying a column the other must never use.
struct VCSPadMapping {
	VCSInputContext context;

	// The button as PPSSPP delivers it. The two analog triggers arrive as AXES and are turned
	// into NKCODE_BUTTON_L2 / NKCODE_BUTTON_R2 by HandleHostAxis, so this table can stay one flat
	// list of buttons rather than growing an axis half.
	InputKeyCode button;

	u32 psp;

	// Only sent while a scoped weapon, the binoculars or the camera are up. This is for the zoom
	// bumpers:
	// zoom is the game's Square and Cross, which with anything else in hand are Block and Heavy
	// Hit, so an ungated row would have a bumper throwing punches every time it was pressed
	// unscoped.
	bool scopedOnly;

	// Engineering note, for the debugger's mapping table - same job as VCSKeyMapping's.
	const char *description;

	// Player-facing name and page for the controls card, on the same terms as the keyboard's: no
	// listName means the row is real but not shown, which is what a claimed-and-inert row wants.
	const char *listName;
	VCSKeyList list;
};

extern const VCSPadMapping kVCSPadMappings[];
extern const size_t kVCSPadMappingCount;

// The gamepad entry point for axes, called from NativeAxis alongside HandleHostKey's call in
// NativeKey. Records the two sticks and turns the analog triggers into button presses.
//
// Returns true when this layer has taken the axis, in which case PPSSPP's own mapper must NOT
// also see it - it drives the same PSP stick we do, and both writing it means neither wins.
// False for every other game, for a pad with the scheme switched off, and for an axis no part of
// this scheme uses.
bool HandleHostAxis(const AxisInput &axis);

// Turns the right stick into look movement for this tick, by pushing a delta into the same
// accumulator the mouse fills.
//
// Emu thread, once per frame, and it MUST run before ApplyAnalog: in free aim that accumulator is
// drained to place the crosshair, so a delta arriving after the drain is a frame late every
// frame, which reads as a stick that lags rather than one that does nothing.
void ApplyPadLook(VCSInputContext context);

// Whether a gamepad button is currently held.
//
// The pad's held set is separate from the keyboard's, and has to be: an arrow key and a d-pad
// direction are the SAME InputKeyCode, so one set would have the spawner's arrow-key rows firing
// whenever a player pressed a direction on a pad.
bool IsPadButtonDown(InputKeyCode button);

// A pad button as the controls card prints it - "A", "LB", "LT", "D-PAD UP", "VIEW" on an Xbox
// pad, "CROSS", "L1", "L2", "D-PAD UP", "CREATE" on a PlayStation one. The scheme is the same
// either way; only the labels differ, which is the whole reason one table feeds both columns.
//
// Passing Keyboard is a caller error and answers with the Xbox name, which is the least
// surprising thing a mislabelled cell can say.
std::string PadButtonName(InputKeyCode button, VCSListDevice device);

// One line of the controls listing: an action, and the controls that perform it on each device,
// in table order.
struct VCSListingRow {
	const char *name;
	// One list per column, indexed by VCSListDevice. Empty means this device cannot do the
	// action at all, and the cell is left blank rather than filled with a dash - on a card that
	// shows every device at once the blank IS the information.
	std::vector<std::string> controls[kVCSListDeviceCount];
};

// The listing for one page, all three columns at once. Built by grouping kVCSKeyMappings and
// kVCSPadMappings on listName, after the handful of rows that cannot be in either table - see
// kVCSListingExtras.
//
// A row is dropped only when it has nothing to show on ANY device, which is the four-column
// version of the old rule rather than a relaxation of it: a blank line in one column still says
// "not on this device", and it can only say that next to a column where the action exists.
std::vector<VCSListingRow> KeyListing(VCSKeyList list);

// A key as the listing prints it: short and upper case, the way the game's own Controls screen
// sets them. PPSSPP's GetKeyName is the fallback; the overrides exist because "MB1" and
// "MWheelD" are not what a player calls those.
std::string KeyDisplayName(InputKeyCode key);


// Decides which context the player is in, from the decoded state. Returns Unknown whenever the
// state doesn't give us enough to be sure, which is the safe answer - callers treat Unknown as
// "don't touch the controls".
VCSInputContext ResolveContext(const VCSState &state);

// The host input entry point, called from NativeKey. Records the key state and reports whether
// VCS claims this key, meaning PPSSPP's own control mapper must NOT also act on it - otherwise
// a key bound in both places would fire twice.
//
// Returns false, leaving the caller's behaviour completely unchanged, whenever:
//   - VCS isn't active (every other game, and VCS with the compat flag off),
//   - the input didn't come from a keyboard or mouse,
//   - the context is Unknown, which is the case until the address table can resolve one,
//   - or this key simply isn't mapped in the current context.
//
// Safe to call from the input thread.
bool HandleHostKey(const KeyInput &key);

// Lower-level host key state, used by HandleHostKey and by the debugger window's key simulator.
// Keys not in the mapping table for the current context are recorded but have no effect.
void SetHostKeyDown(InputKeyCode key, bool down);

// Mouse wheel notches since the last call, and zeroed by it.
//
// COUNTED rather than polled, which the rest of this layer does not need to do. A wheel notch is
// a key down and a key up in the same instant - by the time the emu tick asks whether the key is
// held, it is not - so the only way to see one is to record it as it arrives. Filled on the input
// thread, drained on the emu thread; atomics, for the reason the held-key set has a mutex.
void TakeWheelNotches(int *up, int *down);

// Every notch ever seen, never consumed. Purely for the debugger: it separates "the wheel does not
// reach this layer" from "it does and the button is wrong".
int WheelNotchesSeen();

// Clears all held keys. Called on init/shutdown, and worth calling on focus loss later so keys
// don't stick.
void ResetHostKeys();

bool IsHostKeyDown(InputKeyCode key);

// How many host keys are currently held. Purely for the debugger - an unambiguous signal that
// real input is reaching this layer, independent of whether anything is mapped.
size_t HeldHostKeyCount();

// Movement keys. These deliberately aren't in the mapping table above: they drive the analog
// stick, not buttons, and what they mean depends on the context in a way a key->button table
// can't express.
//
//   OnFoot    : WASD is full 2D movement on the left stick.
//   InVehicle : A/D steer on the stick's X axis, while W/S stay BUTTONS (accelerate and brake
//               are Cross and Square, see the table) - so the same physical keys split across
//               two different input mechanisms depending on context. This is the case PPSSPP's
//               static mapper fundamentally cannot express, and the main reason this layer
//               exists at all.
//   Aiming    : depends on whether the game is actually FREE-aiming, see ReticleActive. In free
//               aim the stick is the RETICLE, driven by the mouse, and WASD does nothing - the
//               PSP has exactly one analog axis, so it cannot both walk the player and place the
//               crosshair, and the game gives it to the crosshair. Under lock-on (or with the
//               aim key held and nothing happening) the stick is movement again, exactly as
//               OnFoot, because that is what the game does with it there.
//   Menu      : nothing; WASD is mapped to the d-pad as buttons instead.
//
// Applies the analog stick for this frame. Only touches the stick while a movement key is
// actually held (or, in free aim, while the mouse is actually moving), and releases it exactly
// once when that stops, so a real pad still works when the keyboard isn't being used.
void ApplyAnalog(VCSInputContext context);

// Why the aim path is doing what it is doing, in one short phrase, for the debugger.
//
// FreeAimActive and ContextDrivesCamera are several terms between them, and ANY one of them turns
// the whole aim path off - PedAimTick stops pointing the gun, the nub goes back to WASD, and the
// mouse is handed to the camera. In play that reads as the gun and body detaching from the
// crosshair, with nothing on screen to say which term did it.
//
// Two of the answers are healthy ("camera aim", "reticle"); the rest each name the specific gate
// that is suppressing it. Reading one line beats reproducing an intermittent bug repeatedly and
// guessing between four candidates, which is exactly what this was written after.
const char *AimPathStatus(VCSInputContext context);

// Whether the analog stick is currently the aiming RETICLE rather than movement.
//
// True only in the Aiming context AND while the game reports free aim. Both halves are needed
// and neither is sufficient:
//
//   - The aim key alone is not enough. VCS has no free-aim mode for ordinary weapons; the aim
//     trigger gives you lock-on, where the game aims for you and the stick STRAFES around the
//     target. Driving the stick from the mouse there just walks the player sideways. This was
//     built that way once and that is exactly what it did.
//   - The flag alone is not enough either. IsFreeAiming also reads 1 during cutscenes, so it
//     must stay paired with the player actually holding aim.
//
// In practice this means free aim in VCS is the sniper rifle and the RPG, and those are the only
// places a mouse can really aim. Emu thread only - it reads the decoded state.
//
// Aim held and the game is not steering it - i.e. free aim, as opposed to lock-on. The state,
// independent of which mechanism is currently driving it.
bool FreeAimActive(VCSInputContext context);

bool ReticleActive(VCSInputContext context);

// Whether the player is riding shotgun with a weapon out - the passenger drive-by, which is a
// third aim mechanism beside lock-on and free aim, and behaves like neither.
//
// What makes it its own case is that NOTHING the layer already asked about is true during it. The
// player holds no aim control, because the game gives the mode for the whole ride rather than on
// request; the context is InVehicle, not Aiming; and both IsAiming and IsFreeAiming read 0
// throughout. So every existing gate concluded "not aiming", the mouse went to the camera as it
// does while driving, and the nub - which is what this mode aims with - was left to A and D.
//
// Recognised from the camera instead: the weapon camera and the active camera both in mode 11.
// See the definition for why it takes both, and VCSAddresses.h for the `03E9 2.5 0.5` that set
// this up in the retail script.
//
// Emu thread only - it reads game memory.
bool DriveByAimActive(VCSInputContext context);

// Whether the player is in the vehicle whose mounted cannon the stick aims - the fire truck.
//
// A second in-vehicle stick-aim state, and it shares nothing with the drive-by: no weapon camera,
// no axis scale, no aim flag that means anything. Only the vehicle model identifies it, which is
// the point rather than a shortcut - see cannonMouseAim for the flag that looked like a spray
// signal and measured out as a 15-second timer.
//
// This gates the stick's Y axis ONLY. X stays steering in every vehicle, always.
//
// Emu thread only - it reads the decoded state.
bool CannonAimActive(VCSInputContext context);

// Whether the stick applied on the most recent tick came from the mouse (the reticle) rather
// than from WASD. Purely for the debugger, which otherwise can't tell the two apart.
bool AnalogIsReticle();

// Cumulative counters for the reticle path, because the live stick position above is zero
// whenever the mouse is still and so proves nothing in a screenshot. frames counts ticks where
// the reticle ran; nonZero counts ticks where it actually had mouse movement to apply.
void ReticleStats(u64 *frames, u64 *nonZero);

// Vehicle glance keys, using the game's own look mechanic: L trigger plus a stick direction,
// exactly as the PSP does it. Q is stick left, E is stick right, both together is stick up.
//
// Going through the real mechanic rather than writing the camera angle is what makes drive-by
// shooting work - hold a glance and press Circle and the game fires to that side, because those
// are simply the inputs it expects. The stick half is applied by ApplyAnalog, which lets a
// glance override steering; this returns the L trigger half so it joins the normal button mask.
// Returns 0 outside a vehicle or with no glance key held.
u32 GlanceButtonMask(VCSInputContext context);

// The stick position applied on the most recent tick, for the debugger window.
void GetAppliedAnalog(float *x, float *y);

// Computes the PSP button mask implied by the currently held host keys in the given context.
// Pure function of the key state and the table - no side effects, which makes it easy to show
// in the debugger without actually applying it.
u32 ComputeButtonMask(VCSInputContext context);

// Applies the mapping for this frame by pushing buttons into sceCtrl. Does nothing at all when
// the context is Unknown, so an empty address table means zero interference.
//
// Returns the mask it applied, for display purposes.
u32 ApplyMapping(VCSInputContext context);

// Force PSP buttons down regardless of any mapping, for the debugger's button tester.
//
// This exists because guessing which PSP button does what in VCS has been the single biggest
// source of wrong bindings - aim sat on L trigger doing nothing for a long time. Holding each
// button directly and watching the game answers it in seconds. Mask of CTRL_* bits; 0 clears.
void SetForcedButtons(u32 mask);

// The context resolved on the most recent tick, for the debugger window.
VCSInputContext GetCurrentContext();

// Whether aiming is pinned to the game's own lock-on. While it is, there is no Free Aim press,
// the mouse does not take the analog stick, and it does not turn the camera either - so movement
// still moves, the game frames the target, and looking around does nothing until aim is released.
//
// Two sources, meaning the same thing downstream:
//
//   - the keyboard's manual toggle, which exists for melee, whose lock-on does not register in
//     IsAiming;
//   - the pad, which aims this way ALWAYS - holding its trigger is a request for the game's
//     assist rather than for a crosshair, which is what a stick is good at and what every
//     console shooter does.
//
// A scoped weapon is the exception on the pad's side; see the definition for why that is the
// phrase's meaning rather than a hole in it.
bool LockOnModeActive();

// Whether the player is asking to jump, on EITHER device.
//
// Vaulting is why this is a function rather than a key: a vault happens INSTEAD of a jump when
// there is a ledge in front of the player, so whatever presses the PSP's jump button and whatever
// arms the climb have to be the same question. Asking it of one device is how a pad ended up able
// to jump but never to climb.
//
// Context is the caller's business, not this function's - Space is the handbrake in a car and X is
// too, and it is VaultTick's on-foot check that keeps a climb out of both.
bool JumpHeld();

// Whether the player is asking to recruit a gang member, on EITHER device.
//
// A named intent rather than a key for the reason JumpHeld is one, and it earns it twice over:
// the recruit control is read by the mapping tables AND by the auto-free-aim gate, which has to
// stand down while it is held.
//
// Recruiting needs the game to have TARGETED the henchman - its own help line is "target them and
// use the up button" - and free aim is precisely the state with no target. So on a mouse, where
// aiming drops straight into free aim, holding the recruit key before the aim control is what
// buys the lock-on that makes the press mean anything. A pad already aims by lock-on always and
// needs none of this.
bool RecruitHeld();

// Whether the player is aiming in a mode THIS LAYER steers, as opposed to one the game steers.
//
// The fire hook's guard, and the distinction matters more than "is aim held": the hook redirects
// the shot along the camera ray, which is correct when the camera is the aim and actively wrong
// under lock-on, where the game has chosen a target the camera need not be pointing at.
//
// For the mouse the two questions have always had the same answer, because the mouse always free
// aims - so nothing about that path changes. The pad is where they come apart: it aims by lock-on
// except with a scope, and a scope is precisely the case where the camera IS the aim.
bool CameraDrivenAimHeld();

// Whether any of WASD is held. The free-aim brake keys on this: while a movement key is down the
// latched run is left to carry the player, and releasing them all is what applies the brake.
bool MovementKeysHeld();

// Whether a scoped weapon (sniper, RPG - weapon camera modes 7 and 8) or one of the items you
// look through (the binoculars, the photo camera) is equipped. Those aim by moving the camera,
// because down a scope the camera direction is the firing direction - and the two items are the
// same camera with nothing to fire, or nothing but a shutter. See the definition for why the
// items are matched on the weapon id instead.
bool ScopedWeaponActive();

}  // namespace VCS
