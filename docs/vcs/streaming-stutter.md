# Streaming stutter and instant texture loading

> Read when chasing hitches while driving, or before changing `CacheFullIsoInRam` or texture loading.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

### Streaming stutter, measured - and `CacheFullIsoInRam` is worth its memory

"Average fps" is useless for this. A run that freezes for a third of a second once a second still
averages close to thirty, and thirty is what every earlier measurement in this file reported while
the player was describing a stutter. What a player feels is the LONGEST gap, so the instrument is
the distribution of intervals between increments of the game's own frame counter, sampled as fast
as the debugger will answer, while the player is dragged across the map to force streaming.

Same four-stop tour, same build, one boot each:

| | cache off | cache on |
|---|---|---|
| median frame | 33.3 ms | 33.4 ms |
| 90th percentile | 35.7 ms | 34.8 ms |
| **99th percentile** | **107.0 ms** | **51.9 ms** |
| **worst single frame** | **417.6 ms** | **251.9 ms** |
| **frames over 100 ms** | **1.0%** | **0.3%** |
| frames completed in the same wall time | 583 | 707 |

So it is a real fix for a real part of the problem: **three times fewer hitches and half the
99th-percentile frame time**, and a fifth more frames completed in the same wall clock, which is the
stalled time coming back. It does not cure it - a quarter-second freeze still happens - so whatever
else is behind the remaining spikes is not disc latency.

It costs a slower boot and 1.6 GB of memory: the ISO is read whole at startup, which took **101
seconds against about 25**, and the process sits at 2.6 GB rather than 1.0. On a machine with the
room that is a good trade for a game that streams constantly; on one without it, it is the first
thing to turn off. `IOTimingMethod` is already Fast and is not the lever.

**A tooling note that cost several rounds, and is not about the game at all.** The debugger's memory
reads are serviced on the CPU thread, so anything that stops the emulator - this fork's own pause
menu is a `UIScreen`, and a `UIScreen` pauses emulation - leaves every read unanswered rather than
answered late. A measurement script that treats a read as reliable will hang, and its traceback will
point at the read rather than at the menu. Two further shapes of the same trap turned up in one
session: a probe that reported "the CPU is stopped" when the read had actually succeeded and merely
returned a null player pointer, and a screenshot helper that captured the wrong window because
`Process.MainWindowHandle` did not name the game's - enumerating top-level windows by process id and
picking the visible one is what works. Guard every read, and say which of the two things went wrong.

### Driving stutter was Instant texture loading, and it only waits on 2D now

**Status: cause confirmed from play, rule built.** `ReplacementTextureLoadSpeed` is on Instant for
a reason: on any other speed the splash and credits art shows its low-res original for a moment
before the HD file lands. But Instant waits for EVERY replacement inside the frame that asked for
it, and a chunk that streams in brings dozens of world textures at once - so driving fast into a
new area loaded all of them synchronously and the frame stalled. Switching to Fast removed the
stutter and brought the low-res splash back.

`TextureCacheCommon::PollReplacement` now keeps both: while VCS is active and the speed is
Instant, a 3D draw during play does not wait - it takes its replacement as it arrives, a frame late
at worst, exactly as Fast does - while through-mode draws (splash, credits, menus, loading screens,
the HUD) and everything before `BootPhase::Intro` ends still wait. No setting changed.

**The harness did not reproduce it, and that is worth knowing before trusting one.** Dragging the
ped east at 40 units a second from slot 2 logged one 100 ms stall and not a single frame blocked on
textures. The writes only covered 500 units in 28 seconds, a teleported ped is not a car, and every
step that moved the follow camera far enough made the water module forget the sea - so the path
exercised the streamer far less than driving does. The confirmation is from play.

Two log lines stay, both silent in normal play: `N ms between vblanks at game frame F` past 100 ms
(`VCS::Tick`), and `last frame spent N ms on M texture replacements` past 50 ms
(`TextureCacheCommon::StartFrame`). A replacement line during play means the rule above has stopped
holding.

### The sea painted over the player: two rejects that should never see a person

Reported from play as the water overlapping the character "sometimes". The depth pre-pass is the
only thing that keeps the sea off anything in front of it, so a person rejected from it is a person
the sea is composited over. Two tests could reject one, and neither should:

- **Blended.** The game fades people with their material alpha, which turns its copying blend into
  one that alters the destination, so `BlendAltersDestination` filed a fading player as glass. The
  same applied to the depth-write test.
- **Near the camera.** The test that throws out the game's full-screen overlays - geometry sitting
  entirely within two units of the camera - also caught a separate piece of the player, a head or a
  hand, whenever the camera pulled in behind him. Measured, not reasoned: the first stutter run
  logged `a person was left out of the occlusion pass (sits on the camera)`.

A skinned draw is now exempt from all three. The overlays are flat quads and never skinned, and a
faded person occluding costs the vanilla sea showing through them for the length of the fade -
the same good way round as the alpha-tested palm fronds. `NoteReject` logs once per reason when a
person is still rejected, so if it comes back the log names the test.

`VCSShadow::AddCaster` has a near-camera cutoff of the same shape and was left alone: nothing about
shadows has been reported, and a person's head missing from the shadow pass while the camera is
inside two units of it is a much smaller thing than the sea covering it.

### Two traps in patching this emulator's code

Both cost real time on the draw-distance port, and neither is visible from the API. Anything that
patches game code will meet them again — the fire hook only escaped them because it patches a
`jal` in the middle of a function, which no block starts at and nothing reinstalls.

**PPSSPP hides the game's instruction behind its own marker.** Once a block has been compiled, the
raw word at its first instruction is a `0x68xxxxxx` RUNBLOCK marker, so a raw read sees neither the
game's opcode nor your replacement. Every check has to go through `Memory::Read_Instruction`, which
resolves both back. The sharp edge is on the way out: `RestoreReplacedInstruction` reads the RAW
word and silently declines unless it sees a replacement marker there — so uninstalling a hook whose
address has since become a block start does nothing at all, and the hook stays live while every
flag in your own code says it is gone. Invalidate the block FIRST, then restore.

**`WriteReplaceInstructionAt` returns false for success.** It reports false both when the write
failed and when the identical replacement was already installed. Treating the return value as the
answer made a working, running hook report "far clip replacement refused" on every frame after the
first — which, combined with the trap above, produced a remove/reinstall loop that left the feature
permanently half-installed. Ask what is actually at the address instead of what the writer returned.

### The mounted gun: one state where CameraYaw is not a world angle

A mission can strap the player to a vehicle with a weapon instead of seating him in it - `02B6
attach_ped_to_car $PLAYER_CHAR car $5666 offset 1.6 1.0 0 position 3 angle_limit 70.0 weapon 34`,
which is `GON_C4`'s helicopter ride. **`PlayerVehicle` reads 0 throughout**, so the fork sees an
ordinary on-foot player holding a gun and every on-foot assumption about the camera is wrong at
once.

The one that matters is what `CameraYaw` MEANS. `CCam::Process` mode 45 branches on the attachment
at `0x089a3548`: on that branch it starts `Beta` at ZERO (`0x089a3578`) rather than at `PedHeading
+ PI/2` (`0x089a3584`), skips its own `[0,2PI)` normalise after integrating (`0x089a3980`), and
clamps instead. So while attached the field is a **signed offset from the vehicle's nose**.
Measured by asserting it every frame against a savestate of that ride:

| assert | look, relative to the helicopter |
|---|---|
| 0.00 | 0.0 deg |
| 0.40 | 22.9 deg |
| 1.00 | 56.3 deg |
| -0.40 | -23.8 deg |

Before that first write it sat at exactly 70.0 deg - pinned against the clamp, because this fork
was asserting an absolute world yaw of 3.82 rad into it. Reported in play as an aim that snapped
back to one fixed direction however far the mouse moved.

Two faults, and fixing either alone leaves the other. The anchor was never retaken, because
entering the attachment is a camera change that no CONTEXT change announces - and with `returnLook`
off the hold parks open, so the yaw anchored before boarding is asserted for the whole ride. And
`WrapYaw` forced the value into `[0,2PI)`, which destroys half of an arc: `-0.4` is a small offset
left, `5.88` is two and a half turns past the stop. It re-anchors on both edges now, and clamps to
the game's own limits instead of wrapping.

The limits are the mission's and are read rather than assumed: `PedAimYawLimit` (`ped+0x760`) is
`attach_ped_to_car`'s `angle_limit`, and pitch gets `ped+0xCA0` / `+0xCA4` - 10 deg up, 55 deg
down, written by script opcodes `04CF` / `04D0` as `degrees*PI/180`. Asymmetric because a door
gunner looks down.

**The section is short and cannot be held open on request, which is the other lesson here.** Three
mission runs went into chasing it live, one of them failed by a probe of ours that held the aim
trigger and swept the stick for four seconds. What ended it was a savestate taken the instant
`ped+0x848` went non-zero - a script polling that field over the WebSocket debugger and driving
PPSSPP's own quicksave through `WM_COMMAND`. After that the same moment could be reloaded, written
to and read back as often as needed with nothing at stake. Reach for that on the FIRST attempt at
anything that only exists for a few seconds of a mission.

### The right number in the wrong space

Two separate bugs in one day had the same shape, and neither looked like it at first: both read as
"the game is fighting us", and both were **a correct value written into a space where it means
something else**.

- The auto-save wrote a restart position that was correct for the safe house's interior, into a
  save whose interior state said "outdoors". The pair disagreed, and the player materialised inside
  a building.
- The mounted gun got a correct WORLD yaw written into a field the game reads as an offset from a
  helicopter's nose. It pinned at the clamp.

Neither is a wrong number, and neither is a race. Both are a value whose meaning depends on some
other piece of state - an interior index, an attachment pointer - that was not consulted, and in
both cases the game itself pairs the two consistently by construction: its own save routine only
runs where the pickup is, and its own camera sets `Beta` to zero when the attachment begins.

So when a write "does not take", or takes and then springs back, ask what ELSE has to be true for
that field to mean what you think it means, before reaching for a rate, a spring or a fight over
who writes last. The three questions that separated these two from an ordinary tug of war:

- **Does the game write this field itself, and from what?** Read the writer, not the field.
- **Is there a nearby field that changes what this one means?** `$_282` and `ped+0x848` were both
  one `lw` away in the very code being read.
- **Does the value's RANGE make sense in the space I think it is in?** 3.82 rad in a `+/-1.22`
  window says the space is wrong long before any behaviour has to be explained.

## Working on this

Build and test exactly as upstream describes. To exercise the VCS layer you need the game
running — the module only activates on a real `ULUS10160` boot.

The debugger window is at **Debug → Tools → VCS**. It persists its open state in
`memstick/PSP/SYSTEM/imdebugger.ini` (`vcsOpen`).

`Tools/vcsstatic.py` reads the game's code **offline**, from a savestate, with nothing running —
that is the one to reach for first now. `Tools/vcsdisasm.py` does the same over the WebSocket
debugger against a live game (`python Tools/vcsdisasm.py 0x0898bb4c 24`), which is what you want
when the question is about a *moment* rather than about what the code does.

Reading the game's actual code is far faster than scanning for values, and the record is
lopsided: the second-stick mechanism came out of two functions this way after weeks of guessing
from the outside, and the aim response curve came out of one function after five rounds of
value-correlation had failed to find something that turned out not to exist. When a question is
"what does the game do with X", disassemble; correlation is for "where does the game keep Y".

For finding addresses, use `Tools/vcsscan.py` (run PPSSPP with `--debugger=1337`). It adds the
snapshot-and-diff narrowing that PPSSPP's own `memory.search` can't do — `memory.search` only
finds values you already know, which doesn't help for things like a vehicle pointer. See
[docs/VCS_ADDRESSES.md](../VCS_ADDRESSES.md).

Adding a new address is a three-step change and touches nothing else:

1. Add an enum value to `VCSAddr` and a matching row to `kVCSAddresses` (a `static_assert`
   catches a mismatch).
2. Read it in `VCSState::UpdateState` if it belongs in the decoded state.
3. Document it in `docs/VCS_ADDRESSES.md`.

### Gotcha found the hard way

Passing PPSSPP an unwritable log path — e.g. `--log=C:\foo.log`, since the drive root needs
admin — aborts the process at startup with a CRT "abort() has been called" dialog and no log.
This is upstream behaviour, unrelated to this fork; it reproduces with the compat flag off. Use
a writable path, or enable `FileLogging` in `memstick/PSP/SYSTEM/ppsspp.ini` instead.
