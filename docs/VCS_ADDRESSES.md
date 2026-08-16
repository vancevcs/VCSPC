# GTA: Vice City Stories — address hunt

Every address the VCS input layer needs, what it represents, and how to find it.

The table lives in [`Core/VCS/VCSAddresses.h`](../Core/VCS/VCSAddresses.h). It is the only place
in the codebase where a VCS address may appear. When you find one, edit that file and rebuild —
there is nothing else to update.

**Scope:** USA build, disc ID `ULUS10160`, only. The PAL (`ULES00502`/`ULES00503`) and JP
(`ULJM05297`) releases are different builds; their addresses will not match and would need a
second table.

---

## Tools

| Tool | Where | Good for |
|---|---|---|
| **`Tools/vcsscan.py`** | Command line, needs `--debugger=PORT` | The main tool. Snapshot/diff narrowing for values you can't read, plus exact-value scans and live polling |
| **VCS window** | Debug menu → Tools → VCS | Watching up to 8 candidate addresses live as u32/s32/float/u8, and seeing the decoded state |
| **Memory viewer** | Debug → Memory | Browsing, and a one-shot search for a known value |
| **Breakpoints** | Debug → Breakpoints | Once you have an address, find the code that writes it — usually the fastest route to the surrounding struct |

### Why `vcsscan.py` exists

PPSSPP's built-in `memory.search` only finds values you already **know**. That's fine for
health (scan float `100.0`) but useless for a vehicle pointer, where you don't know the value —
and scanning for `0` on foot returns millions of hits. Cheat Engine's "unknown initial value,
then filter by changed/unchanged" has no equivalent in PPSSPP, so `vcsscan.py` adds it by
dumping RAM at moments you know something about, and intersecting.

Start PPSSPP with `--debugger=1337`, then:

```bash
python Tools/vcsscan.py snapshot foot1
```

Take a snapshot at each meaningful moment, then intersect them. Predicates are `zero`,
`nonzero`, `ptr` (a 4-aligned address inside user RAM), `=N`, `!=N`, `f=N`,
`changed:OTHER`, `same:OTHER` — all ANDed together, and it prints how many candidates survive
each one so you can see which constraint is doing the work.

```bash
python Tools/vcsscan.py search foot1:zero car:ptr foot2:zero
```

For a value you *can* read, skip the snapshots and scan directly:

```bash
python Tools/vcsscan.py value float 100.0
```

And to watch candidates from the shell instead of the debugger window:

```bash
python Tools/vcsscan.py live 0x08b8eeb8 0x08b8ef08
```

Snapshots land in `Tools/.vcsscan/` at 24 MB each and are gitignored.

The VCS window's **Hold** checkbox freezes a row so you can read a value that is changing too
fast to see. Out-of-bounds and unaligned addresses render as `-`, never a crash, so you can
type freely while hunting.

## What we know about the game's data layout

Learned while finding the first two addresses — this is the most useful leverage for the rest.

**Entities are prefixed by a 4×4 transform matrix.** Both the player ped and vehicles start with
one, laid out as three orthonormal rotation rows and then translation:

| Offset | Contents |
|---|---|
| `+0x00` | rotation row 1 (right), 4 floats |
| `+0x10` | rotation row 2 (forward), 4 floats |
| `+0x20` | rotation row 3 (up), 4 floats |
| `+0x30` | **world position x, y, z**, then `w = 1.0` |

This is a very strong filter when hunting: a real entity pointer points at something whose first
twelve floats are all within ±1.0 and whose `+0x3C` word is exactly `1.0`. Coincidental pointers
essentially never satisfy that. It is also how the vehicle pointer was told apart from traffic —
compare the pointee's `+0x30` position against the player's.

**Globals live low.** The static data region is roughly `0x088xxxxx`–`0x08exxxxx`; both found
addresses are there. Anything from `0x093` upward is heap and will move between sessions. When a
scan returns hundreds of hits, filtering to addresses below `0x09000000` usually leaves a
handful — of the 414 vehicle candidates, only 7 were in the global region.

**Rotation is available for free.** Since entities carry a full matrix, the player's and the
camera's facing can be derived from the rotation rows rather than hunting for a separate yaw
float. Worth trying before scanning for `CameraYaw`.

## Useful facts about the PSP

- **No ASLR.** Addresses are stable across runs of the same build, so anything you find once is
  good forever. This is why a `constexpr` table is a reasonable design.
- **User RAM** is `0x08800000`–`0x09FFFFFF`. The game's own globals live low in that range,
  just after the loaded EBOOT.
- **Prefer statics over heap addresses.** A value inside a heap-allocated struct will move
  between sessions. If a candidate address changes after a reload, you have found a field
  inside an allocation — find the pointer to it instead and store *that*, then read through it.
  `VCS::ReadPointer` exists for exactly this and validates both the pointer and its target.
- Scratch/uncached mirrors of RAM exist at other base addresses; normalise to the `0x08…` view.

## General workflow

For a value you can read on screen (health, weapon id):

1. Note the current value.
2. Scan for it (`memory.search`, or the Memory viewer's search).
3. Change it in game — take damage, switch weapon.
4. Scan the previous hits for the new value. Repeat until a handful remain.
5. Put the survivors in the VCS scratchpad and watch them while you play. The real one tracks
   perfectly; the others drift or stay put.

For a value you cannot read (camera yaw, flags):

1. Dump RAM (Debug → Tools → Memory dump).
2. Change exactly one thing — rotate the camera 90°, get in a car.
3. Dump again and diff the two files. A few hundred addresses will differ.
4. Repeat with a *different* single change and intersect the candidate sets.
5. Watch the survivors in the scratchpad.

Once you have an address, set a write breakpoint on it. The code that writes it almost always
writes its neighbours too, which usually hands you several other fields in one go.

---

## The addresses

### `PlayerBase` — u32 (pointer) — **FOUND: `0x08bc8170`**

Pointer to the local player ped. Verified across a fresh boot: it resolves to a live entity whose
position tracks the player, including moving to the car's position while driving.

Note there are at least two other globals nearby (`0x08bc7f10`, `0x08bc85f0`) that point at the
ped while on foot but switch to the *vehicle* while driving — those are the "currently controlled
entity", useful later for camera work, but not what `PlayerBase` should be.

### `PlayerHealth` — float — **FOUND: `PlayerBase + 0x4e4`**

Current health. Stored as a **base + offset** entry, not an absolute address: it lives inside the
heap-allocated ped struct, so hardcoding where it happened to sit would break the moment the game
allocated differently.

Confirmed by writing `25.0` and measuring the HUD bar: it shrank to 1284 pink pixels from 5141 at
full health — `1284/5141 = 0.2497`, i.e. the bar tracks the value exactly proportionally.

The winning scan was a single match out of 6 million words:

```
after hp_full:f=100.0          2023 candidates
after hp_hurt:changed:hp_full     1 candidates
  0x098b4f24  hp_full=100.0000  hp_hurt=97.9721
```

Note it sits at ped+0x4e4, well past the first 0x300 bytes — an earlier look at just the start of
the struct missed it entirely. Don't assume interesting fields are near the top.

**Ruled out already:** `ped+0x064`, `ped+0x0d0`, `ped+0x0d4` (absolute `0x098b4aa4`,
`0x098b4b10`, `0x098b4b14`). Two of them read exactly `100.0` and looked ideal, but writing
`10.0` to each left the HUD health bar completely unchanged. Don't retry these.

Two lessons worth keeping:

- **A float reading `100.0` in the player struct is not evidence.** There are several, and most
  are not health.
- **Writing to a candidate is the cheapest confirmation available.** If poking it doesn't move
  the HUD, it isn't the value — no need to take damage at all. Note whether the game *keeps*
  your written value too: one that gets overwritten immediately is derived from something else.

**Strategy that will work.** Snapshot before and after taking damage, then intersect:

```bash
python Tools/vcsscan.py snapshot hp_full     # at full health
# now take damage - fall off something, get punched, don't die
python Tools/vcsscan.py snapshot hp_hurt
python Tools/vcsscan.py search hp_full:f=100.0 hp_hurt:changed:hp_full --type float
```

Then confirm the survivors by writing a low value and watching the HUD bar, rather than by
taking more damage.

If `f=100.0` yields nothing, health may not be full at 100 or may be an int — drop the first
term and use `hp_full:changed:hp_hurt` alone, which needs no assumption about the value.

**Gotcha.** Armour behaves almost identically and will show up in the same scans. Distinguish
them by taking damage with zero armour.

### `PlayerOnFoot` — u32 (boolean)

Nonzero when on foot rather than in a vehicle.

**Strategy.** Snapshot/diff: dump on foot, get in a car, dump again, diff, get out, dump, and
intersect. Look for a value that is exactly 0/1 and flips reliably.

**Note.** This may not exist as an explicit flag. If it doesn't, `PlayerVehicle == 0` is a fine
substitute — `VCSState` already derives one from the other, whichever you find first. Filling in
*either* is enough to bring the input context system to life.

### `PlayerVehicle` — u32 (pointer) — **FOUND: `0x08bb4064`**

Pointer to the vehicle the player occupies; 0 on foot. Verified: reads 0 on foot across a fresh
boot, and the context system resolves `OnFoot` from it.

Found with the three-snapshot recipe below. 1,139,569 candidates → 481 after `car:ptr` → 414
after `foot2:zero`. The final tiebreak among the survivors was **position**: the winning entry
pointed at an object sitting at the same world coordinates as the player ped, while the other
candidates pointed at traffic ~57 units away.

**Strategy.** Snapshot on foot, in a car, and on foot again, then intersect:

```bash
python Tools/vcsscan.py search foot1:zero car:ptr foot2:zero
```

The `ptr` constraint does most of the work — requiring a 4-aligned address inside user RAM
throws out the vast majority of coincidental zero/nonzero flips. If too many survive, add a
fourth snapshot in a *different* vehicle and append `car2:ptr`.

**Payoff.** This is the single highest-value address in the table: it alone unlocks the on-foot
vs in-vehicle context switch, which is the whole point of the fork.

### `WeaponIndex` — u32

Currently selected weapon slot or id.

**Strategy.** Scan for a small integer, cycle weapons, rescan. Cycling forward through the whole
list and watching the candidate increment predictably confirms it fast.

**Gotcha.** There may be two related values — a *slot* (0–9, the weapon wheel category) and a
*weapon id* (the specific gun). Direct weapon selection needs the one the game actually acts on;
check which changes when you pick up a different gun in the same category.

### `IsAiming` — u32 (boolean) — **FOUND: `0x08bb32a0`**

**1 while locked on to a target**, not merely while the aim button is held. Confirmed by two
controlled live watches:

| test | `0x08bb32a0` | `0x08bb42b4` |
|---|---|---|
| aim held, people nearby (auto-locked) | 0 → **1** → 0 | 0 → 1 → 0 |
| aim held, nothing to lock onto | **stays 0** | changed, but out of sync with input |

That second row is what separated them: `0x08bb42b4` was coincidental noise, and `0x08bb32a0`
behaves exactly like a lock-on flag.

**A negative result recorded here was wrong, and it cost a lot of time.** It read: *"nothing
flipped reliably when aiming at empty space. VCS does not appear to have a free-aim state at all."*

VCS **does** have free aim for ordinary weapons, with a crosshair, and mouse yaw and pitch both
track in it. It just isn't reachable with the game as shipped: it needs `CameraInputMode`
(`gp-0x3F00`) set to 1, and then the lock broken with a large aim deflection. Neither was known
when the note above was written, so "aim at empty space and watch" could never have found it.

See "Mouse free aim — how it actually works" in `CLAUDE.md`. The lesson worth keeping: a negative
result only covers the conditions you actually tried, and this one got quoted back for weeks as if
it were a property of the game.

**First, a control-mapping discovery that blocked this entirely:** aim in VCS is the **R trigger**,
not the L trigger. The binding was originally on L, and holding it did nothing whatsoever — the
character never entered an aim stance, so a whole snapshot pair captured no aiming state at all.
Confirmed by holding each in game and watching the character: R trigger raises the weapon across
the chest, L trigger does nothing visible on foot. `VCSInput`'s table is fixed, and bindings there
are now marked verified/unverified so this class of mistake is visible.

**Candidates**, from three snapshots (aim off, aim held, aim off again) intersected with
`off == off2 != aiming` and restricted to small ints in the global region:

| address | off | aiming | off again |
|---|---|---|---|
| `0x08bb32a0` | 0 | **1** | 0 |
| `0x08bb42b4` | 0 | **1** | 0 |
| `0x08bb329c` | 2 | 3 | 2 |
| `0x08bb93f0` | 1 | 0 | 1 |
| `0x08bc91d4` | 8 | 1 | 8 |
| `0x08e8ab10` | 2 | 1 | 2 |

The first two are clean booleans and the most likely. Confirm by watching them while aiming
**by hand** — synthetic mouse input proved unreliable for this:

```bash
python Tools/vcsscan.py live 0x08bb32a0 0x08bb42b4
```

Then aim and release a few times. The right one flips to 1 exactly while aim is held.

### `IsFreeAiming` — u32 (boolean) — **FOUND: `0x08bafb54`**

**1 while free-aiming**, which in VCS means the sniper rifle or the RPG. Stays 0 when locked on,
so it is fully independent of `IsAiming` and the pair gives three states:

| state | `IsAiming` | `IsFreeAiming` |
|---|---|---|
| not aiming | 0 | 0 |
| locked on | 1 | 0 |
| free aim | 0 | 1 |

**Neither flag drives the input *context* any more, but `IsFreeAiming` decides what the mouse
does.** This section used to say `ResolveContext` treated either one as the `Aiming` context.
That changed in two steps:

1. `IsFreeAiming` came out of `ResolveContext`, because it also reads 1 during **cutscenes** — it
   is probably something broader, like "player control restricted". Using it applied aiming
   bindings during cutscenes and broke skipping them.
2. `IsAiming` came out too, when aiming was rebuilt around the reticle. It means *locked on*, so
   it can't mark the boundary of "the player wants to aim" — it stays 0 when aiming at empty
   space. The `Aiming` context is entered from the held aim key instead: no address, no latency,
   and it's what hold-to-aim actually means.

**`IsFreeAiming` then came back, in a narrower role**, after the reticle was found to be wrong
without it. It now gates `ReticleActive()` — whether the mouse drives the analog stick (free aim)
or the camera (everything else). Holding the aim trigger in VCS gives **lock-on**, where the nub
strafes; gating the reticle on the key alone meant moving the mouse walked the character around
with no crosshair anywhere. See "VCS has no free aim for ordinary weapons" in `CLAUDE.md`.

That narrower role is also what makes it safe. It's only consulted with the player on foot and aim
held, and all it decides is where the stick's input comes from — it no longer changes which buttons
are bound, so the cutscene breakage can't recur.

`IsAiming` remains a pure diagnostic. Both are decoded into `VCSState` and shown in the debugger,
and they're the best way to check what the game *did* with the input that was sent.

**How to reach free aim:** equip a sniper or RPG. Earlier notes suggested cycling targets past
the last one, which is true but impractical to stage - a busy street always offers another
target. The sniper route enters free aim directly, independent of surroundings.

#### Two false starts worth remembering

`0x08bf2334` looked perfect across **five** snapshots from two sessions: 1 only during free aim,
0 when idle and 0 when locked on. It is an oscillator, flipping about twice a second regardless
of input. A 50/50 value matching a 5-sample pattern is roughly a 1-in-32 shot, and there were 22
candidates - so one matching by chance was close to expected. Snapshot agreement is **not**
independent evidence when the value moves faster than the sampling.

The snapshots also reported `0x08bafb54` with the **opposite polarity** to reality (they showed
idle=1, free=0; live shows idle=0, aiming=1). The live behaviour is reproducible and input-synced,
so it wins - but treat snapshot polarity as unreliable.

**What actually settled it:** counting. Two aim cycles produced exactly four transitions, twice
over, with no changes at all while idle. Correlation with a counted input is the only test that
has reliably distinguished a real flag from a coincidence here.

#### Ruled out

- The **ped struct** contains nothing: zero flag-like changes in its first `0x900` bytes as either
  u32 or u8 between idle and free aim. Don't look there.
- `0x08bcf124`, `0x08e8a2b4` - never changed live.
- `0x08e8ab00` - changed once and stayed (a counter, not a flag).
- `0x08e94624`, `0x08e94644`, `0x08e94714` - move together, most likely HUD elements hiding when
  the scope comes up rather than the aim state itself.

### `CameraYaw` — float — **FOUND: `0x08bc7f1c`**

Horizontal camera angle. **Task 2 is unblocked.**

**Convention** — important for task 2:

- Stored in **radians in `[0, 2PI)`**, not `-PI..PI`. Mouse look must wrap into that range.
- It equals the camera matrix yaw **+ PI/2** exactly (verified to 1e-5 across seven snapshots and
  again on a fresh boot). So `matrixYaw = CameraYaw - PI/2`, if you ever need to cross-check.
- **Writing it rotates the view immediately.** But the game runs its own camera smoothing and
  eases yaw back toward whatever its follow logic wants. A single large write sticks (it
  effectively re-anchors the follow target), whereas small per-frame nudges get undone on the
  frames in between — measured at 135 successful writes producing *no net rotation*. Continuous
  mouse look therefore has to re-assert its own yaw every frame, not read-modify-write. See
  `Core/VCS/VCSCamera.cpp`.

**How it was found.** Deriving it from the camera matrix at `0x08bc7e30` looked like a shortcut
but produced three false positives — constants near 1.667 that matched in one snapshot and were
frozen in all the others. What worked was a **correlation across all seven snapshots**: compute
the matrix yaw per snapshot, then find floats whose circular offset from it is identical
everywhere. That left exactly four candidates out of 6 million words, all with a perfect
correlation of 1.000000.

Three of the four were indistinguishable by value. Writing `+1.6 rad` to each and measuring how
much of the screen changed separated them instantly:

| candidate | view changed |
|---|---|
| `0x08bb37a0` | 1.5% (a copy the game doesn't read) |
| `0x08bc7fb8` | 0.8% (ditto) |
| **`0x08bc7f1c`** | **62.4%** |
| `0x08bb34b4` | 29.3% (reverse heading, `+PI`) |

`0x08bb34b4` holds the same angle offset by `PI` and does move the view; it is probably the
player heading rather than the camera. Left out of the table, but noted in case task 2 wants it.

**Strategy.** Snapshot/diff while rotating the camera. Rotate exactly 180° between dumps so the
change is large and unmistakable. Then watch candidates in the scratchpad while spinning: the
real one sweeps smoothly and wraps at the range boundary.

**What to expect.** Roughly `-π`…`π` (about ±3.1416) or `0`…`2π`. If a candidate wraps at
exactly those values you have almost certainly found it. Note the sign convention — which
direction is positive — because task 2 needs it.

**Gotcha.** There are usually several yaw-ish floats: the camera's current angle, its target
angle, and the player's facing. The one to write for mouse look is the one that, when frozen,
stops the camera dead. Test by holding a value and pushing the stick.

### `CameraPitch` — float — **FOUND: `0x08bc7f18`**

Vertical camera angle. **Sits 4 bytes *before* `CameraYaw`** — the camera struct stores them
adjacent as `{pitch, yaw}`. Stored **negated** relative to the matrix pitch: raising the value
tilts the view up.

Found by the same correlation method as yaw, which worked even though the camera was near-level
in every snapshot — there was still a 22.6° spread (one snapshot caught the camera pitched down
after taking damage), and that was enough. Five candidates came out, and the same three-camera-
struct pattern as yaw appeared: `0x08bb37a4` and `0x08bc7fbc` are pitch fields of the two inert
copies, sitting +4 from their respective yaw copies.

**Important difference from yaw:** a one-shot write does *not* stick. Writing `0.55` was undone
within 0.4 s (back to `-0.05`), which is also why a naive write-test showed only a 4–6% screen
change and looked like a dead end. Pitch must be re-asserted every frame, which `VCSCamera`
already does for both axes.

There is no wrap — pitch is a clamped look angle, so `VCSCamera` clamps to ±0.9 rad (~±50°)
rather than wrapping, or the camera would tumble through the ground.

**Strategy.** Same as yaw, but look up and down. Pitch is clamped, so the value stops at a hard
minimum and maximum instead of wrapping — that clamping is itself a good confirmation signal.
Note the limits; task 2 must respect them or the camera will fight the game.

### `GameState` — u32 (enum)

Distinguishes gameplay from menus, loading and cutscenes.

**Strategy.** Dump during gameplay and again on the pause menu, and diff. Look for a small
integer that takes a consistent, repeatable value per screen.

**Why it matters.** Without it, a paused game looks like normal gameplay and gets gameplay
bindings, so Escape and the movement keys do the wrong thing in menus. It is also what the
`Menu` input context keys off. Lowest priority of the nine, but the last one needed before the
mapping is genuinely safe to use.

---

## Suggested order

1. ~~`PlayerVehicle`~~ — **done**, `0x08bb4064`. The context switch works.
2. ~~`PlayerBase`~~ — **done**, `0x08bc8170`.
3. ~~`PlayerHealth`~~ — **done**, `PlayerBase + 0x4e4`.
4. ~~`CameraYaw`~~ — **done**, `0x08bc7f1c`. Task 2 unblocked.
5. ~~`CameraPitch`~~ — **done**, `0x08bc7f18`. Full 2-axis mouse look works.
6. ~~`IsAiming`~~ — **done**, `0x08bb32a0`. `WeaponIndex` remains: snapshot before and after
   switching weapon, then `search a:changed:b`.
7. ~~`TimeStep`~~ — **done**, `0x08bb3b5c` (`gp + 0x1dfc`). Found by disassembly, not by scanning:
   it is the value every camera mode multiplies its angular rate by. The aim response model needs
   it.
8. `GameState` — last, before the mapping is safe to use for real.
9. `PlayerOnFoot` — skip. `VCSState` already derives it from `PlayerVehicle`.
10. `AimYaw` / `AimPitch` — **skip, permanently.** See below.

### `PedVelX` / `PedVelY` — **FOUND: `PlayerBase + 0x140` / `+0x144`**

The player's per-frame movement velocity. Peak about **0.10** while walking, which matches the
walking speed measured from position deltas.

**How it was found, because the method generalises.** Two captures, both with the analog stick
deflected: one walking, one free-aiming. Fields alive in the first and dead in the second are
downstream of whatever suppresses movement in free aim; fields alive in both are upstream. Both
states were labelled automatically from the nub (`CPad + 0x2`) and the camera mode, so it needed no
coordination with the player beyond them doing both things at some point.

That beats "diff two snapshots" because it holds the *input* constant and varies only the state
being investigated — the stick is pushed either way, so anything that differs is the game reacting
differently, not the player doing something different.

#### Two dead ends, recorded so nobody re-walks them

**`PlayerBase + 0x108` is not velocity, it is `|position - previousPosition|`.** Computed in VFPU
at `0x08a6a7cc` inside the ped movement update (`0x08a699b0`) and stored. It correlates with speed
at r=0.983 for the dullest possible reason: it *is* the position delta. Being downstream of
everything, no breakpoint on it can locate anything — and a write breakpoint on it trips on the
frame-start zeroing at `0x08a699f8`, not on anything meaningful.

**A velocity write over the WebSocket debugger does nothing**, and this does *not* mean writing
velocity cannot work. Verified the game was genuinely running (frame counter advancing at 31/sec,
156 frames elapsed) and the field read back `0.0000` immediately after each write. But the same
asymmetry already documented for `CameraYaw` applies: a one-shot debugger write lands at an
arbitrary point in the frame and is wiped, while a per-frame write from `hleEnterVblank` is not.
**The debugger cannot answer this class of question** - only building it can.

### `AimYaw` / `AimPitch` — **there is nothing to find**

Five rounds of value-correlation failed to locate a stored aim direction, and the reason is that
there isn't one. The weapon-aim camera (`CCam` mode 45, `Process` at `0x089a341c`) integrates the
look axis straight into the camera's own `Beta`/`Alpha` — the fields this document already calls
`CameraYaw` and `CameraPitch` — and the gun is resolved from that plus ped state every frame.

Ruled out along the way, each by writing to it and watching:

| candidate | what it actually is |
|---|---|
| `CameraYaw` `0x08bc7f1c` | the right *kind* of value, wrong consumer — swings the view, gun lags |
| `PlayerBase + 0x334` | head look-at tracking |
| `PlayerBase + 0x34c` | accepts and holds writes, no observable effect |
| `0x08bb34b4` | inert; best of 322 angle-ranged full-RAM hits |

**The lesson is about method, not about this address.** Every one of those five rounds was a
search for a value, when the question — "where does the aim live" — was a question about
*structure*. Forty lines of disassembly answered it, and would have answered it at any point.
When a scan fails repeatedly, consider that it may be looking for something that does not exist,
and go read the code that would have to write it.

Camera layout, established the same way and worth having recorded:

| | |
|---|---|
| `CCamera` | `0x08bc7e30` |
| `CCamera + 0x50` | active cam index (u8; 0 in gameplay) |
| `CCamera + 0x70` | `CCam m_asCams[3]`, stride `0x260` — `cam[0]` is `0x08bc7ea0` |
| `CCam + 0x00` | mode (s16); 45 and 11 are the weapon-aim modes, 15 the on-foot follow cam |
| `CCam + 0x78` / `+0x7c` | pitch / yaw — this fork's `CameraPitch` / `CameraYaw` |
| `CCam + 0x124` / `+0x130` | smoothed per-frame increment added to those |
| `CCam + 0x128` | FOV |
| `CCamera + 0x7b8` | `PlayerWeaponMode.Mode` (s16) |
| `CPad + 0xd0` / `+0xd4` | the game's own aim axis scale (1.05 / 0.5), applied only in modes 45/11 |

### The correlation trick, generalised

When a value has no known target but *does* have a known relationship to something you can
already read (here, the camera matrix), don't scan for "changed" — scan for **consistent
correlation across every snapshot you own**. Compute the reference quantity per snapshot, then
keep only addresses whose offset from it is identical in all of them. With seven snapshots this
went from 6 million candidates to 4, and it works no matter how many conventions
(sign, offset, wrapping) the game might use, because a constant offset is exactly what you test
for. Then separate the survivors by writing to them.

## The recipe that works, in general

Most addresses here fell to the same three steps:

1. **Snapshot at moments where you know something** about the value, and intersect. Two snapshots
   plus `changed:` is usually enough; the vehicle pointer needed three.
2. **Filter by what the value must look like** — `ptr` for pointers, `f=100.0` for health, and
   for entity pointers the matrix check (twelve floats within ±1.0, `w = 1.0` at `+0x3C`).
3. **Confirm by writing to it** and watching the game react. This is far faster and more certain
   than trying to catch a change while playing, and it is what ruled out three plausible-looking
   health candidates that all read exactly `100.0`.

**Try step 0 first, though: read the code.** `Tools/vcsstatic.py` disassembles from a savestate
with nothing running, so this costs a minute rather than a session. `TimeStep` and the whole
camera layout above came out that way, and `AimYaw` — five failed rounds of the recipe — was
answered by it in one.

The recipe is for "where does the game keep Y". Disassembly is for "what does the game do with X",
and it also answers "does Y exist at all", which the recipe structurally cannot: a scan that finds
nothing looks identical whether the value is well hidden or absent.

Everything works with any subset filled in. Unset entries read as `nullopt`, display as `unset`,
and keep the input layer in passthrough.
