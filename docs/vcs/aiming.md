# Aiming: lock-on, mouse free aim, the second stick

> Read before touching `VCSCamera`, `VCSFireHook`, the aim response model, deadbands or the fire-site ray.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

### The passenger drive-by is aimed with the view, like free aim

**Status: confirmed in play on Jive Drive, 2026-10-06, "works well now".** `driveByCameraAim`, on by
default. In the passenger seat (camera mode 11 and weapon camera mode 11, `DriveByAimActive`), all of
this happens at once:

- **The mouse turns the view.** The chase camera builds a drive-by view (`g_driveByView`: the
  vehicle view with no glances and no recentring), looking 1.3 m above the car so the car sits low
  in the frame. It uses free aim's numbers: Aiming sensitivity and "invert aim vertically".
- **The shot goes down the middle of that view.** `CameraDrivenAimHeld` counts the seat as aiming,
  and the fire hook takes the ray from `ChaseCamDriveByBasis`, the view exactly as rendered, through
  screen centre. `EmuScreen` draws a crosshair there.
- **The passenger's gun follows the view.** `ApplyAnalog` drives the stick with a proportional loop
  on the angle between the view and the gun (`driveByFollowYaw` 8, `driveByFollowPitch` 20). The
  gun's direction is what the game writes into `CameraYaw`/`CameraPitch` in this mode each frame
  (front yaw = `CameraYaw` − π, as on foot).

**Why not the stick, which is how it was aimed before.** Reported from a tester: "inverted by
default, even if I change it in the settings", "very insensitive". Four builds of tuning the mouse
onto the nub went inverted, then too slow, then "one very small move moves it like crazy", then
"like moving a joystick". The reason is in the code: the drive-by's stick is target selection, not
free aim. `0x0894fddc` reads the scaled axes (`0x0898de2c` X, `0x0898df58` Y, which return
`trunc(stick * CPad+0xd0/+0xd4)` with no clamp) and nudges an aim point that it also snaps onto
entities: it copies a target's position into `CPed+0xcf0`. The game's own hint says the same thing:
"Hold [fine aim] to make fine adjustments to your aim". No mapping of a mouse onto that can be
precise. For the record:

- vertical WAS inverted relative to the reticle's convention; horizontal was not, and flipping it
  played as "right moves it left";
- a write to `CameraYaw` in mode 11 is overwritten within the frame, so it is derived, not stored.

**Shots first landed off the crosshair, because the hook read the wrong camera.** The chase camera
writes the camera update's working vectors (`0x08a225a4`'s stack, see `kVCSCamPreShake`), which
become the CCamera matrix the renderer uses. It never writes CCam[0]'s stored basis (`kVCSCam0` +
0x10 front, + 0x20 source). That basis is what the fire hook reads on foot, where it IS the view. In
the drive-by it is the game's own mode-11 camera, so every shot went down the game's line, "in some
kind of offset". Fixed with `ChaseCamDriveByBasis`.

**And the fire hook did not survive a savestate.** `InstallFireHook` installed once per boot and never
looked again, while loading a state puts the game's own `jal` back at `0x08A41D74`. Free aim stopped
redirecting anything for the rest of the session, on foot as well. A breakpoint on that address
stopped on the plain `jal`, which is what showed it. It now checks every tick through
`Read_Instruction`, the way the chase camera checks its own hooks. The drive-by's shots come through
that same site, from the weapon routine at `0x08A48DEC`.

**Testing this without replaying the mission.** A scratch memstick (see the test-fixture memory)
with Save State / Load State on F2 / F4 in its `controls.ini`: the tester presses F2 in the car once,
and every later build is checked with F4 through `VCS_KEY_PIPE`. The first F4 after boot does not
take, so press it twice. Keep the game window in front: macOS App Nap throttles it in the background,
and every measurement taken that way was noise.

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
