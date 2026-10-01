# The phone build

> Read before touching the touch controls (`UI/VCSTouchControls.*`), the Android flavor and its packaging, the phone menu, or the Widescreen row.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

**Status: Android plays, on a Galaxy S10e (Exynos, Mali-G76), at 30fps.** The touch controls, the
HUD taps and the phone-sized menu are built and compile clean; what has been played is the first
two - and the rebuilt layout below (icons, the jump-and-sprint button, the aim toggle, the
shooting glances, the cutscene screen and the radar row) has NOT been played at all. It compiles,
and every context of it was laid out against a phone-sized mock built from the table itself
(`kControls` parsed, radii and positions resolved, overlaps and off-screen controls reported), so
what is checked is placement rather than feel. iOS is untouched beyond the notes below.

The app is a Gradle flavor rather than a fork of the fork: `vcs` in `android/build.gradle.kts`,
its own application id so it installs beside a real PPSSPP, arm64 only, signed with the repo's
debug keystore because it is sideloaded and a release nobody can install is no use.

**targetSdk 29, for the storage and nothing else.** It is what PPSSPP's own legacy APK does, and
it buys plain file paths: `/sdcard/GTAVCS/` holds `memstick/` and the disc, which is the PC
package's layout with the exe taken out. Thousands of texture files through the storage framework
is the alternative, and it is slow enough to feel. `VCS::SetGameFolder` is how Core learns about
it - `GameFolder()` was the exe's directory, and a phone has no such thing.

**The permission arrives after the first run has already asked.** Storage permission is granted
some seconds into the first launch, by which time `NativeInit` has already failed to find
`/sdcard/GTAVCS` and `CreateStartScreen` has already decided there is no disc. So
`UseVCSGameFolderMemstick` runs again on the grant, and `VCSNoDiscScreen` looks again whenever the
app comes back to the front - which is also what makes copying the ISO across while the app is
open work without a restart.

### The touch controls are a third device, not a second scheme

PPSSPP's own on-screen pad writes `__CtrlUpdateButtons` directly, which skips every context rule
this fork has: the same button cannot be Jump on foot and Handbrake in a car, because nothing
downstream ever hears about it. So VCS gets its own overlay - one line in `EmuScreen::CreateViews`,
and every other game keeps PPSSPP's.

What the overlay presses is the PAD's controls, matched against `kVCSPadMappings`. There is no
touch mapping table and there should never be one: lock-on aiming, the scoped zoom rows, the
glances, vaulting and the Menu context all already work in terms of pad controls, and a second
table would be a copy of that scheme free to drift from it. `IsPadButtonDownLocked` answers for
both devices, which is the "name the intent, ask that" rule this file keeps recording, applied to
a question that already existed.

`UI/VCSTouchControls.cpp` is one table of rows - pad control, what it draws, where it sits, which
contexts it belongs to, what else has to be true, and what a press MEANS. Positions are dp from an
EDGE rather than fractions of the screen because a thumb's reach is a fixed distance in
centimetres, so a fraction puts the same button out of reach on a bigger phone.

The shape is the trilogy's phone ports': two columns down the right-hand side with the most-pressed
control nearest the corner, the left half empty except in a vehicle, and everything else the
camera. Three buttons on foot - attack, move, get in - against the four a pad has, which is what a
screen with room for three has to do.

#### A press is one of six things, and `Kind` is the whole of it

One field rather than a behaviour plus a "this row is special" flag, because the rows that press no
pad control at all - the menu, the steering arrows, the cutscene skip - are not a different kind of
thing from the rows that do. They are the same kind of thing doing something else.

- **`Hold`** is nearly everything: down while touched.
- **`Toggle`** is the aim control, and it is the one place this overlay deliberately disagrees with
  the pad scheme it otherwise stands on. A trigger is held because a finger can rest on it; a thumb
  holding a patch of glass is a thumb that cannot do anything else, and aiming is exactly when the
  other hand is busy. It latches, and only `ForceUp` - the control leaving the screen - can take it
  away, which is why the finger-up path and the left-the-screen path had to become two functions:
  the first build released on `UP` and the toggle switched itself off the instant you let go.
- **`JumpSprint`** is jump and sprint on one button: held it sprints, tapped twice it jumps. The
  two are told apart on the way DOWN rather than on the way up, which is what makes a jump feel
  like a press instead of a delayed reaction - the release of the FIRST tap is what arms it, so a
  slow double tap is simply two sprints. A single tap is therefore a few frames of sprint, and that
  is deliberately not defended against: sprint is a held control that does nothing in the time a
  tap lasts, so waiting out the double-tap window before sprinting at all would buy nothing and
  cost every sprint a tenth of a second. The jump itself is held for its own 8 frames rather than
  for as long as the finger is down, because the finger is already on its way up - and it presses
  `kVCSPadJumpButton`, so vaulting still sees it.
- **`Menu`**, **`Skip`**, **`SteerLeft`** and **`SteerRight`** press nothing at all.

#### The glances shoot

On the PSP a drive-by is the glance modifier and a stick direction, and then Circle - three
controls for one act, and the third of them is the only part a thumb would have to find. So the two
LOOK buttons in a vehicle press both: the glance decides the side and the trigger goes down with
it. That is not a mechanism of its own, it is the thing `GlanceDirection`'s own comment already
described - "holding a glance and pressing Circle fires to that side, because that is simply what
the game does with those inputs".

The glance is worth having without a gun, so the trigger half is added only with something to shoot
- and because both can be held at once, the second control is REFERENCE COUNTED rather than set and
cleared. A plain clear would have the first one released cancelling the shot the second is still
asking for.

#### The melee set is melee-only

Block, heavy and grab are what the face buttons become when the game decides you are fighting
rather than shooting. With a gun in hand the same three buttons do nothing at all, so three dead
controls appeared the moment anyone aimed a pistol. That is what the table's `hide` column is for,
and the target-cycling pair uses it the other way round: shown when armed, hidden down a scope,
which has no lock-on to cycle and wants those two places for its zoom.

#### The row beside the radar

The camera, the sub-mission button and the radio are the controls nobody presses in a hurry, and
they were in the top-right corner sitting on the game's own clock, money and weapon panel - the one
piece of the screen that is information rather than scenery. They are anchored to the RADAR now,
which this port has already moved to the empty top-left corner, and nothing sits to its right.

**Anchoring them to the radar means finding out where the radar actually is, and the first build
got that wrong twice.**

- **The game is not the screen.** PSP 480x272 is 16:9.1 and a modern phone is 19.5:9 or wider, so
  the game sits in a pillarboxed strip with black down both sides - and a patch of "HUD" measured
  as a fraction of the SCREEN lands well to the left of the radar it is meant to cover. Integer
  scaling, a stretched aspect and any window that is not the game's own shape do the same. The
  route overlay has used `CalculateDisplayOutputRect` for exactly this reason since it was written;
  the touch layer now uses the same mapping, and the tappable radar it inherited was wrong on every
  phone before that.
- **The radar moves after the views are built.** This port patches the module to move the radar to
  the top-left, and that patch lands in `__KernelLoadExec` - after `EmuScreen::CreateViews` has run.
  Placed once at construction the whole row sat beside where the radar used to be, in the
  bottom-left. `PlaceHudViews` re-runs whenever the geometry changes, which also covers a rotation,
  a change of display layout, and somebody moving the radar from the debugger.

#### A cutscene takes the controls away and gives back two

Everything comes off and SKIP and PAUSE go up. Pause is not optional furniture: the way into this
fork's menu during play is a tap on the radar, and a cutscene takes the radar away.

**The signal underneath it is softer than anything else the overlay reads, and it is guarded
accordingly.** It is the HUD going away (`RadarOnScreen`), and inside that the one field that was
found by diffing the whole HUD object across a single cutscene rather than by reading the code that
gates it - see `kVCSHudCutscene`. Three things therefore have to be true at once:

- the player has the game's own HUD switched ON, because a player who turned the panel off has a
  screen that looks exactly like a cutscene to this;
- the context is not `Menu`, because the game takes its own HUD away there too - without that,
  every visit to the map would read as a cutscene;
- and `hideInCutscene` is on, which is a row on the Touch page precisely so a build where the
  correlation stops holding is one switch away from being playable.

**SKIP presses Cross, and that is the harmless guess rather than the measured one.** Cross is what
was measured to skip the CREDITS - see `UpdateBootPhase` - and in-game cutscenes have not been
tested, because nobody has had the disc and a phone in the same room since this went in. A wrong
Cross does nothing; a wrong Start would open the game's own pause menu on top of the scene. One
constant to change if it turns out to be Start.

#### The icons are baked, and there is a path renderer in the tool because nothing else would do it

`Tools/vcstouchicons.py` writes `assets/vcs/touch_*.png` - line art in a 24x24 box, a few lifted
from Tabler's open set and the rest drawn there in the same idiom. They are WHITE with the shape in
the alpha channel, because a control tints its icon by the same opacity as everything else it
draws and an icon carrying its own colour would be the one thing on screen that did not fade.

The tool flattens SVG path data to polylines and strokes it with PIL, which is about two hundred
lines. That is not invented work: cairosvg, rsvg-convert and reportlab's renderPM backend are all
absent here, and pulling one in for twenty-eight small images is a dependency the build does not
otherwise need. Round caps and joins - which is what makes line art look drawn rather than
assembled - fall out of the stroking loop for free: a segment is a thick line, and a disc of the
same diameter at every vertex is exactly a round join and a round cap.

**A missing icon is not fatal**: the table's label draws instead, and a miss is cached as `nullptr`
so a set that was never deployed does not go back to the VFS on every frame. The textures are
released from `EmuScreen::deviceLost` rather than when the layout is rebuilt, which is the same
split the menu's title art and the boot curtain already make - a screen outlives the graphics
device, and rebuilding a layout has nothing to do with whether its textures are still valid.

**Four of them were redrawn after being looked at rather than after being reasoned about**, and the
pattern in what failed is worth keeping: a fist drawn as knuckles read as a bread roll, a pistol
drawn as an outline read as a car, and a tyre with two axes through it read as a second reticle
beside the real one. What fixed all three was the same move - draw the SILHOUETTE (a boxing glove, a
filled pistol) or change the object (a handbrake lever instead of a tyre). A contact sheet of the
whole set at button size is a thirty-second check and it caught every one of them.

A control leaving the screen under a thumb calls `ForceUp` on the way out, because a hidden view is
never told about the release.


### The radar's corner is four constants, and only one of them has to move

VCS draws the radar bottom-left, which on a phone is exactly where the left thumb lives. The phone
ports of the trilogy all put it top-left.

It is not stored anywhere to be written. The rect comes out of four functions that return layout
constants, read out of the blip transform at `0x0880eb64`, whose else path assembles it:

```
x0 = f_089baa88()   12.0     centre = x0 + w * 0.5
w  = f_089baad0()   65.7     screenX = centre + radarX * w * 0.5
y0 = f_089baaa8()  196.0     and the same again vertically
h  = f_089baafc()   65.7
```

So the retail radar is `(12, 196, 65.7, 65.7)` in the PSP's 480x272 screen, centred on
`(44.85, 228.85)`. Three sources agree: that arithmetic, the `(44.1, 228.6)` this fork had measured
by eye years earlier for its route overlay, and a frame measured for this work, where the green
disc came out centred on `(42.5, 228.6)` with a radius of 28.8 inside a ring of 33.

**Only the top edge is patched.** The left edge is already 12, and mirroring 196 to 12 puts the
radar the same distance from the top as it was from the bottom - one `lui` immediate per branch of
the display-mode test, and nothing to re-measure. `PatchRadarCorner` does it in
`PatchLoadedModule`, beside the memory-pool patch and for the same reason: the module is in memory
and has not executed, so no JIT block exists over the constants yet.

**The first attempt found the game's OWN custom-radar mode and it is not enough.** A byte at
`radar + 0x1AC3` with x, y and a scale at `+0x1AC4/+0x1AC8/+0x1ACC` is a real feature - the game
writes it from three places - and setting it live moved the map disc and the blips to wherever it
said. It left the RING behind, because the ring is drawn from the viewport path instead. The
patched constants move all of it, which is what a screenshot of the boot showed: ring, disc, blips
and the compass letter together in the top-left.

**And patching it live is what the JIT-marker trap is about.** A plain read of `0x089baac0` during
play came back `0x68298a0c` - a RUNBLOCK marker rather than the game's instruction - and writing
through it took the emulator down. AGENTS.md has warned about this for a long time; it is written
here again because the warning was read and the mistake made anyway, in the same hour.

### Tapping the game's own HUD

The radar opens the menu; the weapon on the HUD changes weapon. Neither draws anything - the thing
you press is already on screen, drawn by the game, which is how the phone ports did it. They are
placed in the PSP's 480x272 coordinates and converted to the display through the game's own output
rect, so they sit on the artwork rather than near it - see the row beside the radar, above, for the
two ways that conversion was wrong before.

- **The radar zone comes from `RadarSettings()`**, not from a constant of its own, so it follows
  the radar when the patch moves it. `PatchRadarCorner` writes both.
- **The weapon zone is measured**: the icon's box runs x 423..471, y 15..62, off a captured frame.
- **The menu opens on RELEASE.** A tap that slid off the radar was a camera drag that happened to
  start there, and pausing the game on it would be a menu nobody asked for.
- **Weapons are only tappable on foot.** That same control is the radio in a car, and picks the
  target with aim held - which is what the PREV and NEXT buttons say out loud in that context.


### Widescreen: the view widens, the picture is not stretched

**Status: built, builds clean for Android, NOT seen on a device.** Reported as "the S10e has an
aspect ratio different than 16:9, the content is stretched now".

The PSP's screen is 480x272 - 16:9.06 - and a modern phone is 19.5:9 or wider. PPSSPP can do one
of two things with that and neither is right: keep the aspect and leave black bars down both
sides, or stretch the frame to fill and make everything 20% wide.

**The frame IS stretched to fill, and the 3D is rendered pre-corrected by the same factor so that
it comes out right.** Nothing is distorted. What the correction does to the FIELD OF VIEW is the
choice the row offers, and it is not cosmetic:

    squash = (480/272) / (screen width / screen height)     0.836 on a 2280x1080 phone

| | what it does | what it costs |
|---|---|---|
| **CROP** (default) | scales the projection's y row UP by 1/squash, so the top and bottom of the old view fall outside the clip box | you see less vertically |
| **WIDER** | scales the x row DOWN by squash, so about 20% more world comes inside it | things pop in and out at the edges |

**The pop-in is why CROP is the default, and it was reported from play**: "when widescreen is on,
it made us see more content on the edges - as a result we can see objects pop in and out of view
in these edges, because the base game was optimized in such a way that what is not in the camera
will deload and despawn". Exactly right. The game culls and streams against ITS OWN idea of the
view, and the strip either side that it never expected to show is a strip where things appear and
vanish as you turn.

**Widening whatever the game culls against was the obvious fix, it was built, and it is worse than
the problem.** The cull is the RenderWare view window and it CAN be opened to 109 degrees with the
picture untouched - and a build that did dropped objects inside the ordinary 70-degree view. See
"VCS culls to the camera, and widening it is worse than the pop-in". So the strip either side that
WIDER reveals is still a strip the game never expected to show, and CROP is still the default for
exactly the reason it always was.

The pop-in was the first symptom in this whole file to contradict the four inert distance
mechanisms, and it arrived from play rather than from a measurement - which is what got the cull
found. It just did not get it widened.

**CROP's argument does not depend on knowing what that mechanism is**, which is the whole reason
it is the default: it shows strictly LESS than the frustum the game was already drawing, so there
is nothing new for any culling rule to fail to have loaded.

**The ini key was renamed for this**, which is the one case where the usual rule against renaming
one is wrong. Position 1 used to mean "widen and leave the HUD stretched" and now means "crop
instead of widening" - the same number saying something else - so `Widescreen` became
`WidescreenFill` and everybody starts again at the new default. The HUD is squashed in both modes
now, because the frame is stretched horizontally in both; it was never really a second axis of
choice.

**Why it is done in the renderer rather than in the game.** A real widescreen mod patches the
game's own FOV, and that would be better for everything downstream, because the game would know.
It needs an address hunt this fork has not done - and the renderer can reach the same place
without one, because of a fact about PPSSPP worth knowing on its own:

**Through-mode draws never touch `u_proj`.** They are transformed in the decoder and the vertex
shader passes them through (`if (!useHWTransform) { outPos = position; }`), and through mode always
takes that path - `CanUseHardwareTransform` returns false for it. So the 3D projection can be
widened on its own, in ONE place, with the 2D untouched.

The three places that see that matrix all go through `VCS::WidenProjection`, which scales elements
**0, 4, 8 and 12** - the x row of `u_proj * viewPos` for a column-major mat4 built from gstate's
sixteen floats, and not the first four, which is the mistake the comment there exists to prevent:

| | |
|---|---|
| `ShaderUniforms.cpp` | the upload, on the `DIRTY_PROJMATRIX` copy |
| `VCSShadow.cpp` | its captured copy, or the screen-space mask slides across the picture |
| `VCSWater.cpp` | the same, for the same reason |

**The HUD is squashed by the same factor in both modes**, so it keeps its proportions exactly and
sits inside the middle 16:9 band - the radar stays round and moves in from the screen corner.
Left alone it would be stretched with the frame and the radar would be an ellipse.

**The 2D squash is in the software transform, and the reason is the exemption.** It would be four
lines in `ConvertViewportAndScissor` to shrink the viewport for a through draw - and a fade to
black, the game's own menu backdrop and anything else covering the screen would then cover only
the middle of it, leaving the world showing down both sides. `RunSoftwareTransform`'s through
branch is where the positions are, so it is also the only place that knows how WIDE the draw is:
anything spanning 90% of the buffer is left alone, and everything smaller - which is the HUD - is
squashed about the frame's centre. The clear detection below it still sees full-width rects
unsquashed, so a screen clear is still recognised as one.

**Everything this fork draws ON the game's HUD has to ask.** `VCS::ApplyHudSquash` moves a
horizontal screen span into the band, and the route line, the map cursor and the touch overlay's
radar and weapon zones all go through it. Miss one and it lands beside the thing it is meant to
sit on.

**The stretch is set from `RefreshWidescreen`, not from the row's `onChange`.** The row fires
before a game has booted, where there is nothing to decide yet; without the tick-rate version the
first boot after switching it on would widen the view and never stretch the frame, which is worse
than either half alone. It is written only on a disagreement, and only while a VCS game is
running, so this fork's binary pointed at some other game never rewrites the player's own display
setting.

**Eight faults came back from the first build on the device, and two of them were in here.** The
others were the front end; these two are worth keeping because both are the same shape - a change
applied to one half of a pair.

- **The minimap's content slid sideways inside its ring.** The vertices were squashed and the
  SCISSOR was not, and the radar's map is drawn clipped to its own circle - so the content moved
  toward the centre while the clip stayed where it was. `VCS::g_widescreenSquashedDraw` carries
  the per-draw decision forward from the transform to `ConvertViewportAndScissor`, which happens
  later in the same Flush, and a change in it dirties the scissor state so the next draw cannot
  inherit the last one's.
- **And a guard that should have been there from the start**: the squash now only applies to a
  FULL-SIZE render target. A game that draws part of its HUD to a texture first - which is how a
  round radar is usually masked - would otherwise have that texture's contents squashed about the
  texture's own centre, which is not a place that means anything.

**What is NOT verified**: whether the HUD half is right now. The arithmetic is checked - squash 0.836, a
vertex at the old edge lands at 0.836w, so the horizontal FOV widens by 1/0.836 = 19.6%, and the
frame is then stretched by exactly 2280/480 over 1080/272 = 1.196 - but nobody has looked at the
result. The first things to suspect if it is wrong: whether the through-mode centre is really
`curRTWidth * 0.5` (this game renders 512x320 and displays it scaled, so the whole buffer is shown
and the centre is the buffer's), and whether any full-screen 2D draw is narrower than the 90%
exemption and so gets squashed when it should not.

### Arranging the controls, which is a screen rather than a settings row

`ARRANGE CONTROLS` on the Touch page opens the overlay with the game paused behind it and every
control draggable. What you drag is the THING - the same layout, at the size and with the icon it
really has - rather than a set of stand-ins, because the question being answered is what a control
covers, and a stand-in cannot answer it.

- **Offsets, not positions.** A control remembers how far it has been dragged from where the table
  put it, in unscaled dp, keyed by a new `id` on every row. So a control nobody has touched follows
  the table when the table changes, changing the control SIZE afterwards moves the cluster as one
  rather than scattering it, and the edge a control is anchored to is still its edge.
- **An id rather than an index**, because an index is stable only until somebody adds a control:
  inserting one in the middle would silently shift every saved layout by one button. They are also
  the only part of that table a player ever sees - `foot.fist`, `car.pedal_gas` - because they are
  the keys in `[TouchLayout]` in `vcs.ini`.
- **A control dragged back to where it started is erased**, so a reset layout and a fresh install
  are the same file, and the section says only what was actually moved.
- **The drag's sign follows the anchor.** Dragging left has to INCREASE a distance measured from
  the right edge, or the control runs away from the finger. Then it is clamped by where it
  actually landed rather than by an estimate - which for a row anchored to the radar is the only
  way to know.
- **The editor came up EMPTY twice, for two different reasons, and the second is the one worth
  remembering.** The first was the AnchorLayoutParams trap above - the controls were placed off
  screen. The second was that they were placed correctly and drawn at zero alpha: the overlay's
  opacity is a fade on time since the last `GamepadTouch`, nothing in a menu calls that, and by
  the time somebody has walked three pages to reach ARRANGE CONTROLS it has been faded out for a
  while. Everything worked - laid out, hit-tested, dragged - and none of it was visible, which is
  precisely how it was reported: "maybe works? but blindly". The editor forces the opacity, and
  never below 0.6, because a player who has turned the controls down to a ghost still has to see
  the one they are moving.
- **`TouchLayoutGeneration` is how the game's own overlay finds out.** There are two overlays
  while the editor is open - the game's, behind the paused screen, and the editor's - and they do
  not know about each other. A counter compared once a frame is cheaper than plumbing a
  notification between two screens.

### The HD pack cannot be read by a phone, and converting it is a two-step job

The pack ships as BC7 inside `.dds`. `ReplacedTexture.cpp` checks whether the GPU supports the
format and SKIPS the texture when it does not, so on any Mali, PowerVR or Adreno phone the whole
748MB pack quietly does nothing and the PSP's own textures are drawn - with no error anywhere.

`Tools/vcsktx2.py` converts it to KTX2/UASTC, which PPSSPP hands to the Basis transcoder and gets
ASTC, ETC2 or BC out of depending on the GPU. One pack for every phone, and the desktop reads it
too. Two steps, because no single tool does both ends: Pillow decodes BC7, the Basis encoder reads
PNG, and the intermediate lives in a scratch directory. 5,952 textures, about twelve minutes on
eight cores, 724MB of `.dds` in and 647MB of `.ktx2` out.

The encoder is built on demand into `.deps`, and it has to be built with `STATIC=OFF`: the
project's own default asks the linker for `crt0.o`, which does not exist on a Mac and fails every
executable in the build while leaving the library that actually matters sitting there built.

`vcspackage.py --android` ships the converted pack and refuses to ship the other one, because a
748MB pack the GPU will skip is worse than no pack at all - it looks like a broken feature rather
than a missing one.

### The menu on a phone

**The same front end as the desktop, at phone-sized numbers, with the desktop's own settings taken
off it.** A landscape phone is about 390dp tall and the desktop layout puts a 104dp heading and a
38dp bar around rows of 76dp - three of which fill the screen - so the phone values are 52dp rows
under a 62dp heading, and whatever still runs past the bar scrolls, which the desktop rule
deliberately forbids.

**THIS WAS A TILE GRID AND A TWO-PANE SETTINGS SCREEN FOR ONE BUILD, AND IT CAME BACK OUT.** The
root page became a grid of labelled tiles and the settings tree became one screen with the
categories down the left, on the reasoning that eight rows on a screen showing five and a half is
a page you scroll every time you pause. The reasoning was not wrong and the verdict was: use the
desktop front end. What is worth keeping from it is in `git log`, and what is worth keeping HERE
is the four bugs it shipped with, because three of them are traps this codebase can set again:

- **A child of an `AnchorLayout` needs `AnchorLayoutParams`.** Given plain `LayoutParams`,
  `As<AnchorLayoutParams>()` answers nullptr, the view is measured at wrap-content and placed at
  the origin, and everything inside it lands off screen. That is also what emptied the control
  EDITOR, which is not part of the revert and did need the fix.
- **A ViewGroup hands every child the same touch.** There is no first-refusal and no early-out
  unless `exclusiveTouch_`. Two side-by-side columns whose rows both claim the full screen width
  therefore both fire on every tap - and when one of them rebuilds the views, the other then runs
  against freed ones. `VCSMenuItem::HitBounds` uses the row's OWN bounds now, which is the same
  thing for a FILL_PARENT row and the correct thing for anything narrower.
- **This front end's row layout wants the whole screen.** A label right-aligned into a centre line
  with its value past it needs everything to the label's left, so in any column narrower than the
  screen a long label runs back underneath whatever is beside it - at any font size. A layout
  across the column is a different design, not a smaller one.

**What the phone keeps from that build**: the settings it took off. CONTROLS is skipped entirely -
`GoToPage` redirects it and the SETTINGS page points straight at TOUCH CONTROLS - because the
three rows it carries are the mouse, the pad and the bindings card, all about devices a phone does
not have. The touch page itself is four rows and an action rather than nine. Nothing was deleted:
`Option::hidden` keeps a row loading, saving and working at whatever value the file holds while
leaving it off the page, and `vcs.ini` is still there for anyone who wants one back.

**The cutscene switch MOVED rather than hid**, and that is the one exception worth stating: it is
the safety valve for a signal that is a correlation, so a build where it stops holding takes the
controls away during ordinary play and it has to stay reachable. It is on GAMEPLAY, where a
statement about what happens during cutscenes belongs anyway.

**GAMEPLAY is on every build, so its phone rows hide themselves off a phone** - this one and
`Radar corner`, whose whole argument is a thumb. Moving them there leaked both onto the desktop
menu, reported as "settings from the Android build got into this one". `kPhoneBuild` in
`VCSSettings.cpp` asks the question `kPhoneLayout` asks in the menu, and `hideLast()` keeps the rows
loading and saving, so an ini carried between the two loses nothing.

**A tap changes a setting, and wraps where the arrow keys stop.** That difference is the device: a
keyboard has both directions, so stopping at the maximum is right; a thumb has one gesture, and a
setting that stopped at the top could be raised and never lowered again. The block strip is still
draggable for the values worth aiming at rather than stepping through.

**And a row is hit anywhere along its own width.** The narrow hit box measured from the glyphs
exists because a MOUSE hovering at the same height as a distant label should not highlight it -
hover is the whole reason it is measured - and a thumb has no hover at all.

**There is a visible way BACK**, and it is the difference between a menu laid out for a phone and
a desktop menu that happens to fit on one. A keyboard has Escape, a pad has its cancel button, and
Android's back gesture is already in PPSSPP's own cancel set (`NKCODE_BACK`, hardcoded in
`KeyMap.cpp`) - so every device except the one this build is FOR could leave a page in one press.
A thumb had the BACK row at the bottom of a page it first had to scroll.

`VCSMenuIconButton` is the only view on this screen that is not a row: a round button in the
corner opposite the heading, drawn from the same `touch_back.png` the overlay uses. Two things
about it:

- **It is deliberately not focusable.** Every device that could move focus onto it already has a
  Back of its own, so putting it in the ring would only add a stop to arrow past.
- **`GoBack()` was extracted rather than duplicated.** The key handler and the button are the same
  act - up one page, out to the world from the root, and the main menu declining to close because
  it is the bottom of the screen stack - and two copies of a three-way branch is two things to
  keep in step. The one case the button does NOT share is Escape on the pause root, which the
  dialog base finishes for the keyboard; the button finishes it by hand, exactly as Backspace
  already did.

### The iOS build

**Status: built, packaged and released 2026-10-01 - NOT yet run on an iPhone.** There is no
simulator runtime on the build Mac, so nothing past the packaging has been seen on iOS at all.

`./b-ios-vcs.sh` builds an unsigned device app into `build-ios/`, and
`python3 Tools/vcspackage.py --ios --zip` turns it into `dist/GTA Vice City Stories.ipa` plus a
game-files folder and zip beside it - the Android split, for the Android reason. The overlay, the
menu and every phone default are shared code, so the iOS-specific part is small:

- **The game's folder is the app's Documents.** `NativeInit` names it with `SetGameFolder` and puts
  the memory stick at `Documents/memstick`, which is what Finder's file sharing and the Files app
  both show. The remembered disc goes through `TryUpdateSavedPath`, PPSSPP's own fix for the app
  container moving on every reinstall - and a sideloaded app is reinstalled every week.
- **Edge gestures and safe areas were already upstream's**: `preferredScreenEdgesDeferringSystemGestures`
  defers every edge in game, and the safe insets reach the UI. Nothing to add.
- **The packager does the rest, the way it does for the Mac**: the name, the bundle ID (the Android
  app's), the icon from `ios/vcs.xcassets` through `actool`, MoltenVK into `Frameworks/`, landscape
  only, `LSSupportsOpeningDocumentsInPlace`, and an ad hoc signature with `get-task-allow`. A
  sideloading tool re-signs it with the player's Apple ID; nobody without a developer account can
  sign for a device.

Three things that would each have shipped a broken app, all found before it shipped:

- **Xcode 27 will not build for anything older than iOS 15**, and this tree asked for 13 (11 for the
  sideload target). Both are 15 now.
- **CMake's Xcode generator never compiles the launch storyboard.** It is listed in the target and
  in no build phase, so the bundle had none - and an app with no launch storyboard is a legacy app
  to iOS, run letterboxed in an iPhone 5 sized window. The packager compiles it with `ibtool`, black
  rather than PPSSPP's blue.
- **The shipped ppsspp.ini starts with a byte order mark**, so `[General]` never matched as a section
  in `sanitise_ini`. Harmless while nothing needed General; the iOS package adds `ScreenRotation = 5`
  there, because iOS's default follows the phone into portrait and the desktop ini never wrote the
  key down to be forced.

**The interpreter holds full speed, measured on a Mac as a stand-in.** A sideloaded app gets no JIT
unless something attaches as a debugger, so the iPhone runs PPSSPP's IR interpreter - a path this
fork had never run. The Mac build with `--cpu=ir` on an M1, whose cores are roughly an A14's:

| | standing, Vice Point | dragged west ~900 units across the bay | longest gap |
|---|---|---|---|
| IR interpreter | 30.0 fps | 30.0 fps | 62 ms |
| JIT (control) | 30.0 fps | 29.7 fps | 273 ms |

The chase camera's code patch installed under IR too. What this does not cover: GPU and thermals on
a real phone, and anything older than A14.
