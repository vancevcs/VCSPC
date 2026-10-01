# Architecture, seams and threading

> Read before adding a source file, a hook into PPSSPP, or a new address; or when the data flow is unclear.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

## Architecture

```
Core/VCS/                      no dependency on ImGui, UI, or any renderer
  VCSAddresses.h    constexpr address table — the single source of truth
  VCSMemory.h/cpp   std::optional accessors over PSP RAM; a bad address is never fatal
  VCSState.h/cpp    per-frame decoded game state
  VCSInput.h/cpp    input context enum, (context, host key) -> PSP button table, WASD analog
  VCSCamera.h/cpp   mouse and right-stick look; hands the turn to the chase camera, and writes
                    CameraYaw/CameraPitch directly only for aiming and the mounted gun
  VCSChaseCam.h/cpp the camera the player looks through on foot, driving and flying - built
                    inside the game's camera update, pulled in by the game's own line of sight
  VCSFireHook.h/cpp free aim resolved at the fire site, by rewriting the raycast's target
  VCSWorld.h/cpp    calls the game's own collision code, via a program written into PSP memory
  VCSVault.h/cpp    ledge detection and the pull-up, built on that query
  VCSCheats.h/cpp   the game's own button-combo cheats, typed by a sequencer on the game's clock
  VCSFrontEnd.h/cpp a bridge into the GAME's pause menu - its map, its save list - and the Menu gate
  VCSGame.h/cpp     lifecycle + per-frame tick; the only entry point the rest of PPSSPP sees
  VCSSettings.h/cpp the player-facing option table, and the only thing that persists any of it

GPU/Common/
  VCSShadow.h/cpp   dynamic sun shadows: caster capture, cascade, screen mask, composite
  VCSWater.h/cpp    the sea, and rain on the roads: capture, depth, shading, composite

UI/ImDebugger/
  ImVCS.h/cpp       the "VCS" debugger window (lives here so Core stays ImGui-free)

UI/
  VCSMenuScreen.h/cpp  the GTA-styled pause menu and its option pages
```

Data flows one way: `VCSAddresses` → `VCSMemory` → `VCSState` → `VCSInput` → `sceCtrl`, with
`VCSCamera` branching off the same state to write camera angles directly.

### Why each piece exists

**`VCSAddresses.h`** is `constexpr` and exhaustive. Reads of unset entries return `nullopt`
*before touching memory*, which is what makes a partly-filled table a working state rather than a
crash. The PSP has no ASLR, so global addresses are stable across runs and baking them into a
constexpr table is reasonable.

Entries come in two forms. An **absolute** entry (`base = kNoBase`) is a fixed address, valid only
for globals in the static data region — roughly `0x088xxxxx`–`0x08exxxxx`. A **based** entry
(`base = VCSAddr::Something`, `address` = a byte offset) chases the pointer stored at that base
and adds the offset, resolved fresh on every read. Anything living inside a heap-allocated struct
*must* use the based form — the struct moves, so an absolute address would be a latent bug.
`PlayerHealth` is `PlayerBase + 0x4e4`. Only one level of indirection is supported.

**`VCSMemory`** exists so that a wrong address is a normal condition, not a fault. It's a thin
wrapper over PPSSPP's own `Memory::IsValidRange` / `IsValid4AlignedRange` — deliberately not new
bounds-checking logic. You will type wrong addresses into the scratchpad constantly while
hunting; that must stay silent, with no logging and no assert.

**`VCSState`** uses `std::optional` per field so "not found yet" and "found, and it's zero" stay
distinguishable. Collapsing those would make the input layer act on an empty table.

**`VCSInput`** keys on `(context, InputKeyCode)`. Context is the part a static pad mapping can't
express, and it's why this doesn't live in PPSSPP's `ControlMapper`. It only ever *clears* the
button bits it owns, so a real pad still works alongside it.

It also drives the **analog stick** for movement, via `ApplyAnalog` — deliberately not part of the
key→button table, because WASD produces an axis and its meaning splits across mechanisms by
context: on foot all four keys are 2D movement, while in a vehicle A/D steer the stick and W/S are
*buttons* (Cross/Square). That single split is the clearest justification for this whole layer —
PPSSPP's mapper binds one key to one meaning globally and cannot express it. Same ownership
discipline as the buttons: the stick is only touched while a movement key is held, and released
exactly once when the last one comes up, so a real pad is unaffected.

**`VCSCamera`** turns mouse and right-stick movement into look angles. It doesn't go through `sceCtrl`,
because no PSP input can express "rotate by exactly this many radians". On foot, driving and flying it
hands the angles to **`VCSChaseCam`**, which owns those views outright. Everywhere else - aiming, the
mounted gun, and plain look with the chase camera switched off - it writes `CameraYaw` and
`CameraPitch` directly, re-asserted every tick while it holds them, because the game eases a one-off
write straight back (measured: 135 successful writes, zero net rotation). It also owns the raw
mouse-delta buffer for the *aiming* path, which spends the delta on the stick instead - see "Aiming is
a stick, not a camera" below.

**`VCSChaseCam`** is the camera the player looks through, and it is built rather than negotiated: it
lets the game compute its camera and replaces the result inside the game's own frame, at two points in
the camera update, with the collision done by the game's own `CWorld::ProcessLineOfSight`. See "The
chase camera: the view is built, not negotiated" below for how, and for why the angle-writing mouse
look it replaced could never have been made to work in a vehicle.

**What four builds of handing the camera back taught, kept short.** The old mouse look wrote the
on-foot and vehicle cameras' angles and had to give them back when the player stopped - a fade, a
target behind the player, a learner that measured the follow camera's offset, a hold until the player
moved. Every version hit the same wall: mode 4 holds one resting position while walking and another
at a standstill, so a handback at rest is a value the game will always disagree with. The vehicle
pitch runaway was the same shape from the other side - a second writer of one field, at a different
rate from the game's own integrator. Both argue for the design that replaced them: when the game's
camera does the wrong thing with a number you hand it, stop handing it numbers and replace what it
produces. The general lesson from the learner is the one this file has recorded four times: a
quantity that is only valid in some states is not a constant, and asking *when* it was measured is a
different question from asking whether it was measured correctly.

**Two FOV reference constants, on purpose.** `AimAxisStep` scales by `FOV / 80` and `FOVLookScale`
scales by `FOV / 70`, and the difference is not an oversight to be tidied away. The 80 is *the
game's* constant, matched so the term cancels in the solve. The 70 is the FOV VCS actually runs at,
picked so the look path's scale is exactly 1.0 in ordinary play and `sensitivity` keeps the meaning
it was tuned with — re3's own 80 is likewise just what *its* defaults were tuned against. They also
apply to different paths and must never both apply to one: direct angle writes get `FOVLookScale`,
the reticle gets the game's term via `AimAxisStep`. Applying both is the sniper bug, twice over.

**`VCSGame`** is the only thing the rest of PPSSPP knows about — three call sites, all no-ops for
other games.

## Seams — where this hooks into PPSSPP

The entire integration is five small edits. Keep it that way.

| What | Where | Note |
|---|---|---|
| Per-frame tick | `hleEnterVblank()` in [Core/HLE/sceDisplay.cpp](../../Core/HLE/sceDisplay.cpp) | Once per vblank on the emu thread, next to the existing `g_controlMapper.UpdateAutoMovements` call |
| Init | `__KernelInit()` in [Core/HLE/sceKernel.cpp](../../Core/HLE/sceKernel.cpp) | After `__CtrlInit`, since it drives sceCtrl |
| Shutdown | `__KernelShutdown()` in [Core/HLE/sceKernel.cpp](../../Core/HLE/sceKernel.cpp) | *Before* `__CtrlShutdown`, so held buttons get released while sceCtrl is alive |
| Host keys | `NativeKey()` in [UI/NativeApp.cpp](../../UI/NativeApp.cpp) | Inside the existing `passKeyThrough` branch, which already handles ImGui capture and UI-vs-ingame gating |
| Mouse look | `NativeMouseDelta()` in [UI/NativeApp.cpp](../../UI/NativeApp.cpp) | Same claim pattern as keys; skipped while the ImGui debugger wants the mouse |
| Compat flag | [Core/Compatibility.h](../../Core/Compatibility.h) + `.cpp` + [assets/compat.ini](../../assets/compat.ini) | One struct field, one `CheckSetting` line, one `[VCSInputOverhaul]` section - and a second pair for `VCSDynamicShadows`, kept separate because one is a renderer feature and the other is input, and a third for `VCSWaterQuality` |
| Shadow capture | `Flush()` in [GPU/Vulkan/DrawEngineVulkan.cpp](../../GPU/Vulkan/DrawEngineVulkan.cpp) | Classify + capture on both transform paths, the predecode gate, and `OnFlush` at the top for the 3D→2D seam |
| Shadow frame reset | `BeginHostFrame()` in [GPU/Vulkan/GPU_Vulkan.cpp](../../GPU/Vulkan/GPU_Vulkan.cpp) | Publishes last frame's counts; renders nothing |
| Water capture | `Flush()` in [GPU/Vulkan/DrawEngineVulkan.cpp](../../GPU/Vulkan/DrawEngineVulkan.cpp) | Beside the shadow capture on both transform paths, and its own `OnFlush` *after* the shadow one - see the comment there for why the order matters |
| Water frame reset | `BeginHostFrame()` in [GPU/Vulkan/GPU_Vulkan.cpp](../../GPU/Vulkan/GPU_Vulkan.cpp) | Also where the road mask is rasterised and uploaded, because it is the one point in the frame with no render pass open |
| Pause menu | the three `GamePauseScreen` sites in [UI/EmuScreen.cpp](../../UI/EmuScreen.cpp) | All three now call `CreatePauseScreen()`, which returns PPSSPP's own screen unless `VCS::IsActive()` |

**Mouse input requires "Use Mouse Control" to be on** (Settings → Controls, `UseMouse` in
`ppsspp.ini`). PPSSPP gates mouse delta delivery on it. Turning it on costs nothing here because
`VCS::HandleMouseDelta` consumes the delta before PPSSPP's own mouse-to-analog processing runs,
and it also gives cursor hiding/confining for free.

**Not** `__DisplayVblankBeginCallback`, despite the name. That's a `WAITTYPE_VBLANK` wait-type
callback registered via `__KernelRegisterWaitTypeFuncs`; it only fires when a thread already
blocked in `sceDisplayWaitVblankCB` gets a callback delivered, once per waiting thread, and not
at all if nothing is waiting. `hleEnterVblank` is the real per-frame seam.

### Threading

`NativeFrame()` — and therefore `Core_RunLoopUntil()` **and `UI/ImDebugger/*.cpp`** — always run
on the same thread, regardless of graphics backend (see "Debugger threading model" in AGENTS.md). So
the VCS debugger window reads PSP memory directly, with no marshalling and no snapshot buffer,
and still satisfies the emu-thread-only rule. Don't "fix" this by adding a lock; the legacy Win32
debugger is the one that needs `g_frameMutex`, not this.
