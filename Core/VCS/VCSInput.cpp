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
#include <atomic>
#include <mutex>
#include <set>

#include "Common/Common.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/VCS/VCSCamera.h"
#include "Core/VCS/VCSGame.h"
#include "Core/VCS/VCSInput.h"
#include "Core/VCS/VCSMemory.h"

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

// TOGGLES the PSP's lock-on instead of free aim. G because PPSSPP binds nothing to it (checked
// against Core/KeyMapDefaults.cpp) and neither does the table below, so claiming it costs nothing.
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
// not a control scheme. Flip it when you switch to fists and flip it back after.
const InputKeyCode kVCSLockOnKey = NKCODE_G;

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
static VCSInputContext g_prevAppliedContext = VCSInputContext::Unknown;

const char *VCSInputContextName(VCSInputContext context) {
	switch (context) {
	case VCSInputContext::OnFoot: return "OnFoot";
	case VCSInputContext::InVehicle: return "InVehicle";
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
	{ VCSInputContext::OnFoot,    NKCODE_SPACE,              CTRL_SQUARE,    "Jump" },
	{ VCSInputContext::OnFoot,    NKCODE_SHIFT_LEFT,         CTRL_CROSS,     "Sprint" },
	{ VCSInputContext::OnFoot,    NKCODE_EXT_MOUSEBUTTON_1,  CTRL_CIRCLE,    "Attack / fire" },
	{ VCSInputContext::OnFoot,    kVCSAimKey,                CTRL_RTRIGGER,  "Aim (verified)" },
	{ VCSInputContext::OnFoot,    NKCODE_F,                  CTRL_TRIANGLE,  "Enter vehicle" },
	{ VCSInputContext::OnFoot,    NKCODE_E,                  CTRL_RIGHT,     "Next weapon" },
	{ VCSInputContext::OnFoot,    NKCODE_Q,                  CTRL_LEFT,      "Previous weapon" },
	// L trigger on foot is NOT unused, which this file claimed for a long time. Standing near a
	// dropped weapon, it switches to that weapon's type - a pickup/swap, distinct from the Q/E
	// cycle through what you already carry. Verified in game.
	//
	// Tab costs PPSSPP's fast-forward (VIRTKEY_FASTFORWARD in Core/KeyMapDefaults.cpp) - a
	// claimed key is withheld from PPSSPP's mapper, which is the inverse Escape trap, accepted
	// deliberately here. Rebind fast-forward in PPSSPP's own controls if you want it back.
	{ VCSInputContext::OnFoot,    NKCODE_TAB,                CTRL_LTRIGGER,  "Switch to nearby weapon drop (verified)" },
	{ VCSInputContext::OnFoot,    NKCODE_V,                  CTRL_SELECT,    "Change camera (verified)" },
	{ VCSInputContext::OnFoot,    NKCODE_P,                  CTRL_START,     "Pause (Start)" },

	// --- In a vehicle ---
	{ VCSInputContext::InVehicle, NKCODE_W,                  CTRL_CROSS,     "Accelerate" },
	{ VCSInputContext::InVehicle, NKCODE_S,                  CTRL_SQUARE,    "Brake / reverse" },
	{ VCSInputContext::InVehicle, NKCODE_SPACE,              CTRL_RTRIGGER,  "Handbrake" },
	{ VCSInputContext::InVehicle, NKCODE_F,                  CTRL_TRIANGLE,  "Exit vehicle" },
	{ VCSInputContext::InVehicle, NKCODE_EXT_MOUSEBUTTON_1,  CTRL_CIRCLE,    "Drive-by fire" },
	{ VCSInputContext::InVehicle, NKCODE_H,                  CTRL_DOWN,      "Horn (verified)" },
	{ VCSInputContext::InVehicle, NKCODE_R,                  CTRL_RIGHT,     "Next radio station (verified)" },
	{ VCSInputContext::InVehicle, NKCODE_T,                  CTRL_LEFT,      "Previous radio station (verified)" },
	{ VCSInputContext::InVehicle, NKCODE_V,                  CTRL_SELECT,    "Change camera (verified)" },
	{ VCSInputContext::InVehicle, NKCODE_P,                  CTRL_START,     "Pause (Start)" },
	// Claimed but deliberately mapped to nothing. Sprint is meaningless in a car, and leaving
	// Shift unclaimed let it fall through to PPSSPP, whose default binding is VIRTKEY_RAPID_FIRE
	// - that alternates held buttons, so it machine-gunned the accelerator and made throttle
	// stutter. This is the inverse of the Escape trap: there we stole a key PPSSPP needed, here
	// PPSSPP was stealing one we needed to be inert.
	{ VCSInputContext::InVehicle, NKCODE_SHIFT_LEFT,        0,              "Suppressed (blocks PPSSPP rapid-fire)" },

	// Q and E in a vehicle are NOT here: they are glances, which on the PSP are L trigger
	// plus a stick direction, so they need both a button and an axis. See GlanceDirection.

	// --- Aiming (on foot, aim key held) ---
	// Separate from OnFoot because the same inputs mean different things here: the mouse becomes
	// the reticle instead of the camera, WASD goes quiet because the stick is now the reticle, and
	// Q/E cycle targets instead of weapons.
	{ VCSInputContext::Aiming,    NKCODE_EXT_MOUSEBUTTON_1,  CTRL_CIRCLE,    "Fire" },
	{ VCSInputContext::Aiming,    kVCSAimKey,                CTRL_RTRIGGER,  "Hold aim (verified)" },
	{ VCSInputContext::Aiming,    NKCODE_Q,                  CTRL_LEFT,      "Previous target" },
	{ VCSInputContext::Aiming,    NKCODE_E,                  CTRL_RIGHT,     "Next target" },
	// Same as on foot. Bound here mainly so Tab can't fall through to PPSSPP's fast-forward
	// mid-fight, which would suddenly run the game at several times speed while aiming.
	{ VCSInputContext::Aiming,    NKCODE_TAB,                CTRL_LTRIGGER,  "Switch to nearby weapon drop (verified)" },
	{ VCSInputContext::Aiming,    NKCODE_P,                  CTRL_START,     "Pause (Start)" },
	// Same rapid-fire suppression as in a vehicle - it would stutter the fire button while aiming.
	{ VCSInputContext::Aiming,    NKCODE_SHIFT_LEFT,        0,              "Suppressed (blocks PPSSPP rapid-fire)" },
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
	{ VCSInputContext::OnFoot,    NKCODE_G,                 0,              "Toggle lock-on mode (for melee) instead of free aim" },
	{ VCSInputContext::Aiming,    NKCODE_G,                 0,              "Toggle lock-on mode (for melee) instead of free aim" },

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

	// Arm the auto-free-aim pulse on the edge into Aiming - not while in it, or it would retrigger
	// every frame and hold d-pad down forever.
	if (context == VCSInputContext::Aiming && g_prevAppliedContext != VCSInputContext::Aiming) {
		if (CameraSettings().autoFreeAim && !g_lockOnMode.load(std::memory_order_relaxed)) {
			// Delay + pulse. A delay of 0 presses Free Aim on the very same tick the aim trigger
			// goes down, which is the least lock-on the game can be given.
			int delay = CameraSettings().aimFreeAimDelay;
			if (delay < 0) delay = 0;
			g_freeAimTimer = delay + kFreeAimPulse;
		}
	} else if (context != VCSInputContext::Aiming) {
		// Letting go of aim cancels a pulse in flight, so releasing early can't leave d-pad down
		// asserted into whatever context comes next.
		g_freeAimTimer = 0;
	}
	g_prevAppliedContext = context;

	u32 setMask = ComputeButtonMask(context) | GlanceButtonMask(context) | g_forcedButtons;

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

bool FreeAimActive(VCSInputContext context) {
	if (context != VCSInputContext::Aiming) {
		return false;
	}
	// The lock-on key means lock-on, all the way through. It already suppresses the Free Aim press;
	// without also standing the reticle down it was only half a mode - the game stayed in lock-on
	// while the mouse still took the stick, and in lock-on the stick is MOVEMENT, so the mouse
	// walked the player around and WASD did nothing.
	//
	// That half-state is exactly what melee is stuck in permanently: its lock-on does not register
	// in IsAiming (confirmed in play - the flag reads 0 while visibly locked on), so the test below
	// concludes free aim and hands over the stick. Holding this key is the manual way out until the
	// weapon itself can be identified; see the WeaponIndex note in docs/VCS_ADDRESSES.md.
	if (g_lockOnMode.load(std::memory_order_relaxed)) {
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
	// Stand down only if the direct-aim path can actually run. It needs AimYaw, which is not
	// found yet - and standing down without it left free aim with NO driver at all, since
	// PadStickActive keys off this too. "Prefer the better mechanism" must always be conditional
	// on that mechanism being available, or the fallback disappears with it.
	if (CameraSettings().mouseLookInFreeAim && IsAddrSet(VCSAddr::AimYaw)) {
		return false;
	}
	// Any other aim mechanism takes precedence, and the left-stick reticle stands down. Two of
	// them running at once fight over one crosshair, and worse, whichever runs first eats the
	// mouse delta and starves the other.
	//
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
