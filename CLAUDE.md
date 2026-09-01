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
| Compat flag | [Core/Compatibility.h](Core/Compatibility.h) + `.cpp` + [assets/compat.ini](assets/compat.ini) | One struct field, one `CheckSetting` line, one `[VCSInputOverhaul]` section |
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

`$783..785` is where the last save pickup was collected, and it is zero in a game where nobody has
walked into a save icon yet - which an auto-save fired by the first mission passed will meet. The
player's own position is used there instead, because restarting at the map's origin is not a place.

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
  camera-origin ray on, weapon range on, `aimPitchDeadband` **0.165**, `aimYawKick` **0.166**,
  `aimYawDeadband` **0**, everything else off. `pedFollowAim` off - the game turns the character
  itself. The `vertical` row above is now only half the story: both axes stall, and each gets its
  lead by a different route - see the section below for why the difference is structural.

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
