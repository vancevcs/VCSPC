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

// Where to put the shot's own source BACK, once the raycast has read it.
//
// The hooked call sits in a wrapper that returns immediately after it:
//
//     08a41d74  jal   0x889786c        <- kVCSWeaponRaycastCall, the hook
//     08a41d78  sw    $t4, 0x10($sp)   <- delay slot
//     08a41d7c  lw    $ra, 0x20($sp)   <- this, exactly once, after the raycast
//     08a41d80  jr    $ra
//     08a41d84  addiu $sp, $sp, 0x30
//
// So this instruction runs after ProcessLineOfSight has consumed point1 and before the CALLER -
// CWeapon::FireInstantHit, which is what draws the gunflash - gets control back. That makes it the
// one point where the source can be a camera-ray origin for the raycast and the muzzle for
// everything after it. See the note on useCameraOrigin.
inline constexpr u32 kVCSWeaponRaycastDone = 0x08A41D7C;
inline constexpr u32 kVCSWeaponRaycastDoneOp = 0x8FBF0020;   // lw $ra, 0x20($sp)

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

// THE SPLASH, and why a vault on dry land made one.
//
// The climb-out is the SWIMMING pull-up, so the animation plays a water splash partway through -
// and nothing on that path asks whether there is any water, because until this fork existed there
// always was. It sits in the anim finish callback at 0x08905824, on the branch taken when the
// climb stage goes 1 -> 2, i.e. the moment the ped clears the edge:
//
//     08905914  lw    $a1, 0x60($s1)      ; the ped's audio entity id
//     0890591c  addiu $a0, $gp, 0x1e58    ; DMAudio
//     08905920  jal   0x08a05f80          ; <- kVCSPedClimbSplashCall
//     08905924  ori   $a2, $zero, 0x18    ; sound 24
//
// Sound 24 is the splash, established rather than assumed: the ped code at 0x08927740 plays the
// same id and plays it ONLY inside `if (ped->0xEC & 0x100)` - the in-water flag. The climb's call
// has no such test. The physics and vehicle code play it too (0x088a5db8, 0x08837b14), which is
// what a general "something entered water" sound looks like. The audio manager turns it into
// sample 0xae at 14000 Hz, with no surface lookup and no variants.
//
// 0x08a05f80 is a two-instruction wrapper onto cAudioManager::PlayOneShot (0x089b83c0), whose
// first test is `bltz $a1` - a negative audio entity id is dropped before it touches the queue.
// That is the whole suppression mechanism: the hook hands it -1 and the call runs to a no-op, so
// the game's own instruction is never rewritten and a genuine climb out of the water still
// splashes. See "Vaulting" in CLAUDE.md.
//
// A `jal` in the middle of a function, like the fire hook's site and unlike kVCSFindGroundZOp2 -
// nothing branches here, so no JIT block starts here and the live word really is the game's.
inline constexpr u32 kVCSPedClimbSplashCall = 0x08905920;
inline constexpr u32 kVCSPedClimbSplashOp = 0x0E2817E0;  // jal 0x08a05f80

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
// --- The front end's widget layout ---
//
// Read off the live objects rather than guessed: `Map_AE` measured x=0 y=0 w=480 h=224, a tab
// label x=138 y=248 w=110 h=28 - which is the bottom strip - and `Background` 0,0,480,272.
//
// `Visible` is the one that matters most: the page draw at 0x08ae20c8 does `lbu +0x20` on each
// widget and skips it entirely when zero, so hiding a widget is a byte, not a code patch.
inline constexpr u32 kVCSWidgetName = 0x00;     // pointer to the widget's ASCII debug name
inline constexpr u32 kVCSWidgetX = 0x0c;
inline constexpr u32 kVCSWidgetY = 0x10;
inline constexpr u32 kVCSWidgetW = 0x14;
inline constexpr u32 kVCSWidgetH = 0x18;
inline constexpr u32 kVCSWidgetAlpha = 0x1c;    // float
inline constexpr u32 kVCSWidgetVisible = 0x20;  // u8; zero means the draw loop skips it
inline constexpr u32 kVCSWidgetsBegin = 0x24;   // a page/group's own widget vector
inline constexpr u32 kVCSWidgetsEnd = 0x28;
// The entry a page has highlighted - a pointer to one of its own children, so its NAME says
// which one. Read live: the Game page holds LoadGame_MI on arrival, and the confirm page
// BTN_SPECIAL_CANCEL, moving to BTN_SPECIAL_CONFIRM on a d-pad down.
//
// This is the field the direct page jump got wrong: writing MenuPage moves what is drawn and
// leaves +0x30 pointing at a widget belonging to the page you came from, which is a real object
// of the wrong type and dies on the third dereference. See jumpDirectlyToPage.
inline constexpr u32 kVCSWidgetSelected = 0x30;

// The PSP framebuffer, which is what the front end lays itself out in - and NOT what is actually
// displayed here, which measures about 330 rows. That gap is the whole reason a backdrop sized to
// the game's own idea of the screen still leaves a black band. See VCSFrontEnd's backdropHeight.
inline constexpr int kVCSScreenHeight = 272;

// How many of the eleven pages are on the tab strip: map(0) brief(1) game(2) stats(3) controls(4)
// audio(5) display(6) multiplayer(7). The remaining three - MP_ERROR_PAGE, MEMCARD_FULL_PAGE,
// CONFIRM_PAGE - are dialogs pushed on top, and are not tabbed to.
inline constexpr int kMenuTabPages = 8;

// Where MAP_PAGE's `Map_AE` widget keeps what the map is centred on. Found by descending into the
// page and holding a direction: right moved +0xc0 by -166.69 and down moved +0xc4 by the same, so
// they are a symmetric x/y pair in map units. Nothing moves while the tab strip still has focus,
// which is why an earlier search for this found nothing at all.
// The path node array, straight out of 0x08977830's own arithmetic:
//
//     lw   $a3, 0xc($a0)     ; count, bounds-checked against the index
//     lw   $a3, 0($a0)       ; the array
//     sll  $t0, $a2, 3       ; index*8 ...
//     addu $t0, $a2, $t0     ; ... + index   = index*9
//     addu $t3, $a2, $t0     ; ... + index   = index*10   <- the stride
//     lh   $t0, 0($t2)       ; x
//     lh   $t1, 2($t2)       ; y
//     lb   $t2, 4($t2)       ; z
//
// Positions are stored times 8, which the extents confirm: x spans -16638..12005, or -2080..1501
// once divided, against a player measured at -1093.
inline constexpr u32 kVCSPathNodeArray = 0x00;    // ThePaths+0x00
inline constexpr u32 kVCSPathNodeCount = 0x0c;    // ThePaths+0x0c, read 8380
inline constexpr u32 kVCSPathNodeStride = 10;
inline constexpr float kVCSPathNodeScale = 8.0f;
inline constexpr u32 kVCSPathNodeX = 0x00;        // s16
inline constexpr u32 kVCSPathNodeY = 0x02;        // s16
inline constexpr u32 kVCSPathNodeZ = 0x04;        // s8
inline constexpr u32 kVCSPathNodeFirstLink = 0x06;  // u16 into the link array
inline constexpr u32 kVCSPathNodeLinkCount = 0x08;  // low nibble; the high nibble is flags

// The links are u16 node indices sitting immediately AFTER the node array - there is no pointer to
// them. Confirmed by arithmetic rather than by a scan: the gap between the node array's end and
// ThePaths+0x08 is 35264 bytes, which is exactly the 17632 links the nodes themselves declare.
inline constexpr u32 kVCSPathLinkStride = 2;

// The nodes are TWO graphs in one array, and no link crosses between them. `ThePaths+0x10` counts
// the first group and `+0x14` the second; they sum to exactly the total.
//
// The first group is the ROAD network - measured, after two heuristics disagreed. Parked on an
// ordinary road, the car sat **1.46 units** from a group-1 link segment and **5.63** from the
// nearest group-2 one: on a road, and a pavement's width off the other. Connectivity says the same
// - group 1 is 13 components with 78% in one, group 2 is 95 components with 40% in the largest,
// which is what a pavement network fragmented by roads looks like and what a drivable road network
// cannot be.
//
// The byte at node+5 pointed the other way - non-zero on 95% of group 2 and only 9% of group 1 -
// so it is not the road width it looked like. Recorded because it is the one piece of evidence
// that disagreed, and a later reader deserves to know it was weighed rather than missed.
inline constexpr u32 kVCSPathCarNodeCount = 0x10;   // ThePaths+0x10, read 3087: nodes [0, 3087)
inline constexpr u32 kVCSPathPedNodeCount = 0x14;   // ThePaths+0x14, read 5293: the rest

// The blip store is an array of 0xc0-byte entries. Found by toggling the map marker and diffing:
// one Square press flipped entry 13's active flag from 4 to 0 and its type from 0x56 to 2.
//
//   +0x04  active   4 while placed, 0 once cleared
//   +0x10  world X  }  same units as the player position
//   +0x14  world Y  }
//   +0x20  type     0x56 for the player's map marker; 0x7e for safe houses
//
// **Found by TYPE, never by slot index**, and that is the whole point of these constants. The
// first version of this used a fixed offset, on the evidence that two markers placed in a row
// landed in the same entry - which proves nothing, because a transient blip reuses the free slot
// it just released. It read a safe house the moment anything else was allocated, and the route
// drew confidently to the wrong place.
// The array starts at store+0x270 and the entries are 0x30 apart. Both fall out of the handle
// validator at 0x0880e450, which is the game's own arithmetic rather than anything measured:
//
//     sll  $a3, $v0, 4     ; index * 16
//     addu $t1, $a3, $a3   ;  ... * 32
//     addu $a3, $a3, $t1   ;  ... * 48 = 0x30    <- the stride
//     lhu  $a0, 0x29a($a0) ; the entry's serial
//
// 0x29a is entry+0x2a once the array base is 0x270, which is what fixes the base. The marker sits
// at store+0x9c0, and (0x9c0 - 0x270) / 0x30 = 39 exactly.
//
// **This was 0xc0 from the store base and worked by luck.** The marker happened to land on a
// multiple of four entries, so a scan stepping 0xc0 hit it; any other slot and it would have been
// invisible. Second time the same mistake has been made on this store - see the note below.
inline constexpr u32 kVCSBlipArray = 0x270;
inline constexpr u32 kVCSBlipStride = 0x30;
inline constexpr u32 kVCSBlipActive = 0x04;

// What the blip is FASTENED TO, which decides whether its stored position means anything.
//
//   1  a vehicle      2  a ped      3  an object      4  a plain coordinate
//
// The first three carry an entity handle at +0x08 and the game resolves it every frame when it
// draws them; the x/y in the record is only where the entity WAS when the blip was made, and the
// game never writes it again. Measured on a live mission blip: type 1, handle 0x1746, its stored
// position 2081 units from the player and frozen for as long as it was watched - while the car
// that handle names sat 25 units away, beside the player, having driven the whole way there.
//
// So a route built on the stored coordinate of an entity blip goes to a place the mission stopped
// caring about, which is exactly what "the GPS keeps sending me back to where I have already been"
// turned out to be.
inline constexpr u32 kVCSBlipEntityKind = 0x04;
inline constexpr u32 kVCSBlipEntityHandle = 0x08;
inline constexpr u32 kVCSBlipKindMaxEntity = 3;   // 1..3 are fastened to something; 4+ are places
inline constexpr u32 kVCSBlipX = 0x10;
inline constexpr u32 kVCSBlipY = 0x14;
inline constexpr u32 kVCSBlipType = 0x20;
// The byte at +0x20 is flags, and its value sorts a blip into a kind. Three are known, read off a
// live store with a mission running:
//
//   0x56  the marker the player drops on the pause map
//   0x66  a mission destination - a coord blip with no icon id, drawn as a plain coloured marker,
//         which is the pink dot a mission points you at
//   0x7e  a permanent map icon: shops, safe houses, the things with their own artwork
//
// The low bits are shared and are set by the creator itself (0x02 on create, 0x04 in use), so the
// kind lives in the high ones. Matched whole rather than by bit, because which bit means what is
// not established and a wrong mask would quietly pick up the safe houses - which is exactly the
// bug that had the route pointing at one.
inline constexpr u32 kVCSBlipMarkerType = 0x56;
inline constexpr u32 kVCSBlipMissionType = 0x66;

// The icon a blip draws, and the field that separates a live mission destination from the hint the
// game leaves pointing at your safe house between jobs. Both are `0x66`; measured, the difference
// is here:
//
//   mid-mission, being sent somewhere   +0x20 = 0x66   +0x29 = 0x00
//   between jobs, go-home hint          +0x20 = 0x66   +0x29 = 0x06
//
// A blip with an icon is a PLACE - a shop, a safe house, the character who starts the next job -
// and it has artwork of its own. A mission destination is a bare coordinate with no icon, which is
// precisely why it draws as a plain coloured dot. So "objective marker with no icon" is the live
// one, and requiring the zero is what stops the GPS overriding a dropped waypoint to send the
// player home.
//
// If a mission ever marks its destination WITH an icon, this stops matching it and the route falls
// back to the waypoint - which is a quiet failure rather than a wrong one.
inline constexpr u32 kVCSBlipIcon = 0x29;
inline constexpr u32 kVCSBlipSerial = 0x2a;      // u16; the high half of a blip handle

// 75 slots, and "in use" is bit 2 of the type byte. Both come from the creator's own free-slot
// search at 0x0880e06c, which is also what finally confirmed the base and stride above:
//
//     lb    $v0, 0x290($t1)     ; $t1 = store + i*0x30, so this is entry+0x20
//     andi  $v0, $v0, 4         ; bit 2: taken
//     beqz  $v0, claim
//     addiu $t1, $t1, 0x30      ; next entry
//     sltiu $t0, $t2, 0x4b      ; ... 75 of them
inline constexpr int kVCSBlipMaxEntries = 75;
inline constexpr u32 kVCSBlipInUseBit = 0x04;

// From the radar object, which is the same object as the blip store. The range is the world
// radius the radar shows, and it changes with the zoom - 96.0 at the default.
// Whether the HUD - and therefore the radar - is on screen at all.
//
// `+0x2436` is the FIRST thing the HUD draw reads and a zero returns immediately, so it is
// reVC's CHud::m_Wants_To_Draw_Hud by behaviour if not by name. Two further gates sit just
// below it, and `+0x2be8 == 2` is the one of them that is a plain field rather than a call.
//
// This is what a cutscene turns off. Without it the route line and the player arrow went on
// being drawn over a screen with no radar under them - which is the one failure mode an overlay
// painted onto the finished frame cannot notice by itself.
// The one field that tracked a cutscene, and it was found by diffing rather than by reasoning.
//
// The three gates the HUD draw actually tests were all chased down and NONE of them is this:
//   +0x2436          read first, zero returns immediately - stayed 1 across a whole cutscene
//   +0x2be8 == 2     never took the value 2
//   FrontEndMenuManager+0x20 != 0        that is MenuActive, handled separately
//   *(float*)(CCamera+0xa54) == 255.0    a full fade to black, not a cutscene
//
// So the HUD draw runs during a cutscene and the radar is suppressed somewhere below it. Rather
// than guess a fourth time, all 11328 bytes of the HUD object were sampled four times a second
// for five minutes with one cutscene in the middle. Exactly one non-text field went to zero for
// the cutscene's duration and was non-zero either side of it: this one.
//
// It is a CORRELATION over a single cutscene, not a gate anyone has read in the code, and it is
// labelled that way on purpose. If the line ever hides during ordinary play, this is the reason
// and removing it costs nothing.
inline constexpr u32 kVCSHudCutscene = 0x0c;

inline constexpr u32 kVCSHudDrawFlag = 0x2436;
inline constexpr u32 kVCSHudSuppressState = 0x2be8;
inline constexpr u32 kVCSHudSuppressValue = 2;

inline constexpr u32 kVCSRadarRange = 0x1ab8;

// The map's cursor - the pink cross - and how it is put away.
//
// The whole crosshair is one call: `jal 0x0897bcc8` at 0x0897ba2c, inside the map's draw. That
// helper draws two quads through 0x089d1b1c (build a rect) and 0x08af5130 (draw it), tinted by
// 0x08a3550c(rect, 0xff, 0x8b, 0xc2, 0xb4) - RGBA 255,139,194,180, which is the pink on screen and
// is how the helper was identified. Half-thickness is a literal 1.0 at 0x0897bd1c.
//
// Its extents are the screen, by construction rather than by data:
//
//   horizontal   x from 0 to Map_AE+0x14 (the widget's width), at centreY +/- 1
//   vertical     y from 0 to screenHeight - 47,                 at centreX +/- 1
//
// so there is no field to shrink and nothing short of rewriting the arithmetic would make it
// smaller. Nopping the one call removes all of it, which leaves a cross of our own free to be any
// size - and is a single instruction to install and to undo.
inline constexpr u32 kVCSMapCrosshairCall = 0x0897ba2c;

// Where the cursor sits, which the same helper computes as
//
//   centre = (Map_AE.width / 2 + [0xd0],  Map_AE.height / 2 + [0xd4])
//
// in the PSP's own 480x272 screen coordinates. Both offsets read zero in play, so the cursor is
// the middle of the WIDGET - and the widget's height is 400 because the chrome fix stretches it to
// kill the black bar, which puts the cursor at y=200 on a 272-row screen rather than at 136. That
// is where the game really does place a waypoint, so it is where our cross has to go: drawing at
// the visual centre would put the cross somewhere the marker does not land.
//
// **READ THESE. DO NOT WRITE THEM.** They read zero because they are zero most of the time, not
// because they are static: the map's update at 0x0897cba0-0x0897cd4c is a pan solver that stores
// the remaining distance to its target in this pair and zeroes each axis as it arrives, with
// +0xc0 / +0xc4 driven off their sign. Writing a constant into +0xd4 to move the cursor is
// therefore an assertion that the map is still travelling vertically, made every frame and
// winning against the game's own logic - which is how the map came to pan left and right but not
// up or down. See the note where `centreMapCursor` used to be, in VCSFrontEnd.h.
inline constexpr u32 kVCSMapCursorOffsetX = 0xd0;
inline constexpr u32 kVCSMapCursorOffsetY = 0xd4;

inline constexpr u32 kVCSMapPanX = 0xc0;
inline constexpr u32 kVCSMapPanY = 0xc4;

// --- The script globals a save is made of -----------------------------------------------------
//
// Indices into the global block at `ScriptSpace` - global N is at `ScriptSpace + N*4`.
//
// **A save is not just the world; it is these.** The engine writes the global block into the save
// file, and the main script reads them back on boot to decide what kind of game this is. Setting
// the save-menu request byte and nothing else produces a file that loads into the OPENING MISSION
// with the player's money and outfit, which is what "the save game button worked but loading it
// started the game over" turned out to be - measured twice, and confirmed by reading `$4` as 0 in
// the game that came back.
//
// The names are the decompiled script's. Every index below was decoded from MAIN.SCM at 0xc65c,
// which is the run-up to `0260 activate_save_menu` in the safehouse save routine:
//
//     04 00 d0 15 07 01     0004: $789 = 1        ONMISSION
//     04 00 cd 04 07 01     0004: $4   = 1        "this game came from a save"
//     36 00 ce 1c d0 0f     0036: $284 = $783     the restart position, x
//     36 00 ce 1d d0 10     0036: $285 = $784                           y
//     36 00 ce 1e d0 11     0036: $286 = $785                           z
//     c0 02 d0 0e ce 12     02C0: weapon  -> $274
//     fe 02 d0 0e ce 13     02FE: armour  -> $275
//     96 00 d0 0e ce 14     0096: money   -> $276
//     5a 00 d5 78 d5 77     005A: get_time_of_day $2168 $2167
//     60 02                 0260: activate_save_menu
//
// The main script's first decision is on `$4`:
//
//     $_4 == 0 && $2 == 0  ->  $ONMISSION = 1; 0289: load_and_launch_mission_internal 8 // Soldier
//
// - which is the bug, in the game's own words.
//
// The last three are NOT written here, and that is a decision rather than an omission: the load
// path (MAIN_2139) restores the player's weapons from the engine's own save data and only reads
// `$274`/`$275` again on the wasted-and-busted path, which the script refreshes for itself at
// every mission start. Position is different - the load path copies `$284..286` straight back
// into `$783..785` - so it is written.
//
// **And the position written is the PLAYER's, not `$783..785`.** That is a deliberate departure
// from the routine above, and the reason is INITSAV, the script's own spawn code, which pairs the
// restart position with `$_282` - the safe house whose interior is currently swapped in:
//
//     $_282 > -1  ->  swap that interior back in, place the player from its own table
//     otherwise   ->  load_scene $783 $784 $785
//     either way  ->  get_ground_z_for_3d_coord $783 $784 $785 ; set_char_coordinates there
//
// The safe house routine can copy the pickup because it only ever runs while the player is
// standing ON that pickup, inside the house, with `$_282` naming it: both halves of the pair
// describe the same moment. An auto-save fires wherever the mission ended, with `$_282` at -1 -
// so copying the pickup pairs an INTERIOR position with an EXTERIOR world, and the load drops the
// player inside the safe house's solid shell with no way out but dying. Reported from play, and
// it stayed hidden for as long as one particular safe house was in use, whose pickup happens to
// sit somewhere escapable. The player's own position cannot disagree with `$_282`, because the
// two are read on the same frame.
//
// `$_287` is the heading INITSAV faces the player in on that same branch, and goes with the
// position for the same reason. DEGREES there - the script's own literals run to -177.68 - and
// radians in `PedHeading`, which is where ours comes from.
inline constexpr u32 kVCSGlobalLoadedGame = 4;     // $_4
inline constexpr u32 kVCSGlobalOnMission = 789;    // $ONMISSION
inline constexpr u32 kVCSGlobalRestartX = 284;     // $_284, $_285, $_286
inline constexpr u32 kVCSGlobalRestartHeading = 287;  // $_287, degrees
inline constexpr u32 kVCSGlobalSavePointX = 783;   // $783, $784, $785 - the last save pickup

inline constexpr u32 kVCSEntityPositionOffset = 0x30;
inline constexpr u32 kVCSVehicleModelOffset = 0x56;   // same field VCSAddr::VehicleModel reads
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
	PedAttachedTo,
	PlayerRemoteVehicle,
	PedAimYawLimit,
	PedAimPitchUp,
	PedAimPitchDown,

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

	// --- The game's own front end ---
	//
	// `FrontEndMenuManager` is a fixed global, so these are absolute rather than based: the object
	// does not move, only the page objects it points at do. See "Reaching the game's own front
	// end" in docs/VCS_ADDRESSES.md for how it was found.
	// The front end's object tree, for finding widgets by NAME. See "The front end is a named
	// widget tree" in docs/VCS_ADDRESSES.md - every widget carries a pointer to its own debug
	// name, which the retail build kept, so nothing here has to be identified by index.
	MenuRootPage,      // The chrome page - MASTER: the backdrop and the eight tab labels.
	MenuPagesBegin,    // Vector of the eleven content pages: MAP_PAGE, GAME_PAGE, ...
	MenuPagesEnd,
	MenuOverlaysBegin, // Vector of the four BUTTONS groups - the button-hint bar.
	MenuOverlaysEnd,

	// The road network - the middle of a GPS route, between the player and the marker.
	ThePaths,          // Pointer to CPathFind. The node array and its links hang off it.

	// The map marker the player drops - the destination half of a GPS route.
	BlipManager,       // Pointer to the radar's blip store. The map marker is found by scanning it.

	// The radar's own view of the world, for drawing a route line over it. Read out of the
	// transform at 0x0880edb0 - see VCSRadar.h for the arithmetic these three feed.
	HudObject,         // Pointer to the HUD. Says whether it is drawing itself at all.
	RadarOrigin,       // Two floats: the world point the radar is centred on. Tracks the player.
	RadarForward,      // Two floats: a unit vector, the direction the radar treats as up.

	MenuUseRoot,       // 1 while the tab strip has focus, 0 once you are inside the page.
	MenuActive,        // Nonzero while the game's own pause menu is up. The Menu context's gate.
	MenuPage,          // s8. Which of its pages is showing; -1 when the menu is closed.
	SaveMenuRequest,   // Set to 1 to ask the front end to open the save menu. It clears it itself.

	// What the script writes when a mission is passed - the auto-save's trigger. See the note
	// over the rows in the table below for where these came from.
	LatestMissionKey,  // First word of the 8-byte GXT key of the last STORY mission passed.
	LatestMissionKey2, // Its second word. Eight bytes have to be compared, not four - LAN_C01.
	MissionsPassed,    // Count of missions passed, odd jobs included. Not restored by a load.

	// The loaded SCM, which is where the script's global variables live. A POINTER, and a heap
	// one - it reads 0x09f68400 on one run and will read something else on the next.
	ScriptSpace,

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

	// --- The attached gunner, and the one state where CameraYaw is NOT a world angle ---
	//
	// A mission can strap the player to a vehicle with a weapon instead of seating him in it:
	// `02B6 attach_ped_to_car $PLAYER_CHAR car $5666 offset 1.6 1.0 0 position 3 angle_limit 70.0
	// weapon 34`, which is GON_C4's helicopter ride. PlayerVehicle reads 0 throughout - the game
	// does not consider him to be IN anything - so the fork sees an on-foot player holding a gun,
	// and every on-foot assumption about the camera is wrong at once.
	//
	// What actually changes is the meaning of `CameraYaw`. CCam::Process mode 45 branches on this
	// pointer at 0x089a3548, and on the attached branch it starts Beta at ZERO (0x089a3578) rather
	// than at `PedHeading + PI/2` (0x089a3584), skips the [0,2PI) normalise after integrating
	// (0x089a3980), and clamps instead. So while attached the field is a SIGNED angle measured
	// from the vehicle's own heading.
	//
	// Measured, by asserting the field every frame on a savestate of that ride and reading where
	// the camera's Front then pointed relative to the helicopter:
	//
	//     assert 0.00 -> 0.0 deg      assert  0.40 ->  22.9 deg
	//     assert 1.00 -> 56.3 deg     assert -0.40 -> -23.8 deg
	//
	// One to one, from the nose. Before that first write it sat at exactly 70.0 deg - pinned
	// against the clamp, because this fork had been asserting an absolute world yaw of 3.82 rad
	// (219 deg) into it. That is the whole bug: the aim appears to snap back to a fixed place and
	// stay there, because a value that far outside the window has nowhere else to land.
	{ VCSAddr::PedAttachedTo,    "PedAttachedTo",    VCSAddrType::U32,   0x848, VCSAddr::PlayerBase,  "The vehicle this ped is strapped to, 0 when not. CCam mode 45 reads it at 0x089a3548" },

	// The vehicle the player is driving BY REMOTE, which PlayerVehicle knows nothing about.
	//
	// "Domo Arigato Domestoboto" hands the player a robot - model 208, `bobo` in the game's own
	// name table - and drives it from a ped standing somewhere else entirely: measured with the ped
	// at (5.0, 1175.7, -195.2) in an interior and the robot 13 units away at (17.5, 1172.1, -188.8),
	// PlayerVehicle reading 0 the whole time. So the fork saw a man on foot and gave him walking
	// bindings for a machine that drives, which is why Shift accelerated it and Space reversed it.
	//
	// Found by taking the six static globals that pointed at the robot and reading the same six out
	// of three savestates of ordinary play. Five of them hold an entity in all of those - they are
	// camera and world bookkeeping. This one is 0 in every one of them and holds the robot here,
	// which is the whole of the evidence for what it means.
	{ VCSAddr::PlayerRemoteVehicle, "PlayerRemoteVehicle", VCSAddrType::U32, 0x08bde4b4, kNoBase, "The vehicle being driven by remote (the Domestobot), 0 when none. Near CPad, so likely CPlayerInfo's" },
	{ VCSAddr::PedAimYawLimit,   "PedAimYawLimit",   VCSAddrType::Float, 0x760, VCSAddr::PlayerBase,  "Yaw arc each side, radians - attach_ped_to_car's angle_limit. 1.22173 = 70 deg on GON_C4" },
	{ VCSAddr::PedAimPitchUp,    "PedAimPitchUp",    VCSAddrType::Float, 0xca0, VCSAddr::PlayerBase,  "Pitch limit UP, radians. Script 04CF writes it as degrees*PI/180; 10 deg on GON_C4" },
	{ VCSAddr::PedAimPitchDown,  "PedAimPitchDown",  VCSAddrType::Float, 0xca4, VCSAddr::PlayerBase,  "Pitch limit DOWN, radians. Script 04D0, same conversion; 55 deg on GON_C4" },
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

	// FrontEndMenuManager is at 0x08bc9100, reached from `0261 has_save_game_finished`
	// (0x089def04), which materialises it with `lui 0x8bd / addiu -0x6f00` and passes it to the
	// one-instruction getter at 0x0882e9b4 - `lbu $v0, 0x20($a0)`. That getter's result is OR'd
	// with SaveMenuRequest to answer "is the front end busy", which is what identifies +0x20 as
	// the menu's own active flag rather than as any other boolean in the object.
	{ VCSAddr::MenuRootPage,  "MenuRootPage",  VCSAddrType::U32,   0x08bc9100,    kNoBase,              "FrontEndMenuManager+0x00. The MASTER page: backdrop plus the eight *_t tab labels" },
	{ VCSAddr::MenuPagesBegin, "MenuPagesBegin", VCSAddrType::U32,  0x08bc9104,    kNoBase,              "FrontEndMenuManager+0x04. begin() of the page vector - eleven entries" },
	{ VCSAddr::MenuPagesEnd,  "MenuPagesEnd",  VCSAddrType::U32,   0x08bc9108,    kNoBase,              "FrontEndMenuManager+0x08. end() of the page vector" },
	{ VCSAddr::MenuOverlaysBegin, "MenuOverlaysBegin", VCSAddrType::U32, 0x08bc9110, kNoBase,            "FrontEndMenuManager+0x10. begin() of the BUTTONS groups - the hint bar" },
	{ VCSAddr::MenuOverlaysEnd, "MenuOverlaysEnd", VCSAddrType::U32, 0x08bc9114,   kNoBase,              "FrontEndMenuManager+0x14. end() of the BUTTONS groups" },
	// FrontEndMenuManager+0x1e. Measured, not inferred: on the Game page it reads 1, one Cross
	// takes it to 0 while the page index stays 2, and a second Cross then activates LOAD GAME and
	// pushes CONFIRM_PAGE. So 1 is "the tab strip has focus" and 0 is "you are inside the page" -
	// which is why a page tabbed to needs one more press before its entries respond, and why that
	// press must never be sent when this already reads 0.
	// gp - 0x4220. From `01B5 get_closest_car_node` (handler 0x08a9247c), which loads it and hands
	// it to 0x08976fbc - FindNodeClosestToCoors, taking a coordinate and a 800.0 radius - then
	// passes the node it gets back to 0x08977830, which turns a node index into world coordinates.
	// That second function is what documents the whole layout; see docs/VCS_ADDRESSES.md.
	{ VCSAddr::ThePaths,      "ThePaths",      VCSAddrType::U32,   0x08badb40,    kNoBase,              "gp-0x4220. CPathFind. Nodes at +0x00, count at +0x0c (8380), links after the nodes" },
	// gp + 0x16dc. Reached from `00C3 add_blip_for_coord` (handler 0x08a7913c), which collects
	// three floats and then calls 0x0880e450 with `*(gp + 0x16dc)` as its first argument - the
	// blip store - taking back an index or -1.
	{ VCSAddr::BlipManager,   "BlipManager",   VCSAddrType::U32,   0x08bb343c,    kNoBase,              "gp+0x16dc. The radar's blip store. Read 0x08e8ed40" },
	// gp+0x1744 and gp+0x174c, found by sweeping the globals for a float pair that tracked the
	// player and then confirmed from the code: 0x0880edb0 loads exactly these two pairs, and the
	// second reads as a unit vector to four decimal places.
	// gp+0x16d8, four bytes before the blip store. Traced from the radar draw at 0x0880d548 up
	// through its single caller each time - HUD draw 0x089bd32c, HUD update 0x089ba6ec - to
	// 0x08936530, which loads it from here and passes it down as `this`.
	{ VCSAddr::HudObject,     "HudObject",     VCSAddrType::U32,   0x08bb3438,    kNoBase,              "gp+0x16d8. CHud. +0x2436 is whether it draws at all" },
	{ VCSAddr::RadarOrigin,   "RadarOrigin",   VCSAddrType::Float, 0x08bb34a4,    kNoBase,              "gp+0x1744. Radar centre in world units; Y follows at +4" },
	{ VCSAddr::RadarForward,  "RadarForward",  VCSAddrType::Float, 0x08bb34ac,    kNoBase,              "gp+0x174c. Unit vector the radar rotates by; Y follows at +4" },
	{ VCSAddr::MenuUseRoot,   "MenuUseRoot",   VCSAddrType::U8,    0x08bc911e,    kNoBase,              "FrontEndMenuManager+0x1e. 1 = focus on the tab strip, 0 = inside the page" },
	{ VCSAddr::MenuActive,    "MenuActive",    VCSAddrType::U8,    0x08bc9120,    kNoBase,              "FrontEndMenuManager+0x20. 1 while the game's own pause menu is up. Read 0 in gameplay" },
	// +0x1c is a SIGNED byte index into the page vector at +0x04 (begin) / +0x08 (end) - the
	// getter at 0x0882e950 does `lb +0x1c / lw +0x04 / sll 2 / addu / lw`, and falls back to the
	// root page at +0x00 when +0x1e is set. Eleven pages in the gameplay savestate, and the index
	// reads -1 there, which is the closed state. Read as U8; the caller casts.
	{ VCSAddr::MenuPage,      "MenuPage",      VCSAddrType::U8,    0x08bc911c,    kNoBase,              "FrontEndMenuManager+0x1c, SIGNED. Page index into the 11-entry vector at +0x04. -1 = closed" },
	// gp + 0x1076. `0260 activate_save_menu` (0x089dee68) writes 1 here; the front end's update
	// reads it at 0x0882e23c/0x0882e284 and clears it at 0x0882e290 as it opens the save menu.
	{ VCSAddr::SaveMenuRequest, "SaveMenuRequest", VCSAddrType::U8, 0x08bb2dd6,   kNoBase,              "gp+0x1076. Write 1 to ask for the save menu; the front end clears it when it opens" },

	// Straight out of `01EB register_mission_passed`, resolved from the script command table the
	// usual way - u32(0x08b846e0 + 0x1eb*8 + 4) = 0x08886064 - and read there rather than hunted
	// for. It memcpys eight bytes of GXT key to gp+0x1fc0 and increments gp+0x1fc8:
	//
	//     088860e0  addiu $a0, $gp, 0x1fc0
	//     088860e8  jal   0x8b58ba0          ; memcpy(gp+0x1fc0, key, 8)
	//     088860f0  lw    $a0, 0x1fc8($gp)
	//     088860f4  addiu $a0, $a0, 1
	//     088860fc  sw    $a0, 0x1fc8($gp)
	//
	// The decompiled MAIN.SCM calls that opcode from exactly ONE place - a shared subroutine every
	// mission ends through - so this is the game's own definition of "a mission was passed", not a
	// correlation with one.
	//
	// `036A register_oddjob_mission_passed` (0x0888640c) bumps the COUNTER and leaves the key
	// alone, which is what separates the two: a story mission changes the key, a race or an empire
	// job only moves the count.
	//
	// Read live at gp = 0x08bb1d60: the key held "LAN_C01" on a 14.4% save, and the counter read 0
	// because a load does not restore it - it counts this session. Only the EDGE is used, so that
	// costs nothing, but do not display it as a total.
	{ VCSAddr::LatestMissionKey,  "LatestMissionKey",  VCSAddrType::U32, 0x08bb3d20, kNoBase,        "gp+0x1fc0. First 4 bytes of the last story mission's GXT key" },
	{ VCSAddr::LatestMissionKey2, "LatestMissionKey2", VCSAddrType::U32, 0x08bb3d24, kNoBase,        "gp+0x1fc4. Second 4. LAN_C01 and LAN_C02 share the first word" },
	{ VCSAddr::MissionsPassed,    "MissionsPassed",    VCSAddrType::U32, 0x08bb3d28, kNoBase,        "gp+0x1fc8. Missions passed this session, odd jobs included. Zeroed by a load" },

	// Where the script keeps its global variables, from the operand decoder at 0x08861a7c: an
	// operand byte >= 0xcd is a global, its index is `(type - 0xcd) << 8 | nextByte`, and the
	// address is `[gp - 0x71dc] + index * 4`. Read as a pointer, never baked: the value is a
	// heap address that differs every run.
	//
	// This is what makes a save a save. See kVCSGlobalLoadedGame below.
	{ VCSAddr::ScriptSpace,       "ScriptSpace",       VCSAddrType::U32, 0x08baab84, kNoBase,        "gp-0x71dc. Pointer to the loaded SCM; global N lives at [this] + N*4" },
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
