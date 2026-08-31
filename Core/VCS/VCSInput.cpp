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
#include "Common/System/System.h"
#include "Core/KeyMap.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSCheats.h"
#include "Core/VCS/VCSFrontEnd.h"
#include "Core/VCS/VCSFireHook.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSMemory.h"
#include "Core/VCS/VCSVault.h"

namespace VCS {

// The pad's own held set, its sticks, and its settings.
//
// A SECOND set rather than more entries in the keyboard's, and that is forced rather than tidy:
// PPSSPP delivers a d-pad direction and an arrow key as the same InputKeyCode, so one set would
// have the debug spawner - which is on the arrow keys - firing whenever somebody pressed a
// direction on a pad. The two devices genuinely need separate state.
//
// The sticks are atomics rather than mutex-guarded because they are two floats written whole by
// the input thread and read whole by the emu thread, with no invariant between them that a torn
// pair could break: a frame that saw a new X against an old Y would aim a fraction of a degree
// wrong, once. The held set has no such luxury - a missed insert leaves a button stuck.
static std::set<InputKeyCode> g_heldPadButtons;
static std::atomic<float> g_padLeftX{0.0f};
static std::atomic<float> g_padLeftY{0.0f};
static std::atomic<float> g_padRightX{0.0f};
static std::atomic<float> g_padRightY{0.0f};

VCSPadSettings &PadSettings() {
	// Function-local static for the same reason CameraSettings() is one: the option table points
	// into it and is itself built lazily, so a file-scope object would be racing that order.
	static VCSPadSettings settings;
	return settings;
}

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

// The pad's two. X is jump because it sits where the PSP's Square does and Square is jump; LT is
// aim because that is where a modern pad puts it. Both are named so the table rows below and the
// code that asks about them cannot drift apart - see JumpHeld and CameraDrivenAimHeld.
const InputKeyCode kVCSPadAimButton = NKCODE_BUTTON_L2;
const InputKeyCode kVCSPadJumpButton = NKCODE_BUTTON_X;

// Recruit a gang member into your group, which is the PSP's d-pad UP pressed while targeting one.
//
// Not a guess and not a button-tester finding: the game says so itself. ENGLISH.GXT carries a
// per-configuration table of control names, and for the shipping configuration `C0TGSUB` reads
// "the up button" against the help line `H_GANG1`, "To recruit henchmen into your group, target
// them and use ~TGSUB~". That table is the whole control scheme written down by the people who
// made it - see "Ask the GXT what a control is called" in CLAUDE.md before probing for the next
// one.
//
// G on the keyboard, which was the lock-on toggle until that moved to L and has been free since.
// Nothing in PPSSPP's own defaults binds it, so claiming it costs nothing - the check every new
// binding here has to pass.
const InputKeyCode kVCSRecruitKey = NKCODE_G;
const InputKeyCode kVCSPadRecruitButton = NKCODE_DPAD_UP;

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
// Defined with the rest of the pad plumbing further down; needed here because "is the player
// asking to move" is a question the free-aim latch has to ask of whichever device is in hand.
static bool PadLeftStick(float *x, float *y);

bool MovementKeysHeld() {
	if (IsHostKeyDown(NKCODE_W)) {
		return true;
	}
	// The stick pushed FORWARD, which is the pad's W. Any deflection would be the wrong test: the
	// latch exists to give the game a running state to hold on to, and a player strafing sideways
	// into free aim would latch a sidestep.
	float x = 0.0f, y = 0.0f;
	return PadLeftStick(&x, &y) && y > 0.0f;
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
	// The menu keys again, because the game's own pause menu can be opened while aim is held and
	// this context has to answer for it. Without these, Enter fell through to PPSSPP's mapper,
	// which binds it to Select - so it did nothing at all in a menu, while Shift (Cross, the
	// heavy hit here) went on confirming and made the failure look like Enter specifically being
	// broken. The cost is the one the OnFoot rows already accept: Enter also throws a heavy hit
	// and Backspace also fires, neither of which is reachable while a menu is up.
	{ VCSInputContext::Aiming,    NKCODE_ENTER,              CTRL_CROSS,     "Confirm in menus (also heavy hit)" },
	{ VCSInputContext::Aiming,    NKCODE_DEL,                CTRL_CIRCLE,    "Back in menus, Backspace (also fires)" },
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
	// Recruit, which is d-pad UP with a henchman targeted - the game's own help line, see
	// kVCSRecruitKey. It fills in one of the two `?` cells the PSP button table carried for
	// years: up on foot was never nothing, it was only ever meaningful with a target.
	//
	// In the Aiming context and nowhere else, because targeting is the whole precondition. The
	// gesture on a mouse is hold G, then hold aim: RecruitHeld stands the auto-free-aim pulse
	// down for that entry, so the game keeps the lock-on it just acquired and the held UP lands
	// on a target rather than into free aim.
	{ VCSInputContext::Aiming,    kVCSRecruitKey,            CTRL_UP,        "Recruit gang member (hold it before aim, so the lock-on survives)", "Recruit gang member", VCSKeyList::OnFoot },
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
	{ VCSInputContext::Aiming,    NKCODE_Z,                 CTRL_SQUARE,    "Scope / binocular zoom in (verified)", "Zoom in (scope, binoculars)", VCSKeyList::OnFoot },
	{ VCSInputContext::Aiming,    NKCODE_Y,                 CTRL_CROSS,     "Scope / binocular zoom out (verified)", "Zoom out (scope, binoculars)", VCSKeyList::OnFoot },
	{ VCSInputContext::Aiming,    NKCODE_EXT_MOUSEWHEEL_UP,   CTRL_SQUARE,  "Scope / binocular zoom in (verified)", "Zoom in (scope, binoculars)", VCSKeyList::OnFoot },
	{ VCSInputContext::Aiming,    NKCODE_EXT_MOUSEWHEEL_DOWN, CTRL_CROSS,   "Scope / binocular zoom out (verified)", "Zoom out (scope, binoculars)", VCSKeyList::OnFoot },

	// --- Menus / pause screens ---
	// Keyboard navigation, so the player never has to think in PSP buttons.
	// Claimed and dropped: the zoom they drive is a LATCHED press, applied from the map's own
	// tick, because a notch is far too short for a front end sampling at 30Hz to see. Leaving them
	// unclaimed would hand them to PPSSPP's mapper on top of that.
	{ VCSInputContext::Menu,      NKCODE_EXT_MOUSEWHEEL_UP,   0,             "Map zoom in (latched)" },
	{ VCSInputContext::Menu,      NKCODE_EXT_MOUSEWHEEL_DOWN, 0,             "Map zoom out (latched)" },
	{ VCSInputContext::Menu,      NKCODE_ENTER,              CTRL_CROSS,     "Confirm" },
	// Claimed and mapped to nothing, because what it does is not a PSP button at all: while the
	// game's own menu is up, Back means "back to THIS fork's menu", posted as a UI message on the
	// press edge in HandleHostKey. Sending the game its own Circle instead is what it used to do,
	// and that lifts focus back to the tab strip - leaving the player inside a menu this port
	// navigates for them, in front of a row of tabs it deliberately hides.
	{ VCSInputContext::Menu,      NKCODE_DEL,                0,              "Back to the menu (Backspace)" },
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

// The gamepad scheme. Xbox names throughout; LT and RT arrive as axes and are turned into
// NKCODE_BUTTON_L2 / R2 by HandleHostAxis before they reach this table.
//
// Read it against "What each PSP button actually does in VCS" in CLAUDE.md and the shape is
// clear: the face buttons stay where the handheld put them, because Xbox A/B/X/Y sit in the same
// four places as Cross/Circle/Square/Triangle and VCS already has the right actions on them. What
// moves is everything a PSP had nowhere to put - aim and fire onto the triggers, the camera onto
// a stick, the glances onto the bumpers - and the d-pad, which stops being four unrelated
// functions and becomes weapons on the horizontal and view controls on the vertical.
//
// EVERY BUTTON IS CLAIMED IN EVERY CONTEXT, mapped or not, and the psp = 0 rows are not padding.
// PPSSPP's XInput defaults (Core/KeyMapDefaults.cpp, defaultXInputKeyMap) put VIRTKEY_PAUSE on
// the left trigger, VIRTKEY_FASTFORWARD on the right one and VIRTKEY_SPEED_TOGGLE on the right
// stick click. An unclaimed control here does not fall through harmlessly - it opens a menu, or
// runs the game at triple speed, in the middle of whatever the player was doing.
const VCSPadMapping kVCSPadMappings[] = {
	// --- On foot ---
	{ VCSInputContext::OnFoot,     NKCODE_BUTTON_A,      CTRL_CROSS,     false, "Sprint", "Sprint", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,     NKCODE_BUTTON_B,      CTRL_CIRCLE,    false, "Attack / fire", "Fire", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,     kVCSPadJumpButton,    CTRL_SQUARE,    false, "Jump (vaults a ledge when there is one)", "Jump", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,     NKCODE_BUTTON_Y,      CTRL_TRIANGLE,  false, "Enter vehicle", "Enter vehicle", VCSKeyList::OnFoot },
	// The triggers, which is the change this whole table exists for. On the PSP aim is R and fire
	// is a face button, so aiming and shooting need a thumb and a shoulder; here they are the two
	// fingers already resting on the triggers, which is what every shooter since has settled on.
	{ VCSInputContext::OnFoot,     kVCSPadAimButton,     CTRL_RTRIGGER,  false, "Aim - lock-on, always; see LockOnModeActive", "Aim weapon", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,     NKCODE_BUTTON_R2,     CTRL_CIRCLE,    false, "Fire", "Fire", VCSKeyList::OnFoot },
	// The d-pad: weapons across, and the vertical pair left as the PSP's own.
	//
	// It used to carry view controls down here, on the reasoning that the PSP's vertical pair is
	// unrelated to the horizontal one and does nothing on foot. That was true of GAMEPLAY and
	// false of everything else. The game's own menus - pause, map, stats, and the save screen -
	// read the PSP d-pad, so rebinding the vertical pair left a pad that could move sideways
	// through a menu and not up or down, the horizontal pair working only by accident because
	// weapon cycling happens to sit on the same two PSP buttons.
	//
	// The keyboard never hit this: its arrow keys are not in kVCSKeyMappings at all, so they
	// reach the PSP d-pad through PPSSPP's mapper. The pad's d-pad is claimed here, and a claimed
	// control has nowhere to fall through to. Same intent, two devices, one of them with no way
	// to express it.
	//
	// Bound explicitly rather than left unclaimed, because falling through means depending on
	// PPSSPP's mapping for THIS device id - which is exactly what a pad on a slot nobody mapped
	// does not have. Up is the game's own recruit button on foot (see kVCSPadRecruitButton and
	// the GXT note above it), so the row is what the PSP does anyway and the Aiming context
	// already agreed with it; down reaches the game's Free Aim, which is where the keyboard's
	// arrow-down has always landed.
	//
	// The two functions that were here move to the stick clicks, which were doing nothing at all.
	{ VCSInputContext::OnFoot,     NKCODE_DPAD_LEFT,     CTRL_LEFT,      false, "Previous weapon", "Previous weapon", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,     NKCODE_DPAD_RIGHT,    CTRL_RIGHT,     false, "Next weapon", "Next weapon", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,     NKCODE_DPAD_UP,       CTRL_UP,        false, "Recruit gang member (target one first) / menu up", "Recruit gang member", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,     NKCODE_DPAD_DOWN,     CTRL_DOWN,      false, "Menu down (the game's own d-pad, unchanged)", "Menu down", VCSKeyList::OnFoot },
	// Start is the FORK's menu and View is the GAME's. That is the modern split - the system
	// button opens the system menu - and it is why Start carries no PSP button: it is handled on
	// the press edge in HandlePadKey, because a UI message is not something a button mask can say.
	{ VCSInputContext::OnFoot,     NKCODE_BUTTON_START,  0,              false, "Opens this menu (posted from HandlePadKey, not a PSP button)", "Menu", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,     NKCODE_BUTTON_SELECT, CTRL_START,     false, "Pause (the game's own, PSP Start)", "Pause", VCSKeyList::OnFoot },
	// Claimed and inert. The bumpers are zoom, which only means anything down a scope, so they
	// live in the Aiming rows below; leaving them unclaimed here would let PPSSPP's defaults send
	// them to the PSP's L and R, i.e. a second weapon-drop swap and a stray aim.
	{ VCSInputContext::OnFoot,     NKCODE_BUTTON_L1,     0,              false, "Suppressed (zoom belongs to the Aiming rows)" },
	{ VCSInputContext::OnFoot,     NKCODE_BUTTON_R1,     0,              false, "Suppressed (zoom belongs to the Aiming rows)" },
	// The stick clicks, which is where the d-pad's two view controls went. R3 for the camera is
	// what every modern game settled on anyway, and both were dead before this: L3 was the one
	// control this table never claimed at all, and R3 was claimed purely to keep PPSSPP's speed
	// toggle off it.
	{ VCSInputContext::OnFoot,     NKCODE_BUTTON_THUMBR, CTRL_SELECT,    false, "Change camera (verified)", "Change camera", VCSKeyList::OnFoot },
	{ VCSInputContext::OnFoot,     NKCODE_BUTTON_THUMBL, CTRL_LTRIGGER,  false, "Switch to nearby weapon drop (verified)", "Take nearby weapon", VCSKeyList::OnFoot },

	// --- In a vehicle ---
	//
	// The triggers become the pedals, which is the one substitution here that is really about
	// feel rather than about layout: they are analog, the PSP's accelerate and brake were two
	// face buttons, and driving with a thumb is the single most handheld-feeling thing left.
	// The PSP button underneath is still digital - the game reads a button, not a pedal - so what
	// this buys is where your fingers rest, not throttle control.
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_R2,     CTRL_CROSS,     false, "Accelerate", "Accelerate", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_L2,     CTRL_SQUARE,    false, "Brake / reverse", "Brake / reverse", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_A,      CTRL_DOWN,      false, "Horn (verified)", "Horn", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_B,      CTRL_CIRCLE,    false, "Drive-by fire", "Drive-by fire", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_X,      CTRL_RTRIGGER,  false, "Handbrake", "Handbrake", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_Y,      CTRL_TRIANGLE,  false, "Exit vehicle", "Exit vehicle", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_DPAD_LEFT,     CTRL_LEFT,      false, "Previous radio station / lower forks (verified)", "Previous radio station", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_DPAD_RIGHT,    CTRL_RIGHT,     false, "Next radio station / raise forks (verified)", "Next radio station", VCSKeyList::InVehicle },
	// The vertical pair is the PSP's own here too, for the menu reason spelled out in the on-foot
	// rows. Down being the horn that button A already is stops being a reason to leave it unbound
	// the moment the alternative is a pause menu the pad cannot move down through.
	{ VCSInputContext::InVehicle,  NKCODE_DPAD_UP,       CTRL_UP,        false, "Menu up", "Menu up", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_DPAD_DOWN,     CTRL_DOWN,      false, "Horn / menu down", "Horn / menu down", VCSKeyList::InVehicle },
	// The glances. psp = 0 because a glance is L trigger AND a stick direction at once: the
	// trigger half comes from GlanceButtonMask and the stick half from ApplyAnalog, exactly as
	// the keyboard's Q and E do it. The rows exist to claim the bumpers.
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_L1,     0,              false, "Look left (L trigger + stick, see GlanceDirection)" },
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_R1,     0,              false, "Look right (L trigger + stick, see GlanceDirection)" },
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_START,  0,              false, "Opens this menu (posted from HandlePadKey)", "Menu", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_SELECT, CTRL_START,     false, "Pause (the game's own, PSP Start)", "Pause", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_THUMBR, CTRL_SELECT,    false, "Change camera (verified)", "Change camera", VCSKeyList::InVehicle },
	{ VCSInputContext::InVehicle,  NKCODE_BUTTON_THUMBL, 0,              false, "Unbound (the weapon drop it carries on foot means nothing in a car)" },

	// --- Helicopters and planes ---
	//
	// The bumpers yaw, which is where the PSP had them (L and R directly, not the car's glance
	// modifier), and the triggers climb and descend. So the shoulders keep their meaning from the
	// car - the outer pair turns you, the inner pair is the throttle - even though the buttons
	// underneath are completely different.
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_R2,     CTRL_CROSS,     false, "Climb / throttle up", "Climb", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_L2,     CTRL_SQUARE,    false, "Descend / throttle down", "Descend", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_L1,     CTRL_LTRIGGER,  false, "Yaw left", "Yaw left", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_R1,     CTRL_RTRIGGER,  false, "Yaw right", "Yaw right", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_B,      CTRL_CIRCLE,    false, "Fire (Hunter)", "Fire (Hunter)", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_Y,      CTRL_TRIANGLE,  false, "Exit aircraft", "Exit aircraft", VCSKeyList::Aircraft },
	// A and X have nothing to do up here - climb and descend moved to the triggers, and the PSP
	// put nothing else on those two in the air. Claimed so they cannot reach Cross and Square,
	// which in an aircraft are exactly climb and descend and would fight the triggers.
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_A,      0,              false, "Unbound (climb is on the right trigger)" },
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_X,      0,              false, "Unbound (descend is on the left trigger)" },
	{ VCSInputContext::InAircraft, NKCODE_DPAD_LEFT,     CTRL_LEFT,      false, "Previous radio station", "Previous radio station", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_DPAD_RIGHT,    CTRL_RIGHT,     false, "Next radio station", "Next radio station", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_DPAD_UP,       CTRL_UP,        false, "Menu up", "Menu up", VCSKeyList::Aircraft },
	// The PSP's own centre view, kept reachable. It is d-pad down there too, so this is the one
	// aircraft row where the modern layout and the handheld's happen to agree.
	{ VCSInputContext::InAircraft, NKCODE_DPAD_DOWN,     CTRL_DOWN,      false, "Centre view", "Centre view", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_START,  0,              false, "Opens this menu (posted from HandlePadKey)", "Menu", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_SELECT, CTRL_START,     false, "Pause (the game's own, PSP Start)", "Pause", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_THUMBR, CTRL_SELECT,    false, "Change camera", "Change camera", VCSKeyList::Aircraft },
	{ VCSInputContext::InAircraft, NKCODE_BUTTON_THUMBL, 0,              false, "Unbound (nothing on foot's L3 applies in the air)" },

	// --- Aiming, which is also hand-to-hand ---
	//
	// Entered by holding LT, and the four face buttons are the melee set the moment the game
	// decides you are fighting rather than shooting - see the state table in the keyboard rows
	// above. They need no remapping at all: the PSP's melee assignments already sit under the
	// Xbox button in the same position.
	{ VCSInputContext::Aiming,     kVCSPadAimButton,     CTRL_RTRIGGER,  false, "Hold aim (verified)" },
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_R2,     CTRL_CIRCLE,    false, "Fire", "Light attack / fire", VCSKeyList::Melee },
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_B,      CTRL_CIRCLE,    false, "Light attack / fire", "Light attack / fire", VCSKeyList::Melee },
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_A,      CTRL_CROSS,     false, "Heavy hit / stomp / knee (melee)", "Heavy hit / stomp", VCSKeyList::Melee },
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_X,      CTRL_SQUARE,    false, "Block (melee)", "Block", VCSKeyList::Melee },
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_Y,      CTRL_TRIANGLE,  false, "Grab / throw / neckbreak / pull up", "Grab / throw", VCSKeyList::Melee },
	{ VCSInputContext::Aiming,     NKCODE_DPAD_LEFT,     CTRL_LEFT,      false, "Previous target", "Previous target", VCSKeyList::OnFoot | VCSKeyList::Melee },
	{ VCSInputContext::Aiming,     NKCODE_DPAD_RIGHT,    CTRL_RIGHT,     false, "Next target", "Next target", VCSKeyList::OnFoot | VCSKeyList::Melee },
	// Up is RECRUIT, and this row used to be a suppression on the reasoning that up would change
	// the camera mid-fight. It would not: on foot the PSP's up is recruit-with-a-target and
	// nothing else, so the row was suppressing the one thing the button is for. It reaches the
	// game unchanged here - a pad aims by lock-on always, so a target is exactly what it has.
	//
	// Down stays dead, and for the reason it always had: it is the game's own FREE AIM button and
	// the auto-free-aim pulse in ApplyMapping owns it, so a player pressing it by hand at the
	// wrong moment would cancel or double the pulse.
	{ VCSInputContext::Aiming,     kVCSPadRecruitButton, CTRL_UP,        false, "Recruit gang member (target one first)", "Recruit gang member", VCSKeyList::OnFoot },
	{ VCSInputContext::Aiming,     NKCODE_DPAD_DOWN,     0,              false, "Suppressed (d-pad down is Free Aim, which the pulse owns)" },
	// Zoom, and the reason VCSPadMapping has a scopedOnly column at all. These are the game's
	// Square and Cross, which with anything but a scope in hand are Block and Heavy Hit, so
	// ungated they would have a bumper throwing punches every time it was pressed in a fight.
	//
	// The gate covers the BINOCULARS too, and the game asked for that in as many words: `H_BINO1`
	// tells the player to zoom them with ~SNZI~ and ~SNZO~, the same two controls named here. See
	// ScopedWeaponActive.
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_R1,     CTRL_SQUARE,    true,  "Scope / binocular zoom in (verified)", "Zoom in (scope, binoculars)", VCSKeyList::OnFoot },
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_L1,     CTRL_CROSS,     true,  "Scope / binocular zoom out (verified)", "Zoom out (scope, binoculars)", VCSKeyList::OnFoot },
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_START,  0,              false, "Opens this menu (posted from HandlePadKey)" },
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_SELECT, CTRL_START,     false, "Pause (the game's own, PSP Start)" },
	// Both clicks stay dead while aiming: changing the camera mid-shot is the last thing anyone
	// wants, and the claim is still what keeps PPSSPP's speed toggle off R3.
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_THUMBR, 0,              false, "Suppressed (camera changes belong outside a shot)" },
	{ VCSInputContext::Aiming,     NKCODE_BUTTON_THUMBL, 0,              false, "Suppressed (weapon swaps belong outside a shot)" },
};

const size_t kVCSPadMappingCount = ARRAY_SIZE(kVCSPadMappings);

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
	// One control per column of the card, indexed by VCSListDevice: keyboard, Xbox, PlayStation.
	// A null cell means the action does not exist on that device, and the cell is left blank -
	// which is most of what separates the columns, since a few of this layer's keyboard-only
	// inventions have no button at all and the pads have no on-foot camera.
	//
	// One string rather than a list, unlike the rows the mapping tables produce. A cell that
	// really is several controls writes them out - "W / A / S / D" is one answer to "how do I
	// walk", not four - so the list a mapping row needs would buy nothing here.
	const char *controls[kVCSListDeviceCount];
	// Movement belongs at the top of a page and the odd-job keys at the bottom, and neither has
	// a place in the mapping table's order to be interleaved with. One bit is enough to say
	// which end.
	bool atEnd;
};

static const VCSListingExtra kVCSListingExtras[] = {
	// One row for one action, with the four keys in the cell rather than a row each. Splitting
	// them out gave five lines to say "you walk with these", and four of the five had an empty
	// cell under both pads that read as a missing binding rather than as a stick.
	{ VCSKeyList::OnFoot,    "Move",           { "W / A / S / D", "LEFT STICK", "LEFT STICK" } },
	// Blank on the pad because VCS has no on-foot look, not because one was left out: the PSP
	// has one stick, the game gives it to movement, and the camera follows by itself. Changing
	// the camera on Select is the whole of a pad's control over it, and that row comes from the
	// mapping table.
	{ VCSKeyList::OnFoot,    "Look",           { "MOUSE", "RIGHT STICK", "RIGHT STICK" } },

	{ VCSKeyList::InVehicle, "Steer",          { "A / D", "LEFT STICK", "LEFT STICK" } },
	{ VCSKeyList::InVehicle, "Look",           { "MOUSE", "RIGHT STICK", "RIGHT STICK" } },
	// Glances, for shooting out of the side of a car. Not in the mapping table because on the
	// PSP they are L trigger plus a stick direction - a button and an axis at once, which a row
	// there cannot express. See GlanceDirection. That combination is exactly what the pad columns
	// have to print, and they are the one place a bumper is named here rather than by
	// PadButtonName - which is why the Xbox and PlayStation cells differ by hand.
	{ VCSKeyList::InVehicle, "Look left",      { "Q", "LB", "L1" } },
	{ VCSKeyList::InVehicle, "Look right",     { "E", "RB", "R1" } },
	// The same two keys as the radio above, which is the game's doing rather than ours: a
	// special vehicle reuses d-pad left and right for its own function. Listed separately
	// because one row cannot carry both meanings, and a player in a forklift is not looking for
	// the radio.
	{ VCSKeyList::InVehicle, "Raise forks / turret", { "T", "D-PAD RIGHT", "D-PAD RIGHT" }, true },
	{ VCSKeyList::InVehicle, "Lower forks / turret", { "R", "D-PAD LEFT", "D-PAD LEFT" },   true },

	// The one row where the mouse and the nub really are the same control: both are the axis
	// pair the aircraft flies on.
	{ VCSKeyList::Aircraft,  "Pitch and roll", { "MOUSE", "LEFT STICK", "LEFT STICK" } },
	// Pad only. On the keyboard the aircraft camera is the mouse, which the row above already
	// says; on the pad it is a stick of its own and needs its own line.
	{ VCSKeyList::Aircraft,  "Look",           { nullptr, "RIGHT STICK", "RIGHT STICK" } },

	// Melee targeting is the game's own on a pad - it picks the target and turns you to face
	// them, and there is nothing to press.
	{ VCSKeyList::Melee,     "Face target",    { "MOUSE" } },

	// The keyboard half of a row the pad table already owns, which is the one thing an extras row
	// could not do until these started merging by name. Escape opens this menu and CANNOT be in
	// kVCSKeyMappings - claiming it would withhold it from PPSSPP's mapper, which is where the
	// pause it opens actually comes from, and the table says so in capitals. It is still what the
	// player presses, so the card has to say it.
	//
	// At the end, so it merges into the pad's Menu row where that row already is rather than
	// creating one above everything else.
	{ VCSKeyList::OnFoot | VCSKeyList::InVehicle | VCSKeyList::Aircraft,
	                         "Menu",           { "ESC" }, true },
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

// The pad's buttons as the card names them, in the two vocabularies the card prints.
//
// One scheme, two sets of labels. The layout does not change between them - a DualSense and an
// Xbox pad have the same controls in the same places and differ in what is printed on the plastic
// - so this is a naming function and nothing more. That is the whole reason both pad columns come
// out of one mapping table.
//
// The Xbox names are the pad's own current ones rather than the historical ones: "VIEW" is what
// Microsoft has called that button for over a decade and what is printed next to it. The
// PlayStation column follows the same rule, which is why the face buttons are spelled out rather
// than drawn - the shapes are glyphs this font does not have, and "CROSS" is what a player would
// say aloud anyway.
//
// "CREATE / SHARE" names one button across two generations, because it genuinely has two names:
// a DualSense prints CREATE and a DualShock 4 prints SHARE, and a card that picked one would be
// wrong for half the pads it describes.
std::string PadButtonName(InputKeyCode button, VCSListDevice device) {
	if (device == VCSListDevice::PlayStation) {
		switch (button) {
		case NKCODE_BUTTON_A: return "CROSS";
		case NKCODE_BUTTON_B: return "CIRCLE";
		case NKCODE_BUTTON_X: return "SQUARE";
		case NKCODE_BUTTON_Y: return "TRIANGLE";
		case NKCODE_BUTTON_L1: return "L1";
		case NKCODE_BUTTON_R1: return "R1";
		case NKCODE_BUTTON_L2: return "L2";
		case NKCODE_BUTTON_R2: return "R2";
		case NKCODE_BUTTON_THUMBL: return "L3";
		case NKCODE_BUTTON_THUMBR: return "R3";
		case NKCODE_BUTTON_START: return "OPTIONS";
		case NKCODE_BUTTON_SELECT: return "CREATE / SHARE";
		default:
			// The d-pad is the one group both vocabularies agree about, so it falls through to
			// the Xbox switch rather than being written out twice.
			break;
		}
	}

	switch (button) {
	case NKCODE_BUTTON_A: return "A";
	case NKCODE_BUTTON_B: return "B";
	case NKCODE_BUTTON_X: return "X";
	case NKCODE_BUTTON_Y: return "Y";
	case NKCODE_BUTTON_L1: return "LB";
	case NKCODE_BUTTON_R1: return "RB";
	case NKCODE_BUTTON_L2: return "LT";
	case NKCODE_BUTTON_R2: return "RT";
	// CLICK, spelled out: these rows now sit on a card directly under LOOK / RIGHT STICK, and
	// "RIGHT STICK" against two different actions reads as a contradiction rather than as two
	// halves of one control. The PlayStation column says L3 / R3 and never had the problem.
	case NKCODE_BUTTON_THUMBL: return "LEFT STICK CLICK";
	case NKCODE_BUTTON_THUMBR: return "RIGHT STICK CLICK";
	case NKCODE_DPAD_UP: return "D-PAD UP";
	case NKCODE_DPAD_DOWN: return "D-PAD DOWN";
	case NKCODE_DPAD_LEFT: return "D-PAD LEFT";
	case NKCODE_DPAD_RIGHT: return "D-PAD RIGHT";
	case NKCODE_BUTTON_START: return "START";
	case NKCODE_BUTTON_SELECT: return "VIEW";
	default:
		break;
	}
	// Nothing in kVCSPadMappings reaches this, and if something ever does, a name is better than
	// a blank cell - a blank one would read as an unbound action rather than as a missing string.
	std::string name = KeyMap::GetKeyName(button);
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

	// Rows sharing a name are one line with several controls against it - which is how the card
	// shows that Fire is both the left mouse button and Backspace, or on the pad both B and RT,
	// without either of them being written down twice. It is also what merges the three devices
	// into one row per action, since all three tables use the same listName for the same thing.
	auto addRow = [&rows](const char *listName, VCSListDevice device, const std::string &control) {
		VCSListingRow *row = nullptr;
		for (VCSListingRow &existing : rows) {
			if (!strcmp(existing.name, listName)) {
				row = &existing;
				break;
			}
		}
		if (!row) {
			rows.push_back(VCSListingRow{listName});
			row = &rows.back();
		}
		std::vector<std::string> &column = row->controls[(size_t)device];
		// The same control can reach one action through two contexts - Q cycles weapons on foot
		// and targets while aiming, and the aim control appears in both the OnFoot and Aiming
		// rows. Show it once.
		if (std::find(column.begin(), column.end(), control) == column.end()) {
			column.push_back(control);
		}
	};

	// The extras go through addRow too, not straight into the list, so a row here can fill in one
	// device's cell on a row a mapping table owns. Escape under MENU is the case that needs it -
	// no table can hold that key - and a row with nothing on any device simply never gets created.
	auto addExtras = [&addRow, list](bool atEnd) {
		for (const VCSListingExtra &extra : kVCSListingExtras) {
			if (!(extra.list & list) || extra.atEnd != atEnd) {
				continue;
			}
			for (size_t i = 0; i < kVCSListDeviceCount; i++) {
				if (extra.controls[i]) {
					addRow(extra.name, (VCSListDevice)i, extra.controls[i]);
				}
			}
		}
	};

	addExtras(false);

	// Every column is built from the table that actually drives that device. Deriving the pad's
	// from the keyboard's table would have been possible while a pad still went through the PSP's
	// own layout - every row knew the button its key produced - and it stopped being possible the
	// moment the pad got a scheme of its own, because the two no longer agree about anything but
	// the face buttons.
	//
	// The keyboard goes first, so the card is in keyboard-table order and the handful of rows only
	// a pad has - the fork's own menu button, the aircraft's centre view - land after it rather
	// than interleaved somewhere neither table asked for.
	for (size_t i = 0; i < kVCSKeyMappingCount; i++) {
		const VCSKeyMapping &mapping = kVCSKeyMappings[i];
		if (!(mapping.list & list) || !mapping.listName) {
			continue;
		}
		addRow(mapping.listName, VCSListDevice::Keyboard, KeyDisplayName(mapping.key));
	}
	for (size_t i = 0; i < kVCSPadMappingCount; i++) {
		const VCSPadMapping &mapping = kVCSPadMappings[i];
		if (!(mapping.list & list) || !mapping.listName) {
			continue;
		}
		// One row of one table filling two columns, which is the point of naming the pad twice
		// rather than tabulating it twice.
		addRow(mapping.listName, VCSListDevice::Xbox,
			PadButtonName(mapping.button, VCSListDevice::Xbox));
		addRow(mapping.listName, VCSListDevice::PlayStation,
			PadButtonName(mapping.button, VCSListDevice::PlayStation));
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

	// The game's own front end is up, so the player is working a menu rather than a character.
	// This is the long-standing `GameState` TODO, answered - not by the general "what is the game
	// doing" enum that was hunted for and never found, but by the specific half of it that this
	// layer actually needed: FrontEndMenuManager's own active flag. See Core/VCS/VCSFrontEnd.h.
	//
	// It is checked FIRST because it outranks everything below it: the player can be on foot, in a
	// car, or holding the aim key when the menu opens, and none of those describe what the keys
	// should do while it is up.
	//
	// Note what this is NOT: the reverted "FrameCounter stalled means menu" shortcut from
	// 03c1f3cfe0. That inferred the menu from the game's logic being stopped, which is also true
	// during loading screens and cutscenes. This reads the menu's own flag, which is true when the
	// menu is up and at no other time.
	if (GameMenuActive()) {
		return VCSInputContext::Menu;
	}

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
		// Either device's aim control. The pad's is the left trigger, which by the time it gets
		// here is an ordinary held button - HandleHostAxis turns the axis into one, precisely so
		// that questions like this one have a single shape to ask.
		if (IsHostKeyDown(kVCSAimKey) || IsPadButtonDown(kVCSPadAimButton)) {
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

// The same question of the pad's set. Assumes g_hostKeyMutex is held.
//
// The scheme being switched off answers false here rather than at each call site, and that is the
// difference between a clean switch and a stuck one: HandleHostAxis stops recording when the
// scheme goes off, so a trigger held across that moment would otherwise stay in the set forever -
// and the aim trigger holding means the Aiming context never ends.
static bool IsPadButtonDownLocked(InputKeyCode button) {
	if (!PadSettings().enabled) {
		return false;
	}
	return g_heldPadButtons.find(button) != g_heldPadButtons.end();
}

bool IsPadButtonDown(InputKeyCode button) {
	std::lock_guard<std::mutex> guard(g_hostKeyMutex);
	return IsPadButtonDownLocked(button);
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

	// Either device. The bumpers are the pad's glance for the same reason Q and E are the
	// keyboard's: they are the controls a thumb is not already using while driving.
	bool q, e;
	{
		std::lock_guard<std::mutex> guard(g_hostKeyMutex);
		q = IsHostKeyDownLocked(NKCODE_Q) || IsPadButtonDownLocked(NKCODE_BUTTON_L1);
		e = IsHostKeyDownLocked(NKCODE_E) || IsPadButtonDownLocked(NKCODE_BUTTON_R1);
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

// --- The pad's input path ---------------------------------------------------------------------

static bool IsPadDevice(InputDeviceID device) {
	return (device >= DEVICE_ID_PAD_0 && device <= DEVICE_ID_PAD_9) ||
	       (device >= DEVICE_ID_XINPUT_0 && device <= DEVICE_ID_XINPUT_3);
}

// Which way is up on this pad's Y axes.
//
// XInput reports a stick positive when it is pushed AWAY from the player. DirectInput's lY and
// the HID drivers' `ly - 128` for a DualShock or a DualSense both report positive when it is
// pulled TOWARD them - and a Switch Pro, on the same HID path, negates in its own decoder and so
// agrees with XInput after all. Three drivers can deliver a pad on Windows and they do not agree,
// while X does agree across all of them, which is why an inverted pad walks backwards and steers
// correctly rather than being wrong in both axes.
//
// PPSSPP never has to notice, because it reads sticks through the mapping and its defaults carry
// the difference: An.Up is axis Y POSITIVE for XInput and axis Y NEGATIVE for a generic pad,
// while An.Left and An.Right are the same keycode for both. This layer bypasses the mapper, so it
// has to ask the same question itself - and it used to answer "XInput", which is right for one
// driver out of three.
//
// Asked OF THE MAPPING rather than of the device id, which is the whole point: a pad PPSSPP reads
// correctly cannot then be read backwards here, and a player who fixes their axis in PPSSPP's
// mapper fixes it here too, with nothing to keep in step. The device class is only the fallback,
// for a pad on a slot that has no mapping at all - and it says what PPSSPP's own defaults say.
static constexpr int kPadSignSlots = (DEVICE_ID_XINPUT_3 - DEVICE_ID_PAD_0) + 1;
static std::atomic<int> g_padSignGeneration{-1};
static std::atomic<int> g_padSignLeftY[kPadSignSlots]{};
static std::atomic<int> g_padSignRightY[kPadSignSlots]{};

static int PadDefaultYSign(InputDeviceID device) {
	return device >= DEVICE_ID_XINPUT_0 ? 1 : -1;
}

// Cached, because this runs for every axis event at the poll rate and MappedAxesForDevice walks
// the whole controller map behind a lock. g_controllerMapGeneration is what PPSSPP bumps on any
// rebind, so the cache drops itself rather than having to be told.
static void PadYSigns(InputDeviceID device, int *leftY, int *rightY) {
	const int slot = (int)device - (int)DEVICE_ID_PAD_0;
	if (slot < 0 || slot >= kPadSignSlots) {
		*leftY = 1;
		*rightY = 1;
		return;
	}

	const int generation = KeyMap::g_controllerMapGeneration;
	if (g_padSignGeneration.exchange(generation, std::memory_order_relaxed) != generation) {
		for (int i = 0; i < kPadSignSlots; i++) {
			g_padSignLeftY[i].store(0, std::memory_order_relaxed);
			g_padSignRightY[i].store(0, std::memory_order_relaxed);
		}
	}

	// Zero is "not worked out yet" rather than a sign, so a race here costs one extra lookup and
	// two stores of the same answer.
	if (g_padSignLeftY[slot].load(std::memory_order_relaxed) == 0) {
		const MappedAnalogAxes axes = KeyMap::MappedAxesForDevice(device);
		const int fallback = PadDefaultYSign(device);
		g_padSignLeftY[slot].store(axes.leftY.direction ? axes.leftY.direction : fallback,
			std::memory_order_relaxed);
		g_padSignRightY[slot].store(axes.rightY.direction ? axes.rightY.direction : fallback,
			std::memory_order_relaxed);
	}

	*leftY = g_padSignLeftY[slot].load(std::memory_order_relaxed);
	*rightY = g_padSignRightY[slot].load(std::memory_order_relaxed);
}

// A RADIAL deadzone, rescaled so that the first movement past it is small rather than a jump to
// the deadzone's own width.
//
// Radial rather than per-axis, and that is not a detail on a look stick: a square deadzone lets
// one axis through while the other is still inside the box, so a slow diagonal push starts as a
// pure horizontal sweep and only becomes diagonal once it clears the corner. On a camera that
// reads as the stick refusing to look diagonally.
static bool PadStickDeflection(float rawX, float rawY, float *x, float *y) {
	*x = 0.0f;
	*y = 0.0f;

	const float dead = std::clamp(PadSettings().deadzone, 0.0f, 0.9f);
	const float length = std::sqrt(rawX * rawX + rawY * rawY);
	if (length <= dead || length <= 0.0f) {
		return false;
	}

	float scaled = (length - dead) / (1.0f - dead);
	if (scaled > 1.0f) {
		scaled = 1.0f;
	}
	*x = rawX / length * scaled;
	*y = rawY / length * scaled;
	return true;
}

// Both sticks answer false with the scheme switched off, so one check covers every caller rather
// than each of them remembering to ask.
static bool PadLeftStick(float *x, float *y) {
	if (!PadSettings().enabled) {
		*x = 0.0f;
		*y = 0.0f;
		return false;
	}
	return PadStickDeflection(g_padLeftX.load(std::memory_order_relaxed),
		g_padLeftY.load(std::memory_order_relaxed), x, y);
}

static bool PadRightStick(float *x, float *y) {
	if (!PadSettings().enabled) {
		*x = 0.0f;
		*y = 0.0f;
		return false;
	}
	return PadStickDeflection(g_padRightX.load(std::memory_order_relaxed),
		g_padRightY.load(std::memory_order_relaxed), x, y);
}

// Whether this context has a row for this button at all - the pad's answer to ContextMapsKeyLocked,
// and the thing that decides whether the button is claimed. Reads only the table, so it needs no
// lock.
static bool PadContextMapsButton(VCSInputContext context, InputKeyCode button) {
	for (size_t i = 0; i < kVCSPadMappingCount; i++) {
		if (kVCSPadMappings[i].context == context && kVCSPadMappings[i].button == button) {
			return true;
		}
	}
	return false;
}

// An analog trigger, turned into a button.
//
// Two thresholds rather than one, and the fire button is why. A trigger resting against a single
// line - which is exactly where a finger holds one - crosses it on noise alone, and the button
// underneath is Fire. Pressing at 0.45 and releasing at 0.35 puts a tenth of the travel between
// the two decisions, which is more than any trigger's jitter and less than any player's hold.
static void SetPadTrigger(InputKeyCode button, float value) {
	std::lock_guard<std::mutex> guard(g_hostKeyMutex);
	const bool wasDown = IsPadButtonDownLocked(button);
	const bool nowDown = wasDown ? value > PadSettings().triggerRelease
	                             : value > PadSettings().triggerPress;
	if (nowDown == wasDown) {
		return;
	}
	if (nowDown) {
		g_heldPadButtons.insert(button);
	} else {
		g_heldPadButtons.erase(button);
	}
}

static bool HandlePadKey(const KeyInput &key) {
	if (!PadSettings().enabled) {
		return false;
	}

	const bool down = (key.flags & KeyInputFlags::DOWN) != 0;
	const bool up = (key.flags & KeyInputFlags::UP) != 0;
	if (!down && !up) {
		return false;
	}

	// Start opens the fork's own menu, which is the modern split: the system button reaches the
	// system menu and the game's pause screen is on View. Handled here rather than in the table
	// because a UI message is not a PSP button, and BEFORE the context gate below on purpose - a
	// menu button that only works once the address table has resolved a context is a menu button
	// that does not work on the boot screen, which is where somebody reaching for it is most
	// likely to be.
	if (key.keyCode == NKCODE_BUTTON_START) {
		if (down) {
			System_PostUIMessage(UIMessage::REQUEST_GAME_PAUSE);
		}
		return true;
	}

	const VCSInputContext context = g_currentContext.load(std::memory_order_relaxed);

	// B is Back, and in the game's own menu Back is this fork's menu - the pad's half of the
	// Backspace rule in HandleHostKey. Answered before the held set below and outside its lock,
	// because posting a UI message with an input mutex held is an ordering nothing else here
	// takes. Claimed on the release as well, so it cannot reach PPSSPP's mapper and arrive at the
	// game as a Circle a moment later.
	if (key.keyCode == NKCODE_BUTTON_B && context == VCSInputContext::Menu) {
		if (down) {
			System_PostUIMessage(UIMessage::REQUEST_GAME_PAUSE);
		}
		return true;
	}

	std::lock_guard<std::mutex> guard(g_hostKeyMutex);
	if (down) {
		g_heldPadButtons.insert(key.keyCode);
	} else {
		g_heldPadButtons.erase(key.keyCode);
	}

	// Same rule the keyboard follows: claim only what we will act on, so an empty address table
	// leaves a pad behaving exactly as it did before any of this existed.
	if (context == VCSInputContext::Unknown) {
		return false;
	}
	return PadContextMapsButton(context, key.keyCode);
}

bool HandleHostAxis(const AxisInput &axis) {
	if (!IsActive() || !PadSettings().enabled || !IsPadDevice(axis.deviceId)) {
		return false;
	}

	// The claim is withheld until a context resolves, exactly as HandleHostKey withholds it - and
	// here it is not a nicety but the difference between a working pad and a dead one. With the
	// context Unknown, ApplyAnalog applies nothing; claiming the sticks anyway would take the
	// movement away from PPSSPP's mapper and then not use it, leaving a player unable to move at
	// all on a build where the address table came up empty.
	//
	// The positions are still RECORDED either way. That costs nothing, and it means the first
	// tick after a context resolves already knows where the sticks are rather than waiting for
	// the player to move them.
	const bool claim = g_currentContext.load(std::memory_order_relaxed) != VCSInputContext::Unknown;

	// Both Y axes are normalised to the XInput convention on the way in, so that everything
	// downstream - the deadzone, ApplyAnalog, ApplyPadLook and its one negation for the mouse -
	// keeps the single convention it was written and measured against. See PadYSigns.
	int leftYSign = 1, rightYSign = 1;
	PadYSigns(axis.deviceId, &leftYSign, &rightYSign);

	switch (axis.axisId) {
	// The sticks are stored raw and read through the deadzone, rather than deadzoned here. The
	// deadzone is a setting, and a value stored through the old one would survive a change to it.
	case JOYSTICK_AXIS_X:
		g_padLeftX.store(axis.value, std::memory_order_relaxed);
		return claim;
	case JOYSTICK_AXIS_Y:
		// Positive is AWAY from the player once the sign is applied, and the PSP stick's positive
		// Y is also away from the camera, so the two agree and the game gets forward for forward.
		g_padLeftY.store(axis.value * (float)leftYSign, std::memory_order_relaxed);
		return claim;
	case JOYSTICK_AXIS_Z:
		g_padRightX.store(axis.value, std::memory_order_relaxed);
		return claim;
	case JOYSTICK_AXIS_RZ:
		g_padRightY.store(axis.value * (float)rightYSign, std::memory_order_relaxed);
		return claim;
	// Claiming the triggers is not optional. PPSSPP's XInput defaults put VIRTKEY_PAUSE on the
	// left trigger and VIRTKEY_FASTFORWARD on the right one, so an unclaimed trigger opens a menu
	// or triples the game speed - and this scheme's triggers are aim and fire, i.e. held.
	case JOYSTICK_AXIS_LTRIGGER:
		SetPadTrigger(NKCODE_BUTTON_L2, axis.value);
		return claim;
	case JOYSTICK_AXIS_RTRIGGER:
		SetPadTrigger(NKCODE_BUTTON_R2, axis.value);
		return claim;
	default:
		// A pedal set, a hat, a sixth axis on some flight stick. Not ours; let the mapper have it.
		return false;
	}
}

void ApplyPadLook(VCSInputContext context) {
	switch (context) {
	case VCSInputContext::OnFoot:
	case VCSInputContext::InVehicle:
	case VCSInputContext::InAircraft:
	case VCSInputContext::Aiming:
		break;
	default:
		// Nothing looks around in a context we cannot identify, and accumulating a delta that
		// nothing drains would dump the whole hoard into the camera the moment one resolved.
		return;
	}

	float x = 0.0f, y = 0.0f;
	if (!PadRightStick(&x, &y)) {
		return;
	}

	// Squared response, sign preserved. A stick is a position and the camera wants a rate, so the
	// player is holding the speed rather than moving to it - and a linear map makes the slow half
	// of the travel useless for fine work while the fast half is all the same to a thumb. Squaring
	// is the standard answer and costs nothing.
	const float ax = x * std::fabs(x);
	const float ay = y * std::fabs(y);

	const VCSPadSettings &pad = PadSettings();
	float dx = ax * pad.lookSpeed;
	// The stick's positive Y is away from the player; a mouse's positive dy is DOWN the screen,
	// and everything downstream of this - the camera, the free-aim solver, the invert settings -
	// was written for a mouse. One negation here rather than a special case in each of them.
	float dy = -ay * pad.lookSpeed;
	if (pad.invertLookY) {
		dy = -dy;
	}

	// Into the same accumulator the mouse fills, which is the whole design: the stick becomes
	// mouse movement at the edge and every measured thing behind it - sensitivity, the FOV scale,
	// the aim response model - is reused rather than reimplemented against a second input.
	AddLookDelta(dx, dy);
}

static std::atomic<int> g_wheelUp{0};
static std::atomic<int> g_wheelDown{0};
// Never consumed, so the debugger can tell "no notches arrive at all" from "they arrive and the
// button is wrong" - two completely different bugs, and this one was the first.
static std::atomic<int> g_wheelSeen{0};

// Counted here rather than in SetHostKeyDown, which is where this started and which HandleHostKey
// does not call - it updates the held set inline. So the counter sat in a function only the
// debugger's key simulator reaches, and every real notch went uncounted.
static void NoteWheelNotch(InputKeyCode key) {
	if (key == NKCODE_EXT_MOUSEWHEEL_UP) {
		g_wheelUp.fetch_add(1, std::memory_order_relaxed);
		g_wheelSeen.fetch_add(1, std::memory_order_relaxed);
	} else if (key == NKCODE_EXT_MOUSEWHEEL_DOWN) {
		g_wheelDown.fetch_add(1, std::memory_order_relaxed);
		g_wheelSeen.fetch_add(1, std::memory_order_relaxed);
	}
}

int WheelNotchesSeen() {
	return g_wheelSeen.load(std::memory_order_relaxed);
}

void TakeWheelNotches(int *up, int *down) {
	*up = g_wheelUp.exchange(0, std::memory_order_relaxed);
	*down = g_wheelDown.exchange(0, std::memory_order_relaxed);
}

void SetHostKeyDown(InputKeyCode key, bool down) {
	if (down) {
		NoteWheelNotch(key);
	}
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

	// A gamepad goes down its own path, against its own table.
	//
	// It used to be turned away right here - this was a keyboard and mouse layer, and a pad went
	// past it to PPSSPP's mapper and the PSP's own layout. That is precisely what made playing
	// with a pad feel like playing a handheld: not the buttons being wrong, but nothing in this
	// file ever seeing them.
	if (IsPadDevice(key.deviceId)) {
		return HandlePadKey(key);
	}

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

	// A notch is a press and a release in the same instant, so it is recorded as it arrives rather
	// than left for the emu tick to notice - see TakeWheelNotches.
	if (down) {
		NoteWheelNotch(key.keyCode);
	}

	// Back, out of one of the game's own pages and into ours.
	//
	// Posted here on the press edge rather than mapped in the table, for the same reason Start is
	// posted from the pad path: a UI message is not something a button mask can say. EmuScreen
	// shuts the game's menu and raises this fork's once it has gone, so the two are one step from
	// the player's side - and the same key pressed again in that menu leaves for the world.
	if (down && context == VCSInputContext::Menu && key.keyCode == NKCODE_DEL) {
		System_PostUIMessage(UIMessage::REQUEST_GAME_PAUSE);
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
		g_heldPadButtons.clear();
	}
	// A stick left deflected across a shutdown would walk the player the instant the next game
	// started, before any axis event arrived to correct it.
	g_padLeftX.store(0.0f, std::memory_order_relaxed);
	g_padLeftY.store(0.0f, std::memory_order_relaxed);
	g_padRightX.store(0.0f, std::memory_order_relaxed);
	g_padRightY.store(0.0f, std::memory_order_relaxed);
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

	// Read before the lock, deliberately. This reads the decoded game state, and taking that
	// while holding the input mutex would be the only place in this file where two locks are held
	// at once - in an order nothing else guarantees.
	//
	// It also costs this function the "no side effects, pure function of the key state" property
	// its comment used to claim. The gate is worth it: see the scopedOnly rows.
	const bool scoped = ScopedWeaponActive();

	// One lock for the whole table rather than one per row.
	std::lock_guard<std::mutex> guard(g_hostKeyMutex);
	u32 mask = 0;
	for (size_t i = 0; i < kVCSKeyMappingCount; i++) {
		const VCSKeyMapping &mapping = kVCSKeyMappings[i];
		if (mapping.context == context && IsHostKeyDownLocked(mapping.key)) {
			mask |= mapping.psp;
		}
	}
	// The pad's rows OR into the same mask. Both devices are live at once and always have been -
	// somebody with a pad in their hands and a hand on the keyboard gets both, and two rows
	// naming the same button simply set the same bit. No check for the scheme being on: that
	// lives in IsPadButtonDownLocked, which is the only place it should.
	for (size_t i = 0; i < kVCSPadMappingCount; i++) {
		const VCSPadMapping &mapping = kVCSPadMappings[i];
		if (mapping.context != context || (mapping.scopedOnly && !scoped)) {
			continue;
		}
		if (IsPadButtonDownLocked(mapping.button)) {
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

// The binoculars, as an equipped ITEM rather than as a raised camera - so it is already true on
// the frame the aim key goes down, which is the frame the entry sequence is armed on. Keying that
// gate on the camera instead would arm the sequence first and only learn better afterwards.
//
// Same unset rule as MeleeEquipped: a read that failed leaves the previous behaviour standing
// rather than guessing, because claiming "binoculars" wrongly would cost every gun its entry into
// free aim.
static bool BinocularsEquipped() {
	const std::optional<u32> weapon = GetState().weaponType;
	return weapon && WeaponTypeIsBinoculars(*weapon);
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

	// A cheat combination owns the pad for the second or so it takes to type, on the same terms a
	// vault owns the character - and for a sharper reason. The game is being sent an exact
	// sequence of presses, so a held W is not merely noise, it is a Cross arriving between two of
	// them: a ninth press in an eight-press combination, which fails it silently.
	//
	// Applied HERE rather than in VCSCheats, which is why that file presses nothing itself. One
	// function stays the only thing in this fork that touches sceCtrl, and the sequencer inherits
	// the release discipline below instead of growing a second copy of it.
	//
	// Note the clear mask is the usual `held & ~wanted` and not the whole of it. On the first
	// frame of a combination the player may still be holding the key that produces its first
	// press, and clearing everything would drop that press before the game ever sampled it.
	//
	// The front-end bridge takes the pad on the same terms while it presses Start and walks to a
	// page - and gives it back the moment the player is meant to be working the game's menu
	// themselves, which is why FrontEndDriving goes false at hand-over rather than when the menu
	// closes. From there the Menu context does the work instead.
	if (CheatEntryInProgress() || FrontEndDriving()) {
		// Cheats win a tie. Only one can be requested at a time - the row that queues either one
		// closes this fork's menu behind it - but a combination is an exact sequence where the
		// bridge is a loop that re-reads its own progress, so if the two ever did overlap the
		// sequence is the one that cannot survive an extra bit being ORed into it.
		const u32 ownMask = CheatEntryInProgress() ? CheatButtonMask() : FrontEndButtonMask();
		const u32 ownClear = g_lastAppliedMask & ~ownMask;
		if (ownMask != 0 || ownClear != 0) {
			__CtrlUpdateButtons(ownMask, ownClear);
		}
		g_lastAppliedMask = ownMask;
		g_prevAppliedContext = context;
		return ownMask;
	}

	// Arm the auto-free-aim pulse on the edge into Aiming - not while in it, or it would retrigger
	// every frame and hold d-pad down forever.
	if (context == VCSInputContext::Aiming && g_prevAppliedContext != VCSInputContext::Aiming) {
		// Through LockOnModeActive rather than the raw flag: the pad is a second source of the
		// same answer, and reading the atomic here would have armed the pulse for a pad that is
		// meant never to leave lock-on.
		// RecruitHeld joins the gate rather than being handled anywhere near the recruit rows,
		// because it is the same question the other two terms ask: is this player asking for the
		// game's lock-on rather than for a crosshair. Recruiting needs a target and free aim is
		// the state with none, so a pulse fired here would cancel the very thing the key is for.
		//
		// THE BINOCULARS STAND DOWN FOR THE REASON MELEE DOES, and the failure is the same shape
		// one item over. Both buttons this sequence presses mean something else with them raised:
		// the sprint latch holds CTRL_CROSS for ~8 ticks and Cross while glassing is ZOOM OUT, so
		// aiming while walking forward zoomed the view back out every time; and the pulse presses
		// d-pad Down, the game's Free Aim button, which an item with nothing to fire has no use
		// for. Neither could be noticed before, because neither meant anything with a gun up.
		if (CameraSettings().autoFreeAim && !LockOnModeActive() && !MeleeEquipped()
			&& !BinocularsEquipped() && !RecruitHeld()) {
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

	// In the game's own menu, left and right change tab - and the tab strip that would have shown
	// which one is no longer drawn. Claimed and dropped rather than unbound: an unclaimed arrow
	// key falls through to PPSSPP's mapper, which sends the very d-pad direction being suppressed.
	// Same discipline as the psp = 0 rows, applied at runtime because it is a setting.
	// The map's place-marker, asked for with Space or a click. ORed in rather than taking the pad
	// over, because it is the player pressing a key, not the bridge running a sequence.
	if (context == VCSInputContext::Menu) {
		setMask |= MapWaypointButtonMask();
		setMask |= MapZoomButtonMask();
	}

	// Only on a page THIS FORK navigated to. The lock exists because the tab strip is hidden on
	// those pages, so tabbing moves you somewhere unmarked - it is not a statement about menus in
	// general, and applying it to one the game opened itself was a real bug: the save UI reports
	// MenuPage 0, which resolves to MAP_PAGE, which has no selectable widgets, so up and down were
	// dropped and the save slots could not be moved through.
	//
	// And only while the TAB STRIP has focus, which is the one state the four directions mean
	// "change tab" in. Inside a page they belong to the page - they pan the map, they move down
	// the save slots - so a lock that ignored `MenuUseRoot` was taking the arrows away from the
	// player at exactly the moment the page had a use for them. That is why the map could not be
	// panned from the keyboard at all: its arrows ARE claimed here, unlike a pad's, so the lock
	// reached them.
	//
	// `MenuPageHasItems` is no longer consulted. It was standing in for this question - "are the
	// arrows doing something on this page" - and the flag the front end keeps for it answers
	// directly, on every page, without a widget walk. It defaults to `true` when unreadable,
	// which keeps the lock's failure mode where it was: an unreadable menu tabs rather than
	// stranding the player with dead arrows.
	if (context == VCSInputContext::Menu && FrontEndSettings().lockMenuTabs && BridgeOwnsMenu()
		&& MenuOnTabStrip()) {
		setMask &= ~(u32)(CTRL_LEFT | CTRL_RIGHT | CTRL_UP | CTRL_DOWN);
	}

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
	//
	// And during a cheat combination, for the reason the buttons stand down there: the stick is
	// how W and S reach a car, so leaving it live would have the player still accelerating into
	// traffic through the second it takes to type one.
	if (VaultInProgress() || CheatEntryInProgress() || FrontEndDriving()) {
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

	// The drive-by joins the reticle here rather than getting a branch of its own, because it is
	// the same statement about the nub: it is the aim, not movement, so the mouse belongs on it.
	// Everything below - the model, the frame pacing, the clamp - applies unchanged, and the one
	// thing that differs is already handled where it belongs, in ReadAimResponse's per-axis scale.
	//
	// A passenger cannot steer, so nothing is being taken away by A and D standing down here. They
	// were only ever moving the gun because the gun is what this stick does in that seat.
	if (DriveByAimActive(context) || ReticleActive(context)) {
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

		// The mounted cannon's elevation, on the stick's Y axis - the one channel a vehicle
		// otherwise leaves dead, since W and S are the pedals and live in the button table.
		//
		// Taken here rather than peeked because ApplyAnalog runs BEFORE CameraTick, so a peek
		// would return the previous frame's movement. The horizontal half is handed straight back
		// so the camera still turns with the mouse: only the vertical is spent on the cannon,
		// which is the axis the player has no other way to reach.
		//
		// Computed before the key mutex is taken - TakeMouseDelta and AddLookDelta both want the
		// delta lock, and nesting the two in one order here would be the only place that does.
		float cannonY = 0.0f;
		if (CannonAimActive(context)) {
			float mdx = 0.0f, mdy = 0.0f;
			TakeMouseDelta(&mdx, &mdy);
			if (mdx != 0.0f) {
				AddLookDelta(mdx, 0.0f);
			}
			const VCSCameraSettings &cs = CameraSettings();
			cannonY = -mdy * cs.cannonSensitivity * (cs.aimInvertY ? -1.0f : 1.0f);
			if (cannonY > 1.0f) cannonY = 1.0f;
			if (cannonY < -1.0f) cannonY = -1.0f;
		}

		// The model keeps state between frames, so it has to be dropped when aiming stops -
		// otherwise the next free aim opens by cancelling a glide that ended long ago, and jumps.
		//
		// The reticle is the only aim channel there is, so "not driving" and "nothing is aiming"
		// are the same statement again. They came apart while the d-pad carried a second channel,
		// which needed the model kept alive here; that channel is gone.
		AimModelReset();
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

		// The pad's left stick, which REPLACES the keys rather than adding to them.
		//
		// Summing would be the obvious thing and is wrong: a stick already carries magnitude, so
		// adding a key's full 1.0 to a half-pushed stick asks for a deflection neither input
		// requested, and holding W with a centred stick would be indistinguishable from holding
		// both. Whichever device is actually being moved is the one that answers.
		float padX = 0.0f, padY = 0.0f;
		if (PadLeftStick(&padX, &padY)) {
			switch (context) {
			case VCSInputContext::OnFoot:
			// Aiming without free aim is lock-on, where the stick strafes around the target -
			// movement, so the stick drives it exactly as on foot. Free aim never reaches here:
			// the reticle branch above returns first, and it has to, because the PSP has one
			// stick and in free aim it is the crosshair.
			case VCSInputContext::Aiming:
				x = padX;
				y = padY;
				break;
			case VCSInputContext::InVehicle:
				// Steering only, exactly as A and D are. The pedals are the triggers and go
				// through the button table, so the stick's Y has nothing here to drive.
				x = padX;
				break;
			case VCSInputContext::InAircraft:
				// X banks and Y pitches - the whole reason this context exists. Pushed away from
				// the player is positive on both the stick and the PSP's nub, and that pitches
				// the NOSE DOWN, which is what converts the rotor's lift into forward flight.
				x = padX;
				y = padY;
				break;
			default:
				break;
			}
		}

		// After the pad, because the pad's in-vehicle row assigns X and leaves Y alone - so the
		// two do not compete, and a player on a pad still gets the cannon from the mouse.
		y += cannonY;
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

// Sniper and RPG - the manually-aimed, scoped weapons. Weapon camera modes 7 and 8, measured.
//
// AND THE BINOCULARS, which are a scope with no trigger behind it. Hold aim and the game raises
// them into their own first-person camera (mode 47), aimed by turning the view and zoomed with the
// same two buttons the sniper uses - and that last part is the game saying so rather than this
// fork inferring it. The mission's own help line is `H_BINO1`, "Use ~SNZI~ and ~SNZO~ to zoom in
// and out with the binoculars", and SNZI/SNZO are the keys the GXT resolves to Square and Cross,
// which is exactly what the sniper zoom rows already send. See "Ask the GXT what a control is
// called" in CLAUDE.md.
//
// Every caller wants them treated alike, which is what makes this the right place rather than a
// second predicate beside it: the zoom bumpers must reach the game (that gate is this function),
// the pad's always-lock-on rule must except them the way it excepts a scope, and the two aim
// leads - the yaw kick and the pitch deadband - must stand down, because both exist to break
// stiction in front of the weapon camera's integrator and there is none in a camera that follows
// the moment it is written. Feeding one to a camera that has none is the twitch this file already
// records for the sniper and the RPG.
//
// Keyed on the WEAPON ID here and on the weapon camera mode above, and the split is deliberate.
// The binoculars do have a weapon camera mode - it reads 47, measured live over the WebSocket
// debugger, the same 47 the active camera takes - but it is only set once they are RAISED, and
// reads 0 for the whole time they are merely selected. The id is true from the moment one is
// picked, which is this function's own stated preference and what the entry-sequence gate in
// ApplyMapping needs: that fires on the frame the aim key goes down, before any camera has
// changed, so a mode test would arm the sequence first and only learn better afterwards.
//
// That the camera has no integrator is measured too, and it is the argument for every lead this
// predicate stands down: across a whole binocular session CCam+0x130 and +0x124 - the game's own
// smoothed aim increments - stayed at exactly 0.00000. The game is not accumulating a look axis
// in mode 47, so there is nothing in front of our write to break loose.
bool ScopedWeaponActive() {
	const std::optional<u32> mode = ReadAddrU32(VCSAddr::WeaponCamMode);
	if (mode && (*mode == 7 || *mode == 8)) {
		return true;
	}
	return BinocularsEquipped();
}

bool DriveByAimActive(VCSInputContext context) {
	// An aircraft has no drive-by, and on foot is the other mechanism entirely. Restricting this
	// to InVehicle also keeps it away from the Aiming context, where the reticle already owns the
	// stick - two aim paths on one nub is the failure ReticleActive warns about.
	if (context != VCSInputContext::InVehicle) {
		return false;
	}
	if (!CameraSettings().driveByMouseAim) {
		return false;
	}
	// Measured live, in the passenger seat, mid-mission: CamMode 11, WeaponCamMode 11, and the aim
	// axis scale sitting at exactly the 2.5 / 0.5 that the retail script's single `03E9` call sets
	// up for a passenger drive-by - see AimAxisScale in VCSAddresses.h, which had that call
	// decoded long before there was anything to do with it.
	//
	// Both modes are required rather than either, and that is the guard that matters: WeaponCamMode
	// is a property of the WEAPON and outlives the moment it was set, so a stale 11 left behind by
	// a drive-by that has ended must not be able to take the stick away from steering while the
	// player is driving. The active camera being mode 11 too is what says the drive-by is now.
	const std::optional<u32> camMode = ReadAddrU32(VCSAddr::CamMode);
	const std::optional<u32> weaponMode = ReadAddrU32(VCSAddr::WeaponCamMode);
	return camMode && weaponMode && *camMode == 11 && *weaponMode == 11;
}

// The fire truck, whose water cannon a mission asks the stick to aim. Read live from the occupied
// vehicle: model 194, the same field VehicleClassForModel uses to tell a helicopter from a car.
static const u32 kVCSCannonVehicleModel = 194;

bool CannonAimActive(VCSInputContext context) {
	if (context != VCSInputContext::InVehicle) {
		return false;
	}
	if (!CameraSettings().cannonMouseAim) {
		return false;
	}
	// The model, and deliberately nothing else - see cannonMouseAim for the flag that looked like
	// a spray signal and turned out to be a 15-second timer.
	const std::optional<u32> model = GetState().vehicleModel;
	return model && *model == kVCSCannonVehicleModel;
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
	// LockOnModeActive rather than the raw flag, for the same reason ApplyMapping asks it: the
	// pad is a second source of this answer, and a half-applied mode is exactly what the comment
	// above describes going wrong.
	if (LockOnModeActive()) {
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

const char *AimPathStatus(VCSInputContext context) {
	// Deliberately in the same order the real predicates test, so the answer names the term that
	// actually short-circuits rather than the first one that happens to be true.
	//
	// The drive-by comes first because it is the one aim path that is not in the Aiming context,
	// so the blanket "not aiming" below would otherwise be a lie about it - and this line existing
	// is how the next person sees the mouse is on the nub without reproducing the seat.
	if (DriveByAimActive(context)) {
		return "drive-by - mouse drives the nub (passenger seat, cam mode 11)";
	}
	if (CannonAimActive(context)) {
		return "cannon - mouse Y drives the nub's Y, steering keeps X";
	}
	if (context != VCSInputContext::Aiming) {
		return "not aiming";
	}
	// The three FreeAimActive gates, in its order.
	if (LockOnModeActive()) {
		return "SUPPRESSED: lock-on mode (L toggle, or the pad's aim trigger)";
	}
	if (MeleeEquipped()) {
		return "SUPPRESSED: melee equipped (weaponIndex reads as a melee slot)";
	}
	if (GetState().isAiming.value_or(false)) {
		// Partial since the gun-pointing was moved ahead of this gate: the pose keeps tracking
		// the crosshair, only the heading write stands down. See PedAimTick.
		return "PARTIAL: the game is locked on (IsAiming = 1) - gun still tracking";
	}
	// Then the two that gate the reticle specifically.
	if (g_latchTimer > 0) {
		return "entry latch (establishing a run before Free Aim is pressed)";
	}
	if (CameraAimActive(context)) {
		return "camera aim - mouse drives CameraYaw, nub is free for WASD";
	}
	return "reticle - mouse drives the nub";
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

VCSInputContext GetCurrentContext() {
	return g_currentContext.load(std::memory_order_relaxed);
}

bool JumpHeld() {
	return IsHostKeyDown(kVCSJumpKey) || IsPadButtonDown(kVCSPadJumpButton);
}

bool RecruitHeld() {
	return IsHostKeyDown(kVCSRecruitKey) || IsPadButtonDown(kVCSPadRecruitButton);
}

bool CameraDrivenAimHeld() {
	// The mouse, unchanged and unconditional: it has no lock-on mode of its own to be in.
	if (IsHostKeyDown(kVCSAimKey)) {
		return true;
	}
	// The pad, only where it is not in lock-on - which is the scoped weapons, and exactly the
	// case where the right stick is steering the camera as the aim.
	return IsPadButtonDown(kVCSPadAimButton) && !LockOnModeActive();
}

bool LockOnModeActive() {
	// The keyboard's manual toggle, unchanged.
	if (g_lockOnMode.load(std::memory_order_relaxed)) {
		return true;
	}

	// And the pad, which aims this way always.
	//
	// Expressed as this mode rather than as a mechanism of its own, because it IS this mode: no
	// Free Aim press, the stick stays movement, the camera stays the game's. Everything already
	// written against the toggle therefore applies to the pad without being taught that a pad
	// exists - and there is exactly one place to look when the two ever need to differ.
	//
	// The mouse's aim key wins when both are somehow held. Free aim is what a mouse is for, and
	// reaching for it is the more deliberate of the two acts.
	if (!IsPadButtonDown(kVCSPadAimButton) || IsHostKeyDown(kVCSAimKey)) {
		return false;
	}

	// A SCOPE IS THE EXCEPTION, and it is not a softening of "always lock-on" - it is what the
	// phrase means when the game has no lock-on to offer. The sniper and the RPG have none: aim
	// with one and the game scopes in, and the shot follows the camera rather than any target the
	// game picked. Pinning them to a lock-on that does not exist would not make aiming assisted,
	// it would leave the right stick dead with a crosshair on screen and nothing able to move it.
	//
	// Same shape as the MeleeEquipped rule in FreeAimActive: an automatic, weapon-driven exception
	// beside the manual toggle.
	return !ScopedWeaponActive();
}

}  // namespace VCS
