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

### `VehicleModel` — u16 — **FOUND: `PlayerVehicle + 0x56`**

The occupied vehicle's model id — 209 patriot, 213 maverick, 251 forklift, and so on. Needed
because VCS controls a helicopter completely differently from a car, so one in-vehicle binding
set cannot be right for both; `VehicleClassForModel` in `Core/VCS/VCSState.cpp` turns it into
car / bike / boat / heli / plane.

**How it was found**, in about two minutes and with no snapshot diffing at all: get into any
vehicle over the WebSocket debugger, read `PlayerVehicle`, then scan the first 0x200 bytes of
the object it points at for a u16 in the vehicle id range (170–280). Exactly one offset pair
came back — `+0x56` and `+0x58`, both reading 209, and the vehicle was a Patriot.

**Confirmed** by turning it around: scan all of RAM for entity-shaped matrices (four 16-byte
aligned vec3s, `+0x3c == 1.0`, unit-length rows) that also carry a valid id at `+0x56`. That
produces the **vehicle pool** — ~30 live objects on a `0x820` stride starting around
`0x098ef520`, each with a sensible model name and its own world position. A coincidence would
not have been coherent across the whole pool.

`Tools/vcsvehicle.py info` prints the candidates for whatever you are sitting in, so this is
cheap to re-check if a future build moves it.

**Model ids come from the game itself**, not just from a wiki: there is a model-name table at
`ModelInfo[id] = 0x093857b4 + id * 0x1c` (heap, so the base moves between runs) whose entries
are `char name[20]; void *resource; u32 flags`. Entry 213 is `maverick`, 251 is `forklift`, 280
is `airtrain` — matching gtamods' list exactly. The game's own **type** names live in the static
region at `0x08bafc6c`: `car boat jetski train heli plane bike ferry bmx quad`. That table is
what confirms the classification is the game's own idea and not an invented one; where the per-
model type is *stored* is still unknown, which is why the class table is keyed on the id instead.

### `WeaponIndex` — u32

Currently selected weapon slot or id.

**The weapon array itself is found: `PlayerBase + 0x58c`.** Nine records of 28 bytes:

| offset in record | meaning |
|---|---|
| `+0x00` | `0x000d8700`, identical in every record — the type marker the array is recognised by |
| `+0x08` | weapon type |
| `+0x0c` | state |
| `+0x10` | ammo in clip |
| `+0x14` | ammo total |

Read out of `load_undo.ppst` (the only savestate of the three that is in gameplay — the other two
have `PlayerBase` at 0), eight slots were live: types 10, 13, 19, 23, 26, 28, 32, 31 with ammo
`0/0`, `1/10`, `17/17`, `1/75`, `30/206`, `30/299`, `1/5`, `7/21`. Ascending type per slot except
the last two, which is what a category-ordered slot array looks like.

That collapses the slot-vs-id gotcha below: **only the slot needs finding**, because the type then
comes from `array[slot] + 8`, and the type is what tells melee from a gun — the thing the melee
lock-on bug actually needs.

One dead end worth recording: *there is no active-weapon pointer.* No word anywhere in the 27.9 MB
image points at any weapon record, so the game keeps no `CWeapon*` — the current weapon is an index,
which is what sent the hunt to the code. (Also note when scanning code that it runs well past
`0x089f0000`: `0x08adca6c` and `0x08b65de8` are both real functions, so the usable span is
`0x08804000`–`0x08b70000`, 897,024 words.)

**FOUND: `PlayerBase + 0x789`, a signed byte, and it is a SLOT not a weapon id.** Get the id with
`PlayerBase + 0x574 + slot * 28 + 4`. Verified live: the byte read 8, slot 8 held type 31 with 7/21
ammo, and the formula reproduces the whole slot table above, including the slot 0 the data-side
scan had filtered out.

It came from the game's own code via the script command table (next section) — the handler for
`02C0 get_current_char_weapon` at `0x08b2bb88`:

```
08b2bbc4  lb    $a0, 0x789($v0)     ; the slot
08b2bbc8  sll   $a1, $a0, 5         ; \
08b2bbcc  sll   $a0, $a0, 2         ;  }  slot * 28
08b2bbd0  subu  $a0, $a1, $a0       ; /
08b2bbd4  addu  $a0, $v0, $a0
08b2bbd8  addiu $a0, $a0, 0x574     ; array base - note 0x574, not the 0x58c the data suggests
08b2bbdc  lw    $a0, 4($a0)         ; -> weapon type
```

Reading the handler beats hunting the arithmetic: the `i*28` sequence occurs at 462 sites, and
because the base materialises as `0x574` rather than `0x58c`, correlating on `0x58c` finds nothing.

### The script command table — resolving any opcode's handler

The general key, worth reaching for before any value-correlation hunt: **anything the mission script
can ask for, the game has a function for, and that function's address is in a table.**

`0x08b846e0`, 8 bytes per opcode, indexed by the masked command id. Each entry is a
pointer-to-member: word 0 is the discriminator (0 in the direct case), **word 1 is the handler**.
Unimplemented opcodes read as two zero words.

```python
handler = u32(0x08b846e0 + opcode * 8 + 4)
```

The dispatcher is at `0x08862600`; its signature is `andi $a1, $a1, 0x7fff` (strip the NOT flag)
then `sll $a2, $a1, 3` against base `0x08b846e0`.

Handlers resolved so far: `02C0 get_current_char_weapon` `0x08b2bb88`, `010B
set_current_char_weapon` `0x08b284b0`, `02E7 get_char_weapon_in_slot` `0x08b2c2ac`, `03E5
is_developer_flag_active` `0x08a57c8c`, `03E6 set_developer_flag` `0x08a57d20`, `03E9` (the
undocumented pad command the CLEO plugin calls) `0x089e0b14`, `03FD unknown_check_command_92ea`
`0x08a9f1ac`.

**Two false leads.** A jump-table scan will never find this table — every second word is a
discriminator, not a code pointer, so runs of consecutive code addresses break immediately. And
there is a *second*, unrelated byte-stream VM at `0x088c0e90` that also reads a u16, tests bit 15
and masks `0x7fff`, dispatching via `0x08adca6c` into a `std::map<u16,handler*>` (u16 key at
node+`0x10`, value at node+`0x14`). It looks exactly like the thing you want and is a dead end: its
registry `*(gp + 0x16f0)` reads 0 during gameplay, so it is not the live interpreter.

### The weapon fire path — where the shot is actually resolved

The target for the free-aim work: re3 and reVC do free aim entirely at the fire site, by
raycasting through the crosshair pixel instead of along the ped's aim (`CWeapon::FireInstantHit`
-> `CCamera::Find3rdPersonCamTargetVector`). To do the same here, the raycast has to be found.

| | |
|---|---|
| `CWorld::ProcessLineOfSight` | `0x0889786c` |
| `CWorld::GetIsLineOfSightClear` (the yes/no form) | `0x088967ac` |
| `CWorld` current scan code | `gp-0x6394` |
| the weapon fire function | `0x08ac811c` |
| its raycast call site | `0x08acc844` |
| `source` / `target` at that site | `sp+0x440` / `sp+0x450` |
| the `bInclude*` global it brackets the call with | `gp+0x1f88` (`0x08bb3ce8`) |
| vec sub / add / magnitude / scale | `0x8a931d0` / `0x8a931b8` / `0x8ac75c0` / `0x8ac7620` |

**Found structurally, with `Tools/vcsxref.py`, from a savestate.** Every CWorld sector scan opens
by advancing a `u16` scan code and resetting it at `0xffff` — re3's `AdvanceCurrentScanCode`
exactly — so the functions touching `gp-0x6394` *are* the collision family: 61 of them, and
sorting by caller count puts the two general raycasts on top. The argument shapes tell them
apart: `0x088967ac` masks `a2,a3,t0-t3` to bytes so only `a0,a1` are pointers, while
`0x0889786c` leaves `a0-a3` alone and starts the bools at `t0` — i.e. four pointers first
(`point1, point2, colPoint&, entity&`), which is `ProcessLineOfSight`.

Then `--bracketed` picked the fire site out of its 74 callers in one step: re3 sets
`bIncludeCarTyres` / `bIncludeDeadPeds` / `bIncludeBikers` immediately before the weapon's
raycast and clears them after, and **exactly one** call site here is wrapped in that
set-then-zero pattern. Camera clipping and AI visibility checks are not.

The arithmetic just above the call confirms it, matching reVC's lock-on branch line for line
(`Weapon.cpp:886-889`) — `target -= *fireSource`, magnitude, `target *= range/dist`,
`target += *fireSource`, then the raycast with `a2`/`a3` pointing at stack colPoint and victim
slots and `t0-t3` all 1. `0x08ac811c` is a real function start (preceded by a `jr $ra` delay
slot, frame `0x540`, `a0` dereferenced at `+0x1d0` as `this`) with 2 callers, consistent with
`CWeapon::Fire` reaching it from two paths.

**`0x08ac811c` is NOT the path an ordinary shot takes, and the structural search was misleading
here.** `CWeapon::Fire` does not call it at all. It is a real weapon raycast bracketed exactly
as re3 brackets its own, but a free-fired pistol never goes near it. The bracket found *a* fire
path, not *the* fire path — and two rounds of live testing were spent on it before that was
clear. A fingerprint that matches re3 proves the code is the same *shape*, not that it is on the
path you care about.

### The path a shot actually takes — measured, not inferred

| | |
|---|---|
| `CWeapon::Fire` | `0x08a45338` |
| the weapon's raycast wrapper | `0x08a41d28` |
| its `ProcessLineOfSight` call | `0x08a41d74` (returns to `0x08a41d7c`) |
| weapon record | `PlayerBase + 0x574 + slot*28`, **type `+0x04`, clip `+0x0c`, total `+0x10`** |

**`CWeapon::Fire` was caught with a write breakpoint on clip ammo** — one trip per shot, no
guessing — and identified beyond doubt by the code around the write, which is reVC's
`m_nAmmoTotal < 25000` check compiled literally: `lw / blez / addiu -1 / sw`, then
`slti a0, a0, 0x61A8`. Note the record layout that came out of it is **four bytes off** what
the sections above derive from a savestate; the live read wins.

The raycast is not on the stack when that breakpoint trips, because `Fire` spends the round
*after* its FireXxx helper has returned. It was found by breaking on `Fire`, arming the whole
collision family behind a **condition on `ra`** so camera clipping could not drown the signal,
and reading back which one the shot reached. `0x08a41d74`, on consecutive shots. The enclosing
`0x08a41d28` is reached from two different weapon functions, which is the shape of reVC's
file-local `ProcessLineOfSight` wrapper — so every weapon raycast funnels through one place,
a better hook point than re3 itself offers.

**The shot direction is writable there. Confirmed in play:**

```
 # site       mode      hit point                    moved  hit
 1 08a41d74   control   (-1746.35  -227.46   15.27)   0.00   y
 2 08a41d74   deflected (-1751.45  -225.42   15.27)   5.50   y
 3 08a41d74   control   (-1746.35  -227.46   15.27)   0.00   y
 4 08a41d74   deflected (-1751.45  -225.40   15.26)   5.50   y
```

Rotating the target vector 25° about Z immediately before the call moves the resolved hit point
by 5.50 units, reproducibly, with the controls bit-identical. That is the necessary condition
for porting re3's model — the raycast obeys a written target — and `Tools/vcsfiretest.py`
reproduces it.

#### Breakpoints on the JIT cannot be trusted here, and it cost several rounds

`CPUCore = 1` gave breakpoints that fired *once* and then stopped firing at the same address,
which reads exactly like "this code is not on the path" and is not. Everything above was
established on `CPUCore = 0`. Two intermediate negative results — that the shot does not pass
`0x08a4e69c` / `0x08a4e908` — were collected under the JIT and had to be retracted; they were
later confirmed on the interpreter, but they were not evidence when they were first reported.

**A breakpoint on the `jal` at a call site never fired even on the interpreter**, while one on
the called function's entry, conditioned on `ra`, fires every time. Break on entry with a
condition; do not break on call sites.

**A false lead, so nobody re-walks it.** `0335 fire_hunter_gun` in `scm/VCSSCM.INI` looks like the
ideal way in via the script command table, and is not: its handler (`0x08abb0b4`) collects about
five parameters where the INI declares one. Sanny's VCS opcode names are partly inherited from
Vice City and cannot be trusted for an opcode nobody has confirmed. The command table itself is
fine — the formula was re-verified against the known `02C0` handler while establishing this.

### The camera stores a real view basis, and it is not the same thing as `CameraYaw`

**This is the answer to "shots land chaotically and not where the crosshair is", and it corrects a
note in the address table that said these fields could not be used.**

`CCam[0]` (`0x08bc7ea0`) holds, alongside the orbit angles already in the table:

| offset | what | measured in `ULUS10160_1.03_1.ppst` |
|---|---|---|
| `+0x010` | `Front`, unit | `(-0.22437, +0.96148, -0.15878)` |
| `+0x020` | `Source` — the camera's world position | `(-1745.340, -241.625, 15.942)`, 4.633 m from the player |
| `+0x060` | `Up`, unit | `(-0.03608, +0.15462, +0.98731)`, `dot(Front, Up) == 0.000000` |
| `+0x128` | FOV, degrees, **vertical** | `70.0` |
| `+0x190` | the look-at point | the player's position with `z + 0.600` |

All of it geometric, none of it correlated: `Front` reproduces `normalize(LookAt - Source)` to five
decimal places, `Front` and `Up` are exactly orthonormal, and `Source` is the only candidate whose
bearing to the look-at point carries the stored pitch. That last test is what settles `+0x020`
against `+0x210` — the six position-shaped vec3s behind the camera are hard to tell apart
horizontally, and trivial to tell apart *vertically*: `+0x210` sits level with its target, implying
a camera looking straight ahead, while `Alpha` reads −11.9°.

**Why the earlier attempt concluded these "did not reconcile with `CameraYaw`".** They don't, and
they shouldn't. `Beta`/`Alpha` are the ORBIT angles about the look-at target; `Front` points from
the source at that target. The player is not in the middle of the screen, so the two differ by
however far off-centre he is — **4.42°** in this savestate, roughly 3.3 horizontal and 2.8 vertical.

That discrepancy is the whole bug, and the important property is that **it is not a constant**. It
is an angular offset, so it grows as the camera closes on the player and swings as the camera
orbits. The `crosshairX` slider was fitting its horizontal half, which is exactly why the fitted
value kept wandering — measured in play at 0.5125 to 0.5300 across a 180° sweep, non-monotonically.
No single trim can hold, and the vertical half had no slider at all.

Two independent numbers agree that the horizontal trim was measuring this and not the crosshair:
the trim wanted **3.2°** in play, and `Front` sits **3.3°** off the angle it was correcting. So with
`Front` used directly the crosshair multipliers should sit at 0.5 — which is what they now default
to, and which is a falsifiable prediction rather than a tuned value.

The port is now re3's `CCamera::Find3rdPersonCamTargetVector` as actually written:

```
tanHalf = tan(FOV/2)
right   = Front x Up                       (screen-right; +X for a camera facing +Y)
dir     = Front + right * (2*(chairX - 0.5)) * tanHalf * (480/272)
                + Up    * (2*(0.5 - chairY)) * tanHalf
dir.Normalise()
origin  = Source, slid along dir to the point nearest the muzzle
target  = origin + dir * weaponRange
```

### `CWeaponInfo` and the weapon's range — because the ray length was also wrong

`CWeaponInfo::GetWeaponInfo` is `0x08b1fd70` and is three instructions:

```
offset = type * 128 - type * 16          ; stride 0x70
base   = gp[-0x1ba8] ? tbl[0] : tbl[1]   ; tbl = *(void**)(gp + 0x2950)
return base + offset
```

so with `gp = 0x08bb1d60`: selector at `0x08bb01b8`, table pointer at `0x08bb46b0`, **range at
`+0x08`**. Read back across the arsenal in a savestate and it is obviously right — pistol 30, python
40, shotgun 15, SMG 45, AR 90, M60 100, sniper 55, RPG 75; entries decode as noise past about 40.

**Why this matters to aiming at all.** The hook kept the length of whatever target the game had
already computed, and that length means three different things depending on which branch of
`FireInstantHit` (`0x08a4842c`) produced it:

| branch | what `point2` is | so `Dist(source, target)` is |
|---|---|---|
| `m_pPointGunAt` is a dummy, shooter is the player | `CPed + 0xC80` | the distance to the free-aim point — near |
| `m_pPointGunAt` is a ped | that ped's bone position | the distance to him |
| no `m_pPointGunAt` | `source + range * (-sin, cos, 0)` from the ped's heading | the weapon range |

and on the last one `0x08a5071c` — an auto-aim assist that gathers entities in a sphere and snaps
the target onto one — runs *before* the raycast and can shorten it again. Redirect the direction
while keeping that length and the bullet stops short by a different amount every shot, which is
what "chaotic" was.

Worth recording separately, because it is a fact about the game rather than about this fork: **the
no-lock-on branch is completely flat.** `target.z = source.z`, always. Vertical aim through the
game's own path does not exist; the fire-site hook is the only thing that provides it.

### `PedHeading` / `PedHeadingTarget` — **FOUND: `PlayerBase + 0x8d0` / `+0x8d4`**

re3's `CPed::m_fRotationCur` and `m_fRotationDest`, and found by value rather than by correlation:
the ped's matrix forward is `(-sin, cos)` of its heading, so the heading is computable directly from
the entity struct. Exactly three floats in the whole 6 KB ped matched it — `+0x6b8`, `+0x8d0` and
`+0x8d4`. The adjacent pair is cur/dest; `+0x6b8` is a third copy.

Confirmed independently out of the game's own code: `FireInstantHit` reads `+0x8d0` for the player,
compares it against a global copy of its previous value and stores the new one back
(`0x08a48694`..`0x08a486bc`) — it is tracking how far the player turned between shots, which is only
meaningful for the live heading.

Conversion from the camera, since it is the thing that gets got wrong: the camera looks along
`camYaw - PI` read as `(cos, sin)`, the ped stores a heading whose forward is `(-sin, cos)`, so

```
pedHeading = atan2(-cos(camYaw - PI), sin(camYaw - PI)) = camYaw + PI/2   (mod 2PI)
```

Checked against the savestate: `camYaw` 4.88071 gives 0.168 where the ped was facing 0.064, a 6°
difference which is the follow camera trailing the player rather than a convention error.

**Note this is `+ PI/2`, where the `CameraYaw` entry above says `matrixYaw = CameraYaw - PI/2`.**
Both are right; they are different quantities. That note is about the camera's own matrix, this is
about the ped's facing, and they differ by the half turn between "looking at the player" and
"looking the way the player looks".

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
6. ~~`IsAiming`~~ — **done**, `0x08bb32a0`. ~~`WeaponIndex`~~ — also **done**, `PlayerBase + 0x789`,
   found from the script command table rather than by scanning.
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
