# Vehicle detail

> Read before touching `VCSVehicleDetail`, the vehicle LOD distances, or what draws at a distance.

`Core/VCS/VCSVehicleDetail.cpp`: how far away a vehicle keeps its full model. The row is **Vehicle
detail** on the graphics page's advanced half, `VehicleDetail` in vcs.ini, read live:

| position | a car's full model to | big vehicles and boats switch at | where the numbers come from |
|---|---|---|---|
| PSP | 50 m | 50 m, 110 m | the game as shipped |
| HIGH (default) | 120 m | 120 m, 140 m | Rockstar's own high table, from LCS's Android port |
| ULTRA | 240 m | 240 m, 280 m | twice HIGH |

## Where it came from

[draw-distance.md](draw-distance.md), "The LOD multiplier was patched properly", scaled
`CCamera+0x7a8` and found it moved peds and vehicles and not the map, so the row went. This is the
vehicle half done on its own. The LCS fork (`Dev/gtalcspc`) found the function that turns the camera's
multipliers into the actual distances: LCS's Android port keeps its symbols, and its
`CVisibilityComponents::SetRslCamera` reads a per-device table (`dvLODSettingsHigh_VehicleDist0`
120, `VehicleDist1` 140, `VehicleFadeDist` 160, `BigVehicleDist0` 120, `BigVehicleDist1` 140) where
the PSP has `lui` immediates (50, 70, 90, 50, 110). VCS has the same function at `0x08946158`, the
same constants, `$gp`-relative globals and `CCamera+0x7a0` as the vehicle multiplier
(docs/VCS_ADDRESSES.md, "The vehicle LOD distances").

## How

A `HOOKENTER` on the `lw $ra` after the function's last store (`0x08946240`) multiplies the five
vehicle values by the square of (the row's metres / the PSP's) on their way out, so the game's own
multiplier still applies, nothing compounds (the function recomputes them before every pass), and
the peds, the parts culled by angle and the map are left as they were. Installed from `Tick` and
re-checked every tick through `Read_Instruction`; a savestate brings back the game's op and the
install puts the hook back (logged). PSP takes the hook out. The install first checks the last store
and the first `lui` (50.0), so another build leaves the game alone.

## Measured

On the boot autoload (a street in Vice City), the log's "vehicle detail HIGH - a car keeps its full
model to 120.0 m (the game's 50.0)", and lod0, lod1, big lod0 and big lod1 read 14400, 19600, 14400
and 19600 live, the peds' 4900 untouched. On LCS, where the same hook was A/B'd live, a pickup 59 m from the camera
drew its low-detail model (diamond wheels, a solid cab) at PSP and its full one at HIGH. Not A/B'd on
screen here: this fork's key pipe has no `set`, and the row can't be moved without the menu.

## Not done

- Peds keep 70 m (Rockstar's high table keeps 70 too).
- How far away cars exist is the population code's and unchanged, so at ULTRA the far end of the
  ladder may be past any car.
