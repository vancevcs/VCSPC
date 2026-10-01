# Water and rain on the roads

> Read before touching `GPU/Common/VCSWater.*`, the road mask, or the water dev environment variables.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

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
