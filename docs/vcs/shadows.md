# Dynamic sun shadows

> Read before touching `GPU/Common/VCSShadow.*` or the shadow capture hooks in the draw engine.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

### Dynamic sun shadows

**Status: working, confirmed in play, on by default.** Three passes inside every frame, at full
speed (30.0 fps measured on an ordinary Vice Point street with 1369 draws and ~146k vertices in
the frame). `GPU/Common/VCSShadow.cpp` holds all of it; the hooks are four lines in
`DrawEngineVulkan` and one in `GPU_Vulkan`. The row is `Shadow quality` on Display Setup → ADVANCED, and
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

#### Opening the map broke the shadows until a restart, because the framebuffer grew

Reported as shadows "acting weird" after MAP, never after BRIEF or STATS, and a settings toggle did
not bring them back. Nothing in the shadow pass was wrong with its own state; the framebuffer under
it had changed size. Measured from PPSSPP's own FrameBuf log with the map opened over the debugger:

| | game draws | framebuffer |
|---|---|---|
| before the map | 512x320 | 512x320 |
| after the map | 512x320 | **512x401** |

The chrome fix stretches `Map_AE` to `backdropHeight` rows, the map draws down to row 400, and PPSSPP
enlarges a framebuffer as soon as something draws past its edge - and shrinks one again only once it
has more than halved (`FramebufferManagerCommon`, the `newHeight * 2 < bufferHeight` test). So one
visit to the map leaves the rest of the session drawing 320 rows into a 401-row buffer. BRIEF and
STATS have no widget stretched past the edge, which is the whole difference.

Both passes sized their viewport from `curRTRenderWidth/Height`, which is the whole BUFFER, while the
post-projection matrix maps into `curRTWidth/Height`, the part the game DRAWS - the same number until
a buffer grows. After the map the mask was spread over 2406 pixel rows instead of 1920, and every
shadow slid 1.25x down the screen. `DrawnArea` now sizes it the way PPSSPP sizes its own viewport
(`ConvertViewportAndScissor`): drawn size, times render pixels over buffer size. The water pass had
the same maths and also copied the whole buffer for its reflection, so it copies only the drawn part.

The log says `the game draws WxH into a WxH framebuffer ... the mask covers WxH` whenever the sizes
change. Two different framebuffer sizes in that line is this mechanism engaging, not a fault.

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

**Only channel 0 is the sun, and the street lamps are why.** Reported as some palm trees behaving
strangely while driving from about 21:00 until morning. VCS lights the things that move with its
time-of-day directional on channel 0, and near street lamps it adds per-object directionals on
channels 1-3, each pointing from its lamp to that object - full white, a different direction for every
object, sweeping as you ride past. The pick took the brightest light, so after dark a lamp beat the
dim moon and every shadow swung towards the nearest lamp; the palms along the road showed it most. A
temporary jump log on a night ride settled it: 35 jumps in about twenty seconds, every one to channel 1
or 2, while channel 0 held the setting sun and then the moon. The other channels are still recorded
for the Shadows tab; they just never win.

**The sunset swap is eased rather than snapped.** When the sun sinks under the horizon it is replaced
by the mirrored moon, lifted to 0.35 - the same log measured that as a 16-degree jump in one frame -
and the strength changed from the sun's luminance to `moonStrength` in that frame too. The game also
moves its sun in 2.7-degree steps every half second at dusk. `TurnTowardsSun` turns the cast direction
towards the pick at 8 degrees a second, which keeps up with the game's 5.4, and `EaseShadowStrength`
moves the strength at 0.35 a second. A change over 60 degrees - a save loaded at another hour - still
snaps. Both log when they act. Confirmed in play, along with the lamp fix above: the palms hold still
at night and sunset no longer jumps. The jump log that found both was temporary and is gone.

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

#### A cell took whole FLUSHES, and a flush is not an object

**Status: fixed in code 2026-09-30, NOT yet verified in play.** Reported after everything above as
"a building in front of me, I turn my back on it, and its shadow often goes out" - buildings worst,
palms mostly fine.

`RememberCaster` put a whole draw into the cell of the draw's centre, and a draw is a flush: PPSSPP
merges consecutive draws with the same state, and which ones it merges depends on what the game
culled, so it changes as the camera turns. That is the fact the props and the fronds had already
recorded - three lamp posts in one batch, frond flushes 500 to 3000 units wide - and the cache was
never taught it. The sequence that loses a building: drawn alone, it is in its own cell; a
neighbour with the same texture comes into view, the two merge, and the flush's centre lands in a
cell between them. The building's own cell is still wholly on screen, so it is REPLACED - with
whatever else is centred there, without the building. The cell between is half off screen, so it
keeps what it had and throws the flush away. The building is in no cell, and goes out when it
leaves the view.

Palms survived by accident rather than design: cut-outs are remembered per flush too
(`RememberCutout`), but a frond flush is so wide its centre lands in a cell too far away ever to be
replaced, so it only ever fills and is never overwritten.

Each triangle now goes to the cell of its own centroid, which is independent of how draws were
grouped. Only the vertices a casting triangle uses are stored, where the whole draw's were before,
so a cell holds less and fills its u16 range later. `VCS_CACHE_TRACE` and the Shadows tab count
draws split across cells and triangles dropped by a full cell; the second should read zero.

**That build on its own made the flicker WORSE, reported from play the same day**, and the
reasoning that said it could not was the mistake: "a triangle centred in a cell wholly on screen was
drawn, so a capture of that cell misses nothing". Drawn is not captured. A flush merged past
`maxCasterSpan`, or a resource still streaming, leaves an object out of one frame's capture, and
with cells this small the cell is still touched by whatever else was drawn there - so it was
replaced without the object. Whole-flush bucketing had hidden that by accident: a missing flush
touched no cell, so its old cell was replayed over the gap.

So the plain half is now a quality ladder instead of a wholesale replace, which is also the rule
asked for from play - keep whichever LOD of a thing is best, across the next unload and load. A
close capture (within `cacheReplaceRadius`) beats a far one; between two captures from the same side
the one with MORE triangles wins; a capture with fewer triangles than the memory is taken as
incomplete, and the memory is kept and replayed over it. Static scenery does not shrink, so this
converges on the most complete capture and never regresses. Two exceptions, both deliberate: a far
memory yields to a close sighting that is wholly on screen whatever its count (the stand-in is not
the building), and the light moving more than ten degrees since the commit lets a slightly smaller
capture in, because it changes which faces are kept. Cells nearer than about forty units are never
wholly on screen, so this rule is also the only way they ever improve on their first fill.

The cost is ghosts: something static that genuinely goes away stays in the shadow. Nothing in this
game's scenery does that often enough to matter, and a knocked-over bin that lands in another cell
is the case to look at if one is ever reported.

Cut-outs still go by the flush. They work, and making them sound as well means one `CutPiece` per
cell per draw - worth doing if a palm is ever reported doing what the buildings did.

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
| `0000000022d2a1e9eb997d9a` | 128x128 | `Misc/vcs_no_shadow_128x128.png` - the motorcycle's |

The car's was the one no scan of the dump could find, because it was never dumped: the pack already
mapped it, to `Particles/00000000f5df92ec0336c0b3.dds`, filed among the particles. It was found with
the Debug build's ImGui **Textures** window, which the fork extends to list only textures drawn in the
last couple of frames (with a size cap) and to show the selected one's key as `textures.ini` spells it,
with Copy key and Copy as empty buttons. Stand next to the thing, filter, click - that is the tool for
the next one of these. Its empty PNG matches the size of the replacement it displaces, 128x128.
The motorcycle's was the same case - `Particles/0000000022d2a1e9eb997d9a.dds`, also 128x128 - and
was found in play the same way.

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

**It was also hiding the street lamps' pools of light**, and that one is fixed (2026-09-30, not yet
seen in play). GTA draws both through one shadow system - `SHADOWTYPE_DARK`/`INVCOLOR` for the blob
under a ped or a car, `SHADOWTYPE_ADDITIVE` for the light a lamp or a headlight throws on the road -
so a lamp's pool is small, flat, writes no depth and lies under the lamp post. It was dropped while
the post was on screen and popped back when the post left it, reported as a hard-edged shadow
appearing with the lamp just behind the camera. `BlendOnlyBrightens` keeps anything whose blend
keeps the destination whole (`FIXB` white, or `MAX`) out of both the positional and the learned
paths: a shadow can only darken.

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
far clip and the LOD distance multiplier - and not one of them decides anything for the map. See
"The LOD multiplier was patched properly" below for them in one table.

**That sentence used to end "so VCS draws whatever the streamer has loaded, and widening the view is
a STREAMING question and nothing else", and it was wrong** - the paragraph above it says a building
you have not looked at casts nothing, which a game drawing whatever is loaded could not do. The cull
was found - it is the RenderWare view window - and **widening it made things worse**, dropping
objects the player was looking straight at. See "VCS culls to the camera, and widening it is worse
than the pop-in". So this note stands exactly as it did: what has not been looked at cannot cast,
and the caster cache is the whole of the answer.

**A remembered frond needs its texture to still be in the cache.** Off-screen fronds are replayed
only if the texture cache still holds a texture at their address, so a palm whose texture has been
freed stops casting until the game draws it again. See "Palm fronds cast through their own texture".

**The moon is the sun's light mirrored, not a real lunar position.** It puts night shadows
somewhere plausible rather than somewhere correct.

#### The cache was measured WHILE DRIVING, and it is not the thing that is short

**Status: measured 2026-09-22, over a 3,500-unit loop at 36 units a second.** Prompted by the idea
of building a second camera pointed backwards so the game would submit geometry behind the player
for the shadow pass to use. The cache is what that would feed, so the question to settle first is
whether the cache is actually short of anything - and it is not.

`VCS_CACHE_TRACE=1` prints cells held, cells replayed, cells replayed BEHIND the camera and the
rebase and clear counts, once a second. Four legs of a square, continuous movement at the speed of
a car, at a locked 30fps:

| | |
|---|---|
| cells held | **200 - 460 throughout**, never draining |
| GE rebases | **20**, each carrying 350 - 460 cells with it, none lost |
| whole-cache clears | **1**, at a genuine island change |
| cells replayed | 6 - 40 a frame |
| of those, behind the camera | **up to 16 a frame; about HALF the replayed ones at rest** |

So the cache retains across travel, follows the world as the GE space rebases under it, and the
geometry it hands the depth pass is about half from behind the player. That half is the whole
feature working.

**The number that answers the camera idea is the ratio.** At rest it held 363 cells and replayed
23 - **it carries about eighteen times more geometry than the depth pass ever draws.** What decides
whether a remembered cell casts is the `reaches` test, not whether the cell exists: `cascadeRadius`
is 70 with `centreDistance` 40 ahead, so the box spans 30 units behind the camera to 110 in front,
extended 300 units UP-SUN by `casterReach`. A second camera would pour more geometry into a cache
that is already oversupplied by a factor of eighteen, and the depth box would go on drawing the
same cells.

**It would answer exactly one thing, and that thing is still true**: geometry never looked at at
all. The cache can only remember frames the game drew, so a building you have driven past facing
away from casts nothing until you have seen it once. That is the note above, unchanged - and it is
a much narrower gap than "shadows go out behind you", which this measurement shows the cache
already covers.

**Against it: the cost is the failure this file already recorded.** Making the game render a second
view means the game's own entity scan, streaming and resource residency for two views at once, out
of a streaming heap that runs 95% full at retail - and merely WIDENING one view was measured to
make the streamer drop objects the player was looking straight at. See "VCS culls to the camera,
and widening it is worse than the pop-in". A second camera is that experiment with more to hold, not
less. **The caster cache is a backwards camera already, built for free out of frames the game had
drawn anyway** - which is why it was built that way rather than by asking the game for more.

**Two traps in the measurement itself, both of which faked a broken cache before the harness was
fixed.**

- **Pacing the teleport on the WALL clock.** The game stalls for seconds while an island streams;
  a step every 30ms banks up through the stall and arrives as a 230-unit jump in one game frame,
  which is over `RebaseCachedCasters`'s 40-unit threshold, so the cache was cleared nine times in
  thirty seconds and the log said "the camera jumped 228 units". Every one of those was the
  instrument. Paced on `FrameCounter` instead, with a cap of two frames of travel per step, the
  largest single step was 2.4 units and the clears went to one - the island change, which is
  correct. **A harness that drives the game has to run on the game's clock**, the same rule the
  cheat sequencer and the aim model already follow.
- **Appending a marker to the log file with `echo >>`.** The emulator holds its own file offset, so
  the marks were overwritten as it wrote. Correlate by printing a wall clock from the driving
  script and matching it against the log's own timestamps.

And one the previous rounds had already taught, confirmed again: **standing still reads as a
perfectly healthy cache**, because the same cells replay with byte-identical counts for minutes.
Every number above only moves while travelling.

#### Half of what casts behind you is a LOD stand-in

**Status: measured 2026-09-22, same harness, ULTRA.** The measurement above cleared "the cache is
empty behind you" and did not look at what the cache is FULL of. It is mostly stand-ins.

A cell takes full detail only from a sighting inside `cacheReplaceRadius` (150 units). An EMPTY
cell takes whatever it is offered at any distance out to `cacheRadius` (350) and then keeps it, and
past ~266 units the level archives hand over the LOD stand-in or a building with parts missing -
see "The map is baked per cell". So a building you only ever passed at 200 units casts a blocky or
holed shadow **for as long as it lives in the cache**, and every count before this one reads that
cell as healthy. `Bucket::fillDist` records the distance its contents were committed at, which is
what makes the two distinguishable.

Over the same paced square - 3,700 units, four legs, two island changes:

| | all driving | warm cache only (>=150 cells) |
|---|---|---|
| held cells that are stand-in | 75% | 79% |
| REPLAYED cells that are stand-in | 29% | 22% |
| **replayed BEHIND the camera that are stand-in** | **43%** | **50%** |

The warm-cache column is the control that matters: it excludes every post-clear rebuild, where a
fresh cache is filled from wherever the player happens to be, and the number goes **up**. So this is
not an artefact of the island changes.

**The shape explains why it hid.** Stand-ins are 75-83% of the cache and only ~7% of the cast
GEOMETRY at rest, because they live out at the rim of `cacheRadius` where the cascade rarely
reaches. Behind the camera is the exception - it is exactly the population you have driven past and
possibly never come within 150 units of - which is why the one place the number is bad is the one
place play reported a problem.

**This is the same symptom "Cast once, then there" already fixed once, in the half it could not
reach.** That section made a far sighting stop OVERWRITING a near one; it could not conjure a near
sighting for a building on the far side of a canal, and nothing built out of the game's own
rendering ever can.

So the ordering for fixing it, cheapest first:

- **`cacheReplaceRadius` is conservative.** The archives bake full detail to ~266 units from the
  CELL, and a cell is up to ~80 across, so the guaranteed radius from the camera is nearer 180 than
  150. Worth a try and worth almost nothing - it moves the boundary, it does not remove it.
- **Persist the cache to disk.** Cells upgrade whenever the player does eventually drive within 150,
  so over a few sessions everything you actually drive past becomes full detail. No format
  reversing. Does nothing for what you never get close to.
- **A geometry source that is not the game's own rendering**, which is the only thing that fixes
  "never within 150 units". That is where extracting the map from the PS2 disc becomes a live
  question rather than a speculative one - the PSP-native mesh format has no public tooling, the
  PS2-native RenderWare one has twenty years of it, and the cell grid and instance records are
  already decoded here.

**What is NOT measured is whether it LOOKS wrong.** These are counters. A stand-in's shadow may be
perfectly acceptable at the distance it is cast from, and the cheap way to find out is a picture
rather than another number: one build where stand-in cells do not cast at all, and whatever
disappears from the frame is what they were contributing.

#### A thin lit ring round the player was the near cascade's edge fade

Reported: "when under a building's shadow, it's like a 2px cut in the otherwise full shadow that
follows the player in distance, like an end of a radius". The mask shader fades a cascade's shadow
towards lit near the cascade's edge (`edgeFade`, 0.15 of the box), and it did that for the near
cascade too. But the near cascade hands over to the far one at 0.92 of its box. So just inside the
handover every shadow was faded to about half (0.53 at 0.92), and one step further out the far
cascade drew it at full strength: a lit ring at the near box's radius that moved with the player.
The fade now applies to the far cascade's edge only, which is where shadows really end. Past the near
box the far cascade carries them.

#### In the rain the passes ran on a quarter of the city: an early 2D draw

Reported: "shadows in rain act still strange... it feels like the same case when I opened the map,
they were very weirdly misplaced". Forcing rain (weather 2) and reading the log settled it. In the
rain the "second frame" re-capture fired on every frame, 1000 times in 40 seconds, and the passes
ran twice a frame. A probe on the restart showed the same framebuffer, the same camera, no mirroring
(view determinant 1.000). The seam the passes first fired at was a blended, textured 2D draw on the
same texture every frame (`09715bd0`), with about 10,000 of the frame's 42,000 caster indices in.
That is the game's rain overlay, drawn a quarter of the way through its world. So the passes ran on
part of the city, the rest arrived as "a second frame", and the passes ran again on that rest: two
composites, each from part of the world. That, not the puddles, was what `hideInRain` hid.

The answer is to learn the draw. When a frame's passes run twice at two DIFFERENT draws, the first
draw's texture is remembered (`s_earlySeamTex`, up to four). From then on a 2D draw with it is
passed over, the capture carries on, and the passes run once, at the seam after the world.
Requiring two different draws matters. A real second frame, which happens a few times a session in
clear weather, runs at the HUD's first draw both times, and learning that texture would pass over
the only seam there is. The textures are forgotten after 600 frames without one. The log says `comes
before the world is finished` when it learns one and `passed over the early 2D draw` as it works.

Two approaches that did not work, recorded. A share of the casters ("defer a seam reached with under
60% of last frame's casters") failed because an early seam arrives with anywhere from a quarter to
over half of them, depending on the view. The first build of it also summed a restarted frame's two
captures for the baseline, so no seam ever reached the bar and the shadows went out completely. The
cheap guard beside it stays: a re-capture with under a quarter of the first's casters is not a
second frame, and the first composite is kept.

Whether the pause map's misplacement is the same mechanism (a 2D draw before the world when the map
closes) is not measured.

#### The dark band at dusk was the shadows, and a low sun now fades them

Reported from play: "during around 19:00 - 21:00 the world gets very dark (sun is falling and moon is
slowly rising), then it gets brighter because of the moon as it should". Measured at one spot with
the clock set to 18:00, 19:00, 19:30, 20:00, 20:30 and 21:30, shadows on and off. Off, the game's own
dusk is dim but even. On, 19:00 and 19:30 went grey-green dark. A sun a few degrees up throws shadows
long enough to cover nearly everything, and it kept the full strength its colour carries until it
crossed `kMinSunElevation` (0.05, about 3 degrees). Only then did the moon take over: lifted to 0.35,
at `moonStrength`, which is why it got brighter.

`lowSunFade` (0.26, about 15 degrees) fades the sun's strength with a smoothstep from there down to
the moon threshold. The swap now starts from nothing and the moon's shadows ease in through
`EaseShadowStrength`. Ambient occlusion follows, since it takes its share from the composite's
strength. Re-measured: 19:00-20:30 now look like shadows off, with the moon's arriving after 21:00.
The slider is on the Shadows tab.

#### Shadows go out in the rain - no longer

**Superseded 2026-10-06: `hideInRain` is off by default.** The main thing it hid was the passes
running twice a frame in the rain - see "In the rain the passes ran on a quarter of the city". The
puddles also mirrored the shadows, because the water pass runs after the composite and reflects the
frame. For that, `VCSShadow::CompositedMask` hands the road pass this frame's mask and the tint and
strength it was composited with, and the water shader divides that factor back out of what a road
reflects. See "Puddles reflected the shadows" in water.md. The fade is kept as a switch.

The original note: reported from play, when it starts raining the shadows project strangely onto the
puddle reflections. `hideInRain` faded them out over `rainFadeSeconds` (2) and keeps them out while the
roads are wet, because the puddles outlast the rain. They start back once VCSWater's lagged wetness
drops under `rainShadowsReturnWetness` (0.30, about 10% of a road still wet) and are fully back at
half of it (2%). It was 0.1 down to 0, reported as shadows returning only once every puddle had gone.
With the water pass off it
goes by the game's rain level alone. Fully faded, `OnFlush` skips all three passes but the capture
carries on, so the caster cache is warm when they come back. A skipped frame is not a BLINK; the log
says `off for the rain` and `back, the roads are dry` at NOTICE; both knobs are on the Shadows tab.

#### ULTRA flickered with the camera because the box followed the VIEW

Reported as "ULTRA flickers when the camera moves", with the suggestion to cast less if that was the
cause. It was not how many were cast - ULTRA runs at full speed - but WHERE the box was: centred
`centreDistance` (40) along the view direction, so a half turn of the camera swung the whole cascade
80 units. Large casters at the box's edge - buildings, which only ULTRA casts - entered and left the
shadow map, and the remembered cells' replay test (`reaches`) is measured from the same centre, so
whole building shadows came and went. HIGH's casters are people, cars and props a few units from
the player, inside the box wherever it points, which is why HIGH never showed it. The near cascade
had the same habit at a sixth of the scale, moving things between the sharp tile and the soft one.

The lead exists so a car does not outrun its shadows, which is a lead along the TRAVEL. So that is
what it follows: `CascadeLead` smooths the camera's horizontal velocity over half a second and
leads by 1.2 s of it, clamped to `centreDistance`; a jump over fifteen units in a frame is a rebase
or a teleport and leaves the velocity alone. Standing and turning, the lead is zero and the shadow
map does not change at all. Centred on the camera at rest, the box reaches as far behind as ahead,
so `cascadeRadius` went from 70 to 85 to keep most of a street's reach in front - about 4.2 cm a
texel in the far tile.

Checked with a slow 360 turn on ULTRA on Ocean Beach at 16:00, frames grabbed continuously: the long
building and palm shadows on the walkway hold their shapes through the turn. There is no clean
"before" capture to set beside it - the player was playing in the same instance during that one.

#### Ambient occlusion, read off the mask's depth (prototype, 2026-09-30)

**Status: prototype, off by default.** `Ambient occlusion` on the Graphics page (greyed while the
shadows are off, via `enabledBy(&VCSShadow::g_active)`), every knob on the Shadows tab. Branch
`vcs-ambient-occlusion`.

It is Scalable Ambient Obscurance (McGuire, Mara and Luebke 2012), and the reason it is cheap here
is that the expensive half already exists: the MASK's depth buffer is every captured receiver drawn
through the camera's own `view * proj * post`, lined up with the frame to the pixel. So occlusion
is one full-screen pass over that depth plus a two-pass edge-aware blur, all into targets of its
own, then one more multiply in the existing composite, before the HUD. Nothing goes back through
the geometry. `RenderAO` runs between `RenderMask` and `RenderComposite`.

- **Positions are rebuilt relative to the camera.** `inverse(cameraViewProj) * T(-camera)` is folded
  on the CPU, in double - the depth row is a perspective squeezed into [0,1], and a float inverse
  turns the far half of it into noise. Relative to the camera the shader's numbers stay small.
- **Depth is sampled at the snapped texel centre, and the position rebuilt there.** Rebuilding at
  the unsnapped UV puts points up to half a texel off their own surface.
- **Normals come from the nearer neighbour on each axis**, so a silhouette does not build a normal
  across the edge into whatever is behind it.
- **The blur keys on depth packed into green and blue**, 16 bits as two whole numbers out of 255
  (which an 8-bit channel stores exactly), and stops on a depth step RELATIVE to the depth - an
  absolute threshold stops dead on distant road, whose depth changes faster per pixel.
- **Pixels per unit** is measured by pushing a one-unit step through `cameraViewProj`, not read off
  the projection, because that matrix also holds the PSP viewport and the render-target remap.
  Logged as `VCS AO: ran at WxH ... N px per unit at 1 unit`: 1625 at 2048x1280, a ~43 degree
  vertical FOV, which is believable for VCS.

**It only applies a share in sunlight** (`aoInSun`, 0.35). Occlusion removes ambient light, but the
composite only has the finished colour; at full strength a sunlit corner goes as dark as a shaded one
and reads as dirt. "How much of the light is sun" is read back off the shadows' own eased strength
(`tint.a / strength`), which already carries the sun's brightness, the moon and the rain.

**People get a share too** (`aoOnPeople`, 0.3), and this was the one real finding. SAO weighs a
nearby surface just above the tangent plane very heavily, so on a body the arms darken the whole
torso: measured with the split view straddling the player, the AO half of the suit came out a flat
shade darker and muddier, not just in the creases. The mask now marks people in its ALPHA - they are
already drawn in their own call for `pedReceiverBias`, so it is a uniform, not a pass - and the
composite scales their occlusion. People still occlude the ground: the contact shade at the feet
is unchanged. The ped stream is split off whenever AO is on, even with `pedReceiverBias` at 0.

**Measured on the Release exe at 2048x1280 (internal res Auto, 1080p screen): 30-31 fps with it on**,
the game's own cap - so it fits in the frame on this APU. That is "no visible cost", not a
millisecond figure; there is no GPU timer here.

**Testing it with nobody at the keyboard.** Every boot resumes somewhere different (see
boot-and-saves.md), so two runs are never the same picture. `VCS_AO_SPLIT=1` applies it to the left
half only - with and without in one frame - and `VCS_AO_SHOW=1` draws the raw estimate instead of
the frame. Both are also checkboxes on the Shadows tab. The first raw capture showed the crease
where pavement meets wall, the ledges on a facade, the contact shade under the player's feet, and
white sky.

**Known, and left for after a look:**
- It only exists while the shadows do: off with the shadows, and skipped with them in the rain -
  which is a pity, since overcast is when ambient light is all there is.
- It sees only what the capture accepted. Foliage cut-outs, particles and glass get whatever the
  surface behind them gets, exactly as the shadow mask does.
- Lit windows and neon at night are darkened like anything else.
- The composite's upsample is not depth-aware, so below `aoScale` 0.5 it will halo.
