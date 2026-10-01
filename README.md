GTA: Vice City Stories - PC version
===================================

**Grand Theft Auto: Vice City Stories**, the 2006 PSP game, made to play like a PC GTA. You get mouse
look and mouse aiming, WASD, controls that change between walking, driving and flying, an HD
texture pack, real-time shadows, shaded water and rain-soaked roads, and a GTA-style menu. The
game's tutorial messages name your keys instead of PSP buttons.

It runs on **Windows**, **macOS**, **Android** and **iOS**. Bring your own copy of the game: no
game files are included.

**[Download the latest release](https://github.com/vancevcs/VCSPC/releases)**

---

## What you need

- **Your own copy of the game**: the USA UMD of *Grand Theft Auto: Vice City Stories*, disc ID
  **`ULUS10160`**, dumped to an `.iso` or `.cso`. European and Japanese releases are not supported,
  because every memory address this build relies on is specific to the USA disc.
- One of the following:

| platform | download | needs |
|---|---|---|
| Windows | `GTA.Vice.City.Stories.PC.zip` | 64-bit Windows 10 or 11, a Vulkan-capable GPU, about 3 GB of free RAM |
| macOS | `GTA Vice City Stories Mac.zip` | Apple Silicon (M1 or newer), macOS 11 or later |
| Android | the `.apk` plus the game-files zip | an arm64 phone, Android 8.1 or later |
| iOS / iPadOS | the `.ipa` plus the game-files zip | iOS 15 or later, and a sideloading tool (AltStore, SideStore, Sideloadly or TrollStore) |

The iOS build is a **pre-release**. It has been built and packaged but not yet tested on an iPhone.

## Getting started

Every download has a `README.txt` with step-by-step instructions for that platform. In short:

**Windows / macOS**
1. Unzip the folder anywhere. Nothing gets installed.
2. Put your `.iso` or `.cso` in that folder, next to the game.
3. Run **GTA Vice City Stories**. It starts fullscreen and goes straight into the game.

On a Mac the first launch is blocked because the app isn't signed with a paid Apple certificate.
Open **System Settings → Privacy & Security** and click **Open Anyway**. You only need to do this
once.

**Android**: copy the `GTAVCS` folder to the root of internal storage, put your disc inside it,
install the APK, and allow file access when it asks.

**iOS**: sideload the `.ipa`, open it once, copy `memstick` and your disc into its folder in the
Files app, then open it again.

The game finds the disc on its own, and the first run remembers it. Keep the folder together:
your saves and settings live in the `memstick` folder beside the game, and nothing is written
anywhere else.

## Features

### Controls that feel like a PC game
- **Mouse look.** The camera follows the mouse directly, on foot, driving and flying. It pulls in
  around walls the way San Andreas on PC does, and swings back behind your car once you let go.
- **Mouse aiming.** Hold the right mouse button to aim, move the mouse to place the crosshair,
  and click to fire. Bullets land where the crosshair is, and scopes zoom with the mouse wheel.
- **Controls that depend on what you're doing.** Space is jump on foot, handbrake in a car, and
  block in a fistfight. W/S accelerate and brake in a car but climb and descend in a helicopter.
- **Walking.** Hold Alt (Option on a Mac) with WASD to walk instead of run. The PSP could only do
  this with a half-pushed stick.
- **Climbing.** Press jump in front of a wall or fence to pull yourself up onto it. The game plays
  its own climbing animation.
- **Gamepad support**, with a modern layout: triggers aim and fire, the right stick looks, and the
  bumpers glance while driving. Xbox and PlayStation pads both work.
- **Touch controls on phones.** The buttons on screen change between on foot, aiming, driving
  and flying. You can drag each one wherever suits your thumbs. Tap the radar to open the menu,
  and tap the weapon icon to switch weapons.

### Graphics
- **HD texture pack**, included and switched on.
- **Real-time sun shadows**, with four quality levels. People, cars, lamp posts, buildings and palm
  fronds cast shadows that follow the time of day and fade out at night and in the rain.
- **Shaded water.** The sea has moving waves, sun glitter, and reflections of the sky and shore.
- **Wet roads.** When it rains, the streets darken, reflect the city and fill with puddles that
  splash. They dry out gradually after the rain stops.
- **Widescreen on phones.** The picture fills wide screens without stretching.

### Menus and saves
- **A Vice City Stories-style menu** opens with Escape. It has settings for controls, audio,
  graphics and gameplay, and a card listing every binding for keyboard, Xbox and PlayStation side
  by side.
- **The game's own map, mission brief and stats** open straight from that menu.
- **Your own save list**: load, delete, or start a new game, with each save named after its
  last mission.
- **Auto-save after every story mission**, plus manual saves at safe houses as on the PSP.
  When you start the game it continues from your newest save.
- **Tutorial messages that name keyboard keys.** For example, "hold LEFT MOUSE" instead of
  "press the circle button".

## Controls

The menu's **Controls → Bindings** page has the full list for every device. These are the main
keyboard and mouse controls:

| | on foot | driving | flying |
|---|---|---|---|
| **W A S D** | move | W/S accelerate and brake, A/D steer | W/S climb and descend, A/D roll |
| **Mouse** | look | look | look |
| **Left mouse / Ctrl** | attack, fire | drive-by | fire (Hunter) |
| **Right mouse** | aim (hold) | | |
| **Space** | jump, or climb at a ledge | handbrake | |
| **Shift** | sprint | | |
| **F** | enter a vehicle | exit | exit |
| **Q / E** | previous / next weapon | glance left / right | yaw left / right |
| **R / T** | | radio station (forks on a forklift) | radio station |
| **H** | | horn | |
| **Arrow keys** | | | pitch and roll |
| **V** | change camera | change camera | change camera |
| **Esc** | menu | menu | menu |

These ones are less obvious:

| key | what it does |
|---|---|
| **Alt + WASD** | walk instead of run (Option on a Mac) |
| **G** | start or stop a sub-mission: vigilante, taxi, paramedic, empire sites. While aiming at a gang member, it recruits them |
| **Tab** | swap to a weapon lying at your feet |
| **L** | toggle the game's lock-on targeting instead of free aim. Fists use lock-on automatically |
| **S** while aiming | switch from lock-on to free aim |
| **Q / E** while aiming | cycle between targets |
| **Mouse wheel**, **Z / Y** | zoom a scope, binoculars or camera in and out |
| **Shift / Space / F** while fighting | heavy hit, block, grab or throw |
| **Backspace** | from the game's map or stats screen, return to the menu |

## Settings worth knowing

- **Graphics → Shadow quality** starts on the highest setting. If the game runs slowly, lower
  this first.
- **Graphics → Texture quality** switches between the HD pack and the PSP's original textures.
- **Graphics → Water quality**: LOW is the original water, MEDIUM shades the sea, and HIGH also
  adds wet roads in the rain.
- **Gameplay → HUD / Subtitles** are the game's own display settings. This build hides the
  game's original menu pages, so they are offered here instead.
- Settings are saved to `memstick/PSP/SYSTEM/vcs.ini`.

## Troubleshooting

| problem | fix |
|---|---|
| Nothing happens on Windows, or a DLL is reported missing | Install the Microsoft Visual C++ Redistributable (x64). |
| It takes a minute or more to start | The whole disc is loaded into memory first, so the city can stream in without stutter. This is slower from a slow drive, and needs about 3 GB of free RAM. |
| A file browser opens instead of the game | There is no disc image in the folder, or there is more than one. Pick the one you want and it will be remembered. |
| Textures look like the PSP's | Set **Settings → Graphics → Texture quality** to HIGH, and check that the `memstick` folder came along with the game. |
| The game starts mid-story when you expected a new game | It continues from your newest save. Use **Game → New Game** in the menu. |

## Building from source

```
git clone --recursive https://github.com/vancevcs/VCSPC.git
```

| platform | build | output |
|---|---|---|
| Windows | `msbuild Windows\PPSSPP.sln /t:PPSSPPWindows /p:Configuration=Release /p:Platform=x64 /m`, using Visual Studio 2022 with the C++ desktop workload | `GTA Vice City Stories.exe` in the repo root |
| macOS | `./b-macos.sh` | `build/` |
| Android | `./gradlew :android:assembleVcsRelease` | the `vcs` flavor APK |
| iOS | `./b-ios-vcs.sh` | an unsigned app in `build-ios/` |

`python Tools/vcspackage.py --zip` turns a build into a release folder and zip. Add `--android`
or `--ios` for the phone packages. It needs a populated `memstick/` folder, which you can copy
from a release zip. A Windows Debug build (`PPSSPPDebug64.exe`) also includes an in-game
debugger at **Debug → Tools → VCS**.

### Where things are

| path | what it is |
|---|---|
| `Core/VCS/` | the game layer: input, camera, aiming, climbing, saves, menu bridge, settings, memory addresses |
| `GPU/Common/VCSShadow.*`, `VCSWater.*` | shadows, the sea, and wet roads |
| `UI/VCSMenuScreen.*`, `UI/VCSTouchControls.*` | the menu and the touch controls |
| `assets/vcs/` | menu art, the menu font, sounds and touch icons |
| `Tools/vcs*.py` | packaging, the keyboard-text patcher, texture conversion, and reverse-engineering tools |
| `docs/vcs/`, `docs/VCS_ADDRESSES.md` | design notes for each feature, and how every game address was found |

## Licence and credits

This is free software under the GNU GPL, version 2 or later; see `LICENSE.TXT`. It is built on
the [PPSSPP](https://github.com/hrydgard/ppsspp) emulator. The menu typeface, Pricedown, is by
Ray Larabie.

No game data is included, apart from three short menu sound effects taken from the game
(`assets/vcs/sfx_fe_*.wav`). Grand Theft Auto and Vice City Stories are trademarks of Take-Two
Interactive Software. This project is not affiliated with or endorsed by Rockstar Games or
Take-Two.
