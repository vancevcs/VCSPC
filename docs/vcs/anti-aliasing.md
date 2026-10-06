# Anti-aliasing (SMAA)

> Read before touching `GPU/Common/VCSSmaa.*` or its hook in the draw engine.

### SMAA over the 3D scene, before the HUD

**Status: working, checked on a split screen in play, on by default on a desktop and off on a
phone.** The row is `Anti-aliasing` (OFF / SMAA) on Graphics → ADVANCED. Everything is in
`GPU/Common/VCSSmaa.cpp`, hooked into `DrawEngineVulkan::Flush` after the shadow and water passes, so
it smooths the finished world with its shadows and sea, and runs before the HUD is drawn, so the
text stays sharp.

```
copy      the game's framebuffer, blitted into one of ours, so the passes can read it
edges     luma steps against the left and upper neighbour, with SMAA's local contrast
          adaptation (threshold 0.1, factor 2.0)
weights   for each edge pixel, walk along the edge to both ends (up to 24 px), read which way
          each end turns, and work out the area the silhouette line covers
resolve   one triangle back into the game's framebuffer (RGB only, the game keeps things in
          alpha), blending along whichever direction carries more weight
```

**Why not a post shader.** PPSSPP's post-shader chain runs on the finished frame, HUD included,
and each pass in it sees only the pass before it. SMAA's last stage needs the original image *and*
the weights from the stage before it.

**The middle stage is not quite SMAA.** Real SMAA reads the coverage area from a precomputed lookup
texture (AreaTex). Here the area is integrated in the shader at four points across the pixel, for
the orthogonal MLAA shapes: Z (ends turn opposite ways), L (one end turns), and U (both ends turn
the same way, split at the middle). That covers what SMAA's LOW and MEDIUM presets use the texture
for. Diagonal patterns and corner rounding (HIGH and ULTRA) are not done. Four samples of a straight
line are exact to within 1/64, and the shader only runs this on edge pixels.

#### When it runs: after 3D, at the next 2D draw, as often as that happens

It's armed by a non-through flush into a target at least 480×272 (the game's own framebuffer, not
a render-to-texture), and fires at the next through-mode flush. The first version fired at the
first through flush of the host frame instead, and **nothing it did reached the screen**: the weights
debug view showed an ordinary picture, because the world was drawn over it afterwards.

It re-arms within a host frame because the game sometimes draws a whole second frame into one host
frame (measured by the shadow pass, see "RestartCapture" in `VCSShadow.cpp`). Every such frame is
counted and logged: `ran 2 times in one host frame`, 3 times in ~25 s standing on Ocean Beach. A
world drawn in pieces with 2D between them would also show up as double runs. Nothing so far says
which of the two these are.

The game's framebuffer during play is 512×320 PSP pixels, 3072×1920 at the Mac's Auto resolution.
The log says `running at WxH` whenever the size changes.

#### Looking at it

`VCS_SMAA_DEBUG=<n>` at boot, or the debugger's **SMAA** tab:

```
0   as it ships
1   split screen: left half smoothed, right half as the game drew it, in the SAME frame
2   blend weights: red for horizontal edges, green for vertical edges (x2 so 0.5 is full)
```

Use the split. Two separate boots never frame the same shot: the camera settled differently on
each of the first A/B attempts, which made them useless.

#### What is NOT verified

- The cost on a phone. That's why it's off there by default, not because of a measurement.
- Whether the double runs are second frames or a world split by 2D draws (see above).
- It only runs on Vulkan, like the shadow and water passes beside it.
