# Verified status

> A log of what was confirmed working on the real game, and how. Historical; the other files carry the reasoning.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

## Status

**Working and verified on the real game:**

- Disc ID detection, compat-flag gating, init/shutdown lifecycle.
- Per-frame tick (the VCS window's tick counter climbs — a frozen counter means the vblank hook
  is broken, which is a different failure from a wrong address).
- The debugger window: live state, the address table, the scratchpad, the mapping table, the
  camera diagnostics.
- Scratchpad reading real memory and rendering `-` for out-of-bounds addresses without faulting.
- The `NativeKey` hook and the full input chain: physical key → `HandleHostKey` →
  `ResolveContext` → `ComputeButtonMask` → `sceCtrl`, verified end to end (Space on foot shows
  context `OnFoot` and mask `0x00008000`, `CTRL_SQUARE`, Jump).
- **Context-aware bindings**, confirmed in play. On foot and in vehicles behave differently.
- **Vehicle class detection**, confirmed in play. `VehicleModel` reads back correctly through the
  live `PlayerVehicle` pointer (209 `patriot` in a Patriot), and every non-car class has now been
  flown or driven: the aircraft and boat bindings behave as intended, across all 15 special models
  the spawner offers (8 helicopters, 4 fixed-wing, hovercraft, jetski, predator). What made that
  testable was a car spawner - see "Spawning special vehicles" below.
- **WASD movement**, confirmed in play, on foot and driving. Analog on foot; in a vehicle A/D
  steer the stick while W/S stay buttons.
- **Mouse look, through the chase camera** (2026-09-15). Yaw and pitch on foot, driving and flying,
  with the camera pulled in by the game's own collision instead of fighting it. Measured over the
  debugger on foot and on a motorbike; not yet played by hand, and aircraft untested. See "The chase
  camera: the view is built, not negotiated". Not during free aim - there the mouse is the reticle.
- **Lock-on aiming**, which is how every ordinary weapon in VCS aims. Holding the aim key gives
  the aim bindings (Q/E cycle targets), WASD strafes, and the mouse keeps driving the camera.

- **Mouse free aim, yaw and pitch both tracking**, with ordinary weapons. Requires the
  "Drive the game's second stick" toggle, which sets the mode flag. See "Mouse free aim — how it
  actually works" for the three conditions and the two channels involved.

- **Free aim that feels like a mouse rather than a thumbstick**, confirmed in play. The game's
  axis→rotation response is a signed square with an exponential smoother on top; `aimResponseModel`
  inverts both, so mouse displacement becomes aim displacement the way it already does for mouse
  look. Read out of the game's own code rather than guessed — see "Why free aim felt like a
  thumbstick, and what fixed it".

- **Lock-on no longer appears at all** when aiming. It was never the game being stubborn: the Free
  Aim press waited 9 ticks after the aim trigger, which at 60Hz against a 30fps game is ~150ms of
  open season for the game to find a target. `aimFreeAimDelay` is 0 now — Free Aim is pressed on
  the same tick as the trigger. No memory writes, no fighting the targeting code; the bug was ours.

- **Mouse-native free aim, end to end**, confirmed in play and described there as *"genuinely
  smooth, how I wanted it... feels like PC native"*. Four pieces had to be right at once:

  | | |
  |---|---|
  | the ray | built from the camera's stored `Front`/`Up`/`Source`, not reconstructed from the angles |
  | its length | the weapon's own range out of `CWeaponInfo`, not whatever the game's target implied |
  | the gun | `m_pPointGunAt`'s entity moved onto the crosshair, down the same ray the bullet takes |
  | vertical | a deadband lead on pitch, because the aim camera will not move until the angle leads it |

  **The settled configuration**, which is now the default: crosshair 0.5/0.5, yaw trim 0,
  camera-origin ray on, weapon range on, `aimPitchDeadband` **0**, `aimYawKick` **0**,
  `aimYawDeadband` **0**, everything else off. `pedFollowAim` off - the game turns the character
  itself. The `vertical` row above is superseded: neither axis stalls once the gun target is placed
  along the asserted aim - see "There was never a deadband" below. Until 2026-09-15 this line read
  0.165 and 0.166, and those two leads were the rightward free-aim drift.
