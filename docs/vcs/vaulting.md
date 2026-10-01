# Vaulting

> Read before touching `VCSVault` or `VCSWorld` (the collision-query program written into PSP memory).
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

### Vaulting — pulling up onto a ledge, and calling game code to find one

**Status: working in play, with the game's own animation on very nearly every vault.** On by
default (`VaultSettings().enabled`, or the Vault tab in the debugger window). Confirmed 2026-08-19
against a head-height wall: the probe armed, the jump key vaulted instead of jumping, and the
player ended up standing on top. The animation came later, in two steps - asking the game to climb
(2026-08-19), then forcing it when the game's own search declines (2026-08-21). Both are below.

The motion, recorded from memory at 20Hz as it happened:

```
20.36s   z 11.121   start (footing 10.068 + the ped origin's 1.040)
20.70s   z 13.223   rise done, 0.55m forward - the 25% share
21.03s   z 13.116   stepped on and settled
21.36s   z 13.116   standing, not falling, not ejected
```

`13.116` is exactly `ledge 12.076 + origin 1.040`, so the offset cancellation is right to the
millimetre, and the whole move takes 0.67s. **The game accepts a written position on top of a
wall** - no collision rejection, no sinking, no snap-back - which was the real unknown in the
fallback path.

**Measured constants, since they anchor every setting here.** World units are metres: a waist wall
reads **+0.908** above the player's footing, a head-height wall **+2.008**. The ped origin sits
**1.040** above the surface it stands on - the number this design deliberately avoids needing, now
known anyway. The default band (**1.50 .. 3.03**) therefore excludes the waist wall by design and
catches the head-height one, with room above it for a tall fence.

The design decisions, in the order they were made:

**It lives in the fork, not in `MAIN.SCM`.** The script VM has no raycast and runs at script
granularity; everything this needs — per-frame probing, a state machine, memory writes — the fork
already does.

**The trigger is the jump key, contextually.** Space vaults when a ledge is in front of the player
and jumps when there isn't, so nothing is taken away and there is no new key. `VaultTick` runs
*before* `ApplyMapping` for exactly this reason: it consumes the press on the tick it happens, and
`ApplyMapping` then sends the game nothing at all for the duration. Reversed, every vault would
begin with a hop.

**Height is measured against the player's own footing, never against the ped's position.** Nobody
knows where in the character the entity origin sits — GTA peds carry it somewhere around the hips —
so the first of the five probe lines is fired straight down *through the player*, and every height
is a difference against what it hits. The unknown offset is in both terms and cancels. This is also
what makes the landing height work at the far end: the ped is placed at `ledge + (its own height
above the surface it was standing on)`.

**The probe's start height is the climbing ceiling, for free.** `FindGroundZFor3DCoord` only
reports surfaces *below* the point you ask from, so a wall taller than `probeCeiling` above the
footing simply does not register — no check, no arbitrary rejection rule.

**Eight lines, once every one or two frames:** one through the player (footing), six at the wall
(near to far, nearest match wins), one past it (somewhere to go). Six at the wall rather than three
because **a vertical line only reports what it is dropped through**, and a fence rail a hand's
breadth deep falls between lines spaced 0.40 apart far more often than it falls on one - and a
fence that is missed reads as *nothing in front of the player*, not as a fence that was refused.
Six across the same reach puts them 0.16 apart. That is the whole of the "narrow models" fix; the
rest of the probe never cared how wide anything was.

**The last line picks between two moves rather than only permitting one.** A far side level with
the ledge is a surface to stand on - a pull-up **onto** it, which is the case the game can animate.
A far side *below* the ledge but no more than `landingDrop` (1.50) below the player's own footing is
a fence: go **over**, clear the top, and come down on the other side. Deeper than that is a parapet
with the street underneath, and higher than the ledge is a second wall with no room between them;
neither arms. A going-over vault never asks the game to climb, whatever `preferNative` says - its
climb-out finishes standing on top of what it found, and on top of a fence is a rail.

**How the query works at all** is the genuinely new capability here, and it is written up in
"Asking the world a question" in [docs/VCS_ADDRESSES.md](../VCS_ADDRESSES.md): a ~40 instruction
MIPS program in a `userMemory` block, run on the game's own thread through `hleEnqueueCall`,
because the function takes its arguments in `$f12..$f14` and that call path fills `$a0..$a3` only.
It is asynchronous — ask on one tick, read on the next — which a ledge probe can afford.

**The one hard rule, learned by crashing the game (2026-08-19):** a call may only be enqueued from
inside a syscall, from one that neither blocks nor reschedules, and **not from an interrupt
handler**. Enqueuing from `VCS::Tick` — a vblank timing event, not a syscall — planted the call on
an unrelated thread and produced `Corrupt stack on HLE mips call return: 28fefefe`, a stack-fill
pattern with a marker half written over it. `RequestGroundZ` now only *prepares* a question;
`WorldQueryDispatch` sends it.

**The host that works is `sceGeListEnQueue`**, and finding it took three wrong guesses that are
worth keeping, because each was wrong for a different reason:

| candidate | why not |
|---|---|
| `sceKernelGetSystemTimeLow`, `sceKernelLibcClock`, both dcache calls | call `hleReSchedule` |
| `sceCtrlReadBufferPositive` | blocks waiting for the next pad sample |
| `sceDisplaySetFrameBuf`, `sceDisplayIsVblank`, `sceKernelPowerTick` | **VCS calls them from inside its vblank interrupt handler** |

That last row is the interesting one and it was measured, not guessed: with a refusal reason
written into the block, the count climbed with `refusedWhy = "intr"` and `refusedThread = 0x110`,
which is `idle0` — i.e. an interrupt borrowing whatever context was current. VCS imports
`sceKernelRegisterSubIntrHandler`, and the thread list has no render thread, so the flip had to be
happening somewhere other than `threadmain` (`0x116`, identified by the pad read). **An interrupt
is not a thread**, and a call planted on one lands on a stack the game never used — which is very
likely what the original crash actually was, rather than merely "not a syscall".

The display-list submit is the right host for the same reason the flip looked like one: every game
does it once a frame from its own main loop. It just does it from the loop rather than from the
interrupt.

#### The animation: the game climbs it, and this only asks

**Confirmed in play, 2026-08-19: the player pulls up with the game's own animation.** Two of the
game's functions do all of it - `CanClimb(ped, &result)` searches for something to climb and
`StartClimb(ped, &result)` plays the pull-up and moves the ped - and `VCSWorld` runs both in one
dispatch when the jump key fires. See "The climb-out is two calls" in
[docs/VCS_ADDRESSES.md](../VCS_ADDRESSES.md) for how they were found and what the struct holds.

So there are two motions now, and the fallback is not dead code: the game's own search can decline
a wall this fork's probe was happy with, and the written-position curve is what runs when it does.
The Vault tab counts them separately for exactly that reason - a native count stuck at zero while
attempts climb means the game is refusing every ledge, which is a different problem from the vault
not triggering.

#### Forcing it, when the game's own search says no

**Status: working in play, ON by default (2026-08-21).** `VaultSettings().forceNativeClimb`, or the
last checkbox in the Vault tab. **The animation now runs on very nearly every vault**, against
roughly one in ten before this.

Measured first, which is what prompted it: **8 of 10 asks were declined**, 0 unanswered, 2
animated, across 22 vaults. The query path is healthy; the game's own search simply refuses most of
what this fork's probe arms. Widening the band made that worse, not better - our band and
`CanClimb`'s are different searches and only the game's one gates the animation.

**The lever is that `StartClimb` never asks where its struct came from.** `CanClimb` only *fills*
one - `found` at +0x00, entity at +0x08, target at +0x10 - and the two are separate functions with
a plain pointer between them. So the climb program now fills that struct itself when the search
declines and calls `StartClimb` on it anyway. The animation, the ped motion and the landing are
still entirely the game's; only the *decision* is taken away from it. This is the fallback
`kVCSPedSetClimbTarget` was written down for.

**The entity turned out not to matter, and that is the finding here.** `StartClimb` hands entity and
target to `0x0890f6ec`, which takes a *reference* on the entity and stores the target *relative to
it*, so the expectation going in was that a real entity would be needed - and getting one means
calling `ProcessVerticalLine` rather than the `FindGroundZFor3DCoord` wrapper the probe uses, an
eleven-argument call with three arguments past the eight this build passes in registers, whose
stack layout is not something to guess at (see the corrupt-stack crash above).

So the entity was made a **host-supplied field in the block** and the host passed `0`, to test the
cheap question first: *does a forced `StartClimb` animate at all?* **It does.** `0x0890f6ec`
null-checks the entity, and a target handed over with no entity to be relative to is taken as
world-absolute - which is exactly what a vault wants. `ProcessVerticalLine` is therefore not needed
for this, and the field stays host-supplied in case something later does want a real entity (a
climb onto a *moving* object would: with a null entity the target cannot track it).

**A climb that never engages is recoverable**, and the guard stays even though it has not needed to
fire. `VaultPhase::Climbing` waits `kClimbEngageTicks` (8) for the ped state to actually reach 44,
and drops back to the written motion if it never does - re-anchored to where the abandoned climb
left the ped, since the state-44 experiment dropped him 0.57 into a hang before giving up. Without
it, a forced climb the game accepts and then ignores would leave the player standing at the wall
having pressed jump for nothing. The Vault tab counts it as `never engaged`.

**The band was then walked up with the animation running, and the edge is 3.03** - not a round
number, and 3.10 is past it: the climb stops carrying. Worth keeping straight that this is still a
much higher ceiling than the game's own search would ever have allowed - `CanClimb` was refusing
about four walls in five inside this same band before forcing existed.

**With forcing on, fences are asked for too.** The reason they were excluded is that the game's
climb-out finishes on top of whatever *it* found, and on top of a fence is a rail - but once we are
supplying the target, it can be pointed at the far side instead. The target convention is a guess
(the landing surface point, not the ped origin) and is the first thing to calibrate if a forced
climb runs but lands somewhere wrong.

**Four things were tried before the two calls, and each one looked like the answer.** They are
written up in the addresses doc rather than repeated here, but the shape is worth carrying:

- the climb **stage** byte (`CPed+0x1d9`, 0 then 1..4) is a *status*, not an input - writing it on
  land sticks and does nothing;
- the climb **state** (`CPed+0x8b4 = 44`) engages the game's climb and then aborts within a second,
  because nothing told it *what* to climb;
- the **in-water flag** cannot be forced - the game recomputes it from the world every frame, and
  the attempt left the ped under the collision and dropped him through the map;
- `0x0892f140` is the **script command's** anim API, not the engine's - measured, zero calls during
  ordinary play.

The through-line: **a state is not an entry point.** Three of those four were real fields with the
right values in them, and setting a field the game writes is not the same as doing the thing the
game does when it writes it. The entry point was a function all along, and the way to a function
nobody can name is to break on the field and read the stack.

#### The splash, and why a vault on dry land made one

**Status: fixed 2026-08-28.** The animation this whole feature wanted is the climb *out of the
water*, and partway through it the game plays a splash. On a quay that is the sea letting go of
you; on a fence in Little Haiti it is a bug. Nothing on that path tests for water, and nothing had
to — before this fork there was no way to reach the climb on dry land.

One call does it, in the anim finish callback at `0x08905824`, on the branch taken when the climb
stage goes 1 → 2: `jal 0x08a05f80` at **`0x08905920`**, with sound **24** in the delay slot. See
"The splash the climb-out makes" in [docs/VCS_ADDRESSES.md](../VCS_ADDRESSES.md) for the audio
addresses and for how 24 was *established* to be the splash rather than assumed — the ped code at
`0x08927740` plays the same id and plays it only when the in-water flag is set.

**The sound is refused, not the code rewritten.** `cAudioManager::PlayOneShot` drops a negative
audio entity id on its first instruction, so `Hook_vcs_climb_splash` — a `REPFLAG_HOOKENTER`
replacement installed by address, exactly as the fire hook is — writes `-1` into `$a1` and lets
the call run to nothing. That keeps the whole thing to one conditional register write, with the
game's own instruction still in place and restored at shutdown.

**It is gated on whose climb it is, not on whether the ped is in water**, and the difference
matters. The in-water flag is recomputed from the world every frame, so by the time the ped has
cleared the edge it may already have gone off; testing it would silence the very climb the sound
was written for. The vault knows for certain which climbs are its own, and a genuine swim to a
quay never goes through it. The Vault tab counts the ones it has silenced, because a count stuck
at zero while vaults animate is a hook that never installed — which looks nothing like a hook with
no work to do.

**Where the audio lives, since nothing here had touched it before.** The retail build still
carries the sound service's debug strings, and
`set voice: voice=%d sfx=%d bank=%d addr=%x length=%d` at `0x08b7400c` is what located the module
at all. From there: the ped one-shot queue at manager `+0x187a` (stride `0x38`) gave
`PlayOneShot` at `0x089b83c0`, and listing every call site with its constant sound id gave both
the splash and the proof of what it is. All of it offline, from a savestate, with
`Tools/vcsstatic.py`.
