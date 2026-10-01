# Input: bindings, the gamepad, button numbering, vehicles and combat

> Read before adding or changing a key binding, a context, a vehicle class, or anything that presses a PSP button.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

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
  see "There is no stored aim direction". See [docs/VCS_ADDRESSES.md](../VCS_ADDRESSES.md).
- **Direct weapon selection.** `WeaponIndex` now exists, so this is unblocked; it still probably
  wants a memory write rather than a button press, since the PSP only exposes
  cycle-next/cycle-previous.
- **Aircraft under the chase camera.** Helicopters and planes are expected to run camera mode 18 as in
  Vice City, which is what the chase camera owns - but no aircraft has been flown since it went in. If
  the view still tips with the nose, the Camera tab's mode readout is the first thing to look at.
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
