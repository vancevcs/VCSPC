# The chase camera and camera limits

> Read before touching `VCSChaseCam`, mouse look on foot or in vehicles, or camera pitch limits.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

### The chase camera: the view is built, not negotiated

**Status: built 2026-09-15, measured on foot and on a motorbike over the WebSocket debugger, NOT yet
played by hand.** Aircraft, boats and the glance keys have not been run at all - there is no
helicopter fixture, and the glances are keyboard-only.

Mouse look used to write the game's orbit angles and let the game's own camera modes build the view
from them. Three reported faults came straight out of that arrangement, and none of them was tuning:

| | what the game's mode did with a written angle |
|---|---|
| on foot, mode 4 | held its distance and steered round walls - so the camera sat on the ground when you looked up, the player left the frame, and it would not turn toward a wall you stood beside |
| driving, mode 18 | integrated pitch, and a written pitch wound the integrator up until the view pinned to the roof - on a pitch change, or just driving up and down hills |
| flying, mode 18 | followed the aircraft's own pitch, so tipping a helicopter forward tipped the view down into the rotor |

So the chase camera does not write into those modes. It lets the game compute its camera and
**replaces the result inside the game's own frame**, at two points in the camera update
(`0x08a225a4`, `$s0` = CCamera):

```
08a2262c  addiu $fp, $sp, 0x50      Source      the three vectors every later stage works on
08a22630  addiu $s6, $sp, 0x70      Up
08a22634  addiu $s7, $sp, 0x60      Front
...
08a23a60  lwc1  $f12, 0xb38($s0)    <- PREPARE (HOOKENTER): the game's camera, before shake
...                                    shake
08a23be8  jal   0x08a1a5dc          <- redirected to a program in PSP memory, then the matrix build
```

The prepare hook reads the game's three vectors, works out the pivot (the player, or the vehicle),
the direction the player is looking and how far back the camera wants to be, and writes a fan of up
to five rays into a block of PSP memory. The patched `jal` lands in a ~50 instruction program in the
same block that calls the game's own `CWorld::ProcessLineOfSight` once per ray, reaches a second
HOOKENTER - the finish hook - which pulls the camera in to the nearest hit and writes Source, Front
and Up over the game's, and then tail-calls the cross product the `jal` was for. From the game's side
the `jal` did what it always did.

Three properties fall out of doing it there, and each is one of the old bugs gone by construction:

- **The collision is the game's.** Buildings, objects and (on foot) vehicles, with see-through
  surfaces ignored - the flags the camera update's own clip passes at `0x08a239dc`.
- **The timing is the game's.** One pass per logic frame, inside it. Nothing is written at vblank,
  so there is no double write for an integrator to wind up on - and in a vehicle nothing is written
  into the game's angles at all.
- **Everything downstream sees it.** The matrix, and therefore the renderer, the frustum, the
  streamer and the radar, all take the camera the player sees.

**What it is modelled on is San Andreas on PC.** The camera orbits a point above the character and
looks at it from where the mouse put it; when something is in the way it moves IN along that line
rather than somewhere else. Look up and it comes down behind the player's legs; stand between two
walls and it comes in close instead of refusing to turn. Pulling in is immediate and easing back out
is not (`easeOutRate`), or the view pumps past every railing.

**What it leaves to the game** is every other mode - aiming, scopes, cutscenes, first person, the
mounted gun, the drive-by. Handing a view over is a short blend each way.

**How far back is its own, and the first build got that wrong in an instructive way.** It learned
the distance from the game's camera, on the reasoning that VCS already knows how to frame a player
and a bus. It sat at **2.29** for a whole session on a street where VCS frames the player at 4.5.
The game's own camera starts every frame from the one the finish hook wrote - the active cam's Source
reads back within a few centimetres of the matrix - and mode 4 is a camera on a string: it only
enforces a band, so asked what distance it wants it answers with the one it was handed, and the boot
load had handed it a close one. A value read back from a system you are driving is your own value.

So on foot the distance is a setting, 4.5 - where VCS sits in the open (4.63 from the player in
`ULUS10160_1.03_1.ppst`). A vehicle is framed from its collision box, which is the data mode 18
itself reads (`[[gp+0x18] + model*4] + 0x14`): the pivot at 0.95 of the box's top, and the distance
`5.0 + 0.65 * length` at the middle zoom level, fitted to the motorbike fixture - a box 1.98 long,
framed at 6.26 by its own camera - and scaled by 0.8 and 1.35 at the near and far levels Select picks
(`CCamera+0x798`).

**On foot the game's angles still follow the view.** `CameraYaw = yaw + PI` and a clamped
`CameraPitch` are written from the finish hook, because they are where the aim camera starts from and
where the walk direction comes from. Never in a vehicle.

**Vehicles swing back behind the car** a moment after the mouse is left alone, once the vehicle is
moving (`vehicleRecentre`, the one row on the player's Mouse page). The glance keys look left, right
and behind relative to the car, and letting go asks for the default view back. Aircraft yaw with Q/E,
so there the glances stand down.

**Loading a savestate takes the patch away**, and this is handled rather than hoped about: every
tick the install reads back the magic word, the `jal`, and both hooks. The hooks sit on branch
targets, which is where the JIT starts a block, so they are read through `Memory::Read_Instruction` -
the first build read them raw, found a block marker, and reinstalled everything on every tick. A
state taken WITH the patch brings back a `jal` into a block this session has no record of, and the
install adopts that block by its magic instead of refusing.

**Testing it needs a look input a script can send**, and there is none - the debugger's analog
injection goes straight to the PSP stick. So the block takes a look command: write yaw at `+0x30`,
pitch at `+0x34`, then flags at `+0x2c` (bit 0 yaw, bit 1 pitch) with `memory.write_u32`. The block's
address is in the install's log line. `VCS_CAM_TRACE=1` in the environment writes one NOTICE line a
game second with the mode, the game's distance, the wanted, allowed and actual distance, and the hits.

**Measured**, with look commands and `peek` reading CCamera's matrix back:

| where | asked for | camera from the player | hits |
|---|---|---|---|
| street, level | pitch -0.1 | 4.57 - 4.60 | 0/5 |
| street, look up | pitch 0.9 | 1.40, just above the pavement, view 51.6 deg up | 5/5 |
| street, look up | pitch 1.15 | 1.04, view 65.9 deg up | 5/5 |
| street, back to level | pitch -0.1 | eased back out to 4.60 | 0/5 |
| street, look down | pitch -1.2 | 5.06, view 68.8 deg down | 0/5 |
| interior corner, 16 yaws | full circle | 4.60 away from the walls, 1.87 / 1.30 / 0.72 turning into them | 5/5 at the wall |
| motorbike, level | - | 6.39, wanting 6.29 against the game's own 6.26 | 0/5 |
| motorbike, look up | pitch 0.6 | 1.49, low behind the bike | 5/5 |
| motorbike, look down | pitch -0.9 | 6.80 above and behind | 0/5 |

And the sequences: the view set to the bike's side, throttle held, and 1.5 s after the last command it
swung round behind - yaw 1.57, 1.46, 0.08, -0.26 against a heading of -0.32, pitch settling to -0.12.
The bike then ran into two police officers and threw the player off, and the handover to on foot eased
the distance 5.18, 4.53, 4.50 without a cut. Loading savestate slot 3 on foot reinstalled both hooks
into a fresh block and took the vehicle view on the next frame.

**Reported from play once it went in: the view showed the inside of the character and of the car.**
Everything else about it was, in the player's word, perfect - and the cause was the one thing the
chase camera had not taken over from the game: the near clip plane.

`RwCamera+0x78` (the scene camera is `[gp+0x22c0]`) reads **0.9** - set for a camera VCS keeps four to
six metres out. The chase camera comes in to a metre or less when the ground or a wall is behind it,
and anything nearer the lens than 0.9 is not drawn, so the front of the player or the car was cut away
and the far side seen from inside. In a vehicle a second cause stacked on it: looking up swung the
camera's position down below the pivot, toward and into the bodywork.

Three changes, in the order they matter:

- **The near plane follows the clearance.** The finish hook works out how far the camera is from the
  player's body (a capsule, `footBodyRadius` about his axis, feet to just over the head) or the car's
  collision box, and asks for `clearance * nearClipFactor`, clamped to `nearClipMin` and never above
  the game's own value that frame. The program calls the game's `RwCameraSetNearClipPlane`
  (`0x08890798`) with it after the hook returns - a function rather than a write, because the setter
  resyncs the camera. It lasts one frame by construction: the prologue sets 0.9 at `0x08a227d8` every
  frame and mode 18's own close-camera lowering at `0x0899f688` runs before the hook too, so the smaller
  value wins and nothing has to be put back.
- **In a vehicle the position stops 0.35 rad below the pivot** (`vehicleOrbitUp`). Looking further up
  tilts the view instead: Front comes from the look pitch, the position and the rays from the capped
  orbit pitch. **0.08 was tried first and was too tight** - the bike was off the bottom of the screen
  by a 0.6 look-up. At 0.35 the ground still stops a car's camera ~1.6 past its rear bumper.
- **A camera that ends up inside anyway is lifted out over the top** (`subjectMargin`): out of the
  capsule to just over the player's head, or out of the box along world up. The view keeps looking
  where the player asked. This is the backed-into-a-corner case, which used to leave the camera at the
  back of the player's neck. **Not yet exercised** - no run so far stood the player in a corner.

Measured on the Release build:

| where | look | camera | clearance | near plane |
|---|---|---|---|---|
| on foot, open pavement | level | 4.50 from the pivot | 4.13 | 0.90 (the game's) |
| on foot, looking up | pitch 1.15 | 1.55, ground behind | 0.29 | **0.17** |
| motorbike | level | 6.29 | 5.13 | 0.90 |
| motorbike | pitch 0.3 | 3.31, ground behind | 2.02 | 0.90 |
| motorbike | pitch 0.6 and 1.0 | 2.88, held at the orbit cap | 1.58 | 0.90 |

On foot looking up, the player's arm and back now read solid from below. On the bike at 0.6 the rider is
framed against the sky at the bottom of the screen; at 1.0 the bike has slid out of frame, which is the
San Andreas tilt rather than a camera inside it. The clearance and the near plane are on the Camera tab
and in the `VCS_CAM_TRACE` line.

**What the old arrangement taught, kept short.** Four builds went into handing the on-foot camera
back to the game - a fade, a heading target, a learner, an idle hold - and every one landed on the
same wall: mode 4 has one resting position while walking and another at a standstill, so a handback
at rest is a value the game will always disagree with. The vehicle pitch runaway was the same shape
from the other end: a second writer of one field, at a different rate from the game's own. Both are
arguments for the same design: when the game's camera does the wrong thing with a number you give
it, stop giving it numbers and replace what it produces.

### Camera pitch: limits must be anchor-relative, and vehicles are excluded

**Superseded 2026-09-15 for vehicles and aircraft, and for looking around on foot.** Nothing writes
`CameraPitch` into mode 18 any more: the chase camera replaces that camera's output instead of
feeding it an angle - see "The chase camera: the view is built, not negotiated". What follows still
applies to the one path that writes pitch directly, aiming, and it is the record of why the vehicle
camera could not be driven by its angles.

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
