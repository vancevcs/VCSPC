# The front end: the pause menu, its pages, controls and widgets

> Read before touching `VCSMenuScreen`, `VCSFrontEnd`, the option pages, menu sounds, page art or focus handling.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

## The front end

**Status: working, confirmed against the running game.** Esc opens it over a paused VCS, the page
table navigates, hover and keyboard both move the selection, left/right edits a value and the
blocks track it, Esc walks back up the pages and then resumes, and `vcs.ini` is written on the way
out with all 20 of the options this fork owns - the four that belong to `g_Config` are marked
`external` and saved by PPSSPP instead.

It imitates VCS's own front end, not Vice City's PC one, and the difference is the whole design:

| | VC on PC (what this was built as first) | VCS (what it is now) |
|---|---|---|
| list | left column, left-aligned | centred on the screen |
| selection | slanted highlight quad behind the row | carried by colour alone |
| palette | pink on pink | cyan, selected row cream |
| headings | sans, top right | brush script, top left |
| values | number and a continuous bar | discrete blocks |
| backdrop | the dimmed game | opaque purple with palm fronds |
| bottom | nothing | a bar: what the row means, and how to work it |

Worth stating plainly because the first build got it wrong: "the GTA menu look" is not one thing.
Building it from Vice City's PC frontend produced something coherent that was recognisably the
wrong game.

### What was taken from reVC, and how little of it

reVC's `CMenuManager` is ~6600 lines, and most of that is a UI toolkit: hit-testing, hover state,
keyboard repeat, layout, text measurement. The game had no toolkit, so the menu had to be one.
PPSSPP already has that layer, themed and working on mouse, keyboard and pad, so re-implementing
it would have been several thousand lines spent to arrive back where we started.

Three things *were* worth taking:

- **The page table.** A page is a header, a parent page, and a list of rows; a row is either a
  jump to another page or a binding to one setting. Back-navigation is data (`ParentPage`), not
  code. reVC stores this as `aScreens[MENUPAGES]`.
- **CFO** - re3's "Custom Frontend Options", the reason `Core/VCS/VCSSettings.h` exists. A row is
  described by the variable it edits plus the ini key it saves to, so adding an option is one row
  in one table: no renderer change, no switch arm, no new save code.
- **The helper line.** The selected row's explanation along the bottom, which is what lets an
  option be called "Free aim" instead of a sentence.

Its *palettes* are worth knowing about too, and they are not what you would guess. reVC ships two
whole front ends - `Frontend.cpp` for PC and `Frontend_PS2.cpp` behind `PS2_MENU` - and neither is
this one: PC is pink (`LABEL_COLOR(255,150,225)`), PS2 is gold (`SELECTED_TEXT_COLOR(255,182,48)`).
The cyan-and-cream here came from VCS itself.

### The settings did not persist before this

Every VCS tunable lived in the ImGui debugger, bound to `VCSCameraSettings` and
`VCSFireHookSettings`, and reset to defaults on every launch. `VCSSettings` is the fix, and it
deliberately does not move the values - those structs go on owning them, documented where they are
used, with the debugger still binding to them directly. What the table adds is names, ranges and
persistence for the subset a *player* would set. A knob that needs a paragraph of measurements to
interpret stays in the debugger window.

A row is `Bool`, `Float`, `Int` or `Choice`, and the split between the last two is the one worth
stating: **`Choice` stores an index into a label list, `Int` stores the value itself.** Master
volume was written as a `Choice` over `"0"`..`"100"` in tens, which reads correctly and is wrong -
`g_Config.iGameVolume` runs 0..100, so the index and the value are different numbers, and the
first left-press took the volume from 100 to 10. `Int` carries `minInt`/`maxInt`/`stepInt`,
renders as the same ten blocks a `Float` does, and one press moves one block. Anything owned by
the rest of the emulator is a value, not an index; reach for `Choice` only when the thing really
is a list of names.

Two more details that will bite if they are "simplified":

- `Options()` captures each default from the live variable the first time it is called, so it
  **must** run before the ini is read. `LoadSettings()` guarantees that by calling it first thing.
  Reverse the order and "restore defaults" quietly means "restore whatever was last saved".
- Settings live in `memstick/PSP/SYSTEM/vcs.ini`, next to `imdebugger.ini`, **not** in
  `ppsspp.ini`. Putting them in `Core/Config.cpp` would mean editing a file upstream touches
  constantly, which is the difference between a clean rebase and a conflict on every pull.
- An `external` row's default belongs to PPSSPP, so ask it: `Config::GetDefaultValueInt(&g_Config.x)`.
  Restating the number here means "restore defaults" quietly disagrees with the value the rest of
  the emulator would restore - anisotropy was written as `0` against an upstream default of `4`.
  It is safe to call for anything in `struct Config`; only the blocks that override
  `CanResetToDefault()` (display layout, touch controls, gestures) assert.

### The Graphics page has an ADVANCED page under it

Ambient occlusion, anisotropic filtering and world memory moved to `OptionPage::GraphicsAdvanced`
when AO joined and the page stopped reading at a glance. ADVANCED is the last settings row, above
the spacer and RESTORE DEFAULTS, because it leads to more settings rather than doing something -
and each page's RESTORE DEFAULTS resets only its own rows. Its heading is
`assets/vcs/title_advanced.png`, rendered on the Mac with `.venv/bin/python Tools/vcsmenuart.py
advanced` (the system python has no Pillow).

Since the frame limiter arrived the two pages split the way the PC ports split DISPLAY SETUP from
their advanced options. DISPLAY SETUP is the picture: Brightness, Resolution, Fullscreen,
Widescreen, Frame limiter, Show FPS. ADVANCED is quality and its cost: Texture quality, Shadow quality,
Ambient occlusion, Water quality, Anti-aliasing (OFF / SMAA, on by default on a desktop, off on a
phone - see [anti-aliasing.md](anti-aliasing.md)), Anisotropic filtering, World memory.
Desktop pages are centred and don't scroll, so about eleven rows is the most a page can hold on a
16:10 screen before BACK goes under the help bar.

### The Gameplay page has a second kind of row now: the game's own settings

`SUBTITLES` and `HUD` are not this port's inventions, and that makes them the first rows on that
page that are not. They are the PSP game's own, off the DISPLAY page of its front end - the page
this fork's bridge deliberately does not offer, because there are pages of our own for controls,
audio and display. That left three of the game's four display settings unreachable rather than
superseded, which is the same loss the map and the save list took when Start was claimed, and the
same answer applies: give it back.

So the page's rule is now "settings about the GAME, as opposed to about the device you drive it
with", and both kinds qualify - a switch that hands one of our own additions back, and a switch
that reaches one of the game's that nothing else can.

**Reaching for the game's existing setting is the whole point, not a shortcut.** Before this there
were two mechanisms sketched for hiding the HUD, both ours, and both would have been wrong: the
game has the setting, it persists it, and its own menu goes on agreeing with ours because there is
only one value. Same doctrine as the vault calling `CPed::CanClimb` and the front-end bridge
pressing Start.

**They are mirrored, not edited in place.** The menu runs on the UI thread and PSP memory belongs to
the emu thread, so `VCSGameSettings` holds a bool for each and `ApplyDisplayPrefs` in `VCSGame::Tick`
pushes them across - **only when the game disagrees**, which makes it self-healing across a load
without being the re-assert-every-frame mistake that the camera pitch runaway taught.

**HUD MODE is the panel, not the radar**, and the row's help line has to say so. See "The game's own
SUBTITLES and HUD MODE" in docs/VCS_ADDRESSES.md for how both were found, and for the two candidates
that tracked the setting perfectly and still decided nothing.

### The Audio page's other two rows, and the value that decides

`SFX VOLUME` and `RADIO VOLUME` are the game's own, reached the same way and for the same reason as
subtitles and the HUD. What makes them worth their own note is that **each setting is two values**,
and the obvious one is the wrong one.

`DisplayPrefs + 0x1c` and `+0x20` are the preferences: they are what the game's own AUDIO page shows
and what a save keeps, they step 16 at a time and top out at 127, and **writing them changes nothing
you can hear**. The levels the mixer actually reads are two plain bytes at `0x08bb3b74` and
`0x08bb3b75`, nothing recomputes them from the preferences, and the game's own slider moves both
together. So the fork's rows write both, and a row that wrote either one alone would be a volume
that forgets itself across a boot, or a slider that moves and does nothing.

**The lesson generalises past audio, and it cost time twice.** A value that tracks a setting
perfectly is not necessarily the value that decides anything - HUD MODE has two gp-relative shadows
that follow it exactly and revert within a frame when written. The test is one line of work: write
it, and look at whether the thing you wanted actually changed. Three of the five game settings found
this way failed that check on the first candidate.

**The step is 13, not the game's 16.** The strip draws ten blocks and this menu's rule is that one
press moves one block, so the step is a tenth of the range rounded up - ten presses reach the top,
where the write path clamps to 127 exactly as the game's own slider does at its last notch. The
game's ladder of 16 is eight notches against a ten-block strip, which would move one block on some
presses and two on others. Both produce values the game accepts; this one matches the control the
player is looking at.

### BRIGHTNESS, the game's own, and a strip of eight blocks

The first row of DISPLAY SETUP, where the PC ports put it, and the third of the game's own Display
page settings to get a row. Reached the way SUBTITLES and HUD MODE are - a mirror in
`VCSGameSettings`, pushed by `ApplyGamePrefs` only on a disagreement - and simpler than the
volumes: one value, no live copy. See "BRIGHTNESS" in docs/VCS_ADDRESSES.md.

**Eight blocks, not ten, and `Option::blocks` is how a row says so.** The game's slider has eight
notches of 32, and this menu's rule is one press moves one block; eight steps on a ten-block strip
would light one block on some presses and two on others - the same trap the volume step of 13
avoids from the other side. Every other row leaves `blocks` at 0 and draws ten.

### LANGUAGE, and the USA build that never chooses one

**Status: working, measured.** Booted with FRANCAIS selected, the beeper, the help box and the
game's own front end are French, accents included, and the boot's auto-load still walks that front
end to the save list - the French layout's tab grid differs, and the walk does not care, because it
watches the page index rather than counting presses.

The USA disc carries all five GXTs the PAL release has, and the text loader at `0x089f656c` still
switches on a language index to pick one - so the languages were always there, and the only thing
missing is anything that sets the index. See "The game's language" in `VCSAddresses.h`: the index
sits behind a lazy-init flag whose initialiser zeroes it, nothing else writes it, and the game's one
`sceUtilityGetSystemParamInt(LANGUAGE)` call feeds the save dialog only. **Setting PPSSPP's system
language to French was tried first and changed nothing**, which is what sent this to the code.

**Data, not code.** `PatchLanguage` writes the init flag and the index before the module starts, so
every reader finds the struct already initialised and takes our value. Nothing is patched, and
English - the game's own answer - is left entirely alone. The row is on GAMEPLAY and says it takes
effect next time the game starts, as RADAR CORNER does: the GXT and the front end's per-language
layouts (`FR_MASTER.PSP` and the rest) are loaded once, at boot.

**What it does not do yet: name keyboard keys in the other four.** `ENGLISH.GXT` is the only one
`Tools/vcsgxtkeys.py` rewrites, so a French tutorial line still says "appuyez sur X". The tokens are
the same in every GXT; what is missing is a key-name vocabulary and the article rules per language,
and four rows in `kVCSDiscPatches`.

### `VCS_MENU_TEST`, for looking at a page with nothing to type with

`VCS_MENU_TEST=<page>[:<row>]` raises the pause menu once the world has settled, on the page whose
`PageTitle` key is `<page>` (`displaysetup`, `keybindings`), with row `<row>` focused. It exists
because the Mac this fork is measured on has no way to send a key into the window without
accessibility permission, and a page is otherwise unreachable from the WebSocket debugger - which
presses PSP buttons, below the layer that opens this menu.

### CUSTOM SOUNDTRACKS, the PSP's own, reached by calling its setter

**Status: working against the running game, with a disc AT3 standing in for a ripped track.** The
row is on AUDIO SETUP; ON with a `.gta` in `PSP/SAVEDATA/ULUS10160CUSTOMTRACKS/` puts the game in
exactly the state its own Audio page does (preference 1, station 9, playing bit set), OFF puts it
back, and an empty folder reads NONE FOUND. The fork makes the folder at boot, because nothing else
ever will - the PSP had Rockstar's PC tool do it. See "CUSTOM SOUNDTRACKS" in docs/VCS_ADDRESSES.md.

**A call, not a write**, and it is the first menu row that needs one: the setter retunes the audio
manager, and a field written behind its back would be a preference the radio never heard about.
`EnqueueGameCall` takes two arguments now for it.

**Two faults in the call path, both found by this row.** The thread the game runs its main loop on
was only recorded once the vault's query block existed, so with vaulting off no call could ever be
made; and the ledge probe asks every frame while the player moves, so "the probe wins a tie" was a
queued call that never went at all. The thread is recorded regardless now, and a waiting call goes
after it has given way twice.

**What is not done: making a `.gta`.** It is ATRAC3 or ATRAC3plus in a RIFF, and nothing on this Mac
can encode either - ffmpeg only decodes ATRAC.

### Discord and GitHub, two icons in a corner

The root page carries the project's links as two icons in the bottom-left corner, above the help
bar, which is empty on that page because its rows have no help line. `VCSMenuLink` opens the
browser through `System_LaunchUrl`. They're deliberately not rows, since they're not what the menu
is for, and not focusable, like the phone's back icon, so the arrow keys never wander into them.

The art is `assets/vcs/link_discord.png` and `link_github.png`: the logos the project supplied,
cropped square to 128 px, white with coverage in alpha only. The menu tints them, so they take the
rows' cyan and light cream on hover. They're drawn with the same 2 px drop shadow as the rows' text.
An empty URL hides its icon (`kVCSDiscordUrl`, `kVCSGitHubUrl` in `VCSMenuScreen.cpp`).

### The menu's own sounds, taken from the game rather than invented

The front end was silent, and the three sounds it wanted were already on the disc. A
`SET*/SFX*_PSP.RAW` file is a plain concatenation of Sony VAG streams, each with a 0x30 header
carrying its rate and - the part that turned this from a hunt into a listing - **its name**:

    SFX_FE_HIGHLIGHT   44 ms   the blip as the selection moves
    SFX_FE_SELECT      90 ms   a row being chosen
    SFX_FE_BACK        49 ms   going back a page

`Tools/vcsmenusfx.py` decodes those three to WAV in `assets/vcs/`, beside the page titles and for
the same reason. The same bank also holds an error blip per direction and three noise bursts, if
the menu ever wants them.

**Three new `UISound` values, not three swapped samples.** `SoundEffectMixer::UpdateSample` would
have let the fork replace `SELECT`/`CONFIRM`/`BACK` with no upstream diff at all, and it was the
wrong trade: those samples are global, so a Debug session that booted another game after VCS would
have GTA blips in PPSSPP's own menus. Adding to the enum is six lines and is additive - nothing
outside `VCSMenuScreen.cpp` plays them.

**And they do not go through `UI::PlayUISound`.** That path is gated on `g_Config.bUISound`, which
ships *off* and lives on PPSSPP's own Audio page - a screen the game build has no way into. A menu
that stays silent until you find a setting you cannot reach is a silent menu, so this calls the
mixer directly.

**Which means the volume has to be computed at the call, because nothing downstream does it.** A UI
sound is mixed on top of everything - `NativeMix` adds the effect mixer after the game's audio - so
it misses `iGameVolume`, which is applied inside `__sceAudio` to the emulated game only, and it
misses the SFX level, which is a value *inside* the game applied by the game's own mixer. These
first shipped answering to neither, on a page that has a row called Master volume and a row called
SFX volume, and it read as a bug the moment anyone moved one. Both are applied now, and so is
`bEnableSound` - that gates `__sceAudio` rather than the audio device, so the effect mixer goes on
running with the emulator "muted" unless it is asked.

Following the SFX row rather than only the master is the honest half: these ARE the game's
front-end sound effects, and in the retail game they play on the channel that row governs.

**`FocusChanged` is the seam for the highlight**, and it is the only one that catches both halves:
hover sets the focused view from `Touch`, keyboard and pad navigation go through PPSSPP's own
`MoveFocus`, and both end up there. The `CAUSE_*` flags then do the discrimination that a timer
would otherwise have to - `CAUSE_FOCUS_MOVE` and `CAUSE_OTHER` are somebody moving the selection,
while `CAUSE_SCREEN_CHANGE`, `CAUSE_RESTORE` and `CAUSE_FORCED` are a page building itself or a
click forcing focus onto the row it is about to activate. Blipping on those would mean a sound for
merely opening a page, and two sounds for every click.

### The bridge's own button presses are not the player's, so they are silenced

Picking MAP, BRIEF or STATS makes this port press Start and then tab across the game's front end,
behind a curtain that is there precisely so the walk is not watched. The game blips at every one of
those presses - button sounds for buttons nobody touched.

`ApplyGamePrefs` writes the live SFX level as 0 whenever `FrontEndDriving()` is true, and this is
where the two-layer volume finding pays for itself twice over: **the preference is left alone**, so
nothing about this reaches the game's Audio page or a save, and the level comes back on its own the
tick after the walk ends, because `ApplyPref` is a disagreement check rather than a one-shot.

Muting the CHANNEL rather than the emulator is the point - the radio plays through the whole thing,
which is what the player is actually listening to.

### Walking, which a keyboard could not do until it had a modifier

VCS reads walk out of how far the nub is pushed, so a pad has always had it and a keyboard never
could: a key is fully down or not at all, which is always a run. Alt with WASD is the middle of
the range handed back.

**The deflection is measured, not chosen.** Driven from the debugger in steps with the player's
velocity read back at each one, it came out in flat bands rather than as a curve:

| deflection | speed/frame | |
|---|---|---|
| 0.15 - 0.25 | 0.000 | inside the game's own dead zone |
| 0.30 - 0.60 | 0.026 | identical across the whole band - the walk |
| 0.70 | 0.037 | the transition |
| 0.85 - 1.00 | 0.070 - 0.093 | the run, which is what every key press produces |

`kVCSWalkDeflection` is 0.45, the middle of the walking band, so nothing depends on the
measurement being exact. **Applied after the diagonal normalisation**, or W+D would scale to 0.64
- out of the band and into the jog, so walking diagonally would quietly not be walking. And only
for a keyboard: a stick already says how fast to go, and scaling one that is already half pushed
would take it under the dead zone and stop the player dead with the stick still deflected.

**Both Alts are claimed**, and the right one is not politeness: PPSSPP's default keyboard map puts
`CTRL_CIRCLE` on it, so a player reaching for the Alt nearer their right hand would have jumped
instead of walking. The inverse Escape trap, one more time.

### Alt + a key used to stick, and it was never Alt's fault

Reported as "Alt with another key makes it act weird and non-functional until I press Alt + that
key again", which is a precise description of a lost key-up. `Windows/RawInput.cpp` accepted
`WM_SYSKEYDOWN` as a press and handled only `WM_KEYUP` as a release - but **Windows sends the SYS
variant of BOTH messages for any key pressed while Alt is held**, so the release fell off the end
of the `else if` and the key stayed down: in `keyboardKeysDown`, in this fork's `g_heldKeys`, and
in the game. Pressing the key again without Alt is what cleared it, which is exactly the symptom.

One missing condition, and **it was never specific to this binding** - every Alt combination in
the emulator has had it, for every game. Worth remembering as a shape: a control that "sticks
until you repeat it" is a release that was never delivered, not a press that was mishandled.

### The tutorial messages name keyboard keys now, and it took no code at all

The help lines in the corner - "To sprint, hold ~k~ ~PDSPR~ while running" - do not contain a
button name. They contain a TOKEN, and the game resolves it through a second GXT entry: `~PDSPR~`
looks up `C0PDSPR`, which reads `~X~`. So the entire control vocabulary of every tutorial message
in the game is 58 strings in one table, `C0` through `C3` for the four control configurations, and
pointing them at this fork's bindings is a data change with nothing hooked.

The surface is **51 tokens across 140 occurrences**, and `Tools/vcsgxtkeys.py` rewrites them from a
hand-written mapping that mirrors `kVCSKeyMappings`. Two things in it are worth keeping:

- **The article has to go, by rule rather than by list.** Every PSP name is a noun phrase that
  wants one - "the up button", "the analog stick" - so the lines were written as "use the ~TOKEN~",
  and almost no keyboard name is: the result is "use the the mouse" and "press the LEFT MOUSE".
  Seven lines needed it, in five tables including two only a debug build shows, which is the whole
  argument against fixing them by hand.
- **Two lines needed rewriting outright**, because two PSP controls collapse into one on a mouse
  and the sentence says "and": "Hold ~PDLO1~ and use ~PDLO2~ to look around" has no second half
  left once both are the mouse.

**The GXT writer earned its `--verify`.** Rebuilding the file unchanged and comparing byte for byte
caught two wrong guesses that would otherwise have shipped: strings are NOT pooled by value (2408
entries, 2408 distinct offsets - pooling produced a file 8272 bytes short), and **TKEY is ordered by
key while TDAT is not**, so the pool has to be packed in the original offset order and the key table
written back in its own. Neither would have looked wrong in a hex editor.

### VCS reads its disc by SECTOR, so a filename redirect can never work

This is the finding worth carrying past the GXT, and it cost a full build-and-boot to get.

The delivery route chosen for the patched file was a redirect in the emulator: hook
`MetaFileSystem::OpenFile`, serve the file from the memory stick, leave the ISO alone. It was
built, it compiled, and **it never fired once.** Logging every single file open for a whole boot
showed why - the complete list of what VCS opens is savedata `PARAM.SFO`s, `EBOOT.BIN`, and:

```
disc0:/sce_lbn0x0_size0x65170000
```

The whole UMD, as one stream, seeked by the game to its own files by sector. **No filename is ever
requested for anything on the disc**, so there was never a name for a redirect to match. The
redirect is reverted; nothing of it remains.

So anything replacing a disc file in this game goes through the bytes - and once that is accepted,
the emulator can do it at READ TIME, so the disc never has to be touched at all.

`VCS::PatchDiscRead` is that: `ISOFileSystem::ReadFile` hands it the absolute position it has just
filled, and it overwrites any part of the buffer falling inside a patched range from a file on the
memory stick. Two call sites in one function, because that function is where both read paths meet -
the offset one, and the whole-sector one the `sce_lbn` handle uses. First line returns for every
other game.

**Not a `BlockDevice` wrapper, though that was the first design and reads better on paper.**
`ConstructBlockDevice` is shared by every game, and `Load_PSP_ISO` does a
`dynamic_cast<NPDRMDemoBlockDevice *>` on the result - a decorator would have silently broken demo
ISOs for a feature that has nothing to do with them. The read funnel one layer up costs nothing and
is already free of any of this.

The offset is a property of this disc image, found by searching it for the file's own bytes, so it
gets the treatment a measured address gets: **the disc is checked for the header that should be at
that offset before anything is overwritten**, and a mismatch disables that patch for the boot.
`ENGLISH.GXT` lives at `0x4330000`, sector 34400.

Verified both ways, which is the part that matters. Booting the **retail, untouched** ISO with the
replacement present puts `LEFT MOUSE` in RAM and no `the up button`; moving that one file aside and
booting the same ISO again brings `the up button` back. `Tools/vcsgxtkeys.py --iso` still bakes a
standalone image for anyone who wants one, and `VCS-keyboard-help.iso` is what it produced.

**This also corrects the answer given for the logo video.** A file redirect was recommended as the
neat way to swap `LOGO.PMF`; it would have failed the same way, silently. The disc patch is the
answer there too - one more row in `kVCSDiscPatches` - subject to the same size limit, because the
game seeks by sector numbers baked into its own code.

**And one trap in the diagnosis itself**: the first probe used `WARN_LOG` and produced nothing,
which read as proof. `IOLevel = 2` in `ppsspp.ini` is ERROR-only, so warnings never reached the
file at all. A log line that does not appear is not evidence until the channel is known to carry
it.

### The controls card is two tables in four columns

`CONTROLS - BINDINGS` is a read-only reference card: `ACTION`, then what the action is on the
keyboard, on an Xbox pad and on a PlayStation one, all on screen at once. Each column is generated
from the table that actually drives that device - `kVCSKeyMappings` for the keyboard,
`kVCSPadMappings` for both pads - so no column can drift from what pressing the thing does, which
is the only property here worth protecting.

**Two tables, three columns, and the third is not a third table.** A DualSense and an Xbox pad have
the same controls in the same places and differ in what is printed on the plastic, so the
PlayStation column is `PadButtonName` called with the other vocabulary. A second pad table would be
a second copy of the same scheme waiting to disagree with the first. `CREATE / SHARE` names one
button across two generations for the same reason - picking one would be wrong for half the pads
it describes.

**It was a switch, and losing the switch is the point.** A row on the bindings page chose which
device the card described, and left/right flipped it from inside a card. The switch answered "what
would I press on the device I am not holding", one device at a time, and it could never show the
thing four columns show for free: which actions exist on one device and not on another. WASD
against two blank pad cells, or the lock-on toggle against three, is a sentence the old card had no
way to write. The heading stopped following the switch too - it is `title_bindings.png` now, and
`title_keyboard.png` is gone.

It was **one** table read twice before that, and why that stopped working is worth recording. When
a pad still went through the PSP's own layout, every keyboard row already knew both halves - the
key it binds and the PSP button that key produces - so the pad's card was the same rows with the
other half shown, and it could not drift by construction. The moment the pad got a scheme of its
own that stopped being true: the two devices now agree about the face buttons and about almost
nothing else.

Three rules hold it together, and all three are the kind of thing a tidy-up undoes:

- **A blank cell is information; a blank row is not.** A row is dropped only when it has nothing on
  *any* device. One empty cell says "not on this device", and it can only say that standing next to
  a column where the action exists - which is exactly what the two-column card could not do, and
  why its rule was the stricter "not on this device, not on this card".
- **`kVCSListingExtras` carries one cell per column.** It is the handful of rows no mapping table
  can hold - the sticks, mouse look, the glances, the forks - and they are the ones that can drift,
  in three directions now. It is also how one row says `MOUSE` in one column and `RIGHT STICK` in
  the next two without being written down twice.
- **Extras merge by name, they do not append.** They go through the same `addRow` the mapping
  tables use, so an extras row can fill in *one device's cell* on a row a mapping table owns.
  `MENU` is the case that needs it: Escape opens this menu and can never be in `kVCSKeyMappings` -
  claiming it would withhold it from PPSSPP's mapper, which is where that pause comes from - while
  the pad's Start row is a real binding. One row, two sources, and neither could have written the
  other's half.

**Rows shrink to fit.** `ListRowHeight` divides the space between the panel and the hint bar by the
row count, clamped between 28 and 40dp. Nothing on this page scrolls and there is no second page,
so a card taller than the window is a card with bindings nobody can read. On Foot is the tallest
and the one a player reads first.

### The keyboard column is editable: actions, and one lookup

**Status: working, tried in the running game.** Click a line of a card - or Enter on it - and the
keyboard cell asks for a key; MOVE and STEER ask for theirs one direction at a time. Escape cancels,
RESET KEYS on the KEY BINDINGS page puts everything back, and `vcs.ini` keeps only what moved, under
`[KeyBindings]`, by action id.

**An action is a line of the card on one page**, and it owns the (context, shipped key) pairs that
line is made of - `kVCSKeyActions` in `VCSInput.cpp`. Several own pairs the card never shows, and
those are the ones that matter: AIM also holds aim while aiming, MOVE BACK also enters free aim, the
move keys also strafe under lock-on. Per page, as San Andreas does it: CHANGE CAMERA on foot and in a
car are two bindings, which is what lets one key go on meaning different things in different places.

**Everything still names the shipped key, and asks `BoundKey(context, key)` first.** The mapping
table, `ApplyAnalog`, the glances, `JumpHeld` and the rest were not rewritten into a second table;
the shipped key is the action's name and the binding is what the player presses for it. A rebind
changes one atomic per action, read from the input thread and the emu thread alike.

Three rules that a tidy-up would undo:

- **A key the player moved an action off stays claimed.** Space is the PSP's Start in PPSSPP's own
  defaults; release it and the old jump key opens a menu - the inverse Escape trap, one more time.
- **A row no action owns stands down for a key an action has taken** (`RowKey`). The debug spawner's
  arrow rows would otherwise press L and cycle a weapon every time a player who moved to the arrows
  walked left.
- **A clash swaps, it does not unbind.** Binding JUMP to F hands ENTER VEHICLE the old Space, in each
  context the two share. An action with no key is a thing the game can no longer do, and the player
  asked for a key, not for that.

Escape cannot be bound - it is the pause key and the capture's cancel. The pad is not rebindable
here: its scheme is a layout, and PPSSPP's own mapper is underneath it for anyone who wants that.
The tutorial lines still name the SHIPPED keys, because `Tools/vcsgxtkeys.py` bakes them into the GXT.

### `VCS_KEY_PIPE`, a keyboard for the shell

`VCS_KEY_PIPE=<file>` makes every frame read that file, delete it, and feed what it says through
`NativeKey`: `press F`, `down Space`, `up Space`, `wait 30`, `press MOUSE1`. Built for the same reason
as `VCS_MENU_TEST` - nothing on the Mac this is measured on can send keys to the window - and it goes
through the real door, so a rebind tested this way is tested end to end.

### The cheat menu types the combination, and that is the whole design

**Status: built, builds clean, NOT yet verified against the running game.** The 36 combinations come
from published cheat lists, not from the game's own table - see below for what that means and how to
fix a row that turns out wrong.

VCS has no cheat entry screen: every cheat is an eight-press pad combination entered during ordinary
play. `CHEATS` on the pause menu's root page leads to five group pages (Player, Vehicles,
Pedestrians, World, Multiplayer), and clicking a row queues it and closes the menu.

**Closing the menu is part of activating the cheat, not a courtesy.** This screen pauses the
emulator, so nothing can be typed while it is up; the row queues an index and sends `DR_CANCEL`, and
`CheatTick` starts pressing on the first tick after the game resumes. Anything else built on this
menu that has to reach the running game - the map bridge, save loading - wants the same shape.

**Why not call the game's handlers.** 36 addresses we have not hunted, against sequences we already
have; and a cheat is not only its effect - entering one flags the save, prints the game's own
confirmation, and toggles flags other systems read. Typing the combination gets all of that by
construction. A wrong sequence costs one row; a wrong address costs a crash.

**It steps on `FrameCounter`, not on vblanks**, for the reason the camera handback does. The game
samples the pad once per logic frame at 30Hz while `VCS::Tick` runs at 60, and neither rate holds
during streaming - so a press held for a fixed number of *vblanks* is a press held for an
unpredictable number of *samples*, and one missed sample in the middle of an eight-press combination
fails it silently. Two game frames held, two released; the gap is what keeps `Circle, Circle` from
reading as one long press. A whole combination takes about a second, and there is no deadline to
beat: GTA's cheat matchers keep a rolling history of presses rather than a timed window.

**`VCSCheats` presses nothing.** It decides what the mask should be; `ApplyMapping` applies it,
through the same `held & ~wanted` release discipline everything else there uses. That keeps one
function as the only thing in this fork touching sceCtrl, and it is why the clear mask is not simply
"everything": on the first frame the player may still be holding the key that produces the
combination's first press.

**The player's controls stand down for the duration**, buttons and stick both, exactly as they do
during a vault. A held W is a Cross, and a Cross between two presses is a ninth press.

**A row that does nothing is a transcription error, not a broken feature.** The OSD says
`Cheat: FULL HEALTH` when the sequence starts and the game prints its own confirmation when it
lands, so our message with nothing following it is the symptom, and the fix is one line in
`kCheats`. The authoritative version is in the EBOOT - the matcher lives near whatever writes the
flag `02A4 are_any_car_cheats_activated` reads - and mining it would replace the table with measured
data. Worth doing; it was not worth blocking a working menu on.

### The map and the save list, reached through the game's own front end

**Status: working, measured live against the running game.** `MAP`, `BRIEF`, `STATS` and `GAME`
sit on the pause root, and the game's own menu chrome is stripped off all of them. There is
deliberately no save row - VCS saves by walking into the save icon at a safe house, so a menu row
would be a second way to do something the world already has a place for. Controls, audio and
display are left off for the same kind of reason: this fork has its own pages for all three, and a
second way in that edited the game's copies would be two settings screens quietly disagreeing.

`GAME` is one row rather than three because `NEW GAME`, `LOAD GAME` and `DELETE SAVE DATA` are
entries *on* that page - the game says so itself, see the widget names below.

VCS keeps its map, stats, briefs and save list on the pause menu Start opens, and this fork took
Start for its own menu - so all of it became unreachable, which is a straight loss against retail.
`MAP`, `LOAD GAME` and `SAVE GAME` on the pause root give it back. They queue and close, the same
shape a cheat row uses and for the same reason: this screen pauses the emulator.

`FrontEndMenuManager` is at `0x08bc9100`, found through `0261 has_save_game_finished` rather than
by scanning - see "Reaching the game's own front end" in [docs/VCS_ADDRESSES.md](../VCS_ADDRESSES.md).
Three fields are in the table: `MenuActive` (+0x20), `MenuPage` (+0x1c, a SIGNED index into an
eleven-entry page vector, -1 when closed), and `SaveMenuRequest` (`gp+0x1076`).

**It presses Start; it does not set `MenuActive`.** Opening that menu is not one boolean - the game
builds page objects, stops the world, takes the pad - and a flag set from outside claims all of it
happened. The address table's job here is to *watch*, which is what turns the walk from a timed
guess into a closed loop. Same doctrine as the cheat menu and the vault: drive the mechanic.

**And it tabs; it does not write `MenuPage` either**, one level down on the same argument. A page
almost certainly does work on entry - the map builds a texture, the save list enumerates the memory
stick - so the walk presses the game's own navigation and watches the index until it arrives.

**The tabs are a GRID, and finding that out is what made the walk work.** Measured live over the
WebSocket debugger, pressing a button and reading `MenuPage`:

| | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| top row | map | brief | **game** | stats | controls |
| bottom row | audio (5) | display (6) | multiplayer (7) | | |

D-pad **right cycles within one row and wraps inside it** - `0,1,2,3,4,0` and `5,6,7,5` - while up
and down switch rows. So a walk built on "press next enough times", which is what a ring would
want, spins forever from the wrong row exactly half the time. The walk instead remembers which
pages it has seen since the last row change and presses **down** when it lands on one twice. That
is blind to how many rows there are and to which page is where; the target index is the only thing
it knows, which is the only thing it was told.

**LOAD GAME is not a page of its own** - it is an entry on the `game` tab, so the load target is
2. And the menu opens on 0, which is why the map row usually arrives having pressed nothing.

**R trigger was the first guess and it moves nothing here**, which cost one round. Every other GTA
pause menu tabs with the shoulders, and the game says otherwise on screen: the legend in the corner
draws `move` against the four d-pad glyphs. The prior beat the evidence that was already rendered.

**A request has to be accepted while the menu is already up**, and that is not a refinement. Opening
the map leaves the bridge handed over with the menu still open, so asking for LOAD GAME next arrives
in `Done` rather than `Idle` - and the first build only took requests in `Idle`, so the second row
did nothing when pressed and then something surprising later. Reported as "load game does nothing".

**The failure mode was chosen before the feature was.** With a page number wrong the walk runs out
of presses and stops, leaving the player in the game's menu a tab or two from where they asked -
and the debugger's Front End tab separates the two causes: a page that never moves means the
navigation control is wrong, a page that moves but never matches means the number is.

### The front end is a named widget tree, and that is how the chrome comes off

**The retail build kept the debug names.** Every widget's first field points at its own ASCII name,
so the front end prints itself:

```
MASTER      Background  Map_t  Brief_t  Game_t  Stats_t  Controls_t  Audio_t  Display_t  Multiplayer_t
MAP_PAGE    Map_AE  MapTitle
GAME_PAGE   LoadGame_MI  NewGame_MI  DeleteGame_MI  GameTitle  Reset_MI
BUTTONS0-3  Move  Select  back  placemarker  down_but  up_but  x_but  circ_but  square_but ...
```

Three things are hidden while the menu is up, and **all three are data writes** - `+0x20` on a
widget is a `visible` byte that the page draw checks and skips on, so there is no code to patch,
no JIT block marker to work around, and undoing it is writing the old value back:

| what | how |
|---|---|
| the tab strip | the root page's `*_t` widgets, `visible = 0` |
| the button-hint row | every child of every `BUTTONS*` group, `visible = 0` |
| the black band under both | every backdrop's height stretched to 400 |

**That last row is the one that was diagnosed wrong twice, and the correction is the useful part.**
With the strip and the hints gone the bottom of the screen stayed black. The first explanation was
"something is painting over it" - disproved by shrinking the backdrop to 240x120 and watching it
obediently shrink into the corner, so the rect really does drive the draw. The second was "the map
is laid out 480x224 to leave room for the strip, so those 48 rows are a hole" - which fixed the map
page and did nothing for Brief, Stats or Game, because they have no full-screen widget to grow.

**The real answer: the front end lays itself out in 480x272 - the PSP's framebuffer - and what is
actually displayed here is about 330 rows.** Measured two ways off one widget: a backdrop at h=272
covered 82% of the frame (implying 332) and at h=120 covered 37% (implying 324). So the band was
never a gap the strip left behind, and nothing the game draws was ever going to reach it. `MASTER`'s
`Background` at the game's own idea of full-screen stops 60 rows short of the screen it is on.

`backdropHeight` is therefore **400 - deliberately too big rather than measured**. Drawing off the
bottom costs nothing, an exact value would have to be derived from display settings the player can
change, and a backdrop that stops one row short is the entire bug returning.

**Grown by name, and that list is short on purpose**: `Background`, `Map_AE`, `WRAPPER`. A rule
like "any full-width widget" would also match `Reset_MI`, which is a 480-wide *menu row* on the
Game page - stretching that would turn one entry into the whole page.

### Left and right stop switching tabs

`lockMenuTabs` drops `CTRL_LEFT`/`CTRL_RIGHT` in the `Menu` context. It follows from hiding the
strip rather than being a separate opinion: with the tabs drawn, tabbing is navigation the player
can see; with them hidden it moves you to an unmarked page for no visible reason, and this fork's
menu is what picks the page now.

Claimed and dropped rather than unbound, which is the inverse-Escape-trap discipline applied at
runtime - an unclaimed arrow key falls through to PPSSPP's own mapper, which sends the very d-pad
direction being suppressed. Up and down are untouched: they are how `LOAD GAME` is selected on the
Game page.

**Matched by name, never by index.** The first working version used widget indices, and they were
right by luck - the eight tabs happened to be children 1..8 in that run. `EndsWith(name, "_t")` and
`strncmp(name, "BUTTONS")` cannot drift the way an index can, and they cost a string compare on a
tree that is walked once per menu open rather than once per frame.

**Applied on the edge, not every frame.** The game writes these fields when it builds a page and
never afterwards - confirmed by poking them and watching them stick - so re-asserting them every
tick would be the shape of mistake the camera pitch runaway already taught. `RestoreChrome` runs on
the way out and on shutdown, so turning the setting off, or taking a savestate with the menu open,
cannot leave the front end missing its furniture.

### Writing the page index froze the game, and the reason is a second field

`jumpDirectlyToPage` writes `MenuPage` instead of pressing the game's next-tab control until the
index matches. It removes the visible riffle through Controls/Audio/Display, it was shipped on, and
**it froze the game the first time someone pressed Enter on the Game page.**

```
Invalid exec address 00000000 pc=00000000 ra=08ae22c8
```

```
08ae22a8  lw    $a0, 0x30($s0)     ; the page's SELECTED widget
08ae22b0  lw    $a2, 0x24($a0)     ; its function table
08ae22bc  lw    $a2, 4($a2)        ; the method - read as 0
08ae22c0  jalr  $a2                ; <- ra=08ae22c8 is here
```

A page keeps its selection in `+0x30`, and writing `+0x1c` moves what is DRAWN without moving that.
So the Game page was on screen with a widget from the page you came from still selected, and Enter
made a virtual call through an object of the wrong type - which is why it survived two pointer
reads and died on the third. A null `+0x30` would have faulted on the *first* read; this got to the
third, which is what says "wrong type" rather than "missing".

**The verify-a-frame-later guard did not catch it and could not have.** It was added for exactly
this class of failure - the game reconciling the index on its next frame - and it was the wrong
guard: the index really did hold. Nothing was ever going to disagree about `+0x1c`, because `+0x1c`
was never the problem.

The setting stays, defaulted off, with the field named in its comment. The fast path is still the
right idea; it needs to move the selection with the page.

**The flicker is fixed the safe way instead.** `hidePagesWhileWalking` turns every page's widgets
off for the duration of the walk and back on when it ends - `visible` bytes only, the same
mechanism as the chrome, and it changes nothing about what the front end thinks is selected. The
backdrop stays up, so a walk reads as a brief pause rather than as four pages riffling past.

Every exit from a walk restores them - `Finish`, `GoIdle` and `RestoreChrome` all call
`ShowPagesAgain` - because a walk that ends any other way leaves the entire front end invisible.

### The front end has two levels of focus, and that one flag explains three bugs

`MenuUseRoot` (`FrontEndMenuManager + 0x1e`) reads **1 while the TAB STRIP has focus and 0 once you
are inside the page**. Measured on the Game page, not inferred:

| | useRoot | page |
|---|---|---|
| tabbed to it | **1** | 2 |
| after one Cross | **0** | 2 |
| after a second Cross | 0 | **10** - CONFIRM_PAGE, the "lose unsaved progress" prompt |

Three separate complaints were all this flag:

- **"I have to press another Enter to control the options."** A page tabbed to arrives with the
  strip still holding focus. `Arrive` now sends that Cross itself.
- **"Cross dismisses the map legend."** Same press. The legend belongs to the strip-level view of
  the map, so descending into the page is what puts it away - it was never a legend-specific
  mechanism, which is why generalising it cost nothing.
- **"The map cannot be panned."** The d-pad only pans once you are inside the page; at strip level
  it changes tab. An earlier hunt for the pan variables held a direction at strip level, watched
  the page index change, and concluded there were no pan variables.

**The guard on that press is not tidiness.** Sent while already inside the page, Cross does not
descend - it activates the selected entry, which on the Game page is `LOAD GAME`. `MenuOnTabStrip`
defaults to *true* when unreadable, so an unreadable flag makes the press conditional rather than
automatic.

### Dragging the map, and why this one writes state

`Map_AE + 0xc0` / `+0xc4` are what the map is centred on - symmetric x/y, found by descending into
the page and holding each direction (right moved x by -166.69 over the same interval down moved y).
`MapDragTick` adds the mouse delta to them while the left button is held.

This is the only part of the front-end work that writes game state instead of pressing a control,
and it earns the exception on the same grounds `VCSCamera` writes angles directly: **a held
direction is a RATE and a mouse gives a DISPLACEMENT**, and no input the game accepts means "pan by
this many units". Everything else here had a button that meant what we wanted; this does not.

Plus rather than minus, because a drag is the opposite verb to a d-pad press: holding right moves
the VIEW right (x decreases), while dragging right brings the CITY right.

The mouse reaches it because `ContextWantsMouse` now claims the delta in the `Menu` context while
`ContextDrivesCamera` refuses it - claimed but not steered, taken away from PPSSPP's own
mouse-to-analog path and spent on the map. `MapDragTick` runs from `FrontEndTick`, which is before
`CameraTick` in `VCSGame::Tick`, and that ordering is what lets it take the delta before the camera
drains it.

### Back comes back HERE, one level at a time

With the game's own menu up, Back shuts it and raises this fork's menu - the one those pages were
reached from - and the next Back leaves for the world. Two presses, two levels, and the row you
came from still has focus when you arrive.

It used to close the game's menu and drop straight into the world, on the reasoning that stacking
two menus is how one comes to feel like two. That reasoning still holds for STACKING and does not
hold for returning: the map is one row down from where the player was, and being sent two levels
for one press is how somebody loses their place.

**It cannot be done in one step**, and that is the shape of the code rather than a preference.
Closing the game's menu is a queued walk that presses the game's own Back control, and a menu of
ours on top pauses the emulator that walk needs to run. So `EmuScreen`'s `REQUEST_GAME_PAUSE` arm -
the one place every Escape, pad Start and Windows-menu pause funnels through - closes and sets a
flag, and `update` raises our menu on a later frame, once `GameMenuActive` has actually gone false
and the bridge has stopped driving.

**Backspace and the pad's B are what get you there, and neither is a PSP button any more.** Both
used to reach the game as Circle, which inside one of its pages lifts focus back to the TAB STRIP -
leaving the player standing in a menu this port navigates for them, in front of a row of tabs it
deliberately hides. They are claimed in the Menu context now and post `REQUEST_GAME_PAUSE` on the
press edge instead, the way the pad's Start already did. `VCSMenuScreen` answers Backspace as Back
as well, because `UI::IsEscapeKey` is PPSSPP's question about its own UI and has never heard of it.

### The Menu context finally triggers, and not the way it was tried before

`ResolveContext` returns `Menu` when `MenuActive` is set, which closes the TODO that has sat in it
since the beginning. Everything downstream was already written for it: the Menu rows in
`kVCSKeyMappings` map WASD to the d-pad, and `ContextWantsMouse` already excluded Menu, so the
mouse is left alone in menus without a line being changed.

**`GameState` was the wrong question, and that is the transferable part.** It was hunted twice as a
general "what is the game doing" enum and never found - a 192KB sweep gave 1249 candidates and a
correlation run ruled out the best cluster. The input layer never wanted the enum. It wanted one
bit, "is a menu up", and that bit is a field of the menu's own object, reachable from a script
command in twenty minutes. Ask what the caller needs before hunting for the thing it was called.

This is also **not** the reverted `FrameCounter`-stall shortcut from `03c1f3cfe0`. That inferred
the menu from the game's logic being stopped, which is also true during loading screens and
cutscenes - a superset that catches the game mid-play. This reads the menu's own flag.

### The page titles are art, and they have to be

`Tools/vcsmenuart.py` bakes the script-font headings into `assets/vcs/`, and they are baked
because PPSSPP cannot draw them any other way. `FontStyle` selects a font
*family* - `SansSerif` or `Fixed`, and that is the whole enum - and `SetFontNameOverride` maps a
family to one face **globally**. There is no way to ask for a brush script for one string and the
UI sans for the next. Baking them also matches what the game does: its headings are sprites.

Re-run the script after editing the title list; the keys in `TITLES` are the names
`VCSMenuScreen::PageTitle` asks for, so the two have to stay in step. It uses stock Windows fonts
(Brush Script MT) and ships only the rendered pixels, no font file.

**The backdrop is a flat fill now**, `kBackgroundColor` in the menu, and it is the generated
image's own base colour - the midpoint of that gradient - so nothing moved when the art came out.
`make_background()` is still in the tool but is no longer called; it is the only record of how
the patterned version was built, and one line in `main()` brings it back.

The one thing to know about that generator, if it ever is brought back: the leaflet base width is
derived from the leaflet spacing, not chosen. Narrower than the gap and the fronds read as
fishbones - a row of separate spines with the background showing through. At 0.8 of the spacing
they overlap into one silhouette with a jagged edge, which is the shape the eye reads as a palm.

Missing art is not fatal - a missing title simply does not draw - so the menu still works on a
platform where `assets/vcs/` was not deployed.

### The menu face: two font systems, two different names for the same font

Menu items are set in Pricedown (`assets/vcs/pricedown.ttf`, listed in `g_fontDescs` under the
new `FontFamily::Display`). Getting there took four wrong turns, all worth writing down because
every one of them fails *silently* - a font that cannot be found is never an error in either API,
the text just comes out in some substituted face.

1. **A family maps to exactly one face, globally.** `FontStyle` picks `SansSerif` or `Fixed`, and
   `SetFontNameOverride` sets the face for a whole family. Wanting Pricedown for menu items *and*
   the UI sans for the hint bar therefore means wanting a third family, which is why
   `FontFamily::Display` exists. Hijacking `Fixed` would have worked and would have quietly
   changed the code font in the dev screens.
2. **On Win10+ the text drawer is DirectWrite, not GDI.** `TextDrawer::Create` picks
   `TextDrawerUWP` unless RenderDoc is attached or the OS is older than Win10, in which case it is
   `TextDrawerWin32`. The first attempt registered the font with `AddFontMemResourceEx` - a GDI
   call - and it succeeded, handle and all, into a font table nothing was reading.
3. **The two APIs match on different names.** This font's name-table ID 1 is "Pricedown Black"
   and its typographic family (ID 16) is "Pricedown". **GDI matches ID 1; DirectWrite matches
   ID 16.** No single string satisfies both, so the desc carries the DirectWrite name, which is
   the one that matters on the platform this fork targets. Tools mostly report ID 16, so the name
   you get from asking a font library is the one that does *not* work under GDI.
4. **Nothing needs to register it at runtime.** `TextDrawerUWP` already builds its own
   `IDWriteFontCollection` from an in-memory font set, loading every filename in `g_fontDescs`
   through the VFS. Adding a row to that table is the entire integration. The file is an OTF
   (CFF outlines) named `.ttf` because the loader appends that extension unconditionally; every
   backend sniffs the container rather than trusting the name.

### A screen that pauses the game has to say so

`VCSMenuScreen::update()` calls `UpdateUIState(UISTATE_PAUSEMENU)` every frame, exactly as
PPSSPP's own pause screen does, and it is not decoration. The mouse pointer is the visible
symptom: `CorrectCursor()` in `Windows/MainWindow.cpp` only un-hides the cursor once the UI state
is something other than `UISTATE_INGAME`, and with mouse control on it also keeps the pointer
clipped to the window. Without the call the menu came up with no pointer at all. The framebuffer
manager and the WebSocket game broadcaster read the same state, so this is not only about the
cursor.

Measured rather than assumed, via `GetCursorInfo`: `HIDDEN` in gameplay, `SHOWING` with the menu
open.
