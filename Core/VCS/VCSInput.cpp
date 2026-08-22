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

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>
#include <set>

#include "Common/Common.h"
#include "Core/KeyMap.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSFireHook.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSVault.h"

namespace VCS {

// Host keys currently held down. Written from the input thread, read from the emu thread, so
// it needs the mutex. Only keys in the mapping table matter, but tracking everything is simpler
// and the set stays tiny in practice.
static std::mutex g_hostKeyMutex;
static std::set<InputKeyCode> g_heldKeys;

// Written by ApplyMapping on the emu thread, read by HandleHostKey on the input thread.
static std::atomic<VCSInputContext> g_currentContext{VCSInputContext::Unknown};

// Bits we set on the previous frame, so we can release them cleanly when the key comes up or
// the context changes. Without this, letting go of a key would leave the PSP button stuck.
// Emu thread only.
static u32 g_lastAppliedMask = 0;

// Buttons forced down by the debugger's tester, independent of the mapping table.
static u32 g_forcedButtons = 0;

// Whether we are currently driving the analog stick, and where we put it. Emu thread only.
static bool g_analogHeld = false;
static float g_analogX = 0.0f;
static float g_analogY = 0.0f;
// Whether that stick position came from the mouse (reticle) rather than WASD. Debugger only.
static bool g_analogIsReticle = false;
// Cumulative, for the debugger. The live x/y self-centres the instant the mouse stops, so it
// reads zero in every screenshot and can neither confirm nor deny that this path works. These
// don't decay: frames counts ticks the reticle ran, nonZero counts ticks it had movement to
// apply. nonZero flat while frames climbs means the routing is right and no mouse is arriving.
static u64 g_reticleFrames = 0;
static u64 g_reticleNonZero = 0;

// The right analog stick, which only the CLEO plugin path drives. Tracked separately from the
// left stick because it has its own owner and its own release. Emu thread only.
static bool g_aimStickHeld = false;
static float g_aimStickX = 0.0f;
static float g_aimStickY = 0.0f;

// Right mouse button. See the declaration in the header for why this is named rather than
// written out at each of its three use sites.
const InputKeyCode kVCSAimKey = NKCODE_EXT_MOUSEBUTTON_2;

// TOGGLES the PSP's lock-on instead of free aim. L as of 2026-08-17 (was G); checked free first -
// nothing in PPSSPP's own defaults binds it and neither does the table below, so claiming it costs
// nothing.
//
// This was CapsLock, held, and it never worked once - reported after living with it for a long
// time. CapsLock is mapped (Windows/RawInput.cpp maps VK_CAPITAL) and was claimed here with a
// psp = 0 row, so it looked correct in every place anyone would check; it is the OS that treats a
// lock key specially. The lesson is the one already in this file about bare modifiers: pick an
// ordinary key for anything that has to be observed, and confirm it arrives rather than confirming
// it is bound.
//
// A toggle rather than a hold because the thing it is for is melee, whose lock-on does not register
// in IsAiming - and holding a third key alongside the aim button and WASD, for a whole fight, is
// not a control scheme.
//
// It is a manual OVERRIDE now rather than the melee ritual it started as. MeleeEquipped answers the
// same question automatically, on both the free-aim routing and the automatic entry into it, so
// fists need no flipping before a fight - reach for this only when the weapon read is wrong or a
// gun should be aimed under lock-on deliberately.
const InputKeyCode kVCSLockOnKey = NKCODE_L;

// Jump, and - when a ledge is in front of the player - vault. See VCSVault: the vault consumes
// the press, so the game never sees a jump on the tick a climb starts.
const InputKeyCode kVCSJumpKey = NKCODE_SPACE;

// Set on the input thread by the key above, read on the emu thread by FreeAimActive.
static std::atomic<bool> g_lockOnMode{false};

// Auto free aim, as a small timer rather than a flag, because entering it is a BUTTON PRESS and
// a press needs both an edge and some duration.
//
// The delay before the press is the thing that decides whether you see lock-on at all. The game
// starts hunting for a target the moment the aim trigger registers, so every tick spent waiting is
// a tick in which it can find one and snap to it - which is exactly the "it locks onto the nearest
// NPC first" complaint. The original 9 was a guess at how long the trigger needs to register, and
// nine ticks is about 150ms, which is plenty of time to acquire someone.
//
// It is a setting now (aimFreeAimDelay) rather than a constant, because the right value is the
// smallest one that still works and that can only be found by trying it. The pulse LENGTH is
// separate and stays fixed - it only has to be long enough for the game's 30fps pad read to see
// the button down at all, and these are 60Hz ticks, so a few is right.
static int g_freeAimTimer = 0;
static const int kFreeAimPulse = 4;

// Automating the run-into-free-aim entry, so there is no ritual to perform.
//
// Free aim latches whatever movement state exists when it engages - that is what makes moving
// while aiming possible at all, see MoveGateBranch. Done by hand it means sprinting, letting go of
// sprint, and only then aiming, which is not a control scheme. All of it is producible from here:
// hold sprint and the movement stick for a few ticks after the aim key goes down, drop sprint but
// keep moving for a few more so the state settles to a run rather than a sprint, and only then
// press Free Aim. The game latches a running player and the animation comes with it.
//
// While this runs the stick has to carry MOVEMENT, not the mouse - otherwise there is no movement
// state to latch. ReticleActive stands down for the duration.
static int g_latchTimer = 0;
static const int kLatchStart = 16;        // ~8 game frames at 60Hz ticks
static const int kLatchDropSprintAt = 7;  // sprint for the first half, run for the rest

// Forward only, on W.
//
// Full WASD works and was built: the stick is camera-relative during the latch window, so holding A
// latches a run to the left of where you are looking exactly as W latches forwards, and a change of
// key simply re-runs the latch. It is kept to one key deliberately - every extra direction is
// another latched heading to get stuck in, and the movement cannot be steered once latched.
bool MovementKeysHeld() {
	return IsHostKeyDown(NKCODE_W);
}

// Defined further down, next to the explanation of why a scope changes the answer.
static bool CameraAimActive(VCSInputContext context);
static VCSInputContext g_prevAppliedContext = VCSInputContext::Unknown;

const char *VCSInputContextName(VCSInputContext context) {
	switch (context) {
	case VCSInputContext::OnFoot: return "OnFoot";
	case VCSInputContext::InVehicle: return "InVehicle";
	case VCSInputContext::InAircraft: return "InAircraft";
	case VCSInputContext::Aiming: return "Aiming";
	case VCSInputContext::Menu: return "Menu";
	case VCSInputContext::Unknown:
	default:
		return "Unknown";
	}
}

// Default bindings, chosen to feel like a PC GTA rather than to mirror the PSP pad. These are
// starting values - the point of the table is that changing a binding is a one-line edit here.
//
// Note that the PSP button a key maps to is only half the story; what that button *does* is up
// to the game and differs per context, which is exactly why the context column exists. The
// description says what the player should experience.
//
// Verification status matters here. Bindings marked "verified" were confirmed by holding the
// key in game and watching the character. The rest are plausible guesses at what each PSP
// button does in VCS and may well be wrong - aim was originally on L trigger and did nothing
// at all until it was checked against the real game (R trigger is aim).
//
// DO NOT bind Escape here. PPSSPP maps it to VIRTKEY_PAUSE by default (Core/KeyMapDefaults.cpp),
// and because a claimed key is withheld from PPSSPP's own mapper, binding it would swallow the
// only way to open the emulator's pause menu and effectively trap the player in the game. P is
// used for the PSP Start button instead. The same caution applies to any key PPSSPP binds to a
// VIRTKEY_ - check KeyMapDefaults.cpp before adding one.
const VCSKeyMapping kVCSKeyMappings[] = {
	// --- On foot ---
	{ VCSInputContext::OnFoot,    NKCODE_SPACE,              CTRL_SQUARE,    "Jump (vaults a ledge when there is one)", "Jump", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,    NKCODE_SHIFT_LEFT,         CTRL_CROSS,     "Sprint", "Sprint", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,    NKCODE_EXT_MOUSEBUTTON_1,  CTRL_CIRCLE,    "Attack / fire", "Fire", VCSKeyList::OnFoot },
	// Ctrl fires too, alongside the left mouse button.
	//
	// For the trackpad case, which is not a niche one: a great many laptop touchpads cannot report
	// a second button while the first is held, so RMB-to-aim plus LMB-to-fire is physically
	// unpressable on them. A keyboard alternative for the trigger costs nothing and is the whole
	// difference between the aiming work being usable on a laptop and not.
	//
	// One row per context that already binds fire, and each carries the SAME listName as the mouse
	// row it joins, so the controls card renders one "Fire" line with both keys against it rather
	// than the action twice. See KeyListing.
	//
	// NKCODE_CTRL_LEFT is the keyboard key; CTRL_LEFT elsewhere in this table is the PSP D-PAD
	// LEFT button. The two are unrelated and the names collide, which is worth a sentence here
	// because a row pairing them would look plausible and be nonsense.
	//
	// Checked against Core/KeyMapDefaults.cpp before adding: PPSSPP binds no VIRTKEY to it, so
	// claiming it takes nothing away - the trap Escape and Tab document further up.
	{ VCSInputContext::OnFoot,    NKCODE_CTRL_LEFT,          CTRL_CIRCLE,    "Attack / fire", "Fire", VCSKeyList::OnFoot },
	// Menu keys, living in the OnFoot context on purpose.
	//
	// Menus run under OnFoot, because the Menu context never resolves - it needs GameState, which
	// is still unset. So the rows further down under Menu are dead, and what actually drives a menu
	// is whatever OnFoot maps to Cross and Circle: Shift (Sprint) confirms and left-click (Attack)
	// goes back, which is what "better in menu controls" was about.
	//
	// These ADD Enter and Backspace alongside them. Shift and left-click cannot be taken away
	// without losing sprint and firing, since nothing here can tell a menu from gameplay - that
	// genuinely needs GameState. Costs: Enter also sprints and Backspace also fires, neither of
	// which collides with anything.
	{ VCSInputContext::OnFoot,    NKCODE_ENTER,              CTRL_CROSS,     "Confirm in menus (also sprint)" },
	{ VCSInputContext::OnFoot,    NKCODE_DEL,                CTRL_CIRCLE,    "Back in menus, Backspace (also fires)" },
	{ VCSInputContext::OnFoot,    kVCSAimKey,                CTRL_RTRIGGER,  "Aim (verified)", "Aim weapon", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,    NKCODE_F,                  CTRL_TRIANGLE,  "Enter vehicle", "Enter vehicle", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,    NKCODE_E,                  CTRL_RIGHT,     "Next weapon", "Next weapon", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,    NKCODE_Q,                  CTRL_LEFT,      "Previous weapon", "Previous weapon", VCSKeyList::OnFoot },
	// L trigger on foot is NOT unused, which this file claimed for a long time. Standing near a
	// dropped weapon, it switches to that weapon's type - a pickup/swap, distinct from the Q/E
	// cycle through what you already carry. Verified in game.
	//
	// Tab costs PPSSPP's fast-forward (VIRTKEY_FASTFORWARD in Core/KeyMapDefaults.cpp) - a
	// claimed key is withheld from PPSSPP's mapper, which is the inverse Escape trap, accepted
	// deliberately here. Rebind fast-forward in PPSSPP's own controls if you want it back.
	{ VCSInputContext::OnFoot,    NKCODE_TAB,                CTRL_LTRIGGER,  "Switch to nearby weapon drop (verified)", "Take nearby weapon", VCSKeyList::OnFoot },

	// --- vehicle spawner (patched-ISO feature) ---------------------------------------------------
	//
	// DBGCARS, the debug spawner the patched ISO enables, wants L HELD plus d-pad left/right. Making
	// the player hold a modifier for it is horrible, so the chord is synthesised here instead: two
	// rows for one key OR together in ComputeButtonMask, so each of these presses L and a direction
	// at once. Tab keeps its own meaning and is no longer part of the trigger.
	//
	// F9 duplicates arrow-right on purpose, and this is a limitation of the script rather than a
	// choice: DBGCARS sets its spawn state (8@ = 1) at the END of both cycle paths, so advancing the
	// selection IS the spawn. There is no "spawn what is already selected" to bind, so F9 means
	// "next vehicle, and spawn it".
	{ VCSInputContext::OnFoot,    NKCODE_F9,                 CTRL_LTRIGGER,  "Spawn next vehicle (with the row below)" },
	{ VCSInputContext::OnFoot,    NKCODE_F9,                 CTRL_RIGHT,     "Spawn next vehicle" },
	{ VCSInputContext::OnFoot,    NKCODE_DPAD_RIGHT,         CTRL_LTRIGGER,  "Spawner: next model (with the row below)" },
	{ VCSInputContext::OnFoot,    NKCODE_DPAD_RIGHT,         CTRL_RIGHT,     "Spawner: next model" },
	{ VCSInputContext::OnFoot,    NKCODE_DPAD_LEFT,          CTRL_LTRIGGER,  "Spawner: previous model (with the row below)" },
	{ VCSInputContext::OnFoot,    NKCODE_DPAD_LEFT,          CTRL_LEFT,      "Spawner: previous model" },
	{ VCSInputContext::OnFoot,    NKCODE_V,                  CTRL_SELECT,    "Change camera (verified)", "Change camera", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,    NKCODE_P,                  CTRL_START,     "Pause (Start)", "Pause", VCSKeyList::OnFoot },

	// --- In a vehicle ---
	{ VCSInputContext::InVehicle, NKCODE_W,                  CTRL_CROSS,     "Accelerate", "Accelerate", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle, NKCODE_S,                  CTRL_SQUARE,    "Brake / reverse", "Brake / reverse", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle, NKCODE_SPACE,              CTRL_RTRIGGER,  "Handbrake", "Handbrake", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle, NKCODE_F,                  CTRL_TRIANGLE,  "Exit vehicle", "Exit vehicle", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle, NKCODE_EXT_MOUSEBUTTON_1,  CTRL_CIRCLE,    "Drive-by fire", "Drive-by fire", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle, NKCODE_CTRL_LEFT,          CTRL_CIRCLE,    "Drive-by fire", "Drive-by fire", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle, NKCODE_H,                  CTRL_DOWN,      "Horn (verified)", "Horn", VCSKeyList::InVehicle },
	// These two are the radio in an ordinary vehicle - and, verified in play, the FORKS in a
	// forklift: R raises and T lowers. The game repurposes d-pad left/right there rather than
	// leaving them on the radio, so no new binding was needed; the keys already did it and
	// nobody had pressed them in one. The description below is therefore wrong for exactly one
	// vehicle, which a per-vehicle context could fix and isn't worth adding for a string.
	{ VCSInputContext::InVehicle, NKCODE_T,                  CTRL_RIGHT,     "Next radio station / raise forks (verified)", "Next radio station", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle, NKCODE_R,                  CTRL_LEFT,      "Previous radio station / lower forks (verified)", "Previous radio station", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle, NKCODE_V,                  CTRL_SELECT,    "Change camera (verified)", "Change camera", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle, NKCODE_P,                  CTRL_START,     "Pause (Start)", "Pause", VCSKeyList::InVehicle },
	// Claimed but deliberately mapped to nothing. Sprint is meaningless in a car, and leaving
	// Shift unclaimed let it fall through to PPSSPP, whose default binding is VIRTKEY_RAPID_FIRE
	// - that alternates held buttons, so it machine-gunned the accelerator and made throttle
	// stutter. This is the inverse of the Escape trap: there we stole a key PPSSPP needed, here
	// PPSSPP was stealing one we needed to be inert.
	{ VCSInputContext::InVehicle, NKCODE_SHIFT_LEFT,        0,              "Suppressed (blocks PPSSPP rapid-fire)" },

	// Q and E in a vehicle are NOT here: they are glances, which on the PSP are L trigger
	// plus a stick direction, so they need both a button and an axis. See GlanceDirection.

	// --- Helicopters and planes ---
	//
	// A separate context because the car bindings physically cannot fly. In a car W/S are
	// buttons and A/D are the stick's X axis, so nothing anywhere drives the stick's Y axis -
	// and in an aircraft Y is PITCH, which is the only thing that makes it move forward. The
	// game's own scheme (Controls screen, and the manual): Cross climbs, Square descends, L and
	// R yaw, and the nub is pitch and roll.
	//
	// Laid out like flying in GTA San Andreas: W/S climb and descend, Q/E yaw, arrow keys pitch
	// and roll. A/D roll as well, so short hops don't need the right hand to leave the mouse -
	// delete those two rows if that feels like a mistake, nothing else depends on them.
	{ VCSInputContext::InAircraft, NKCODE_W,                  CTRL_CROSS,     "Climb / throttle up", "Climb", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_S,                  CTRL_SQUARE,    "Descend / throttle down", "Descend", VCSKeyList::Aircraft },
	// Yaw. On the PSP these are the two shoulder buttons directly, NOT the glance modifier they
	// are in a car - which is why GlanceDirection deliberately answers only for InVehicle.
	{ VCSInputContext::InAircraft, NKCODE_Q,                  CTRL_LTRIGGER,  "Yaw left", "Yaw left", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_E,                  CTRL_RTRIGGER,  "Yaw right", "Yaw right", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_F,                  CTRL_TRIANGLE,  "Exit aircraft", "Exit aircraft", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_EXT_MOUSEBUTTON_1,  CTRL_CIRCLE,    "Fire (Hunter)", "Fire (Hunter)", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_CTRL_LEFT,          CTRL_CIRCLE,    "Fire (Hunter)", "Fire (Hunter)", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_T,                  CTRL_RIGHT,     "Next radio station", "Next radio station", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_R,                  CTRL_LEFT,      "Previous radio station", "Previous radio station", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_V,                  CTRL_SELECT,    "Change camera", "Change camera", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_P,                  CTRL_START,     "Pause (Start)", "Pause", VCSKeyList::Aircraft },
	// Claimed and inert, all for the inverse-Escape-trap reason rather than for anything they do
	// here. Shift would otherwise reach PPSSPP's rapid-fire and stutter the climb button exactly
	// as it stuttered the throttle in a car. Space is handbrake in a car, which in an aircraft
	// would land on R trigger and yaw the nose right whenever someone reached for a brake that
	// doesn't exist. A and D are claimed because PPSSPP's defaults map them to Square and
	// Triangle - descend and BAIL OUT - so leaving them unclaimed is worse than dead keys; they
	// roll via ApplyAnalog rather than through this table.
	{ VCSInputContext::InAircraft, NKCODE_SHIFT_LEFT,         0,              "Suppressed (blocks PPSSPP rapid-fire)" },
	{ VCSInputContext::InAircraft, NKCODE_SPACE,              0,              "Suppressed (no handbrake in the air)" },
	{ VCSInputContext::InAircraft, NKCODE_A,                  0,              "Roll left (via the stick)", "Roll left", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_D,                  0,              "Roll right (via the stick)", "Roll right", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_DPAD_UP,            0,              "Pitch nose down (via the stick)", "Pitch nose down", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_DPAD_DOWN,          0,              "Pitch nose up (via the stick)", "Pitch nose up", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_DPAD_LEFT,          0,              "Roll left (via the stick)", "Roll left", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_DPAD_RIGHT,         0,              "Roll right (via the stick)", "Roll right", VCSKeyList::Aircraft },

	// --- Aiming (on foot, aim key held) ---
	// Separate from OnFoot because the same inputs mean different things here: the mouse becomes
	// the reticle instead of the camera, WASD goes quiet because the stick is now the reticle, and
	// Q/E cycle targets instead of weapons.
	{ VCSInputContext::Aiming,    NKCODE_EXT_MOUSEBUTTON_1,  CTRL_CIRCLE,    "Fire", "Light attack / fire", VCSKeyList::Melee },
	{ VCSInputContext::Aiming,    NKCODE_CTRL_LEFT,          CTRL_CIRCLE,    "Fire", "Light attack / fire", VCSKeyList::Melee },
	{ VCSInputContext::Aiming,    kVCSAimKey,                CTRL_RTRIGGER,  "Hold aim (verified)" },
	// Same as on foot. Bound here mainly so Tab can't fall through to PPSSPP's fast-forward
	// mid-fight, which would suddenly run the game at several times speed while aiming.
	{ VCSInputContext::Aiming,    NKCODE_TAB,                CTRL_LTRIGGER,  "Switch to nearby weapon drop (verified)" },
	{ VCSInputContext::Aiming,    NKCODE_P,                  CTRL_START,     "Pause (Start)" },
	// --- Hand-to-hand combat, i.e. the four face buttons while targeting ---
	//
	// These need no melee-specific detection, and that is the whole reason they fit here as three
	// rows rather than as a context. VCS decides what each button means from the state it can see -
	// targeting, holding someone from the front or the rear, standing over a prone or a dead body -
	// so one key per PSP button covers all five states:
	//
	//   |          | targeting     | held, front  | held, rear | prone      | dead  |
	//   | Circle   | light, 4 hits | jab, 2 hits  | jab        | floor hits | stomp |
	//   | Cross    | heavy, 2 hits | heavy / K.O. | knee       | stomp      | -     |
	//   | Triangle | grab          | throw        | neckbreak  | pull up    | -     |
	//   | Square   | block         | -            | -          | -          | -     |
	//
	// Circle is already bound above (left mouse, "Fire"), and Q/E already cycle targets, so only
	// Cross, Square and Triangle were missing.
	//
	// Every one of the three keeps the PSP button it already has on foot - Shift is Cross (sprint),
	// Space is Square (jump), F is Triangle (enter vehicle). One key means one button everywhere and
	// the game reinterprets it, which is exactly the split the context column exists for, and it is
	// the reason none of these needed a key that isn't already spoken for.
	//
	// Sprint, jump and enter-vehicle are all unreachable while targeting, so nothing is lost by the
	// sharing.
	//
	// Shift was a psp = 0 claim before, purely to keep PPSSPP's rapid-fire (VIRTKEY_RAPID_FIRE on
	// left shift) off the fire button. That suppression is unaffected: claiming is what withholds a
	// key from PPSSPP's mapper, not the button the row produces.
	{ VCSInputContext::Aiming,    NKCODE_SHIFT_LEFT,        CTRL_CROSS,     "Heavy hit / stomp / knee (melee)", "Heavy hit / stomp", VCSKeyList::Melee },
	// Space was NOT claimed in this context until now, and the cost of that was not the harmless
	// fall-through it looked like: PPSSPP's default keyboard map binds Space to CTRL_START
	// (Core/KeyMapDefaults.cpp, `Start = 1-62` in memstick/PSP/SYSTEM/controls.ini), so pressing it
	// with aim held opened the PAUSE MENU. The inverse Escape trap, in the middle of a fight.
	//
	// The sniper-zoom comment further down describes this as "it only opens a menu when pressed
	// unscoped, where Square is still Jump" - that reading was wrong. The menu was Start, from
	// PPSSPP, in every case; the key never reached Square here at all, because no row sent it there.
	{ VCSInputContext::Aiming,    NKCODE_SPACE,             CTRL_SQUARE,    "Block (melee) / sniper zoom in", "Block", VCSKeyList::Melee },
	// F is free in PPSSPP's defaults, so this row is a pure gain - and "F grabs" is about as
	// idiomatic as PC bindings get. Near a vehicle while not targeting anyone, Triangle is still
	// enter-vehicle; that ambiguity is the game's own and the PSP has it too.
	{ VCSInputContext::Aiming,    NKCODE_F,                 CTRL_TRIANGLE,  "Grab / throw / neckbreak / pull up", "Grab / throw", VCSKeyList::Melee },
	// WASD claimed here with psp = 0, which looks pointless because they steer the stick rather
	// than pressing buttons - but the claim is the point. PPSSPP's default keyboard mapping binds
	// them to real PSP buttons (W to R trigger, A to Square, S to Triangle - see
	// Core/KeyMapDefaults.cpp), so any context that doesn't claim them lets them fall through and
	// makes strafing jump, enter vehicles and mash the aim trigger. The inverse Escape trap.
	//
	// The claim has to be unconditional because whether these actually steer varies FRAME BY
	// FRAME within this context: they move the player under lock-on and go quiet in free aim,
	// where the stick is the reticle instead (see ApplyAnalog). Claiming and steering are
	// separate questions, and only the second one is conditional.
	{ VCSInputContext::Aiming,    NKCODE_W,                 0,              "Strafe (quiet in free aim)" },
	{ VCSInputContext::Aiming,    NKCODE_A,                 0,              "Strafe (quiet in free aim)" },
	{ VCSInputContext::Aiming,    NKCODE_D,                 0,              "Strafe (quiet in free aim)" },
	// FREE AIM. This is d-pad DOWN, and it is not a guess - the game's own Controls screen
	// labels it, and it was verified in play: with aim held, d-pad up does nothing, left/right
	// cycle targets, and DOWN drops you into free aim, after which the mouse moves the crosshair.
	//
	// Everything else built for this - the CameraInputMode flag, writing the d-pad fields as an
	// analog axis, the CLEO plugin - was unnecessary. Free aim is a button. Read the in-game
	// Controls screen before inventing a mechanism; it documents Look/Fine Aim on L and Free Aim
	// on d-pad down, which is most of what took a day to rediscover from disassembly.
	//
	// S also still feeds the analog stick as a strafe via ApplyAnalog, so it does both. That's
	// tolerable because backing up and entering free aim are both reasonable on the same key,
	// but a dedicated key is the cleaner answer if it ever gets in the way.
	{ VCSInputContext::Aiming,    NKCODE_S,                 CTRL_DOWN,      "Enter free aim (verified)" },
	// Claimed, sends nothing. Held, it suppresses the automatic free-aim pulse and leaves the
	// PSP's lock-on - so it inverts the handheld's priorities, which is the right way round on a
	// PC. Listed here so it shows up in the debugger's mapping table rather than being invisible.
	{ VCSInputContext::OnFoot,    NKCODE_L,                 0,              "Toggle lock-on mode (manual override; melee is automatic)", "Toggle lock-on mode", VCSKeyList::OnFoot },
	{ VCSInputContext::Aiming,    NKCODE_L,                 0,              "Toggle lock-on mode (manual override; melee is automatic)" },
	{ VCSInputContext::Aiming,    NKCODE_Q,                  CTRL_LEFT,      "Previous target", "Previous target", VCSKeyList::OnFoot | VCSKeyList::Melee },
	{ VCSInputContext::Aiming,    NKCODE_E,                  CTRL_RIGHT,     "Next target", "Next target", VCSKeyList::OnFoot | VCSKeyList::Melee },
	// Sniper zoom.
	//
	// Square zooms IN and Cross zooms OUT - measured, not guessed. Each PSP button was injected
	// over the debugger while scoped and the camera block diffed: Square took the FOV at
	// CCamera+0x198 from 70 to 33 with the zoom level at +0x7a0 stepping 1.0 to 2.0, and Cross
	// reversed it. Nothing else moved.
	//
	// The first attempt bound d-pad up/down, on the reasoning that GTA usually puts scope zoom
	// there. It was already the aim axis in free aim - which had been reported in play - so the
	// one pair that could not possibly work is the pair that got chosen. Injecting buttons and
	// watching what moves took two minutes and needed no priors at all.
	//
	// Space reaches Square here too, via the melee block above - so reaching for the obvious zoom key
	// while scoped now does zoom in, which it never did before that row existed.
	//
	// Z/Y and the wheel therefore double as block and heavy hit during a fistfight. Harmless, since
	// nobody scrolls mid-fight, and the alternative - gating these rows on ScopedWeaponActive - is
	// not something a static table can express.
	{ VCSInputContext::Aiming,    NKCODE_Z,                 CTRL_SQUARE,    "Sniper zoom in (verified)", "Sniper zoom in", VCSKeyList::OnFoot },
	{ VCSInputContext::Aiming,    NKCODE_Y,                 CTRL_CROSS,     "Sniper zoom out (verified)", "Sniper zoom out", VCSKeyList::OnFoot },
	{ VCSInputContext::Aiming,    NKCODE_EXT_MOUSEWHEEL_UP,   CTRL_SQUARE,  "Sniper zoom in (verified)", "Sniper zoom in", VCSKeyList::OnFoot },
	{ VCSInputContext::Aiming,    NKCODE_EXT_MOUSEWHEEL_DOWN, CTRL_CROSS,   "Sniper zoom out (verified)", "Sniper zoom out", VCSKeyList::OnFoot },

	// --- Menus / pause screens ---
	// Keyboard navigation, so the player never has to think in PSP buttons.
	{ VCSInputContext::Menu,      NKCODE_ENTER,              CTRL_CROSS,     "Confirm" },
	{ VCSInputContext::Menu,      NKCODE_DEL,                CTRL_CIRCLE,    "Back (Backspace)" },
	{ VCSInputContext::Menu,      NKCODE_W,                  CTRL_UP,        "Up" },
	{ VCSInputContext::Menu,      NKCODE_S,                  CTRL_DOWN,      "Down" },
	{ VCSInputContext::Menu,      NKCODE_A,                  CTRL_LEFT,      "Left" },
	{ VCSInputContext::Menu,      NKCODE_D,                  CTRL_RIGHT,     "Right" },
	{ VCSInputContext::Menu,      NKCODE_DPAD_UP,            CTRL_UP,        "Up" },
	{ VCSInputContext::Menu,      NKCODE_DPAD_DOWN,          CTRL_DOWN,      "Down" },
	{ VCSInputContext::Menu,      NKCODE_DPAD_LEFT,          CTRL_LEFT,      "Left" },
	{ VCSInputContext::Menu,      NKCODE_DPAD_RIGHT,         CTRL_RIGHT,     "Right" },

	// WASD is NOT in this table on purpose - it drives the analog stick, not buttons, and what
	// it means changes per context. See ApplyAnalog.
	// TODO(task 3): direct weapon selection (number keys) can't be expressed here either. It
	// needs VCSAddr::WeaponIndex, and most likely a memory write rather than a button press,
	// since the PSP only has cycle-next/cycle-previous.
};

const size_t kVCSKeyMappingCount = ARRAY_SIZE(kVCSKeyMappings);

// --- The read-only controls listing ---------------------------------------------------------
//
// Rows the listing needs that kVCSKeyMappings cannot hold, because they are not buttons: WASD
// steers the analog stick and its meaning changes per context (see ApplyAnalog), and the mouse
// drives the camera through VCSCamera rather than through sceCtrl at all. Leaving them out
// would produce a controls screen that never mentions how to walk or look, so they are listed
// here - as text only, with nothing reading them but the menu.
//
// Everything else on the listing comes from the mapping table itself, so the keys a page shows
// are the keys that page's rows actually produce. That is the whole reason for the listName
// column: a second hand-written table of bindings would drift the first time one changed.
struct VCSListingExtra {
	VCSKeyList list;
	const char *name;
	const char *keys[3];
	// Movement belongs at the top of a page and the odd-job keys at the bottom, and neither has
	// a place in the mapping table's order to be interleaved with. One bit is enough to say
	// which end.
	bool atEnd;
};

static const VCSListingExtra kVCSListingExtras[] = {
	{ VCSKeyList::OnFoot,    "Forward",        { "W" } },
	{ VCSKeyList::OnFoot,    "Backwards",      { "S" } },
	{ VCSKeyList::OnFoot,    "Left",           { "A" } },
	{ VCSKeyList::OnFoot,    "Right",          { "D" } },
	{ VCSKeyList::OnFoot,    "Look",           { "MOUSE" } },

	{ VCSKeyList::InVehicle, "Steer left",     { "A" } },
	{ VCSKeyList::InVehicle, "Steer right",    { "D" } },
	{ VCSKeyList::InVehicle, "Look",           { "MOUSE" } },
	// Glances, for shooting out of the side of a car. Not in the mapping table because on the
	// PSP they are L trigger plus a stick direction - a button and an axis at once, which a row
	// there cannot express. See GlanceDirection.
	{ VCSKeyList::InVehicle, "Look left",      { "Q" } },
	{ VCSKeyList::InVehicle, "Look right",     { "E" } },
	// The same two keys as the radio above, which is the game's doing rather than ours: a
	// special vehicle reuses d-pad left and right for its own function. Listed separately
	// because one row cannot carry both meanings, and a player in a forklift is not looking for
	// the radio.
	{ VCSKeyList::InVehicle, "Raise forks / turret", { "T" }, true },
	{ VCSKeyList::InVehicle, "Lower forks / turret", { "R" }, true },

	{ VCSKeyList::Aircraft,  "Pitch and roll", { "MOUSE" } },

	{ VCSKeyList::Melee,     "Face target",    { "MOUSE" } },
};

// Short and upper case, the way the game sets them. PPSSPP's own names are the fallback and are
// mostly right once upper-cased; these are the ones where they are not - nobody calls the left
// mouse button "MB1".
std::string KeyDisplayName(InputKeyCode key) {
	switch (key) {
	case NKCODE_EXT_MOUSEBUTTON_1: return "LMB";
	case NKCODE_EXT_MOUSEBUTTON_2: return "RMB";
	case NKCODE_EXT_MOUSEBUTTON_3: return "MMB";
	case NKCODE_EXT_MOUSEWHEEL_UP: return "MS WHEEL UP";
	case NKCODE_EXT_MOUSEWHEEL_DOWN: return "MS WHEEL DN";
	default:
		break;
	}
	std::string name = KeyMap::GetKeyName(key);
	for (char &c : name) {
		c = toupper((unsigned char)c);
	}
	return name;
}

std::vector<VCSListingRow> KeyListing(VCSKeyList list) {
	std::vector<VCSListingRow> rows;
	if (list == VCSKeyList::None) {
		return rows;
	}

	auto addExtras = [&rows, list](bool atEnd) {
		for (const VCSListingExtra &extra : kVCSListingExtras) {
			if (!(extra.list & list) || extra.atEnd != atEnd) {
				continue;
			}
			VCSListingRow row;
			row.name = extra.name;
			for (const char *key : extra.keys) {
				if (key) {
					row.keys.push_back(key);
				}
			}
			rows.push_back(row);
		}
	};

	addExtras(false);

	for (size_t i = 0; i < kVCSKeyMappingCount; i++) {
		const VCSKeyMapping &mapping = kVCSKeyMappings[i];
		if (!(mapping.list & list) || !mapping.listName) {
			continue;
		}

		// Rows sharing a name are one line with several keys against it - which is how the
		// listing shows that Fire is the left mouse button and Backspace, without either of
		// them being written down twice.
		VCSListingRow *row = nullptr;
		for (VCSListingRow &existing : rows) {
			if (!strcmp(existing.name, mapping.listName)) {
				row = &existing;
				break;
			}
		}
		if (!row) {
			rows.push_back(VCSListingRow{mapping.listName, {}});
			row = &rows.back();
		}

		// The same key can reach one action through two contexts - Q cycles weapons on foot and
		// targets while aiming, and the aim key appears in both the OnFoot and Aiming rows. Show
		// it once.
		const std::string name = KeyDisplayName(mapping.key);
		if (std::find(row->keys.begin(), row->keys.end(), name) == row->keys.end()) {
			row->keys.push_back(name);
		}
	}

	addExtras(true);
	return rows;
}

VCSInputContext ResolveContext(const VCSState &state) {
	// With an empty address table there is nothing to base a decision on, and guessing would
	// mean remapping the player's buttons at random. Stay out of the way instead.
	if (!HasContextAddresses() || !state.anyValid) {
		return VCSInputContext::Unknown;
	}

	// TODO: needs VCSAddr::GameState - check it here and return Menu before anything else, once
	// the menu/gameplay values are known. Until then a paused game looks like normal gameplay
	// and gets gameplay bindings.

	if (state.inVehicle.value_or(false)) {
		// A helicopter or plane gets its own bindings. Anything else - including a vehicle whose
		// model we failed to read - falls back to the car set, which is what this layer assumed
		// for every vehicle before the model id was found. An unreadable model must never leave
		// the player with no bindings at all.
		if (VehicleClassIsAircraft(state.vehicleClass)) {
			return VCSInputContext::InAircraft;
		}
		return VCSInputContext::InVehicle;
	}

	if (state.onFoot.value_or(false)) {
		// Aiming is a sub-state of being on foot, so it has to be checked second.
		//
		// Entered by the player HOLDING the aim key, not by the game's own IsAiming flag. That
		// looks backwards - we have a verified address that says whether the game thinks it is
		// aiming - so the reasoning matters:
		//
		//  - IsAiming means LOCKED ON, which is the wrong mode. Under lock-on the game aims for
		//    you and the stick strafes around the target. Handing the stick to the mouse there
		//    would make moving the mouse sidestep the character. The mode this layer is
		//    reproducing - nub places the reticle, R fires along it - is the free-aim one.
		//  - IsFreeAiming, which is that mode, can't drive the context. It also reads 1 during
		//    cutscenes (most likely it means something broader like "player control
		//    restricted"), and using it broke cutscene skipping. See docs/VCS_ADDRESSES.md.
		//  - Holding the key is the player's actual intent, which is the thing hold-to-aim is
		//    supposed to express, and it needs no address at all - so aiming keeps working on a
		//    build where none of the aim flags were ever found.
		//  - It has no latency. A flag only flips once the game has decided; the key is known on
		//    the frame it goes down.
		//
		// Both aim flags are still read into VCSState for the debugger, and remain the way to
		// find out what the game actually did with the input we sent it.
		if (IsHostKeyDown(kVCSAimKey)) {
			return VCSInputContext::Aiming;
		}
		return VCSInputContext::OnFoot;
	}

	return VCSInputContext::Unknown;
}

// Assumes g_hostKeyMutex is held.
static bool IsHostKeyDownLocked(InputKeyCode key) {
	return g_heldKeys.find(key) != g_heldKeys.end();
}

// Assumes g_hostKeyMutex is held. Whether this context maps this key at all.
static bool ContextMapsKeyLocked(VCSInputContext context, InputKeyCode key) {
	for (size_t i = 0; i < kVCSKeyMappingCount; i++) {
		if (kVCSKeyMappings[i].context == context && kVCSKeyMappings[i].key == key) {
			return true;
		}
	}
	return false;
}

// Vehicle glance keys. Q looks to the passenger side, E to the driver side, both together look
// back at the front of the car. These are camera rotations, not buttons - no PSP button means
// "rotate the view 90 degrees" - so they are handled by VCSCamera, not the mapping table.
//
// Vehicle glances use the game's OWN look mechanic rather than moving the camera ourselves:
// hold L trigger and push the analog stick, exactly as the PSP does it.
//
//   L + stick left   -> look left        (Q)
//   L + stick right  -> look right       (E)
//   L + stick up     -> look forward     (Q and E together)
//
// This is much better than writing CameraYaw directly, which is what an earlier version did.
// Going through the real mechanic means drive-by shooting works for free: holding a glance and
// pressing Circle fires to that side, because that is simply what the game does with those
// inputs. Writing the camera angle could never have produced that.
//
// It also explains why the L trigger looked like it did nothing in the button tester - it is a
// modifier, inert on its own, and only means anything combined with a stick direction.
static bool IsGlanceKey(InputKeyCode key) {
	return key == NKCODE_Q || key == NKCODE_E;
}

// Resolves the held glance keys into a stick direction. Returns false when no glance is active.
// Assumes g_hostKeyMutex is NOT held.
static bool GlanceDirection(VCSInputContext context, float *x, float *y) {
	*x = 0.0f;
	*y = 0.0f;
	if (context != VCSInputContext::InVehicle) {
		return false;
	}

	bool q, e;
	{
		std::lock_guard<std::mutex> guard(g_hostKeyMutex);
		q = IsHostKeyDownLocked(NKCODE_Q);
		e = IsHostKeyDownLocked(NKCODE_E);
	}

	if (q && e) {
		// Stick DOWN, not up. Up is "look forward", which is where the chase camera already
		// points, so it produced no visible change at all. Seeing the front of the car means
		// swinging the camera ahead of it and looking back, which is the backward direction.
		*y = -1.0f;
	} else if (q) {
		*x = -1.0f;     // stick left
	} else if (e) {
		*x = 1.0f;      // stick right
	} else {
		return false;
	}
	return true;
}

// Whether this context steers the analog stick with this key. Separate from the button table
// because these produce an axis, not a button - see the comment on ApplyAnalog.
static bool ContextUsesKeyForMovement(VCSInputContext context, InputKeyCode key) {
	switch (context) {
	case VCSInputContext::OnFoot:
		return key == NKCODE_W || key == NKCODE_A || key == NKCODE_S || key == NKCODE_D;
	// Aiming is deliberately absent even though WASD usually DOES steer there. Whether it steers
	// flips with free aim, which changes mid-context, and this function is also called from the
	// input thread - where reading the decoded game state would be a race. The psp = 0 rows in
	// the table cover the claim unconditionally instead, which is the answer that never varies.
	case VCSInputContext::InVehicle:
		// W/S are handled as buttons by the mapping table, so only steering here.
		return key == NKCODE_A || key == NKCODE_D;
	default:
		return false;
	}
}

void SetHostKeyDown(InputKeyCode key, bool down) {
	std::lock_guard<std::mutex> guard(g_hostKeyMutex);
	if (down) {
		g_heldKeys.insert(key);
	} else {
		g_heldKeys.erase(key);
	}
}

bool HandleHostKey(const KeyInput &key) {
	// Cheapest gate first, and the one that guarantees no effect on any other game.
	if (!IsActive()) {
		return false;
	}

	// This is a keyboard and mouse layer. A gamepad should keep going through PPSSPP's own
	// mapper untouched, so don't claim anything from one.
	if (key.deviceId != DEVICE_ID_KEYBOARD && key.deviceId != DEVICE_ID_MOUSE) {
		return false;
	}

	// CHAR events carry a unicode codepoint in the same union as keyCode, not a key code, so
	// reading keyCode from them would be nonsense. They also have neither DOWN nor UP set.
	if (key.flags & KeyInputFlags::CHAR) {
		return false;
	}

	const bool down = (key.flags & KeyInputFlags::DOWN) != 0;
	const bool up = (key.flags & KeyInputFlags::UP) != 0;
	if (!down && !up) {
		return false;
	}

	const VCSInputContext context = g_currentContext.load(std::memory_order_relaxed);

	// The lock-on toggle flips on the press edge, before the context gate below - it has to work
	// even in a context that maps nothing, and it is the one key here whose whole job is to change
	// what the other mappings do.
	if (down && key.keyCode == kVCSLockOnKey) {
		g_lockOnMode.store(!g_lockOnMode.load(std::memory_order_relaxed),
			std::memory_order_relaxed);
	}

	std::lock_guard<std::mutex> guard(g_hostKeyMutex);
	if (down) {
		g_heldKeys.insert(key.keyCode);
	} else {
		g_heldKeys.erase(key.keyCode);
	}

	// Only claim the key if we are actually going to act on it. With an empty address table the
	// context is Unknown, nothing is mapped, and every key falls through to PPSSPP as before.
	if (context == VCSInputContext::Unknown) {
		return false;
	}
	return ContextMapsKeyLocked(context, key.keyCode) ||
	       ContextUsesKeyForMovement(context, key.keyCode) ||
	       (context == VCSInputContext::InVehicle && IsGlanceKey(key.keyCode));
}

void ResetHostKeys() {
	{
		std::lock_guard<std::mutex> guard(g_hostKeyMutex);
		g_heldKeys.clear();
	}
	// Otherwise the debugger keeps showing the last context from a game that has since stopped.
	g_currentContext.store(VCSInputContext::Unknown, std::memory_order_relaxed);
	// Anything we were holding has to be released too, or the buttons stay stuck down in the
	// game. Only touch the bits we own.
	if (g_lastAppliedMask != 0) {
		__CtrlUpdateButtons(0, g_lastAppliedMask);
		g_lastAppliedMask = 0;
	}
	if (g_analogHeld) {
		__CtrlSetAnalogXY(CTRL_STICK_LEFT, 0.0f, 0.0f);
		g_analogHeld = false;
		g_analogX = 0.0f;
		g_analogY = 0.0f;
	}
	g_analogIsReticle = false;
	if (g_aimStickHeld) {
		__CtrlSetAnalogXY(CTRL_STICK_RIGHT, 0.0f, 0.0f);
		g_aimStickHeld = false;
		g_aimStickX = 0.0f;
		g_aimStickY = 0.0f;
	}
}

bool IsHostKeyDown(InputKeyCode key) {
	std::lock_guard<std::mutex> guard(g_hostKeyMutex);
	return IsHostKeyDownLocked(key);
}

size_t HeldHostKeyCount() {
	std::lock_guard<std::mutex> guard(g_hostKeyMutex);
	return g_heldKeys.size();
}

u32 ComputeButtonMask(VCSInputContext context) {
	if (context == VCSInputContext::Unknown) {
		return 0;
	}

	// One lock for the whole table rather than one per row.
	std::lock_guard<std::mutex> guard(g_hostKeyMutex);
	u32 mask = 0;
	for (size_t i = 0; i < kVCSKeyMappingCount; i++) {
		const VCSKeyMapping &mapping = kVCSKeyMappings[i];
		if (mapping.context == context && IsHostKeyDownLocked(mapping.key)) {
			mask |= mapping.psp;
		}
	}
	return mask;
}

// Fists or a melee weapon. FreeAimActive has always asked this question; the point of hoisting it
// is that the automatic ENTRY into free aim never asked it, and with melee equipped both halves of
// that entry are wrong in a way you can see on screen:
//
//  - the sprint latch holds CTRL_CROSS for ~8 ticks, and Cross while targeting is the HEAVY HIT. So
//    starting a fight while walking forward threw an unrequested heavy punch, every time.
//  - the pulse presses d-pad Down, the game's Free Aim button, which melee has no use for.
//
// Neither could be noticed before melee had bindings, because Cross did nothing there.
//
// An unset slot means the read failed, and then the previous behaviour stands rather than a guess -
// same rule as in FreeAimActive: claiming "melee" wrongly would kill free aim for every gun.
static bool MeleeEquipped() {
	const std::optional<u32> weaponSlot = GetState().weaponIndex;
	return weaponSlot && WeaponSlotIsMelee(*weaponSlot);
}

u32 ApplyMapping(VCSInputContext context) {
	g_currentContext.store(context, std::memory_order_relaxed);

	if (context == VCSInputContext::Unknown) {
		// Release anything still held from a previous context, then do nothing further. This is
		// the path taken with an empty address table, so it has to be completely inert.
		if (g_lastAppliedMask != 0) {
			__CtrlUpdateButtons(0, g_lastAppliedMask);
			g_lastAppliedMask = 0;
		}
		return 0;
	}

	// A vault owns the character for its whole duration, so the game is sent nothing at all.
	//
	// Not just the jump that started it: the motion is a written position, and every button that
	// could move the player - sprint, jump, enter vehicle - is the game trying to move someone who
	// is already being moved. Releasing what we held rather than freezing it is the same discipline
	// the Unknown branch above follows.
	if (VaultInProgress()) {
		if (g_lastAppliedMask != 0) {
			__CtrlUpdateButtons(0, g_lastAppliedMask);
			g_lastAppliedMask = 0;
		}
		g_prevAppliedContext = context;
		return 0;
	}

	// Arm the auto-free-aim pulse on the edge into Aiming - not while in it, or it would retrigger
	// every frame and hold d-pad down forever.
	if (context == VCSInputContext::Aiming && g_prevAppliedContext != VCSInputContext::Aiming) {
		if (CameraSettings().autoFreeAim && !g_lockOnMode.load(std::memory_order_relaxed) &&
			!MeleeEquipped()) {
			// If the player is already asking to move, spend a few ticks establishing a running
			// state before pressing Free Aim, so the game has something worth latching. The pulse
			// is armed when that finishes rather than now.
			if (CameraSettings().moveInFreeAim && MovementKeysHeld()) {
				g_latchTimer = kLatchStart;
				g_freeAimTimer = 0;
			} else {
				// Delay + pulse. A delay of 0 presses Free Aim on the very same tick the aim
				// trigger goes down, which is the least lock-on the game can be given.
				int delay = CameraSettings().aimFreeAimDelay;
				if (delay < 0) delay = 0;
				g_freeAimTimer = delay + kFreeAimPulse;
			}
		}
	} else if (context != VCSInputContext::Aiming) {
		// Letting go of aim cancels a pulse in flight, so releasing early can't leave d-pad down
		// asserted into whatever context comes next. Same for the entry sequence.
		g_freeAimTimer = 0;
		g_latchTimer = 0;
	}
	g_prevAppliedContext = context;

	u32 setMask = ComputeButtonMask(context) | GlanceButtonMask(context) | g_forcedButtons;

	// The entry sequence, before the Free Aim press: sprint into a run, then arm the pulse.
	if (g_latchTimer > 0) {
		g_latchTimer--;
		if (g_latchTimer > kLatchDropSprintAt) {
			setMask |= CTRL_CROSS;   // sprint on foot
		}
		if (g_latchTimer == 0) {
			g_freeAimTimer = kFreeAimPulse;
		}
	}

	if (g_freeAimTimer > 0) {
		g_freeAimTimer--;
		if (g_freeAimTimer < kFreeAimPulse) {
			setMask |= CTRL_DOWN;
		}
	}

	// Release only what WE pressed last frame - never the whole set of buttons this context
	// could produce.
	//
	// Clearing the full context-owned mask was a real bug: it wiped buttons every frame even
	// when we had not set them, so anything driven through PPSSPP's own mapper or a real pad
	// that happened to share a button with us (Start, Cross, Circle...) was cancelled within a
	// frame. That broke cutscene skipping and made the controls feel generally broken.
	//
	// g_lastAppliedMask is sufficient anyway: it already covers a context change, because the
	// previous context's presses are exactly what is in it. __CtrlUpdateButtons clears before it
	// sets, so a bit in both masks still ends up set.
	const u32 clearMask = g_lastAppliedMask & ~setMask;

	if (setMask != 0 || clearMask != 0) {
		__CtrlUpdateButtons(setMask, clearMask);
	}

	g_lastAppliedMask = setMask;
	return setMask;
}

u32 GlanceButtonMask(VCSInputContext context) {
	float x, y;
	// The stick direction is applied by ApplyAnalog; the L trigger half is a button, so it has
	// to come through the normal mask path to be set and released cleanly.
	return GlanceDirection(context, &x, &y) ? CTRL_LTRIGGER : 0;
}

void ApplyAnalog(VCSInputContext context) {
	float x = 0.0f;
	float y = 0.0f;

	// The stick goes with the buttons during a vault - see ApplyMapping. A held W would otherwise
	// walk the player forward through the wall he is being lifted over.
	if (VaultInProgress()) {
		if (g_analogHeld) {
			__CtrlSetAnalogXY(CTRL_STICK_LEFT, 0.0f, 0.0f);
			g_analogHeld = false;
			g_analogX = 0.0f;
			g_analogY = 0.0f;
		}
		g_analogIsReticle = false;
		return;
	}

	// A glance takes the stick over completely: on the PSP you steer with the stick, and while
	// L is held that same stick chooses the look direction instead. Reproducing that ordering is
	// what makes L + direction + Circle fire a drive-by to the correct side.
	if (GlanceDirection(context, &x, &y)) {
		__CtrlSetAnalogXY(CTRL_STICK_LEFT, x, y);
		g_analogHeld = true;
		g_analogIsReticle = false;
		AimModelReset();
		g_analogX = x;
		g_analogY = y;
		return;
	}

	if (ReticleActive(context)) {
		// The reticle. In free aim the stick stops being movement and becomes "where the
		// crosshair goes", which is what the game shoots along - so this is the one place the
		// mouse has to end up on the stick rather than on the camera address.
		//
		// Note this is gated on ReticleActive, NOT on the Aiming context. Holding aim in VCS
		// normally gives lock-on, where the stick strafes around the target - putting the mouse
		// on it there makes the player sidestep when you move the mouse, with no crosshair
		// anywhere in sight. Confirmed in play, so don't relax this back to a context check.
		//
		// The nub is a RATE control, and the game's response to it is a square with a smoother on
		// top, so feeding it the mouse delta proportionally cannot feel like a mouse no matter how
		// it is scaled - that was the old behaviour and it is what "feels like a thumbstick"
		// described. AimDeflectionFromMouse asks for a rotation instead and works backwards to the
		// deflection that produces it. See the aim response model in VCSCamera.cpp.
		//
		// Mouse dy is positive DOWNWARD (screen coordinates) while the PSP stick's positive Y is
		// away from the camera, so the natural "push the mouse away, raise the crosshair" mapping
		// needs a negation. aimInvertY flips away from that, it isn't the default - the model does
		// both, since it has to know the signs to cancel the smoother correctly.
		g_analogIsReticle = true;

		if (!AimModelReady()) {
			// The game has not run a logic frame since the last solve - we tick at ~60Hz and it
			// runs at 30fps. Leave the mouse movement accumulating and re-assert what we wrote
			// last time, which is what the game is going to read anyway. Draining here would spend
			// movement on a write the next tick overwrites before the game ever sees it.
			AimLastDeflection(&x, &y);
		} else {
			float dx = 0.0f, dy = 0.0f;
			TakeMouseDelta(&dx, &dy);

			g_reticleFrames++;
			if (dx != 0.0f || dy != 0.0f) {
				g_reticleNonZero++;
			}

			const VCSCameraSettings &s = CameraSettings();
			AimDeflectionFromMouse(dx, dy, s.aimSensitivity, s.aimInvertY, &x, &y);
		}
	} else if (CameraAimActive(context)) {
		// Free aim with the camera driven directly: the stick must stay CENTRED. It is the game's
		// aim axis here, so letting WASD onto it would sweep the crosshair sideways while the mouse
		// was also moving it - two things aiming at once. Movement is not lost by this; it comes
		// from the latch, see MoveGateBranch.
		g_analogIsReticle = false;
		AimModelReset();
		if (g_analogHeld) {
			__CtrlSetAnalogXY(CTRL_STICK_LEFT, 0.0f, 0.0f);
			g_analogHeld = false;
			g_analogX = 0.0f;
			g_analogY = 0.0f;
		}
		return;
	} else {
		g_analogIsReticle = false;
		// The model keeps state between frames, so it has to be dropped when aiming stops -
		// otherwise the next free aim opens by cancelling a glide that ended long ago, and jumps.
		//
		// But "the reticle isn't driving" is no longer the same as "nothing is aiming". With the
		// d-pad as the aim channel the reticle stands down permanently, and resetting here would
		// wipe the model on every single frame, moments before PadStickTick asks it for a
		// deflection. Only reset when neither aim channel is running.
		if (!PadStickActive(context)) {
			AimModelReset();
		}
		std::lock_guard<std::mutex> guard(g_hostKeyMutex);
		switch (context) {
		case VCSInputContext::OnFoot:
		// Aiming without free aim is lock-on, where the stick strafes around the target - which
		// is movement, so WASD drives it exactly as on foot. This is the common case: every
		// ordinary weapon in VCS aims this way.
		case VCSInputContext::Aiming:
			if (IsHostKeyDownLocked(NKCODE_D)) x += 1.0f;
			if (IsHostKeyDownLocked(NKCODE_A)) x -= 1.0f;
			// Positive Y is "away from the camera" on the PSP stick - see __CtrlSetAnalogXY,
			// which negates before scaling. So W is +1, not -1.
			if (IsHostKeyDownLocked(NKCODE_W)) y += 1.0f;
			if (IsHostKeyDownLocked(NKCODE_S)) y -= 1.0f;
			break;
		case VCSInputContext::InVehicle:
			// Steering only. Accelerate and brake come from the button table.
			if (IsHostKeyDownLocked(NKCODE_D)) x += 1.0f;
			if (IsHostKeyDownLocked(NKCODE_A)) x -= 1.0f;
			break;
		case VCSInputContext::InAircraft:
			// Pitch and roll - the whole reason this context exists. In a car the stick is
			// steering and Y is unused; in the air X banks and Y pitches, and pitch is what
			// converts the rotor's lift into forward flight. W/S stay on the buttons.
			//
			// Positive Y is away from the camera, i.e. the nub pushed forward, which pitches the
			// NOSE DOWN and flies forward - so the up arrow is +1, matching "press up to go
			// forward" rather than an aeroplane yoke.
			if (IsHostKeyDownLocked(NKCODE_DPAD_RIGHT) || IsHostKeyDownLocked(NKCODE_D)) x += 1.0f;
			if (IsHostKeyDownLocked(NKCODE_DPAD_LEFT) || IsHostKeyDownLocked(NKCODE_A)) x -= 1.0f;
			if (IsHostKeyDownLocked(NKCODE_DPAD_UP)) y += 1.0f;
			if (IsHostKeyDownLocked(NKCODE_DPAD_DOWN)) y -= 1.0f;
			break;
		default:
			break;
		}
	}

	const bool wantsStick = x != 0.0f || y != 0.0f;

	if (!wantsStick) {
		// Release exactly once, then stop touching the stick. Writing zeroes every frame would
		// fight a real pad (and PPSSPP's own mapper) whenever the keyboard isn't in use.
		//
		// For the reticle this is reached once the model has nothing left to do - the mouse is
		// still AND its correction for the game's smoother has finished. It is deliberately not
		// reached on the first still frame, which is when that correction happens.
		if (g_analogHeld) {
			__CtrlSetAnalogXY(CTRL_STICK_LEFT, 0.0f, 0.0f);
			g_analogHeld = false;
			g_analogX = 0.0f;
			g_analogY = 0.0f;
		}
		return;
	}

	if (g_analogIsReticle) {
		// Clamp each axis on its own. Do NOT normalise the pair here: the model solves X and Y
		// independently against what the game will do with each, so scaling one because of the
		// other makes both wrong and leaves the model's idea of the game's state out of step with
		// the game. The nub cannot go past full deflection anyway - sceCtrl clamps it.
		if (x > 1.0f) x = 1.0f;
		if (x < -1.0f) x = -1.0f;
		if (y > 1.0f) y = 1.0f;
		if (y < -1.0f) y = -1.0f;
	} else {
		// Normalise diagonals, or holding W+D would move the player ~41% faster than W alone.
		const float length = std::sqrt(x * x + y * y);
		if (length > 1.0f) {
			x /= length;
			y /= length;
		}
	}

	__CtrlSetAnalogXY(CTRL_STICK_LEFT, x, y);
	g_analogHeld = true;
	g_analogX = x;
	g_analogY = y;
}

bool PluginAimActive(VCSInputContext context) {
	return context == VCSInputContext::Aiming && CameraSettings().aimViaRightStick;
}

void ApplyAimStick(VCSInputContext context) {
	if (!PluginAimActive(context)) {
		// Release exactly once, then leave the stick alone - same discipline as ApplyAnalog.
		if (g_aimStickHeld) {
			__CtrlSetAnalogXY(CTRL_STICK_RIGHT, 0.0f, 0.0f);
			g_aimStickHeld = false;
			g_aimStickX = 0.0f;
			g_aimStickY = 0.0f;
		}
		return;
	}

	float dx = 0.0f, dy = 0.0f;
	TakeMouseDelta(&dx, &dy);

	const VCSCameraSettings &s = CameraSettings();
	float x = dx * s.aimSensitivity;
	float y = -dy * s.aimSensitivity * (s.aimInvertY ? -1.0f : 1.0f);

	if (x == 0.0f && y == 0.0f) {
		if (g_aimStickHeld) {
			__CtrlSetAnalogXY(CTRL_STICK_RIGHT, 0.0f, 0.0f);
			g_aimStickHeld = false;
			g_aimStickX = 0.0f;
			g_aimStickY = 0.0f;
		}
		return;
	}

	const float length = std::sqrt(x * x + y * y);
	if (length > 1.0f) {
		x /= length;
		y /= length;
	}

	__CtrlSetAnalogXY(CTRL_STICK_RIGHT, x, y);
	g_aimStickHeld = true;
	g_aimStickX = x;
	g_aimStickY = y;
}

void GetAppliedAimStick(float *x, float *y) {
	*x = g_aimStickX;
	*y = g_aimStickY;
}

// Sniper and RPG - the manually-aimed, scoped weapons. Weapon camera modes 7 and 8, measured.
bool ScopedWeaponActive() {
	const std::optional<u32> mode = ReadAddrU32(VCSAddr::WeaponCamMode);
	return mode && (*mode == 7 || *mode == 8);
}

// Whether the mouse should steer the CAMERA rather than the stick.
//
// For scoped weapons this is not just better, it is correct. Looking down a scope means the camera
// direction IS the firing direction - there is no separate ped aim state to desynchronise from, so
// writing CameraYaw moves the shot as well as the view. Confirmed in play: sniper and RPG aim
// beautifully this way.
//
// That is exactly why it FAILS for everything else. In third person the crosshair is drawn from the
// camera while the shot comes from the ped, and steering only the camera splits them - it looks
// right and misses. Same write, opposite outcome, decided entirely by whether the weapon has a
// scope. See the disproven note on mouseLookInFreeAim.
static bool CameraAimActive(VCSInputContext context) {
	if (!FreeAimActive(context)) {
		return false;
	}
	if (CameraSettings().mouseLookInFreeAim) {
		return true;   // the manual override; see the flag's own comment
	}
	// THE FIRE-SITE HOOK UN-DISPROVES mouseLookInFreeAim, and this is where that lands.
	//
	// Driving the camera in free aim was rejected because the crosshair moved and the shot did
	// not - "it looks correct and misses". That verdict was correct and CONDITIONAL: it depended
	// on the shot being resolved from the ped's aim state rather than from the camera. The hook
	// resolves it from the camera, so the condition is gone.
	//
	// This is the payoff of the whole fire-site effort, and without it the hook changes nothing a
	// player can feel: the bullet followed the camera, but the mouse was still spent on the stick,
	// so aiming still went through the game's squared integrator and its smoother. Writing the
	// angle directly is what makes free aim feel like mouse look - a position control with nothing
	// in between - which is the complaint that outlived every other one.
	//
	// Gated on the hook being INSTALLED as well as enabled. If the patch did not land, the shot is
	// still resolved the old way and driving the camera would reintroduce the exact desync that
	// got this disproved in the first place.
	if (FireHookInstalled() && FireHookSettings().enabled) {
		return true;
	}
	return CameraSettings().aimScopedCamera && ScopedWeaponActive();
}

bool FreeAimActive(VCSInputContext context) {
	if (context != VCSInputContext::Aiming) {
		return false;
	}
	// The lock-on key means lock-on, all the way through. It already suppresses the Free Aim press;
	// without also standing the reticle down it was only half a mode - the game stayed in lock-on
	// while the mouse still took the stick, and in lock-on the stick is MOVEMENT, so the mouse
	// walked the player around and WASD did nothing.
	//
	// That half-state is exactly what melee was stuck in permanently: its lock-on does not register
	// in IsAiming (confirmed in play - the flag reads 0 while visibly locked on), so the test below
	// concluded free aim and handed over the stick. This key stays as the manual override; the
	// weapon test right below it is the automatic one.
	if (g_lockOnMode.load(std::memory_order_relaxed)) {
		return false;
	}
	// Melee has no free aim to give, so never hand it the mouse. In lock-on the stick is MOVEMENT,
	// which is precisely what made the mouse walk the player around while WASD did nothing.
	// WeaponIndex was worth finding for this. See MeleeEquipped for the unset-slot rule.
	if (MeleeEquipped()) {
		return false;
	}
	// Aim held, and the game is not steering it for you. See the routing note in ReticleActive
	// for why this keys on IsAiming rather than IsFreeAiming.
	return !GetState().isAiming.value_or(false);
}

bool ReticleActive(VCSInputContext context) {
	if (!FreeAimActive(context)) {
		return false;
	}
	// Stand down so the mouse drives CameraYaw directly, exactly as mouse look does on foot.
	//
	// This used to require AimYaw, an address that does not exist, so it never fired. The target
	// was wrong rather than missing: in free aim the crosshair follows the weapon camera's own
	// Beta, and Beta IS CameraYaw - CCam[0]+0x7c, the value mouse look already writes. There was
	// never a separate aim angle to find.
	//
	// Why bother, when the response model already linearises the stick: the stick still goes
	// through the game's integrator, and no amount of inverting that produces the same feel as
	// writing the angle. Mouse look is smooth because it is a position control with nothing in
	// between. This makes free aim one too.
	if (CameraAimActive(context)) {
		return false;
	}
	// Any other aim mechanism takes precedence, and the left-stick reticle stands down. Two of
	// them running at once fight over one crosshair, and worse, whichever runs first eats the
	// mouse delta and starves the other.
	//
	// The entry sequence needs the stick for MOVEMENT, not the mouse - there has to be a running
	// player for free aim to latch. A dozen ticks of the crosshair not tracking, once per aim.
	if (g_latchTimer > 0) {
		return false;
	}
	// usePadStick DOES suppress this now, and that is the whole point of it.
	//
	// It sets CameraInputMode, which moves the aim axis onto the d-pad pair - so PadStickTick is
	// already aiming, and driving the nub from the mouse as well is redundant. Worse, it is what
	// stops the player moving: the nub is the movement channel, and while the reticle owns it,
	// WASD has nowhere to go. Confirmed in play - with the second stick on, the crosshair moved
	// from the d-pad exactly as intended and WASD did nothing at all.
	//
	// Standing down here hands the nub back to ApplyAnalog's ordinary WASD path, giving two
	// separate channels from one physical stick:
	//
	//   aim      d-pad pair, driven by the mouse    (PadStickTick)
	//   movement nub, driven by WASD                (ApplyAnalog's else branch)
	//
	// That only became useful once the movement call stopped being skipped - see MoveGateBranch.
	// Before that patch the nub had nothing to do either way, which is why suppressing this used
	// to leave free aim with no driver and was reverted.
	if (CameraSettings().usePadStick && PadStickAvailable()) {
		return false;
	}
	if (CameraSettings().aimViaRightStick) {
		return false;
	}
	// FreeAimActive above already established the state. Keyed on IsAiming - the LOCK-ON flag,
	// inverted - and deliberately not on IsFreeAiming, which only ever goes to 1 for the sniper
	// and the RPG (docs/VCS_ADDRESSES.md calls it SUSPECT for exactly this kind of reuse). The
	// ordinary free aim reached with the game's Free Aim button never sets it, so routing on it
	// meant the switch simply never fired.
	return true;
}

bool AnalogIsReticle() {
	return g_analogIsReticle;
}

void ReticleStats(u64 *frames, u64 *nonZero) {
	*frames = g_reticleFrames;
	*nonZero = g_reticleNonZero;
}

void GetAppliedAnalog(float *x, float *y) {
	*x = g_analogX;
	*y = g_analogY;
}

void SetForcedButtons(u32 mask) {
	g_forcedButtons = mask;
}

u32 GetForcedButtons() {
	return g_forcedButtons;
}

VCSInputContext GetCurrentContext() {
	return g_currentContext.load(std::memory_order_relaxed);
}

bool LockOnModeActive() {
	return g_lockOnMode.load(std::memory_order_relaxed);
}

}  // namespace VCS
