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

**`VCSCamera`** is mouse look, horizontal and vertical. It doesn't go through `sceCtrl`, because no PSP input
can express "rotate by exactly this many radians" — it writes `CameraYaw` directly. It also owns
the raw mouse-delta buffer for the *aiming* path, which spends the delta on the stick instead —
see "Aiming is a stick, not a camera" below.

The non-obvious part is *why it writes every frame*. The game runs its own camera smoothing that
eases yaw back toward its follow target, so writing only when the mouse moves accomplishes nothing
— the game undoes each nudge in between (measured: 135 successful writes, zero net rotation).
`VCSCamera` therefore keeps its own `g_desiredYaw`, anchors it to the game's value when a look
starts, and re-asserts it every frame for `lookHoldFrames` (~0.75s) after the last movement. That
outpaces the smoothing while the player is looking around. Don't "simplify" this back into a
read-modify-write; it will silently stop working.

**Releasing that override is its own problem, and used to be a cliff.** The game goes on computing
its own camera underneath ours the whole time - mode 15 builds Beta from the player's heading and
never reads a look axis - so the tick the hold expired handed the player, in one frame, however far
the two had diverged. Reported as *"about a second after I stop, it snaps back to roughly where it
started"*, and the "roughly" was the game's follow logic having drifted meanwhile. The hold now
fades out instead: across `lookReleaseFrames` the written value walks to the game's live value on a
smoothstep, stepping once per **game logic frame**, and the last step writes exactly what it read -
so the tick we stop writing is a non-event by construction. The walk is deliberately blind to *why*
the two differ, which is what makes it correct without having to settle whether the follow camera
had adopted our value: if it had, every step is a no-op and the camera just stays put. The proper
fix, once mode 15's follow TARGET is identified, is to write that instead and never need a handback.

**Pitch and yaw do not come back the same way, and the asymmetry decides where the walk aims.**
Reported in play once the fade was in: pitch always returns to -4.6 deg, yaw does not return behind
the player at all. So the game springs pitch back to a baseline by itself and simply keeps whatever
yaw it finds - which means a walk toward the game's *live* yaw is a walk toward the value we
ourselves last wrote, and correctly does nothing. Yaw therefore gets an explicit target instead:
`PedHeading - PI/2`, the inverse of the `heading = camYaw + PI/2` that `PedAimTick` already ships
(`returnBehindPlayer`, on by default). **On foot only** - a vehicle returns behind the car on its
own, `PedHeading` there belongs to someone sitting in a seat, and that camera is the spring this
fork spent a long time learning not to fight. Pitch keeps fading to the game's live value, because
that is the half the game was already getting right.

**The quarter turn was right, and the thing it is right about is narrower than assumed.** Aiming the
walk at `PedHeading - PI/2` still left a 15-35 degree snap, which looked like a wrong constant - the
known `Beta`-to-`Front` gap is only 4.42 degrees, so it could not be that, and the range was too wide
for one constant slightly off. So a learner went in to measure the offset instead of deriving it:
`CameraYaw - PedHeading`, sampled only while the player walks in a straight line with the camera at
rest and mouse look not driving, low-passed (`learnFollowOffset`, with `returnBehindTrimDeg` as a
hand override). All three sampling gates earn their place - standing still would measure where the
player last left the view rather than where the camera wants to be, and a mid-turn sample would
measure the camera's trailing lag.

**It settled on exactly -90.000 degrees over 1671 samples.** The derived value was correct, and the
hypothesis that sent the learner in was wrong. What the same session measured is the finding:
standing still after the walk, the camera sat at **-59.2 degrees** off the heading - 30.8 degrees
from where the handback had just put it, which is the whole of the reported snap. So there is no
single "behind the player": the follow camera holds `heading - 90` **while walking** and something
else entirely once the player stops, and no value handed over at a standstill is the one it wants.

Hence `holdUntilMoving` (on by default): standing still, the handback simply does not run and the
view stays where the player left it, which is what San Andreas does anyway. The walk fires on the
first frame of movement, when the game's own recentring is live and provably heading for the same
`-90` the walk aims at. The proper fix remains mode 15's own stored Beta - write that and the
handback stops being a negotiation. `ReleaseTrace` on the Camera tab is the instrument for
identifying it: it records the camera's offset from the heading for 24 game frames after we let go,
which separates a stored value being restored (one frame) from a spring easing somewhere (several).

**And above all of it, `returnLook` - now OFF by default.** One switch that says never hand the
camera back at all: the hold does not expire, the view stays where the player left it indefinitely,
and the automatic return stops applying. It is the honest end of the road the three changes above
were walking down. The handback exists to give the game its camera back politely, and once the
measurements showed the game has no single resting position to hand it back TO, a player who would
rather it never took the camera back has nothing left to be polite about. The write discipline is
unchanged, which is what keeps an indefinite hold from being a longer exposure to the vehicle pitch
runaway - pitch is still asserted once per game logic frame and still clamped to the anchor window.

**With no automatic return there has to be a deliberate one**, so the vehicle glance keys - Q, E, or
both - walk the view back behind the car and then hand the camera over (`recenterOnGlance`, on). That
also repairs something the indefinite hold broke: a glance is *the game's own* look mechanic, L
trigger plus a stick direction, so a permanently asserted `CameraYaw` painted straight over it and
the glance appeared to do nothing. Ending the hold gives it the camera back from the position it
expects to start at. The walk targets `heading + offset` directly rather than the game's live yaw,
for the reason the on-foot return does - it lands on a value the game agrees with instead of
negotiating with a camera that is holding our own number. Note the one thing NOT measured here:
whether `PedHeading` tracks the car while driving. The Camera tab's live offset now updates in every
context so it can be checked - drive straight with the view behind and it should read -90.

**The transferable part is the shape of the error, which this file has now recorded four times.** The
measurement said "15-35 degrees off", the inference said "so the constant is wrong", and the constant
was exactly right - the offset was measured against a state (walking) that did not hold at the moment
that mattered (standing still). A quantity that is only valid in some states is not a constant, and
asking *when* it was measured is a different question from asking whether it was measured correctly.

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
| Per-frame tick | `hleEnterVblank()` in [Core/HLE/sceDisplay.cpp](Core/HLE/sceDisplay.cpp) | Once per vblank on the emu thread, next to the existing `g_controlMapper.UpdateAutoMovements` call |
| Init | `__KernelInit()` in [Core/HLE/sceKernel.cpp](Core/HLE/sceKernel.cpp) | After `__CtrlInit`, since it drives sceCtrl |
| Shutdown | `__KernelShutdown()` in [Core/HLE/sceKernel.cpp](Core/HLE/sceKernel.cpp) | *Before* `__CtrlShutdown`, so held buttons get released while sceCtrl is alive |
| Host keys | `NativeKey()` in [UI/NativeApp.cpp](UI/NativeApp.cpp) | Inside the existing `passKeyThrough` branch, which already handles ImGui capture and UI-vs-ingame gating |
| Mouse look | `NativeMouseDelta()` in [UI/NativeApp.cpp](UI/NativeApp.cpp) | Same claim pattern as keys; skipped while the ImGui debugger wants the mouse |
| Compat flag | [Core/Compatibility.h](Core/Compatibility.h) + `.cpp` + [assets/compat.ini](assets/compat.ini) | One struct field, one `CheckSetting` line, one `[VCSInputOverhaul]` section - and a second pair for `VCSDynamicShadows`, kept separate because one is a renderer feature and the other is input, and a third for `VCSWaterQuality` |
| Shadow capture | `Flush()` in [GPU/Vulkan/DrawEngineVulkan.cpp](GPU/Vulkan/DrawEngineVulkan.cpp) | Classify + capture on both transform paths, the predecode gate, and `OnFlush` at the top for the 3D→2D seam |
| Shadow frame reset | `BeginHostFrame()` in [GPU/Vulkan/GPU_Vulkan.cpp](GPU/Vulkan/GPU_Vulkan.cpp) | Publishes last frame's counts; renders nothing |
| Water capture | `Flush()` in [GPU/Vulkan/DrawEngineVulkan.cpp](GPU/Vulkan/DrawEngineVulkan.cpp) | Beside the shadow capture on both transform paths, and its own `OnFlush` *after* the shadow one - see the comment there for why the order matters |
| Water frame reset | `BeginHostFrame()` in [GPU/Vulkan/GPU_Vulkan.cpp](GPU/Vulkan/GPU_Vulkan.cpp) | Also where the road mask is rasterised and uploaded, because it is the one point in the frame with no render pass open |
| Pause menu | the three `GamePauseScreen` sites in [UI/EmuScreen.cpp](UI/EmuScreen.cpp) | All three now call `CreatePauseScreen()`, which returns PPSSPP's own screen unless `VCS::IsActive()` |

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

## The front end

**Status: working, confirmed against the running game.** Esc opens it over a paused VCS, the page
table navigates, hover and keyboard both move the selection, left/right edits a value and the
blocks track it, Esc walks back up the pages and then resumes, and `vcs.ini` is written on the way
out with all 20 of the options this fork owns - the four that belong to `g_Config` are marked
`external` and saved by PPSSPP instead.

It imitates VCS's own front end, not Vice City's PC one, and the difference is the whole design:

| | VC on PC (what this was built as first) | VCS (what it is now) |
|---|---|---|
| list | left column, left-aligned | centred on the screen |
| selection | slanted highlight quad behind the row | carried by colour alone |
| palette | pink on pink | cyan, selected row cream |
| headings | sans, top right | brush script, top left |
| values | number and a continuous bar | discrete blocks |
| backdrop | the dimmed game | opaque purple with palm fronds |
| bottom | nothing | a bar: what the row means, and how to work it |

Worth stating plainly because the first build got it wrong: "the GTA menu look" is not one thing.
Building it from Vice City's PC frontend produced something coherent that was recognisably the
wrong game.

### What was taken from reVC, and how little of it

reVC's `CMenuManager` is ~6600 lines, and most of that is a UI toolkit: hit-testing, hover state,
keyboard repeat, layout, text measurement. The game had no toolkit, so the menu had to be one.
PPSSPP already has that layer, themed and working on mouse, keyboard and pad, so re-implementing
it would have been several thousand lines spent to arrive back where we started.

Three things *were* worth taking:

- **The page table.** A page is a header, a parent page, and a list of rows; a row is either a
  jump to another page or a binding to one setting. Back-navigation is data (`ParentPage`), not
  code. reVC stores this as `aScreens[MENUPAGES]`.
- **CFO** - re3's "Custom Frontend Options", the reason `Core/VCS/VCSSettings.h` exists. A row is
  described by the variable it edits plus the ini key it saves to, so adding an option is one row
  in one table: no renderer change, no switch arm, no new save code.
- **The helper line.** The selected row's explanation along the bottom, which is what lets an
  option be called "Free aim" instead of a sentence.

Its *palettes* are worth knowing about too, and they are not what you would guess. reVC ships two
whole front ends - `Frontend.cpp` for PC and `Frontend_PS2.cpp` behind `PS2_MENU` - and neither is
this one: PC is pink (`LABEL_COLOR(255,150,225)`), PS2 is gold (`SELECTED_TEXT_COLOR(255,182,48)`).
The cyan-and-cream here came from VCS itself.

### The settings did not persist before this

Every VCS tunable lived in the ImGui debugger, bound to `VCSCameraSettings` and
`VCSFireHookSettings`, and reset to defaults on every launch. `VCSSettings` is the fix, and it
deliberately does not move the values - those structs go on owning them, documented where they are
used, with the debugger still binding to them directly. What the table adds is names, ranges and
persistence for the subset a *player* would set. A knob that needs a paragraph of measurements to
interpret stays in the debugger window.

A row is `Bool`, `Float`, `Int` or `Choice`, and the split between the last two is the one worth
stating: **`Choice` stores an index into a label list, `Int` stores the value itself.** Master
volume was written as a `Choice` over `"0"`..`"100"` in tens, which reads correctly and is wrong -
`g_Config.iGameVolume` runs 0..100, so the index and the value are different numbers, and the
first left-press took the volume from 100 to 10. `Int` carries `minInt`/`maxInt`/`stepInt`,
renders as the same ten blocks a `Float` does, and one press moves one block. Anything owned by
the rest of the emulator is a value, not an index; reach for `Choice` only when the thing really
is a list of names.

Two more details that will bite if they are "simplified":

- `Options()` captures each default from the live variable the first time it is called, so it
  **must** run before the ini is read. `LoadSettings()` guarantees that by calling it first thing.
  Reverse the order and "restore defaults" quietly means "restore whatever was last saved".
- Settings live in `memstick/PSP/SYSTEM/vcs.ini`, next to `imdebugger.ini`, **not** in
  `ppsspp.ini`. Putting them in `Core/Config.cpp` would mean editing a file upstream touches
  constantly, which is the difference between a clean rebase and a conflict on every pull.
- An `external` row's default belongs to PPSSPP, so ask it: `Config::GetDefaultValueInt(&g_Config.x)`.
  Restating the number here means "restore defaults" quietly disagrees with the value the rest of
  the emulator would restore - anisotropy was written as `0` against an upstream default of `4`.
  It is safe to call for anything in `struct Config`; only the blocks that override
  `CanResetToDefault()` (display layout, touch controls, gestures) assert.

### The Gameplay page has a second kind of row now: the game's own settings

`SUBTITLES` and `HUD` are not this port's inventions, and that makes them the first rows on that
page that are not. They are the PSP game's own, off the DISPLAY page of its front end - the page
this fork's bridge deliberately does not offer, because there are pages of our own for controls,
audio and display. That left three of the game's four display settings unreachable rather than
superseded, which is the same loss the map and the save list took when Start was claimed, and the
same answer applies: give it back.

So the page's rule is now "settings about the GAME, as opposed to about the device you drive it
with", and both kinds qualify - a switch that hands one of our own additions back, and a switch
that reaches one of the game's that nothing else can.

**Reaching for the game's existing setting is the whole point, not a shortcut.** Before this there
were two mechanisms sketched for hiding the HUD, both ours, and both would have been wrong: the
game has the setting, it persists it, and its own menu goes on agreeing with ours because there is
only one value. Same doctrine as the vault calling `CPed::CanClimb` and the front-end bridge
pressing Start.

**They are mirrored, not edited in place.** The menu runs on the UI thread and PSP memory belongs to
the emu thread, so `VCSGameSettings` holds a bool for each and `ApplyDisplayPrefs` in `VCSGame::Tick`
pushes them across - **only when the game disagrees**, which makes it self-healing across a load
without being the re-assert-every-frame mistake that the camera pitch runaway taught.

**HUD MODE is the panel, not the radar**, and the row's help line has to say so. See "The game's own
SUBTITLES and HUD MODE" in docs/VCS_ADDRESSES.md for how both were found, and for the two candidates
that tracked the setting perfectly and still decided nothing.

### The Audio page's other two rows, and the value that decides

`SFX VOLUME` and `RADIO VOLUME` are the game's own, reached the same way and for the same reason as
subtitles and the HUD. What makes them worth their own note is that **each setting is two values**,
and the obvious one is the wrong one.

`DisplayPrefs + 0x1c` and `+0x20` are the preferences: they are what the game's own AUDIO page shows
and what a save keeps, they step 16 at a time and top out at 127, and **writing them changes nothing
you can hear**. The levels the mixer actually reads are two plain bytes at `0x08bb3b74` and
`0x08bb3b75`, nothing recomputes them from the preferences, and the game's own slider moves both
together. So the fork's rows write both, and a row that wrote either one alone would be a volume
that forgets itself across a boot, or a slider that moves and does nothing.

**The lesson generalises past audio, and it cost time twice.** A value that tracks a setting
perfectly is not necessarily the value that decides anything - HUD MODE has two gp-relative shadows
that follow it exactly and revert within a frame when written. The test is one line of work: write
it, and look at whether the thing you wanted actually changed. Three of the five game settings found
this way failed that check on the first candidate.

**The step is 13, not the game's 16.** The strip draws ten blocks and this menu's rule is that one
press moves one block, so the step is a tenth of the range rounded up - ten presses reach the top,
where the write path clamps to 127 exactly as the game's own slider does at its last notch. The
game's ladder of 16 is eight notches against a ten-block strip, which would move one block on some
presses and two on others. Both produce values the game accepts; this one matches the control the
player is looking at.

### The menu's own sounds, taken from the game rather than invented

The front end was silent, and the three sounds it wanted were already on the disc. A
`SET*/SFX*_PSP.RAW` file is a plain concatenation of Sony VAG streams, each with a 0x30 header
carrying its rate and - the part that turned this from a hunt into a listing - **its name**:

    SFX_FE_HIGHLIGHT   44 ms   the blip as the selection moves
    SFX_FE_SELECT      90 ms   a row being chosen
    SFX_FE_BACK        49 ms   going back a page

`Tools/vcsmenusfx.py` decodes those three to WAV in `assets/vcs/`, beside the page titles and for
the same reason. The same bank also holds an error blip per direction and three noise bursts, if
the menu ever wants them.

**Three new `UISound` values, not three swapped samples.** `SoundEffectMixer::UpdateSample` would
have let the fork replace `SELECT`/`CONFIRM`/`BACK` with no upstream diff at all, and it was the
wrong trade: those samples are global, so a Debug session that booted another game after VCS would
have GTA blips in PPSSPP's own menus. Adding to the enum is six lines and is additive - nothing
outside `VCSMenuScreen.cpp` plays them.

**And they do not go through `UI::PlayUISound`.** That path is gated on `g_Config.bUISound`, which
ships *off* and lives on PPSSPP's own Audio page - a screen the game build has no way into. A menu
that stays silent until you find a setting you cannot reach is a silent menu, so this calls the
mixer directly.

**Which means the volume has to be computed at the call, because nothing downstream does it.** A UI
sound is mixed on top of everything - `NativeMix` adds the effect mixer after the game's audio - so
it misses `iGameVolume`, which is applied inside `__sceAudio` to the emulated game only, and it
misses the SFX level, which is a value *inside* the game applied by the game's own mixer. These
first shipped answering to neither, on a page that has a row called Master volume and a row called
SFX volume, and it read as a bug the moment anyone moved one. Both are applied now, and so is
`bEnableSound` - that gates `__sceAudio` rather than the audio device, so the effect mixer goes on
running with the emulator "muted" unless it is asked.

Following the SFX row rather than only the master is the honest half: these ARE the game's
front-end sound effects, and in the retail game they play on the channel that row governs.

**`FocusChanged` is the seam for the highlight**, and it is the only one that catches both halves:
hover sets the focused view from `Touch`, keyboard and pad navigation go through PPSSPP's own
`MoveFocus`, and both end up there. The `CAUSE_*` flags then do the discrimination that a timer
would otherwise have to - `CAUSE_FOCUS_MOVE` and `CAUSE_OTHER` are somebody moving the selection,
while `CAUSE_SCREEN_CHANGE`, `CAUSE_RESTORE` and `CAUSE_FORCED` are a page building itself or a
click forcing focus onto the row it is about to activate. Blipping on those would mean a sound for
merely opening a page, and two sounds for every click.

### The bridge's own button presses are not the player's, so they are silenced

Picking MAP, BRIEF or STATS makes this port press Start and then tab across the game's front end,
behind a curtain that is there precisely so the walk is not watched. The game blips at every one of
those presses - button sounds for buttons nobody touched.

`ApplyGamePrefs` writes the live SFX level as 0 whenever `FrontEndDriving()` is true, and this is
where the two-layer volume finding pays for itself twice over: **the preference is left alone**, so
nothing about this reaches the game's Audio page or a save, and the level comes back on its own the
tick after the walk ends, because `ApplyPref` is a disagreement check rather than a one-shot.

Muting the CHANNEL rather than the emulator is the point - the radio plays through the whole thing,
which is what the player is actually listening to.

### Walking, which a keyboard could not do until it had a modifier

VCS reads walk out of how far the nub is pushed, so a pad has always had it and a keyboard never
could: a key is fully down or not at all, which is always a run. Alt with WASD is the middle of
the range handed back.

**The deflection is measured, not chosen.** Driven from the debugger in steps with the player's
velocity read back at each one, it came out in flat bands rather than as a curve:

| deflection | speed/frame | |
|---|---|---|
| 0.15 - 0.25 | 0.000 | inside the game's own dead zone |
| 0.30 - 0.60 | 0.026 | identical across the whole band - the walk |
| 0.70 | 0.037 | the transition |
| 0.85 - 1.00 | 0.070 - 0.093 | the run, which is what every key press produces |

`kVCSWalkDeflection` is 0.45, the middle of the walking band, so nothing depends on the
measurement being exact. **Applied after the diagonal normalisation**, or W+D would scale to 0.64
- out of the band and into the jog, so walking diagonally would quietly not be walking. And only
for a keyboard: a stick already says how fast to go, and scaling one that is already half pushed
would take it under the dead zone and stop the player dead with the stick still deflected.

**Both Alts are claimed**, and the right one is not politeness: PPSSPP's default keyboard map puts
`CTRL_CIRCLE` on it, so a player reaching for the Alt nearer their right hand would have jumped
instead of walking. The inverse Escape trap, one more time.

### Alt + a key used to stick, and it was never Alt's fault

Reported as "Alt with another key makes it act weird and non-functional until I press Alt + that
key again", which is a precise description of a lost key-up. `Windows/RawInput.cpp` accepted
`WM_SYSKEYDOWN` as a press and handled only `WM_KEYUP` as a release - but **Windows sends the SYS
variant of BOTH messages for any key pressed while Alt is held**, so the release fell off the end
of the `else if` and the key stayed down: in `keyboardKeysDown`, in this fork's `g_heldKeys`, and
in the game. Pressing the key again without Alt is what cleared it, which is exactly the symptom.

One missing condition, and **it was never specific to this binding** - every Alt combination in
the emulator has had it, for every game. Worth remembering as a shape: a control that "sticks
until you repeat it" is a release that was never delivered, not a press that was mishandled.

### The tutorial messages name keyboard keys now, and it took no code at all

The help lines in the corner - "To sprint, hold ~k~ ~PDSPR~ while running" - do not contain a
button name. They contain a TOKEN, and the game resolves it through a second GXT entry: `~PDSPR~`
looks up `C0PDSPR`, which reads `~X~`. So the entire control vocabulary of every tutorial message
in the game is 58 strings in one table, `C0` through `C3` for the four control configurations, and
pointing them at this fork's bindings is a data change with nothing hooked.

The surface is **51 tokens across 140 occurrences**, and `Tools/vcsgxtkeys.py` rewrites them from a
hand-written mapping that mirrors `kVCSKeyMappings`. Two things in it are worth keeping:

- **The article has to go, by rule rather than by list.** Every PSP name is a noun phrase that
  wants one - "the up button", "the analog stick" - so the lines were written as "use the ~TOKEN~",
  and almost no keyboard name is: the result is "use the the mouse" and "press the LEFT MOUSE".
  Seven lines needed it, in five tables including two only a debug build shows, which is the whole
  argument against fixing them by hand.
- **Two lines needed rewriting outright**, because two PSP controls collapse into one on a mouse
  and the sentence says "and": "Hold ~PDLO1~ and use ~PDLO2~ to look around" has no second half
  left once both are the mouse.

**The GXT writer earned its `--verify`.** Rebuilding the file unchanged and comparing byte for byte
caught two wrong guesses that would otherwise have shipped: strings are NOT pooled by value (2408
entries, 2408 distinct offsets - pooling produced a file 8272 bytes short), and **TKEY is ordered by
key while TDAT is not**, so the pool has to be packed in the original offset order and the key table
written back in its own. Neither would have looked wrong in a hex editor.

### VCS reads its disc by SECTOR, so a filename redirect can never work

This is the finding worth carrying past the GXT, and it cost a full build-and-boot to get.

The delivery route chosen for the patched file was a redirect in the emulator: hook
`MetaFileSystem::OpenFile`, serve the file from the memory stick, leave the ISO alone. It was
built, it compiled, and **it never fired once.** Logging every single file open for a whole boot
showed why - the complete list of what VCS opens is savedata `PARAM.SFO`s, `EBOOT.BIN`, and:

```
disc0:/sce_lbn0x0_size0x65170000
```

The whole UMD, as one stream, seeked by the game to its own files by sector. **No filename is ever
requested for anything on the disc**, so there was never a name for a redirect to match. The
redirect is reverted; nothing of it remains.

So anything replacing a disc file in this game goes through the bytes - and once that is accepted,
the emulator can do it at READ TIME, so the disc never has to be touched at all.

`VCS::PatchDiscRead` is that: `ISOFileSystem::ReadFile` hands it the absolute position it has just
filled, and it overwrites any part of the buffer falling inside a patched range from a file on the
memory stick. Two call sites in one function, because that function is where both read paths meet -
the offset one, and the whole-sector one the `sce_lbn` handle uses. First line returns for every
other game.

**Not a `BlockDevice` wrapper, though that was the first design and reads better on paper.**
`ConstructBlockDevice` is shared by every game, and `Load_PSP_ISO` does a
`dynamic_cast<NPDRMDemoBlockDevice *>` on the result - a decorator would have silently broken demo
ISOs for a feature that has nothing to do with them. The read funnel one layer up costs nothing and
is already free of any of this.

The offset is a property of this disc image, found by searching it for the file's own bytes, so it
gets the treatment a measured address gets: **the disc is checked for the header that should be at
that offset before anything is overwritten**, and a mismatch disables that patch for the boot.
`ENGLISH.GXT` lives at `0x4330000`, sector 34400.

Verified both ways, which is the part that matters. Booting the **retail, untouched** ISO with the
replacement present puts `LEFT MOUSE` in RAM and no `the up button`; moving that one file aside and
booting the same ISO again brings `the up button` back. `Tools/vcsgxtkeys.py --iso` still bakes a
standalone image for anyone who wants one, and `VCS-keyboard-help.iso` is what it produced.

**This also corrects the answer given for the logo video.** A file redirect was recommended as the
neat way to swap `LOGO.PMF`; it would have failed the same way, silently. The disc patch is the
answer there too - one more row in `kVCSDiscPatches` - subject to the same size limit, because the
game seeks by sector numbers baked into its own code.

**And one trap in the diagnosis itself**: the first probe used `WARN_LOG` and produced nothing,
which read as proof. `IOLevel = 2` in `ppsspp.ini` is ERROR-only, so warnings never reached the
file at all. A log line that does not appear is not evidence until the channel is known to carry
it.

### The controls card is two tables in four columns

`CONTROLS - BINDINGS` is a read-only reference card: `ACTION`, then what the action is on the
keyboard, on an Xbox pad and on a PlayStation one, all on screen at once. Each column is generated
from the table that actually drives that device - `kVCSKeyMappings` for the keyboard,
`kVCSPadMappings` for both pads - so no column can drift from what pressing the thing does, which
is the only property here worth protecting.

**Two tables, three columns, and the third is not a third table.** A DualSense and an Xbox pad have
the same controls in the same places and differ in what is printed on the plastic, so the
PlayStation column is `PadButtonName` called with the other vocabulary. A second pad table would be
a second copy of the same scheme waiting to disagree with the first. `CREATE / SHARE` names one
button across two generations for the same reason - picking one would be wrong for half the pads
it describes.

**It was a switch, and losing the switch is the point.** A row on the bindings page chose which
device the card described, and left/right flipped it from inside a card. The switch answered "what
would I press on the device I am not holding", one device at a time, and it could never show the
thing four columns show for free: which actions exist on one device and not on another. WASD
against two blank pad cells, or the lock-on toggle against three, is a sentence the old card had no
way to write. The heading stopped following the switch too - it is `title_bindings.png` now, and
`title_keyboard.png` is gone.

It was **one** table read twice before that, and why that stopped working is worth recording. When
a pad still went through the PSP's own layout, every keyboard row already knew both halves - the
key it binds and the PSP button that key produces - so the pad's card was the same rows with the
other half shown, and it could not drift by construction. The moment the pad got a scheme of its
own that stopped being true: the two devices now agree about the face buttons and about almost
nothing else.

Three rules hold it together, and all three are the kind of thing a tidy-up undoes:

- **A blank cell is information; a blank row is not.** A row is dropped only when it has nothing on
  *any* device. One empty cell says "not on this device", and it can only say that standing next to
  a column where the action exists - which is exactly what the two-column card could not do, and
  why its rule was the stricter "not on this device, not on this card".
- **`kVCSListingExtras` carries one cell per column.** It is the handful of rows no mapping table
  can hold - the sticks, mouse look, the glances, the forks - and they are the ones that can drift,
  in three directions now. It is also how one row says `MOUSE` in one column and `RIGHT STICK` in
  the next two without being written down twice.
- **Extras merge by name, they do not append.** They go through the same `addRow` the mapping
  tables use, so an extras row can fill in *one device's cell* on a row a mapping table owns.
  `MENU` is the case that needs it: Escape opens this menu and can never be in `kVCSKeyMappings` -
  claiming it would withhold it from PPSSPP's mapper, which is where that pause comes from - while
  the pad's Start row is a real binding. One row, two sources, and neither could have written the
  other's half.

**Rows shrink to fit.** `ListRowHeight` divides the space between the panel and the hint bar by the
row count, clamped between 28 and 40dp. Nothing on this page scrolls and there is no second page,
so a card taller than the window is a card with bindings nobody can read. On Foot is the tallest
and the one a player reads first.

### The cheat menu types the combination, and that is the whole design

**Status: built, builds clean, NOT yet verified against the running game.** The 36 combinations come
from published cheat lists, not from the game's own table - see below for what that means and how to
fix a row that turns out wrong.

VCS has no cheat entry screen: every cheat is an eight-press pad combination entered during ordinary
play. `CHEATS` on the pause menu's root page leads to five group pages (Player, Vehicles,
Pedestrians, World, Multiplayer), and clicking a row queues it and closes the menu.

**Closing the menu is part of activating the cheat, not a courtesy.** This screen pauses the
emulator, so nothing can be typed while it is up; the row queues an index and sends `DR_CANCEL`, and
`CheatTick` starts pressing on the first tick after the game resumes. Anything else built on this
menu that has to reach the running game - the map bridge, save loading - wants the same shape.

**Why not call the game's handlers.** 36 addresses we have not hunted, against sequences we already
have; and a cheat is not only its effect - entering one flags the save, prints the game's own
confirmation, and toggles flags other systems read. Typing the combination gets all of that by
construction. A wrong sequence costs one row; a wrong address costs a crash.

**It steps on `FrameCounter`, not on vblanks**, for the reason the camera handback does. The game
samples the pad once per logic frame at 30Hz while `VCS::Tick` runs at 60, and neither rate holds
during streaming - so a press held for a fixed number of *vblanks* is a press held for an
unpredictable number of *samples*, and one missed sample in the middle of an eight-press combination
fails it silently. Two game frames held, two released; the gap is what keeps `Circle, Circle` from
reading as one long press. A whole combination takes about a second, and there is no deadline to
beat: GTA's cheat matchers keep a rolling history of presses rather than a timed window.

**`VCSCheats` presses nothing.** It decides what the mask should be; `ApplyMapping` applies it,
through the same `held & ~wanted` release discipline everything else there uses. That keeps one
function as the only thing in this fork touching sceCtrl, and it is why the clear mask is not simply
"everything": on the first frame the player may still be holding the key that produces the
combination's first press.

**The player's controls stand down for the duration**, buttons and stick both, exactly as they do
during a vault. A held W is a Cross, and a Cross between two presses is a ninth press.

**A row that does nothing is a transcription error, not a broken feature.** The OSD says
`Cheat: FULL HEALTH` when the sequence starts and the game prints its own confirmation when it
lands, so our message with nothing following it is the symptom, and the fix is one line in
`kCheats`. The authoritative version is in the EBOOT - the matcher lives near whatever writes the
flag `02A4 are_any_car_cheats_activated` reads - and mining it would replace the table with measured
data. Worth doing; it was not worth blocking a working menu on.

### The map and the save list, reached through the game's own front end

**Status: working, measured live against the running game.** `MAP`, `BRIEF`, `STATS` and `GAME`
sit on the pause root, and the game's own menu chrome is stripped off all of them. There is
deliberately no save row - VCS saves by walking into the save icon at a safe house, so a menu row
would be a second way to do something the world already has a place for. Controls, audio and
display are left off for the same kind of reason: this fork has its own pages for all three, and a
second way in that edited the game's copies would be two settings screens quietly disagreeing.

`GAME` is one row rather than three because `NEW GAME`, `LOAD GAME` and `DELETE SAVE DATA` are
entries *on* that page - the game says so itself, see the widget names below.

VCS keeps its map, stats, briefs and save list on the pause menu Start opens, and this fork took
Start for its own menu - so all of it became unreachable, which is a straight loss against retail.
`MAP`, `LOAD GAME` and `SAVE GAME` on the pause root give it back. They queue and close, the same
shape a cheat row uses and for the same reason: this screen pauses the emulator.

`FrontEndMenuManager` is at `0x08bc9100`, found through `0261 has_save_game_finished` rather than
by scanning - see "Reaching the game's own front end" in [docs/VCS_ADDRESSES.md](docs/VCS_ADDRESSES.md).
Three fields are in the table: `MenuActive` (+0x20), `MenuPage` (+0x1c, a SIGNED index into an
eleven-entry page vector, -1 when closed), and `SaveMenuRequest` (`gp+0x1076`).

**It presses Start; it does not set `MenuActive`.** Opening that menu is not one boolean - the game
builds page objects, stops the world, takes the pad - and a flag set from outside claims all of it
happened. The address table's job here is to *watch*, which is what turns the walk from a timed
guess into a closed loop. Same doctrine as the cheat menu and the vault: drive the mechanic.

**And it tabs; it does not write `MenuPage` either**, one level down on the same argument. A page
almost certainly does work on entry - the map builds a texture, the save list enumerates the memory
stick - so the walk presses the game's own navigation and watches the index until it arrives.

**The tabs are a GRID, and finding that out is what made the walk work.** Measured live over the
WebSocket debugger, pressing a button and reading `MenuPage`:

| | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| top row | map | brief | **game** | stats | controls |
| bottom row | audio (5) | display (6) | multiplayer (7) | | |

D-pad **right cycles within one row and wraps inside it** - `0,1,2,3,4,0` and `5,6,7,5` - while up
and down switch rows. So a walk built on "press next enough times", which is what a ring would
want, spins forever from the wrong row exactly half the time. The walk instead remembers which
pages it has seen since the last row change and presses **down** when it lands on one twice. That
is blind to how many rows there are and to which page is where; the target index is the only thing
it knows, which is the only thing it was told.

**LOAD GAME is not a page of its own** - it is an entry on the `game` tab, so the load target is
2. And the menu opens on 0, which is why the map row usually arrives having pressed nothing.

**R trigger was the first guess and it moves nothing here**, which cost one round. Every other GTA
pause menu tabs with the shoulders, and the game says otherwise on screen: the legend in the corner
draws `move` against the four d-pad glyphs. The prior beat the evidence that was already rendered.

**A request has to be accepted while the menu is already up**, and that is not a refinement. Opening
the map leaves the bridge handed over with the menu still open, so asking for LOAD GAME next arrives
in `Done` rather than `Idle` - and the first build only took requests in `Idle`, so the second row
did nothing when pressed and then something surprising later. Reported as "load game does nothing".

**The failure mode was chosen before the feature was.** With a page number wrong the walk runs out
of presses and stops, leaving the player in the game's menu a tab or two from where they asked -
and the debugger's Front End tab separates the two causes: a page that never moves means the
navigation control is wrong, a page that moves but never matches means the number is.

### The front end is a named widget tree, and that is how the chrome comes off

**The retail build kept the debug names.** Every widget's first field points at its own ASCII name,
so the front end prints itself:

```
MASTER      Background  Map_t  Brief_t  Game_t  Stats_t  Controls_t  Audio_t  Display_t  Multiplayer_t
MAP_PAGE    Map_AE  MapTitle
GAME_PAGE   LoadGame_MI  NewGame_MI  DeleteGame_MI  GameTitle  Reset_MI
BUTTONS0-3  Move  Select  back  placemarker  down_but  up_but  x_but  circ_but  square_but ...
```

Three things are hidden while the menu is up, and **all three are data writes** - `+0x20` on a
widget is a `visible` byte that the page draw checks and skips on, so there is no code to patch,
no JIT block marker to work around, and undoing it is writing the old value back:

| what | how |
|---|---|
| the tab strip | the root page's `*_t` widgets, `visible = 0` |
| the button-hint row | every child of every `BUTTONS*` group, `visible = 0` |
| the black band under both | every backdrop's height stretched to 400 |

**That last row is the one that was diagnosed wrong twice, and the correction is the useful part.**
With the strip and the hints gone the bottom of the screen stayed black. The first explanation was
"something is painting over it" - disproved by shrinking the backdrop to 240x120 and watching it
obediently shrink into the corner, so the rect really does drive the draw. The second was "the map
is laid out 480x224 to leave room for the strip, so those 48 rows are a hole" - which fixed the map
page and did nothing for Brief, Stats or Game, because they have no full-screen widget to grow.

**The real answer: the front end lays itself out in 480x272 - the PSP's framebuffer - and what is
actually displayed here is about 330 rows.** Measured two ways off one widget: a backdrop at h=272
covered 82% of the frame (implying 332) and at h=120 covered 37% (implying 324). So the band was
never a gap the strip left behind, and nothing the game draws was ever going to reach it. `MASTER`'s
`Background` at the game's own idea of full-screen stops 60 rows short of the screen it is on.

`backdropHeight` is therefore **400 - deliberately too big rather than measured**. Drawing off the
bottom costs nothing, an exact value would have to be derived from display settings the player can
change, and a backdrop that stops one row short is the entire bug returning.

**Grown by name, and that list is short on purpose**: `Background`, `Map_AE`, `WRAPPER`. A rule
like "any full-width widget" would also match `Reset_MI`, which is a 480-wide *menu row* on the
Game page - stretching that would turn one entry into the whole page.

### Left and right stop switching tabs

`lockMenuTabs` drops `CTRL_LEFT`/`CTRL_RIGHT` in the `Menu` context. It follows from hiding the
strip rather than being a separate opinion: with the tabs drawn, tabbing is navigation the player
can see; with them hidden it moves you to an unmarked page for no visible reason, and this fork's
menu is what picks the page now.

Claimed and dropped rather than unbound, which is the inverse-Escape-trap discipline applied at
runtime - an unclaimed arrow key falls through to PPSSPP's own mapper, which sends the very d-pad
direction being suppressed. Up and down are untouched: they are how `LOAD GAME` is selected on the
Game page.

**Matched by name, never by index.** The first working version used widget indices, and they were
right by luck - the eight tabs happened to be children 1..8 in that run. `EndsWith(name, "_t")` and
`strncmp(name, "BUTTONS")` cannot drift the way an index can, and they cost a string compare on a
tree that is walked once per menu open rather than once per frame.

**Applied on the edge, not every frame.** The game writes these fields when it builds a page and
never afterwards - confirmed by poking them and watching them stick - so re-asserting them every
tick would be the shape of mistake the camera pitch runaway already taught. `RestoreChrome` runs on
the way out and on shutdown, so turning the setting off, or taking a savestate with the menu open,
cannot leave the front end missing its furniture.

### Writing the page index froze the game, and the reason is a second field

`jumpDirectlyToPage` writes `MenuPage` instead of pressing the game's next-tab control until the
index matches. It removes the visible riffle through Controls/Audio/Display, it was shipped on, and
**it froze the game the first time someone pressed Enter on the Game page.**

```
Invalid exec address 00000000 pc=00000000 ra=08ae22c8
```

```
08ae22a8  lw    $a0, 0x30($s0)     ; the page's SELECTED widget
08ae22b0  lw    $a2, 0x24($a0)     ; its function table
08ae22bc  lw    $a2, 4($a2)        ; the method - read as 0
08ae22c0  jalr  $a2                ; <- ra=08ae22c8 is here
```

A page keeps its selection in `+0x30`, and writing `+0x1c` moves what is DRAWN without moving that.
So the Game page was on screen with a widget from the page you came from still selected, and Enter
made a virtual call through an object of the wrong type - which is why it survived two pointer
reads and died on the third. A null `+0x30` would have faulted on the *first* read; this got to the
third, which is what says "wrong type" rather than "missing".

**The verify-a-frame-later guard did not catch it and could not have.** It was added for exactly
this class of failure - the game reconciling the index on its next frame - and it was the wrong
guard: the index really did hold. Nothing was ever going to disagree about `+0x1c`, because `+0x1c`
was never the problem.

The setting stays, defaulted off, with the field named in its comment. The fast path is still the
right idea; it needs to move the selection with the page.

**The flicker is fixed the safe way instead.** `hidePagesWhileWalking` turns every page's widgets
off for the duration of the walk and back on when it ends - `visible` bytes only, the same
mechanism as the chrome, and it changes nothing about what the front end thinks is selected. The
backdrop stays up, so a walk reads as a brief pause rather than as four pages riffling past.

Every exit from a walk restores them - `Finish`, `GoIdle` and `RestoreChrome` all call
`ShowPagesAgain` - because a walk that ends any other way leaves the entire front end invisible.

### The front end has two levels of focus, and that one flag explains three bugs

`MenuUseRoot` (`FrontEndMenuManager + 0x1e`) reads **1 while the TAB STRIP has focus and 0 once you
are inside the page**. Measured on the Game page, not inferred:

| | useRoot | page |
|---|---|---|
| tabbed to it | **1** | 2 |
| after one Cross | **0** | 2 |
| after a second Cross | 0 | **10** - CONFIRM_PAGE, the "lose unsaved progress" prompt |

Three separate complaints were all this flag:

- **"I have to press another Enter to control the options."** A page tabbed to arrives with the
  strip still holding focus. `Arrive` now sends that Cross itself.
- **"Cross dismisses the map legend."** Same press. The legend belongs to the strip-level view of
  the map, so descending into the page is what puts it away - it was never a legend-specific
  mechanism, which is why generalising it cost nothing.
- **"The map cannot be panned."** The d-pad only pans once you are inside the page; at strip level
  it changes tab. An earlier hunt for the pan variables held a direction at strip level, watched
  the page index change, and concluded there were no pan variables.

**The guard on that press is not tidiness.** Sent while already inside the page, Cross does not
descend - it activates the selected entry, which on the Game page is `LOAD GAME`. `MenuOnTabStrip`
defaults to *true* when unreadable, so an unreadable flag makes the press conditional rather than
automatic.

### Dragging the map, and why this one writes state

`Map_AE + 0xc0` / `+0xc4` are what the map is centred on - symmetric x/y, found by descending into
the page and holding each direction (right moved x by -166.69 over the same interval down moved y).
`MapDragTick` adds the mouse delta to them while the left button is held.

This is the only part of the front-end work that writes game state instead of pressing a control,
and it earns the exception on the same grounds `VCSCamera` writes angles directly: **a held
direction is a RATE and a mouse gives a DISPLACEMENT**, and no input the game accepts means "pan by
this many units". Everything else here had a button that meant what we wanted; this does not.

Plus rather than minus, because a drag is the opposite verb to a d-pad press: holding right moves
the VIEW right (x decreases), while dragging right brings the CITY right.

The mouse reaches it because `ContextWantsMouse` now claims the delta in the `Menu` context while
`ContextDrivesCamera` refuses it - claimed but not steered, taken away from PPSSPP's own
mouse-to-analog path and spent on the map. `MapDragTick` runs from `FrontEndTick`, which is before
`CameraTick` in `VCSGame::Tick`, and that ordering is what lets it take the delta before the camera
drains it.

### Back comes back HERE, one level at a time

With the game's own menu up, Back shuts it and raises this fork's menu - the one those pages were
reached from - and the next Back leaves for the world. Two presses, two levels, and the row you
came from still has focus when you arrive.

It used to close the game's menu and drop straight into the world, on the reasoning that stacking
two menus is how one comes to feel like two. That reasoning still holds for STACKING and does not
hold for returning: the map is one row down from where the player was, and being sent two levels
for one press is how somebody loses their place.

**It cannot be done in one step**, and that is the shape of the code rather than a preference.
Closing the game's menu is a queued walk that presses the game's own Back control, and a menu of
ours on top pauses the emulator that walk needs to run. So `EmuScreen`'s `REQUEST_GAME_PAUSE` arm -
the one place every Escape, pad Start and Windows-menu pause funnels through - closes and sets a
flag, and `update` raises our menu on a later frame, once `GameMenuActive` has actually gone false
and the bridge has stopped driving.

**Backspace and the pad's B are what get you there, and neither is a PSP button any more.** Both
used to reach the game as Circle, which inside one of its pages lifts focus back to the TAB STRIP -
leaving the player standing in a menu this port navigates for them, in front of a row of tabs it
deliberately hides. They are claimed in the Menu context now and post `REQUEST_GAME_PAUSE` on the
press edge instead, the way the pad's Start already did. `VCSMenuScreen` answers Backspace as Back
as well, because `UI::IsEscapeKey` is PPSSPP's question about its own UI and has never heard of it.

### The Menu context finally triggers, and not the way it was tried before

`ResolveContext` returns `Menu` when `MenuActive` is set, which closes the TODO that has sat in it
since the beginning. Everything downstream was already written for it: the Menu rows in
`kVCSKeyMappings` map WASD to the d-pad, and `ContextWantsMouse` already excluded Menu, so the
mouse is left alone in menus without a line being changed.

**`GameState` was the wrong question, and that is the transferable part.** It was hunted twice as a
general "what is the game doing" enum and never found - a 192KB sweep gave 1249 candidates and a
correlation run ruled out the best cluster. The input layer never wanted the enum. It wanted one
bit, "is a menu up", and that bit is a field of the menu's own object, reachable from a script
command in twenty minutes. Ask what the caller needs before hunting for the thing it was called.

This is also **not** the reverted `FrameCounter`-stall shortcut from `03c1f3cfe0`. That inferred
the menu from the game's logic being stopped, which is also true during loading screens and
cutscenes - a superset that catches the game mid-play. This reads the menu's own flag.

### The page titles are art, and they have to be

`Tools/vcsmenuart.py` bakes the script-font headings into `assets/vcs/`, and they are baked
because PPSSPP cannot draw them any other way. `FontStyle` selects a font
*family* - `SansSerif` or `Fixed`, and that is the whole enum - and `SetFontNameOverride` maps a
family to one face **globally**. There is no way to ask for a brush script for one string and the
UI sans for the next. Baking them also matches what the game does: its headings are sprites.

Re-run the script after editing the title list; the keys in `TITLES` are the names
`VCSMenuScreen::PageTitle` asks for, so the two have to stay in step. It uses stock Windows fonts
(Brush Script MT) and ships only the rendered pixels, no font file.

**The backdrop is a flat fill now**, `kBackgroundColor` in the menu, and it is the generated
image's own base colour - the midpoint of that gradient - so nothing moved when the art came out.
`make_background()` is still in the tool but is no longer called; it is the only record of how
the patterned version was built, and one line in `main()` brings it back.

The one thing to know about that generator, if it ever is brought back: the leaflet base width is
derived from the leaflet spacing, not chosen. Narrower than the gap and the fronds read as
fishbones - a row of separate spines with the background showing through. At 0.8 of the spacing
they overlap into one silhouette with a jagged edge, which is the shape the eye reads as a palm.

Missing art is not fatal - a missing title simply does not draw - so the menu still works on a
platform where `assets/vcs/` was not deployed.

### The menu face: two font systems, two different names for the same font

Menu items are set in Pricedown (`assets/vcs/pricedown.ttf`, listed in `g_fontDescs` under the
new `FontFamily::Display`). Getting there took four wrong turns, all worth writing down because
every one of them fails *silently* - a font that cannot be found is never an error in either API,
the text just comes out in some substituted face.

1. **A family maps to exactly one face, globally.** `FontStyle` picks `SansSerif` or `Fixed`, and
   `SetFontNameOverride` sets the face for a whole family. Wanting Pricedown for menu items *and*
   the UI sans for the hint bar therefore means wanting a third family, which is why
   `FontFamily::Display` exists. Hijacking `Fixed` would have worked and would have quietly
   changed the code font in the dev screens.
2. **On Win10+ the text drawer is DirectWrite, not GDI.** `TextDrawer::Create` picks
   `TextDrawerUWP` unless RenderDoc is attached or the OS is older than Win10, in which case it is
   `TextDrawerWin32`. The first attempt registered the font with `AddFontMemResourceEx` - a GDI
   call - and it succeeded, handle and all, into a font table nothing was reading.
3. **The two APIs match on different names.** This font's name-table ID 1 is "Pricedown Black"
   and its typographic family (ID 16) is "Pricedown". **GDI matches ID 1; DirectWrite matches
   ID 16.** No single string satisfies both, so the desc carries the DirectWrite name, which is
   the one that matters on the platform this fork targets. Tools mostly report ID 16, so the name
   you get from asking a font library is the one that does *not* work under GDI.
4. **Nothing needs to register it at runtime.** `TextDrawerUWP` already builds its own
   `IDWriteFontCollection` from an in-memory font set, loading every filename in `g_fontDescs`
   through the VFS. Adding a row to that table is the entire integration. The file is an OTF
   (CFF outlines) named `.ttf` because the loader appends that extension unconditionally; every
   backend sniffs the container rather than trusting the name.

### A screen that pauses the game has to say so

`VCSMenuScreen::update()` calls `UpdateUIState(UISTATE_PAUSEMENU)` every frame, exactly as
PPSSPP's own pause screen does, and it is not decoration. The mouse pointer is the visible
symptom: `CorrectCursor()` in `Windows/MainWindow.cpp` only un-hides the cursor once the UI state
is something other than `UISTATE_INGAME`, and with mouse control on it also keeps the pointer
clipped to the window. Without the call the menu came up with no pointer at all. The framebuffer
manager and the WebSocket game broadcaster read the same state, so this is not only about the
cursor.

Measured rather than assumed, via `GetCursorInfo`: `HIDDEN` in gameplay, `SHOWING` with the menu
open.

### The main menu at launch, and the one thing it needs to know

`CreateStartScreen()` is what the app opens on, hooked into the `AfterLogoScreen::DEFAULT` arm of
LogoScreen. It returns the VCS main menu when a VCS disc has been booted before, and PPSSPP's
ordinary game browser otherwise - there is nothing to put behind LOAD GAME until we have been told
which ISO that is.

The main menu and the pause menu are the *same screen* in two modes (`VCSMenuMode`), because they
differ only in what the top row does and in what Back means. One page table, one look, one set of
option rows. Two things the MainMenu mode has to get right:

- Back on the root page is swallowed. The main menu is the bottom of the screen stack, and
  letting `UIDialogScreen` finish it would pop the last screen and leave the app with nothing.
- `EmuScreen::bootComplete()` records the disc through `File::ResolvePath`, not as given.
  `gamePath_` is routinely relative - a command line, a drag-and-drop - and storing it raw meant
  the main menu only found the disc when the app was started from the same working directory.
  It looked like it worked, because during testing it always was.

### The boot sequence, measured: there is no menu to bridge to

The launch-time main menu was expected to need a bridge into the game's own front end - drive its
menu, pick New Game, get out of the way. **That front end does not exist.** Measured on the retail
disc, cold boot, nothing touched:

| time | on screen | `FrameCounter` `0x08bb3bb4` | `TimeStep` `0x08bb3b5c` |
|---|---|---|---|
| 0-60s | logos, then the credits sequence | `0` | `0.0` |
| ~60-68s | the seam | | |
| 68s+ | gameplay, Fort Baxter, story running | `30919`, climbing ~32/s | `1.6684` |

VCS goes logos → credits → straight into the story. There is no New Game / Load Game screen at
boot at all; loading a save is reached from the pause menu once you are in. So the hardest part of
the launch-menu problem was never there, and "START GAME continues the autoloading" is the literal
truth: the game is already on its way in.

**The hook is the `FrameCounter` 0 → nonzero edge.** It is the moment the world starts, it happens
exactly once per boot, and both clocks agree on it.

Two things this measurement corrected:

- **`FrameCounter` does not count frames since boot.** It jumps 0 → ~30600 at the seam rather
  than counting up, so it is initialised with the world, not incremented from power-on. Only the
  edge is meaningful; the magnitude is not.
- **This is not the idea that was reverted in `03c1f3cfe0`.** That one used "logic stopped" as a
  *continuous* gate during play, where loading screens and cutscenes make it wrong. This uses the
  *first* 0 → nonzero transition after boot, once, to place the menu at the seam. Same address,
  different claim - and this one is monotonic, so the ambiguity that killed the other cannot
  arise.

**Cross skips the credits; Start alone does not.** Measured by injecting a press through the
WebSocket debugger 20s into the credits and timing the seam: it arrived 3.7s later instead of the
~60s the sequence takes on its own. So the skip presses the game's own button rather than trying
to fast-forward anything, which is why there is still a few seconds of wait after a keypress.
`FrameCounter` reads a fixed 30602 at the seam every run, so the value is deterministic even
though only the edge is used.

**The flow, as built.** `CreateStartScreen` boots the remembered disc with no menu in front of
it; `VCS::UpdateBootPhase` watches for the edge; `EmuScreen` acts on it when it lands. Any key
during the intro calls `RequestIntroSkip`, hooked in `NativeKey` next to the existing VCS key
claim.

(What `EmuScreen` did there was raise a menu. It loads the most recent save now - see the next
section but one.)

Do not trust a reading taken from a session that has been running a while: mid-investigation this
looked disproven, because the sample was taken from an instance that had already crossed the seam
and was showing the gameplay values. The credits really do run with both clocks at zero; check
what is on screen before believing a reading.

### There is no menu at the seam any more, because the game is what you asked for

The startup menu is gone. It was raised at the seam, over a game that was already running, and it
offered START GAME - which is a screen asking you to start the game you started. What happens
there now is the thing its LOAD GAME row was for: `RequestAutoLoad` walks the game's own front end
to LOAD GAME and takes the save the dialog offers, with no key pressed. `VCSMenuMode::Startup` is
still defined and nothing raises it.

Two things make that a small change rather than a feature:

**The dialog's own default IS the most recent save, and the game chose that, not us.** VCS asks
the firmware to focus the latest entry, so the list comes up on it. Measured with eight saves on
the memory stick: the list opened on the one written at 12:25 that day rather than on slot 0. So
"load the most recent" needs no slot bookkeeping at all - and a player who wants a different one
gets the ordinary list to move through, because the sequence stops pressing the moment the load
starts.

**A first run has nothing to load, and asks for nothing.** `RequestAutoLoad` checks the memory
stick for a `PARAM.SFO` under this disc's ID before it presses anything, and the story the game
has already started just runs.

The one place the sequence is patient rather than strict is the very first Start: the seam lands
inside the opening scene, where that button may be spent skipping something. It gets three goes,
and only the auto-load does - everything else still gives up after one.

### What a save is actually made of, and the bug that hid in it

**The save-menu request byte is not a save.** Setting `gp+0x1076` opens the save UI and the game
writes a file, and that file loads into the OPENING MISSION carrying your money, your clothes and
your percentage. Reported once as "the save game button worked, but loading it started the game
over", and reproduced here twice before the cause turned up.

The cause is in the script, eight instructions before it asks for the menu. The safehouse save
routine reads:

```
04 00 d0 15 07 01     0004: $789 = 1        ONMISSION
04 00 cd 04 07 01     0004: $4   = 1        "this game came from a save"
36 00 ce 1c d0 0f     0036: $284 = $783     the restart position, x
36 00 ce 1d d0 10     0036: $285 = $784                           y
36 00 ce 1e d0 11     0036: $286 = $785                           z
c0 02 d0 0e ce 12     02C0: weapon  -> $274
fe 02 d0 0e ce 13     02FE: armour  -> $275
96 00 d0 0e ce 14     0096: money   -> $276
5a 00 d5 78 d5 77     005A: get_time_of_day $2168 $2167
60 02                 0260: activate_save_menu
```

and the main script's first decision, on boot, is on the second of those:

```
$_4 == 0 && $2 == 0  ->  $ONMISSION = 1; 0289: load_and_launch_mission_internal 8 // Soldier
```

**`$4` is the whole bug, in the game's own words.** The engine writes the script's global block
into the save file; a save taken without that preamble carries `$4 = 0`, and the game that loads
it does exactly what the script says to do with a zero there. Confirmed from the other end too:
after loading such a save, `$4` reads 0 and `$789` reads 1 - a game running mission 8.

So `PrepareScriptForSave` does what those instructions do and `RestoreScriptAfterSave` puts back
what it changed, which is also what the script does four lines further down its own routine. The
restore runs from `GoIdle`, the one funnel every sequence ends through, because leaving
`$ONMISSION` at 1 after an abandoned save would quietly stop every mission trigger in the city.

Three things worth keeping from working it out:

- **Global N lives at `[gp - 0x71dc] + N*4`**, from the operand decoder at `0x08861a7c`: an
  operand byte `>= 0xcd` is a global and its index is `(type - 0xcd) << 8 | nextByte`. The base is
  a heap pointer - `0x09f68400` on one run - so it is read every time and never baked.
- **`$_N` and `$N` in a Sanny listing are the same numbering.** `$_274` decoded to `ce 12`, which
  is index 274. The underscore is notation, not a second address space, and assuming otherwise
  would have cost a day.
- **The weapons, armour and money at the end of that preamble are deliberately not written.** The
  load path (`MAIN_2139`) restores weapons from the engine's own save data and only reads `$274` /
  `$275` again on the wasted-and-busted path, which the script refreshes for itself at every
  mission start. The position is different - the load path copies `$284..286` straight back into
  `$783..785` - so that one is written.

**The restart position written is the PLAYER's, and NOT `$783..785` the way the script copies it.**
That is a deliberate departure, and the reason is that the position is only meaningful next to
`$_282`, the safe house whose interior is swapped in. `INITSAV`, the script's own spawn code, reads
the pair together:

```
$_282 > -1  ->  swap that interior back in, place the player from its own table
otherwise   ->  load_scene $783 $784 $785
either way  ->  get_ground_z_for_3d_coord ; set_char_coordinates there
```

The safe house routine can copy the pickup because it only ever runs while the player stands ON
that pickup, inside the house, with `$_282` naming it - both halves describe one moment. An
auto-save fires wherever the mission ended, with `$_282` at -1, so copying the pickup pairs an
INTERIOR position with a world where that building is solid. Reported from play: loading such a
save materialised the player inside the safe house's shell with no way out but dying, and it had
looked harmless for as long as one particular safe house was in use whose pickup happens to sit
somewhere escapable. Measured on the one that produced the report - restart `-800.73 -1183.65
10.90` against a pickup at `-817.56 -1181.43 13.76`, with nothing to swap that building in for the
second row.

Reading the player on the same frame `$_282` is saved cannot disagree with it. `$783..785` survives
only as the fallback for an unreadable player, and `$_287` - the heading `INITSAV` faces the player
in, degrees there and radians in `PedHeading` - goes with the position for the same reason.

### A loading screen over both of them, because the machinery is not the game

Neither automatic sequence is something the player did, and both of them put the game's own menus
on screen: a tab strip being walked, a save list, a firmware dialog answering its own prompts.
Reported, fairly, as "kind of clunky looking". So both go behind a curtain - the menu's own
backdrop with one word on it, LOADING or SAVING - and the player sees a loading screen where they
would otherwise have watched a menu being driven by nobody.

**It is drawn onto EmuScreen, not pushed as a screen.** A `UIScreen` pauses the emulator, and the
one thing this curtain cannot do is stop the game: what it is hiding is the game working. So it is
a draw call at the end of `EmuScreen::render`, after the route overlay, over everything.

**The same answer hides the walk and silences the pad.** A key pressed during a walk lands in
whichever page the walk has reached - it could pick a different save, or answer the prompt with No
- so `NativeKey` swallows presses whole while the curtain is up, before the mapper, before the VCS
layer and before the UI queue. Two exceptions, both deliberate:

- **Releases still go through**, keys and axes alike. A player who skipped the credits is still
  holding that key when the curtain goes up a few seconds later, and a release nobody is told
  about is a key that stays down forever - in this fork's held-key set and in PPSSPP's mapper. A
  release can select nothing.
- **The VCS layer is still told about axes**, and only the route to the mapper is cut, for the
  reason `NativeAxis` already documents: an axis carries its whole state in every event.

**It comes down on the GAME's clock, not on a timer**, and that is what makes it fit. The curtain
waits for the sequence to finish and then for thirty game frames - and the game's frame counter
does not advance while the world is being loaded, so those thirty frames cannot start counting
until there is a world again. Measured over a boot: up at the seam, and down as the first frame of
the safe house arrives, eleven seconds later, most of which is the game's own load.

There is a hard ceiling of 1800 vblanks on top of that, and it is not defensive habit: this thing
covers the screen AND holds the pad, so a curtain that can get stuck is a game that cannot be
played.

**A save the player asks for is not covered.** `RequestSaveMenu` raises nothing - somebody who
opened the save menu went looking for it. Only the auto-save does.

### The save menu is ours now, and the game loads a save by itself at boot

`GAME` on the pause root opens this fork's own page - NEW GAME, LOAD GAME, DELETE GAME - and the
two lists are ours as well: eight rows built from `PARAM.SFO`, the mission each save is named
after as the label and the game's own description as the help line, with a `*` against whichever
is newest. Everything on screen there was written by the game; only the drawing is this port's.

Loading still goes through the game, because only the game can put a world back together: the row
asks for a slot, the bridge walks the front end to LOAD GAME and steers the firmware's list to
that entry, all behind the loading screen. Deleting does not - it is a directory, and the game
re-enumerates the memory stick every time it opens a list, so `VCS::DeleteSave` removes it here
rather than walking into a third firmware dialog to do the same thing.

**The finding that cost the most, and explains three earlier confusions: VCS loads a save by
itself at boot.** Not through its menu, and not through this fork - the game asks the firmware for
a silent AUTOLOAD of a save it names, and the world that comes up is that save's world. It is why
a first run with an empty memory stick starts the story and a later run does not, and it is why
the fork's own auto-load usually looks like it "worked" even in builds where it had not run at all.

That is what defeated the first two attempts at NEW GAME:

- **Walking the game's own NEW GAME hung it, until the bridge let go of the world first.**
  Confirming that entry tears the world down, and the bridge was standing in the middle of the
  teardown holding pointers into it. The press was paced on the game's logic clock, which stops
  while the world is being rebuilt, so it never finished: the game came back with its own confirm
  page open on a black screen, its frame counter racing, and nothing recovered it. The same
  presses injected from outside, released on a wall clock, started a new game every time - that
  control run is what separated our timing from the game's behaviour. A wall-clock watchdog on the
  press was tried and did NOT fix it, which ruled out the stuck press and left the teardown itself
  as the thing to get out of the way of.

  What works is releasing everything that points into the front end BEFORE the confirming press -
  `ShowPagesAgain`, `RestoreChrome`, `g_frontEndVolatile`, and clearing any queued request so a
  stale Close cannot fire into the new world - then pacing that last press in VBLANKS and letting
  `Phase::EndSequence` dispatch ahead of the game-frame gate, so the pad is handed back even
  though the game's clock has stopped. Verified from the other end: the mission key empty,
  `$ONMISSION` 1 with mission 8 running, the player at Fort Baxter, the front end closed and the
  frame counter climbing - and on screen, Martinez's office, with no logos and no credit roll.
  That last part is the whole point of walking rather than rebooting.
- **Rebooting the disc is not enough on its own**, because the boot autoload then quietly
  continues from a save again. Twice this looked like "the reset did not happen".

The reboot survives as the FALLBACK, for the case the walk cannot start at all: the game keeps its
menu to itself during a cutscene, which is exactly where a player is most likely to ask for
another new game - the opening scene of the one they just started. Start is declined, the walk
gives up, and `AbandonSequence` boots the disc and tells that one autoload to find nothing
(`TakeNewGameBoot`, consumed by `PSPSaveDialog`'s autoload path). It costs the intro, which is
what walking was worth avoiding, and it beats a row that does nothing. That give-up used to stop
at an error toast, which is how "NEW GAME during a cutscene" came to do nothing at all.

### The questions are pages, not popups

Deleting a save, starting a new game and quitting each ask first, and all three ask on a page of
this menu rather than through `MessagePopupScreen`. PPSSPP's popup is a perfectly good dialog and
it is the wrong one here: it is a blue box in PPSSPP's own font over a menu that is neither.

One page serves all three, because a confirmation IS a page with two rows on it and this menu
already knows how to draw one. It carries the heading of whatever page asked - Delete Game, Game,
Quit Game - so the screen does not appear to jump somewhere else to ask, and Back on it goes to
the page that asked, which the page table expresses by making `ParentPage` a member rather than a
static: the parent of a question is wherever it was asked from.

The shape is San Andreas's, from the screenshots that prompted it: the question left-aligned under
the heading, wrapped in a box so a save's name can be as long as it likes, and NO above YES,
centred, with NO focused. **NO first is not a style choice** - the default answer to a question
nobody has read yet should be the one that changes nothing, and the game's own confirm page does
the same.

Every affirmative only RECORDS what to do; `update` acts on it a frame later. That is the same
rule the popups needed and for the same reason: a row that finishes the screen or rebuilds it from
inside its own click handler leaves the menu rendering and taking no input at all.

### The save list is a column, not a stack of centred names

Rows on the two save pages start at ONE x rather than being centred on their own length -
`VCSMenuItem::SetLeftAligned`, and it is the only page that asks for it. Eight titles of eight
different lengths, each centred on itself, read as a ragged pile; started at the same x they read
as the list they are. BACK stays centred, because it is an action rather than an entry in the
list.

### Saving after a mission, and how the game says one was passed

`01EB register_mission_passed` is the trigger, resolved from the script command table the usual
way - `u32(0x08b846e0 + 0x1eb*8 + 4)` = `0x08886064`. It memcpys eight bytes of GXT key to
`gp+0x1fc0` and increments `gp+0x1fc8`, and **the decompiled MAIN.SCM calls it from exactly one
place** - a shared subroutine every mission ends through. That is the game's own definition of a
mission being passed rather than a correlation with one.

`036A register_oddjob_mission_passed` bumps the counter and leaves the key alone, which is what
separates the two: a story mission changes the key, a race or an empire job only moves the count.
The auto-save watches the KEY, so odd jobs deliberately do not fire it.

Eight bytes get compared, not four. `LAN_C01` and `LAN_C02` share their first word.

**The counter is not a total.** It reads 0 on a 14.4% save, because a load does not restore it -
it counts the session. Only the edge is used, so that costs nothing, but do not put it on screen
as "missions passed".

**A load looks exactly like a mission being passed**, and that is the one trap in the watch: the
world restarting brings a different key with it. The frame counter rewinding is what tells the two
apart, and without that check the first thing an auto-load does is fire an auto-save over the save
it just loaded.

### Driving a dialog the game does not own

The load and the save both end at the firmware's savedata dialog - the slot list, the "overwrite?"
prompt, the progress bar - and PPSSPP draws all of it. So the doctrine the rest of this fork
follows, drive the mechanic and watch the state, runs out of state to watch: `FrontEndMenuManager`
freezes at whatever page it was on and nothing in PSP memory says which screen is up.

`Core/VCS/VCSSaveDialog.h` is the answer - a read-only report of what the dialog is showing,
filled in by `PSPSaveDialog::VCSPeek`. The alternative was pressing Cross on a timer, which is the
class of thing that works on the machine it was written on. Three things it has to carry:

- **The buttons, because which is which is a setting.** "Save completed" offers Back alone
  (`DS_SAVE_DONE` accepts only the cancel button), so a sequence pressing OK at it sits there with
  the save already written until it times out. Measured, from a run that did exactly that.
- **`yesnoChoice`, because the overwrite prompt opens on NO.** LEFT is what moves it to YES, and
  an OK sent without that press cancels the save it was meant to confirm.
- **Busy, which folds the IO thread and the fades into one answer**, because the caller does the
  same thing with both: wait. A press that lands in a fade is a press the screen underneath never
  sees, and the sequence goes on believing it was made.

**The dialog phases are paced in vblanks, not game frames**, and that is not a preference: the
world is stopped behind that dialog, so the logic clock every other phase here runs on has stopped
with it and would never hand out another frame.

### A menu we own sidesteps the GameState problem entirely

reVC's menu knows the game is paused because it *is* the game. This fork cannot ask VCS the same
question - `GameState` was hunted for and never found, and the `FrameCounter`-stall shortcut was
shipped and reverted for breaking the controls (see the `Menu` context note below).

None of that matters here, and it is worth being clear about why: the pause menu is a PPSSPP
`UIDialogScreen`, so PPSSPP owns its open/closed state and pauses emulation for it the same way
it does for its own. The unsolved problem is only reachable if a menu tries to sit on top of the
*game's* front end and mirror it. **A launch-time main menu - New Game, Load Game - is the case
that does need a bridge into the game's own front end**, and that bridge is a one-shot scripted
input sequence right after boot, not a per-frame state read. That is the narrow version of the
problem and it is still open.

Being a normal PPSSPP screen also means it runs on the same thread as `VCS::Tick()`, for the same
reason `UI/ImDebugger` does - see Threading above - so it reads and writes the settings structs
directly, with no marshalling.

### Hover is the one thing PPSSPP does not give you

PPSSPP moves focus on click and on keyboard/pad navigation, and has no notion of a hovered row -
`TouchInputFlags::HOVER` exists in the enum and nothing uses it. A menu whose rows do not light up
under the mouse reads as broken on a PC, so `VCSMenuItem::Touch` sets focus on a buttonless
`MOVE` inside its bounds. That is exactly what reVC's `UserInput()` does when it hit-tests the
pointer against every row, and it is the only piece of its input handling that genuinely had to be
written again. The buttonless check is what keeps it from firing mid-drag on a value.

### Five bugs this layout produced, all worth recognising again

- **The row you can see is not the row you can click.** Rows are laid out at the full screen
  width, because the two-column settings layout aligns on the screen's centre line rather than on
  anything the row owns. That makes `bounds_` a band the entire width of the display, so
  hit-testing it meant any pointer at the same *height* as a label counted as being on it, however
  far away horizontally. Measured, not guessed: a trace of the touch events showed every row as
  `bounds = 0.0, y, 1920.0 x 76.0`. `VCSMenuItem` now measures the drawn text during `Draw` and
  hit-tests that box, and reproduces `Clickable::Touch`'s press/release handling against it -
  delegating to the base is not possible, because every containment test in it reads `bounds_`.

- **Centre on the screen, not on the row.** Every row is laid out full width, so `bounds_.centerX()`
  looks like the screen's midpoint and reads correctly in code. It was not - the rows came out
  narrower than the screen - and the entire menu sat 80 pixels left of centre while every
  individual piece of it looked right. `ScreenCenterX()` asks `g_display` instead. Any layout
  built around a centre line wants the screen's, not a view's.
- **A screen outlives the graphics device, so its GPU objects cannot be freed in its
  destructor.** The title textures were released in `~VCSMenuScreen` and cached in a file-scope
  static, and both are too late: `NativeShutdownGraphics` calls `g_screenManager->deviceLost()`
  and then tears Vulkan down, while `NativeShutdown` deletes the screen manager much later
  still - and a static's destructor is later than that again. Quitting with the menu open
  therefore left textures alive past the end of the allocator, which VMA reports as
  `m_pMetadata->IsEmpty() && "Some allocations were not freed before destruction of this memory
  block!"`. Quitting from gameplay did not, because closing the menu had already released them -
  which is what made it look intermittent. The cache is a member now and `deviceLost()` is what
  empties it. Two things follow that are easy to miss: the release path has to be idempotent,
  since the destructor still runs afterwards, and a lazy loader has to be **gated** on the
  device being present - `Title()` creates a texture whenever the map lacks one, and `Release()`
  empties the map, so without the flag the first draw after a device loss would rebuild them all
  on a dead device.

  Still open, and deliberately not chased: closing the window with the menu up has once produced
  `vkQueuePresentKHR failed! result=VK_ERROR_DEVICE_LOST` from `VulkanRenderManager::Run`. It did
  not reproduce on the next attempt, it is a different failure from the VMA one above, and one
  occurrence is not enough to act on. Note it if it recurs; do not assume the fix above covers it.

  **Second sighting, 2026-08-19**, and this one was NOT at a menu: the process died mid-session
  during the vaulting work, a few minutes after an experiment that dropped the player through the
  world (writing the ped's climb state on land - see the vaulting section). Whether the fall is
  related is unknown; a ped below the map does put the camera somewhere the renderer never expects.
  Two occurrences, two different contexts, still not enough to act on - but if a third arrives,
  the common factor to look at first is the camera being somewhere absurd rather than anything
  about menus.

- **A "did we try yet" flag has to be cleared with the thing it guards.** `backgroundTried` was
  set on the first load attempt and never reset by `Release()`, so the backdrop appeared once per
  run and every menu after the first came up on the fallback colour - which looks enough like a
  deliberate flat background to not read as a bug. The flag and the texture are both gone now
  that the background really is a flat fill, but the shape of the mistake is not specific to
  textures: any cache with a companion "already tried" flag has to reset both together.
- **Taking focus on hover means taking it *forced* on click.** With hover focus added and
  `Clickable::Touch` reimplemented, the mouse stopped selecting anything at all: every row
  highlighted correctly and no click ever fired. `TouchEvent` calls `EnableFocusMovement(false)`
  on any press that did not force focus, which sends `LOST_FOCUS` to the focused view, and
  `Clickable::FocusChanged` clears `down_` and `dragging_` - so the release found nothing pressed.
  Stock survives this because it claims focus only `if (IsFocusMovementEnabled())`, and after the
  first press there is no focused view left to lose. Hover puts one back every frame, so that
  escape hatch is gone. `VCSMenuItem::ClaimFocus` uses `SetFocusedView(this, CAUSE_FORCED, true)`,
  which is what `TextEdit::Touch` uses to keep the focus it takes - and it has to run *before*
  `down_` is set, because `SetFocusedView` sends `LOST_FOCUS` to the outgoing view even when the
  outgoing view is this one.

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
- **Vehicle class detection**, confirmed in play. `VehicleModel` reads back correctly through the
  live `PlayerVehicle` pointer (209 `patriot` in a Patriot), and every non-car class has now been
  flown or driven: the aircraft and boat bindings behave as intended, across all 15 special models
  the spawner offers (8 helicopters, 4 fixed-wing, hovercraft, jetski, predator). What made that
  testable was a car spawner - see "Spawning special vehicles" below.
- **WASD movement**, confirmed in play, on foot and driving. Analog on foot; in a vehicle A/D
  steer the stick while W/S stay buttons.
- **Mouse look.** Yaw on foot and in vehicles; pitch on both as of 2026-08-17 - vertical look while
  driving is now on by default, with one known residual (see the pitch section below).
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

- **Mouse-native free aim, end to end**, confirmed in play and described there as *"genuinely
  smooth, how I wanted it... feels like PC native"*. Four pieces had to be right at once:

  | | |
  |---|---|
  | the ray | built from the camera's stored `Front`/`Up`/`Source`, not reconstructed from the angles |
  | its length | the weapon's own range out of `CWeaponInfo`, not whatever the game's target implied |
  | the gun | `m_pPointGunAt`'s entity moved onto the crosshair, down the same ray the bullet takes |
  | vertical | a deadband lead on pitch, because the aim camera will not move until the angle leads it |

  **The settled configuration**, which is now the default: crosshair 0.5/0.5, yaw trim 0,
  camera-origin ray on, weapon range on, `aimPitchDeadband` **0**, `aimYawKick` **0**,
  `aimYawDeadband` **0**, everything else off. `pedFollowAim` off - the game turns the character
  itself. The `vertical` row above is superseded: neither axis stalls once the gun target is placed
  along the asserted aim - see "There was never a deadband" below. Until 2026-09-15 this line read
  0.165 and 0.166, and those two leads were the rightward free-aim drift.

### There was never a deadband: the free-aim camera turns toward the gun target

**Status: fixed 2026-09-15, confirmed in play. Supersedes the three sections below** - the deadband
lead, the yaw kick and every argument about stiction. They are kept because the measurements in them
are real; what they measured was this.

Reported as "software stick drift": after a flick, an aim press or a shot, the crosshair went on moving
with the mouse still - and narrowed down in play to **strokes to the right, never to the left**. A
temporary per-game-frame recorder in `CameraTick` settled it in one session. In every one of fourteen
captures the game's `CameraYaw` sat within **±0.1669 rad** of the value we wrote: exactly that far
behind the lever while a stroke was moving, pinned at that distance after a leftward stroke, and after
a rightward one walking the whole window - up to 19 degrees - before stopping at the far edge.

**The mechanism, read out of the game.** While the ped has a gun target (`CPed+0x81C`, which in free
aim is always the placeholder `pedAimGun` moves), the free-aim camera does not integrate a look axis at
all. Around `0x0899cd10` it works out the yaw and pitch that frame the target, adds the crosshair
offsets (`0x0899d0e8`), and at `0x0899d14c..0x0899d278` turns Beta and Alpha toward the result by at
most `[gp-0x3584] * TimeStep` - **0.1 * 1.668 = 0.1668 rad a frame**. That rate is the "9.5 degree
deadband": we write the angle, the game steps back toward the target by up to one rate.

`PedAimGunTick` put the target down the camera's **live Front**, which closed a loop through that step.
The game turns by the crosshair offset to frame the target, Front turns with it, the target moves with
Front, and the game turns again - until the step hits the edge of the window around our write. The
offsets have a fixed sign, so the walk only ever went one way. The yaw kick decided how much of the
window was left to walk: after a rightward stroke it was parked on the far side, after a leftward one
the walk was already at its end. Pitch did the same through its own lead.

**The fix is to break the loop, not to tune anything:**

- `AimIntentRay` builds the target's ray from the angles this tick asserted rather than from the live
  Front, so nothing the game does can move the target. Front is exactly
  `(cos(Beta - PI), sin(Beta - PI)) * cos(Alpha), sin(Alpha)` - the recorder read `frontYaw == CameraYaw
  - PI` and `frontPitch == CameraPitch` to four decimals. `SolveAimRayAlong` reuses the fire hook's
  crosshair maths on that vector. Mounted guns keep the live ray: there `CameraYaw` is an offset from
  the vehicle's nose, and the attached branch never reads the target.
- `aimYawKick` and `aimPitchDeadband` are 0. With the target where the aim is, the step never binds,
  and a lead is only an offset.
- Free aim is asserted from its first frame - a zero-length stroke on entering Aiming, re-taken once if
  the camera mode changes before the mouse moves - and the hold is parked for as long as aim is held.
  Both close the gaps where nothing was asserted and the target fell back onto the live camera.

Verified from the fixed session's capture, not only by feel: every still period starts within 0.04 rad
of the lever and settles to within 0.02, with no walk in either direction.

**The lesson is the one this file keeps recording, one level further in.** Every measurement of the
"deadband" was correct: 9.4 degrees on pitch, 9.56 on yaw, symmetric, "releasing the instant the view
breaks loose", "Front moved one deadband while the camera angle moved two". So were the contradictory
readings - "converges onto the lever" and "a held lead is perfect" - because they were the two
directions of one walk. What was never done was reading the code that consumes `CameraYaw` in this
mode. It had been assumed to be mode 45's `Process`, whose unattached path builds Front straight from
Beta and Alpha and has no deadband in it at all. **When a fitted model needs a new setting every few
builds, stop fitting and find the code.** Reading the code that writes the field, and whatever it
reads alongside it, took a morning.

A tooling note: the camera-mode jump table at `0x08b7ed88` holds eight-instruction stubs rather than
functions, each with its `jal` at +0x10, and the stub a mode index lands on is not obviously the one
you would pick by counting. Scanning RAM for writes to `+0x7c` near reads of `+0x81c` found the right
function without resolving the table at all.

### Both axes need a deadband lead. Only one of them survives being given it closed-loop

The aim camera will not move vertically until `CameraPitch` leads the real aim by about **9.4
degrees**, measured directly:

```
before pitching   Front pitch -0.0442   CameraPitch -0.0442   off  0.00 deg
still no movement Front pitch -0.0442   CameraPitch +0.1198   off -9.40 deg
first movement    Front pitch -0.0390   CameraPitch +0.1278   off -9.56 deg
```

The two start **identical**, so they are normally one number. At 0.004 rad/count that gap is ~41
mouse counts of nothing before vertical starts - "Y needs a hard flick", in units.

The fix is the same doctrine as `aimResponseModel`: **integrate the intent at 1:1 and solve for the
lever.** Track where the player is asking to look in `Front`'s own space, then set `CameraPitch` to
that plus the deadband. The lead appears in full on the first frame, so pitch starts immediately,
and it collapses when `Front` catches up - it removes delay without adding speed. Anchored to the
live `Front` every frame, so a wrong deadband costs one frame rather than compounding.

**Scaling the input instead does not work**, and the reason generalises: the deadband is a
*constant*, so paying for it with gain overcharges every movement that was never near it. That
build was reported as hyper-sensitive. A constant error wants a constant correction.

**Then the same treatment was applied to yaw, and it caused a snap that took four attempts to
undo.** Yaw's deadband is **0** - horizontal aiming was already smooth on the plain accumulator, and
the lead created a limit cycle: `Front` chases a lever leading it by `D`, passes the intent, the
error crosses zero, the sign flips and the lever moves `2D` in one frame. Measured at 0.340 rad
against 0.328 predicted.

Three cures were tried on that snap and **all three failed, because all three treated the cycle
rather than the setting that created it** - re-solving on still frames (a runaway: the lever is
written directly, so each pass adds another deadband), retracting the lead at stroke end (wrong
moment, the snap is mid-stroke), and blending the lead through zero (right mechanism, wrong layer).
What fixed it was `aimYawDeadband = 0`.

**The lesson is about where to look, not about aiming.** The snap was reported in the same build
that introduced the yaw lead, and that fact was in hand before any of the three attempts. A symptom
that appears with a change is evidence about that change, and it outranks any mechanism you can
derive for it afterwards - all three cures rested on real measurements and correct arithmetic, and
were still aimed at the wrong thing.

Why the axes differ is not mysterious once the camera code is read: they reach `Front` by different
routes in mode 45 - pitch through `SetRotateX(Alpha)`, yaw through a Z-rotate whose offset is picked
from four quadrants by `ped + 0x780`. There was never a reason to expect one number to describe
both. The 9.4 degrees they appeared to share was only ever measured on pitch.

**Then it was measured on yaw, and the number is the same.** 2026-08-21, off the `LIVE Front yaw`
row while free-aiming: `Beta` leads `Front`'s yaw by **9.56 deg (0.166 rad)** and `Front` does not
move at all, **symmetrically** - push right and the offset reads +9.56, push left and it reads -9.56
- releasing the instant the view breaks loose. A constant between the two spaces would hold one
*signed* value both ways instead of mirroring, so this is stiction, and it is within noise of
pitch's 0.165.

So the paragraph above is wrong where it counts, and the way it went wrong is the useful part.
**"Yaw does not need the lead" was never measured; it was inferred from the lead making things
worse** - which it genuinely did. Both halves were true and the conclusion joining them was not. The
symptom was about the *mechanism that delivered* the lead, and it got read as being about the lead.
That is the same error this file has now recorded four times, and this time it was made while
writing down the rule against it: the section immediately above says a symptom appearing with a
change is evidence *about that change*, and the change was the closed-loop solve, not the constant.

`aimYawKick` carries the same 0.166 open loop - `desired = accumulator + kick * strokeDirection`,
nothing read back. With no measured error there is no sign to flip on its own, so the limit cycle is
not tuned away, it is **unrepresentable**. The one place a reversal still costs `2*kick` is when the
player genuinely reverses, gated behind a hysteresis threshold so a wobble inside a stroke cannot
trigger it. `aimYawDeadband` stays at 0 and stays visible, because the distinction between the two
is the whole lesson and deleting the loser hides it.

**The general form, which is worth more than the aiming fix:** when a correction makes things worse,
separate the *quantity* from the *delivery*. A closed loop and an open loop carrying the identical
constant are not the same experiment, and only one of them can produce a limit cycle. Four attempts
went into curing that cycle and a fifth into abandoning the constant, and none of them tried simply
handing the same number over by a route that has no feedback in it.

**Settled in play the same day, and it corrected the model.** The kick shipped with
`aimYawKickRetract` ON - drop the lead once, on the first still frame - on the reasoning that the
hold window expires 45 ticks later and the game inherits whatever is in `Beta`, so a lever parked a
deadband past the aim is a value the player never asked for. Reported at once: the aim snapped
somewhere just after each stroke ended. Unchecking it made free aim, in the player's words, perfect.

Every clause of that reasoning is true. The unstated assumption is what failed: that moving the
lever back by `D` is *invisible*, i.e. that the deadband is a standing gap `Front` sits inside. It
is not. **The deadband is static friction - it gates the ONSET of movement and does not persist once
the aim is at rest.** So a full-deadband step applied to a resting lever is precisely the step that
breaks it loose again, and the view follows it. The retract was working exactly as designed.

Two things fall out. The lead can be held indefinitely at no cost, because a lead that never moves
never moves anything - which is independent confirmation of why pitch has run with `aimLeadRetract`
off since the beginning, arrived at from the opposite end. And the "does `Front` converge onto the
lever or settle short of it" question that `aimLeadOnStallOnly` was built to answer is the wrong
question: at rest there is no offset to hold either way, and the whole quantity only exists while
something is moving.

**Addresses found so far**, all verified stable across a fresh boot (the four `Pad*` entries are
documented in "VCS has a second analog stick" below rather than repeated here):

| address | value | how it was confirmed |
|---|---|---|
| `PlayerVehicle` | `0x08bb4064` | 3-snapshot intersect; position matched the player, not traffic |
| `PlayerBase` | `0x08bc8170` | entity matrix whose position tracks the player |
| `PlayerHealth` | `PlayerBase + 0x4e4` | wrote `25.0`, HUD bar dropped to exactly a quarter width |
| `VehicleModel` | `PlayerVehicle + 0x56` | read 209 (`patriot`) in a Patriot; the same offset across the whole vehicle pool (~30 objects, `0x820` stride) gives coherent models and distinct positions |
| `CameraYaw` | `0x08bc7f1c` | correlated across 7 snapshots, then write-tested (62% of screen changed) |
| `CameraPitch` | `0x08bc7f18` | same correlation; sits 4 bytes *before* yaw |
| `IsAiming` | `0x08bb32a0` | 1 only while **locked on** |
| `IsFreeAiming` | `0x08bafb54` | 1 only while **free-aiming** (sniper/RPG); counted aim cycles matched transitions exactly |
| `TimeStep` | `0x08bb3b5c` | `gp + 0x1dfc`. Read as a float by every camera mode that integrates the look axis, e.g. at `0x089a37f8`. Reads 1.668 in gameplay and 0.0 at the menu |
| `FrameCounter` | `0x08bb3bb4` | `gp + 0x1e54`. The `lw / addiu 1 / sw` at `0x08a114c4`, last thing `CTimer::Update` does. 11621 in an in-game savestate, 0 at the menu |
| `PedHeading` | `PlayerBase + 0x8d0` | the only adjacent float pair matching `atan2(-fwd.x, fwd.y)` from the ped's own matrix; the game reads it back in `FireInstantHit` to see how far the player turned between shots |
| `PedHeadingTarget` | `PlayerBase + 0x8d4` | `m_fRotationDest`, beside it |

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

**SUPERSEDED, conditionally — and the condition is the whole point.** The verdict above was
correct and it was *contingent*: it held because the shot was resolved from the ped's aim state.
The fire-site hook resolves it from the camera, so the condition is gone and the conclusion goes
with it. `CameraAimActive` now returns true whenever the hook is installed and enabled, gated on
**installed** as well as enabled — with the patch not landed, the shot is resolved the old way and
driving the camera would reintroduce exactly this desync.

Worth asking of any negative result in this project: *what would have to change for this to stop
holding?* Two documented negatives turned out conditional rather than permanent on the same day —
this one, and "VCS has no free aim for ordinary weapons". Both were correctly measured. Both
described the game plus an assumption.

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
| `CCam + 0x10` | **`Front`**, unit — the direction the camera actually looks |
| `CCam + 0x20` | **`Source`**, the camera's world position |
| `CCam + 0x60` | **`Up`**, unit and exactly orthogonal to `Front` |
| `CCam + 0x78` / `+0x7c` | pitch / yaw — i.e. this fork's `CameraPitch` / `CameraYaw`, on `cam[0]` |
| `CCam + 0x124` / `+0x130` | the smoothed per-frame increment added to those |
| `CCam + 0x128` | FOV, degrees, vertical |
| `CCam + 0x190` | the point the camera is looking at (the player, `z + 0.6`) |
| `CCamera + 0x7b8` | `PlayerWeaponMode.Mode` (s16) |

**The three bold rows correct an earlier claim in this file** that `Front`/`Up` "did not reconcile
with `CameraYaw` when measured" and therefore could not be used. They don't reconcile, and they
never will: `Beta`/`Alpha` are the ORBIT angles about the look-at target while `Front` points from
`Source` at that target, and the player is not in the middle of the screen. See "Shots landed
chaotically because the ray was built from the wrong two numbers" below — that 4.42° gap is the
whole aiming bug, not a reason to distrust the fields.

The index math is at `0x08a228a0` (`idx * 608 + 0x70`) and the mode jump table at `0x08b7ed88`.
Mode 15 — the ordinary on-foot follow camera — routes to `0x08999f60`, which **never reads the
look axis at all**. That is why writing `CameraYaw` works so cleanly for mouse look and so badly
for aiming: on foot there is no integrator to fight, and in free aim there is.

### Shots landed chaotically because the ray was built from the wrong two numbers

The fire-site hook worked — the bullet went where it was told, confirmed by deflection test — and
the bullets still did not land on the crosshair. Three separate errors, and the first one is the
interesting one because it had already been *found* and then explained away.

**1. The direction came from `CameraYaw`/`CameraPitch`, which are not the camera's forward.**

The hook rebuilt a direction as `(cos, sin)` of `camYaw - PI` with `camPitch` used as-is. Both
conventions were calibrated in play, and both are correct descriptions of what they measured — the
camera's **orbit angles about its look-at target**. The camera's actual forward is a separate stored
vector (`CCam + 0x10`), and the two differ by however far the player sits from screen centre:
**4.42°** in the savestate, about 3.3 horizontal and 2.8 vertical.

The reason this reads as *chaos* rather than as a constant miss is that an angular offset is not a
constant: it grows as the camera closes on the player and swings as the camera orbits. The
`crosshairX` slider was fitting its horizontal half — which is why the value that was right at one
angle was wrong at another, measured at 0.5125 to 0.5300 across a 180° sweep, non-monotonically —
and the vertical half had no slider at all.

Two numbers agree on what the trim was really measuring: it wanted **3.2°**, and `Front` sits
**3.3°** off the angle it was correcting. So with `Front` used directly, `crosshairX` should want
0.5. It defaults there now, as a prediction rather than a fitted value.

**2. The ray length was whatever the game's own target happened to be.** That length means three
different things across `FireInstantHit`'s branches — the weapon range on one, the distance to a
locked-on ped on another, the distance to the free-aim dummy on a third — and an auto-aim assist
can shorten it again before the raycast. The range is read out of `CWeaponInfo + 0x08` now.

**3. `useCameraOrigin` was off, so the ray started at the gun.** It was off for an honest reason:
the camera position had been chosen by ranking candidate vec3s for being "most anti-parallel to the
camera's forward vector", using the forward vector that turns out to be 4.4° wrong. `+0x210` won
that ranking and put shots nowhere visible. The comment left behind said the method was the suspect,
and it was right. Measuring instead — which candidate's bearing to the look-at point reproduces the
stored pitch — gives `+0x020` unambiguously, because the six candidates are hard to separate
horizontally and trivial to separate vertically.

**The method note, which is the transferable part.** All three were settled offline in one sitting
with `Tools/vcsstatic.py` and one gameplay savestate, by *geometry* rather than by correlation or by
play-testing: `Front` reproduces `normalize(LookAt - Source)` to five decimals, `dot(Front, Up)` is
exactly 0, and `Source` is 4.63 m from the player on the far side. A savestate is a complete world
state, so anything with a geometric relationship to something already known can be identified
without booting the game — and a wrong answer shows up as a number that does not fit rather than as
a shot that goes somewhere odd.

This is the third time in this file that a **correct measurement produced a wrong conclusion**, and
the shape is always the same: the observation is about a symptom, the conclusion is about a
mechanism, and nothing in between was read. Here the missing step was two vec3s sitting 16 bytes
apart from a value the table had held for weeks.

### The bullet followed the crosshair and the man did not

Driving the camera in free aim left the character **frozen, aiming wherever he happened to be
pointing when the aim key went down**. Not a rendering artefact: his heading is where the gun
points, and in free aim nothing turns it once we stop feeding the stick.

The cause is the same latch that makes movement in free aim possible. `MoveGateBranch` skips the
movement call, so nothing re-evaluates the ped's heading either — whatever it was on entry is held
forever. That is the feature in one place and the failure in the other.

`PedAimTick` in `VCSCamera.cpp` writes `m_fRotationDest` (and `m_fRotationCur`, since a mouse is a
position control and a turn rate is the lag the aim model exists to remove) from the live camera
yaw. `PedHeading` / `PedHeadingTarget` are `PlayerBase + 0x8d0` / `+0x8d4`; see
docs/VCS_ADDRESSES.md for how they were found and for the `camYaw + PI/2` conversion.

**Then the body turned and the gun still did not**, described in play as *"his hand acts exactly like
a chicken's head — locked into place while the rest of the body is moving"*. That names the mechanism
exactly: **the arm aims at a world POSITION, not at an angle.** Nothing moved that position once the
stick stopped being fed, so the hand held its world bearing while the body rotated underneath.

The position is `CPed + 0x81C` (`m_pPointGunAt`), and moving that entity is the fix. It drives the
native shot as well as the arm — for any non-ped target `FireInstantHit` reads `entity + 0x30`
straight into its raycast target (`0x08a48a6c`), so the gun and the bullet agree by construction.
`pedAimGun` writes it every frame, down the same ray `SolveAimRay` gives the fire hook.

**Measured, and it corrected the guess:** the entity is type **4, an OBJECT** — not the type-5 dummy
that `FireInstantHit`'s special-case branch implied. That branch is the exception, not the rule. The
guard is therefore a blocklist: never write the position of a ped, vehicle or building, because that
teleports something the world owns; everything else is a placeholder.

### The game already turns the player, and writing it ourselves was the lag

`pedFollowAim` shipped on, produced a character that turned correctly, and then produced a new
complaint: *"when aiming, I have to wait until the character's legs/torso turns and then I can move
the crosshair along with the full body."* Horizontal aim could not outrun the body.

**Mode 45's `Process` writes the ped's heading itself, every frame** — `0x089a3e20`..`0x089a3e5c`:

```
angle = atan2(Front.y, Front.x)      ; the camera's own look vector, CCam+0x10
if (angle < 0) angle += 2*PI
angle -= PI/2                        ; 0x3fc90fdb, loaded at 0x089a3984
ped->m_fRotationCur  = angle         ; +0x8d0
ped->m_fRotationDest = angle         ; +0x8d4
```

which is the same formula this fork had derived independently. So `pedFollowAim` was a **second
writer of one field at a different rate** — ours at vblank, the game's at its 30fps logic rate. That
is the exact shape of the CameraPitch runaway documented above, and it is off now.

Two things worth carrying:

- **Before writing a field, check whether the game writes it.** This one is not subtle: the write
  sits inside the very function whose aim response the fork already reverse-engineered in detail.
  Reading a function for one purpose does not mean it has been read.
- **The game resolves the ped's aim through `Front`, not through Beta/Alpha.** Independent
  confirmation that the fire hook's rewrite was right — the game's own aiming has always gone
  through the stored basis, and the angles were never the aim.

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

### Ask the GXT what a control is called

**`ENGLISH.GXT` contains the whole control scheme, written down by the people who made the game.**
Not the Controls *screen* - the strings that screen is built from, plus every help line that names
a button, and they are readable offline with nothing running.

The keys are `C<n><ACTION>`, one set per `CONFIGURATION` the options screen offers, so `C0*` is the
shipping layout and `C1`-`C3` are the rearrangements. The values are English, not glyphs:

| key | value | what it settles |
|---|---|---|
| `C0TGSUB` | `the up button` | recruit is d-pad up |
| `C0FREE1` | `down button` | free aim is d-pad down, as the Controls screen says |
| `C0PDLT` | `R button` | aim is R trigger |
| `C0PDLO1` | `L button` | the "unused" L trigger is Look/Fine Aim |
| `C0VEHN` | `down button` | horn |
| `C0SNZI` / `C0SNZO` | `~S~` / `~X~` | sniper zoom in is Square, out is Cross |

Every one of those is a fact this file records as *measured*, some of them after a day of probing.
The help lines are the other half: `H_GANG1` is "To recruit henchmen into your group, target them
and use ~TGSUB~", which is both the binding and its precondition in one sentence.

The format is GXT2 with `TABL`/`TKEY`/`TDAT` sections; a `TKEY` entry is a 4-byte offset followed
by an 8-byte name, and the offsets are into `TDAT`+8 as UTF-16. Forty lines of Python reads it.

**Read this before the button tester, not after it.** The tester answers "what happens when I hold
this", which is the wrong question for a control that needs a target, a modifier, or a place to be
standing - and the two controls that cost the most time here, L trigger and d-pad up, are both of
that kind. This is the same lesson as "Read the game's own Controls screen first", one level
further in: the screen is a rendering of this table, and the table has entries the screen never
shows.

### Free aim is a button

`Free Aim` = **d-pad down**, bound to `S` in the Aiming context. Verified in play: with aim held,
left/right cycle targets and **down** enters free aim - after which the mouse moves the crosshair
through the ordinary reticle path (mouse → nub), with no flag set and no second-stick machinery
at all. It works with "Drive the game's second stick" **off**.

**"d-pad up does nothing" was part of that same observation, and it was wrong.** Up is `Recruit`,
and it does nothing without a gang member actually targeted - which is not a state anyone reaches
while probing buttons. The GXT section above had it written down the whole time.

Character movement is unavailable in free aim, which is expected: the nub is the aim.

The sections below record how this was approached before that was known. They contain real and
still-correct findings about the pad layout and the flag, but `usePadStick`, `CameraInputMode`
and the CLEO route are **not needed for free aim** and should not be reached for first.

### The game takes the lock-on back, and the gun has to keep up

**The symptom, because it is unmistakable once named:** mid-free-aim the character stops physically
aiming where the crosshair is. The view still turns with the mouse, the shots still land on the
crosshair, but the pose is stale and the only thing that moves his hands is WASD. After a while it
fixes itself.

**Confirmed cause:** `IsAiming` goes to 1. VCS re-acquires a lock-on **by itself** when a target
wanders into range, with nothing the player did asking for it. That turns `FreeAimActive` false,
`PedAimTick` used to return at that gate, and the gun stopped being pointed - while
`ContextDrivesCamera` stayed *true*, so the camera carried on following the mouse and the fire hook
carried on putting the bullet where the crosshair is. It ends when the game loses the target.

**Escaping the lock-on does not work, and this is worth writing down so nobody spends the evening
on it twice.** The obvious fix is to re-press the game's own Free Aim button on the rising edge of
`IsAiming` - the pulse that gets you into free aim on entry, fired again. It was built, it was
correct in shape, and it changed nothing: **the press that enters free aim from a standing start
does not break a lock the game has already taken.**

**What works is not fighting it.** `PedAimGunTick` now runs *before* the `FreeAimActive` gate, so
the gun keeps tracking the crosshair through a lock-on the game took on its own. That is safe
because it was never the write the gate was guarding: the gate protects the HEADING write, which
fought the game's facing logic during melee lock-on and once came out as reversed movement, and
that write keeps its stricter condition. The gun works by moving the entity the ped points at, and
`CanMoveAimTarget` already refuses anything the world owns - so a real lock-on onto a person is
left alone, and the case this rescues is the one the debugger showed: the target still the free-aim
placeholder object, still movable, and simply no longer being moved.

**The debugger names the gate now**, which is what ended this. `AimPathStatus` prints one line in
the Camera tab - `camera aim`, `reticle`, or which specific term of `FreeAimActive` /
`ContextDrivesCamera` is suppressing the path. Four candidates produce an identical symptom, and
guessing between them costs a play session each time; the line costs nothing and answered it
immediately. Reach for it before theorising about any aim complaint.

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

**Status: the code is gone; the findings are why this section stays.** `usePadStick`,
`PadStickTick` and the rest were removed once free aim turned out to be a button - the mechanism
worked and was never needed, and it sat behind a switch nobody turned on. What is worth keeping is
everything below: the two pad functions, the field offsets, and the fact that VCS wants a second
stick at all. Rebuild it from here if a reason ever appears; do not reach for it first.

It was written as the one that superseded both the reticle work and the plugin route. That was
wrong in the same way both of those were - see "Read the game's own Controls screen first".

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

**Status: the code is gone.** `aimViaRightStick` and `ApplyAimStick` fed the PSP's right stick for
a plugin that was never installed here, so the path could not run at all; nothing in VCS itself
reads that stick. The account below is kept because it explains how another port solved this, and
that is a different kind of fact from a switch in our own menu.

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

**Opcode `03E9` is now known, and it is not a mystery worth chasing further: it sets the aim axis
scale.** Its handler (`0x089e0b14`, resolved through the script command table at `0x08b846e0` — see
docs/VCS_ADDRESSES.md) calls `CPad::GetPad(0)` twice and stores its two float parameters to
`CPad+0xd0` and `CPad+0xd4`, i.e. this table's `AimAxisScale` and the `AimAxisScaleY` beside it. So
their `03E9 1.4 0.0` is "scale X by 1.4, kill Y" — the plugin zeroes Y because it drives Y itself.
The retail script calls it exactly once, `03E9 2.5 0.5`, setting up a passenger drive-by. That also
means the aim response model already reads one of the two values this opcode writes.

**Still unknown:** the `CPad + 29` write, which needs the PRX disassembled. And `cleo.prx` is a
third-party binary of unknown licence: fine to run locally, **don't commit it**.

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
testing, where holding it visibly produced lock-on. It is not the L *trigger* (not to be confused
with the keyboard `L`, which since 2026-08-17 toggles the fork's own lock-on mode for melee).

### Camera pitch: limits must be anchor-relative, and vehicles are excluded

Two things here were learned expensively.

**Pitch limits must be anchor-relative, not absolute** - the baseline is not guaranteed to be the
same per camera, and an early fixed `+/-0.9` pinned the view where it could not be brought back down.
`VCSCamera` clamps to `anchor +/- kPitchRange`. Don't reintroduce absolute limits.

**Corrected 2026-08-17: this section used to claim the vehicle baseline was about `-1.55`. It is
not.** Measured live with `pitchInVehicle` off, so the fork was not writing the field, the game's own
in-vehicle `CameraPitch` is **`-0.11861` rad (`-6.80` deg)** and completely steady - the same
ballpark as the `-0.05` on foot. The vehicle camera is `CCam` mode **18**.

`-1.55` was near-certainly read *while the pitch bug was active* and then written down as the
baseline, so every conclusion drawn from "the vehicle anchor sits at -1.55" was unsound. `-1.5532`
rad is `-89.0` deg, a hair under `-PI/2`: that is the **game's own** pitch limit, not this fork's
clamp. With the anchor at `-0.1186` the fork's window is `-0.8187 .. +0.5813`, so the fork
*cannot* produce `-1.55` at all.

**That rules the clamp out as the cause of the vehicle pitch bug.** The fork writes at most `-0.82`
and the game still ends up pinned at `-89` deg, so the wind-up happens downstream in the game's own
camera; changing `kPitchRange` cannot fix it. Mode 18 writes pitch itself every frame - four
pitch/yaw write pairs sit in `0x089a1xxx` on `$s0` - so the field has two authors and the game is
integrating something the fork's write perturbs. Recovery by exiting/re-entering the vehicle, or by
glancing with L/R, is consistent with that: both make the game rebuild its camera state.

**Pitch IS driven in vehicles now** (`pitchInVehicle`, on by default since 2026-08-17). It was off for
a long time for the reason described below, which is now understood and fixed. Historical account of
the failure, kept because the measurements in it are still the evidence: yaw was always fine, but
asserting pitch against the vehicle follow-camera winds the game's pitch all the way into its own
`-89` deg limit, showing the roof, and from there it is close to impossible to bring back down. It
sometimes recovers to the `-6.8` deg baseline on its own, and reliably recovers on exiting and
re-entering the vehicle or on glancing with L/R (Q/E). Reported again 2026-08-17 with the value
`-1.5532` rad, which is what identified the limit as the game's rather than ours.

**ROOT CAUSE, settled by a boundary trace: writing pitch EVERY FRAME is itself the bug.** Not the
clamp, not the anchor, not the entry value - the act of asserting the field 60 times a second while
the game's camera code also updates it. The trace, in a vehicle, mouse barely moving:

```
live=-0.1232 des=-0.1232 anc=-0.1232 haveAnc=1 hold=44 dy=+0.00 dx=+0.00
live=-0.3223 des=-0.1232 anc=-0.1232 haveAnc=1 hold=43 dy=+0.00 dx=-1.00
live=-0.1232 ... then -0.3387 -0.4415 -0.5546 -0.6639 -0.8006 -0.9742 -1.1034 (saturates)
```

`des` and `anc` never move - **every clamp in this file was working and none of them mattered.** Only
`live` diverges, alternating between our write and a value growing more negative each frame, and it
keeps growing on frames where `dy` AND `dx` are zero. That rules out input, and it rules out a spring:
a spring converges toward its target, this accelerates away from ours. It is positive feedback, our
write winding up the game's own camera integrator, with the alternating rows being the two writers
taking turns (we run at vblank, the game at its 30fps logic rate).

Every-frame re-assertion is *correct* on foot - the game undoes a one-shot write within 0.4s - and
destructive in a vehicle, where the game keeps a written value by itself. That asymmetry is the whole
story, and it had been measured earlier (a hand-written pitch survives untouched while stationary)
without the connection being made.

**FIX, and the shipped behaviour: in a vehicle, assert pitch once per GAME LOGIC FRAME**, tracked with
`FrameCounter`, instead of once per vblank. That removes the double-write the integrator was winding up
on, and unlike the intermediate attempt below it leaves no gaps. Confirmed in play; `pitchInVehicle` is
on by default as of this change.

**Known residual, accepted.** Forcing pitch hard down *through* the vehicle entry and then continuing to
force it down once seated can still provoke the runaway. Normal play does not do that - reported as
working "99% of the time" - so the feature ships on with this documented rather than held back.

An intermediate attempt, kept because it explains a symptom someone may reintroduce: asserting only on
frames where the mouse moved also stops the runaway, but trades it for CHOP, because the game reclaims
pitch in the skipped frames and the view stutters between the two values. Rate-matching is the answer,
not skipping.

**The proper fix would be to drive the game's own control input rather than the position**, exactly as
`aimResponseModel` does for aiming. `CCam+0x11c` was the standing candidate and is **ruled out**: it is
game-owned, writes to it decay within a frame or two (`+0.80` became `+0.14`, `-0.30` became `+1.10`),
and pitch does not track it - it just lurches. The real input is somewhere in mode 18's pitch writers
at `0x089a1xxx` on `$s0`, and finding it is a proper RE job, not a probe.

Earlier framing of this section, kept because the numbers are still useful but the conclusion was
wrong - it described the behaviour as a spring being pumped:

```
live=+0.53335  desired=+0.53335  delta=+0.00000    <- our write lands
live=-0.89684  desired=+0.53335  delta=-1.43020    <- the game corrects 1.43 rad in ONE frame
... alternates every frame while g_holdFrames > 0 ...
hold=0                                             <- we stop asserting
live=-1.55328                                      <- the spring OVERSHOOTS to -89 deg
live=-1.26423 / -0.85280 / -0.53299 / -0.37273 / -0.21595   <- then decays back to baseline
```

So `-1.5532` is not a limit, a wrap, or a clamp - it is the **overshoot peak of the game's own
spring** after we stop fighting it. It self-recovers, which is the "sometimes it goes back to -7 deg
on its own" report; moving the mouse again re-kicks it, which is why it feels permanently pinned.

Two mechanisms make it violent. The correction scales with how far we drag pitch from the target
(1.43 rad of correction at 0.7 rad of displacement, so roughly 2x). And we write at vblank (60Hz)
while the game corrects at its 30fps logic rate, so the two alternate and pump the spring rather
than settling.

**Fixed separately: an anchor ratchet.** The clamp anchor was re-captured at every stroke start, so
it followed its own output - each stroke ended at `anchor - 0.7`, the next anchored there, and the
window walked down until it hit the game's limit. The anchor is now captured once per CONTEXT
(`g_haveAnchorPitch`), which bounds pitch to entry +/- `kPitchRange` for as long as you stay in that
camera. Confirmed in the trace: the anchor held at `-0.12265` throughout.

**FIXED, and the cure is an ASYMMETRIC window: the entry angle is the ceiling.** In a vehicle pitch
may now travel numerically *below* the anchor by `pitchVehicleDown` (1.45 rad, about -89 deg of
look-up, effectively the game's full range) and **not one radian above it**. Confirmed good in play at
the full 1.45.

Sign convention, since it is the opposite of what the old notes implied: **more negative is looking
UP.** `-6.8` deg is level-ish, `-89` deg is the roof. So the useful direction in a vehicle is
numerically downward, and it is the numerically *upward* excursion that breaks things.

Two corrections fell out of getting this working, both worth keeping because both were wrong in the
same direction - assuming a measured number meant what it looked like:

- **Depth was a red herring.** Mean per-frame fight looked like it grew with depth (0.024 rad in the
  first 0.15 below the anchor, 0.26 at 0.5-0.75), which predicted shudder at 1.45 - it does not
  shudder. Those deep samples came from a trace where above-entry excursions were happening in the
  same session, so the "fight at depth" was mostly the spring recovering from being pumped by those.
  Depth is fine; direction is what matters.
- **The band is not the mechanism, the ceiling is.** An earlier version shipped a deliberately tiny
  0.15 band on that mistaken reasoning, which worked only by staying away from the fight and gave
  about -15 deg of look-up - far less than usable.

**The ceiling is only as good as the anchor, and the anchor arrives contaminated from on foot.**
Reported in play: entering a vehicle with `CameraPitch` at **+5 deg or more** brings the runaway back.

The chain: on foot the window is symmetric, so pitch can legitimately be left as high as `+0.65` rad
(`+37` deg). The game does **not** reclaim pitch when you get in - measured, a written value survives
untouched while the vehicle is stationary - so that on-foot value carries straight into the vehicle
context. The anchor is captured from it, the anchor *is* the vehicle ceiling, and the ceiling therefore
sits tens of degrees above the `-6.8` deg baseline: parked in the region that pumps the spring.

That is also why it looked like a regression between sessions with no code change. It depends entirely
on what pitch happened to be when you got in.

**Fix: in a vehicle the anchor is capped at level (0.0).** Capping at level rather than at the measured
`-0.1186` baseline avoids a hardcoded per-camera constant and costs nothing, because the wanted
direction in a vehicle is upward (more negative) - nothing useful lies above level.

`kMaxPlausiblePitch` (1.6 rad) also gates the capture, and pitch is not driven at all until a
believable anchor exists. That guard is a validity test on the anchor's *source*, not an absolute look
limit - absolute look limits were the original bug. **It did not fix this one**, and the reason is
worth keeping: `+5` deg is `0.087` rad, far inside `1.6`, so a plausibility check could never catch it.
The problem was never implausible values, it was plausible ones from the wrong camera.

Also checked and ruled out while chasing this: the fork hardcodes `CameraPitch`/`CameraYaw` to
`CCam[0]`, so a camera-slot switch on entering a vehicle would have made it write the wrong camera
entirely. Measured - `CCamera+0x50` (active cam index) reads **0** both on foot and in a vehicle, and
`cam[1]`/`cam[2]` sit at zero. `CCam[0]` is correct. (Incidentally the on-foot mode reads **4**, not
the 15 this document claims elsewhere.)

Because the ceiling is sufficient, **the spring's target field no longer needs finding.** If someone
ever wants pitch numerically ABOVE the entry angle in a vehicle, that is when it becomes necessary
again; the standing candidate is `CCam+0x11c`, which read `+0.12087` while resting pitch was
`-0.11861` - same magnitude, opposite sign. Note also that the spring does **not** act while the
vehicle is stationary (a hand-written `+0.55` survived untouched across two tool runs), so any future
probe of it needs the vehicle actually moving, which cannot be arranged by injecting buttons.

A methodology note, because it cost several rounds: the first diagnosis of the clamp bug was
*correct*, then wrongly retracted after checking snapshots that showed pitch only ever between
`-0.44` and `-0.03`. That data was unrepresentative - there was exactly one in-vehicle snapshot,
taken before mouse look existed. The live readout in the Camera tab settled it in one screenshot.
Prefer the live diagnostic over archived snapshots when asking "what range does this take".

### Ask the game which button does what, don't guess

`input.buttons.send` injects a PSP button over the WebSocket debugger, and the memory it moves
identifies it. Sniper zoom was found this way in about two minutes: press each button while
scoped, diff the `CCamera` block, see what changed.

```
square    CCamera+0x198  70.000 -> 33.068     (FOV: zoom IN)
          CCamera+0x7a0   1.000 ->  2.000     (zoom level)
cross     CCamera+0x198  31.719 -> 64.404     (zoom OUT)
select    CCamera+0x7b4   2.000 ->  1.000     (camera mode)
```

So **Square zooms in, Cross zooms out** while scoped, and `CCamera+0x7a0` is the zoom level.

The attempt before that bound zoom to **d-pad up/down**, reasoning from where GTA usually puts it.
That was the one pair that could not possibly work - the d-pad is the aim axis in free aim, which
had already been reported in play. **A prior from other games beat evidence already in hand**, which
is the same failure as trusting the "matrix yaw + PI/2" note over a measurement.

Use this for the remaining `?` rows in the button table below, and to check the ones marked
unverified. Take a baseline diff of the block first with nothing pressed - a few fields drift on
their own, and subtracting them is what makes a single press legible.

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

| PSP button | On foot | In a vehicle | In an aircraft |
|---|---|---|---|
| Cross | sprint | accelerate | **climb / throttle up** |
| Square | jump | brake / reverse | **descend** |
| Circle | attack / fire | drive-by fire | fire (Hunter) |
| Triangle | enter vehicle | exit vehicle | exit |
| R trigger | **aim** (lock-on) | handbrake | **yaw right** |
| L trigger | **switch to a nearby weapon drop** | glance modifier (L + stick direction) | **yaw left** |
| Nub | movement | steering (X only) | **pitch (Y) and roll (X)** |
| D-pad Up | **recruit gang member** (only with one targeted) | ? | special mission |
| D-pad Down | free aim (while aiming) | **horn** | centre view |
| D-pad Left | previous weapon | **previous radio station** | previous radio station |
| D-pad Right | next weapon | **next radio station** | next radio station |
| D-pad L/R *in a forklift* | — | **lower / raise the forks** | — |
| Select | change camera | change camera | change camera |
| Start | pause | pause | pause |

The aircraft column started as the game's own manual and WikiGTA's PSP controls pages, cross-checked
against the in-game Controls screen. **It has since been flown.** Every aircraft class the spawner
offers was taken up and the bindings behave as intended, so treat this column as verified at the
scheme level. Individual rows were not probed button-by-button, so if one specific control ever
feels wrong, `Tools/vcsvehicle.py probe --base cross` is still the way to pin it down.

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

### The gamepad is remapped, not passed through

A pad used to go straight past this layer. `HandleHostKey` answered only for a keyboard or a
mouse, so a pad reached the PSP's own layout through PPSSPP's mapper - and that, rather than any
individual binding being wrong, is what made playing with a pad feel like playing a handheld.
`kVCSPadMappings` is a second context-aware table beside the keyboard's, and `HandleHostAxis` is
the axis half, called from `NativeAxis` the way `HandleHostKey` is called from `NativeKey`.

The shape of it: triggers aim and fire on foot and are the pedals in a car, the right stick looks,
the bumpers glance in a vehicle and yaw in the air and zoom a scope, the d-pad carries weapons
across and stays the PSP's own vertically, the stick clicks carry the view controls that used to
be there, Start opens *this* menu while View opens the game's.

The vertical d-pad pair was view controls until it turned out that the game's menus - pause, map,
stats, save - read the PSP d-pad, so a pad could move sideways through a menu and not up or down.
The keyboard never showed it, because arrow keys are not in `kVCSKeyMappings` and reach the PSP
d-pad through PPSSPP's mapper; a claimed pad control has nowhere to fall through to. Needs no menu
detection, which is the point - see the `Menu` context note further down.

Five things are worth knowing before changing any of it.

**The face buttons did not move, and that is not laziness.** Xbox A/B/X/Y sit in the same four
places as Cross/Circle/Square/Triangle, and VCS already puts sprint, fire, jump and enter-vehicle
on them - and the whole melee set too. On foot and in a fight the modern layout and the handheld's
agree by position. Everything else differs.

**Every control is claimed in every context, mapped or not.** PPSSPP's XInput defaults put
`VIRTKEY_PAUSE` on the left trigger, `VIRTKEY_FASTFORWARD` on the right one and
`VIRTKEY_SPEED_TOGGLE` on the right stick click. An unclaimed control here does not fall through
harmlessly - it opens a menu or triples the game speed in the middle of a firefight. The `psp = 0`
rows are what prevent that, and they are the same inverse-Escape-trap discipline the keyboard
table already documents.

**The `Menu` context was the exception, and "every context" is not a slogan - it is that bug.**
The pad table had rows for on-foot, vehicle, aircraft and aiming, and none at all for the game's
own pages. So in the map, the briefs and the stats every pad control fell through: a finger
resting on the left trigger opened PPSSPP's own pause screen over the game's menu, the right one
fast-forwarded. The tab lock could not reach a pad either, because it works by stripping
directions out of the mask and a direction that was never claimed is not in the mask - which is
why the keyboard's arrows behaved on those pages and a pad's d-pad wandered off the hidden tab
strip. Both fixed by giving `Menu` a full row set like every other context.

**And the nub navigates this front end, which is worth knowing before synthesising anything.**
`ApplyAnalog`'s switch had a case per gameplay context and fell through for `Menu`, so the stick
was claimed by `HandleHostAxis` and then spent on nothing: dead in every one of the game's pages,
on the device most players reach for first. Measured over the debugger before choosing a fix -
`input.analog.send` with the pause menu up steps the page exactly as a d-pad press does, ONE step
per deflection, the game doing its own edge detection (page 0 → 5 on a 1.2s hold, same as one
press of down). So the stick is passed straight to the nub rather than synthesised into d-pad
steps, and it asks the tab lock the same question the buttons do.

**The pad needs a held-set of its own.** An arrow key and a d-pad direction are the *same*
`InputKeyCode`, so a single set would have the debug spawner's arrow-key rows firing whenever
somebody pressed a direction on a pad. The two devices genuinely cannot share that state.

**A question asked of one device is a bug waiting for the other.** Vaulting asked
`IsHostKeyDown(kVCSJumpKey)` and the fire hook asked `IsHostKeyDown(kVCSAimKey)`, and both were
right for as long as a keyboard was the only thing that could answer. A pad turned them into
silent failures: it jumped but never climbed, and a scoped shot followed the ped's aim while the
camera moved - the "looks correct and misses" failure this file already warns about, arriving by a
new road. Both are named questions now, `JumpHeld` and `CameraDrivenAimHeld`, and the rule is: if
more than one device can express an intent, name the intent and ask *that*, never a key.

The second one is worth reading rather than copying, because "either device" would have been the
wrong fix. The hook redirects the shot along the CAMERA ray, which is correct when the camera is
the aim and actively wrong under lock-on, where the game has chosen a target the camera need not
be pointing at. So the question is not whether aim is held but whether this fork is steering it -
identical for the mouse, which always free aims, and different for the pad, which does not.

**The pad aims by lock-on, always - and that is `LockOnModeActive`, not a mechanism of its own.**
Holding a trigger on a pad is a request for the game's assist rather than for a crosshair; a stick
is bad at placing one, which is why every console shooter since has offered help. So the
auto-free-aim pulse that fires for the mouse deliberately does not fire for the pad. Expressing it
as the *existing* lock-on mode means everything already written against that toggle applies
without being taught that a pad exists, and there is exactly one place to look the day the two
need to differ. A scoped weapon is the exception, and it is what the phrase means rather than a
hole in it: the sniper and the RPG have no lock-on to be pinned to, so asserting one would leave
the right stick dead with a crosshair on screen and nothing able to move it. Same shape as the
`MeleeEquipped` rule in `FreeAimActive` - an automatic, weapon-driven exception standing beside
the manual toggle.

**The right stick becomes mouse movement at the edge.** `ApplyPadLook` pushes a delta into the
same accumulator `HandleMouseDelta` fills, so the sensitivity, the FOV scale, the free-aim solver
and the vehicle pitch rules are all the code that was measured against a mouse rather than a
stick-shaped copy of it. It has to run *before* `ApplyAnalog` in `VCSGame::Tick` - that is where
free aim drains the accumulator, and filling it afterwards makes the stick a frame late in every
frame, which reads as lag rather than as nothing happening.

Sharing that accumulator cost one gate its meaning, and the fix is worth knowing about.
`CameraTick` used to return early on `g_settings.enabled` - the *mouse* flag - which was the same
question as "is anything driving the camera" while the mouse was the only thing that could. It now
asks whether **either** device is enabled, because a player on a pad has every reason to turn
Mouse control off and would otherwise have found the right stick silently dead. Nothing leaks
through: each device gates its own contribution at the entry point, so with both off nothing fills
the accumulator in the first place.

**Being told about input and being allowed to act on it are different questions.** Both entry
points sit behind `PassInputToMapper()`, which closes whenever a screen is on top - so while our
own pause menu is up, the layer is told nothing. Hold the aim trigger, press Start, release it in
the menu, and the release is the event that never arrives: the layer goes on believing aim is
held and the Aiming context latches for the rest of the session. It surfaced as "Enter stopped
confirming in the game's menus", because Aiming had no row for Enter and it fell through to
PPSSPP's mapper, which binds it to Select - while Shift went on working, since Cross is the heavy
hit there. A control that half-works is how a latched context announces itself.

The two paths need different answers, and the reason is in the shape of the data. `HandleHostAxis`
is now called regardless of the gate, which it can afford because an axis carries its whole state
in every event - a late one simply corrects the record. A key cannot: its release is a single
event and a dropped one is gone forever, so `VCSMenuScreen`'s constructor calls `ResetHostKeys()`
instead and nothing survives across the menu at all.

**Two thresholds on the triggers, not one.** A trigger resting against a single line - which is
where a finger holds one - crosses it on noise alone, and the button underneath is Fire. Press at
0.45, release at 0.35.

`VCSPadMapping` carries one column the keyboard's does not: `scopedOnly`. The zoom bumpers are the
game's Square and Cross, which with anything but a scope in hand are Block and Heavy Hit, so
ungated they would have a bumper throwing punches on every press. It is the only row-level gate on
either table, and it is why the pad table is its own struct rather than a second `VCSKeyMapping`.

**If a stick ever feels inverted, check the axis sign first.** XInput reports `JOYSTICK_AXIS_Y`
positive when the stick is pushed away from the player, and the PSP's nub is also positive away
from the camera, so `HandleHostAxis` negates nothing. That is specific to XInput - PPSSPP's
*generic* pad defaults invert that axis, because an SDL joystick reports the opposite sign - so a
non-XInput pad is the case to suspect. The look stick is negated exactly once, in `ApplyPadLook`,
because a mouse's positive dy is down the screen and everything downstream expects a mouse.

### The items you look THROUGH: binoculars and the photo camera

Two weapons in VCS are not weapons at all in the way the aim model cares about. Hold aim and the
game raises them into their own first-person camera, aimed by turning the view and zoomed with the
sniper's two buttons. They are one predicate, `WeaponTypeIsOpticalItem`, and `ScopedWeaponActive`
puts them beside the sniper and the RPG.

| | binoculars | photo camera |
|---|---|---|
| weapon id (slot 9 for both) | 39 | **38** |
| `CamMode` / `WeaponCamMode` when RAISED | 47 | **46** |
| `WeaponCamMode` when merely selected | 0 | **0** |
| zoom | Square in, Cross out | same |
| `CCam+0x130` / `+0x124` while raised | 0.00000 | **0.000000** |

Every row of that table is why the code looks the way it does:

- **Keyed on the weapon ID, never the camera mode.** `WeaponCamMode` reads 0 until the item is
  actually up, and the free-aim entry gate fires on the frame the aim key goes down - before any
  camera has changed. A mode test would arm the sequence first and learn better afterwards.
- **The aim leads stand down.** Those two `CCam` fields are the game's own smoothed aim
  increments, and they sit at exactly zero while either item is raised - against ~0.0995 on the
  follow camera moments earlier. The yaw kick and the pitch deadband exist to break stiction in
  front of that smoother; fed to a camera that follows the instant it is written, they are a
  twitch. This is the same failure the sniper and RPG already had recorded.
- **The free-aim entry sequence stands down too**, for the reason melee does. Its sprint latch
  holds Cross for ~8 ticks and Cross while glassing is ZOOM OUT, so aiming while walking forward
  zoomed the view back out every time.

The camera was found by being told "it's the same kind of glitchiness as the binoculars" and then
measuring the same three things: id, the two modes, and the increments. The id came from the
script - `request_model #CAMERA` on the line before `give_weapon_to_char $PLAYER_CHAR weapon 38`
(`METALDE_1936`), the same class of evidence as `H_BINO1` for the binoculars - and the zoom pair
was measured live, Square taking the FOV from 70.00 to 59.25 and Cross putting it back. **If a
third item ever turns up that "aims wrong", measure those three before touching the aim model.**

### Recruiting is a binding with a precondition, and the precondition is the work

`Recruit gang member` is `G` on the keyboard and d-pad up on the pad, in the **Aiming** context on
both. The binding is one row per table. What is worth writing down is the half a row cannot say.

The game's own line is "target them and use the up button", and *targeted* is the whole of it: the
press means nothing in free aim, because free aim is the state with no target. That is fine on a
pad, which aims by lock-on always (`LockOnModeActive`), and it is exactly wrong on a mouse, where
holding aim fires the auto-free-aim pulse and leaves the crosshair pointing at a henchman the game
is no longer tracking.

So `RecruitHeld()` joins `LockOnModeActive()` and `MeleeEquipped()` in the gate that arms that
pulse. Holding G *before* the aim control is what buys the lock-on, and since G is also the press,
the whole thing is one gesture rather than a ritual with the lock-on toggle in the middle of it.

Two things this is not:

- **Not a new mechanism.** The gate already had two terms asking the same question - is this player
  asking for the game's assist rather than for a crosshair - and this is a third answer to it,
  which is why it goes in the same `if` rather than anywhere near the recruit rows.
- **Not a reason to bind `G` on foot.** There is no target outside the Aiming context either, so a
  row there would be a control that does nothing. Nothing in PPSSPP's defaults claims `G`, so
  holding it while walking costs nothing and reaches the held-key set regardless of whether any
  context maps it.

The pad's d-pad up was a `psp = 0` suppression before this, on the reasoning that up would change
the camera mid-fight. It would not - on foot the PSP's up is recruit and nothing else - so the row
was suppressing the one thing the button is for. That is the general shape of the mistake: a
control that "does nothing" in the tester is a control whose precondition you have not met.

### Hand-to-hand combat is four buttons and five states

Melee looks like it needs a mode of its own and does not. **The game detects the state** — targeting,
holding someone from the front, holding them from the rear, standing over a prone body, standing
over a dead one — and reinterprets the same four face buttons in each. So the whole fighting system
is four bindings, and this fork needs no melee detection to express it:

| PSP button | targeting | held, front | held, rear | prone | dead |
|---|---|---|---|---|---|
| Circle | light hit, 4-move combo | jab/punch, 2-move | jab | floor punches | stomp |
| Cross | heavy hit, 2-move combo | heavy, tap = brief K.O. | knee in the back | stomp / ground kick | — |
| Triangle | grab (front or rear) | throw | neckbreak | pull up | — |
| Square | block | — | — | — | — |

**The keys follow one rule: a key keeps its PSP button in every context.** Shift is Cross (sprint on
foot, heavy hit while targeting), Space is Square (jump, block), F is Triangle (enter vehicle, grab),
left mouse is Circle everywhere. Nothing new had to be found a home for, because sprint, jump and
enter-vehicle are all unreachable while targeting — the same sharing the PSP itself does.

| key | PSP | while targeting |
|---|---|---|
| Left mouse | Circle | light hit — already bound as "Fire" |
| Left Shift | Cross | heavy hit |
| Space | Square | block |
| F | Triangle | grab |
| Q / E | d-pad L/R | previous / next target — already bound |

**A melee `VCSInputContext` was considered and rejected.** It would mean duplicating every `psp = 0`
claim row, and teaching `ApplyAnalog`, `ContextWantsMouse` and `FreeAimActive` about a fourth
on-foot context — for bindings that are identical to the aiming ones anyway. Add one only if melee
ever needs a key that means something *different* from what it means while aiming.

**The automatic free-aim entry had to learn about melee, and this is the part that mattered.**
`FreeAimActive` has always checked `WeaponSlotIsMelee`, but the *entry sequence* in `ApplyMapping`
only checked the manual `L` toggle — so with fists:

- the sprint latch held **`CTRL_CROSS` for ~8 ticks**, and Cross while targeting is the heavy hit. So
  entering a fight while walking forward threw **an unrequested heavy punch, every single time**.
- the pulse pressed **d-pad Down**, the game's Free Aim button, which melee has no use for.

`MeleeEquipped()` now gates both, and `FreeAimActive` shares it. Neither could have been noticed
before this change, because Cross had no melee meaning to misfire — **the bindings didn't cause the
bug, they made an existing one visible.** Worth remembering when a new binding "breaks" something:
the input may have been going out all along.

**`L` is therefore a manual override now, not the ritual it was.** Fists need no flipping before a
fight; reach for it only when the weapon read is wrong, or a gun should be aimed under lock-on on
purpose.

**Lock-on mode also stands the mouse down from the camera** (`ContextDrivesCamera`, 2026-08-19).
It already refused the pulse and refused the analog stick, but yaw and pitch were still written
from the mouse while it was on — and under lock-on the game is steering the camera around the
target it picked, so the two fought every frame and the view juddered between them. While the
toggle is on and aim is held, the mouse now does nothing: WASD moves, the game frames the target.
Outside the `Aiming` context mouse look is untouched, so the toggle can be left on between fights
without losing the camera on foot. The delta is still *claimed* and still drained in `CameraTick`
— dropping the claim would hand it to PPSSPP's mouse-to-analog path, which is strafing here.

**Space was falling through to PPSSPP's `CTRL_START` — the inverse Escape trap, third instance.**
The `Aiming` context never claimed it, and PPSSPP's default keyboard map binds Space to Start
(`Start = 1-62` in `memstick/PSP/SYSTEM/controls.ini`), so pressing it with aim held opened the pause
menu. The old sniper-zoom comment read this as "Square is still Jump when unscoped" and was wrong on
both counts — the key never reached Square in that context at all. Binding it fixes block *and*
makes the obvious zoom key work while scoped.

**Not yet confirmed in play**, and these are the three to look at first: Square and Cross while
aiming an *unscoped* gun (both expected inert — Space and Shift now send them there), and whether
holding Square blocks rather than tapping it. `Z`/`Y` and the mouse wheel also still map to
Square/Cross for sniper zoom, so they double as block and heavy hit during a fistfight; harmless,
and a static table cannot gate them on `ScopedWeaponActive`.

### One in-vehicle context was never enough — aircraft cannot fly with the car bindings

`InVehicle` assumed a car for as long as it existed, and for a car it is right. For a helicopter
it is not merely tuned wrong, it is **structurally unable to fly**, and the reason is worth
stating precisely because it is invisible from the binding table:

- In a vehicle, W/S are *buttons* (Cross/Square) and A/D are the stick's *X axis*. **Nothing
  anywhere drives the stick's Y axis.**
- In an aircraft the stick's Y axis is **pitch**, and pitch is the only thing that converts the
  rotor's lift into forward flight. Cross alone just goes straight up.

So the old bindings gave a helicopter that would take off, hover, and refuse to go anywhere —
which is exactly the complaint the GameFAQs question "I can't make it move forward" describes,
except here it was our mapping rather than the player.

Two smaller collisions came with it: Q/E in a vehicle are *glances* (L trigger + a stick
direction), and in an aircraft L is **yaw left** — so glancing yawed the aircraft and rolled it
at the same time. And Space is handbrake, i.e. R trigger, which in an aircraft is **yaw right**,
so reaching for a brake that doesn't exist spun the nose.

`VCSInputContext::InAircraft` fixes all three. It is selected from `VehicleModel`
(`PlayerVehicle + 0x56`) through `VehicleClassForModel`, and the layout is GTA San Andreas's:
W/S climb and descend, Q/E yaw, arrow keys pitch and roll (A/D also roll). A vehicle whose model
can't be read falls back to the car set, which is the pre-existing behaviour and never worse.

**Boats and the forklift deliberately stay on the car bindings.** A boat accelerates, reverses
and steers exactly like a car, so the existing set covers it - the only dead key is Space, since
boats have no handbrake.

### Spawning vehicles

Testing the aircraft and boat bindings needs an aircraft or a boat, and hunting one down in the
world is slow. VCS ships its own spawner and never runs it: `DBGCARS`, a debug script with the full
`request_model` / `has_model_loaded` / `create_car` sequence, reachable only from a debug menu that
retail never opens. Two ISOs in the project root turn it on with same-size byte patches - no
recompile, and the retail ISO is never touched:

**`VCS-full-spawner.iso` (25 bytes, current) - every vehicle, 170..280.** This is the one to use.

1. `MAIN`'s `launch_mission @WARPSPO` repointed to `@DBGCARS`. WARPSPO is a dev warp-spot script
   retail launches unconditionally, so it is the free donor slot.
2. The model filter **neutralised rather than inverted**: `DBGCARS` consults `Noname_3_30` (15 ids)
   and `Noname_3_13` (1 id) as *exclude* lists, skipping any model they match. Setting all 16 ids to
   `0` leaves the branch polarity stock and makes the lists match nothing, because the model under
   test only ever holds 170..280. Nothing is skipped, so the cycle offers the whole range.
3. The wrap bounds raised `279` -> `280` (the up-cycle compare and the down-cycle assign) so the
   last model is reachable - even the stock debug script could not reach it.

**`VCS-spawner.iso` (25 bytes, superseded) - 15 hand-picked non-car classes.** Same launch repoint,
but it *inverts* the two land-branch `goto` pairs so the cycle offers only what the list matches,
and rewrites the 15 ids from watercraft to the special classes. Kept because it is what proved out
every non-car control scheme; the full spawner covers it.

**Controls (as of 2026-08-17): `F9` spawns, arrow Left / Right scroll.** All three are OnFoot rows,
none is a chord, so no modifier is held. `V` (Select) still toggles traffic and pedestrians.

The one wart, and it is the script's fault rather than the binding's: **`DBGCARS` sets its spawn
state (`8@ = 1`) at the end of *both* cycle paths, so advancing the selection *is* the spawn.** There
is no "spawn whatever is currently selected" entry point to bind, so `F9` is necessarily "next model,
and spawn it" - the same thing arrow Right does. Making `F9` re-spawn the current model without
advancing would need a real script edit: a third path that jumps to the `create_car` block with `7@`
left alone. Worth doing only if the duplication actually gets in the way.

Earlier versions used a chord (hold Tab for the L trigger, tap Q / E). That is gone - the request was
for the trigger to be a single key with the arrows free-standing.

Five facts worth keeping if you ever patch the script again:

- **Never blind-scan for an immediate and patch every hit.** Raising the wrap bound `279` -> `280`
  by scanning the `DBGCARS` region for the 16-bit value found *four* matches; only two were the
  bounds. The other two were the byte pair `17 01` occurring inside unrelated operands, and writing
  them corrupted two values that had nothing to do with the cycle. Caught only by counting: the
  decompiled listing showed the value on two lines, not four. **Patch a hit only after confirming it
  decompiles to the instruction you meant**, and if the counts disagree, revert the extras.
- **Verify by decompiling the patched script, not by trusting the write.** Every claim about
  `VCS-full-spawner.iso` above (launch target, all 16 filter ids, both wrap bounds, branch polarity)
  was read back out of a fresh Sanny decompile of the patched file. This is what caught the above.
- **Sanny's CLI is single-instance and fails silently.** If a `sanny.exe` is already running, a
  second `sanny --no-splash -m vcs_psp -d <file>` invocation **exits 0 and writes no `.txt` at all**.
  Kill any stale instance first, and poll for the output file rather than trusting the exit code.
- **Script pointers are `file_offset - 8`** (an 8-byte header precedes script space). Getting this
  wrong is silent and fatal: an earlier patch off by 8 sent the interpreter into a misaligned
  opcode, the dispatcher fetched a garbage handler, and the game died with
  `Invalid exec address`. Verify any pointer you write by checking that `target + 8` lands on a
  plausible opcode.
- **Sanny can decompile but not faithfully recompile.** A round-trip of the untouched script
  compiles fine and produces a *different* 1.8 MB file - one segment shrinks 236 bytes, everything
  after shifts, and Sanny appends a `__SBFTR` footer. Patch bytes in place instead.
- **`MAIN.SCM` sits contiguously at ISO offset `0x32aa0000`**, so a script byte at file offset N is
  at `0x32aa0000 + N` in the ISO. No extraction or rebuild needed to patch it.

### The debug menu also works - but is deliberately not shipped

**It lives only in `VCS-debugmenu.iso`, and no other ISO enables it.** It was turned on to find out
whether it could be, which it could; in play it was unwanted, because the open combo is reachable by
accident from ordinary keys. Nothing needed reverting - the gate is a single byte and the spawner ISOs
are built from a pristine copy, so they carry the stock value. If you ever wonder which is which:
`0x32aa7a5a` reads `0xc4` for stock and `0x56` for menu-on.

`VCS-debugmenu.iso` (project root, **one** byte different from retail) opens the shipped debug menu.
The gate in `MAIN`'s init is `83FD not unknown_check_command_92ea` / `0022 goto_if_false`; the patch
retargets that branch to the instruction right after it, at file `0x7a5e`, so `$2 = 1` runs whatever
the check returns. The byte is at file `0x7a5a` (ISO `0x32aa7a5a`), `0xc4` -> `0x56`.

**Open it with d-pad Down + Circle + R trigger, then release** - `METALDE_4119` tests those three
(script indices 9, 17, 6) with `$4164 == 0`, and `METALDE_4150` waits for release before opening.
Confirmed by injecting the three buttons and watching `$4165` go to 1, with `$4152 == -1`,
`$4136 == $4137 == 1`, `$4160 == -1` and `$4167 == 1` proving METALDE and DEBMENU both started.
All of those read 0 on a normal boot.

On the keyboard that combo is **only reachable from a vehicle**, where the fork binds all three:
H (`CTRL_DOWN`), Space (`CTRL_RTRIGGER`), Mouse1 (`CTRL_CIRCLE`). On foot, `CTRL_DOWN` and
`CTRL_RTRIGGER` are unbound, so either sit in a car first or add two OnFoot rows.

The menu contents: level skip, weather and time changer, MoCap menu, USJ editor, player
coordinates, marketing camera, player cheats, character viewer, empire status, audio debug, launch
jetski mission, complete all story missions, unlock end-game viewer.

Worth noticing what draws it: `DEBMENU_321` builds the panel with `set_empire_hud_visibility`,
`set_empire_hud_colour 0 rgba 50 50 50 128`, `set_empire_hud_size 0 width 170 height 150` and
`set_empire_hud_position 0 to 5 5` - the empire-HUD commands from `scm/VCSSCM.INI`. Those opcodes
are how R* Leeds drew their own debug UI.

### Script button numbering

The script's button numbering is its own, not PSP bit order. `007F is_button_pressed` dispatches
through a 20-entry jump table at `0x08b79928`; indices 8-11 read `CameraInputMode` and then
`CPad + 0x12/0x14/0x16/0x18`, i.e. this table's four `PadDPad*` entries. Confirmed live by holding
each key and watching which `CPad` halfword goes to 255:

| script index | 4 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| button | L | up | down | left | right | Start | Select | Square | Triangle | Cross | Circle |
| `CPad +` | 0x0a | 0x12 | 0x14 | 0x16 | 0x18 | 0x22 | 0x24 | 0x26 | 0x28 | 0x2a | 0x2c |

**The forklift needs no new bindings either, and that is a finding rather than an assumption.**
Confirmed in play: `R` and `T` raise and lower the forks. Those are already bound - `CTRL_RIGHT`
and `CTRL_LEFT` - so the game **reuses the radio buttons for the forks** when you are in one, and
the keys that were mapped for changing station happen to land on exactly the right control.

(As of 2026-08-17 the two keys are swapped by request - `T` is now `CTRL_RIGHT` and `R` is
`CTRL_LEFT`, in both the InVehicle and InAircraft contexts. That inverts which key raises and which
lowers the forks along with the radio, since it is the same pair of PSP buttons.)

Two things follow. There is no radio switching in a forklift, so the `"Next / Previous radio
station (verified)"` descriptions on those two rows are wrong *for this one vehicle* - the table
has no way to say so, since a per-vehicle description would need a per-vehicle context, and one
cosmetic string does not justify one. And it is worth remembering that **a vehicle can repurpose
a button rather than merely ignore it**: the assumption that the car scheme is a superset which
special vehicles subtract from is wrong, and the forks are the proof.

### Measuring what a button does, without being able to see the screen

`Tools/vcsvehicle.py` is the automated form of the Input tab's button tester. It holds one PSP
button over the WebSocket debugger, watches the *vehicle's own transform* — matrix at `+0x00`,
world position at `+0x30`, same layout as the player ped — and reports the displacement and the
rotation of each basis row. "Cross climbed 4.2 units" is a better answer than "it felt like it
went up", and it needs no screenshot: `gpu.buffer.screenshot` fails with "Could not download
output" on this setup, so memory is the only channel.

Two things it learned the hard way, both encoded in the tool now:

- **Momentum bleeds between measurements.** The first run reported that Square moved the car
  *forwards* 12 units. It was the coast from the previous Cross. Every probe now waits for the
  vehicle to stop drifting first, and prints the drift it gave up at.
- **A stationary vehicle answers nothing.** Steering does nothing at rest, and an aircraft
  ignores pitch and roll until the rotor is spinning. `--base cross` holds throttle through the
  whole probe, which is the only way those rows mean anything.

The tool can also drive the player: injected analog + a Triangle press every couple of seconds
found and entered a car with no human involved, which is how the model-id offset was found. What
it cannot do is *navigate* — reaching a helipad or the docks blind is not realistic, so the
special vehicles still need someone to fly there.

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
- **The ImGui debugger swallows the keyboard when focused, and it swallows our menu's too.**
  `WantCaptureKeyboard` strips `InputMode::Keyboard` before `passKeyThrough`, so click the game
  area, not the VCS window, when testing input by hand. The part that wastes an afternoon is the
  *second* place it filters: `NativeFrame` drops queued key events on the same flag, but keeps UP
  events "to avoid stuck keys" (`UI/NativeApp.cpp`, the `filterKey` branch). A view then sees the
  release of a key whose press it never saw, and since `Clickable::Key` sets `down_` on the press
  and only clicks on the release, **every row in the VCS menu silently stops responding to Enter
  while the debugger has focus** - which looks exactly like a broken menu rather than like
  captured input. Arrow keys still work throughout, because focus movement is driven from
  `KeyEventToFocusMoves` inside `NativeKey`, upstream of the queue. Rows that highlight but will
  not activate mean ImGui has the keyboard, nothing more.

**Deliberately not implemented yet:**

- **`GameState` is the last one worth hunting.** `WeaponIndex` is **done** (`PlayerBase + 0x789`, a
  u8 *slot* 0..9 rather than a weapon id - the type lives at `PlayerBase + 0x574 + slot*28 + 4`).
  `PlayerOnFoot` is derived rather than read, and `AimYaw`/`AimPitch` are unset *permanently* —
  see "There is no stored aim direction". See [docs/VCS_ADDRESSES.md](docs/VCS_ADDRESSES.md).
- **Direct weapon selection.** `WeaponIndex` now exists, so this is unblocked; it still probably
  wants a memory write rather than a button press, since the PSP only exposes
  cycle-next/cycle-previous.
- **Vertical look while driving.** ON by default now (`pitchInVehicle`), asserted once per game logic
  frame. One residual: forcing pitch down through the entry and onward can still run away. See the
  pitch section above.
  Unresolved, not abandoned. `InAircraft` is gated by the same setting, for the same reason.
- **Boat-specific bindings.** Boats run the car set on purpose - see the aircraft section - but
  nobody has held each button in one, so "Space does nothing" is inference rather than a
  measurement.

- **SOLVED: the `Menu` context triggers now.** It is gated on `MenuActive`, the front end's own
  flag - see "The Menu context finally triggers" above. The account below is kept because the dead
  end in it is still worth knowing about, and because the *reason* it was a dead end is the lesson.

  It needed `GameState`, which was hunted for and NOT found: a 192KB sweep of the globals gave 1249
  candidates from two rounds, and a proper correlation run over the most promising cluster (around
  `IsFreeAiming`, where the boolean-shaped ones landed) ruled it out. What was never asked is
  whether the context needed a general game-state enum at all. It did not - it needed one bit.

  **Do not "solve" this by watching `FrameCounter` stall.** It was tried, shipped, and broke the
  controls. The game's logic also stops during loading screens, cutscenes and frame-rate hitches,
  so "logic stopped" is not a synonym for "menu open" - it is a superset that catches the game
  mid-play. Reverted in `03c1f3cfe0`.

  The idea was seductive because every script written to hunt `GameState` used exactly that signal
  to label its own samples, and it worked perfectly *there* - a capture only has to be right on
  average, whereas a control scheme has to be right on every frame. **Elegance was doing the
  persuading, and elegance is not evidence.** The caveat was even written into the commit message
  as "harmless" rather than treated as the reason not to do it.

  Working around it is cheap anyway: Enter and Backspace are bound in the OnFoot context alongside
  Shift and left-click, so menus have working keys without any detection at all.
- **Most bindings are unverified guesses.** Only the ones marked "verified" in
  `kVCSKeyMappings` were checked against the real game. Aim sat on the wrong trigger (L instead
  of R) for a long time and silently did nothing - assume crouch, horn, radio and camera are
  wrong until someone holds them and looks.
- **PAL / JP builds.** Different builds, different addresses; would need a second table.

### Vaulting — pulling up onto a ledge, and calling game code to find one

**Status: working in play, with the game's own animation on very nearly every vault.** On by
default (`VaultSettings().enabled`, or the Vault tab in the debugger window). Confirmed 2026-08-19
against a head-height wall: the probe armed, the jump key vaulted instead of jumping, and the
player ended up standing on top. The animation came later, in two steps - asking the game to climb
(2026-08-19), then forcing it when the game's own search declines (2026-08-21). Both are below.

The motion, recorded from memory at 20Hz as it happened:

```
20.36s   z 11.121   start (footing 10.068 + the ped origin's 1.040)
20.70s   z 13.223   rise done, 0.55m forward - the 25% share
21.03s   z 13.116   stepped on and settled
21.36s   z 13.116   standing, not falling, not ejected
```

`13.116` is exactly `ledge 12.076 + origin 1.040`, so the offset cancellation is right to the
millimetre, and the whole move takes 0.67s. **The game accepts a written position on top of a
wall** - no collision rejection, no sinking, no snap-back - which was the real unknown in the
fallback path.

**Measured constants, since they anchor every setting here.** World units are metres: a waist wall
reads **+0.908** above the player's footing, a head-height wall **+2.008**. The ped origin sits
**1.040** above the surface it stands on - the number this design deliberately avoids needing, now
known anyway. The default band (**1.50 .. 3.03**) therefore excludes the waist wall by design and
catches the head-height one, with room above it for a tall fence.

The design decisions, in the order they were made:

**It lives in the fork, not in `MAIN.SCM`.** The script VM has no raycast and runs at script
granularity; everything this needs — per-frame probing, a state machine, memory writes — the fork
already does.

**The trigger is the jump key, contextually.** Space vaults when a ledge is in front of the player
and jumps when there isn't, so nothing is taken away and there is no new key. `VaultTick` runs
*before* `ApplyMapping` for exactly this reason: it consumes the press on the tick it happens, and
`ApplyMapping` then sends the game nothing at all for the duration. Reversed, every vault would
begin with a hop.

**Height is measured against the player's own footing, never against the ped's position.** Nobody
knows where in the character the entity origin sits — GTA peds carry it somewhere around the hips —
so the first of the five probe lines is fired straight down *through the player*, and every height
is a difference against what it hits. The unknown offset is in both terms and cancels. This is also
what makes the landing height work at the far end: the ped is placed at `ledge + (its own height
above the surface it was standing on)`.

**The probe's start height is the climbing ceiling, for free.** `FindGroundZFor3DCoord` only
reports surfaces *below* the point you ask from, so a wall taller than `probeCeiling` above the
footing simply does not register — no check, no arbitrary rejection rule.

**Eight lines, once every one or two frames:** one through the player (footing), six at the wall
(near to far, nearest match wins), one past it (somewhere to go). Six at the wall rather than three
because **a vertical line only reports what it is dropped through**, and a fence rail a hand's
breadth deep falls between lines spaced 0.40 apart far more often than it falls on one - and a
fence that is missed reads as *nothing in front of the player*, not as a fence that was refused.
Six across the same reach puts them 0.16 apart. That is the whole of the "narrow models" fix; the
rest of the probe never cared how wide anything was.

**The last line picks between two moves rather than only permitting one.** A far side level with
the ledge is a surface to stand on - a pull-up **onto** it, which is the case the game can animate.
A far side *below* the ledge but no more than `landingDrop` (1.50) below the player's own footing is
a fence: go **over**, clear the top, and come down on the other side. Deeper than that is a parapet
with the street underneath, and higher than the ledge is a second wall with no room between them;
neither arms. A going-over vault never asks the game to climb, whatever `preferNative` says - its
climb-out finishes standing on top of what it found, and on top of a fence is a rail.

**How the query works at all** is the genuinely new capability here, and it is written up in
"Asking the world a question" in [docs/VCS_ADDRESSES.md](docs/VCS_ADDRESSES.md): a ~40 instruction
MIPS program in a `userMemory` block, run on the game's own thread through `hleEnqueueCall`,
because the function takes its arguments in `$f12..$f14` and that call path fills `$a0..$a3` only.
It is asynchronous — ask on one tick, read on the next — which a ledge probe can afford.

**The one hard rule, learned by crashing the game (2026-08-19):** a call may only be enqueued from
inside a syscall, from one that neither blocks nor reschedules, and **not from an interrupt
handler**. Enqueuing from `VCS::Tick` — a vblank timing event, not a syscall — planted the call on
an unrelated thread and produced `Corrupt stack on HLE mips call return: 28fefefe`, a stack-fill
pattern with a marker half written over it. `RequestGroundZ` now only *prepares* a question;
`WorldQueryDispatch` sends it.

**The host that works is `sceGeListEnQueue`**, and finding it took three wrong guesses that are
worth keeping, because each was wrong for a different reason:

| candidate | why not |
|---|---|
| `sceKernelGetSystemTimeLow`, `sceKernelLibcClock`, both dcache calls | call `hleReSchedule` |
| `sceCtrlReadBufferPositive` | blocks waiting for the next pad sample |
| `sceDisplaySetFrameBuf`, `sceDisplayIsVblank`, `sceKernelPowerTick` | **VCS calls them from inside its vblank interrupt handler** |

That last row is the interesting one and it was measured, not guessed: with a refusal reason
written into the block, the count climbed with `refusedWhy = "intr"` and `refusedThread = 0x110`,
which is `idle0` — i.e. an interrupt borrowing whatever context was current. VCS imports
`sceKernelRegisterSubIntrHandler`, and the thread list has no render thread, so the flip had to be
happening somewhere other than `threadmain` (`0x116`, identified by the pad read). **An interrupt
is not a thread**, and a call planted on one lands on a stack the game never used — which is very
likely what the original crash actually was, rather than merely "not a syscall".

The display-list submit is the right host for the same reason the flip looked like one: every game
does it once a frame from its own main loop. It just does it from the loop rather than from the
interrupt.

#### The animation: the game climbs it, and this only asks

**Confirmed in play, 2026-08-19: the player pulls up with the game's own animation.** Two of the
game's functions do all of it - `CanClimb(ped, &result)` searches for something to climb and
`StartClimb(ped, &result)` plays the pull-up and moves the ped - and `VCSWorld` runs both in one
dispatch when the jump key fires. See "The climb-out is two calls" in
[docs/VCS_ADDRESSES.md](docs/VCS_ADDRESSES.md) for how they were found and what the struct holds.

So there are two motions now, and the fallback is not dead code: the game's own search can decline
a wall this fork's probe was happy with, and the written-position curve is what runs when it does.
The Vault tab counts them separately for exactly that reason - a native count stuck at zero while
attempts climb means the game is refusing every ledge, which is a different problem from the vault
not triggering.

#### Forcing it, when the game's own search says no

**Status: working in play, ON by default (2026-08-21).** `VaultSettings().forceNativeClimb`, or the
last checkbox in the Vault tab. **The animation now runs on very nearly every vault**, against
roughly one in ten before this.

Measured first, which is what prompted it: **8 of 10 asks were declined**, 0 unanswered, 2
animated, across 22 vaults. The query path is healthy; the game's own search simply refuses most of
what this fork's probe arms. Widening the band made that worse, not better - our band and
`CanClimb`'s are different searches and only the game's one gates the animation.

**The lever is that `StartClimb` never asks where its struct came from.** `CanClimb` only *fills*
one - `found` at +0x00, entity at +0x08, target at +0x10 - and the two are separate functions with
a plain pointer between them. So the climb program now fills that struct itself when the search
declines and calls `StartClimb` on it anyway. The animation, the ped motion and the landing are
still entirely the game's; only the *decision* is taken away from it. This is the fallback
`kVCSPedSetClimbTarget` was written down for.

**The entity turned out not to matter, and that is the finding here.** `StartClimb` hands entity and
target to `0x0890f6ec`, which takes a *reference* on the entity and stores the target *relative to
it*, so the expectation going in was that a real entity would be needed - and getting one means
calling `ProcessVerticalLine` rather than the `FindGroundZFor3DCoord` wrapper the probe uses, an
eleven-argument call with three arguments past the eight this build passes in registers, whose
stack layout is not something to guess at (see the corrupt-stack crash above).

So the entity was made a **host-supplied field in the block** and the host passed `0`, to test the
cheap question first: *does a forced `StartClimb` animate at all?* **It does.** `0x0890f6ec`
null-checks the entity, and a target handed over with no entity to be relative to is taken as
world-absolute - which is exactly what a vault wants. `ProcessVerticalLine` is therefore not needed
for this, and the field stays host-supplied in case something later does want a real entity (a
climb onto a *moving* object would: with a null entity the target cannot track it).

**A climb that never engages is recoverable**, and the guard stays even though it has not needed to
fire. `VaultPhase::Climbing` waits `kClimbEngageTicks` (8) for the ped state to actually reach 44,
and drops back to the written motion if it never does - re-anchored to where the abandoned climb
left the ped, since the state-44 experiment dropped him 0.57 into a hang before giving up. Without
it, a forced climb the game accepts and then ignores would leave the player standing at the wall
having pressed jump for nothing. The Vault tab counts it as `never engaged`.

**The band was then walked up with the animation running, and the edge is 3.03** - not a round
number, and 3.10 is past it: the climb stops carrying. Worth keeping straight that this is still a
much higher ceiling than the game's own search would ever have allowed - `CanClimb` was refusing
about four walls in five inside this same band before forcing existed.

**With forcing on, fences are asked for too.** The reason they were excluded is that the game's
climb-out finishes on top of whatever *it* found, and on top of a fence is a rail - but once we are
supplying the target, it can be pointed at the far side instead. The target convention is a guess
(the landing surface point, not the ped origin) and is the first thing to calibrate if a forced
climb runs but lands somewhere wrong.

**Four things were tried before the two calls, and each one looked like the answer.** They are
written up in the addresses doc rather than repeated here, but the shape is worth carrying:

- the climb **stage** byte (`CPed+0x1d9`, 0 then 1..4) is a *status*, not an input - writing it on
  land sticks and does nothing;
- the climb **state** (`CPed+0x8b4 = 44`) engages the game's climb and then aborts within a second,
  because nothing told it *what* to climb;
- the **in-water flag** cannot be forced - the game recomputes it from the world every frame, and
  the attempt left the ped under the collision and dropped him through the map;
- `0x0892f140` is the **script command's** anim API, not the engine's - measured, zero calls during
  ordinary play.

The through-line: **a state is not an entry point.** Three of those four were real fields with the
right values in them, and setting a field the game writes is not the same as doing the thing the
game does when it writes it. The entry point was a function all along, and the way to a function
nobody can name is to break on the field and read the stack.

#### The splash, and why a vault on dry land made one

**Status: fixed 2026-08-28.** The animation this whole feature wanted is the climb *out of the
water*, and partway through it the game plays a splash. On a quay that is the sea letting go of
you; on a fence in Little Haiti it is a bug. Nothing on that path tests for water, and nothing had
to — before this fork there was no way to reach the climb on dry land.

One call does it, in the anim finish callback at `0x08905824`, on the branch taken when the climb
stage goes 1 → 2: `jal 0x08a05f80` at **`0x08905920`**, with sound **24** in the delay slot. See
"The splash the climb-out makes" in [docs/VCS_ADDRESSES.md](docs/VCS_ADDRESSES.md) for the audio
addresses and for how 24 was *established* to be the splash rather than assumed — the ped code at
`0x08927740` plays the same id and plays it only when the in-water flag is set.

**The sound is refused, not the code rewritten.** `cAudioManager::PlayOneShot` drops a negative
audio entity id on its first instruction, so `Hook_vcs_climb_splash` — a `REPFLAG_HOOKENTER`
replacement installed by address, exactly as the fire hook is — writes `-1` into `$a1` and lets
the call run to nothing. That keeps the whole thing to one conditional register write, with the
game's own instruction still in place and restored at shutdown.

**It is gated on whose climb it is, not on whether the ped is in water**, and the difference
matters. The in-water flag is recomputed from the world every frame, so by the time the ped has
cleared the edge it may already have gone off; testing it would silence the very climb the sound
was written for. The vault knows for certain which climbs are its own, and a genuine swim to a
quay never goes through it. The Vault tab counts the ones it has silenced, because a count stuck
at zero while vaults animate is a hook that never installed — which looks nothing like a hook with
no work to do.

**Where the audio lives, since nothing here had touched it before.** The retail build still
carries the sound service's debug strings, and
`set voice: voice=%d sfx=%d bank=%d addr=%x length=%d` at `0x08b7400c` is what located the module
at all. From there: the ped one-shot queue at manager `+0x187a` (stride `0x38`) gave
`PlayOneShot` at `0x089b83c0`, and listing every call site with its constant sound id gave both
the splash and the proof of what it is. All of it offline, from a savestate, with
`Tools/vcsstatic.py`.

### Draw distance — ported, measured, removed

**Status: gone.** It was built, it worked, and it changed nothing anyone could see, so it came out
rather than go on sitting in the Graphics page as a switch with no effect. `VCSDrawDistance.cpp`,
its debugger tab and its four menu rows are all deleted; what follows is the part worth keeping,
because this is an idea that comes back.

Every address came from PSPRecomp's VCS profile — a static-recompilation project targeting this
same disc, whose `vcs_draw_distance_patch.cpp` names `CDraw::ms_fFarClipZ` at `$gp + 0x1e74`, the
setter at `0x08a1ad6c`, the IDE/model-info table at `$gp + 24` with its count at `$gp + 7656`, and
three draw distances at `+0x2c`, `+0x30` and `+0x34` off each model-info. None of it was hunted
for here. All of it was disassembled out of a savestate before being trusted, and all of it was
right, which is the only reason the port took an afternoon.

The METHOD was not theirs and could not be, and that part outlives the feature. A static recompiler
swaps whole functions out of a dispatch table and jumps to a continuation address when it is done;
PPSSPP runs the real MIPS and a replacement always returns to `$ra`. So each of their five hooks
was read first and then re-expressed as the smallest thing reaching the same value. Three of the
five stopped being code patches at all — the vehicle and ped range constants are `lui` immediates,
so they are one 16-bit field each. `SetFarClipZ` is a two-instruction leaf, which is the one case
PPSSPP's replacement path fits exactly. Only the entity LOD site needed a hook.

**What the measurement said.** The far clip scaled exactly as intended: 1978.9 out to 3957.9 at
2x, read live off the debugger tab. At 8x the view was indistinguishable from stock.

That was not a bug in the port, and it is the finding worth keeping: **the far clip only governs
how far the game is WILLING to draw.** Whether anything is out there to draw is decided by
streaming and the per-model distances, and VCS appears to carry no separate LOD geometry to put at
range — the level containers show essentially no LOD-prefixed models, against one in `GAME.DTZ`.
Flown and looked at; vanilla draw distance is adequate. Do not restart this without a reason that
is not "the far clip is too close".

The one lever never actually pulled was the model-info table walk. It reported zero entries scaled
in the build that was tested, because of the second trap below, and the fix for that was never
exercised. If the question does come back, start there, and start from
`git log --diff-filter=D -- Core/VCS/VCSDrawDistance.cpp` rather than from nothing — the `$gp`
offsets are worth trusting: on this build `$gp` reads `0x08bb1d60`, putting the far clip at
`0x08bb3bd4` and the model count at `0x08bb3b48`, right among `TimeStep` and `FrameCounter` in the
address table.

### Dynamic sun shadows

**Status: working, confirmed in play, on by default.** Three passes inside every frame, at full
speed (30.0 fps measured on an ordinary Vice Point street with 1369 draws and ~146k vertices in
the frame). `GPU/Common/VCSShadow.cpp` holds all of it; the hooks are four lines in
`DrawEngineVulkan` and one in `GPU_Vulkan`. The row is `Dynamic shadows` on the Graphics page, and
everything worth tuning is on the debugger's **Shadows** tab.

```
capture    every draw the caster filter accepts is baked to world space on the CPU and
           flattened into one triangle list - two index streams over one vertex buffer
cascade    that list drawn once from the sun into a 2048^2 depth map
mask       the same list drawn again from the camera, comparing against the map, into a
           screen-sized black-and-white image of what the sun reaches
composite   one triangle over the game's own framebuffer, multiplying it by that mask
```

**All three run inside the frame, at the seam where it turns from 3D to 2D**, not at the top of
the next one against the frame that just ended. The earlier attempt did the latter to keep the
passes out of the way of the game's own render passes, and for the shadow MAP that is fine -
geometry barely moves in a frame. For the screen-space mask it is not: the mask is the camera's own
view, so a frame of lag slides the whole shadow layer across the picture every time the player
turns. `VCSShadow::OnFlush` runs from the top of `DrawEngineVulkan::Flush`, on the first flush of
the frame whose batch is in through mode. That is the seam by construction: any change to the
through bit goes through `Execute_VertexTypeSkinning`, which flushes first. It is also why the HUD
is not shadowed - the radar and the wanted stars are drawn after this, over the top.

#### The mask has to line up with the frame, and view*proj is not enough

The projection the mask rasterises with is `view * proj * post`, and `post` is the rest of what
PPSSPP's own vertex shader does: the perspective divide, the PSP's viewport scale and offset, the
raster offset, and the remap into the render target's NDC. All of that is linear in the clip vector
- the divide the viewport transform needs is undone again by the multiply back into clip space at
the end - so it folds into one more 4x4 rather than having to be replayed. Measured on this game:
scale `(256, -160, -32755.5)`, offset `(2048, 2048, 32763.5)`, raster offset `(1792, 1888)`, render
target 512x320 PSP pixels. Stopping at `view * proj` gets a mask that resembles the frame and does
not sit on it, and a shadow mask a few pixels out is worse than no shadow at all.

**Depth is the one axis deliberately not copied.** Those numbers are a REVERSED depth buffer - near
maps to 65519 and far to 8, and the game sets a matching `GEQUAL` compare, which is visible in any
draw's state. This pass owns its own depth buffer and tests `LESS` against it, so copying the
mapping verbatim makes the FARTHEST surface win every pixel of the mask. Nothing downstream cares
which way camera depth runs, because the shadow comparison happens in light space, so
`BuildPostProjMatrix` inverts it when the Z scale is negative and stops there.

#### The game draws full-screen overlays as world geometry, and they own the mask

The single hardest bug here, and it looks like nothing else. With the depth direction fixed, the
mask came back as one flat colour over the entire frame - not white, not black, a specific
mid-value - while every count in the panel read healthy.

What it is: a quad **0.6 world units across, centred exactly on the recovered camera**, covering
every pixel. It is one of the game's own full-screen overlays - the colour filter - drawn as 3D
geometry rather than in through mode, so no render-state test can see it for what it is: it writes
depth, its blend is a copy, and it is a perfectly ordinary triangle strip. Once the mask keeps the
nearest surface, it wins every pixel, and every pixel then samples the same texel of the shadow map
and gets the same answer.

`nearCameraCutoff` (2.0 units) throws away any draw whose whole footprint sits within that of the
camera, caster and receiver both. The camera is never inside real geometry and the third-person
camera sits 4.6 units behind the player, so there is a wide gap to put the threshold in. The
Shadows tab counts what it drops - a frame here drops 8 - and a count of zero next to a flat mask
is the signature of this returning.

**How it was found is the transferable part.** Four rounds of reasoning about the light matrix,
the depth pass, the sampler and the shader all failed, because all four were arguments about
mechanism from a symptom. What settled it in one build was carrying the raw `a_position` through as
a second varying and drawing `fract(world * 0.05)` to the screen: uniform to within 0.6 units
across the whole frame, with one visible diagonal seam. That is not a fact about shadows, it is a
fact about what is being rasterised, and it could not be argued with. When an interpolated value is
constant and it should not be, ask what geometry is actually there before asking what is wrong with
the maths.

#### A one-character bug in PPSSPP that this feature was the first to hit

`VKContext::DrawIndexedUP` in `Common/GPU/Vulkan/thin3d_vulkan.cpp` filled the index buffer from
`vdata` instead of `idata`. **Nothing in PPSSPP itself calls `DrawIndexedUP`**, which is how it
survived - this fork's shadow passes are its only user on any backend.

It does not fail loudly. The vertex data reinterpreted as `u16` indices is still a list of valid
vertex numbers, so the draw renders a mesh - an arbitrary triangulation of the real vertices, which
from a distance looks like *something*. What it cost:

- the shadow map and the mask were both drawn from garbage topology, so every value read out of
  them was meaningless while looking plausible;
- and the enormous triangles that topology produces cover the screen many times over, which took
  the game from 30 fps to **1.24 fps**. That number was read off the game's own frame counter over
  the WebSocket debugger, and it is what made the bug findable: a 24x cost is not a shadow pass
  being expensive, it is something structurally wrong.

Worth reporting upstream. The fix is one identifier.

#### What the filters are for

- **`nearCameraCutoff`** - the overlays above.
- **`maxCasterSpan`** (400 units) - a draw wider than this receives shadow but never casts it. The
  sky and the map-spanning ground and water quads are what it is for: one surface across the whole
  cascade fills the shadow map and puts the entire city in its own shadow. This is why there are
  two index streams over one vertex buffer rather than one.
- **the caster filter itself** - unchanged from the first attempt and still the interesting one.
  Through mode is 2D. Sprites are rejected because a camera-facing billboard casts a shadow that
  swings as you turn. Depth-write-off catches particles, coronas and the vanilla blob. And
  `BlendAltersDestination` is the test that matters: VCS switches blending on for its opaque pass
  and specifies a blend that copies, so asking "is blending enabled" throws away the entire city
  and asking "can this blend change the destination" keeps it.
- **an off-screen-target reject** - a render-to-texture pass has its own camera, and mixing its
  geometry into the capture would project one frame's shadows from two of them.

#### Pixelated was the FILTER, not the resolution

Reported as "really low quality, pixelated", and the instinct - a bigger shadow map - would have
been treating the symptom. The mask took nine NEAREST samples of the depth map and averaged them,
so the shadow term could only ever be one of ten values, and every one of them changed at a texel
boundary. At 2048 texels across a 140-unit cascade that is a visible step every 7cm on the ground,
and averaging more taps only ever makes a bigger staircase.

The filter now weights each of those same nine taps by how far the sample point actually falls from
it. The taps sit on a grid of whole texels `spacing` apart so they always land on texel centres,
and the weights are the ordinary bilinear ones evaluated in that grid's units - which makes the
result continuous rather than quantised, at exactly the cost it had before. `pcfRadius` still means
"softer": it widens the spacing and the reconstruction stretches with it, instead of meaning "more
values to average".

The map went to **4096** alongside it, which is worth about 3.4cm a texel. That is the part that
IS resolution, and it is cheap here for a reason worth remembering: the depth pass is depth-only
over geometry that is already sitting in a vertex buffer, so doubling the map costs rasterisation
nobody was short of rather than memory bandwidth on a texture nobody is sampling. The debugger's
Shadows tab drops it back to 2048 in one click if a machine disagrees.

#### Two cascades in one atlas, because one can never be both wide and sharp

Reported after the filter fix as "still quite pixelated", and the arithmetic says why a third round
of filtering would not have helped either. A single cascade 70 units across a 4096 map is 3.4cm a
texel; at arm's length that is about **eleven screen pixels** at 1080p, and no reconstruction turns
eleven pixels of one value into a sharp edge. The far cascade cannot simply be shrunk, because its
size is what puts shadows on the road before you drive into them.

So the depth pass renders **twice into one texture**: `nearCascadeRadius` (18 units, about 0.9cm a
texel - three screen pixels) around the camera in the left tile, and `cascadeRadius` for everything
beyond it in the right. The mask picks per pixel.

**One texture rather than two is the whole reason this is a small change.** A second shadow map
would mean a second sampler in the mask pipeline, which means a different descriptor layout and a
different pipeline; an atlas means the fragment shader halves a `u` and adds a tile offset, and
every other line of the pass is untouched. The filter above still works entirely in TILE texels and
only converts at the moment it samples, which is what let a fairly delicate PCF survive the change
without being re-derived.

Four things in it are worth knowing before touching it:

- **Each cascade snaps to its OWN texel size.** Sharing the far one's would leave the near cascade
  crawling as the camera moves, which is the exact artefact snapping exists to remove - and it
  would be four times more visible in the tile that is four times finer.
- **The near cascade is anchored on the camera, not ahead of it.** `centreDistance` exists because
  a car outruns its shadows; the near tile is for the ground under your feet and the vehicle beside
  you, so it sits a third of its own radius ahead and no further.
- **The handover happens at 0.92 of the near box, not at its edge.** A PCF tap at the very boundary
  would reach outside the tile and read whatever the far cascade left in the neighbouring texels -
  the two tiles are adjacent in one image, so "outside" is not empty, it is the other cascade. A few
  texels of margin puts the seam where both still agree.
- **`BuildLightViewProj` is called three times rather than copied.** The near matrix is built by
  swapping the near radius and centre into `ShadowView`, calling the same builder, taking a copy,
  and swapping back. Two cascades that construct their box by different code are two cascades that
  will eventually disagree about the near plane.

**Measured: 29.91 fps, which is full speed, with 4096-texel tiles - an 8192x4096 depth buffer.**
The depth pass draws the same geometry twice, and it was never the expensive half: it is depth-only
over vertices already sitting in a buffer. Setting `Near cascade radius` to 0 in the debugger turns
the split off and halves the atlas, which is the first thing to try if a machine disagrees.

**And a note on how this was checked, because the first screenshot said "broken".** A shot taken
under the pier came back with a huge blue slab across the frame and read as a serious bug; the same
build on an open street came back with crisp building and palm shadows and the player's own shadow
correct. The slab was a real shadow - the structure overhead - and the two pictures differed by
where the player stood and an hour of game time. **One screenshot of a shadow feature is not
evidence**; this file already records two rounds lost to exactly that, and the working practice is a
control shot from the same spot with the feature off.

#### Bias, and why it is not the usual bias

Only ~24 draws in 161 carry vertex normals - the peds and the vehicles. The world is prelit into
vertex colours and ships without them, so normal-offset bias, the good answer to shadow acne, is
not available for the geometry that needs it most. The mask uses `fwidth` of the light-space depth
instead: it is large exactly where the light grazes a surface, which is where acne is, and near
zero on a wall facing the sun. `slopeBias` is that term and `depthBias` is the constant under it.

#### The sun is the game's own, and so is the strength

VCS hands the GE a white directional light and the shadow direction comes straight off it - no
game clock, no solar model. Two things about it:

- **The pick has to reject a light that is level with the horizon.** A capture came back with
  channel 0 at exactly `(1, 0, 0)` at full white, which is what a channel holds when nobody has set
  it, and brightness cannot separate that from the real sun when both are white. Z is up here, so
  candidates at or below the horizon are refused. Every genuine sun measured sat between 0.17 and
  0.54 in Z; the impostor sat at 0.
- **The shadow's strength is multiplied by that light's own luminance**, so shadows thin out
  towards dusk and are gone at night without anything here having to know the time.

**It does turn with the clock** - `(0.50, 0.50, 0.71)` early on, `(-0.39, 0.60, 0.70)` an
afternoon later. An earlier version of this note said the opposite, on the strength of half a dozen
samples that all happened to fall within half an hour of game time. A constant is not established
by measuring it repeatedly at the same moment.

#### Three things the first working version got wrong

Reported from play, and each one is a different kind of mistake.

**1. Shadows blinked out of the middle of the road.** The capture only ever sees what reaches the
GPU, and the game culls to its own camera frustum - that is how a PSP ran this at all. So a
building a few degrees off the left edge is not drawn, is not captured, and stops casting the
shadow that was lying across the road in front of you.

The fix is a **caster cache**, not a wider frustum. Making the game draw more costs frames on top
of everything the shadows already cost, and the geometry that matters here does not move: a
placement that was captured recently is still exactly where it was. Entries are keyed on
`(vertexAddr, vertex count, world matrix)` - the model plus where it was put - and re-submitted to
the DEPTH pass alone, never the mask, because a receiver has to be on screen to be shaded.

Four rules keep it honest, and each is there because of a specific way it could ghost:

- **Two consecutive sightings before an entry is kept.** A moving object has a different matrix
  every frame and therefore a different key every frame, so it can never reach two - the cost of a
  car driving past is one map entry and no geometry at all.
- **Skinned meshes never go in.** Their vertices arrive already in pose, so a remembered copy is a
  person frozen mid-stride.
- **Same model, nearly the same place, different matrix means it moved**, and the old memory of it
  is evicted at once. Distance is what makes that safe: the second copy of a building a street
  away is the same model in a different place and says nothing about the first.
- **A hold, and a radius.** Four seconds and 150 units. The radius is what stops the cache growing
  into the whole city as you drive across it; the hold is the backstop for the one case the rules
  above cannot catch, an object that moves and leaves the view in the same frame.

Kept is not the same as submitted: an entry only enters the depth pass if its sphere reaches the
cascade, which is a hundred units across against a cache a hundred and fifty in every direction.
On an ordinary street that is ~600 draws re-submitted out of ~1400 held, at no cost in frame rate,
because it is geometry the game was going to make us pay for anyway and the GPU is not the
bottleneck here.

The cache holds positions in the space the GE is fed, and the game rebases that space. A rebase, a
teleport or a load shows up as the recovered camera jumping further in one frame than any camera
can move, and everything remembered is then in the wrong place - so that jump clears the cache.
Cheaper than tracking the offset, and it cannot be subtly wrong.

**2. Palm leaves cast their bounding boxes.** A leaf card is a quad with a texture that is mostly
hole, and the depth pass carries no textures, so it wrote the rectangle.

The caster filter had this backwards and said so in its own comment: it asked
`gstate_c.vertexFullAlpha` and noted that erring towards "opaque" was the cheap direction to be
wrong in. It is not - it is the direction that puts crates in the sky. The final alpha is vertex
alpha TIMES texture alpha, and only the first half was being asked about.

**PPSSPP tracks the second half and it is unusable here.** `gstate_c.textureSolidAlpha` is set from
the decoded texture's own alpha, and only on the path that decodes the PSP's texture: with
replacement on, which is this port's whole point, `LoadTextureLevel` takes the replaced branch and
leaves the status at `TextureAlpha::Any`. Believing it threw out the entire city - 0 casters, 58 of
58 blendable draws rejected, no shadows anywhere. That is worth remembering as a shape: a flag that
is correct for the emulator's own purposes can be systematically wrong for yours, and the way it
fails is silent.

So `TextureIsSolid()` asks the GAME's texture instead, which is also the better question - whether
a palm leaf is a cut-out is a fact about VCS, not about which pack is installed. It scans level 0
for texels below half alpha (5551, 4444, 8888, and CLUT4/8 through the palette entries the indices
actually use), calls it a cut-out past 2% - an antialiased edge is not a hole - and caches the
answer per texture, so it costs one scan each and a hash lookup per draw. Swizzling does not matter
to a scan that only counts.

Alpha-*tested* draws get the same question and their own reject reason. The result is that foliage
casts nothing rather than casting a box, which is the honest outcome until the depth pass can carry
a texture; cut-out shadows are the obvious next step and need a draw call per texture.

**3. The vanilla blob shadow was still there, so everything that moves had two.**

The textures it uses are **learned, not named**. A flat, small, alpha-blended quad that writes no
depth and lies directly underneath something the capture accepted is that object's shadow,
whatever texture it is using today - and the addresses move, so naming them would not have worked
anyway. The test is positional on purpose: the render state a blob uses is the render state a decal
uses, and a tyre mark on open road has nothing above it.

A texture is not believed on one sighting. Eight are needed before it goes in the table, because
without that count the learner filled all eight of its slots within seconds of the world loading -
which is what over-learning looks like from outside. With the count it settles at around ten
textures on an ordinary street and drops five or six draws a frame.

`ShouldSkipDraw` re-checks the shape as well as the texture before dropping anything, so a learned
texture that also turns up in through mode - the HUD is not ours to edit - is left alone.

#### And six more from the next play session

**Stripes across the road.** Shadow acne, and the bias was never going to fix it. Both passes read
the *same* captured vertices, so a surface is compared against ITSELF and the only error is where
the shadow map's texel grid falls - and no bias small enough to keep shadows attached to their
casters is reliably bigger than that.

The answer is the textbook one: **only faces turned away from the light go into the depth map**.
Storing the far side of every object puts a whole object's thickness between the caster and the
receiver, which no rasterisation difference can cross. It is done on the CPU during the capture,
per triangle, against the light direction the frame has seen so far - the light moves far too
slowly for last frame's to decide the wrong side. Winding comes from `gstate.getCullMode()`,
because PPSSPP culls with the game's own cull mode rather than normalising it away, so a draw with
mode 1 has the opposite front face and a facing test that ignored that would be right half the
time. `flipCasterWinding` is there if the whole convention turns out inverted, which reads as
worse acne rather than as missing shadows.

It also halves what the depth pass draws. Measured on an ordinary street: 28k triangles cast,
127k do not - and most of that difference is the ground, which faces the sky and so never casts
onto itself. That is the acne, gone by construction rather than by tuning.

**Shadows still vanished when the caster left the view, and the cache was the reason.** It held
almost nothing while moving: fifteen hits against fourteen hundred inserts a frame, *standing
still*. Two findings, in order:

- The key was `(vertexAddr, world matrix)`, and **VCS feeds its geometry through a per-frame
  scratch buffer** - the same wall is at a different address every frame, so every key was new.
- Keying on the geometry instead - vertex count, triangle count, bounding box - did no better,
  and the reason is the more useful one: **`AddCaster` sees one FLUSH, not one object.** PPSSPP
  merges draw calls until some piece of state changes, and where those merges fall moves from
  frame to frame even when the scene is identical. No key built out of a flush can repeat.

So the cache remembers **places, not draws**: a 32-unit grid, and whatever was captured inside a
cell replaces whatever was there before. A grid does not care how draws are grouped. It is also
simpler than what it replaced - no model identity, no placement keys, no "did this move" rule,
because a cell that is drawn is simply overwritten. Now carrying ~60 cells and ~17k vertices the
game is no longer drawing, at no cost in frame rate.

Two things make it safe. Skinned meshes are never remembered, because their vertices arrive
already in pose and a remembered copy is a person frozen mid-stride. And the geometry is stored in
the space the GE is fed, which was worth checking rather than assuming: a 3609-vertex building
reported the same bounding box corner to four decimal places across several seconds of play, and
the offset between that space and the game's own coordinates measured a constant (-287, 216). A
jump in the recovered camera bigger than any camera could move in a frame still clears the whole
cache, for the rebase or the load that would invalidate all of it at once.

**Shadows arriving late while driving.** The cascade was 50 units with its centre 25 ahead, which
is fine on foot and about a second short in a car. 70 and 40 now, with the cache reaching 220 - it
has to hold what the cascade will want next, not what it wants now.

**Palm leaves cast their bounding boxes** and **the vanilla blob shadow was still there** - both
covered in the section above; they were reported in the same session.

**No shadows at all after dark.** Correct, and not what anyone wants: the game's brightest
directional light is genuinely below the horizon at night, and a light below the horizon casts
nothing. It is mirrored back above it instead - which is roughly where the moon is - lifted to a
plausible elevation if mirroring alone leaves it grazing, and scaled by `moonStrength`. The
impostor channel that the horizon test used to catch is now named directly instead: a channel at
exactly `(1, 0, 0)` is what one holds when nobody has set it, and no real solar or lunar direction
lands on an axis.

This also corrected the note above about the sun not turning. It does: sampled across an afternoon
it moved from `(0.50, 0.50, 0.71)` to `(-0.39, 0.60, 0.70)`. The earlier samples all read the same
because they were all taken within half an hour of game time.

**Microstuttering while travelling.** Two causes, both from work that arrives in bursts rather
than steadily:

- **Texture alpha scans.** A 256x256 32-bit texture is a quarter of a megabyte to read, and new
  ones arrive in clumps as the streamer works. Two a frame now; a texture that has not been
  scanned yet is treated as solid, so the worst a delay costs is one frame of a leaf quad casting
  its box.
- **The capture buffers growing a piece at a time.** They settle after a few frames and then stop
  allocating, but the first frames of a new area reallocate a megabyte at a time - which is a
  hitch exactly when the world is streaming. Reserved up front.

**One bug worth recording for its shape rather than its cause.** `Settings` is initialised
positionally, three new fields went in at the top of the initialiser and in the middle of the
struct, and the compiler had nothing to say: `cacheHoldSeconds` took a `bool`, became zero, and the
cache expired every frame. Every count in the panel stayed plausible - cells held, geometry
captured, frame rate fine - because the only wrong number was one nobody was printing. Positional
initialisers for a struct that is still growing are a trap; the order is now called out in a
comment at the top of the list.

#### The near plane, which is what "the building behind me casts nothing" actually was

Reported again after the cache was fixed, and the cache was never the problem - it had the
building. The depth pass was throwing it away.

A shadow travels along the light and nowhere else, so in light space a caster sits at the same
place as the shadow it throws and differs from it only in DEPTH. The light box was pulled back
along the light by exactly one cascade radius, which meant anything more than seventy units
towards the sun was clipped out of the map before it could cast into it. With the sun behind you
that is most of a street.

`casterReach` (300 units) moves the near plane back instead. It costs depth range and nothing
else: the sides of the box were already exactly right, because a caster outside them lands outside
them too. Widening the cascade would have fixed the same thing by spending resolution everywhere.

The cache's own reach test had to learn the same lesson - it was culling cells by distance from the
cascade centre, which is the camera's way of measuring. It measures in the light's frame now:
across the light a cell has to be inside the box, along it a cell can be most of a street away.

#### A cell that is half on screen was overwriting its own memory

Reported a third time, after the cache and after the near plane, and it was neither of those. The
cache was working - proved by switching the LIVE casters off for one build and leaving only what
had been remembered, which still shaded the street. The depth map, put on screen as debug view 4,
holds a whole neighbourhood: roads, buildings, lamp posts well outside the view.

The bug was in how a cell is refreshed. A cell is replaced wholesale by whatever the game drew in
it, which is the property that makes a grid immune to re-batching - and it is wrong the moment a
cell is only PARTLY visible. As a building slides off the edge of the screen, the sliver of it
still being drawn keeps its cell alive and overwrites the memory of the whole with the memory of
the sliver. The shadow shrank away exactly as the caster left the view, which is the symptom the
cache exists to remove, wearing the cache's own clothes.

`CellIsWhollyInView` gates the replacement on the cell's centre being comfortably inside the same
camera matrix the mask rasterises with - three quarters of the way to the edge. Outside that, the
cell keeps what it had and only its timestamp is refreshed. An empty cell always accepts what it is
offered, or a cell that is never fully seen would never hold anything.

**The method note is the part worth keeping.** Three rounds went into this: a cache, a near plane,
and this. The first two were reasoned from the symptom and both were real bugs that were not THE
bug. What ended it was two pictures - the depth map on screen, and the frame with live casters
disabled - neither of which is an argument about mechanism. Both took one build each. The reasoning
took several.

#### One row, three positions: off, people and vehicles, everything

`Shadows` on the Graphics page, **defaulting to the middle** since 2026-09-09. It was a `Dynamic
shadows` switch with a separate `Shadows from` mode under it, and fusing them is not tidying: the
two were one decision wearing two controls, so the mode row had to grey itself out when the switch
was off, and a saved mode went on existing for a feature that was not running. One `Choice`, one
ini key, and `ApplyShadowSetting` as the single place that turns the number into the two answers
the renderer wants - which is what stops "off" and "people and vehicles" ever being live together.

The middle position is the frame-rate lever for this whole feature - what the shadow pass costs is mostly the geometry it
draws a second time, and the city is nearly all of that geometry. It is also a real preference
rather than a quality ladder: shadows under the player, the traffic and the crowd are the ones a
player watches, and they are the ones the PSP game itself fakes with a blob under every object.

**The world still RECEIVES.** Only the caster half of the split is filtered - the same mechanism
`maxCasterSpan` already uses to keep the sky and the map-spanning ground quad out of the depth map -
so the road goes on darkening under a car. A mode that dropped the world from both halves would put
the car's shadow nowhere.

**The discriminator is vertex normals, and in this game that is a fact rather than a heuristic.**
VCS ships its scenery prelit into vertex colours and carries no normals for any of it; the models
that have to be lit as they move carry them, because nothing can prelight a car that drives. That
was measured here long before this setting existed, while working out why normal-offset bias was
not available: 24 draws of 161 on an ordinary street, and 24 is what a street's worth of traffic and
pedestrians looks like. So the test is one bit of the vertex type, already decoded, costing nothing.

**It turns the caster cache off, and that is correctness rather than an optimisation.** The cache
remembers PLACES: a 32-unit cell keeps whatever was captured in it until something else is drawn
there, which is exactly right for scenery that has left the view and exactly wrong for a car, whose
cell has nothing to replace it with once it has driven out. Everything this mode captures is
something that moves, so there is nothing left worth remembering - and the cache is skipped rather
than left running to no purpose.

**Switching the mode ON also throws the cache away rather than letting it drain.** Everything in it
is scenery, nothing captured afterwards will replace any of it, and a held cell casts for
`cacheHoldSeconds` - twenty-five of them. Without the clear, the setting appears not to work for the
better part of half a minute, which reads as the filter being broken rather than as the cache being
patient.

**Measured, on an ordinary street: 24 of 1154 captured draws cast, and 12 of those 24 are skinned.**
Two per cent of the frame, and half of what survives is people - which is what "only the things that
move" is supposed to look like, since the other half is the traffic. The skinned count is the check
worth keeping: a healthy caster count with zero skinned draws in it would mean the filter is keeping
the wrong twenty-four, and no screenshot taken at the wrong time of day could tell you that.

It says so once per boot in the log, for the reason the caster cache does: this rests on a claim
about the game's vertex formats, and a build where the claim stopped holding would show up as
shadows quietly missing rather than as anything an assert could catch. The Shadows tab carries the
same two numbers every frame.

The row is `enabledBy` the switch above it, so it greys out with the feature rather than sitting
there implying shadows are on. The debugger's Shadows tab has the same toggle, next to
`Max caster span`, for flipping it against a scene without leaving the game.

**A note on testing this at all, which cost more than the feature did.** Judging it by screenshot is
much harder than it sounds: the sun's own brightness scales the shadow strength, so dusk looks
identical to the feature being off, and this fork's save loads indoors. Two full rounds of
screenshots were taken before noticing that `vcs.ini` had `DynamicShadows = False` in it - shadows
had been off entirely, and the pictures said nothing about the mode. The counter is the instrument;
the picture is the illustration. Check the setting is actually on before reading a frame.

#### The blob shadows came back in people-and-vehicles mode, and the cause was one word

Reported as "with the shadow set to player + peds, on loading the game the static shadow sprites
still appear" - the vanilla blob under every ped and car, in the one mode whose entire point is that
those things have real shadows instead.

**The learner matches a flat quad against the bounds of things drawn near it, and that list was
gated on `casts`.** In people-and-vehicles mode `casts` is false for all the scenery, so the list
stopped containing the ground, the kerbs and the props a blob actually lies on, and nothing matched
any more. Measured with a temporary counter through the funnel: **400 ground quads, 96 of them flat
and small, and 0 over an object**, against 23 bounds in the frame. With the gate removed: 24 flat
quads, **24 over an object**, 944 bounds, and textures learning again within seconds of a load.

The list exists only for this test, so it never wanted the caster mode's opinion. `s_objectBounds`
is now filled for any small draw the caster filter's shape tests accepted, in every mode, which is
what it was doing before the mode existed.

**A second, independent bug was fixed alongside it, and it is the one the "upon loading" in the
report is really about.** The learner keys on a texture's ADDRESS, and a load puts the same artwork
somewhere else - so after loading a save every learned entry is a stale address matching nothing,
while the table stays full at its sixteen slots and refuses to learn the new ones. The world-reload
detection that already clears the caster cache - the recovered camera jumping further in one frame
than any camera can move - now clears the learned blob textures too.

**The method note.** Three explanations were argued for before any of them was tested: a stale
table, a slow learner, a mode that skipped the call. Two were real bugs and one of those was not
THIS bug, which is the shape this file has now recorded five times. What ended it was ten lines of
counter printing where the candidates were being lost - the funnel says "0 over an object" and there
is nothing left to have an opinion about. **A counter through the stages costs less than one round
of reasoning about which stage it is.**

#### Props cast too, and the test for one is that it stands up

`People, vehicles and props`, which is the same middle position renamed for what it now keeps.
Lamp posts, bins, hydrants, parking meters, benches, fence posts - the things a player walks
past, and the ones where a missing shadow is at eye level rather than across the street.

They are scenery by every test the game offers: VCS ships them prelit into vertex colours with no
normals, exactly like a building, so the normals rule that separates a car from the world throws
them away with everything else. The rule that keeps them is geometric, and it is two tests:

    isProp = footprint <= propMaxSpan (6 units) && height >= propMinHeight (2 units)

**Both halves earn their place, and the height is the interesting one.** The footprint separates a
prop from a building. The HEIGHT separates it from the only other two things with a small
footprint, and both of those are flat by definition: a road decal, and the vanilla blob shadow the
learner spends its time hunting. A prop is the one small thing that stands up. Reading the same
fact the other way, `propMinHeight` is what stops this mode putting the blob shadows back by the
front door after the learner has taken them out of the back.

**It also gives that mode its caster cache back, and that is a correctness change rather than an
optimisation.** The cache was off in people-and-vehicles mode for a good reason - it remembers
PLACES, and a cell keeps what it held until something else is drawn there, which is right for
scenery that has left the view and wrong for a car, whose cell has nothing to replace it with once
it has driven out. A prop is the exception that repairs the argument: it is the only thing in that
mode that cannot move. So the rule is now "may this thing move" rather than "which mode is this" -
skinned meshes never (people, frozen mid-stride), vehicles never in this mode, props always. A lamp
post keeps casting after you have driven past it.

**Measured on a street: 20 of 274 captured draws cast, 4 skinned, 16 props**, at 31fps - which is
full speed, and is what "the things you stand next to" should look like against a street's worth of
traffic. The Shadows tab carries all three numbers and both sliders.

The number to watch if it is ever retuned is `propMaxSpan`. Pushing it past a car's width starts
catching pieces of BUILDING, and a building that casts in pieces is the exact complaint this whole
line of work started from - so a prop count that climbs into the hundreds is the signature of a
span set too wide, not of a street full of bins.

#### Four positions, because props and people are two questions

`LOW`, `MEDIUM`, `HIGH`, `ULTRA` - off, people and vehicles, plus props, and the whole city -
defaulting to the third. The row was renamed to a quality ladder to sit with the other graphics
rows; the indices did not move, so a settings file written before that still means what it said.
The renderer carries the two halves separately - `entityCastersOnly` and `propCasters` - because
they answer different questions: one is about what MOVES, the other about what is small enough to
be a thing rather than the world. A player who dislikes one has no reason to lose the other.

**`propMaxSpan` is tuned by eye and the count is a poor guide**, which is worth stating plainly
because the number looks measured and is not. Six metres put 283 props among 307 casters on one
street - that is not a street full of bins, it is building segments getting through - and 2.5m
caught nothing at all. The cliff between them is the tell: `AddCaster` sees one FLUSH, not one
object, so a batch of three lamp posts has a bounding box three lamp posts wide and no single
threshold cleanly separates a prop from a wall. It sits at 4.0 with a slider on the Shadows tab
next to a live prop count - hundreds means buildings, zero means nothing qualifies, a few dozen is
a street.

#### Shadow distance was a row, and it could never have done anything

Removed. `cascadeRadius` is back to living on the debugger tab where knobs that need measuring
belong.

It was not broken - `ApplyShadowSetting` really did push it from the ini at boot - it was
**inert by construction in the mode anyone would be using it in**. In people-and-vehicles-and-props
mode every caster is a person, a car or a bin, all of them a few units from the camera and all of
them already inside the default 70-unit box. Widening the box adds nothing because there is nothing
out there that casts. It can only ever matter in `Everything`.

The lesson is one this file keeps relearning from a new angle: a setting has to be judged in the
mode it will actually be used in. The earlier measurement that said "400 makes shadows weaker" was
taken in `Everything`, where the row does something, and was then shipped as a row for players who
would never be in that mode.

#### A wheel is a disc, not a card

Motorcycle wheels cut their spokes out with the alpha test, so `Reject::Cutout` threw them away
with the palms - and the reasoning that justifies it for foliage does not survive being pointed at
a wheel. "The depth pass carries no textures, so it would write the rectangle" is only damning when
the geometry IS a rectangle. A palm frond is a flat card whose shape lives entirely in its texture;
a wheel is a disc of real geometry whose silhouette is very nearly what the depth pass would draw
anyway.

`cutoutEntitiesCast` lets a cut-out draw through when it carries vertex NORMALS - the same fact
about this game the caster modes rest on, used the other way round. Scenery cut-outs ship prelit
and carry none, so palms and chain-link are untouched.

**Not verified in play**: getting onto a motorcycle needs a person at the keyboard, and the reject
counters cannot be read remotely. It is a well-supported hypothesis with a switch next to it, not
a measurement.

#### People stopped shadowing themselves, in a second pass over the same geometry

Reported as the player and the NPCs casting their own shadows onto themselves. It is real
shadowing and it is correct - an arm over a torso, the far leg behind the near one - and on a body
two metres tall against a shadow map built for a street it reads as blotches crawling over the
model rather than as an arm.

Skinned draws are now a THIRD index stream over the same vertex buffer, drawn after the ordinary
receivers with `depthBias + pedReceiverBias`. One more draw call per batch and one uniform update;
no second copy of the geometry and no second pipeline. The bias (0.003, about a metre in the
default box) skips a body's own thickness while leaving anything deeper - a building, a car, a wall
- still shadowing them normally, which is why this is a bias rather than simply refusing to shade
them. Set it to 0 to get self-shadowing back.

#### Cast once, then there: the cache keeps scenery for good

Reported from play: a building's shadow still came apart as you walked away from it, and still went
out when the building had been behind you long enough. Neither was missing geometry. Both were rules
in the caster cache, and the cache had the building the whole time.

- **The clock.** `cacheHoldSeconds` was 25, so a building behind you for longer than that expired.
  The clock only ever existed for things that move, and in this game everything that moves carries
  vertex normals - so entities now stay out of the cache in every mode (props still go in), and
  the hold defaults to 0, which now means for as long as the cell is within `cacheRadius`.
- **Replace-on-sight at a distance.** A drawn cell replaced its memory wholesale whenever it was
  wholly on screen. But a far building is drawn as its LOD stand-in, or as only some of its parts,
  because the full-detail radius is baked into the level archives - see "The map is baked per cell"
  - so walking away overwrote the full shadow with the stand-in's. Only a sighting within
  `cacheReplaceRadius` (150 units, inside the ~266 the archives bake) may replace a cell now. A far
  one can still fill an empty cell.
- **A cell that keeps its memory now casts it that frame too.** Keeping used to mean keeping for
  later, so a half-visible or far cell cast only the live draw while it was being drawn - half a
  shadow for as long as half a building was on screen. It replays the memory alongside the live draw
  now. Depth keeps the nearest surface, so the two agree where they overlap and the remembered one
  fills in wherever the live one is short.

The log says so once per boot - "cells the game drew only in part or in low detail are casting
their remembered shadow" - at NOTICE, because WARN does not survive this build's log level. The
Shadows tab has `Replace only within`, and `Remember for` reads "forever" at zero.

What it does not change: geometry that has never been on screen still cannot cast, and a far building
seen for the first time is remembered as its stand-in until you have been within 150 units of it once.

**Two more ways a remembered shadow went out, both reported as shadows that disappear after being
cast once:**

- **A rebase wiped the cache.** Any recovered-camera jump over 40 units cleared everything, on the
  reasoning that a rebase, a teleport and a load look alike from the GE. The game rebases the GE space
  as you travel, so every rebase put out the shadow of everything already passed. The game's own
  camera (`CameraWorldX/Y`, read in `ClassifyDraw` on the same frame) tells them apart - it stays put
  across a rebase - so a rebase now shifts every cell by how far the space moved
  (`RebaseCachedCasters`), and only a camera that really jumped clears. Both are logged, the first
  twenty of each.
- **"Wholly in view" tested the cell's centre.** A 32-unit cell beside the camera has its centre on
  screen while half of it is off the edge; the game draws that half, and it replaced the whole cell's
  memory. `CellIsWhollyInView` now wants all eight corners inside the camera frustum.

**Not yet measured in play.** It needs a building walked away from and the camera turned, and the
harness can do neither - see the tooling limits in the streaming grid section.

#### The vanilla shadow sprite is always empty, and the texture pack says so

The game's own shadow sprite flashed for a moment whenever a new chunk loaded, and some edge cases
summoned it even at ULTRA. The blob hider cannot catch it reliably - it knows a sprite by its texture
ADDRESS, a chunk that streams in puts the same artwork somewhere new, and the positional test needs
something already captured standing over the sprite.

The replacement hash does not move, so the fix is data rather than code: `textures.ini` maps the
sprite to an empty PNG, whatever the shadow setting, so there is no sprite to flash and nothing has
to recognise it first. Off means no sprites as well - that was the decision, not an accident.

| key | size | replaced with |
|---|---|---|
| `00000000c7e766bc10deb3d8` | 64x64 | `Misc/vcs_no_shadow_64x64.png` |
| `000000007a13bf646b5c82bb` | 8x8 | `Misc/vcs_no_shadow_8x8.png` |
| `000000002303bdd0f09f186f` | 64x64 | `Misc/vcs_no_shadow_64x64.png` - the round one under people |
| `0000000007abc5ff3c1d7920` | 64x64 | `Misc/vcs_no_shadow_64x64.png` - a vehicle's |
| `0000000072614c46f94bfae7` | 64x64 | `Misc/vcs_no_shadow_64x64.png` - a vehicle's |
| `000000000914bef8f56d4b11` | 64x64 | `Misc/vcs_no_shadow_64x64.png` - a soft noisy square |
| `00000000f695bda8ad66d582` | 64x64 | `Misc/vcs_no_shadow_64x64.png` - a soft noisy square |
| `00000000122f0bca04a78a03` | 64x64 | `Misc/vcs_no_shadow_64x64.png` |
| `00000000f5df92ec0336c0b3` | 128x128 | `Misc/vcs_no_shadow_128x128.png` - the car's |

The car's was the one no scan of the dump could find, because it was never dumped: the pack already
mapped it, to `Particles/00000000f5df92ec0336c0b3.dds`, filed among the particles. It was found with
the Debug build's ImGui **Textures** window, which the fork extends to list only textures drawn in the
last couple of frames (with a size cap) and to show the selected one's key as `textures.ini` spells it,
with Copy key and Copy as empty buttons. Stand next to the thing, filter, click - that is the tool for
the next one of these. Its empty PNG matches the size of the replacement it displaces, 128x128.

The last three were picked by eye in play from a gallery of every transparent texture in the dump,
which is the fastest way to finish this list: a page of checkerboard cards, sprite-like ones first,
click to collect keys. It was built from the dump folder by a throwaway script, not kept in the repo.

The first two used to point at the pack's HD match for them, VC's `shad_heli`, which is how they were
found among the dump's blob-shaped textures. The other three were reported from play - a round blob
still under the player - and **their pack names are no help at all**: they were upscales under `up/`,
and the textures the pack does call `blackshadow1`, `blackshadow3` and `boatshadow_32` turned out to be
a curved panel, a striped sign and noise. What found them was the dump itself: every texture of 128
texels or less whose visible pixels are one flat colour and whose alpha does the shaping, drawn as a
labelled contact sheet and read by eye. Scorch marks, blood pools and hanging grass come up in that
same scan and are not shadows - leave them. The ini before the change is `textures.ini.bak-shadowsprites`,
which `Tools/vcspackage.py` leaves out of a package like every other backup.

**One line of code stays**, in `TextureCacheCommon::PollReplacement`: the rule that lets 3D draws
during play take replacements without waiting (see the driving stutter section) makes an exception
for any replacement whose file is `vcs_no_shadow_*`. Without it the first sighting of each sprite per
boot would draw the original for a frame.

**Add to the list from the dump, by eye, and nothing else.** A log of "the key of every draw the
positional blob test dropped" was built to complete it and taken straight back out: it named a NO
PARKING sign, chain-link, a fence, a newspaper, starflowers and rail mesh as blob shadows. It looked
the key up by texture address, and after a chunk streams in the first cache entry at an address can
belong to whatever lived there before.

The same run is a reason to look harder at the positional hider itself: skid marks and blood pools
genuinely are flat blended decals lying under something, and that rule drops those draws on sight.

#### People and vehicles flickered because the game drew a second frame after the passes

Reported on MEDIUM, where nothing is remembered: people, vehicles and the player flickering in and out
of shadow. A temporary per-frame log settled it in one play session. On the frames that flickered,
**1168 casters were classified after the passes had run, against 1159 before** - a whole second
scene, every person and vehicle in it. The game sometimes gets two frames into one host frame; the
passes ran at the first seam, the second frame painted over the first, and nothing had captured it.

`RestartCapture` is the fix: a caster that arrives after the passes starts a fresh capture, and the
next seam runs the passes again for the frame that is actually shown. It logs the first few times. The
cost is a second set of passes on those frames, which are the ones that were broken anyway.

The same log named a second, smaller cause: 5 to 10 draws of people and vehicles a frame fell to
`nearCameraCutoff`, the rule that drops the game's full-screen overlays, whenever the camera pulled in.
Skinned draws are now never dropped by it, and draws with normals only when they are no bigger than the
0.6-unit overlay quad.

What the log ruled out is worth as much: the sun did not jump between frames (once, by 26 degrees, in
two minutes), no frame went without a composite, and the rebase of the GE space was being followed -
seven rebases, each a multiple of the streaming grid (125 by 108.25), none of them clearing anything.

A second log settled what the second frames are: the same scene drawn again into the SAME framebuffer,
with draw, caster and receiver counts within a few of the first, not one frame split by a 2D draw. So
the re-capture is right as it stands, and it was not the whole story.

**The rest - and the cause of every flicker on every setting - was draws in two spaces at once.** It
still vanished after that, and neither log could see why, because the player was captured as a caster
in every frame. A rolling capture of the game window (below) put the drop-out at 16:42:38.5-39.0, and
the water module's camera-jump log - which, unlike the shadow one, is not capped - had a rebase of the
GE space at 38.676. The player had called it first: "somehow it's bound to the chunk generation".

The capture bakes every draw with its world matrix into one space and projects all of it with the view
matrix of the frame's FIRST caster. Around a chunk swap the game draws part of the scene with the camera
of the NEW space while the rest is still in the old one. Each draw is right on screen, because the GE
puts it through its own view matrix; baked with its world matrix alone it lands a whole streaming cell
away. So the player sat 125 units from the ground under them, the shadow was cast onto nothing, and the
same happened to cars, people, remembered buildings - everything, on every setting. `BakeDraw` now moves
a draw whose view matrix differs into the frame's space (`PrepareViewCorrection`: through its own view,
back through the inverse of the frame's). Confirmed in play: 33 logged corrections, every one exactly a
cell step apart (62.5 x 108.2, or 125), starting 30-280 ms before each logged rebase, about 5000 draws
every few seconds of riding. Reported afterwards as nothing flickering at all, ULTRA included.

**Two things built on a wrong reading of the same capture, recorded so nobody re-derives them.** The
crop showed the shadow vanishing exactly where "a separately drawn stretch of pavement began, behind a
hard seam", and that was read as ground that could not receive a shadow. It was the newly streamed chunk
arriving in the new space. `AddReceiver` - flat blended and cut-out surfaces kept as receivers - was
added on that reading and did not fix the drop-out; it stays because shadows landing on pavement edges
and decals is right on its own terms, and it logs the first one it takes. The re-capture of a second
frame and the near-camera exemption for people are NOT in that category: both were measured.

The first question for any future "it flickers around here" report is whether a rebase lines up with it.

The rolling capture is worth rebuilding for any "it flickers" report: a screenshot taken after the
player says so is always too late, and the logs only see what the capture was told to count. One trap
in writing it: PowerShell variable names ignore case, so a ring size `$N` and a frame counter `$n` are
the same variable, and the buffer silently stops being a ring.

#### Palm fronds cast through their own texture

Reported as "first it was a square texture, and when you fixed it, it doesn't cast the leaves at
all". Both halves were the plain depth pass carrying no textures: it could only write a frond's
card, which is the square, and rejecting cut-outs outright is what left palms casting nothing.

A cut-out is now drawn into the same shadow map by a second pipeline that samples the texture and
discards the holes. Four things make it work, and each one is the kind of thing a tidy-up undoes:

- **Fronds are drawn with a BLEND, not only the alpha test.** The standard alpha blend over opaque
  vertices blends only through the texture's alpha, so a mostly-hole texture drawn that way used to
  be filed as glass (`Reject::Blended`) before the cut-out test was even reached. `TestDraw` calls it
  `Reject::Cutout` now.
- **The geometry is baked before the texture is known.** Classification runs before the draw engine
  applies the draw's texture, so `AddCutoutCaster` bakes positions and UVs, and `NoteCutoutTexture` -
  called after the texture block on both transform paths - commits them with the image view. A bake
  nobody commits is dropped at the next `ClassifyDraw`, or it would pick up the next flush's texture.
- **The decoded UVs are already final.** In `GE_TEXMAP_TEXTURE_COORDS` the decoder applies the game's
  UV scale and offset, and the shader's own `u_uvscaleoffset` is 1.0 unless the texture is a
  framebuffer - which, like a palette expanded in the shader, is skipped rather than sampled.
- **A remembered frond does not keep its view.** The caster cache remembers cut-outs per cell with
  their texture's ADDRESS and dimensions, and every frame that replays them looks the view up again
  among the texture cache's live entries (`TextureCacheCommon::ForEachNativeTextureView`). A view
  handed to Vulkan after the cache freed it is a crash, not a wrong shadow. Not found means skipped
  for that frame, and the Shadows tab counts it as "unresolved".

Every triangle casts, not only the half facing away from the light: the back-face rule works by
putting an object's thickness between caster and receiver, and a frond is one card. Cut-outs are
casters only, never receivers - the mask carries no textures either, and a card's holes would hide
the ground behind them. Anything flatter than half a unit is a decal and casts nothing.

**On HIGH, cut-out scenery casts at any size a caster may be.** HIGH casts people, vehicles and
props, and a prop is at most `propMaxSpan` (4 units) across because a wide SOLID draw on that setting
is a building. A wide cut-out is still foliage or a fence. A 16-unit cut-out limit was tried first and
the fronds cast on ULTRA and nothing on HIGH - a temporary diagnostic logged 69 frond draws where even
single crowns were wider than that - so the limit is gone rather than tuned.

**Size is judged per connected piece, not per draw.** The game hands over every copy of a texture
across a wide area as one flush, and draws measured here spanned 500 to 3000 units. Judged per draw, a
row of palms sharing a frond texture was one object wider than any caster, while a lone palm beside
it cast - reported as "some palms fixed, others not". `AddCutoutCaster` splits the draw into connected
meshes (a union-find over its triangles) and keeps the pieces that pass. Flatness stays a question
about the whole draw, because a single frond card can lie nearly level.

**The alpha scan's verdict is keyed on content as well as address.** `TextureIsSolid` caches whether
a texture is a cut-out, and it was keyed on address, format and size. The streamer frees textures and
loads others at the same address, often at the same size and format, so a palm could inherit a wall's
"solid" for the rest of the session. `TextureContentFingerprint` samples 32 bytes across level 0 and
the start of the palette into the key. The cache also used to stop storing at 4096 entries, after
which everything new was read as solid between scans; it empties itself at 8192 instead.

**The diagnostic that found both is worth rebuilding the same way if cut-outs go missing again**, and
its first version is worth knowing about: it spent all 400 lines in the frame the city appeared,
because this game draws everything with the alpha test and blending on. Skip unscanned textures and
solid ones, and log the replacement file behind the bound view so a line leads to a texture in the
dump.

thin3d writes whatever texture and sampler are bound into the next draw's descriptor set through any
pipeline, so `DrawCutouts` unbinds both when it finishes. The log says `cut-out pass ran` once per
boot the first time it draws.

**Confirmed in play on ULTRA:** the fronds cast their shape. HIGH, after the size rule above was
dropped, has not been looked at yet, and nobody has measured what the per-texture draw calls cost on
a street full of palms.

#### Known, and deliberately left

**Geometry you have never looked at cannot cast.** The capture only ever sees what the game
draws, and the cache only remembers places that have been on screen. Walk into a street facing away
from a building and it casts nothing until you have seen it once. FOUR mechanisms have now been
found, decoded, patched and measured - the camera frustum, the streamer's delete-behind pass, the
far clip, and the LOD distance multiplier - and not one of them decides anything for the map. What
is left is that VCS draws whatever the streamer has loaded, so widening the view is a STREAMING
question and nothing else. See "The LOD multiplier was patched properly" below for the four
negatives in one table.

**A remembered frond needs its texture to still be in the cache.** Off-screen fronds are replayed
only if the texture cache still holds a texture at their address, so a palm whose texture has been
freed stops casting until the game draws it again. See "Palm fronds cast through their own texture".

**The moon is the sun's light mirrored, not a real lunar position.** It puts night shadows
somewhere plausible rather than somewhere correct.

#### Shadows go out in the rain

Reported from play: when it starts raining the shadows project strangely onto the puddle
reflections. `hideInRain` fades them out over `rainFadeSeconds` (2) and keeps them out while the
roads are wet, because the puddles outlast the rain. They start back once VCSWater's lagged wetness
drops under `rainShadowsReturnWetness` (0.30, about 10% of a road still wet) and are fully back at
half of it (2%). It was 0.1 down to 0, reported as shadows returning only once every puddle had gone.
With the water pass off it
goes by the game's rain level alone. Fully faded, `OnFlush` skips all three passes but the capture
carries on, so the caster cache is warm when they come back. A skipped frame is not a BLINK; the log
says `off for the rain` and `back, the roads are dry` at NOTICE; both knobs are on the Shadows tab.

### Water, and rain on the roads

**Status: shipped.** The sea is shaded rather than flat - moving normals, a Fresnel blend into a
reflection of the frame, and sun glint - and when the game says it is raining, the roads go wet and
catch droplets. One module, `GPU/Common/VCSWater.cpp`, one compat flag (`VCSWaterQuality`), one
player-facing row (Graphics → Water quality: LOW off, MEDIUM sea, HIGH sea and wet roads) and one
debugger tab.

The two halves are one feature because they are one pass. Both need the frame's geometry in world
space, both need the frame's own colour to reflect, and both composite at the 3D→2D seam - so the
second costs very little once the first is on.

#### The sea was identified by what the game submits, not by guessing

There is no "this is water" bit in a display list, so the first thing built was a probe that writes
one frame's draw table to the log (`VCS_WATER_PROBE=<frame>`). Standing on the grass at the west end
of the Downtown bridge, the frame is 451 3D draws and the last sixteen of them are the sea:

| what | count | span | z | normals | colours | lighting |
|---|---|---|---|---|---|---|
| far sectors, one quad each | 5 | 64 | **5.50 exactly** | no | no | off |
| near sectors, the game's own wave mesh | 8 | 32 | 5.86–6.12 | yes | no | off |
| a larger flat piece | 1 | 128 | 5.50 | no | yes | off |
| the map-spanning skirt | 1 | 2048 | 2.50–5.00 | no | yes | off |

All sixteen share one texture, and that is what the classifier keys on. The **learner** looks for
the first row of that table - a texture carrying three or more perfectly flat, unlit, normal-free,
colour-free quads of at least 24 units, all at the same z - and takes its address and its height.
The address is a heap address, so it is learned every session and forgotten when the camera jumps,
exactly as `VCSShadow` learns the blob-shadow textures.

Two things fell out of that measurement for free, and both are used: the sea is a **plane at a known
height**, and the game applies **no hardware lighting to it at all**. There is nothing to preserve
except the colour.

#### Four passes, and why there is a copy of the frame

1. **A copy of the frame** the game has just finished drawing, into a scratch framebuffer.
2. **A depth pre-pass** over the captured geometry, water and solid alike, through the camera's own
   transform. This is the whole of the occlusion: a pier, a boat or the player in front of the sea
   wins those pixels and nothing is painted over them.
3. **One shading pass, drawn twice with the same shader** - once over the solid geometry for road
   wetness, once over the water - with the depth test set to `EQUAL` so only the surface that won
   the pre-pass is shaded. One shader for both is not tidiness: `EQUAL` is only safe because the
   two passes run *the same vertex shader* with the same uniforms.
4. **One triangle** compositing the result over the game's framebuffer, premultiplied, so a pixel
   the pass did not touch is left bit-for-bit as the game drew it.

**The copy is what makes any of this possible**, and it is worth being clear about why. A fragment
shader cannot read the framebuffer it is writing, so without a copy the water could only ever be
painted a colour of its own - which is wrong at sunset, wrong in fog and wrong in the rain. Starting
from the pixel the game drew and moving it towards `deepColour` by `deepMix` keeps VCS's own time of
day for free, and hands the reflection something real to sample.

**The reflection is the frame, mirrored about the horizon.** That is exact for a surface at eye
height and increasingly wrong for one below it, which is the right way round: what a sea reflects is
the sky and the far bank, both effectively at infinity. A road at your feet given the same mirror
comes back with palm trees standing full height in it and reads as a *flood* - reported exactly that
way from the first build - so a road gets the same approximation with the mirror compressed
(`wetMirrorScale`, 0.35). Everything a screen-space ray march would need is already here except a
depth texture, and that is the upgrade path if this is ever not enough.

Reconstructing world position from the GAME's depth buffer instead of drawing the geometry a second
time was rejected for the reason `VCSShadow` rejected it: PPSSPP rewrites `gl_Position.z` on its way
out, differently by backend and render state, and replicating a moving target would break in ways
that look like water bugs.

#### The waves are a sum of sines, and the fade is not an optimisation

Three layers of sine gradient, from world XY, six cosines a pixel. The gradient of a sum of sines
*is* the normal up to the amplitude, so there is no normal map to project, scale or wrap across a
surface 2048 units across. Past a few hundred units the normal changes faster than a pixel and the
sea turns to sparkling noise, which is worse than a flat mirror - hence `detailFade`.

The near sectors already carry the game's own wave *mesh*; this is the detail on top of it, which is
the part 32 units of vertices cannot express.

#### The roads come from the game's own traffic network

`ThePaths` - the graph the AI drives on, already parsed for the GPS (see `VCSRoute.h`) - is
rasterised into a top-down mask around the camera and sampled by world XY in the fragment shader.
One texture fetch a pixel instead of a loop over thousands of segments, rebuilt only when the camera
leaves the middle eighth of it.

Three things had to be measured before that worked, and each of them was a wrong assumption first:

**1. The space the GE is fed is NOT the game's world space.** The probe put the recovered camera at
`(14.0, -31.4, 9.6)` while the player stood at `(236.3, -133.6, 8.1)`. It is a translation, it is
**XY only** - the two Z values agree to the last digit in every sample - and it is *not constant*:
one session held `(225.0, -105.6)` and another `(287.7, -213.6)`. So it is derived live rather than
learned, from `CCam[0]+0x20` (the camera in world space) minus the camera recovered from the view
matrix. That pair is printed side by side on the Water tab; if the Z halves ever stop agreeing, the
rebase has stopped being a translation and the mask will be in the wrong place.

**2. A path node's Z is one byte and it is NOT scaled.** `VCSRoute` divided it by 8 like x and y,
which put the entire road network between 0.6 and 3.2 units - *underneath the sea*, which sits at
5.50 - and one byte at that scale cannot reach a bridge deck. Measured: the road nodes nearest a
player standing at world z 12.77 hold raw bytes of 10, 11 and 13, and the whole road graph spans
5..26. **This was a real bug in shipped code**, invisible because routing is a 2D search and the
radar draws a 2D line; it was found because a wet-road pass came out dry everywhere and the height
term said why.

**3. The mask needs a height channel, or a fence gets rained on.** A top-down mask knows nothing
about the third dimension, so a fence rail, a balcony or a rooftop over a road is "on a road" -
reported as pink fence rails over a canal. The second byte carries the road's own height, and the
shader wants the surface within a few units of it. The window is deliberately generous: a car roof
is a metre and a half up and *should* be wet.

**The graph's width is not the road's width.** A two-way street is two parallel lines of nodes about
eight metres apart and each gets `roadHalfWidth`, so seven metres - which is what a street half-width
measures - produced a mask covering **26.6%** of a 512-metre square and bleeding twenty metres past
the kerb into the canal. Four metres is a street.

#### The droplets

One expanding ring per grid cell, the cell's phase hashed from its coordinates so the impacts are
scattered rather than in step, keyed off WORLD position so a puddle stays where it is while the
camera moves. The ring is a wavelet - a sine damped by distance from the ring's radius - and its
gradient is the normal perturbation. Nine cells, and every one of them is behind the wetness test:
a dry frame pays for a compare, not for nine hashes.

**A tilted normal alone shows from one angle only**, which is how it was reported: "the rain
droplets are visible only in a certain camera angle". The normal reaches the road's colour through
two terms and both are angle-bound - the reflection is weighted by Fresnel, about two per cent
looking down at your feet, and the highlight needs the sun lined up behind the ring. So the rings
showed along the road at a low angle or into the glint, and nowhere else.

Each ring's crest now carries a brightness of its own as well (`rippleLight`, "Droplet brightness"
on the Water tab, riding in `u_wet4.w`): the positive half of the wavelet, lit by the game's own
pixel and the reflection so it follows the time of day and is dim at night, and scaled by
`1 - fres` so a low angle - where the reflection already shows the ring through the normal - does
not get it twice. `rippleStrength` still only tilts the normal.

The same pass fixed the "Droplets per unit" slider, which ended at 2.0 against a default of 2.9 and
so clamped the setting the moment it was touched.

#### The weather is the game's own, and it came out of the script command table

`0166 store_weather` and `0167 restore_weather` name the whole block: their handlers copy exactly
four values into and out of a save slot, which is the shape of VC's `CWeather::StoreWeatherState`.
`0109 force_weather_now` then writes its argument to *both* s16s, which separates Old from New, and
`010A release_weather` writes -1 to the forced slot. Twenty minutes, offline, with nothing running.
`WeatherRain` (gp+0x1ed8) is the only one the renderer reads.

Rain is the one value multiplied every frame by a pass that covers the screen, so `ReadWeather`
clamps it to 0..1 and treats NaN as dry: a wrong address shows dry roads rather than a white screen.

#### Two dev hatches, and why they are environment variables

```
VCS_WATER_PROBE=<host frame>   dump that frame's draw table, and log the camera in both spaces
VCS_WATER_RAIN=<0..1>          force the rain level
VCS_WATER_DEBUG=<0..4>         pick a debug view
```

All three have to be set *before* the frame they affect, from a command line, with nobody at the
keyboard - which is what an environment variable is for and a settings row is not. The last two set
the ordinary `rainOverride` and `debugView` settings, so the menu and the hatch are the same lever.

While the probe is armed, the camera in both spaces and the mask value under the player's own feet
are logged once a second. That second line is the whole wetness calculation short of the surface
normal, and it is what answered "the road is dry and I do not know why" in one run rather than by
staring at screenshots.

#### Rain does not wet a road instantly, and the roads say so

The wetness the puddles follow is a **lagged** copy of the game's rain, with two different rates:
`wetSeconds` (30) to soak and `drySeconds` (90) to dry. Linear rather than exponential on purpose -
"thirty seconds to soak" is a sentence a player can check with a stopwatch, and an exponential's
time constant is not. Measured over a driven cycle: forced rain on, then off, and the wetness came
down 0.200 → 0.178 → 0.155 → … → 0.000 at exactly 1/90 per second.

**A STEP in the rain skips the ramp.** `Rain` follows a forced weather inside half a second - see
the measurement below - so a jump of more than 0.4 in one frame is not weather arriving, it is a
decision: the **RAINY WEATHER cheat**, or a mission script. The ramp models water accumulating from
rain that has been *falling*; a discontinuity means that premise never held. Upwards only: switching
the rain off does not mop the road, and watching the puddles drain is the half worth having.

So the in-game trigger is the cheat that already exists, with no coupling between the cheat layer
and the renderer at all - `VCSCheats` still knows nothing but which buttons to press. `Soak now`
and `Dry now` on the debugger tab are the same thing for testing.

#### Rain is 0.5 at its heaviest, and that is a measurement

Forcing each weather id in turn over the WebSocket debugger and reading `CWeather::Rain` back:

| forced id | 0 | 1 | **2** | 3 | 4 | **5** | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| Rain | 0 | 0 | **0.500** | 0 | 0 | **0.500** | 0 | 0 | 0 | 0 |

Two things fall out. Ids **2 and 5 are the wet ones**, and the maximum is **0.5, not 1.0** - so
everything here divides by `kRainFullScale` and works in a normalised 0..1. Without that the whole
feature ran at half strength in real weather with nothing to say why. And Rain reached its value
inside the first half-second sample every time, with no ramp, which is what makes the step detection
above both possible and necessary.

#### Where the water goes as it dries

Two things happen at once, and neither alone looks like drying:

- the wet **band** retreats towards wherever that stretch of road drains, and
- what is left **breaks up** into patches.

Which way a stretch drains is hashed from world position on a coarse grid (`drainPatchSize`, 24
units), smoothly, so one street does not drain the same way for its whole length and there is no
seam where the cells meet. `drain` is 0 at the crown and 1 at the kerb, and the band is the part of
the road within `wetness` of it. The break-up is a two-octave value noise thresholded against the
same wetness, and a `wetness³` floor keeps a road that is *actually being rained on* uniformly wet
rather than covered in puddles with dry tarmac between them.

**All of that is a pure function of (world x, world y, wetness), so it was tuned off the GPU.**
`Tools/vcspuddles.py` renders the same formula at six wetness levels. That is a far better
instrument than a screenshot here: the game decides where the camera is, drying takes ninety
seconds, and a still frame cannot show a sequence anyway. Two things came out of it that would have
been slow to find in game:

**One octave of noise is a stripe, not a puddle.** Median length of a wet run along the road at half
wetness: **25.6 m** with one octave, **7.7 m** with two. A 25-metre puddle is a stripe painted down
the road.

**And the gutter puddles were being erased by the gutter.** `across` - where a pixel sits between
the crown and the kerb - was normalised over the whole of `reach`, which includes the soft shoulder.
So `across = 1`, the place drying water is supposed to end up, sat out in the shoulder where the
kerb fade had already taken it to nothing. Normalising over the *drivable* half width instead more
than doubled what survives late in the dry: wet area at wetness 0.30 went from **4.8% to 10.2%**,
and at 0.15 from **0.0% to 2.1%**. The bug is worth recognising again in general form: a coordinate
normalised over the wrong extent is not wrong everywhere, it is wrong exactly at one end.

#### The droplets stop when the rain does

The rings follow a separate, barely-lagged copy of the live rain (`s_rippleRain`, 1.5 s), not the
wetness. "It has stopped raining and the road is still wet" is precisely the state in which rings
would be wrong - nothing is landing on it. The 1.5 s is so a forced weather change fades them rather
than cutting one off mid-ring.

#### The sea was too reflective, and the number says why it was not the Fresnel's fault

Reported as looking like a straight-up mirror. The instinct - turn the reflection down - would have
been guessing, so the distribution was measured instead: `MeasureReflection` walks the frame's own
captured water vertices and computes the flat-water Fresnel at each.

```
raw p10 0.278   p50 0.682   p90 0.967   |   74.4% of the visible sea above 0.5
```

**Three quarters of the sea is physically more mirror than water**, and that is correct: a camera a
few units above the surface sees most of the water at a grazing angle, and the Schlick term with
F0 = 0.02 is right about what water does there. So the problem was never the Fresnel curve. It was
two other things:

- **A single tap is a sheet of glass.** Real water at any distance is roughened by capillary waves
  smaller than a pixel and hands back the average over all of them. The reflection now takes five
  taps, spread wider with distance - which is also exactly where the Fresnel term is highest.
- **`reflectionStrength` was 0.85**, which is a near-unity scale on a curve that already reaches 1.

Set from the measurement rather than by eye, at 0.45 with a 0.50 ceiling:

| | p10 | p50 | p90 | raw needed to be half mirror |
|---|---|---|---|---|
| was (0.85, no cap) | 0.236 | **0.580** | 0.822 | 0.59 |
| now (0.45, cap 0.50) | 0.125 | **0.307** | 0.435 | 1.11 — unreachable |

So nothing on screen can now be more than 44% reflection, the median is halved, and the Fresnel
gradient survives intact because the ceiling never actually bites. The tab reports both numbers
live, so this is checkable rather than remembered.


#### The whole feature was dead for a build, and the reason is one identifier

`patch` is a **reserved keyword in GLSL 4.50** - it is a tessellation qualifier - so `float patch`
is a syntax error. The shading fragment shader stopped compiling, `s_setupFailed` latched, and
every pass after it drew nothing. Water AND roads, silently, for a whole round of work.

Two things made it survive longer than it should have:

- **glslang's message goes out at `WARN_LOG`** (thin3d_vulkan.cpp's `VKShaderModule::Compile`),
  and this build's `G3DLevel = 2` drops WARNING while passing NOTICE and ERROR. Only the bare
  `Failed to compile shader vcs_water_shade_fs:` line at ERROR reached the log, with the source
  but no reason. Raise `G3DLevel` to 4 to see the actual error, and put it back afterwards.
- **Nothing said the passes had not run.** Absence of an error was being read as success. The
  probe now prints `PASSES ran` / `PASSES FAILED TO SET UP` once a second, positively, which is
  the line to look for first after any change to this shader.

The general shape, and it is worth recognising again: a latched setup failure is invisible from
the outside and looks exactly like a feature that is switched off. **Log what DID happen, not
just what went wrong.**

#### And the sea still looked untouched at range, for three compounding reasons

Reported as "sea looks like the og one" once the shader compiled again. Arithmetic, not opinion:

1. **The distance fade was killing the waves.** `g` is a SLOPE, so it was fading every octave -
   normal tilt went from 8.3 degrees at 30 metres to 0.9 degrees at 300. Most of the time you see
   this sea it is hundreds of metres away, so the waves existed almost nowhere. A **swell** octave
   at a sixth of the frequency now runs unfaded; a long wavelength costs nothing in steepness and
   is never smaller than a pixel, so it cannot alias, which is the only thing the fade was for.
2. **The reflection had been over-cut.** Reducing `reflectionStrength` to 0.45 took the median
   reflection to 0.22 - and over open sea the reflection is of the SKY, which is nearly the
   water's own colour, so at that weight the term is a no-op. The fix for "it looks like a
   mirror" was the five-tap blur, not the strength; a single tap is a sheet of glass whatever it
   is scaled by. Back to 0.70 with the 0.62 ceiling.
3. **The deep colour was a filter, not depth.** Mixing a fixed 30% of `deepColour` everywhere
   just tints the sea darker. Scaled by `(1 - fres)` it becomes the other half of the Fresnel -
   dark where you look INTO the water, pale where you see sky off it - which is the strongest
   "this is water" cue over open sea after the sun glitter.

And the glint lobe went from `pow(..., 120)` to 55 at nearly double the strength, because at 120
it only existed when the sun was almost dead ahead.

#### Wet roads flickered at chunk swaps, and the shadows had already been through the causes

Reported from play: driving in the rain, the puddles and the fully wet road flicker the way the
shadows used to, at chunk loads. VCSWater copies VCSShadow's camera code by design (see the top of
the file) and had fallen behind it on two of the chunk-swap fixes:

- **A rebase was treated as a load.** Any recovered-camera jump over 40 units forgot the sea texture
  and dropped the road map, and the wet pass then stays off until a rebuild finishes - about eight
  frames. The map is in world space and the offset is re-derived at every seam, so a rebase
  invalidates neither, and the log already showed the same sea address relearned within a frame of
  every "forgetting". Rebases are now told apart as VCSShadow does, by the game's world-space camera
  staying put, and only a real jump forgets.
- **No view correction.** Draws carrying the new space's camera were baked a whole cell away.
  `PrepareViewCorrection` is copied across.
Both log at NOTICE (`rebased by`, `moved into the frame's space`) and the Water tab counts both. A
driven run - the player's Y stepped a unit every 50 ms over the WebSocket debugger - logged six rebases
followed, each a whole cell step, and corrections of exactly one cell.

**VCSShadow's third fix, the re-capture, was copied too and taken back out.** Reported from play at
once: while it rained, the sea shader covered the whole road. A logged run in real rain (weather 2
written to Old, New and Forced over the debugger) settled it: 25-30 restarts a second, each after a 2D
draw in the MIDDLE of the frame (texture `09715bd0` from the moment the rain was on screen), with 35k to
120k solid indices already captured and a small depth-writing 3D draw arriving behind it. A rain frame
is one frame with a 2D draw inside it, not a second frame, so the restart threw the road away and ran
the passes again on what was left - and the sea plane under the city, with nothing captured in front
of it, won every road pixel. The same run logged no view correction off the cell grid, which cleared
the other new change. VCSShadow still re-captures; in rain it is hidden by `hideInRain`, but its passes
will meet the same split frame if that ever changes.

#### Testing this needs the player put somewhere, and there is only one lever

The boot autoload decides where the player stands and **overrides `--state`**, so three rounds of
screenshots came back from inside a shop while the notes claimed the sea had been checked. The way
out is to write `PlayerBase + 0x30/34/38` over the WebSocket debugger and wait about twelve seconds
for the streamer - the benchmark spot at `150.2, -670.8, 13.0` looks straight across the water.
Anything claiming to have verified how this looks should say where it was standing.
#### Four reports from play, and what each one turned out to be

**The puddles swam while driving.** The offset that crosses from the space the GE is fed into the
game's own world space was derived in `BeginFrame` by pairing the camera it had just read out of
PSP memory with the camera recovered from the PREVIOUS frame's view matrix. At driving speed those
are a whole frame of travel apart, so the offset wobbled by however far the car had moved - and the
puddles are a noise field sampled at (GE position + offset), so the entire field moved with it every
frame. Derived at the seam now, where both halves come from the same frame, and latched on top:
measured residual went from a frame of travel to **0.001-0.003 units**.

The latch is worth keeping separately from the pairing. A rebase is a step, not a drift - it holds
for many seconds and then jumps - so anything under `kOffsetLatch` is noise, and moving a
world-anchored field for noise can only make it shimmer.

**The road mask was a 2.8 ms spike every couple of seconds.** It recentres when the camera leaves
the middle eighth of the map, which while driving is every two seconds or so, and it ran in one go
on the emu thread against a 33 ms budget. It is sliced across frames now - it writes a private
buffer and swaps it in only when finished, so the live mask stays correct and merely a little
stale, and the map reaches 256 units against a 64-unit trigger so there is four times the slack a
build needs.

Budgeting it took three attempts and the two failures are the interesting part. **Nodes** were
wrong: cost per node runs from a bounding-box rejection to hundreds of rasterised texels, and
slices came out anywhere between 0.19 and 1.32 ms. **Texels** were wrong too, and more
instructively - most of a build is the ~10000 segment REJECTIONS, not the ~25000 texels that
survive them, so a texel budget let thousands of nodes through for free and put most of the build
back into one frame. Budgeting against the clock works because the clock is the thing being
budgeted. Now ~0.8 ms total at =< 0.35 ms a frame.

A separate 47 ms spike in the same place turned out to be `VCS::EnsureGraph` re-reading ~3000 nodes
and ~17000 links when the level changes. That is once per island, during a load, and is left alone.

**Shadows flickered while driving.** Counted rather than described: a BLINK is a frame where the
composite ran last frame and not this one, which is exactly what the player sees. Two mechanisms,
and the count separated them.

The first was the **sun**. It is read off whatever draw hands the hardware an enabled directional
light, and this game's scenery is prelit and unlit - so the lit draws are the traffic and the
pedestrians, and a frame containing neither has no sun at all. `ComputeShadowView` gave up, and
every shadow on screen vanished for that frame. Measured: five such frames in a forty-second run.
The sun is held now; one that is a frame or two stale differs by a few thousandths of a degree.

With that closed, the remaining blinks were **all** attributed to an empty caster set - a frame
where nothing at all qualified as a caster, which happens while a chunk swaps. The depth map from
the last frame that had something in it is reused for up to `kMaxHeldMapFrames`, which takes the
count from **seven to one** under teleport stress and to **zero** in ordinary play. What goes stale
is only the casters; the receivers are this frame's geometry, so shadows still land on the ground
that is actually there. The camera matrix is emphatically NOT held - the mask is screen space and
the camera has moved - so the two are kept apart: fresh `cameraViewProj`, held light matrices.
Mixing them puts the right shadows in the wrong places.

**The vanilla blob sprite flashed on every chunk load.** Suppression waited for a texture to be
LEARNED, which takes `kBlobSightingsToLearn` frames - and every texture address moves when a chunk
streams in, so each load showed the game's own blob under the player until the learner caught up.
The positional test already knows the answer for the draw in front of it on the first frame it sees
it; the learned set only ever needed to carry that knowledge to frames where whatever stands over
the blob did not reach the capture. Suppressing immediately and learning in parallel: measured over
a session, **28461 blobs hidden by position against 10621 by learned texture**.

#### What is NOT verified

- **Bridges and flyovers.** Where two roads cross at different heights the mask keeps the height of
  whichever covers the texel more strongly. One byte cannot hold both. Driving over the Downtown
  bridge in the rain is the test.
- **Interiors.** Nothing tests for being indoors, so a garage floor at a road's XY and height would
  go wet.
- **The sea drawing over the player's model.** Reported: looking at the character's FRONT with
  water behind him, the water layer overlaps parts of him in some situations. Deferred rather than
  fixed. What has been ruled out: people do reach the depth pre-pass - 20 skinned draws seen, 20
  captured, none rejected - so it is not the caster filter eating them. The next thing to try is
  `Tools`-side camera control (write `CameraYaw`, sweep it round the player at the water's edge)
  with debug view 4, which paints every pixel the water pass claims.
- **How the puddles look on a real road at mid-wetness.** The formula was verified off the GPU at
  six wetness levels and the timing numerically in play, and a fully wet road was photographed -
  but the half-dry state in game was not. Soak now / Dry now on the Water tab is a ten-second
  check for anyone with a keyboard.
- **Cost.** The passes were never profiled. The geometry is captured a second time (`VCSShadow`
  captures its own), which is one vertex transform per vertex on the CPU, and the shading pass draws
  the scene twice more. Nothing was measured, so nothing is claimed.

### The hunt for the visibility function, and the frustum that decides nothing

**Status: the lever was found, verified, measured, and removed. The question is still open.** What
follows is the whole of it, because the next attempt should start from the two things that are
ruled out rather than rediscover them.

VCS culls hard to the camera, which is how a PSP ran it, and the shadow pass can only ever see what
reaches the GPU. So the standing request - *"trick the game that I am always looking 360 degrees"* -
is a request to widen whatever the game tests against before it draws.

**What was found.** A memory WRITE breakpoint on `CCamera + 0xAB0` (`0x08BC88E0`) breaks inside
`0x08a1db40`, which is called once a frame from exactly one place, `0x08a23e84`. It reads a culling
FOV out of `gp - 0x4250` (`0x08BADB10`, reads 70.0), converts to radians, **halves it with a
`lui $a0, 0x3F00` at `0x08a1dbac`** - the float 0.5, whose low sixteen bits are implicitly zero -
and builds four camera-space plane normals from the sine and cosine of the result:

```
+0xAB0  ( cos t, -sin t, 0)     t = 35.00 deg horizontally
+0xAC0  (-cos t, -sin t, 0)
+0xAD0  (0, -sin u,  cos u)     u = 21.87 deg, the same angle narrowed by the aspect
+0xAE0  (0, -sin u, -cos u)
```

**The patch works, and it is one 16-bit field.** Scaling that one immediate scales the half-angle,
and the vertical pair is derived from the same register afterwards, so both axes move together.
Verified in both directions against the planes themselves: 1.8x gave 62.89 deg horizontally and
39.31 deg vertically, and 0.125x gave 4.37 deg. The projection matrix is built elsewhere, so
nothing about the picture changes either way.

**And it decides nothing.** Two measurements, and the second is the one that ends the argument:

- Booted twice into the SAME savestate, so the scene is identical rather than merely similar:
  **1445 draws / 166,762 vertices at 35 deg against 1469 / 168,743 at 62.89 deg**, twenty samples
  each, medians. The two sequences interleave - 1415, 1444, 1445, 1469, 1481, 1518 appear in both -
  which is what a scene playing out identically looks like.
- Narrowed to **4.37 deg** - a slit - and the screen still draws the whole street: buildings, palms,
  traffic, out to both edges. Done on the INTERPRETER, because a raw `memory.write` of an
  instruction is only live there; under the JIT the compiled block holds a stale copy of the
  constant and the write appears to do nothing.

**Nothing reads those planes either.** A read breakpoint on `0x08BC88E0` sat silent through 16
seconds of ordinary play, while the same kind of breakpoint on `TimeStep` fired instantly - the
control that makes the silence evidence rather than a broken tool. So the function computes a
frustum every frame and stores it, and the renderer consults something else.

**Two things that had to be checked before believing any of that.** The FOV global really is the
input and the function really does run: writing 20.0 into `0x08BADB10` changes plane 0 within one
frame, and the game puts 70.0 back inside 100ms. And the emulator being measured has to be the one
that was built - three runs went into a `PPSSPPWindows64.exe` at the repo root that was a week old,
reporting a patch that had never been compiled into it. **The Release build this fork runs is
`GTA Vice City Stories.exe`**, and `PPSSPPDebug64.exe` beside it; check the timestamp against the
source before trusting a measurement.

**The second candidate, also ruled out.** The retail build kept its own debug command names, in
pairs of (name pointer, handler pointer) - the registry around `0x08b7d3c8` is how to find any of
them, and it is worth remembering as a general tool. `IsSphereOnScreen` (`0x08b7d34c`) resolves to
handler `0x08932a80`, which walks a list and calls **`0x08824354`** - a real sphere-versus-frustum
test that uses `CDraw::ms_fFarClipZ` (`gp + 0x1e74`) as its default range and culls against
`camera + 0xd0 / +0xd4` horizontally and `+0xd8 / +0xdc` vertically, the same `(cos, -sin)` shape.
**That function is not called during rendering either**: an execution breakpoint on it never fired
in twelve seconds on the interpreter, while the same breakpoint on `0x08a1db40` fired at once.

**Where to look next, in the order they are worth trying.**

- **`CCamera + 0x7bc` holds the render camera.** At `0x08a23ea0` the caller fetches it and copies
  the camera's position (`+0x30..0x38`) and basis (`+0x10..0x28`) into it. That object is what the
  renderer actually draws through, and whatever it culls with is reachable from there.
- **Find the visible-entity list rather than the test.** In this engine lineage the scan fills a
  large array of entity pointers each frame. An array of many consecutive pointers into the entity
  heap that changes as the camera turns is findable with `memory.search`, and a write breakpoint on
  it lands inside the scan - the same move that found the frustum function, aimed at the right
  structure this time.
- **Consider that there may be no per-entity frustum cull at all.** A 4.37 degree frustum drawing a
  complete street is at least consistent with VCS drawing everything the streamer has loaded,
  regardless of facing, and limiting itself by distance alone. If that is what it does, the shadow
  complaint has a different cause than the one everybody has assumed, and the test is one honest
  measurement: the draw count with the camera pointed two opposite ways from one spot. **Turning the
  camera for that test is itself the hard part** - writing `CameraYaw` from the debugger moved the
  look vector by 0.0 degrees across 361 writes, because on foot mode 15 rebuilds it from the
  player's heading every frame, and `input.analog.send` goes straight to `__CtrlSetAnalogXY` and so
  never reaches this fork's own look path.

**One trap worth fixing wherever else it applies.** The first version of the patch wrote the
instruction whenever the setting moved and then trusted its own bookkeeping. Loading a savestate
replaces PSP RAM wholesale, so the patch goes with it while every variable still says it is
installed - the frustum silently reverted and nothing said so. The fix is to compare against what is
AT the address rather than against what was last written, which costs one instruction read a tick.
**The fire hook, the climb-splash hook and the map cursor all have the same exposure** and none of
them has been checked against a savestate load.

### The streaming grid, and the second lever that turned out to be inert

**Status: decoded, instrumented, measured, and the knob removed. What limits the world is still
open, but two more mechanisms are now ruled out with numbers rather than argument.**

**The grid.** VCS divides the map into **50 x 50 sectors of 80 world units**, and turns a position
into a sector index as `(int)(x / 80 + 30)` and `(int)(y / 80 + 25)`, clamped to 0..49 - so the
world runs -2400..1600 in x and -2000..2000 in y. The sector array is `[gp - 0x6398]`, 56 bytes a
sector, row stride 0xaf0, and each sector carries six entity lists at `+0x00, +0x04, +0x0c, +0x10,
+0x30, +0x34`. Those three constants (80.0, 30.0, 25.0) are the signature to search for when
looking for anything that walks this grid; 58 functions carry at least two of them.

**The streamer's public API is 51 thin wrappers** that load the manager from `[gp - 0x298]` and
tail-call a method - a gp-relative scan finds all of them at once, and that list IS the surface.
`CStreaming::Update` is `0x08ad3e60`, reached once a frame through the wrapper at `0x08ad35d0`, and
it does four things: `0x08ad6158`, `0x08ad62dc` (special models, then the zone streamer at
`0x08ad78dc`), `0x08ad63bc` (the level/interior models, through `0x08809600`), and `0x08ad49ac`,
the loading channel. **None of those four walks the sector grid**, which is the surprise: buildings
are not streamed by a sector scan each frame.

**`0x08ad867c` is `CStreaming::DeleteRwObjectsBehindCamera`,** and it is the only thing in the
streamer that reads the camera's direction. It takes `|forward.x|` against `|forward.y|` from the
camera matrix at `CCamera + 0x00` to pick a dominant axis, then frees every sector from **2 to 10
behind** the camera, across a band 10 sectors wide either side. Two sectors is 160 units, about a
street. It is reached from `CStreaming::Update` through `0x08ad4040` -> `0x08ad6070`, which asks
the allocator at `0x08bc7cf0` for free space and only calls it when there is not enough.

Get the branch polarity right if this is ever revisited: at `0x08ad87d0` the test is
`forward.x <= 0`, and the FALL-THROUGH (facing +x) scans `sectorX - 10 .. sectorX - 2`. Behind, not
ahead. The first reading of this had it backwards and turned a memory-reclaim pass into an
imagined "load ahead" wedge; the whole interpretation followed from one `bc1t`.

**The lever was four 16-bit immediates**, at `0x08ad87f8`, `0x08ad8818`, `0x08ad8c04` and
`0x08ad8c28` - the `+/-2` near edge, one per branch of the two axis-major cases. Raising the
magnitude to N keeps N sectors of world behind the camera; at 10 the near edge meets the far edge,
the loop that walks the band never runs, and nothing behind is freed this way at all. Only the
magnitude is ours to move: the sign says which way "behind" runs on that branch.

**And it never runs.** A `REPFLAG_HOOKENTER` hook was installed on the function purely to count
calls, and it stayed at zero through a full tour of the map - nine teleports across Vice City,
each forcing a fresh area to stream in, with draw counts visibly collapsing and rebuilding at every
stop. The controls that make that silence evidence: the hook really was installed (the disassembler
shows `* replacement:` at the entry while the game runs), and the log channel really does carry the
message (it was moved to ERROR after WARN turned out to be filtered - `SystemLevel = 2` in
`ppsspp.ini` is ERROR-only, which this file has now recorded three times). So the streamer is never
short enough of memory to reach for this, and a setting that moves its threshold is a setting for
something that does not happen. Removed, for the same reason the draw-distance rows and the culling
slider were.

**What that leaves.** Nothing culls the world by view direction (the frustum section above) and
nothing frees it behind you (this one), so what is drawn is bounded by distance alone - which
points at the **per-model draw distances** at model-info `+0x2c`, `+0x30` and `+0x34`, the one lever
from the draw-distance port that was never actually pulled. That is where to go next for "render
much more", and the far clip is NOT it: raising that alone was measured to change nothing, because
it only governs how far the game is willing to draw rather than what exists to be drawn.

**Method note, because it is the third time.** Two mechanisms in two sessions were found, decoded
correctly, and turned out inert - and in both cases the thing that settled it was an instrument
rather than an argument: a slit frustum that still drew the whole street, and a call counter that
stayed at zero. Building the counter cost less than the reasoning it replaced. When the question is
"does this code path matter", the cheapest honest answer is usually to make the game say so.

**Two tooling limits worth knowing before planning any measurement here.** The camera cannot be
turned from outside: writing `CameraYaw` moved the look vector by 0.0 degrees across 361 writes
because mode 15 rebuilds it from the player's heading every frame, writing `PedHeading` turns the
player without the camera following while they stand still, `input.analog.send` goes straight to
`__CtrlSetAnalogXY` and never reaches this fork's own look path, and holding the stick long enough
to swing the view runs the player two hundred metres down the road. Any "does facing matter"
measurement therefore has to be made by somebody at the keyboard. And walking does not cross a
sector - a sector is 80 units and the player covers about two a second on foot - so a streaming
test needs a teleport, not a walk.

### The per-model draw distances are real, and moving them changes nothing

**Status: measured and ruled out.** This was the one lever the draw-distance port never actually
pulled, flagged in that section as where to start if the question came back. It came back - a
building is assembled from parts, its parts stop drawing at different distances, so its shadow comes
apart before the building does - and the lever is dead.

**The table is exactly where the port said.** `[gp + 24]` is the model-info pointer array and
`[gp + 7656]` its length: 7941 slots, 4577 filled on an ordinary street. Each info carries a type at
`+0x10` and three floats at `+0x2c`, `+0x30`, `+0x34`, and reading them back confirms they are
distances rather than anything else - **the histogram of the first one is 50, 100, 150, 300, and a
handful at 2000**, which is a draw-distance ladder and could hardly be anything else. 4255 entries
are type 1 or 3, the two map-object types.

**And scaling them does nothing.** Every one of those 4255 objects had its three distances
multiplied, live over the debugger, and the draw count was sampled from one settled scene at each
factor:

| factor | draws | vertices |
|---|---|---|
| 1.0 | 996 | 127,837 |
| 0.2 | 1062 | 118,065 |
| 1.0 | 726 | 98,361 |
| 6.0 | 767 | 99,764 |
| 20.0 | 808 | 103,469 |

A hundredfold range, and the two readings of the SAME factor differ by more than any pair of
different ones - the scene's own drift, as traffic and streaming move underneath, is larger than the
signal. Shrinking to a fifth does not gut the world either, which is the control that matters: if
these governed what is drawn, a fifth of the distance would empty the street.

**The likeliest explanation, and where to go next.** An entity almost certainly caches what it needs
from its model info when it is CREATED, so editing the info afterwards reaches nothing that already
exists - which is consistent with everything above and would mean the lever has to act at streaming
time rather than as a pass over the table. That is a hook on entity construction, not a memory write.

**One address in the old port is mislabelled and cost an hour.** `kVCSEntityLodStore`, at
`0x08a2412c`, is described as an entity LOD field. It is not: `$s0` there is `CCamera`, the
instruction is `swc1 $f12, 0x7a0($s0)`, and the surrounding code takes that value straight to
`CDraw::SetFarClipZ` at `0x08a1ad6c`. A read breakpoint on `CCamera + 0x7a0` lands in `0x089c73d8`,
which multiplies it by 40 and by 60 and hands the results to what is plainly a fog or haze setter.
So it is the camera's own draw distance feeding the horizon, and nothing to do with per-entity LOD.

**A first attempt at this measurement was wrong and looked convincing**, which is the method note.
It compared a sample taken seconds after a teleport against one taken later and read the world
finishing streaming as the patch making things worse - a 42% "drop" that was the scene loading in.
Any measurement here has to settle first, keep the originals rather than re-reading them after an
earlier experiment has already written over them, and sweep the factor both ways in one session.

### Nothing reads the model draw distances DURING PLAY - they are read at stream-in

**Superseded in part by the section below**, which caught the read and named the lever.
What follows is still correct about what does NOT happen while the game runs, and about
why the earlier draw-count measurements said nothing.

The section above ruled the per-model draw distances out by measuring draw counts, and that
measurement was weak - the scene's own drift was larger than the signal, and two of the runs turned
out to be confounded by the player falling after a teleport. So it was done again from the other
end, and the answer is much sharper than "no effect".

**The values are real, and they stay written.** Twenty models were set to eight times their draw
distance and read back: **20 of 20 still held the written value after five seconds.** So the game is
not putting them back, and any measurement of "we changed it and nothing happened" is a measurement
of the value we intended.

**And nothing reads them.** A memory READ breakpoint was put on `+0x2c` of three named big buildings
- models whose distances are 700, 900 and 2000, so certainly on screen - for sixteen seconds of live
play each. **None of them was ever read.** The control that makes that silence evidence: a wider
breakpoint on the same structs trips immediately on `+0x3a`, a flags halfword read by the code at
`0x0895bc54` that walks the same `[gp + 24]` table. The structs are in active use; the three floats
in them are not touched.

**Entities do not carry a copy either.** Walking the player's sector through the six lists at
`+0x00, +0x04, +0x0c, +0x10, +0x30, +0x34` gives real entities; each one's model id at `+0x56`
resolves back through the table to its model info. Searching 2 KB of each entity for its own model's
distance found nothing in three of four, and the fourth's match repeated on a 0x220 stride, which is
a neighbouring object of the same kind rather than a cached LOD.

**So the numbers are consumed once, somewhere else** - at model load or instantiation, folded into
whatever the renderer really tests - and editing the table afterwards can never reach anything that
already exists. That is where the next attempt starts: find what is written at load time FROM these
floats, by breaking on a read of `+0x2c` while a new area streams in rather than while standing
still, which is the one condition under which the read must happen.

**A tooling finding that cost most of the session and is not about the game.** The WebSocket
debugger's memory reads are serviced on the CPU thread, so anything that stops the emulator leaves
every read *unanswered* rather than answered late - and the scripts then hang with a traceback
pointing at the read. It happened repeatedly, and the correlation was with the game window being in
the BACKGROUND while a measurement ran; fronting it usually revived the connection, and sometimes
only a restart did. Two more shapes of the same trap: a probe that reported "the CPU is stopped"
when the read had succeeded and merely returned a null pointer, and screenshots that captured the
wrong window because `Process.MainWindowHandle` does not name this game's window - enumerate the
process's top-level windows and take the visible one whose title matches. Before trusting any
measurement here, check that the emulator was actually running while it was taken.

### The draw distance is one global float, and here is where it lives

Breaking on a read of a model's draw distance **while an area streams in** - the one condition under
which the read has to happen - caught it on the first hop. Four hundred models were watched at once,
because there is no way to know which ones a new area will need; forty was not enough and found
nothing, which is worth knowing before repeating this.

**The reader is `0x08aae0ec`**, and it is small enough to quote whole:

```
08aae0ec  lhu   $a2, 0x3a($a0)      ; the model info's flags
08aae0f0  lui   $a3, 0x8BC
08aae0f4  addiu $a3, $a3, 0x7E30    ; CCamera
08aae0f8  andi  $a1, $a2, 0x3
08aae0fc  beq   $a1, $zero, +0x10
08aae100  lwc1  $f12, 0x7A8($a3)    ; <- the global scale
08aae104  andi  $a2, $a2, 0x8
08aae108  beq   $a2, $zero, 0x8AAE128
08aae110  lbu   $a1, 0x38($a0)      ; which of the model's distances to use
08aae114  sll   $a1, $a1, 2
08aae118  addu  $a0, $a0, $a1
08aae11c  lwc1  $f0, 0x28($a0)      ; distance = info[0x28 + idx * 4]
08aae124  mul.s $f0, $f0, $f12      ; ... times the global scale
```

Two things fall out of it. The distances are not "the three floats at +0x2c/+0x30/+0x34" - they are
`info + 0x28 + info[0x38] * 4`, an array with a per-model index, which is why the earlier watch on
+0x2c alone caught some models and not others. And every one of them is multiplied by **one float,
`CCamera + 0x7a8`**, which reads exactly **1.0** in ordinary play.

**That float is the draw distance knob for the entire game.** It is rebuilt every frame: computed at
`0x08a240a0` (`cam[0x7a8] = f30 * f12`), clamped DOWN to a stack local at `0x08a24104`, copied to
`CCamera + 0x7a0` at `0x08a2412c` - which is the value the haze setter at `0x089c73d8` multiplies by
40 and 60 - and multiplied once more at `0x08a2413c`. Writing it from outside therefore does not
hold: hammered from the debugger it reads back 1.0000 every time.

**And the call stack says WHEN it is read**, which is the other half of why every earlier attempt
failed:

```
08aae11c  <- the reader
08a7d7d8 / 08a7f170 / 08a7def4 / 08a7e754 / 08a79dec / 08805480 / 08805334
```

The frames are the world-add path, not the renderer. The distance is consumed **when an entity is
brought into the world**, so editing the model table afterwards reaches nothing that already exists -
and a measurement taken without rebuilding the area is measuring the old numbers.

**The lever, then, is `CCamera + 0x7a8`, and it needs a code patch** because the game rewrites it
every frame. The two candidate sites are the final store at `0x08a24140` and the multiply feeding it
at `0x08a2413c`; the reader's own `lwc1` at `0x08aae100` is a third, if a scratch float can be found
inside CCamera's +/-32KB reach that the game does not also write.

**What could NOT be established, and why - so nobody repeats it.** Whether raising it actually helps
is unmeasured, because nothing in this environment holds still long enough to compare two runs:

- The draw count at one teleported spot read **1131, 749 and 436** across three runs of the same
  build. The signal being looked for is smaller than that.
- The away-and-back trip that rebuilds an area costs **14% of the draw count on its own**, with no
  patch applied at all - the control that shows the -46% and -66% readings from patched runs were
  mostly the trip.
- Two screenshots of "the same spot" came back as a golf course at 07:43 and a hotel street at
  10:52: a teleport to fixed coordinates does not reproduce, because the player falls to whatever
  ground is there and the clock keeps moving.

So the scale was patched - twice, because the first hook reached one reader out of three - and the
section below is what it bought, measured against a savestate rather than a teleport.

### The LOD multiplier was patched properly, and it decides nothing either

**Status: built, hooked at the right place this time, measured against a fixed savestate, and
removed.** The multiplier is real and the patch works; what it governs turned out to be peds and
vehicles, not the map. That makes it the fourth distance mechanism in this file to be found,
decoded, verified and shown to be inert, and the four together now say something much more useful
than any of them said alone - see the verdict at the end.

**The first attempt hooked one reader out of three, and that is worth knowing before hooking
anything.** `0x08aae0ec` (`GetLargestLodDistance`) is not the only function that multiplies a
model's distance by `CCamera + 0x7a8`. Scanning the whole code region for `lwc1` of offset `0x7a8`
and looking for a nearby load of the lodDistance array finds three, in adjacent functions:

```
08aae0a4  lwc1  $f13, 0x7a8($t1)    GetAtomicFromDistance - loops the LOD array and returns the
08aae0a8  lwc1  $f14, 0x2c($a0)     atomic to DRAW, or nil. THIS is the render-time decision.
08aae0ac  mul.s $f14, $f14, $f13
08aae100  lwc1  $f12, 0x7A8($a3)    GetLargestLodDistance - what the streamer asks
08aae194  lwc1  $f14, 0x7a8($a0)    the damaged / last-atomic variant
```

A `REPFLAG_HOOKENTER` hook writes the scaled value when ITS function is entered, so hooking the
middle one reached the middle one and nothing else. Measured at +6.8% of draws for an 8x setting,
which was the stream-in path widening on its own while the renderer went on using 1.0.

**The right place is the write side, one instruction after the game's own final store**, which is
the only point that reaches every reader in the frame:

```
08a240a4  swc1  $f12, 0x7a8($s0)   assigned outright - so nothing can compound across frames
08a24104  swc1  $f28, 0x7a8($s0)   clamped DOWN to a ceiling
08a2412c  swc1  $f12, 0x7a0($s0)   copied to the HAZE, before the last multiply
08a2413c  mul.s $f12, $f12, $f0
08a24140  swc1  $f12, 0x7a8($s0)   the final store
08a24144  lbu   $a0, -0x1ba8($gp)  <- hook here
```

That also puts the scaling past the game's own clamp and leaves the horizon fog exactly where the
artists put it, because the haze was taken two instructions earlier from the unscaled value. Verified
live: `lodMult 8.0000, haze 1.0000` held every frame, against `1.0000 / 1.0000` stock.

**And then the measurement, which is the point of the section.** Same savestate, reloaded for each
run, so the fixture is bit-identical - same clock, same camera (`yaw 0.5336`, `pitch -0.0537`), same
player position, same far clip:

| lodMult | draws | vertices |
|---|---|---|
| 0.05 | 466 | 38,139 |
| 1.0 (stock) | 486 | 40,587 |
| 8.0 | 492 | 42,774 |

**A 160-fold range moves four per cent of the frame.** And the shrink control says what the four per
cent is: at 0.05 the entire skyline, the bridge and the far shore are still drawn, pixel for pixel,
and the thing that vanishes is **the player**. The multiplier governs the models that carry real LOD
ladders - peds and vehicles - and VCS's map is not distance-culled by it at all.

The row is therefore gone, for the same reason the culling slider and the streaming knob went: a
setting that cannot do what its name says does not sit on the Graphics page. `git log
--diff-filter=D -- Core/VCS/VCSDrawDistance.cpp` has the working implementation if it is ever wanted.

**The verdict the four negatives add up to.** Four mechanisms have now been found, decoded, patched
and measured, and every one of them is inert for the map:

| | ruled out by |
|---|---|
| the camera frustum planes at `CCamera + 0xAB0` | a 4.37 degree slit still drew the whole street |
| `DeleteRwObjectsBehindCamera` | a call counter that stayed at zero through a nine-stop map tour |
| `CDraw::ms_fFarClipZ` | doubled, and the view was indistinguishable |
| the LOD distance multiplier | 0.05x to 8x moves 4% of the frame |

**So what VCS draws is what the streamer has loaded, and nothing else decides.** That is not a
hypothesis any more; it is what is left after four levers - and the section below found what
does decide: the streaming heap is 4.75MB and runs 95% full. The note that
`DeleteRwObjectsBehindCamera` never fires reads differently in that light - the streamer is not
evicting because it never gets far enough to have to.

**The savestate fixture is the other thing to keep.** Slot 2 (`ULUS10160_1.03_2.ppst`) is a clear
midday spot on the grass at `150.2, -670.8`, looking across the water at the bridge and the downtown
skyline - a long sightline, nothing moving, the player standing still. Loaded through PPSSPP's own
`ID_FILE_SAVESTATE_SLOT_BASE + n` and `ID_FILE_QUICKSAVESTATE` menu commands posted as `WM_COMMAND`,
it reads **486 draws with a min and max of 486** across twenty samples. Zero variance, and identical
camera between runs.

That is what finally made these numbers mean anything. This file records three earlier rounds where
the same question was asked with teleports and the answers came back 1131 / 749 / 436 for one build,
because a teleport drops the player onto whatever ground is there, the clock keeps moving, and the
follow camera is still settling. **A savestate is the fixture; a teleport is not.** Take one at the
spot the question is about, and reload it for every run.

### The world is 4.75MB, and that is what decides how much city there is to see

**Status: found, patched, measured, shipped as `World memory` on the Graphics page.** After four
distance mechanisms that turned out to decide nothing, this is the one that does - and it is not a
distance at all, it is a memory pool.

**What the game does at boot**, read out of PPSSPP's own kernel log rather than inferred, because
`sceKernelCreateFpl` prints the name and size of every pool:

```
03148700 = sceKernelMaxFreeMemSize()
     330 = sceKernelCreateFpl(MainMemoryManager, size 02c18700)   ; 44 MB
                     ... 95 seconds of boot later ...
  528000 = sceKernelMaxFreeMemSize()
     360 = sceKernelCreateFpl(StreamingHeap,     size 004c1000)   ; 4.75 MB
```

Two pools, and the second lives on the first one's leftovers:

```
08abfeac  jal   sceKernelMaxFreeMemSize
08abfeb4  lui   $a0, 0x53          ; 0x00530000 - the RESERVE it does not take
08abfeb8  subu  $a0, $v0, $a0      ; MainMemoryManager = everything else
...
0887f170  jal   sceKernelMaxFreeMemSize
0887f180  subu  $a0, $v0, 0x67000  ; StreamingHeap = what is left, less 0x67000
```

So **one 16-bit immediate decides the size of the resident world.** And the retail heap is
**saturated**: measured on an ordinary street it reads 4.52 MB of 4.75 MB, 95% full. It is not a
budget the game fits comfortably inside, it is a ceiling it is pressed against.

Each pool also has a DEAD branch above it, taken when the mode byte at `gp-0x72c` is non-zero -
`0x00C17800` for MainMemoryManager and `0x004C9000` for the StreamingHeap. That byte reads 0, so
neither runs, but they are the best evidence in the binary of what the game actually needs:
MainMemoryManager wants 12.65 MB and no more.

**The fix is two changes, neither of which touches the game's logic.**

`InitMemorySizeForGame` hands this disc a PSP-2000 partition, which is a one-line seam in
`Core/PSPLoaders.cpp` and needs no game patch at all - VCS asks the kernel how much memory there is
rather than assuming, so `sceKernelMaxFreeMemSize` simply answers 49 MB instead of 17. Deliberately
NOT an entry in `g_HDRemasters`: that sets `g_RemasterMode`, which also turns on double texture
coordinates and changes video handling, none of which this game wants.

On its own that does nothing, and the reason is the whole shape of the problem: **MainMemoryManager
is sized as everything-minus-the-reserve, so it absorbs every byte you add.** Handing the game 32 MB
more took its main pool from 12 MB to 44 MB and left the streaming heap at exactly 4.75 MB.

So `VCS::PatchLoadedModule` raises the reserve to match, and the rule it follows is what makes it
safe without measuring anything: **only ever hand the streaming heap memory a real PSP-1000 never
had.** The ceiling is `retail reserve + (g_MemorySize - RAM_NORMAL_SIZE)`, so MainMemoryManager
keeps exactly the bytes it had at retail and the extra partition goes to the world. On a FAT model
there is no headroom and the patch does nothing, which is correct rather than a limitation.

**Measured at 7x:**

| | streaming heap | in use |
|---|---|---|
| retail | 4.75 MB | 4.52 MB - 95% full |
| 7x | 35.88 MB | 7.51 MB |

The streamer immediately took 7.51 MB - **66% more world than retail can physically hold** - which
is the number that says the ceiling was real and was binding.

**Where the patch has to happen, and why nothing else works.** The pools are built about a second
into boot: long before the first vblank, and long before the WebSocket debugger is even reachable.
A per-tick installer of the kind every other patch in this fork uses is roughly ninety seconds too
late, and a `memory.write` from outside arrives after the fact - both were tried. The seam is
`__KernelLoadExec`, immediately before `__KernelStartModule`: the module is in memory, nothing has
executed, and the raw instruction is still the game's own with no JIT block over it.

**What is NOT yet known.** Whether it looks better. The draw count at a teleported spot came back
the same at 1x and 7x, and that measurement is worth nothing - the teleport landed in two different
streets at two different times of day, which is the trap this file has now recorded four times. The
right instrument is a savestate taken at a building that actually comes apart, reloaded for each
run; slot 2 is one such fixture for a long sightline and reads 486 draws with a min and max of 486.
More resident world is necessary for a wider view and may not be sufficient, and the honest next
question is which specific geometry is still missing once the ceiling is gone.


### The far city was always drawn - the haze is what you cannot see past

**Status: found, applied in the renderer, measured, and removed - never committed.** The numbers
below are real and nobody could see them in play, and "The map is baked per cell" says why: past the
first few hundred units the city IS the LOD stand-ins, and a clearer stand-in is still a stand-in.
The whole implementation was the two lines quoted here and a float read out of `VCSGame.h`. Five distance mechanisms have now been decoded here. Four of them
decide nothing. This is the fifth, it is not a distance either, and unlike the memory pool it costs
nothing at all.

**The evidence was already in this file, in the control rather than in the measurement.** The LOD
multiplier run recorded that at 0.05x "the entire skyline, the bridge and the far shore are still
drawn, pixel for pixel, and the thing that vanishes is the player". Read that the other way round:
the far city is IN the frame at a twentieth of the draw distance. Nothing about distance is taking
it away. What takes it away is the fog painted over it - so the question was never how far the game
draws, it was how far you can see what it has already drawn.

**Where the band lives, and why it is the renderer's business rather than the game's.** The fog is
two GE registers. PPSSPP's shader computes `v_fogdepth = (viewPos.z + fogCoef[0]) * fogCoef[1]`, so
`fogCoef[0]` is where the fog ENDS and `1/fogCoef[1]` is how wide the band is. Scaling one up and
the other down by the same factor moves the start and the end together and leaves the shape of the
fade alone - which is the difference between seeing further and seeing the same view less foggily:

```cpp
	const float fogScale = VCS::FogScale();
	if (fogScale != 1.0f) {
		fogCoef[0] *= fogScale;
		fogCoef[1] /= fogScale;
	}
```

That is the whole feature, in `UpdateFogCoef` (`GPU/Common/ShaderUniforms.cpp`), which all three of
its callers go through. `VCS::FogScale()` is a plain float in `VCSGame.h` rather than a call into
Core, the shape `VCSShadow::g_active` already uses and for the same reason - it is read once per
draw call. It reads exactly 1.0 for every other disc, and `RefreshFogScale` is the only thing that
writes it.

Doing it here rather than in the game is the point. `CCamera + 0x7a0` is the game's own copy of the
distance behind the band, and it is rebuilt every frame - the same property that forced the LOD
multiplier to be a code patch instead of a memory write. The renderer sits downstream of all of it.

**Measured**, on slot 2, with the camera identical across runs to within 1.3/255 over the lower half
of the frame. The instrument is not the draw count, which cannot see this at all: the pixels are
already being drawn either way, and what changes is what colour they are. So it is the standard
deviation of luminance over a band of distant geometry - fog collapses a building towards one flat
sky colour, and contrast coming back IS the building coming back.

| Horizon haze | far shore, contrast | downtown behind the crane, contrast |
|---|---|---|
| 1.0x (stock) | 30.01 | 40.21 |
| 2.0x | 39.97 | 43.87 |
| 4.0x | 45.09 | 46.02 |
| 8.0x | 47.61 | 47.08 |

**+59% on the far shore**, and the mean colour of that band moves from `68, 173, 190` - the sky is
`63, 174, 240` - to `82, 158, 156`, which is what a row of buildings looks like when it is not being
painted the colour of the sky. Most of it arrives by 2x and the curve flattens after 4x, which is
the band having already moved off the geometry that is there.

Two runs of the same setting reproduce to three significant figures (43.86 against 43.87), which is
what says the differences above are the setting and not the scene.

**No hard edge at 8x.** The predicted failure was the game's own far clip arriving before the fade
finishes, and at the ceiling of the row it has not happened at this spot - the far shore still ends
in water and sky. That is one sightline, not a proof, and the help line still warns about it.

### Three ways to lose a fixture, all found in one afternoon

The fog measurement above took eight runs, and five of them were thrown away. Each failure has a
shape worth recognising before it costs a run again.

**A savestate belongs to a partition size.** Slot 2 was written under `World memory` 1 and will only
load under it. Loading it at 7 kills the emulator - once as `Bad Execution Address` at `0x0bffecc0`,
which is high memory a 32MB partition does not have, and once by simply vanishing between two
samples. The commit that made the partition conditional already said this; it is repeated here
because the failure does not look like a savestate problem, it looks like whatever else changed in
that build. **Choose the fixture and `World memory` together, and pass both to the runner** rather
than inheriting whichever is in the file.

**Mouse look randomises the camera.** `VCSCamera` writes yaw directly, from host state a savestate
does not restore, so the same slot came back facing a hotel in one run and the downtown skyline in
the next. A fixture whose camera is not the same is not a fixture. `MouseEnabled = False` for the
duration of a measurement, and check it afterwards: the lower half of the frame is ground and
player, so a mean absolute difference near zero there is the proof that only the sky changed.

**The fork's own autoload will eat a savestate loaded too early.** Boot walks the game's front end
to load the newest save, and that lands somewhere between 100 and 160 seconds in. A savestate loaded
at 100s is silently replaced by the game loading its save on top of it, and the run measures the
autoload's destination instead. Three runs were lost to this - and the dangerous part is that all
three AGREED with each other, because they were all the same wrong scene. **Consistency between runs
is not evidence that the fixture loaded.** Look at one of the frames. The runner now waits past the
walk and loads twice, twenty seconds apart.

And one that is not about the game at all: a PowerShell harness writing `vcs.ini` with `"{0:F6}"`
formats in the machine's locale, which on this one is a decimal COMMA. `FogDistance = 1,000000` is
not a number the ini reader recognises. Use `[System.Globalization.CultureInfo]::InvariantCulture`
for anything written into a config file.

### The map is baked per cell, and that is the whole of the draw distance

**Status: decoded end to end, a rebuild tool and a runtime built and verified mechanically in game,
then removed without being committed.** What wider detail was wanted for was building shadows that
come apart with distance and go out behind you, and "Cast once, then there" fixed that inside the
shadow cache without touching the map. The decoding below stands; the tool and the runtime it
describes are gone, and this section is the only record of them. With the row on, both
levels' chunk tables were widened with nothing refused (461 `BEACH` entries, 482 `MAINLA`), widened
again after every savestate load put the retail table back, and merged chunks streamed in without a
crash - the fixture's own cell among them, chunk 225 at 552,860 bytes instead of 483,092, once the
player had been hopped far enough away to push it out of all three slots. One jump was not enough:
the streamer kept the cell as its spare and reused it. What is not settled is the picture. Every
on/off pair shot in separate sessions came back at a different camera angle, restoring the ped's
whole transform made it worse, and the single-session comparison - flip the loaded chunk's pass
pointers between the merged lists and the original ones it still carries, and shoot on/off/on - was
stopped before it ran. Two things that comparison has to do first: re-flag the original lists, whose
records carry bit 15 as the disc has it (525 of 849 in chunk 225, and the game reads that bit as
hidden), and hold the camera still. This is the section the five inert levers above were circling. None of
them could move the map because the map is not streamed by distance at all.

**What VCS streams.** Each level - `BEACH`, `MAINLA`, `MALL` - is a `.LVZ` and an `.IMG` in
`RUNDATA`. The LVZ is a zlib'd `WRLD` relocatable chunk; the level struct (`sLevelChunk`, laid out
exactly as aap's storiesview has it) holds a resource table and 36 rows of a **staggered grid of
32 x 36 cells, 125 x 108.25 units, origin (-2400, -2000), odd rows shifted half a cell.** Every
cell is its own chunk in the IMG, and the LVZ carries a table of their 32-byte `WRLD` headers:

```
+0x00 ident "DLRW"   +0x08 fileEnd    +0x0C dataEnd    +0x10 relocTab
+0x14 numRelocs      +0x18 globalTab - the chunk's offset into the IMG
```

The disc holds a chunk from coordinate 0x20 onward - the header itself lives only in the table - so
every pointer inside a chunk counts those 32 bytes.

**And a chunk is a precomputed view from its cell**, not a piece of map. Its header lists instance
passes - `superlod, underwater, lod, roads, normal, nozwrite, lights, transparent` - and for cell
(20, 12), the slot 2 fixture's, they reach:

| pass | instances | distance from the cell |
|---|---|---|
| normal | 166 | 36 - 266 |
| transparent | 132 | 36 - 266 |
| roads | 12 | 17 - 214 |
| lod | 423 | 156 - 1633 |
| superlod | 43 | 291 - 1772 |

Full detail out to about 266 units and stand-ins past that, **baked into the archive when the game
was built.** Nothing at runtime chooses between them, which is why the frustum, the delete-behind
pass, the far clip, the LOD multiplier and the fog were all inert for the map - and why the 0.05x
LOD multiplier left the whole skyline standing: it is the `lod` and `superlod` passes of one chunk.

**An instance is 0x44 bytes and carries no pointers** - no relocation entry lands inside a pass
list. `+0x00` u16 id (bit 15 is a hidden flag the game recomputes on every cell swap, set when the
resource is not loaded), `+0x02` u16 resource id, `+0x04` a half-float bounding sphere **in world
units**, and `+0x10` twelve position-matrix components **relative to the cell**, each stored as a
float with its low eight bits dropped: the game shifts the word left 8 and reads IEEE bits
(`0x08958454`). Mixing those two frames up cost one build - see below.

**The streamer is `cWorldStream`, at `[gp + 0x16e8]` (`0x08BB3448`).** `CStreaming::Update` calls
its update at `0x08958764` once a frame. It keeps **six slots at `+0x244 .. +0x258`: the current
cell, the next one and a spare, plus three area packs on a grid a third that size.** It measures the
camera against the current and next cells' centres (`0x08953a08` computes a centre - the only
function in the game that loads 108.25) and swaps at `0x08957d74` when the camera crosses; the
next cell is predicted from a point ahead of the camera built with 50.0 and 150.0.

A load goes `0x08955ec0` (request) -> `0x08956160`, which seeks the IMG to `globalTab`, bump-
allocates `dataEnd` rounded to 16 from `+0x278`, and reads `fileEnd` bytes asynchronously. The
completion at `0x089563c0` relocates the chunk through `0x08a313ec` **using the header in the
in-RAM table** - so the table, not the disc, is what decides a chunk's size and where its
relocation table is. The IMG's file record is at `cWorldStream + 0x270`: `+0x00` first sector,
`+0x04` size, `+0x08` position, and **both seek and read clamp to that size** (`0x089394a4`,
`0x08939590`).

**Memory.** A cell slot's arena is a fixed **773,488 bytes** (`0xBCF70`), four of them sliced out of
one allocation at `0x08957710`, and the bump allocator has no limit check. Measured across all three
levels, a cell with its swap chunks leaves between 8 KB and 755 KB free, median about 500 KB.

**Resources.** A resource whose pointer is present in the LVZ file is part of the level image and is
resident for as long as the level is: 1699 of 1699 agree with a live savestate's resource table.
Everything else rides in with a chunk (its overlay list) or an area pack.

**What that makes the draw-distance lever: the chunks themselves.** `Tools/vcsworldhd.py` rebuilds
each world cell's view to carry its neighbours' full detail as well:

- every cell within 260 units contributes its `roads`, `normal`, `nozwrite` and `transparent`
  instances, deduplicated by id (adjacent chunks share about 60% of them), rebased onto this
  cell's origin, nearest first;
- only instances whose resource lives in the level file are taken, so nothing new has to be loaded
  for them to draw and the chunk needs no new resources or relocations;
- this cell's LOD stand-ins with new detail inside their bounding sphere are dropped, except the
  huge block LODs, whose far side the new detail does not reach;
- the pass lists are **appended past `dataEnd`, all eight of them**, because a pass's end is the
  next pass's start - moving any one moves them all - and the header's pass pointers repointed. The
  old lists stay behind as dead bytes, and the relocation table moves to the new end unchanged;
- a cell is capped by its own arena headroom less 8 KB, dropping the farthest additions first.

For cell (20, 12) that is **full detail out to 511 units instead of 266**: 291 instances added, 114
stand-ins dropped, 68 KB more chunk. Across the game: `BEACH` 461 of 623 cells, `MAINLA` 482 of
601, `MALL` 135 of 1152, 115k instances added, one 35 MB file, built in about six seconds.

**The file is a list of changes, not an archive.** Chunks stay where they are on the disc; the
runtime (`VCS::WorldDetail*` in `VCSGame.cpp`, row `Distant detail`) applies them in two places.
The level's in-RAM chunk table gets the new `fileEnd`, `dataEnd` and `relocTab` every tick while
the setting is on, and the disc's values back while it is off - verified against `ident` and
`globalTab` first, and deferred while `cWorldStream + 0x274` says a read is in flight. And the read
of a merged chunk, which now runs past the chunk's end on the disc, gets the new pass pointers and
the appended bytes laid over it, **keyed on the read starting at that chunk** and armed only when
the table already describes it as merged - so the next chunk's own reads, which overlap the grown
range, are never touched, and a savestate from before the table was widened reads the disc as it is.

**Two things an earlier design got wrong, worth keeping.**

- **`SMALLPAD.DAT` is not free space for the taking.** It is 173 MB of random letters the game never
  reads, and the obvious plan was to map rebuilt chunks into it. Two things kill that: the archives
  are 475 MB and the padding is 173, and the IMG file record clamps every seek to the IMG's own
  size, so a `globalTab` pointing past it lands on the last byte. Serving only the delta, at the
  chunk's own offset, needs neither.
- **The bounding sphere is in world units and the matrix position is not.** The first build rebased
  both, which moves every merged instance's culling sphere a cell-origin away from its geometry, and
  it measured the LOD overlap with the cell origin added twice. The check that found it: across a
  whole cell, sphere centre minus matrix position is a constant 667 units - the length of that
  cell's origin.

**Tooling notes from getting here.** `Tools/vcsstatic.py --disasm` is the disassembler to use for
this code: capstone on its own stops dead at the first VFPU instruction or MIPS32r2 `ins`, and a
dump it "wrote" can be 69 lines of an intended 5000 without saying so. `awk` comparing eight-digit
hex addresses compares a number with a string and returns nothing. And a function's float
constants are split across `lui` and an `ori` up to 28 bytes later, with other instructions in
between, so a scan that only looks at the next three instructions misses the one function it is
looking for.

### Streaming stutter, measured - and `CacheFullIsoInRam` is worth its memory

"Average fps" is useless for this. A run that freezes for a third of a second once a second still
averages close to thirty, and thirty is what every earlier measurement in this file reported while
the player was describing a stutter. What a player feels is the LONGEST gap, so the instrument is
the distribution of intervals between increments of the game's own frame counter, sampled as fast
as the debugger will answer, while the player is dragged across the map to force streaming.

Same four-stop tour, same build, one boot each:

| | cache off | cache on |
|---|---|---|
| median frame | 33.3 ms | 33.4 ms |
| 90th percentile | 35.7 ms | 34.8 ms |
| **99th percentile** | **107.0 ms** | **51.9 ms** |
| **worst single frame** | **417.6 ms** | **251.9 ms** |
| **frames over 100 ms** | **1.0%** | **0.3%** |
| frames completed in the same wall time | 583 | 707 |

So it is a real fix for a real part of the problem: **three times fewer hitches and half the
99th-percentile frame time**, and a fifth more frames completed in the same wall clock, which is the
stalled time coming back. It does not cure it - a quarter-second freeze still happens - so whatever
else is behind the remaining spikes is not disc latency.

It costs a slower boot and 1.6 GB of memory: the ISO is read whole at startup, which took **101
seconds against about 25**, and the process sits at 2.6 GB rather than 1.0. On a machine with the
room that is a good trade for a game that streams constantly; on one without it, it is the first
thing to turn off. `IOTimingMethod` is already Fast and is not the lever.

**A tooling note that cost several rounds, and is not about the game at all.** The debugger's memory
reads are serviced on the CPU thread, so anything that stops the emulator - this fork's own pause
menu is a `UIScreen`, and a `UIScreen` pauses emulation - leaves every read unanswered rather than
answered late. A measurement script that treats a read as reliable will hang, and its traceback will
point at the read rather than at the menu. Two further shapes of the same trap turned up in one
session: a probe that reported "the CPU is stopped" when the read had actually succeeded and merely
returned a null player pointer, and a screenshot helper that captured the wrong window because
`Process.MainWindowHandle` did not name the game's - enumerating top-level windows by process id and
picking the visible one is what works. Guard every read, and say which of the two things went wrong.

### Driving stutter was Instant texture loading, and it only waits on 2D now

**Status: cause confirmed from play, rule built.** `ReplacementTextureLoadSpeed` is on Instant for
a reason: on any other speed the splash and credits art shows its low-res original for a moment
before the HD file lands. But Instant waits for EVERY replacement inside the frame that asked for
it, and a chunk that streams in brings dozens of world textures at once - so driving fast into a
new area loaded all of them synchronously and the frame stalled. Switching to Fast removed the
stutter and brought the low-res splash back.

`TextureCacheCommon::PollReplacement` now keeps both: while VCS is active and the speed is
Instant, a 3D draw during play does not wait - it takes its replacement as it arrives, a frame late
at worst, exactly as Fast does - while through-mode draws (splash, credits, menus, loading screens,
the HUD) and everything before `BootPhase::Intro` ends still wait. No setting changed.

**The harness did not reproduce it, and that is worth knowing before trusting one.** Dragging the
ped east at 40 units a second from slot 2 logged one 100 ms stall and not a single frame blocked on
textures. The writes only covered 500 units in 28 seconds, a teleported ped is not a car, and every
step that moved the follow camera far enough made the water module forget the sea - so the path
exercised the streamer far less than driving does. The confirmation is from play.

Two log lines stay, both silent in normal play: `N ms between vblanks at game frame F` past 100 ms
(`VCS::Tick`), and `last frame spent N ms on M texture replacements` past 50 ms
(`TextureCacheCommon::StartFrame`). A replacement line during play means the rule above has stopped
holding.

### The sea painted over the player: two rejects that should never see a person

Reported from play as the water overlapping the character "sometimes". The depth pre-pass is the
only thing that keeps the sea off anything in front of it, so a person rejected from it is a person
the sea is composited over. Two tests could reject one, and neither should:

- **Blended.** The game fades people with their material alpha, which turns its copying blend into
  one that alters the destination, so `BlendAltersDestination` filed a fading player as glass. The
  same applied to the depth-write test.
- **Near the camera.** The test that throws out the game's full-screen overlays - geometry sitting
  entirely within two units of the camera - also caught a separate piece of the player, a head or a
  hand, whenever the camera pulled in behind him. Measured, not reasoned: the first stutter run
  logged `a person was left out of the occlusion pass (sits on the camera)`.

A skinned draw is now exempt from all three. The overlays are flat quads and never skinned, and a
faded person occluding costs the vanilla sea showing through them for the length of the fade -
the same good way round as the alpha-tested palm fronds. `NoteReject` logs once per reason when a
person is still rejected, so if it comes back the log names the test.

`VCSShadow::AddCaster` has a near-camera cutoff of the same shape and was left alone: nothing about
shadows has been reported, and a person's head missing from the shadow pass while the camera is
inside two units of it is a much smaller thing than the sea covering it.

### Two traps in patching this emulator's code

Both cost real time on the draw-distance port, and neither is visible from the API. Anything that
patches game code will meet them again — the fire hook only escaped them because it patches a
`jal` in the middle of a function, which no block starts at and nothing reinstalls.

**PPSSPP hides the game's instruction behind its own marker.** Once a block has been compiled, the
raw word at its first instruction is a `0x68xxxxxx` RUNBLOCK marker, so a raw read sees neither the
game's opcode nor your replacement. Every check has to go through `Memory::Read_Instruction`, which
resolves both back. The sharp edge is on the way out: `RestoreReplacedInstruction` reads the RAW
word and silently declines unless it sees a replacement marker there — so uninstalling a hook whose
address has since become a block start does nothing at all, and the hook stays live while every
flag in your own code says it is gone. Invalidate the block FIRST, then restore.

**`WriteReplaceInstructionAt` returns false for success.** It reports false both when the write
failed and when the identical replacement was already installed. Treating the return value as the
answer made a working, running hook report "far clip replacement refused" on every frame after the
first — which, combined with the trap above, produced a remove/reinstall loop that left the feature
permanently half-installed. Ask what is actually at the address instead of what the writer returned.

### The mounted gun: one state where CameraYaw is not a world angle

A mission can strap the player to a vehicle with a weapon instead of seating him in it - `02B6
attach_ped_to_car $PLAYER_CHAR car $5666 offset 1.6 1.0 0 position 3 angle_limit 70.0 weapon 34`,
which is `GON_C4`'s helicopter ride. **`PlayerVehicle` reads 0 throughout**, so the fork sees an
ordinary on-foot player holding a gun and every on-foot assumption about the camera is wrong at
once.

The one that matters is what `CameraYaw` MEANS. `CCam::Process` mode 45 branches on the attachment
at `0x089a3548`: on that branch it starts `Beta` at ZERO (`0x089a3578`) rather than at `PedHeading
+ PI/2` (`0x089a3584`), skips its own `[0,2PI)` normalise after integrating (`0x089a3980`), and
clamps instead. So while attached the field is a **signed offset from the vehicle's nose**.
Measured by asserting it every frame against a savestate of that ride:

| assert | look, relative to the helicopter |
|---|---|
| 0.00 | 0.0 deg |
| 0.40 | 22.9 deg |
| 1.00 | 56.3 deg |
| -0.40 | -23.8 deg |

Before that first write it sat at exactly 70.0 deg - pinned against the clamp, because this fork
was asserting an absolute world yaw of 3.82 rad into it. Reported in play as an aim that snapped
back to one fixed direction however far the mouse moved.

Two faults, and fixing either alone leaves the other. The anchor was never retaken, because
entering the attachment is a camera change that no CONTEXT change announces - and with `returnLook`
off the hold parks open, so the yaw anchored before boarding is asserted for the whole ride. And
`WrapYaw` forced the value into `[0,2PI)`, which destroys half of an arc: `-0.4` is a small offset
left, `5.88` is two and a half turns past the stop. It re-anchors on both edges now, and clamps to
the game's own limits instead of wrapping.

The limits are the mission's and are read rather than assumed: `PedAimYawLimit` (`ped+0x760`) is
`attach_ped_to_car`'s `angle_limit`, and pitch gets `ped+0xCA0` / `+0xCA4` - 10 deg up, 55 deg
down, written by script opcodes `04CF` / `04D0` as `degrees*PI/180`. Asymmetric because a door
gunner looks down.

**The section is short and cannot be held open on request, which is the other lesson here.** Three
mission runs went into chasing it live, one of them failed by a probe of ours that held the aim
trigger and swept the stick for four seconds. What ended it was a savestate taken the instant
`ped+0x848` went non-zero - a script polling that field over the WebSocket debugger and driving
PPSSPP's own quicksave through `WM_COMMAND`. After that the same moment could be reloaded, written
to and read back as often as needed with nothing at stake. Reach for that on the FIRST attempt at
anything that only exists for a few seconds of a mission.

### The right number in the wrong space

Two separate bugs in one day had the same shape, and neither looked like it at first: both read as
"the game is fighting us", and both were **a correct value written into a space where it means
something else**.

- The auto-save wrote a restart position that was correct for the safe house's interior, into a
  save whose interior state said "outdoors". The pair disagreed, and the player materialised inside
  a building.
- The mounted gun got a correct WORLD yaw written into a field the game reads as an offset from a
  helicopter's nose. It pinned at the clamp.

Neither is a wrong number, and neither is a race. Both are a value whose meaning depends on some
other piece of state - an interior index, an attachment pointer - that was not consulted, and in
both cases the game itself pairs the two consistently by construction: its own save routine only
runs where the pickup is, and its own camera sets `Beta` to zero when the attachment begins.

So when a write "does not take", or takes and then springs back, ask what ELSE has to be true for
that field to mean what you think it means, before reaching for a rate, a spring or a fight over
who writes last. The three questions that separated these two from an ordinary tug of war:

- **Does the game write this field itself, and from what?** Read the writer, not the field.
- **Is there a nearby field that changes what this one means?** `$_282` and `ped+0x848` were both
  one `lw` away in the very code being read.
- **Does the value's RANGE make sense in the space I think it is in?** 3.82 rad in a `+/-1.22`
  window says the space is wrong long before any behaviour has to be explained.

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
