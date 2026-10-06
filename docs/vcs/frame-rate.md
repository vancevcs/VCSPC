# Frame rate: 30 and 60

> Read before touching the `Frame limiter` row, `ApplyFrameRate` in `Core/VCS/VCSGame.cpp`, or the
> frameskip seam in `Core/HLE/sceDisplay.cpp`.

### 60 fps without slow motion

**Status: working, measured live on the Mac, off by default.** `Frame limiter` on DISPLAY SETUP,
30 or 60. At 60, four things go on together and at 30 they all come off:

```
the cap          0x08A070B4  sltiu $a0, $a0, 2  ->  1           (kVCSFrameLimit)
the ms carry     four words in CTimer::Update                    (kVCSTimerCarry)
the CPU clock    222 MHz -> 666 MHz, unless the player locked one (kVCSFrameRate60ClockHz)
auto frameskip   forced on in sceDisplay's frame timing           (VCS::ForceAutoFrameSkip)
```

At 30 the game runs as it shipped: no word patched, the clock handed back. All of it is switched
live from the pause menu and re-checked every tick, so a savestate can't leave a stale patch behind.

#### How the game paces itself

The vblank interrupt handler (`0x08A079C8`, sub-interrupt 15) counts vblanks into `gp-0x2264` and
kicks the display lists. The main loop at `0x08A070A8` calls `sceDisplayWaitVblankStart` until that
count reaches 2, then zeroes it. That's the whole 30 fps cap: one `sltiu` immediate.

CTimer's clock is `sceKernelGetSystemTime` (`0x0897F9B0`): microseconds × 1e-6, capped at 0.1 s,
×1000 into ms. `CTimer::Update` (`0x08A11214`) turns that into ticks (294912 per ms), and from them:

- the timestep (ms / 20), clamped to 0.5–3.33;
- the integer ms clock, `gp+0x1dec`.

So **the game's speed follows the emulator's clock, not the frame count**. Halving the frame time
halves the timestep, and the game moves at the same speed.

#### What the slow motion actually was

Cap patched over the debugger, everything else unchanged (6x resolution, ULTRA shadows, HIGH water,
SMAA, AO):

```
cap 2:  29.9 fps   timestep 1.668   988 game ms per wall second
cap 1:  47.6 fps   timestep 0.834   783 game ms per wall second   <- slow motion
```

The timestep says 60 emulated frames a second. The wall clock says 47.6. **The host couldn't draw
60 frames a second, so PPSSPP slowed emulated time to match, and a game that follows that clock
slowed with it.** With the passes off, or at 2x, the host kept up and speed stayed at ~98%.

The fix is PPSSPP's own auto frameskip: when the host is behind, skip *drawing* a frame instead of
slowing emulated time. The player's `FrameSkip`/`AutoFrameSkip` settings aren't touched;
`UseAutoFrameSkip()` and a new `FrameSkipSetting()` in `sceDisplay.cpp` OR in
`VCS::ForceAutoFrameSkip()`, with at most one consecutive skip, so it never drops below 30 shown.
Measured on the same heavy settings: **60.0 game frames a second, 1001 game ms per wall second,
50 shown**. Facing the sea at the beach it shows 60.

#### The CPU clock

The game runs at **222 MHz** here: that's what was on the clock when 60 first raised it, and what
30 handed back. At 222 MHz the game alone (no host limit, 2x, passes off) made 48–57 frames a
second on Ocean Beach and the start spot, with timesteps up to 0.94 (19 ms frames). At 666 it held
59.9 in every spot measured.

It's raised with `CoreTiming::SetClockFrequencyHz` from the tick, not through `iLockedCPUSpeed`, so
nothing is saved to the player's config. Skipped when the player has locked a clock of their own,
or when compat says `RequireDefaultCPUClock`. It costs the host nothing while the game waits,
because the wait is a blocking syscall.

#### The millisecond carry

The ms clock is an integer, and each frame adds its length **truncated**: 33.37 ms becomes 33 at 30
fps (1.1% slow, as shipped), and 16.68 becomes 16 at 60 (4% slow: 959 game ms per wall second
against 988). The time of day, script timers and mission countdowns all count in it.

The game already carries the remainder on a path guarded by the byte at `gp-0x1ba8`. That byte is
read at ~300 sites, so it's not one to borrow; the path quantizes to 16 ms, which looks like
multiplayer lockstep. The four words in `kVCSTimerCarry` take that path always, at **1 ms**
(294912 ticks, built in `$t1`, which is free across the function). Every frame is then a whole number
of milliseconds and nothing downstream loses anything. Measured: **1002 game ms per wall second**,
and the timestep alternates 0.80/0.85 (16/17 ms) where it was a steady 0.834. The remainder
global, `gp-0x1fc0`, is used only by that path; it's zeroed on install.

#### Testing it

`fps.py`-style measurement, over the WebSocket debugger: sample `FrameCounter` (`0x08BB3BB4`),
`TimeStep` (`0x08BB3B5C`) and the ms clock (`0x08BB3B4C`) against the wall clock. Game ms per wall
second is the speed. 1000 is right; 960 at 60 means the carry is off; much less means the host is
behind and frameskip isn't engaging.

#### What is NOT verified

- Anything in the game that steps per *frame* rather than per timestep. Movement, the clock and
  timers were measured. Physics edge cases, particles and AI were looked at but not measured at 60.
- The Windows build and a phone. The logic is the same, but frameskip's cost/benefit there wasn't
  measured.
- Driving at speed through dense streaming at 60. The CPU clock was chosen from standing samples.
- A savestate taken at 60 and loaded at 30 in a fresh session keeps 666 MHz (harmless: the cap
  still holds 30), because the tick only gives back a clock it raised itself.
