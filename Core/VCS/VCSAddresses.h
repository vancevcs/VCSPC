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

#include "Common/Common.h"
#include "Common/CommonTypes.h"

// THE address table for GTA: Vice City Stories (ULUS10160).
//
// This is the only place in the codebase where a VCS memory address may appear. Everything
// else refers to entries by their VCSAddr enum value and goes through VCSMemory. If you find
// yourself writing a hex literal anywhere else in Core/VCS or UI/ImDebugger/ImVCS.cpp, add an
// entry here instead.
//
// Every address is currently kUnsetAddress. That is a supported, fully working state: reads of
// unset entries return std::nullopt without touching PSP memory at all, the debug overlay shows
// "unset", and the input layer stays in passthrough. Fill entries in as you find them with the
// VCS debugger window (Debug -> VCS), one at a time.
//
// See docs/VCS_ADDRESSES.md for what each of these represents and how to hunt for it.

namespace VCS {

// Sentinel for "I haven't found this one yet". Address 0 is never a valid PSP user address,
// so this is unambiguous - see Memory::IsValidAddress.
inline constexpr u32 kUnsetAddress = 0;

// A CODE address, not a data one, which is why it sits outside the table below - that table
// describes values to read, and this is a place to hook.
//
// The `jal CWorld::ProcessLineOfSight` inside the weapon's raycast wrapper (0x08a41d28). Every
// weapon raycast funnels through it, and a0/a1 hold point1/point2 there, untouched by the
// wrapper. Overwriting point2 immediately before this call redirects the shot - measured in
// play, not inferred. See "The path a shot actually takes" in docs/VCS_ADDRESSES.md.
inline constexpr u32 kVCSWeaponRaycastCall = 0x08A41D74;

// The instruction we expect to find there: `jal 0x0889786c`. Checked before patching, because
// installing now happens after module load rather than during it - so if the code is not there
// yet, or this is not the build the address was measured on, we must write nothing at all.
inline constexpr u32 kVCSWeaponRaycastOp = 0x0E225E1B;

// CWorld's ground probe, and the reason vaulting can ask about geometry at all.
//
//     float CWorld::FindGroundZFor3DCoord(float x, float y, float z, bool *found)
//
// x/y/z arrive in $f12/$f13/$f14 and the out-parameter in $a0; the height of the first surface
// BELOW the point comes back in $f0. Internally it drops a vertical line from z to -1000 and calls
// CWorld::ProcessVerticalLine, which is why it answers "what is under this point" rather than
// "what is at it".
//
// Found from the game's own script table rather than by correlation, the same route that produced
// get_current_char_weapon: opcode 01BB (get_ground_z_for_3d_coord) resolves through the dispatch
// table at 0x08b846e0 to a handler at 0x08a9e434, which collects three floats and calls this with
// them. No searching, no guessing - the game says which function the command is.
inline constexpr u32 kVCSFindGroundZFor3DCoord = 0x08893460;

// The SECOND and THIRD instructions, `swc1 $f12, 0x10($sp)` and `swc1 $f13, 0x14($sp)`. Checked
// before the world query installs itself, for the reason kVCSWeaponRaycastOp exists: on another
// build, or before the module has loaded, this is not that function and the program must not be
// written.
//
// Deliberately not the FIRST instruction, which is `addiu $sp, $sp, -0x50` and would be the
// obvious thing to check. This address is a function ENTRY, so it is where a JIT block starts, and
// PPSSPP overwrites the first instruction of every compiled block with a 0x68xxxxxx marker - so
// once the game has called this function even once, the live word is not the game's code and the
// check can never pass again. (The fire hook gets away with checking its own site because that one
// is a `jal` in the middle of a function, which no block starts at.) Nothing branches into the
// middle of this prologue, so the two words below are never block starts and can be read directly,
// with no icache invalidation and no recompile.
inline constexpr u32 kVCSFindGroundZOp2 = 0xE7AC0010;
inline constexpr u32 kVCSFindGroundZOp3 = 0xE7AD0014;

// THE GAME'S OWN CLIMB, in two calls. Found by breaking on the ped state word as a real
// swimming climb-out began and walking the backtrace out - see "The climb-out is two calls" in
// docs/VCS_ADDRESSES.md.
//
//     bool CPed::CanClimb(CPed *ped, ClimbResult *out)      0x0892fca4
//     void CPed::StartClimb(CPed *ped, ClimbResult *result)  0x08912be0
//
// The first is the game's own ledge search: it fills a small struct whose first byte is "found",
// with the entity at +0x08 and the world target at +0x10. The second consumes that struct - it
// clears three flag bits, sets the ped state to 44, and hands the entity and target to
// 0x0890f6ec, which takes a reference on the entity and stores the target relative to it.
//
// Why this matters more than any address here: it is the ANIMATION. Writing the state by hand
// engages the climb and then aborts, because the ped has nothing to climb and nowhere to climb to;
// these two calls are how the game supplies both.
inline constexpr u32 kVCSPedCanClimb = 0x0892FCA4;
inline constexpr u32 kVCSPedStartClimb = 0x08912BE0;

// Second instructions, checked before the climb program is written for the same reason
// kVCSFindGroundZOp2 exists - never the first, which is where a JIT block marker lands. StartClimb's
// is `sw $s1, 4($sp)`; CanClimb's is `swc1 $f20, 0x394($sp)`, off a 0x3c0-byte frame, which is a
// function that clearly does a great deal of geometry on our behalf.
inline constexpr u32 kVCSPedStartClimbOp2 = 0xAFB10004;
inline constexpr u32 kVCSPedCanClimbOp2 = 0xE7B40394;

// The pieces StartClimb uses, recorded because they are the fallback if the search ever refuses a
// ledge our own probe is happy with: the state setter, and the call that stores what is being
// climbed (entity at ped+0x1c0) and where to (target at ped+0x1b0, relative to that entity).
inline constexpr u32 kVCSPedSetState = 0x08908D60;
inline constexpr u32 kVCSPedSetClimbTarget = 0x0890F6EC;
inline constexpr u32 kVCSPedStateOffset = 0x8B4;   // 44 while climbing out, 1 standing
inline constexpr u32 kVCSPedClimbStageOffset = 0x1D9;   // 0, then 1..4 through the pull-up

// The general form underneath it: ProcessVerticalLine(point1, z2, colPoint, entity, checkBuildings,
// checkVehicles, checkPeds, checkObjects, checkDummies, ignoreSeeThrough, poly) - point in $a0, the
// floor height in $f12, and the bools filling $a3 and $t0-$t3. Not called yet. It is the way to
// learn WHAT was hit rather than only how high it is, which is what a vault would need before it
// could refuse to climb a moving vehicle.
inline constexpr u32 kVCSProcessVerticalLine = 0x08891DD4;

// CCam m_asCams[0] - CCamera (0x08bc7e30) + 0x70.
inline constexpr u32 kVCSCam0 = 0x08BC7EA0;

// CCam's stored view BASIS, and the answer to where a shot should actually be aimed.
//
// MEASURED out of a gameplay savestate, geometrically, not correlated and not guessed:
//
//   +0x010  Front   unit, and exactly normalize(LookAt - Source) to 5 decimal places
//   +0x020  Source  the camera's world position: 4.63 m from the player, on the far side
//   +0x060  Up      unit, and dot(Front, Up) == 0.000000 - a real orthonormal pair
//   +0x128  FOV     70.0, degrees, VERTICAL (re3 multiplies the x term by the aspect ratio)
//   +0x190  the point the camera looks at: the player's position raised by 0.6
//
// The old note here said these "did not reconcile with CameraYaw" and could not be used. The
// observation was right and the conclusion was wrong. They do not reconcile because Beta/Alpha
// (CameraYaw/CameraPitch) are the ORBIT angles about the look-at target, while Front points from
// the source AT that target - and the player does not stand in the middle of the screen. In the
// savestate the two differ by 4.42 degrees, split roughly 3.3 horizontal and 2.8 vertical.
//
// That 4.42 degrees is the entire "shots do not land on the crosshair" bug. It is not a constant:
// it is the angular size of the player's offset from screen centre, so it scales with 1/distance
// and swings as the camera orbits. `crosshairX`'s 3.2 degree in-play trim was fitting the
// horizontal half of it, which is why the fitted value kept wandering with the view angle.
inline constexpr u32 kVCSCamFrontOffset = 0x010;
inline constexpr u32 kVCSCamSourceOffset = 0x020;
inline constexpr u32 kVCSCamUpOffset = 0x060;
inline constexpr u32 kVCSCamFOVOffset = 0x128;

// The weapon's range, which is what the length of a redirected shot ray has to be.
//
// CWeaponInfo::GetWeaponInfo is 0x08b1fd70, and it is three instructions:
//
//     offset = type * 128 - type * 16          // i.e. stride 0x70
//     base   = gp[-0x1ba8] ? tbl[0] : tbl[1]   // tbl = *(void**)(gp + 0x2950)
//     return base + offset
//
// Range is at +0x08 - FireInstantHit builds its fallback target as `source + range * heading`
// straight out of that field. Read back sensibly across the whole arsenal in a savestate:
// pistol 30, python 40, shotgun 15, SMG 45, AR 90, M60 100, sniper 55, RPG 75.
//
// Worth the plumbing because the alternative is what shipped: reusing whatever length the game's
// own target happened to have. That is the weapon range only on the plain path - it is the
// distance to the locked-on entity on another, and the distance to the free-aim dummy on a third.
// A ray of the wrong length stops short of what the crosshair is on, by a different amount every
// shot.
inline constexpr u32 kVCSWeaponInfoTablePtr = 0x08BB46B0;     // gp + 0x2950
inline constexpr u32 kVCSWeaponInfoTableSelect = 0x08BB01B8;  // gp - 0x1ba8
inline constexpr u32 kVCSWeaponInfoStride = 0x70;
inline constexpr u32 kVCSWeaponInfoRangeOffset = 0x08;

// The player's weapon records, indexed by the slot in WeaponIndex. Already documented on that
// entry; named here so the fire hook can reach the TYPE without a hex literal of its own.
inline constexpr u32 kVCSWeaponRecordsOffset = 0x574;  // from PlayerBase
inline constexpr u32 kVCSWeaponRecordStride = 28;
inline constexpr u32 kVCSWeaponRecordTypeOffset = 0x04;

// CEntity's own layout, which every entity in the game shares. The transform sits at the head with
// the translation at +0x30 - that relation is how PlayerBase, PlayerVehicle and the whole vehicle
// pool were all confirmed. The flags word at +0x48 carries a 3-bit type in bits 1..3, so the type
// is `(flags & 0xe) >> 1`; the game tests the masked value against 6 for a ped (0x08a48934) and
// against 0xe for the case FireInstantHit treats specially (0x08a489cc).
//
// The numbering matches re3's eEntityType, anchored by that ped test: 6 >> 1 == 3 == ENTITY_TYPE_PED.
// So 1 building, 2 vehicle, 3 ped, 4 object, 5 dummy.
inline constexpr u32 kVCSEntityPositionOffset = 0x30;
inline constexpr u32 kVCSEntityFlagsOffset = 0x48;
inline constexpr u32 kVCSEntityTypeMask = 0x0e;
inline constexpr u32 kVCSEntityTypeShift = 1;
inline constexpr int kVCSEntityTypeBuilding = 1;
inline constexpr int kVCSEntityTypeVehicle = 2;
inline constexpr int kVCSEntityTypePed = 3;
inline constexpr int kVCSEntityTypeObject = 4;
inline constexpr int kVCSEntityTypeDummy = 5;

// How to interpret the bytes at an address. Used both by the typed read helpers and by the
// debugger window to decide how to display a value.
enum class VCSAddrType {
	U8,
	U16,
	U32,
	S32,
	Float,
};

// One entry per thing we need to read out of the game. Keep in sync with kVCSAddresses below;
// there is a static_assert at the bottom of this file that catches the easy mistake.
enum class VCSAddr {
	// --- Player / on-foot state ---
	PlayerBase,        // Pointer to the local player ped struct. Most on-foot state hangs off this.
	PlayerHealth,      // Current health.
	PlayerOnFoot,      // Nonzero when the player is on foot rather than in a vehicle.

	// --- Vehicle state ---
	PlayerVehicle,     // Pointer to the vehicle the player currently occupies, 0 when on foot.

	// The vehicle the player is COMMITTED TO ENTERING, latched while still on foot. See the table.
	PedEnteringVehicle,

	// Which vehicle it is. VCS controls a helicopter completely differently from a car - L/R are
	// yaw, the nub is pitch and roll - so one in-vehicle binding set cannot be right for both,
	// and this is what tells them apart. See VehicleClassForModel in VCSState.
	VehicleModel,

	// --- Weapons ---
	WeaponIndex,       // Currently selected weapon slot/id.
	IsAiming,          // Nonzero while LOCKED ON to a target.
	IsFreeAiming,      // SUSPECT. Tracks sniper aiming, but probably a broader
	                   // 'cinematic camera / control restricted' flag - see the docs.

	// --- Camera ---
	CameraYaw,         // Horizontal camera angle, radians. Needed before task 2 (mouse look).
	CameraPitch,       // Vertical camera angle, radians.

	// --- Global game state ---
	GameState,         // Distinguishes gameplay from menus/loading/cutscenes.

	// --- The pad's "second stick" ---
	//
	// VCS internally wants two analog sticks and the PSP only has one, so the game synthesises the
	// second one from the d-pad. These four are int16 fields inside CPad, holding 0 or 255 from the
	// digital buttons - but the game reads them as an ANALOG pair, so writing intermediate values
	// gives smooth camera and aim input through the game's own code path. See CLAUDE.md.
	PadDPadUp,
	PadDPadDown,
	PadDPadLeft,
	PadDPadRight,

	// Selects where the camera/aim axis comes from. 0 (the shipped value) reads the nub; nonzero
	// reads the d-pad pair above. Writing the d-pad fields does nothing at all while this is 0.
	// This is the game's own PLAYER MOVEMENT option (Controls menu: ANALOG STICK vs DIRECTIONAL
	// BUTTONS), not a camera setting - the name is a leftover from before that was known.
	CameraInputMode,

	// Where the player is actually pointing the gun in free aim, as opposed to where the camera
	// is looking. Proven to be a separate value: writing CameraYaw/CameraPitch swings the view
	// while the character keeps aiming at his original target. Finding these is what would let
	// the mouse aim directly instead of nudging the nub, which is the only thing still making
	// free aim feel like a thumbstick.
	AimYaw,
	AimPitch,

	// The game's own frame delta, in its internal time units. Every angular rate the camera code
	// computes is multiplied by this, so the aim response model needs it to work out what a given
	// stick deflection will actually do this frame. See VCSCamera's aim model section.
	TimeStep,

	// Incremented once per game LOGIC frame, which is not the same thing as once per tick: we run
	// from the vblank hook at ~60Hz and this game runs its logic at 30fps. Without it the aim
	// model steps twice per game frame and spends mouse movement the game never reads.
	FrameCounter,

	// --- What the aim response model reads instead of predicting ---
	//
	// The model used to carry its own copy of the game's smoothed increment and its own idea of
	// the constants. Every small error in either accumulated into that copy, and an over-estimate
	// makes the anti-glide correction push too hard - which is the aim visibly snapping backwards
	// at the end of a movement. All of this is stored, so none of it needs predicting.
	CamMode,           // CCam[0]+0x00. Selects WHICH response the game applies - see VCSCamera.
	CamAimIncX,        // CCam[0]+0x130. The smoothed per-frame increment added to CameraYaw.
	CamAimIncY,        // CCam[0]+0x124. Ditto for CameraPitch.
	CamFOV,            // CCam[0]+0x128. Every aim rate is scaled by FOV/80.
	// The player's movement velocity, per frame, in world units. Found by diffing the ped struct
	// between walking and free-aiming with the stick deflected in both: these die in free aim and
	// nothing upstream of them does. Peak ~0.10 per frame, which matches the measured walking
	// speed. Based on PlayerBase, because the ped is heap-allocated and moves between runs.
	PedVelX,
	PedVelY,

	// Which way the player is FACING, which in free aim is which way the gun points.
	//
	// The ped's whole upper body - and therefore the weapon - is carried by its heading. Free aim
	// normally turns it because the stick turns it; once the mouse drives the camera directly the
	// stick is no longer fed, nothing re-evaluates the heading, and the character stands frozen
	// aiming wherever he was pointing when aim was pressed. Writing this is what makes the gun
	// follow the crosshair instead of the crosshair sliding off the gun.
	PedHeading,
	PedHeadingTarget,

	// The entity the ped is pointing its gun at - re3's m_pPointGunAt. Its world position is what
	// the arm IK aims along, which is why the gun reads as world-locked once nothing moves it.
	PedPointGunAt,

	// CODE, not data: the branch that makes aiming and moving mutually exclusive.
	//
	// The player control function processes aiming and then branches straight over the call that
	// applies movement. Turning that branch into a nop lets the aim path fall through into the
	// movement path, so both run. This is the only entry in this table that is patched rather than
	// read, and the only one where a wrong address crashes rather than misbehaves.
	MoveGateBranch,

	LookSensitivity,   // The game's own look sensitivity setting. Squared, in the mode 11 response.
	AimAxisScale,      // CPad+0xd0. The game scales the axis by this in weapon camera modes.
	AimAxisScaleY,     // CPad+0xd4. The Y counterpart, written alongside it by opcode 03E9.
	WeaponCamMode,     // CCamera+0x7b8. Decides whether AimAxisScale applies at all.

	Count,
};

// Sentinel for the `base` field: this entry's address is absolute, not relative to another.
inline constexpr VCSAddr kNoBase = VCSAddr::Count;

struct VCSAddrEntry {
	VCSAddr id;
	const char *name;      // Shown in the debugger window.
	VCSAddrType type;

	// When base == kNoBase, this is an absolute address, and kUnsetAddress means "not found".
	// Otherwise it is a byte OFFSET from the pointer stored at `base` - see below.
	u32 address;

	// Most interesting values live inside heap-allocated structs, whose addresses move between
	// sessions. Storing such a value as an absolute address is a bug waiting to happen. Instead
	// point at the global that holds the struct pointer and give the offset within it, e.g.
	// health is PlayerBase + 0x4e4. Only one level of indirection is supported, which is all
	// this game seems to need.
	VCSAddr base;

	const char *note;      // Short hint; the long version lives in docs/VCS_ADDRESSES.md.
};

// Order must match the VCSAddr enum exactly - LookupAddr indexes into this directly.
inline constexpr VCSAddrEntry kVCSAddresses[] = {
	{ VCSAddr::PlayerBase,    "PlayerBase",    VCSAddrType::U32,   0x08bc8170,    kNoBase,              "Player ped. Entity matrix at +0x00, world position at +0x30" },
	{ VCSAddr::PlayerHealth,  "PlayerHealth",  VCSAddrType::Float, 0x4e4,         VCSAddr::PlayerBase,  "Confirmed: HUD bar scales exactly with this (25.0 -> quarter bar)" },
	{ VCSAddr::PlayerOnFoot,  "PlayerOnFoot",  VCSAddrType::U32,   kUnsetAddress, kNoBase,              "Not needed - VCSState derives it from PlayerVehicle == 0" },
	{ VCSAddr::PlayerVehicle, "PlayerVehicle", VCSAddrType::U32,   0x08bb4064,    kNoBase,              "0 on foot, pointer to the occupied vehicle while driving" },
	// The entry-in-progress signal, and the answer to "when does pitch stop belonging to the on-foot
	// camera". Holds the target vehicle's pointer from the moment entry commits - after the door is
	// opened, when walking away can no longer cancel it - until PlayerVehicle catches up.
	//
	// Found by sampling the ped struct 10x/second through a real entry and diffing. Verified
	// separately: 0 while on foot, still 0 with a car spawned right alongside (so it is "entering
	// this vehicle", NOT "a vehicle is near"), set 0.45s after pressing enter while PlayerVehicle was
	// still 0, and equal to PlayerVehicle once seated - about 1.65s of advance warning.
	//
	// Why it matters: the game does not reset CameraPitch when you get in, so whatever the on-foot
	// camera was left at carries into the vehicle context and becomes the anchor, hence the ceiling.
	// This is the window in which to normalise it.
	{ VCSAddr::PedEnteringVehicle, "PedEnteringVehicle", VCSAddrType::U32, 0x480, VCSAddr::PlayerBase, "Target vehicle while entering, before PlayerVehicle is set. 0 otherwise" },
	// Based on PlayerVehicle, so on foot it resolves to 0x56 and reads as unset rather than as
	// garbage - which is exactly the behaviour a based entry is for.
	//
	// Found by entering a car over the WebSocket debugger and scanning its struct for a u16 in
	// the vehicle id range: +0x56 read 209 (patriot). Confirmed by scanning RAM for entities
	// carrying a valid id at that offset, which produced the vehicle pool - ~30 objects on a
	// 0x820 stride, each with a sensible model and a distinct world position. +0x58 holds the
	// same value; which of the two is the "real" one doesn't matter for reading.
	{ VCSAddr::VehicleModel,  "VehicleModel",  VCSAddrType::U16,   0x56,          VCSAddr::PlayerVehicle, "Vehicle model id (213 maverick, 251 forklift...). Decides the control scheme" },
	// This unblocked a real bug, not just the direct-weapon-selection feature. Melee lock-on does not
	// register in IsAiming (measured: the flag reads 0 while visibly locked on), so the aim layer
	// concluded free aim and handed the stick to the mouse - which in lock-on is movement, so the
	// mouse walked the player and WASD went dead. FIXED: FreeAimActive in VCSInput.cpp now stands
	// down when WeaponSlotIsMelee, confirmed in play - the overlay read slot 1 (melee) and WASD
	// worked normally while locked on.
	//
	// Weapon CAMERA mode cannot separate them: melee-locked-on and pistol-aiming both report 11,
	// measured. Only the weapon identity can.
	//
	// FOUND, and from the game's own code rather than by correlation: the script command
	// 02C0 get_current_char_weapon is dispatched through a table at 0x08b846e0 (8 bytes per
	// opcode, {member-ptr discriminator, handler}), and its handler at 0x08b2bb88 does exactly
	//     lb  a0, 0x789(ped)      ; the slot
	//     slot * 28               ; sll 5 minus sll 2
	//     addiu a0, a0, 0x574     ; weapon array base
	//     lw  a0, 4(a0)           ; -> weapon TYPE
	// so this is a SLOT, not a weapon id, and the id lives in the array it indexes. To tell melee
	// from a gun read the type: PlayerBase + 0x574 + slot * 28 + 4. Records are 28 bytes,
	// {?, type, state, ammoInClip, ammoTotal, ?}; slots 0 and 9 read type 0 in a savestate where
	// 1..8 held 10, 13, 19, 23, 26, 28, 32, 31.
	//
	// Declared U8 because VCSAddrType has no signed byte. The game reads it with `lb`, so if it
	// ever stores -1 for "no weapon" that arrives here as 255 - range-check before indexing.
	{ VCSAddr::WeaponIndex,   "WeaponIndex",   VCSAddrType::U8,    0x789,         VCSAddr::PlayerBase,  "Current weapon SLOT (0-9), not a weapon id. Type = PlayerBase + 0x574 + slot*28 + 4" },
	{ VCSAddr::IsAiming,      "IsAiming",      VCSAddrType::U32,   0x08bb32a0,    kNoBase,              "1 while LOCKED ON to a target. Stays 0 during free aim" },
	{ VCSAddr::IsFreeAiming,  "IsFreeAiming",  VCSAddrType::U32,   0x08bafb54,    kNoBase,              "SUSPECT: tracks sniper aim but likely means cinematic/control-restricted - also 1 in cutscenes. Not used for context" },
	{ VCSAddr::CameraYaw,     "CameraYaw",     VCSAddrType::Float, 0x08bc7f1c,    kNoBase,              "Radians in [0, 2PI). Writing it rotates the view - this is the mouse-look input" },
	{ VCSAddr::CameraPitch,   "CameraPitch",   VCSAddrType::Float, 0x08bc7f18,    kNoBase,              "Radians, sits 4 bytes BEFORE CameraYaw. Negated vs the matrix; game rewrites it every frame" },
	{ VCSAddr::GameState,     "GameState",     VCSAddrType::U32,   kUnsetAddress, kNoBase,              "TODO: compare pause menu vs gameplay" },
	// CPad[0] is at 0x08bde610 - lifted straight out of CPad::GetPad at 0x0898b428, which computes
	// 0x08bde610 + index * 216. Absolute rather than based: it's a static array, not a heap struct,
	// so it does not move. Offsets confirmed live by injecting each d-pad button and watching the
	// field go to 255.
	{ VCSAddr::PadDPadUp,     "PadDPadUp",     VCSAddrType::U16,   0x08bde622,    kNoBase,              "CPad+0x12. Camera/aim Y comes from (Down - Up) / 2" },
	{ VCSAddr::PadDPadDown,   "PadDPadDown",   VCSAddrType::U16,   0x08bde624,    kNoBase,              "CPad+0x14. Pairs with PadDPadUp" },
	{ VCSAddr::PadDPadLeft,   "PadDPadLeft",   VCSAddrType::U16,   0x08bde626,    kNoBase,              "CPad+0x16. Camera/aim X comes from (Right - Left) / 2" },
	{ VCSAddr::PadDPadRight,  "PadDPadRight",  VCSAddrType::U16,   0x08bde628,    kNoBase,              "CPad+0x18. Pairs with PadDPadLeft" },
	// gp - 0x3F00, with gp measured live at 0x08bb1d60. Ships as 0, which selects the NUB - which
	// is why the aim axis fights movement, they are the same stick. Set it to 1 and the axis comes
	// from the d-pad pair instead, which is the whole point of writing those.
	{ VCSAddr::CameraInputMode, "CameraInputMode", VCSAddrType::U8, 0x08bade60,   kNoBase,              "0 = camera/aim axis from the nub (shipped), nonzero = from the d-pad fields" },
	// PERMANENTLY UNSET, and that is now a result rather than a gap. Do not resume the hunt.
	//
	// Four candidates were driven and watched, and all four missed:
	//   CameraYaw   0x08bc7f1c   swings the VIEW, gun keeps pointing where it was
	//   PlayerBase+0x334         turns the character's HEAD (look-at tracking)
	//   PlayerBase+0x34c         accepts and holds writes, no observable effect
	//   0x08bb34b4               inert; best of 322 angle-ranged full-RAM hits, still nothing
	//
	// Reading the game's code settled why. The weapon-aim camera is CCam mode 45, whose Process
	// function is 0x089a341c, and it does not store an aim direction anywhere - it INTEGRATES the
	// look axis into the camera's own Beta/Alpha (the fields this table already knows as CameraYaw
	// and CameraPitch, at CCam+0x7c and +0x78), and the gun is resolved from that plus the ped's
	// own state each frame. There is no scalar to write, which is exactly why five rounds of
	// value-correlation found nothing: the thing being searched for does not exist.
	//
	// What that leaves is the axis itself, and it turns out to be enough - the game's response to
	// the axis is a fixed, readable formula, so it can be inverted. See the aim response model in
	// VCSCamera.cpp, which is what actually fixed the thumbstick feel.
	//
	// Layout confirmed while establishing this, all from the code rather than from correlation:
	//   CCamera            0x08bc7e30
	//   CCamera+0x50       active cam index (u8, observed 0 in gameplay)
	//   CCamera+0x70       CCam m_asCams[3], stride 0x260 - so cam[0] is 0x08bc7ea0
	//   CCam+0x00          mode (s16); 45 and 11 are the two weapon-aim modes
	//   CCam+0x78/+0x7c    pitch / yaw  == this table's CameraPitch / CameraYaw, i.e. cam[0]
	//   CCam+0x124/+0x130  the smoothed per-frame increment added to those
	//   CCam+0x128         FOV
	//   CCamera+0x7b8      PlayerWeaponMode.Mode (s16)
	// The index math is at 0x08a228a0 (idx*608 + 0x70) and the jump table at 0x08b7ed88.
	{ VCSAddr::AimYaw,        "AimYaw",        VCSAddrType::Float, kUnsetAddress, kNoBase,              "Does not exist as a stored scalar - see the note above. Kept unset deliberately" },
	{ VCSAddr::AimPitch,      "AimPitch",      VCSAddrType::Float, kUnsetAddress, kNoBase,              "As AimYaw" },
	// gp + 0x1dfc, with gp measured live at 0x08bb1d60. Read as a float by every camera mode that
	// integrates the look axis - e.g. at 0x0899d2e0 and 0x089a37f8, where it scales the angular
	// rate, and again at 0x089a38fc where it becomes the exponent of the smoothing factor. Read
	// 1.668 in an in-game savestate and 0.0 at the menu, so treat a non-positive value as "not in
	// gameplay" rather than as a real timestep.
	{ VCSAddr::TimeStep,      "TimeStep",      VCSAddrType::Float, 0x08bb3b5c,    kNoBase,              "Game frame delta, in the game's own units. Every camera angular rate is scaled by it" },
	// gp + 0x1e54. The `lw / addiu 1 / sw` at 0x08a114c4, which is the last thing CTimer::Update
	// does. Read 11621 in an in-game savestate (~6.5 minutes at 30fps) and 0 at the menu.
	{ VCSAddr::FrameCounter,  "FrameCounter",  VCSAddrType::U32,   0x08bb3bb4,    kNoBase,              "Game logic frame counter, +1 per 30fps frame. Tells a real frame from a bare vblank" },
	// All from the CCamera layout in the AimYaw note above: CCamera 0x08bc7e30, m_asCams at +0x70,
	// stride 0x260, active cam index 0. So CCam[0] is 0x08bc7ea0 and these are its fields.
	{ VCSAddr::CamMode,       "CamMode",       VCSAddrType::U16,   0x08bc7ea0,    kNoBase,              "CCam[0]+0x00. 7/8/34/45/46/47 use one aim response, 11/28 a different one" },
	{ VCSAddr::CamAimIncX,    "CamAimIncX",    VCSAddrType::Float, 0x08bc7fd0,    kNoBase,              "CCam[0]+0x130. Smoothed increment added to CameraYaw each frame" },
	{ VCSAddr::CamAimIncY,    "CamAimIncY",    VCSAddrType::Float, 0x08bc7fc4,    kNoBase,              "CCam[0]+0x124. Smoothed increment added to CameraPitch each frame" },
	{ VCSAddr::CamFOV,        "CamFOV",        VCSAddrType::Float, 0x08bc7fc8,    kNoBase,              "CCam[0]+0x128. Read 70.0 in gameplay. Aim rates scale with FOV/80" },
	{ VCSAddr::PedVelX,       "PedVelX",       VCSAddrType::Float, 0x140,         VCSAddr::PlayerBase,  "Movement velocity X, per frame. Zero in free aim - that is the thing being worked around" },
	{ VCSAddr::PedVelY,       "PedVelY",       VCSAddrType::Float, 0x144,         VCSAddr::PlayerBase,  "Movement velocity Y. Pairs with PedVelX" },
	// re3's CPed::m_fRotationCur / m_fRotationDest, and found by VALUE rather than by correlation:
	// the ped's matrix forward is (-sin, cos) of its heading, so the heading is computable from the
	// entity struct - and exactly three floats in the whole 6 KB ped matched it, +0x6b8, +0x8d0 and
	// +0x8d4. The adjacent pair is the cur/dest pair; +0x6b8 is a third copy, not written here.
	//
	// Confirmed independently by the game's own code: FireInstantHit reads +0x8d0 for the player,
	// compares it against a global copy of its previous value and stores the new one back
	// (0x08a48694..0x08a486bc) - it is tracking how far the player turned between shots, which is
	// only meaningful for the live heading.
	//
	// Write DEST to turn the way the game turns, with its own animation and rate; write CUR as well
	// to snap. Both, because a mouse is a position control and a turn rate reintroduces exactly the
	// lag the whole aim-model effort exists to remove.
	{ VCSAddr::PedHeading,       "PedHeading",       VCSAddrType::Float, 0x8d0, VCSAddr::PlayerBase, "m_fRotationCur, radians. Forward = (-sin, cos). Free aim freezes without this being driven" },
	{ VCSAddr::PedHeadingTarget, "PedHeadingTarget", VCSAddrType::Float, 0x8d4, VCSAddr::PlayerBase, "m_fRotationDest. The game eases PedHeading toward it" },
	// re3's CPed::m_pPointGunAt, read straight out of FireInstantHit (0x08a48950: `lw a0, 0x81c(s2)`
	// then branch on whether it is null). Set through one setter at 0x08907b98 and cleared at
	// 0x08913a70 - a pointer with exactly one writer, which is what a member like this should look
	// like. The game caches its position at CPed+0xC80 and uses that as the shot's target when the
	// entity is a DUMMY and the shooter is the player.
	{ VCSAddr::PedPointGunAt, "PedPointGunAt", VCSAddrType::U32, 0x81c, VCSAddr::PlayerBase, "Entity the gun is aimed at. In free aim it is a DUMMY parked at the aim point" },
	// World position, from the entity matrix. Unlike velocity, nothing recomputes this from the
	// movement state - so a small step added each frame accumulates instead of being wiped, which
	// is what makes translating the player directly a viable way to move during free aim.
	// `b 0x0894b890` (0x1000000a), reached after the aim call at 0x0894b85c and jumping over the
	// movement call at 0x0894b888. Found by breakpointing all four call sites of the movement
	// applier and diffing: 0x0894b888 ran on 100% of walking frames and 24% of aiming ones.
	//
	// NOTE the live word here reads 0x68xxxxxx, not 0x1000000a - PPSSPP's JIT overwrites the first
	// instruction of each compiled block with a block marker. Invalidate the icache before reading
	// or writing, or you will compare against, and clobber, a JIT pointer.
	{ VCSAddr::MoveGateBranch, "MoveGateBranch", VCSAddrType::U32, 0x0894b864,    kNoBase,              "CODE. The branch that skips movement while aiming. Patched, not read" },
	// gp - 0x3588. Squared at 0x0899d2e4, so it is the game's look sensitivity setting; the Controls
	// menu writes it. Read 0.007.
	{ VCSAddr::LookSensitivity, "LookSensitivity", VCSAddrType::Float, 0x08bae7d8, kNoBase,             "Game's own look sensitivity. Only in the mode 11/28 response, where it is squared" },
	// CPad[0] + 0xd0, applied at 0x0898de90. Read 1.05. Left unmodelled at first, and it cost about
	// 10% of over-cancellation, because the game's response is 1.05^2 stronger than a model without
	// it - which showed up as the aim snapping back at the end of every movement.
	{ VCSAddr::AimAxisScale,  "AimAxisScale",  VCSAddrType::Float, 0x08bde6e0,    kNoBase,              "CPad+0xd0. Axis multiplier, applied only in weapon camera modes 45 and 11" },
	// The pair to AimAxisScale, and the answer to what script opcode 03E9 does. Its handler
	// (0x089e0b14, found via the command table at 0x08b846e0 - see docs/VCS_ADDRESSES.md) calls
	// CPad::GetPad(0) twice and stores its two float params to CPad+0xd0 and CPad+0xd4. So 03E9 is
	// "set aim axis scale x, y". The retail script calls it once, `03E9 2.5 0.5`, setting up a
	// passenger drive-by - wide horizontally, damped vertically; the CLEO plugin's mouse.txt calls
	// `03E9 1.4 0.0`, zeroing Y because it supplies Y itself.
	{ VCSAddr::AimAxisScaleY, "AimAxisScaleY", VCSAddrType::Float, 0x08bde6e4,    kNoBase,              "CPad+0xd4. Y counterpart of AimAxisScale; opcode 03E9 writes both" },
	{ VCSAddr::WeaponCamMode, "WeaponCamMode", VCSAddrType::U16,   0x08bc85e8,    kNoBase,              "CCamera+0x7b8, PlayerWeaponMode.Mode. AimAxisScale applies only when this is 45 or 11" },
};

static_assert(ARRAY_SIZE(kVCSAddresses) == (size_t)VCSAddr::Count,
	"kVCSAddresses must have exactly one entry per VCSAddr, in enum order");

// Returns the table entry for an id. Cheap - it's a direct index.
inline constexpr const VCSAddrEntry &LookupAddr(VCSAddr id) {
	return kVCSAddresses[(size_t)id];
}

// Whether we know where this value lives. For a based entry the offset may legitimately be 0,
// so having a base is itself the signal - don't test the address field alone.
inline constexpr bool IsAddrSet(VCSAddr id) {
	const VCSAddrEntry &entry = LookupAddr(id);
	if (entry.base != kNoBase) {
		return IsAddrSet(entry.base);
	}
	return entry.address != kUnsetAddress;
}

// True once enough of the table is filled in that we can tell on-foot from in-vehicle. Until
// then VCSInput deliberately stays in passthrough, because guessing the context would mean
// remapping buttons at random.
inline constexpr bool HasContextAddresses() {
	return IsAddrSet(VCSAddr::PlayerVehicle) || IsAddrSet(VCSAddr::PlayerOnFoot);
}

}  // namespace VCS
