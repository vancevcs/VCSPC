# Draw distance and the streaming grid

> Read before trying to change how far the city draws. Most obvious levers were measured and are inert; the record says which.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

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

### VCS culls to the camera, and widening it is worse than the pop-in

**Status: the cull was found, the lever works, the feature was built and measured - and it was
REVERTED after play, because it drops objects the player is looking straight at.** Reported as
"some objects that are 100% in that original default viewport get deloaded sometimes in front of
us". Nothing of it ships; the addresses stay in `VCSAddresses.h` and this section is the record.

Read the last part first if you are about to try this again: **the answer is known and the answer
does not help.** The cost of rediscovering that is a day.

#### The four negatives were sound and all looked in the wrong place

The conclusion drawn from them was stated twice in this file - *"what VCS draws is what the
streamer has loaded, and nothing else decides"* - and two things from play contradicted it flatly,
both of them already written down here:

- *"Geometry you have never looked at cannot cast. Walk into a street facing away from a building
  and it casts nothing until you have seen it once."*
- HOR+ widescreen makes *"objects pop in and out of view in these edges"* - at the EDGES, which is
  where a 70-degree cull's boundary falls inside an 84-degree picture.

**An observation from play outranks an inference from four negatives.**

#### The spin, which settled it in one run

Teleport to a fixed spot, point the chase camera four ways with its look command, and histogram the
WORLD bearing of every object-sized draw the game submits. A game drawing whatever is loaded gives
a distribution that holds still while the camera turns. Per mille by world bearing, 15 degrees a
bucket, from 0:

```
yaw   0    161 207 111  14   2   0 ... 0   2   2   6  17  84 173 217
yaw  90      0   0   0  44 352 299 130 119  41   7   1   0 ...
yaw 180      0 ...   0  12  30 116 271 254  56 120  99  17   8   4   8   0 ...
yaw 270      0 ...                   1   0   0   0   2  63 214 274 202 181  47  13   0   0
```

Four narrow cones, each centred on where the camera points. **VCS culls the map to its camera,
hard**, and the four inert mechanisms below were simply not it.

#### The lever is tangents, not planes

`RwCamera + 0x60` is the RenderWare **view window** - the tangent of the half field of view per
axis, reciprocals at `+0x68` - reading `0.7002` = tan(35 deg). It sits 24 bytes from the near clip
this fork had been setting for weeks. There is no frustum in the camera at all.

Found by breaking on a WRITE to it and reading the caller off the stack: one writer,
`RwCameraSetViewWindow` at `0x088906e4`, one caller at `0x0893a7a4` taking
`(RwCamera *, void *, float tanHalfFovX, float aspect)` in `$f12`/`$f13`. It stores `$f12` as the
horizontal half-window and `$f12/$f13` as the vertical, **so scaling both widens horizontally and
leaves the vertical alone** - the scale cancels in the quotient.

Hammering the value from the debugger does nothing: the game rewrites it inside 80 ms. Nopping the
two stores and forcing it wide is what proved the lever - the received arc went from 105 to 150
degrees and the draw count rose 37%.

#### Splitting the cull from the picture, which also works

The view window is what the PROJECTION is built from too, so widening it alone is a fisheye. But
the setter derives the reciprocal from the value it has just stored - `div.s $f13, $f13, $f14` at
`0x08890754`, `$f13` = 1.0 - and **the projection follows the reciprocal**. Dividing `$f14` back by
the same factor there hands back the stock reciprocal exactly. Measured: a 4x view window with a
stock reciprocal renders at the normal field of view.

Two `REPFLAG_HOOKENTER` replacements, both guarded on `$a0` being the scene camera. At the shipped
default it read `viewWindow 1.4004` against `recip 1.4281` - a 109-degree cull behind a 70-degree
picture, 2.5x the object draws, 30 fps, and a screenshot pixel-identical to stock.

**And then it was played, and objects vanished in front of the player.**

#### Why it fails, as far as it was taken

Not investigated to a root cause, because the feature was abandoned first. The strongest candidate
is **resource residency rather than culling**: an instance record carries `bit 15 = hidden`, which
the game recomputes on every cell swap and sets when the resource is not loaded, and the streaming
heap runs **95% full at retail**. Widening the cull hands the streamer far more to want; a streamer
that cannot keep up marks instances hidden, and nothing about that rule distinguishes an instance
in front of the player from one off to the side.

Three things fit that and nothing else does:

- **it needs movement.** Every screenshot taken standing still was clean, across a sweep of five
  factors and four camera angles, which is exactly how a streaming shortfall hides from a fixed
  fixture;
- **it is intermittent and position-dependent**, which is what a heap under pressure looks like;
- **the player's own word was "deloaded"**, which is what the mechanism is called.

If anyone retries this, **raise `World memory` first and measure the streaming heap's free bytes
with the cull wide.** That is a ten-minute test and it either explains the whole thing or clears it.

#### The other ceiling, measured before play found the real one

Past about 2.2x the game draws flat sheets of its own sea across the grass - a world-anchored quad
with straight edges. Swept at one spot with everything else held still: **1.0, 1.5 and 2.0 clean,
2.5 and 3.0 plainly wrong.** Four suspects were ruled out before accepting it, and three of them
looked like the answer:

| | ruled out by |
|---|---|
| this fork's water pass | `Water = 0`, no water lines in the log at all, artefact unchanged |
| this fork's shadow pass | `Shadows = 0`, artefact unchanged |
| PPSSPP's bounding-box cull | widening its matrix took culled draws from 1293 to 514, artefact unchanged |
| PPSSPP's range culling | `DisableRangeCulling` added for this disc, artefact unchanged; reverted |

And the control that says what it IS: **widening the cull and the projection TOGETHER is clean at
any factor.** Same spot, same camera, 140 degrees both ways - no wedge. The engine objects to the
two numbers disagreeing, not to the width.

**So there are two separate failures here**, and it is worth keeping them apart: the sea wedge is
the engine noticing the split, and the vanishing objects happen at a width where the wedge does
not. Fixing either one would not have fixed the other.

#### Two real bugs found on the way, and both are kept

Neither is about the cull; both were exposed by it.

- **PPSSPP culls against the game's projection**, through `gstate_c.cullMatrix` in
  `UpdateMatrixProducts`. With a 140-degree cull behind a 70-degree projection it threw away
  **1293 draws a frame against 280**, including ground that was on screen. That code is reverted
  with the rest, but the fact is worth keeping: **the emulator assumes no game submits geometry
  outside its own projection**, which is true of every game that is not being interfered with.
- **The software transform never sees `u_proj`.** PPSSPP sends a draw down `RunSoftwareTransform`
  whenever `ClipInfoFlags::SoftClipCull` is set, and it projects with `gstate.projMatrix` directly
  - so the widescreen row's HOR+ correction was missing from those draws. About one 3D draw a
  second in VCS, so it was never the visible bug, but it is a genuine pre-existing fault and the
  fix is KEPT.

A third, found while tidying: `g_widescreenSquash` and friends were never reset on shutdown, and
`RefreshWidescreen` only runs from `Tick`. Boot VCS with widescreen on, quit, boot anything else in
the same session and it inherited VCS's squash. Fixed in `Shutdown`.

#### The instrument is kept, and it is the part worth having

`CaptureStats::angleHist` and the counts beside it, on the debugger's **Shadows** tab and in the log
once per boot: for every object-sized draw the game submits, the angle between the way the camera
faces and the way the object lies, flattened onto the ground plane. Four things about how it is
measured, each of which would otherwise make it useless:

- **Yaw, not the solid angle.** The camera sits behind and above the player looking down, so on a 3D
  measure the pavement at his feet is 60 degrees off forward.
- **Object-sized draws only**, by `maxCasterSpan` alone. The sky and the map-spanning ground quad
  have a centre and it is not a place anything is.
- **Only past 30 units** (`kHorizonFarDistance`). A frustum keeps whatever OVERLAPS it, so an object
  is kept while its centre is up to a half-angle plus its own angular radius off forward - and close
  to the camera that radius is enormous. A bin three units to the player's left is at 80 degrees
  whether the cull is 70 wide or 160.
- **The SHAPE, not a percentile and not the maximum.** The distribution is a plateau out to the cull
  edge and then a long thin tail: the 99th percentile sat at 100, 96 and 97 degrees across three
  settings whose geometry counts differ by nearly four times, and the per-frame maximum reads 174
  degrees at the stock cull because one stray draw in six hundred frames sets it.

```
stock       401 337 211  22   4   7   7   5   0   0   1   0     shoulder at 45 deg
109 deg     230 240 342 140  33   4   5   2   0   0   0   0     shoulder at 60 deg
140 deg     146 177 228 207 179  44  11   3   0   0   0   0     shoulder at 75 deg
```

`VCS_HORIZON_TRACE=1` prints the same population bucketed by WORLD bearing once a second with the
camera's own bearing beside it. That is the control the relative measure cannot be, and it is what
the spin above was read off.

#### 360 degrees was never available

A planar projection cannot open to 360 - the tangent runs to infinity at 90 degrees each way - and
this game caps out well before that for the reasons above. **Behind the player is the shadow
module's caster cache and always will be**; it already holds every cell within `cacheRadius` (350
units) for good.

#### Method, because five things in a row looked like the answer

The artefact hunt went: the water pass, the shadow pass, PPSSPP's frustum cull, PPSSPP's range
culling, the software transform. Four were wrong and one was a real bug that was not THAT bug -
the shape this file has now recorded six times.

What ended each round was an instrument rather than an argument: the spin histogram for "is there a
cull", a draw-and-cull count for "who is throwing geometry away", a per-draw counter for "does this
path even run" (one draw a second), and a control run with the game widened by hand and the
renderer left alone for "is it the widening or the correction". Every one cost less than the round
of reasoning it replaced.

**And the one that was not instrumented is the one that sank it.** Every check of "is this width
safe" was a screenshot from a fixed spot with the player standing still - five factors, four camera
angles, all clean - and the failure only exists while moving. A fixed fixture is the right tool for
comparing two builds and the wrong tool for asking whether a build is *correct*: it holds still
exactly the variable the streamer cares about. **Anything touching streaming has to be judged while
travelling, by somebody playing it.**

### The hunt for the visibility function, and the frustum that decides nothing

**Status: CLOSED - and not here. The cull is the RenderWare view window, and widening it turned out
to be worse than leaving it alone; see "VCS culls to the camera, and widening it is worse than the
pop-in".** What follows is the record of two candidates that really are dead storage, kept because
both look exactly like the answer and a future hunt should not spend a session on either again.

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

**Where to look next** - written before the answer was found, and the first line of it was right,
which is worth noticing: the render camera WAS the object, and what it culls with is four bytes from
the near clip this fork had been setting for weeks.

- **`CCamera + 0x7bc` holds the render camera.** At `0x08a23ea0` the caller fetches it and copies
  the camera's position (`+0x30..0x38`) and basis (`+0x10..0x28`) into it. That object is what the
  renderer actually draws through, and whatever it culls with is reachable from there. **It is
  `+0x60`, the view window** - and the way in was a WRITE breakpoint on it, not a search for code.
- **Find the visible-entity list rather than the test.** In this engine lineage the scan fills a
  large array of entity pointers each frame. An array of many consecutive pointers into the entity
  heap that changes as the camera turns is findable with `memory.search`, and a write breakpoint on
  it lands inside the scan - the same move that found the frustum function, aimed at the right
  structure this time.
- **Consider that there may be no per-entity frustum cull at all.** DISPROVEN, and the test named
  here is exactly the one that disproved it - the draw count, or better the bearing histogram, with
  the camera pointed several ways from one spot. **Turning the camera for that test is no longer the
  hard part**: the chase camera's block takes a look command (write yaw at `+0x30`, flags at
  `+0x2c`), which is what made the spin possible. The paragraph below is what stood in the way
  before that existed. - writing `CameraYaw` from the debugger moved the
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

**What VCS draws is bounded by what the streamer has loaded**, and the section below found that
bound: the streaming heap is 4.75MB and runs 95% full. This used to say "and nothing else decides",
as something that "is not a hypothesis any more" - and that was the overreach, because four levers
being inert says only that the lever is elsewhere. Play says something else decides too; see "The
four negatives cannot all be right". The note that
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

**The vault's world query could also crash a load, at the right partition size.** Measured
2026-09-15: loading slot 3 at `World memory` 1, on foot, died as `Bad Execution Address` with the PC
inside the world query's old block and `RA 08000038` - the return of an enqueued call. The vault
probes nearly every frame on foot, so a request is almost always waiting; `WorldQueryDispatch` sent
it on the game's first syscall after the load, into whatever the state held at that address, a
vblank before `InstallWorldQuery` could notice the block had gone. It checks the block's magic before
enqueueing now. Worth suspecting that the `0x0bffecc0` crash above - top of a 64MB partition, which is
where that block is allocated - was this too, rather than the partition alone; it has not been re-run
to find out.

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
