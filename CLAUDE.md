# GTA: Vice City Stories "PC version" fork of PPSSPP

Turns PPSSPP into a dedicated PC-feeling front end for **GTA: Vice City Stories** (PSP, disc ID
`ULUS10160`, USA build only): context-aware key bindings, true mouse look, mouse free aim, direct
weapon selection, a GTA-styled pause/front end, and renderer features (sun shadows, water).

**This file is rules and a map, not a log.** Deep notes, measurements and dead ends live in
[docs/vcs/](docs/vcs/) — one file per subsystem, indexed below. Read the one you need; don't read
them all. A finding goes in the matching `docs/vcs/` file. Only a rule that applies to *every*
task earns a line here — keep this file under ~150 lines.

## Hard constraints

Breaking any of these is a bug, not a style issue:

- **Zero behaviour change for every other game.** Everything is gated on the `VCSInputOverhaul`
  compat flag *and* the disc ID. With the flag off, `VCS::Tick()` returns on its first line.
- **PSP memory is read on the emu thread only.** Never the UI thread.
- **No hardcoded addresses outside `Core/VCS/VCSAddresses.h`.** Not one.
- **It must build and run with any subset of addresses unset.** Unset entries read as `unset` in
  the overlay, never garbage, and any feature that needs a missing address must simply not engage.
- **Minimal diff to upstream.** Don't refactor PPSSPP code unless strictly necessary; this fork
  has to stay easy to rebase.

## Build and run

- **Windows only, through the solution** (never a stray `build/` dir):
  `MSBuild Windows/PPSSPP.sln /t:PPSSPPWindows /p:Configuration=Release /p:Platform=x64 /m`
  (~10 min). Find `MSBuild.exe` via `vswhere` — snippet in [AGENTS.md](AGENTS.md), "Build and Validation".
- **Both exes land in the repo root.** Release is **`GTA Vice City Stories.exe`** (play and measure
  on this one); Debug is `PPSSPPDebug64.exe` (for breakpoints). A `PPSSPPWindows64.exe` there is an
  old pre-rename build. **Check the exe's timestamp against the source before trusting a
  measurement** — three runs once measured a week-old binary.
- **New source file:** list it everywhere its module lists its neighbours — the module's
  `CMakeLists.txt`, `.vcxproj`, `.vcxproj.filters`, the UWP pair, `android/jni/Android.mk`, and
  `libretro/Makefile.common` (Core and GPU only). Copy how `VCSChaseCam.cpp` (Core),
  `VCSShadow.cpp` (GPU) and `VCSMenuScreen.cpp` (UI) are listed. UWP is the easy one to forget.
- **Live debugging:** run with `--debugger=1337`; `Tools/wsdbg` is the client. Breakpoints need the
  interpreter (`CPUCore = 0`); the JIT skips them.
- **Logging:** `INFO_LOG` is invisible at default levels. Probe with `WARN_LOG` or higher.
- **Mouse input needs "Use Mouse Control" on** (Settings → Controls, `UseMouse`). `VCS::HandleMouseDelta`
  consumes the delta before PPSSPP's own mouse-to-analog runs, so leaving it on costs nothing.

**Upstream rules ([AGENTS.md](AGENTS.md)) are not auto-loaded.** Read the relevant section when
you touch that area: adding a CLI option (`Core/CmdLine.cpp`), an HLE module, a `System_*`
function (six platform entry points), the WebSocket debugger, unit tests, headless/frametests,
Android or libretro builds. The three that bite here: **never `git push` without asking**; call out
regression risks for HLE/CPU/GPU/timing/threading/memory changes; mind savestate compatibility
when changing serialized state.

## Code map

```
Core/VCS/                      no dependency on ImGui, UI, or any renderer
  VCSAddresses.h    constexpr address table — the single source of truth
  VCSMemory.h/cpp   std::optional accessors over PSP RAM; a bad address is never fatal
  VCSState.h/cpp    per-frame decoded game state
  VCSInput.h/cpp    input context enum, (context, host key) -> PSP button table, WASD analog
  VCSCamera.h/cpp   mouse and right-stick look; hands the turn to the chase camera, and writes
                    CameraYaw/CameraPitch directly only for aiming and the mounted gun
  VCSChaseCam.h/cpp the camera the player looks through on foot, driving and flying - built
                    inside the game's camera update, pulled in by the game's own line of sight
  VCSFireHook.h/cpp free aim resolved at the fire site, by rewriting the raycast's target
  VCSWorld.h/cpp    calls the game's own collision code, via a program written into PSP memory
  VCSVault.h/cpp    ledge detection and the pull-up, built on that query
  VCSCheats.h/cpp   the game's own button-combo cheats, typed by a sequencer on the game's clock
  VCSFrontEnd.h/cpp a bridge into the GAME's pause menu - its map, its save list - and the Menu gate
  VCSGame.h/cpp     lifecycle + per-frame tick; the only entry point the rest of PPSSPP sees
  VCSSettings.h/cpp the player-facing option table, and the only thing that persists any of it
GPU/Common/
  VCSShadow.h/cpp   dynamic sun shadows: caster capture, cascade, screen mask, composite;
                    and ambient occlusion, read off the mask's depth
  VCSWater.h/cpp    the sea, and rain on the roads: capture, depth, shading, composite
UI/ImDebugger/
  ImVCS.h/cpp       the "VCS" debugger window (lives here so Core stays ImGui-free)
UI/
  VCSMenuScreen.h/cpp  the GTA-styled pause menu and its option pages
  VCSTouchControls.h/cpp  the phone's touch overlay - which pad controls are on screen, and where
```

Data flows one way: `VCSAddresses` → `VCSMemory` → `VCSState` → `VCSInput` → `sceCtrl`, with
`VCSCamera` branching off the same state to write camera angles directly. `VCSGame` is the only
thing the rest of PPSSPP knows about.

**Seams into PPSSPP** (keep them few; full table and the reasons in
[architecture.md](docs/vcs/architecture.md)): per-frame tick in `hleEnterVblank()`
(`Core/HLE/sceDisplay.cpp`) — *not* `__DisplayVblankBeginCallback`; init/shutdown in
`__KernelInit`/`__KernelShutdown`; host keys and mouse in `NativeKey`/`NativeMouseDelta`
(`UI/NativeApp.cpp`); compat flags in `Core/Compatibility.*` + `assets/compat.ini`; shadow and water
capture in `DrawEngineVulkan::Flush` and `BeginHostFrame`; pause menu via `CreatePauseScreen()` in
`UI/EmuScreen.cpp`.

**Threading:** `NativeFrame()`, `Core_RunLoopUntil()` and `UI/ImDebugger/*` all run on one thread,
so the VCS debugger window reads PSP memory directly. Don't add a lock. Only the legacy Win32
debugger needs `g_frameMutex`.

## Working rules this project keeps re-learning

- **Measure, don't guess.** Guessed bindings, addresses and mechanisms have been wrong every time.
  Cheapest first: the game's own Pause → Controls screen and the control strings in `ENGLISH.GXT`
  (readable offline) → offline disassembly (`Tools/vcsstatic.py` on a savestate, no emulator) →
  live measurement. Before inventing a mechanism, check the game doesn't already have one.
  ([aiming.md](docs/vcs/aiming.md): "Read the game's own Controls screen first", "Ask the GXT")
- **Elegance and priors from other games are not evidence.** A "logic stopped = menu open" detector
  and a d-pad zoom binding both looked right and were wrong. ([input.md](docs/vcs/input.md))
- **Confirm an address by writing to it.** A value the game overwrites at once is derived; one that
  sticks is stored.
- **Only release input you pressed.** `ApplyMapping` clears the bits *it* set last frame and nothing
  else; the analog stick likewise. Otherwise a real pad's presses are cancelled within a frame.
- **Check `Core/KeyMapDefaults.cpp` both ways before touching a binding:** a claimed key is withheld
  from PPSSPP (Escape once trapped the player in the game), and an unclaimed key still does
  PPSSPP's default (Shift rapid-fires). A `psp = 0` row claims a key and sends nothing.
- **When the game's camera mishandles a number you hand it, stop handing it numbers** — replace
  what it produces instead. Two writers of one field always lose. ([chase-camera.md](docs/vcs/chase-camera.md))
- **A quantity valid only in some states is not a constant.** Ask *when* it was measured.
- **Two FOV reference constants, on purpose:** `AimAxisStep` uses `FOV/80` (the game's own), direct
  look writes use `FOVLookScale` = `FOV/70`. They apply to different paths and must never both
  apply to one.
- **Address table:** an *absolute* entry is a fixed global (~`0x088xxxxx`–`0x08exxxxx`); anything
  inside a heap-allocated struct must be a *based* entry (pointer + offset, one level only).
- **Testing input by hand:** the ImGui debugger swallows the keyboard when focused — including the
  VCS menu's Enter (rows highlight but won't activate). Click the game area. Bare modifier keys
  never arrive as events; `KeyInput::keyCode` is a union with the CHAR codepoint.

## Tools

`Tools/vcsstatic.py` offline disasm/read from a savestate · `vcsdisasm.py` disasm over the live
debugger · `vcsxref.py` find functions by structural fingerprint · `vcsscan.py` snapshot/diff memory
scanner · `vcsvehicle.py` measure what each button does in the current vehicle · `vcsaimwatch.py` /
`vcsclimb.py` / `vcsvault.py` / `vcsfiretest.py` watch and probe one mechanism · `vcsgxtkeys.py`
rewrite tutorial text to keyboard keys · `vcsmenuart.py` / `vcsmenusfx.py` front-end art and sounds ·
`vcspuddles.py` puddle shader offline · `vcspackage.py` build the distributable folder ·
`vcstouchicons.py` bake the touch-control icons · `vcsktx2.py` convert the HD pack for phone GPUs.
The address-hunt procedure and every known address are in [docs/VCS_ADDRESSES.md](docs/VCS_ADDRESSES.md).

## Deep notes — read the one you need

Sections cross-reference each other by title: `grep -rn "<title>" docs/vcs`.

| File | Covers | Read before |
|---|---|---|
| [architecture.md](docs/vcs/architecture.md) | why each Core/VCS piece exists, the full seam table, threading | adding a hook into PPSSPP or a new address |
| [front-end.md](docs/vcs/front-end.md) | the pause menu, option pages, controls card, cheat menu, widget tree, focus, page art, menu sounds | touching `VCSMenuScreen`, `VCSFrontEnd`, `VCSSettings` |
| [boot-and-saves.md](docs/vcs/boot-and-saves.md) | boot sequence, main menu at launch, what a save is, autoload, loading screen, driving game dialogs | touching launch flow or the save list |
| [aiming.md](docs/vcs/aiming.md) | lock-on, mouse free aim, response model, deadbands, fire-site ray, second stick, CLEO route | touching `VCSCamera`, `VCSFireHook`, aim feel |
| [chase-camera.md](docs/vcs/chase-camera.md) | the built (not negotiated) chase camera, pitch limits | touching `VCSChaseCam` or mouse look |
| [input.md](docs/vcs/input.md) | what each PSP button does, the gamepad remap, vehicles/aircraft, combat, spawning, script button numbering, the `Menu` context | adding or changing a binding or context |
| [vaulting.md](docs/vcs/vaulting.md) | ledge detection, calling game code for collision, the climb animation | touching `VCSVault` / `VCSWorld` |
| [shadows.md](docs/vcs/shadows.md) | dynamic sun shadows: cascades, mask, caster capture, people/props/vehicles; ambient occlusion | touching `VCSShadow` or its capture hooks |
| [water.md](docs/vcs/water.md) | the sea, wet roads, droplets, the weather source, the `VCS_WATER_*` dev env vars | touching `VCSWater` |
| [draw-distance.md](docs/vcs/draw-distance.md) | why the view is what it is: which levers were measured and are inert | trying to change how far the city draws |
| [streaming-stutter.md](docs/vcs/streaming-stutter.md) | driving/streaming hitches, `CacheFullIsoInRam`, instant texture loading | chasing hitches or texture loading |
| [phone.md](docs/vcs/phone.md) | the Android build, touch controls, HUD taps, the radar corner, widescreen, arranging the controls, the phone menu, the iOS build | touching `VCSTouchControls`, the Android flavor, the iOS build, or the Widescreen row |
| [status.md](docs/vcs/status.md) | log of what was confirmed working, and how | (historical) |
