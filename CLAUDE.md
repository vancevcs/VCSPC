@AGENTS.md

---

# GTA: Vice City Stories "PC version" fork

Everything above comes from upstream PPSSPP and still applies — in particular the build
instructions, the seven-places rule for registering new source files, and the debugger threading
model. What follows is specific to this fork.

## Goal

Turn PPSSPP into a dedicated PC-feeling front end for **GTA: Vice City Stories** (PSP, disc ID
`ULUS10160`, USA build only). Three things make it feel like a PC GTA rather than an emulated
handheld:

1. **Context-aware button mapping** — the same key means different things on foot and in a
   vehicle. Space is Jump on foot, Handbrake while driving.
2. **True mouse look** — driving the camera directly instead of nudging an analog stick.
3. **Mouse aiming** — hold to aim, mouse places the crosshair, click to fire.
4. **Direct weapon selection** — number keys, instead of cycling with a shoulder button.

All four now work to some degree - see "Status" for exactly how far each one got, and what is
still guessed rather than verified.

## Hard constraints

These are not style preferences. Breaking any of them is a bug:

- **Zero behaviour change for every other game.** Everything is gated on the `VCSInputOverhaul`
  compat flag *and* on the disc ID. With the flag off, `VCS::Tick()` returns on its first line
  and no other VCS code is reachable.
- **PSP memory is read on the emu thread only.** Never the UI thread.
- **No hardcoded addresses outside `Core/VCS/VCSAddresses.h`.** Not one.
- **It must build and run with any subset of addresses unset.** 6 of 9 are known now, but the
  unset ones must still read as `unset` in the overlay, never as garbage, and every feature
  that depends on a missing address must simply not engage.
- **Minimal diff to upstream.** Don't refactor PPSSPP code unless strictly necessary; this fork
  should stay easy to rebase.

## Architecture

```
Core/VCS/                      no dependency on ImGui, UI, or any renderer
  VCSAddresses.h    constexpr address table — the single source of truth
  VCSMemory.h/cpp   std::optional accessors over PSP RAM; a bad address is never fatal
  VCSState.h/cpp    per-frame decoded game state
  VCSInput.h/cpp    input context enum, (context, host key) -> PSP button table, WASD analog
  VCSCamera.h/cpp   mouse look; writes CameraYaw/CameraPitch directly, not via sceCtrl
  VCSGame.h/cpp     lifecycle + per-frame tick; the only entry point the rest of PPSSPP sees

UI/ImDebugger/
  ImVCS.h/cpp       the "VCS" debugger window (lives here so Core stays ImGui-free)
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

**`VCSCamera`** is mouse look, horizontal and vertical. It doesn't go through `sceCtrl`, because no PSP input
can express "rotate by exactly this many radians" — it writes `CameraYaw` directly. It also owns
the raw mouse-delta buffer for the *aiming* path, which spends the delta on the stick instead —
see "Aiming is a stick, not a camera" below.

The non-obvious part is *why it writes every frame*. The game runs its own camera smoothing that
eases yaw back toward its follow target, so writing only when the mouse moves accomplishes nothing
— the game undoes each nudge in between (measured: 135 successful writes, zero net rotation).
`VCSCamera` therefore keeps its own `g_desiredYaw`, anchors it to the game's value when a look
starts, and re-asserts it every frame for `kHoldFrames` (~0.75s) after the last movement. That
outpaces the smoothing while the player is looking around, then releases so the normal
follow-camera returns. Don't "simplify" this back into a read-modify-write; it will silently stop
working.

**`VCSGame`** is the only thing the rest of PPSSPP knows about — three call sites, all no-ops for
other games.

## Seams — where this hooks into PPSSPP

The entire integration is five small edits. Keep it that way.

| What | Where | Note |
|---|---|---|
| Per-frame tick | `hleEnterVblank()` in [Core/HLE/sceDisplay.cpp](Core/HLE/sceDisplay.cpp) | Once per vblank on the emu thread, next to the existing `g_controlMapper.UpdateAutoMovements` call |
| Init | `__KernelInit()` in [Core/HLE/sceKernel.cpp](Core/HLE/sceKernel.cpp) | After `__CtrlInit`, since it drives sceCtrl |
| Shutdown | `__KernelShutdown()` in [Core/HLE/sceKernel.cpp](Core/HLE/sceKernel.cpp) | *Before* `__CtrlShutdown`, so held buttons get released while sceCtrl is alive |
| Host keys | `NativeKey()` in [UI/NativeApp.cpp](UI/NativeApp.cpp) | Inside the existing `passKeyThrough` branch, which already handles ImGui capture and UI-vs-ingame gating |
| Mouse look | `NativeMouseDelta()` in [UI/NativeApp.cpp](UI/NativeApp.cpp) | Same claim pattern as keys; skipped while the ImGui debugger wants the mouse |
| Compat flag | [Core/Compatibility.h](Core/Compatibility.h) + `.cpp` + [assets/compat.ini](assets/compat.ini) | One struct field, one `CheckSetting` line, one `[VCSInputOverhaul]` section |

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
on the same thread, regardless of graphics backend (see the upstream threading section above). So
the VCS debugger window reads PSP memory directly, with no marshalling and no snapshot buffer,
and still satisfies the emu-thread-only rule. Don't "fix" this by adding a lock; the legacy Win32
debugger is the one that needs `g_frameMutex`, not this.

## Status

**Working and verified on the real game:**

- Disc ID detection, compat-flag gating, init/shutdown lifecycle.
- Per-frame tick (the VCS window's tick counter climbs — a frozen counter means the vblank hook
  is broken, which is a different failure from a wrong address).
- The debugger window: live state, the address table, the scratchpad, the mapping table, the
  camera diagnostics.
- Scratchpad reading real memory and rendering `-` for out-of-bounds addresses without faulting.
- The `NativeKey` hook and the full input chain: physical key → `HandleHostKey` →
  `ResolveContext` → `ComputeButtonMask` → `sceCtrl`, verified end to end (Space on foot shows
  context `OnFoot` and mask `0x00008000`, `CTRL_SQUARE`, Jump).
- **Context-aware bindings**, confirmed in play. On foot and in vehicles behave differently.
- **WASD movement**, confirmed in play, on foot and driving. Analog on foot; in a vehicle A/D
  steer the stick while W/S stay buttons.
- **Mouse look.** Yaw on foot and in vehicles; pitch on foot only (see the pitch section below).
  Not during free aim — there the mouse is the reticle instead.
- **Lock-on aiming**, which is how every ordinary weapon in VCS aims. Holding the aim key gives
  the aim bindings (Q/E cycle targets), WASD strafes, and the mouse keeps driving the camera.

- **Mouse free aim, yaw and pitch both tracking**, with ordinary weapons. Requires the
  "Drive the game's second stick" toggle, which sets the mode flag. See "Mouse free aim — how it
  actually works" for the three conditions and the two channels involved.

- **Free aim that feels like a mouse rather than a thumbstick**, confirmed in play. The game's
  axis→rotation response is a signed square with an exponential smoother on top; `aimResponseModel`
  inverts both, so mouse displacement becomes aim displacement the way it already does for mouse
  look. Read out of the game's own code rather than guessed — see "Why free aim felt like a
  thumbstick, and what fixed it".

- **Lock-on no longer appears at all** when aiming. It was never the game being stubborn: the Free
  Aim press waited 9 ticks after the aim trigger, which at 60Hz against a 30fps game is ~150ms of
  open season for the game to find a target. `aimFreeAimDelay` is 0 now — Free Aim is pressed on
  the same tick as the trigger. No memory writes, no fighting the targeting code; the bug was ours.

**Addresses found so far**, all verified stable across a fresh boot (the four `Pad*` entries are
documented in "VCS has a second analog stick" below rather than repeated here):

| address | value | how it was confirmed |
|---|---|---|
| `PlayerVehicle` | `0x08bb4064` | 3-snapshot intersect; position matched the player, not traffic |
| `PlayerBase` | `0x08bc8170` | entity matrix whose position tracks the player |
| `PlayerHealth` | `PlayerBase + 0x4e4` | wrote `25.0`, HUD bar dropped to exactly a quarter width |
| `CameraYaw` | `0x08bc7f1c` | correlated across 7 snapshots, then write-tested (62% of screen changed) |
| `CameraPitch` | `0x08bc7f18` | same correlation; sits 4 bytes *before* yaw |
| `IsAiming` | `0x08bb32a0` | 1 only while **locked on** |
| `IsFreeAiming` | `0x08bafb54` | 1 only while **free-aiming** (sniper/RPG); counted aim cycles matched transitions exactly |
| `TimeStep` | `0x08bb3b5c` | `gp + 0x1dfc`. Read as a float by every camera mode that integrates the look axis, e.g. at `0x089a37f8`. Reads 1.668 in gameplay and 0.0 at the menu |
| `FrameCounter` | `0x08bb3bb4` | `gp + 0x1e54`. The `lw / addiu 1 / sw` at `0x08a114c4`, last thing `CTimer::Update` does. 11621 in an in-game savestate, 0 at the menu |

### Confirm an address by writing to it

The fastest way to test a candidate is to poke a value in and watch the game react — no need to
manufacture the in-game event at all. Three candidates that all read exactly `100.0` were ruled
out as health this way in about a minute. `Tools/vcsscan.py` plus a `memory.write` over the
WebSocket debugger is all it takes. A candidate the game immediately overwrites is derived from
something else; one that sticks is a real stored value.

### Aiming is a stick, not a camera

The tempting reading of "mouse aiming" is *mouse = crosshair*: point at a thing, shoot it. That is
not what the PSP does, and building it that way cannot work. The real chain is:

```
aim held  ->  weapon aiming mode  ->  nub moves the reticle, relative to the view
          ->  the RETICLE decides the firing direction  ->  fire
```

The consequence that matters: **the reticle, not the camera, is what the game shoots along.**
Writing `CameraYaw` while aiming would swing the view and leave the gun pointing at whatever the
game last aimed it at. So mouse look during aiming isn't merely stylistically wrong — it cannot
aim at all. While free aim is engaged, the mouse delta goes to the analog stick and `VCSCamera`
writes nothing.

**But this only applies to free aim, which in VCS means the sniper rifle and the RPG.** Read the
next section before touching any of it — that scope limit is the single most important fact here,
and building the model without it produces something actively broken.

This is the same lesson vehicle glances already taught: drive the mechanic the game actually
reads, not the camera angle that superficially resembles it. Anywhere those two diverge, the
mechanic wins.

The PC mapping this produces:

| Host input | Meaning | PSP equivalent |
|---|---|---|
| Right mouse, held | Enter/hold weapon aiming | the aim trigger |
| Mouse movement | Move the reticle | analog nub |
| Left mouse | Fire | Circle |

**The nub is a rate control, not a position control.** Holding it deflected *sweeps* the reticle
continuously; it doesn't jump it somewhere. There is no address to write an absolute crosshair
position to — that part is settled, see "There is no stored aim direction" below.

**But "feed it this frame's mouse delta and total travel tracks total mouse travel" was wrong**,
and it was wrong for a reason worth understanding rather than patching around. It assumed the game
integrates the axis linearly. It does not — it squares it. That claim stood for a long time because
it is *almost* right: the shape of the mapping is correct, and only the game's response curve
breaks it. See "Why free aim felt like a thumbstick" below for the measured formula and the fix.

### Why free aim felt like a thumbstick, and what fixed it

This is the answer to the complaint that outlived every other one: mouse look feels PC-native,
free aim feels like emulating a stick. It was never a tuning problem, and every attempt to solve
it by choosing a better sensitivity was doomed before it started.

**The game's weapon-aim camera is `CCam` mode 45**, dispatched through the table at `0x08b7ed88`
into `Process` at `0x089a341c`. Once per frame it does exactly this with the look axis:

```
n     = axis / 128                                        0x089a37d4
d     = n * |n| * (FOV / FOVref) * 0.04 * timeStep        0x089a37f0 .. 0x089a3804
alpha = pow(|axis| < 2 ? 0.5 : 0.9, timeStep)             0x089a38cc .. 0x089a3920
inc   = alpha * inc + (1 - alpha) * d * 0.8               0x089a3924 .. 0x089a3940
Beta += inc                                               0x089a3960 .. 0x089a3968
```

Three separate distortions sit between the mouse and the aim, and all three read as "thumbstick":

| | what it does | why it reads as a stick |
|---|---|---|
| `n * \|n\|` | a signed **square** | travel goes with the sum of the *squares* of the per-frame deltas. Moving 10 counts in one frame rotates **100×** as far as 1 count in each of ten frames. Slow precise movement does nearly nothing; a flick overshoots and then clips. |
| `alpha` | an EMA on the increment, ≈0.84 at a typical timestep | ~6 frames to reach the rate you asked for, and ~6 frames of **glide** after you stop. Mice do not coast. |
| `±127` | saturation | fast movements are truncated outright. |

The square is the one that matters, because **it is a shape, not a scale**. No value of
`aimSensitivity` can flatten a quadratic. That is why this survived so many rounds of retuning —
each round correctly observed that the current value felt wrong, and then changed the one parameter
that could not help.

**The fix is to invert the formula rather than feed it.** `AimAxisStep` in `VCSCamera.cpp` asks
"how far should the aim move this frame" and solves backwards for the deflection that produces it:
undo the EMA (which needs a mirror of the game's `inc`, so we know what it is carrying), undo the
square, clamp, and carry whatever saturation prevented delivering into the next frame. The result
is a position control reached through the mechanic the game actually reads — which is the same
doctrine the rest of this file preaches, applied to a curve instead of to an address.

The best part is what falls out of the EMA inversion for free: when the mouse stops, `want` is 0
while the mirror is not, so the solved deflection is **negative**. It pushes back against the
smoother and kills the glide in a single frame, instead of letting the aim coast to a halt. That
glide is the single most viscerally "controller" part of the old behaviour.

Only `TimeStep` (`gp + 0x1dfc`, `0x08bb3b5c`) had to be added to the address table. The FOV term
divides out, because we *want* the applied rotation to scale with FOV — zooming in should aim
finer, exactly as it does in any mouse-aimed shooter.

**Saturation is the other half, and it sets the sensitivity.** Inverting the curve makes the
mapping linear only up to where the axis saturates. The game's aim rate tops out near
`kAimRate * timeStep` ≈ 0.053 rad/frame, and on the nub the deflection cannot exceed 1, so the
ceiling is low: at 0.004 rad/count — the look sensitivity — the solve saturates at **13 counts in
a frame**, which ordinary mouse movement passes constantly, and the response flattens again
exactly where it was supposed to be linear.

So `aimSensitivity` defaults to **0.0005**, about 8× below the look sensitivity, which saturates
only around 107 counts/frame. That is roughly 23°/inch at 800 DPI — an ordinary aiming
sensitivity, where 0.004 gave an unusable 183°/inch. It was found by tuning in play and landed on
the slider's old minimum, which is generally how a badly chosen default announces itself.

**Free aim is therefore slower than looking around, and that is the game's limit, not a bug.**
`aimRangeBoost` (default 3×) is the only lever on it — but it applies **only to the d-pad
channel**, because `__CtrlSetAnalogXY` runs the nub through `clamp_u8` before the game ever sees
it. On the default configuration, with `usePadStick` off, the boost slider does nothing whatsoever.
The Camera tab says so in red, because it is the obvious control to reach for when aim feels
rate-limited and it is silently inert.

Simulating the model against the game's own formula — same total mouse travel (60 counts, target
13.75°), redistributed across frames, in degrees:

| counts/frame | old mapping | model @1× | model @3× |
|---|---|---|---|
| 1 | 0.04 | 13.75 | 13.75 |
| 5 | 0.13 | 13.75 | 13.75 |
| 10 | 0.18 | 7.98 | 13.75 |
| 20 | 0.24 | 2.65 | 13.75 |
| 30 | 0.28 | 1.40 | 12.59 |

The old column is the whole complaint in one place: a 7× spread across distributions that should
all be identical, and every one of them 50–350× short. The "1 count per frame" row is what "slow,
careful aiming does nothing" was.

Rotation still happening *after* the mouse stops — the most viscerally stick-like symptom:

| | old | model @3× |
|---|---|---|
| slow drag | 0.003° | 0.000° |
| medium | 0.026° | 0.000° |
| hard flick | 0.207° | 5.0° |

Note the old mapping "wins" the last row, and it is not a real win — it only ever moved 0.28° in
total, so it had nothing to coast with. The 5° is the game's smoother carrying momentum we can
only bleed off at the rate the axis allows.

The simulation lives in the scratch work rather than the repo; it is ~60 lines and is worth
rebuilding if these constants are ever retuned, because it catches shape errors that play-testing
would take a long time to isolate.

#### A tick is not a game frame

`TimeStep` reads **1.668**, which is `50 / 30` — GTA's timestep is in 50fps reference units, so
this game runs its logic at **30fps**. `VCS::Tick()` runs from `hleEnterVblank`, which is ~60Hz.
So a tick is not a frame, and the model gets asked for a deflection about twice as often as the
game will use one.

That broke two things at once, and the second is worse than the first:

- The mirror advanced twice per game frame, decaying as `alpha^2` where the game's own increment
  decays as `alpha`. Our idea of what the game was carrying drifted from the truth — and the
  mirror is exactly what the anti-glide correction is computed from.
- The first tick's mouse movement was drained, solved, written, and then **overwritten by the
  second tick's write** before the game ever sampled the pad. Half the input, silently discarded.
  That half predates the model; it applied to the old proportional mapping too.

**Fixed by running the model on the game's clock.** `FrameCounter` (`gp + 0x1e54`, `0x08bb3bb4`)
is incremented by the `lw / addiu 1 / sw` at `0x08a114c4`, the last thing `CTimer::Update` does.
`AimModelBeginFrame` only opens a new frame when it moves; between logic frames the accumulated
mouse movement is left alone and the previous deflection re-asserted, which is what the game is
going to read anyway.

Both places that drain the delta had to learn this — `ApplyAnalog`'s reticle branch and
`CameraTick`'s unconditional drain. Miss either and the movement is still lost, just one function
further along.

The Camera tab shows game frames against skipped ticks; **the ratio should sit near 1:1**, and it
is the check that the counter is really the counter rather than something that merely looked like
one.

With `FrameCounter` unset this degrades to solving every tick — wrong in the two ways above, but
working, which is the rule the whole address table follows.

#### Solve against what the CHANNEL can deliver, not what the setting allows

The mirror is built from the deflection the model settled on, so that value has to be the one the
game actually receives. `aimRangeBoost` only applies to the d-pad channel — the nub goes through
`__CtrlSetAnalogXY`, which `clamp_u8`s it — so solving to 3.0 and then having `ApplyAnalog` write
1.0 makes the mirror record **nine times** the rotation that happened. The square sees to that.

The next frame the model faithfully corrects for movement the game never made, and the correction
is *backwards*. Simulated over a slow mouse landing on alternate game frames, per-frame rotation in
degrees:

```
solving to 3.0, nub writes 1.0:   +0.49  -0.08  +0.43  -0.14  +0.38  -0.17  +0.35  -0.20
solving to 1.0, nub writes 1.0:   +0.49  +0.91  +1.25  +0.58  +0.98  +0.85  +1.21  +0.62
```

Seven backwards frames out of fourteen, while the mouse moves steadily forwards. In play it reads
as the aim snapping back a pixel as you move — a symptom nowhere near its cause. `AimChannelLimit`
exists for this, and the Camera tab prints the limit next to the deflection so the two can be
compared at a glance.

Worth noting how it stayed hidden: while the model was still stepping twice per game frame, the
mirror also decayed twice as fast, and the two errors partly cancelled. Fixing the frame sync is
what made it visible. **Two bugs that mask each other look like no bug, and fixing either one
alone looks like a regression** — which is exactly what it looked like from the outside.

The systematic errors that remain all lean the safe way, and it is worth knowing which way that
is: `CPad + 0xd0` (1.05) and the nub's 127/128 quantisation mean the game's real increment is
slightly *larger* than the mirror thinks. Under-estimating makes the model under-cancel, which
leaves a trace of glide. Over-estimating makes it push backwards. Glide is the far better failure,
so if this ever needs fudging, fudge it low.

#### The model is for lock-on weapons only. Sniper and RPG must not go near it

**Sniper and RPG never had lock-on**, are always manually aimed, and do not aim through the
camera's squared response at all — their reticle is driven directly, and linearly. They report
weapon camera mode **7** and **8** (`CCamera + 0x7b8`), which is how `aimScopedLinear` recognises
them.

Pointing the model at them is not a mild mistuning, it is actively destructive, and the reason is
worth internalising: **the model's entire job is to invert a square.** Applied to something already
linear, the `sqrt` cancels nothing and simply runs — and a `sqrt` makes *small* inputs
disproportionately large. One count of mouse movement asks for a deflection sized for a much
bigger one. In play the scope takes off on its own from the slightest touch.

They aimed correctly *before* the model existed, which is the whole proof and was the observation
that identified this. When a change makes one thing better and another worse, the thing that got
worse is evidence about what the change actually does.

So: model for the lock-on weapons, plain proportional mapping (`aimScopedSensitivity`, 0.045) for
these two. If some other weapon ever behaves the same way, the fix is its weapon mode number added
to that check — get the number from the Camera tab rather than inferring it.

#### Two dead ends recorded, because both looked right

Chasing this, weapon mode was measured across the arsenal and the result killed the theory it was
gathered to support:

| weapon | weapon mode | FOV |
|---|---|---|
| Pistol, SMG, AR | 11 | 70 / 70 / 50 |
| Sniper | 7 | 70 |
| RPG | 8 | 70 |

Pistol and SMG misbehaved while the **AR shared their mode and did not**, so camera mode could not
be the discriminator and a fix keyed on it would have done nothing. That one was caught before it
was built, only because the numbers were measured rather than assumed.

The FOV scaling in `AimAxisStep` (`want *= fovScale`) was then added to explain the sniper problem
and **did not fix it** — the cause was the sqrt, above. It is kept because it is right on its own
terms: both response families genuinely scale their rate by `FOV / 80`, so cancelling it keeps
on-screen speed constant across zoom, which is what a mouse wants. But it was shipped as a fix for
something it had nothing to do with, and it has never been verified in play. **Treat it as
unproven**; if scoped aim feels wrong in some new way, suspect it first.

This is the correction that mattered most, and the lesson generalises well beyond aiming.

The model originally carried its own copy of the game's smoothed increment and its own idea of the
response constants. That copy is what the anti-glide correction is computed from — so **every
error in it, however small, became a push in the wrong direction.** Three accumulated:

| unmodelled | effect on the prediction |
|---|---|
| the live camera mode | modes 11/28 smooth at `alpha` 0.8, not 0.9 — predicted momentum decays too slowly |
| `CPad + 0xd0` = 1.05 | the game squares it, so its real response is ~10% stronger than predicted |
| a deflection clamped after being recorded | mirror records rotation that never happened |

All three inflate the prediction, and an inflated prediction makes the stop **over-cancel** — the
aim does not stop, it reverses. Symptom in play: *"I move 50px left and at the end it snaps back
5px."* Roughly a 10% shortfall, which is about what the axis scale alone accounts for.

**Every one of those values is stored in memory.** The model now reads them:

| | |
|---|---|
| `CamAimIncX` / `CamAimIncY` | the real smoothed increment — the momentum being cancelled |
| `CamMode` | picks the response family, and therefore `alpha` |
| `CamFOV` | both families scale by `FOV / 80` |
| `AimAxisScale` + `WeaponCamMode` | the 1.05, and whether it applies at all |
| `LookSensitivity` | squared, in the mode 11/28 rate |

Anchoring to the real increment every frame means **no error can accumulate at all** — a wrong
constant now costs a fraction of one frame's correction instead of compounding. The mirror survives
only as a fallback for when the reads fail.

Two backstops on top, because over-cancellation is the one failure with a distinctive and
unpleasant signature and its cause sits several layers from its symptom:

- `applied` is forced to zero if it would reverse the aim while the mouse is not asking it to.
- `aimCancel` ("Stop strength") is a 0..1 knob on how much momentum to cancel, so any residual
  backwards motion can be dialled out in play without a rebuild.

**If in doubt, under-cancel.** Coasting slightly is a mild, familiar failure; the aim walking
backwards while you move forwards is not.

#### The stop fired at the wrong time, not with the wrong strength

Tuning in play first drove `aimCancel` down to **0.10** — mostly off. The tempting conclusion, that
the cancellation must still be miscalibrated, was wrong: at 1.0 it is exact, because the momentum
it cancels is read out of the game rather than predicted.

The flaw was *when* it fired. It treated any frame with a zero mouse delta as "the stroke ended",
and at 30fps that is a bad assumption: a game frame is 33ms, and slow careful aiming genuinely
produces zero-count frames **mid-stroke**. So the brakes came on repeatedly during movements, not
only after them. That is the stutter — and **a perfectly calibrated cancellation does it just as
badly**, because accuracy cannot fix a timing error. Detuning the strength was the only lever
available at the time, and it was treating the symptom.

**Fixed by separating the two questions.** `aimStopDelay` (default 2 frames, ~66ms) is how many
consecutive still frames count as a stroke ending; `aimCancel` (back to 1.0) is how hard to stop
once it has. Longer than a gap in real mouse movement, shorter than a noticeable coast. The cost of
waiting is small — the game's smoother has only decayed to about 70% over two frames, so most of
the coast is still there to cancel when the stop does fire.

Stillness is tracked for the **mouse**, not per axis. The player stopping is one physical event;
tracking it per axis would call a pure sideways stroke "still" vertically and brake an axis
mid-movement, which is the exact failure being removed.

The Camera tab shows `Stroke: moving / ended` live. If it reads "ended" while you are still moving,
the delay wants raising — that is the stop firing mid-stroke.

Sensitivity went `0.004 -> 0.0005 -> 0.00128` across these rounds, and the path is diagnostic
rather than fickle: the 0.0005 was compensating for a build that discarded half the mouse movement,
and it rose again once the frame sync made the input actually arrive. **A setting that has to move
a long way after a bug fix is evidence about the bug** — and a setting that has to be detuned to a
tenth of its design value, as `aimCancel` was, is evidence that something structural is wrong
rather than something numerical.

The general lesson, which is the same one the `AimYaw` hunt taught from the other direction: a
prediction of the game's state is a liability that compounds, and this game stores far more than it
looks like it does. Before modelling a value, spend ten minutes checking whether it can simply be
read.

**Two long-standing claims died here, both from the same measurement error:**

- *"Free aim reads these fields as a button, not as an analog axis."* No. The evidence was real —
  arrow keys put 255 in and the aim moved, our writes of 26–50 did nothing — but the conclusion
  was not. An axis of 13–25 out of 127 produces about **1/400th** of the rotation 255 does, once
  squared. It was always analog; it was just being asked for an invisible amount.
- *"Clamped to ±127."* Nothing clamps. The accessor at `0x0898bb4c` subtracts the pair, halves it
  and sign-extends — that is all. 255 is merely the largest value a real d-pad produces. Writing
  past it gives a genuinely larger axis, which is what `aimRangeBoost` exists to exploit: the
  game's own aim rate caps out near 0.05 rad/frame, slower than mouse look, and this is the only
  lever on it.

Both mistakes share a root cause: a correct observation about *symptoms* was promoted to a
conclusion about *mechanism*, without reading the code that produced it. The code was ~40 lines
and answered both in one sitting.

### Movement in free aim is LATCHED, not forbidden — and the patch is a brake

**This supersedes the section below, which concluded the game simply forbids it. That was wrong,
and wrong in a way no amount of disassembly would have corrected**, because the code is identical
in both cases. It only shows up in play.

Enter free aim *while already running* — sprint, release sprint, keep the movement key held, then
aim — and the player **keeps running, with the correct running-with-weapon animation**. Movement
was never impossible in free aim. Only *changing* it was.

The reason is the branch at `0x0894b864` (`MoveGateBranch`): free aim skips the movement call at
`0x0894b888`, so nothing ever re-evaluates the movement. Whatever state you entered with is latched
and applied forever, which is also why it could not be stopped.

**So the patch is a brake, not an accelerator, and that inversion is the whole trick.** There is
nothing to start — the latch does that. What was missing was a way to stop, and letting the
movement call run again is exactly that: it re-reads the stick, finds it centred, and halts the
player. Hence:

```
holding the movement key  ->  patched OUT, latch keeps carrying you
key released              ->  patched IN, the call runs and stops you
```

Confirmed in play. The game does the movement, the animation and the collision; this fork only
decides *when* the game is allowed to re-evaluate.

Two approaches were tried and abandoned first, both recorded in docs/VCS_ADDRESSES.md: writing
`PedVelX/Y` (impossible - the game zeroes and recomputes velocity every frame from the very intent
free aim suppresses) and translating `PedPosX/Y` directly (works, but it is a noclip: no animation,
no slopes, and collision only as far as the physics resolves interpenetration afterwards).
`freeAimTranslate` still exists for comparison and is off.

**The second stick cannot be part of this.** Setting `CameraInputMode` makes the d-pad the aim
axis, which makes d-pad-down indistinguishable from aiming downward - so the game's Free Aim button
becomes unpressable and free aim is unreachable. Aim on the nub, movement latched, patch as brake.

The lesson worth carrying: "the feature is absent" and "the feature is present but frozen" produce
identical symptoms from the outside, and the second is a far easier problem. Ask which one it is
before concluding anything is impossible.

### Superseded: "free aim has no movement channel"

Established in play, not inferred:

- The single analog axis **is** the crosshair in free aim.
- The **d-pad is inert** there — arrow keys do nothing. (Checked that the test was valid first: the
  Aiming context doesn't claim `NKCODE_DPAD_*`, only the never-triggered Menu context does, and
  PPSSPP does bind arrows to the d-pad by default. A test that our own layer swallows proves
  nothing.)

So there is no input to route. Nothing we feed the game can produce movement while free-aiming,
because the game accepts no such input — which is exactly the finger-gymnastics problem the PSP
scheme has, resolved by the game simply not allowing it.

`moveInFreeAim` (off, experimental) therefore writes `PedVelX/Y` directly. That is a different and
heavier class of change from everything else in this fork: every other feature feeds the game
inputs it already understands, and this one **overrides its physics**. Judge it accordingly - the
walk animation will not play, and sliding or collision oddities are the expected failure.

`Tools/vcsstatic.py` could not have found this. The player control function is 1665 instructions
and the movement maths is VFPU, which capstone does not decode - listings show `?vfpu?` exactly
where the answer is. **That is the boundary between the two tools**: static disassembly for control
flow and scalar constants, the live debugger for anything the vector unit computes.

### The crosshair is drawn from the camera; the shot is not

Re-tested directly, because the old note that writing `CameraYaw` "swings the VIEW, gun keeps
pointing where it was" was doubted here on the grounds that it predated knowing the camera modes.
It did predate that. **It was right anyway.**

Driving `CameraYaw`/`CameraPitch` in free aim — the exact path that makes mouse look smooth — moves
the crosshair, smoothly, and **the shot goes somewhere else**. So the crosshair is *drawn* from the
camera while the shot is resolved from the ped's own aim state, and the two agree only because the
stick normally drives both. Steering the camera alone desynchronises them, which is worse than
doing nothing: it looks correct and misses.

`mouseLookInFreeAim` is off, and stays visible in the UI marked DISPROVEN so the result is not
rediscovered by someone reasoning their way to the same idea.

**The consequence is the important part: the stick is the only path to the gun.** The game's
integrator cannot be removed from aiming, only inverted — so `aimResponseModel` is not a
workaround for lacking a better route, it *is* the route. That also caps how smooth aiming can
ever be against mouse look, which writes an angle with nothing in between.

Method note, since this cost a build: re-testing the old claim was right, and *assuming it was
wrong* was not. A doubted note is a hypothesis, not a fact to build on.

### There is no stored aim direction

`AimYaw` / `AimPitch` are permanently unset, and that is a **result, not a gap**. Do not resume
the hunt; five rounds of value-correlation failed because the thing being searched for does not
exist.

Mode 45 does not store an aim direction anywhere. It integrates the look axis straight into the
camera's own `Beta`/`Alpha` — which this table already knows, as `CameraYaw` and `CameraPitch` —
and the gun is resolved from that plus ped state every frame. So "writing `CameraYaw` swung the
view but the gun kept pointing where it was" was recording a real observation about the *ped* side
of that resolution, not evidence of a second angle waiting to be found.

Layout established while settling this, all read out of the code rather than correlated:

| | |
|---|---|
| `CCamera` | `0x08bc7e30` |
| `CCamera + 0x50` | active cam index (u8; observed 0 in gameplay) |
| `CCamera + 0x70` | `CCam m_asCams[3]`, stride `0x260` — so `cam[0]` is `0x08bc7ea0` |
| `CCam + 0x00` | mode (s16). 45 and 11 are the two weapon-aim modes |
| `CCam + 0x78` / `+0x7c` | pitch / yaw — i.e. this fork's `CameraPitch` / `CameraYaw`, on `cam[0]` |
| `CCam + 0x124` / `+0x130` | the smoothed per-frame increment added to those |
| `CCam + 0x128` | FOV |
| `CCamera + 0x7b8` | `PlayerWeaponMode.Mode` (s16) |

The index math is at `0x08a228a0` (`idx * 608 + 0x70`) and the mode jump table at `0x08b7ed88`.
Mode 15 — the ordinary on-foot follow camera — routes to `0x08999f60`, which **never reads the
look axis at all**. That is why writing `CameraYaw` works so cleanly for mouse look and so badly
for aiming: on foot there is no integrator to fight, and in free aim there is.

### Read the game's code without running the game

`Tools/vcsstatic.py` extracts PSP RAM from a savestate and disassembles it offline. Everything in
the two sections above came out of it, in one sitting, with the emulator closed.

```bash
python Tools/vcsstatic.py memstick/PSP/PPSSPP_STATE/ULUS10160_1.03_0.ppst --disasm 0x089a341c 60
```

This is strictly better than `vcsdisasm.py` for reading code: no running game, no WebSocket, no
needing to reach the game state you want to inspect first. `vcsdisasm.py` is still the right tool
for looking at code *while* something is happening; this one is for working out what the code
does. A savestate taken during gameplay also carries live values, so `--read` doubles as a way to
check a candidate address without booting anything.

Prefer this over correlation. Two functions read this way answered a question that five rounds of
snapshot-diffing had got wrong.

### Read the game's own Controls screen first

**Pause → Controls documents the control scheme, and reading it would have saved most of a day.**
It shows, on one page:

- `Free Aim` on **d-pad down** — a button, not a mode to be synthesised
- `Look/Fine Aim` on **L trigger** — the modifier that looked "unused" in the button tester
- `Cycle Weapons/Targets` on d-pad left/right
- `PLAYER MOVEMENT: ANALOG STICK / DIRECTIONAL BUTTONS` — which is all `gp-0x3F00` ever was
- `INVERT LOOK`, and four `CONFIGURATION` setups that rearrange the above

Everything below this section was worked out from disassembly, memory tracing and live
experiment, and most of it was rediscovering that page. The flag hunt, the d-pad-as-analog
writes, the CLEO plugin, the threshold theories - none of it was needed to reach free aim.

Before inventing a mechanism for this game, open the menu and see whether the game already has
one. The same applies to anything with an options screen.

### Free aim is a button

`Free Aim` = **d-pad down**, bound to `S` in the Aiming context. Verified in play: with aim held,
d-pad up does nothing, left/right cycle targets, and **down** enters free aim - after which the
mouse moves the crosshair through the ordinary reticle path (mouse → nub), with no flag set and
no second-stick machinery at all. It works with "Drive the game's second stick" **off**.

Character movement is unavailable in free aim, which is expected: the nub is the aim.

The sections below record how this was approached before that was known. They contain real and
still-correct findings about the pad layout and the flag, but `usePadStick`, `CameraInputMode`
and the CLEO route are **not needed for free aim** and should not be reached for first.

### Mouse free aim — how it actually works

**This section replaces an earlier one that concluded VCS has no free aim for ordinary weapons.
That was wrong.** `docs/VCS_ADDRESSES.md` still carries the original negative result — *"nothing
flipped reliably when aiming at empty space"* — and it misled work here for a long time. Free aim
exists for ordinary weapons, with a crosshair, and it is reachable. Mouse yaw and pitch both track
in it. Treat the old claim as superseded wherever it still appears.

Three things have to be true at once, and every one of them was found the hard way:

**1. The mode flag must be set.** `CameraInputMode` (`gp-0x3F00`, `0x08bade60`) ships as 0. Set it
to 1 and free aim becomes reachable; leave it and it isn't. See the second-stick section below for
what the flag selects and why it is a game *setting* rather than a constant.

**2. Free aim has to be entered, and lock-on is the way in.** Hold aim, then break the lock with a
large aim deflection — shaking the mouse does it. That is standard GTA behaviour, not a bug. An
earlier version of this section claimed pressing S did it; that was inferred, never observed, and
is wrong.

**3. Both channels must be driven, because they carry different axes.** This is the part that took
longest:

| axis | channel | driven by |
|---|---|---|
| yaw | nub, `CPad+0x2` / `+0x4` | `ReticleActive` → `ApplyAnalog` |
| pitch | d-pad up/down, `CPad+0x12` / `+0x14` | `PadStickActive` → `PadStickTick` |

They are **complementary, not alternatives**. Driving one gives half an aim — the symptom being
pitch that only responds to the arrow keys, since those fall through to PPSSPP's defaults as a
d-pad. The two paths therefore run together in free aim and share one delta: whichever consumes it
first calls `TakeMouseDelta`, the second calls `PeekMouseDelta`. Don't make the second one drain,
and don't make them exclusive again.

**Route on `IsAiming`, never `IsFreeAiming`.** A live trace settled this:

- `IsAiming` is exactly right — 1 for the whole lock-on and 0 either side. Inverted, within the
  Aiming context, it means free aim.
- `IsFreeAiming` read 1 for nearly the entire trace including while locked on. It is junk for this
  purpose, exactly as its `SUSPECT` note warns. Four different signals were tried here before
  measuring; measuring took one run.

**Confine the d-pad writes to free aim.** Those four fields double as the d-pad *buttons*, and the
button meanings are destructive: left/right is previous/next target while locked on, and
previous/next weapon on foot. A trace caught us writing 255 during lock-on, which the game read as
holding the button — so moving the mouse cycled through NPCs. Free aim is the one state where
those buttons have nothing worse to do. `PadStickActive` is gated accordingly; widening it will
bring the target-cycling straight back.

**~~Keep sensitivities below saturation.~~ Superseded — see "Why free aim felt like a thumbstick".**
The observation was real: a trace showed `nubX`/`nubY` pinned at ±126 through every movement, which
does collapse every mouse speed above a nudge to one rate. The prescription that followed it —
lower the sensitivity until it fits — was treating the symptom, and it traded snapping for a dead
zone at the other end, because the response is quadratic. `aimResponseModel` solves the actual
problem and carries the overflow into following frames instead, so saturation stops being
something to tune around. The two channels are still sized so that ordinary movement doesn't sit
at the stop, but that is now a consequence rather than a goal.

One thing from the old prescription does survive: staying off the stop is still the lever against
the dual-purpose button problem above, and it is a real reason not to raise `aimRangeBoost`
further than free aim needs.

### VCS has a second analog stick, synthesised from the d-pad

This is the important one, and it supersedes both the reticle work and the plugin route above.

**VCS internally expects two analog sticks.** The PSP has one, so the game builds the second out of
the d-pad. Two pad functions do it, and they disassemble cleanly:

```
0898bb4c  lbu   a1,-0x3F00(gp)      ; mode flag
0898bb50  beq   a1,zero,0x0898BB80  ; flag==0 -> read a real axis at +0x2 instead
0898bb58  lh    a1,0x18(a0)         ; a0 = CPad
0898bb5c  lh    a0,0x16(a0)
0898bb60  subu  a0,a1,a0            ; [+0x18] - [+0x16]
0898bb64..70                        ; round-toward-zero divide by 2
```

So **camera X = `(DPadRight − DPadLeft) / 2`** and, at `0x0898bb8c`, **Y = `(DPadDown − DPadUp) / 2`**.
~~Clamped to ±127.~~ **Not clamped at all** — the tail is a halve and a sign-extend to s16, nothing
more. ±127 is only the range a real d-pad can reach. See `aimRangeBoost`.

The fields are `int16`. A real d-pad only ever puts **0 or 255** in them, so a player can produce
exactly three axis positions: −127, 0, +127. **Nothing in the arithmetic requires that** — not the
intermediate values, and not the endpoints either. Writing values in between yields a genuine
analog axis; writing values beyond 255 yields an axis past ±127 and, through the square, a
proportionally higher aim rate. Since this is the game's own camera and aim input, it drives both,
through code the game already has. No plugin, no function hook, just a memory write.

There are in fact **four** of these accessors, not two: `0x0898bb4c` / `0x0898bb8c` read the current
frame's pad, and `0x0898bbcc` / `0x0898bc0c` read the previous frame's copy at `+0x32`. Callers use
the pair to detect edges. Both pairs consult the same mode flag.

Neither is where the axis is scaled. That happens one level up, in `0x0898de2c` (X) and
`0x0898df58` (Y), which multiply by `CPad + 0xd0` (read 1.05) — but **only** in weapon camera
modes 45 and 11, and only when `CPad + 0xa` is 0. The Y one also reads `gp - 0x3F01`, which is the
Controls menu's **INVERT LOOK**. Anything reasoning about the aim scale wants those two functions,
not these four.

`CPad[0]` is at **`0x08bde610`**, lifted out of `CPad::GetPad` at `0x0898b428`:

```
sll a1,a0,0x5 ; subu a0,a1,a0 ; sll a0,a0,0x3 ; subu v0,a0,a1   -> index * 216
lui a0,0x8BE  ; addiu a0,a0,-0x19F0                             -> 0x08bde610
addu v0,v0,a0
```

Field offsets confirmed live by injecting each d-pad button over the WebSocket debugger and
watching the halfword go to 255 — `+0x12` Up, `+0x14` Down, `+0x16` Left, `+0x18` Right. Do that
again rather than trusting this table if anything ever looks off; it takes about a minute
(`input.buttons.send` plus `memory.read`).

Implemented as `usePadStick` (Camera tab, "Drive the game's second stick"), **off by default** —
it's a different mechanism from the direct `CameraYaw` writes and the two want comparing rather
than assuming. Deflection is a turn *rate*, so a frame's mouse movement maps to how far the stick
is pushed that frame, and it self-centres when the mouse stops.

**The `gp-0x3F00` flag is the load-bearing part, and it ships as 0.** Read live at `0x08bade60`
(gp measured as `0x08bb1d60`). Zero means *both* functions take the `beq` branch and never touch
the d-pad at all — they read `CPad+0x2` and `CPad+0x4` instead. So writing the d-pad fields on
their own accomplishes exactly nothing; the first version of this code did that and would have.

And `CPad+0x2` / `+0x4` turn out to be **the nub**, confirmed by injecting each stick over the
debugger and watching the halfwords:

| stick | lands in |
|---|---|
| left X | `CPad+0x2` |
| left Y | `CPad+0x4` |
| right X/Y | **nowhere** — the right stick never reaches CPad |

That second row kills the right-stick idea outright: PPSSPP carries a right stick in `SceCtrlData`,
but VCS's pad update doesn't copy it anywhere, so `aimViaRightStick` has no receiver without a
plugin. And the first row explains the symptom that started all of this — **aim and movement fight
because they are literally the same stick.**

Which makes the flag the answer. Set it to 1 and the camera/aim axis comes from the d-pad fields
instead, leaving the nub free for movement: one analog channel for walking, another for aiming,
both fed from the mouse. `PadStickTick` therefore writes the flag every frame while active (it's a
game global; a mode change or cutscene could put it back) and restores it to 0 when it stops,
because leaving it set with nothing writing the d-pad would give a dead camera.

**The flag turned out to be much more than an axis selector — it summons free aim.** Setting it to
1 in game produced a **crosshair on screen**, for an ordinary weapon, which is the thing this whole
effort had concluded VCS did not have. It also visibly changes the whole control scheme: the nub
stops walking the player, and d-pad left/right (bound here to Q/E) turn the view instead. So this
is closer to a control-configuration switch than a single toggle, and "CameraInputMode" undersells
it. Rename it once its full effect is understood.

Confirmed working end to end: writes from the vblank hook DO land, despite the pad update clearing
those fields every frame. The earlier worry about frame ordering was unfounded — a one-shot write
from the WebSocket debugger gets wiped, but a per-frame write from `hleEnterVblank` does not.

**The trap this immediately set**, worth reading before touching the aim paths: the moment the
pad-stick path worked, `IsFreeAiming` started reading 1, which made `ReticleActive()` return true,
which made `ApplyAnalog` consume the mouse delta for the left stick before `PadStickTick` could
have it. Result: `Written: x=+0.00`, a dead aim axis, and a left stick moving when it should have
been pure movement. It looked exactly like the new path failing, when in fact the new path
succeeding is what woke the old one up. `ReticleActive` now stands down whenever another aim
mechanism is enabled. Any third mechanism must do the same — one delta, one consumer.

**Still open:** why the nub stops moving the player with the flag set, and what else the flag
changes. Both want mapping out before this becomes the default.

**Credit:** the two function addresses came from
[PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp)'s VCS profile
(`profiles/vcs/host/vcs_camera_input.cpp`), which solves the same problem by replacing those
functions outright in recompiled code. Its `config/vcs_ulus10160.toml` also confirms load base
`0x08804000`, which matches this build exactly.

### The CLEO plugin route — aiming through the right stick

Another PPSSPP-based VCS port (`Enginevcs.exe`, the "GTA Legacy" build) solves free aim a
completely different way, and the approach is worth understanding because it's the one that can
actually add a mechanic rather than remap one.

**It isn't emulator code at all.** Free aim lives in a PSP-side CLEO plugin —
`PSP/PLUGINS/cleo/cleo.prx` — that loads into the game process, resolves the game's main library
(image base `0x08804000`), and calls the game's own functions. Its scripts show the technique:
`mousecar.txt` pattern-scans for `CPad::GetPad()`, calls it, and writes into the returned pad
struct; `mouse.txt` calls an undocumented pad opcode while the aim button is held.

**The input reaches it through the right analog stick.** Their `controls.ini` maps
`RightAn.{Up,Down,Left,Right}` to `2-40{55,54,53,52}` — decode that with
`AXIS_BIND_NKCODE_START` (`Common/Input/InputState.h`, `4000 + axisId*2 + (direction < 0)`) and
it's axes 26 and 27, `JOYSTICK_AXIS_MOUSE_REL_X/Y`. Raw mouse motion, straight onto a stick.

That works because **the right stick is a channel VCS itself never reads.** The PSP has no second
stick; PPSSPP carries one in the spare bytes of `SceCtrlData` (`CtrlData` in `Core/HLE/sceCtrl.cpp`
— *"the PSP has only one stick, but has space for more info"*), at bytes 10–11, memcpy'd to the
game with everything else. Writing to it cannot disturb the game. It is inert unless a plugin is
listening.

**This fork can use both.** The plugin is a stock PSP plugin and upstream PPSSPP already loads that
format (`Core/HLE/Plugins.cpp` reads `PSP/PLUGINS/*/plugin.ini`), and their `[games]` section
already lists `ULUS10160`. It's installed at `memstick/PSP/PLUGINS/cleo/`. Turn on
**`aimViaRightStick`** (Camera tab, "Aim via right stick") and `ApplyAimStick` feeds the right
stick from the mouse while the aim key is held.

Deliberate differences from the left-stick reticle path:

- **Not gated on `IsFreeAiming`.** Nothing in VCS reads this stick, so feeding it can never make
  the player strafe — the caution that governs the left stick doesn't apply. And the plugin may be
  what *puts* the game into free aim, so gating on free aim already being active could never let
  it start.
- **Disables the left-stick reticle**, so two aim mechanisms can't fight over one crosshair.
- **Stops the camera being driven** while aim is held, since the plugin owns the view then.

**Their camera is the part not to copy.** Everything they do routes through the game's own
nub-driven camera, with its rate limits and smoothing. Ours writes `CameraYaw`/`CameraPitch`
directly and feels like a mouse. The two are complementary: their plugin for the aiming mechanic,
our camera for everything else.

**Unknowns.** The semantics of opcode `03E9` (called as `03E9 1.4 0.0`) and of the `CPad + 29`
write aren't established — that needs the PRX disassembled. And `cleo.prx` is a third-party binary
of unknown licence: fine to run locally, **don't commit it**.

**WASD goes dead in free aim, and that part is correct.** The PSP has exactly one analog axis, so
while free-aiming it cannot both walk the player and place the crosshair — the game gives it to the
crosshair. That's a hardware constraint being reproduced, not a preference. The keys stay *claimed*
in the Aiming context regardless, as `psp = 0` rows, because PPSSPP's own defaults bind
W/A/S/D to R-trigger/Square/Triangle and letting them fall through would make strafing jump and
enter vehicles. The claim is unconditional; only the steering is conditional. Claiming and steering
are separate questions — see the inverse Escape trap.

**The context itself is still entered from the held key, not from `IsAiming`.** That part survived:

- `IsAiming` means **locked on**, so it can't mark the boundary of "the player wants to aim" — it
  only goes up once a target exists, and never at all when aiming at empty space.
- Holding the key is the player's actual intent, which is the whole meaning of hold-to-aim, and it
  needs no address — the aim *bindings* still work on a build where neither flag was found.
- No latency. A flag flips once the game decides; the key is known the frame it goes down.

**R trigger as aim is now confirmed twice over** — by the button tester, and by this round of
testing, where holding it visibly produced lock-on. It is not L.

### Camera pitch: limits must be anchor-relative, and vehicles are excluded

Two things here were learned expensively.

**The game's pitch baseline differs per camera**: about `-0.05` on foot but about `-1.55` in a
vehicle. Any *absolute* clamp is therefore wrong in one mode or the other. An early fixed
`+/-0.9` yanked the vehicle anchor from `-1.55` up to `-0.9` on the first write and then pinned
it, so the view snapped to the roof and could not be brought back down. `VCSCamera` now clamps
to `anchor +/- kPitchRange`. Don't reintroduce absolute limits.

**Pitch is not driven in vehicles at all** (`pitchInVehicle`, off by default). Yaw is fine there,
but asserting pitch against the vehicle follow-camera leaves it settled steeply downward after
we release - observed at `-0.99` rad, looking down at the bike from above. Root cause not yet
identified; the toggle exists so it can be re-tested without a rebuild.

A methodology note, because it cost several rounds: the first diagnosis of the clamp bug was
*correct*, then wrongly retracted after checking snapshots that showed pitch only ever between
`-0.44` and `-0.03`. That data was unrepresentative - there was exactly one in-vehicle snapshot,
taken before mouse look existed. The live readout in the Camera tab settled it in one screenshot.
Prefer the live diagnostic over archived snapshots when asking "what range does this take".

### Only release buttons you pressed

`ApplyMapping` clears `g_lastAppliedMask & ~setMask` - the bits *we* set last frame - and nothing
else. It must stay that way.

An earlier version cleared the whole context-owned mask every frame: every button the context
could possibly produce, whether we had pressed it or not. That silently cancelled any input
arriving through PPSSPP's own mapper or a real pad that shared a button with us (Start, Cross,
Circle...), within a frame of it being pressed. Symptom: cutscenes could not be skipped and the
controls generally felt broken, with no obvious connection to this layer.

`g_lastAppliedMask` is sufficient on its own, including across a context change - the previous
context's presses are exactly what it holds.

The same principle applies to the analog stick in `ApplyAnalog`: it is only touched while a
movement key is held, and released exactly once when the last one comes up.

### The inverse Escape trap — keys PPSSPP steals from us

The Escape trap is us claiming a key PPSSPP needs. The mirror image bites too: a key we *don't*
map falls through to PPSSPP's own defaults and does something unwanted.

`Shift` is bound to `VIRTKEY_RAPID_FIRE` by default. It was mapped to Sprint on foot (so claimed
and harmless there) but left unmapped in a vehicle, where it fell through and rapid-fired the
held accelerator, making the throttle stutter. Fixed by claiming it in the vehicle and aiming
contexts with `psp = 0` - a row that claims the key and sends nothing.

`Tab` was the same story in reverse: bound here to next-weapon, it silently disabled PPSSPP's
fast-forward until the binding was removed. It is bound again now, deliberately — to L trigger,
"switch to nearby weapon drop" — with fast-forward accepted as the price. That's the point of the
trap: it's a real cost either way, so make the trade knowingly rather than discovering it later.
Rebind fast-forward in PPSSPP's own control settings to get it back on another key.

So when adding or removing a binding, check `Core/KeyMapDefaults.cpp` **both ways**: does the key
you are claiming matter to PPSSPP, and does a key you are leaving unclaimed already do something
there? A `psp = 0` row is the tool for the second case.

### What each PSP button actually does in VCS

Verified by holding each one in game with the Input tab's button tester. Guessing these was the
single biggest source of wrong bindings, so add to this table rather than assuming.

| PSP button | On foot | In a vehicle |
|---|---|---|
| Cross | sprint | accelerate |
| Square | jump | brake / reverse |
| Circle | attack / fire | drive-by fire |
| Triangle | enter vehicle | exit vehicle |
| R trigger | **aim** (lock-on) | handbrake |
| L trigger | **switch to a nearby weapon drop** | glance modifier (L + stick direction) |
| D-pad Up | ? | ? |
| D-pad Down | ? | **horn** |
| D-pad Left | previous weapon | **previous radio station** |
| D-pad Right | next weapon | **next radio station** |
| Select | change camera | change camera |
| Start | pause | pause |

**There is no crouch in VCS on PSP.** It does not exist as a mechanic, so no button maps to it -
don't go looking. An earlier binding claimed C was crouch and simply did nothing.

**L trigger is not unused — that claim was wrong twice.** It survived several rounds of testing
because it is a *modifier*: inert on its own, meaningful only in combination, so holding it alone
in the button tester genuinely does nothing and looks like a dead button.

- **In a vehicle** it's the glance modifier: L + a stick direction looks that way, which is also
  what makes drive-bys fire to the correct side.
- **On foot** it switches to a nearby weapon drop — stand over a dropped weapon and it swaps to
  that weapon's type. Distinct from the Q/E cycle, which only moves through what you already
  carry. Bound to `Tab`.

The general lesson: a button that "does nothing" in the tester may need a second input, or a
specific situation, before it does anything. Test modifiers in combination and near things.

Use the button tester (Input tab) to fill in the `?` entries rather than guessing.

### The Escape trap — read before adding a binding

A claimed key is deliberately withheld from `g_controlMapper`, so binding a key that PPSSPP maps
to a `VIRTKEY_` silently removes that emulator function. Escape is bound to `VIRTKEY_PAUSE` by
default in `Core/KeyMapDefaults.cpp`; binding it in the VCS table swallowed the only way to open
the pause menu and trapped the player in the game. `P` is used for the PSP Start button instead.
**Check `KeyMapDefaults.cpp` before adding any binding.**

### Things learned while testing input, which will save you time

- **Bare modifier keys never arrive.** Holding only Left Shift produces no `NativeKey` event at
  all, so it is a useless key to test with. Use a normal key like W.
- **`KeyInput::keyCode` is a union with `unicodeChar`.** A `KeyInputFlags::CHAR` event carries a
  codepoint there, not a key code — pressing W produces both a `key=51` event and a `key=119`
  CHAR event. `HandleHostKey` rejects CHAR events for exactly this reason; without that guard
  the held-key set fills up with garbage codepoints.
- **`INFO_LOG` is invisible at default log levels.** Only warnings and above reach the log file,
  so debug printfs added at INFO level will look like the code never ran. Use `WARN_LOG` or
  higher when probing.
- **The ImGui debugger swallows the keyboard when focused.** `WantCaptureKeyboard` strips
  `InputMode::Keyboard` before `passKeyThrough`, so click the game area, not the VCS window, when
  testing input by hand.

**Deliberately not implemented yet:**

- **`WeaponIndex` and `GameState` are still unset**, and are the only two worth hunting.
  `PlayerOnFoot` is derived rather than read, and `AimYaw`/`AimPitch` are unset *permanently* —
  see "There is no stored aim direction". See [docs/VCS_ADDRESSES.md](docs/VCS_ADDRESSES.md).
- **Direct weapon selection.** Needs `WeaponIndex`, and probably a memory write rather than a
  button press, since the PSP only exposes cycle-next/cycle-previous.
- **Vertical look while driving.** Off behind `pitchInVehicle`; see the pitch section above.
  Unresolved, not abandoned.

- **`Menu` context never triggers.** Needs `GameState`. Until then a paused game gets gameplay
  bindings.
- **Most bindings are unverified guesses.** Only the ones marked "verified" in
  `kVCSKeyMappings` were checked against the real game. Aim sat on the wrong trigger (L instead
  of R) for a long time and silently did nothing - assume crouch, horn, radio and camera are
  wrong until someone holds them and looks.
- **PAL / JP builds.** Different builds, different addresses; would need a second table.

## Working on this

Build and test exactly as upstream describes. To exercise the VCS layer you need the game
running — the module only activates on a real `ULUS10160` boot.

The debugger window is at **Debug → Tools → VCS**. It persists its open state in
`memstick/PSP/SYSTEM/imdebugger.ini` (`vcsOpen`).

`Tools/vcsstatic.py` reads the game's code **offline**, from a savestate, with nothing running —
that is the one to reach for first now. `Tools/vcsdisasm.py` does the same over the WebSocket
debugger against a live game (`python Tools/vcsdisasm.py 0x0898bb4c 24`), which is what you want
when the question is about a *moment* rather than about what the code does.

Reading the game's actual code is far faster than scanning for values, and the record is
lopsided: the second-stick mechanism came out of two functions this way after weeks of guessing
from the outside, and the aim response curve came out of one function after five rounds of
value-correlation had failed to find something that turned out not to exist. When a question is
"what does the game do with X", disassemble; correlation is for "where does the game keep Y".

For finding addresses, use `Tools/vcsscan.py` (run PPSSPP with `--debugger=1337`). It adds the
snapshot-and-diff narrowing that PPSSPP's own `memory.search` can't do — `memory.search` only
finds values you already know, which doesn't help for things like a vehicle pointer. See
[docs/VCS_ADDRESSES.md](docs/VCS_ADDRESSES.md).

Adding a new address is a three-step change and touches nothing else:

1. Add an enum value to `VCSAddr` and a matching row to `kVCSAddresses` (a `static_assert`
   catches a mismatch).
2. Read it in `VCSState::UpdateState` if it belongs in the decoded state.
3. Document it in `docs/VCS_ADDRESSES.md`.

### Gotcha found the hard way

Passing PPSSPP an unwritable log path — e.g. `--log=C:\foo.log`, since the drive root needs
admin — aborts the process at startup with a CRT "abort() has been called" dialog and no log.
This is upstream behaviour, unrelated to this fork; it reproduces with the compat flag off. Use
a writable path, or enable `FileLogging` in `memstick/PSP/SYSTEM/ppsspp.ini` instead.
