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

// One row of the mapping table: in this context, this host key produces these PSP button bits.
// psp is a mask of the CTRL_* defines from Core/HLE/sceCtrl.h, so a single key can produce a
// combination if we ever need that.
struct VCSKeyMapping {
	VCSInputContext context;
	InputKeyCode key;
	u32 psp;
	const char *description;
};

// The mapping table. Rows are matched in order; the first row whose context and key both match
// wins. A row with context Unknown would apply everywhere, which we deliberately never use.
extern const VCSKeyMapping kVCSKeyMappings[];
extern const size_t kVCSKeyMappingCount;

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
// Always false when aimViaRightStick is on: that mode aims through the other stick instead, and
// letting both run would give two things fighting over one aim.
// Aim held and the game is not steering it - i.e. free aim, as opposed to lock-on. The state,
// independent of which mechanism is currently driving it.
bool FreeAimActive(VCSInputContext context);

bool ReticleActive(VCSInputContext context);

// Whether the mouse is currently feeding the PSP's RIGHT analog stick for the CLEO plugin.
//
// True whenever the aim key is held and aimViaRightStick is set - deliberately NOT gated on
// IsFreeAiming, unlike the left-stick reticle. Two reasons:
//
//   - Nothing in VCS reads the right stick, so feeding it can never make the player strafe or
//     misbehave. It is inert unless the plugin is listening. The caution that applies to the
//     left stick simply doesn't apply here.
//   - The plugin may be what PUTS the game into free aim, so gating on the game already being in
//     free aim could never let it start. Chicken and egg.
bool PluginAimActive(VCSInputContext context);

// Feeds the right analog stick from the mouse for the plugin. Emu thread only, once per frame,
// and MUST run after ApplyAnalog and before CameraTick - see the ordering note in VCSGame::Tick.
// Only touches the stick while it is actually driving it, and releases it exactly once, the same
// ownership discipline ApplyAnalog follows for the left stick.
void ApplyAimStick(VCSInputContext context);

// The right-stick position applied on the most recent tick, for the debugger window.
void GetAppliedAimStick(float *x, float *y);

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
u32 GetForcedButtons();

// The context resolved on the most recent tick, for the debugger window.
VCSInputContext GetCurrentContext();

// Whether the lock-on toggle is on. While it is, aiming stays in the game's lock-on: no Free Aim
// press, and the mouse does not take the analog stick - so WASD moves and the mouse looks around.
// Exists for melee, whose lock-on does not register in IsAiming.
bool LockOnModeActive();

// Whether any of WASD is held. The free-aim brake keys on this: while a movement key is down the
// latched run is left to carry the player, and releasing them all is what applies the brake.
bool MovementKeysHeld();

}  // namespace VCS
