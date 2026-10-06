# Boot, the main menu, saves and loading screens

> Read before touching launch flow, the save list, autoload, the loading screen or anything that drives a game dialog.
> Moved verbatim out of the old CLAUDE.md; sections cross-reference each other by title, so `grep -rn "<title>" docs/vcs`.

### The main menu at launch, and the one thing it needs to know

`CreateStartScreen()` is what the app opens on, hooked into the `AfterLogoScreen::DEFAULT` arm of
LogoScreen. It returns the VCS main menu when a VCS disc has been booted before, and PPSSPP's
ordinary game browser otherwise - there is nothing to put behind LOAD GAME until we have been told
which ISO that is.

The main menu and the pause menu are the *same screen* in two modes (`VCSMenuMode`), because they
differ only in what the top row does and in what Back means. One page table, one look, one set of
option rows. Two things the MainMenu mode has to get right:

- Back on the root page is swallowed. The main menu is the bottom of the screen stack, and
  letting `UIDialogScreen` finish it would pop the last screen and leave the app with nothing.
- `EmuScreen::bootComplete()` records the disc through `File::ResolvePath`, not as given.
  `gamePath_` is routinely relative - a command line, a drag-and-drop - and storing it raw meant
  the main menu only found the disc when the app was started from the same working directory.
  It looked like it worked, because during testing it always was.

### The boot sequence, measured: there is no menu to bridge to

The launch-time main menu was expected to need a bridge into the game's own front end - drive its
menu, pick New Game, get out of the way. **That front end does not exist.** Measured on the retail
disc, cold boot, nothing touched:

| time | on screen | `FrameCounter` `0x08bb3bb4` | `TimeStep` `0x08bb3b5c` |
|---|---|---|---|
| 0-60s | logos, then the credits sequence | `0` | `0.0` |
| ~60-68s | the seam | | |
| 68s+ | gameplay, Fort Baxter, story running | `30919`, climbing ~32/s | `1.6684` |

VCS goes logos → credits → straight into the story. There is no New Game / Load Game screen at
boot at all; loading a save is reached from the pause menu once you are in. So the hardest part of
the launch-menu problem was never there, and "START GAME continues the autoloading" is the literal
truth: the game is already on its way in.

**The hook is the `FrameCounter` 0 → nonzero edge.** It is the moment the world starts, it happens
exactly once per boot, and both clocks agree on it.

Two things this measurement corrected:

- **`FrameCounter` does not count frames since boot.** It jumps 0 → ~30600 at the seam rather
  than counting up, so it is initialised with the world, not incremented from power-on. Only the
  edge is meaningful; the magnitude is not.
- **This is not the idea that was reverted in `03c1f3cfe0`.** That one used "logic stopped" as a
  *continuous* gate during play, where loading screens and cutscenes make it wrong. This uses the
  *first* 0 → nonzero transition after boot, once, to place the menu at the seam. Same address,
  different claim - and this one is monotonic, so the ambiguity that killed the other cannot
  arise.

**Cross skips the credits; Start alone does not.** Measured by injecting a press through the
WebSocket debugger 20s into the credits and timing the seam: it arrived 3.7s later instead of the
~60s the sequence takes on its own. So the skip presses the game's own button rather than trying
to fast-forward anything, which is why there is still a few seconds of wait after a keypress.
`FrameCounter` reads a fixed 30602 at the seam every run, so the value is deterministic even
though only the edge is used.

**The flow, as built.** `CreateStartScreen` boots the remembered disc with no menu in front of
it; `VCS::UpdateBootPhase` watches for the edge; `EmuScreen` acts on it when it lands. Any key
during the intro calls `RequestIntroSkip`, hooked in `NativeKey` next to the existing VCS key
claim.

(What `EmuScreen` did there was raise a menu. It loads the most recent save now - see the next
section but one.)

Do not trust a reading taken from a session that has been running a while: mid-investigation this
looked disproven, because the sample was taken from an instance that had already crossed the seam
and was showing the gameplay values. The credits really do run with both clocks at zero; check
what is on screen before believing a reading.

### There is no menu at the seam any more, because the game is what you asked for

The startup menu is gone. It was raised at the seam, over a game that was already running, and it
offered START GAME - which is a screen asking you to start the game you started. What happens
there now is the thing its LOAD GAME row was for: `RequestAutoLoad` walks the game's own front end
to LOAD GAME and takes the save the dialog offers, with no key pressed. `VCSMenuMode::Startup` is
still defined and nothing raises it.

Two things make that a small change rather than a feature:

**The dialog's own default IS the most recent save, and the game chose that, not us.** VCS asks
the firmware to focus the latest entry, so the list comes up on it. Measured with eight saves on
the memory stick: the list opened on the one written at 12:25 that day rather than on slot 0. So
"load the most recent" needs no slot bookkeeping at all - and a player who wants a different one
gets the ordinary list to move through, because the sequence stops pressing the moment the load
starts.

**A first run has nothing to load, and asks for nothing.** `RequestAutoLoad` checks the memory
stick for a `PARAM.SFO` under this disc's ID before it presses anything, and the story the game
has already started just runs.

The one place the sequence is patient rather than strict is the very first Start: the seam lands
inside the opening scene, where that button may be spent skipping something. It gets three goes,
and only the auto-load does - everything else still gives up after one.

### The curtain goes up when the movie ends, not at the seam

After the credits - played out or skipped - the game used to show three screens of PSP machinery
before the seam: a title-and-legal card, the firmware's "Loading from Memory Stick, do not remove..."
over `MEMCARD.XTX` while it autoloads, and its own jet-ski loading screen while the world streams.
Then the seam, where the auto-load raised this fork's curtain anyway. The curtain goes up at the END
OF THE MOVIE now and the seam simply takes it over.

**The end of the movie is a disc read.** Every read of a boot logged by file, with the credits
skipped:

| t (s) | read | on screen |
|---|---|---|
| 0.87 | `LOGO.PMF` | logos |
| 2.94 | `TITLES.PMF` | the credits |
| **3.45** | **`SCEALE.XTX`** | the title card - the movie has just ended or been skipped |
| 4.63 | `MEMCARD.XTX` | "Loading from Memory Stick" |
| 6.88 | `LOADSC*.XTX` | the game's loading screen |
| 7.45 | | the seam; `FrameCounter` nonzero |

`SCEALE.XTX` is read at that moment and at no other in the boot, and the fork already sees every read
in `PatchDiscRead` - so the trigger needed no new seam into PPSSPP. It matches the file's 16-byte
header at its sector (26960) before raising anything, because a curtain raised over the movie by a
different layout would be worse than none.

At the seam `RequestAutoLoad` re-raises it fresh and its sequence takes it down as before. With
nothing to load - a first run - `ReleaseBootCurtain` lets it settle thirty game frames and drop on its
own, instead of sitting there until its 1800-vblank ceiling. What shows after that on a first run is
the game's black "Loading..." card for the opening mission, which every mission start shows and is
left alone.

Measured both ways: with saves, LOADING from the first frame after the skip until the safe house;
with the memory stick empty, LOADING until the seam and then the opening mission.

### What a save is actually made of, and the bug that hid in it

**The save-menu request byte is not a save.** Setting `gp+0x1076` opens the save UI and the game
writes a file, and that file loads into the OPENING MISSION carrying your money, your clothes and
your percentage. Reported once as "the save game button worked, but loading it started the game
over", and reproduced here twice before the cause turned up.

The cause is in the script, eight instructions before it asks for the menu. The safehouse save
routine reads:

```
04 00 d0 15 07 01     0004: $789 = 1        ONMISSION
04 00 cd 04 07 01     0004: $4   = 1        "this game came from a save"
36 00 ce 1c d0 0f     0036: $284 = $783     the restart position, x
36 00 ce 1d d0 10     0036: $285 = $784                           y
36 00 ce 1e d0 11     0036: $286 = $785                           z
c0 02 d0 0e ce 12     02C0: weapon  -> $274
fe 02 d0 0e ce 13     02FE: armour  -> $275
96 00 d0 0e ce 14     0096: money   -> $276
5a 00 d5 78 d5 77     005A: get_time_of_day $2168 $2167
60 02                 0260: activate_save_menu
```

and the main script's first decision, on boot, is on the second of those:

```
$_4 == 0 && $2 == 0  ->  $ONMISSION = 1; 0289: load_and_launch_mission_internal 8 // Soldier
```

**`$4` is the whole bug, in the game's own words.** The engine writes the script's global block
into the save file; a save taken without that preamble carries `$4 = 0`, and the game that loads
it does exactly what the script says to do with a zero there. Confirmed from the other end too:
after loading such a save, `$4` reads 0 and `$789` reads 1 - a game running mission 8.

So `PrepareScriptForSave` does what those instructions do and `RestoreScriptAfterSave` puts back
what it changed, which is also what the script does four lines further down its own routine. The
restore runs from `GoIdle`, the one funnel every sequence ends through, because leaving
`$ONMISSION` at 1 after an abandoned save would quietly stop every mission trigger in the city.

Three things worth keeping from working it out:

- **Global N lives at `[gp - 0x71dc] + N*4`**, from the operand decoder at `0x08861a7c`: an
  operand byte `>= 0xcd` is a global and its index is `(type - 0xcd) << 8 | nextByte`. The base is
  a heap pointer - `0x09f68400` on one run - so it is read every time and never baked.
- **`$_N` and `$N` in a Sanny listing are the same numbering.** `$_274` decoded to `ce 12`, which
  is index 274. The underscore is notation, not a second address space, and assuming otherwise
  would have cost a day.
- **The weapons, armour and money at the end of that preamble are deliberately not written.** The
  load path (`MAIN_2139`) restores weapons from the engine's own save data and only reads `$274` /
  `$275` again on the wasted-and-busted path, which the script refreshes for itself at every
  mission start. The position is different - the load path copies `$284..286` straight back into
  `$783..785` - so that one is written.

**The restart position written is the PLAYER's, and NOT `$783..785` the way the script copies it.**
That is a deliberate departure, and the reason is that the position is only meaningful next to
`$_282`, the safe house whose interior is swapped in. `INITSAV`, the script's own spawn code, reads
the pair together:

```
$_282 > -1  ->  swap that interior back in, place the player from its own table
otherwise   ->  load_scene $783 $784 $785
either way  ->  get_ground_z_for_3d_coord ; set_char_coordinates there
```

The safe house routine can copy the pickup because it only ever runs while the player stands ON
that pickup, inside the house, with `$_282` naming it - both halves describe one moment. An
auto-save fires wherever the mission ended, with `$_282` at -1, so copying the pickup pairs an
INTERIOR position with a world where that building is solid. Reported from play: loading such a
save materialised the player inside the safe house's shell with no way out but dying, and it had
looked harmless for as long as one particular safe house was in use whose pickup happens to sit
somewhere escapable. Measured on the one that produced the report - restart `-800.73 -1183.65
10.90` against a pickup at `-817.56 -1181.43 13.76`, with nothing to swap that building in for the
second row.

Reading the player on the same frame `$_282` is saved cannot disagree with it. `$783..785` survives
only as the fallback for an unreadable player, and `$_287` - the heading `INITSAV` faces the player
in, degrees there and radians in `PedHeading` - goes with the position for the same reason.

### A loading screen over both of them, because the machinery is not the game

Neither automatic sequence is something the player did, and both of them put the game's own menus
on screen: a tab strip being walked, a save list, a firmware dialog answering its own prompts.
Reported, fairly, as "kind of clunky looking". So both go behind a curtain - the menu's own
backdrop with one word on it, LOADING or SAVING - and the player sees a loading screen where they
would otherwise have watched a menu being driven by nobody.

**It is drawn onto EmuScreen, not pushed as a screen.** A `UIScreen` pauses the emulator, and the
one thing this curtain cannot do is stop the game: what it is hiding is the game working. So it is
a draw call at the end of `EmuScreen::render`, after the route overlay, over everything.

**The same answer hides the walk and silences the pad.** A key pressed during a walk lands in
whichever page the walk has reached - it could pick a different save, or answer the prompt with No
- so `NativeKey` swallows presses whole while the curtain is up, before the mapper, before the VCS
layer and before the UI queue. Two exceptions, both deliberate:

- **Releases still go through**, keys and axes alike. A player who skipped the credits is still
  holding that key when the curtain goes up a few seconds later, and a release nobody is told
  about is a key that stays down forever - in this fork's held-key set and in PPSSPP's mapper. A
  release can select nothing.
- **The VCS layer is still told about axes**, and only the route to the mapper is cut, for the
  reason `NativeAxis` already documents: an axis carries its whole state in every event.

**It comes down on the GAME's clock, not on a timer**, and that is what makes it fit. The curtain
waits for the sequence to finish and then for thirty game frames - and the game's frame counter
does not advance while the world is being loaded, so those thirty frames cannot start counting
until there is a world again. Measured over a boot: up at the seam, and down as the first frame of
the safe house arrives, eleven seconds later, most of which is the game's own load.

There is a hard ceiling of 1800 vblanks on top of that, and it is not defensive habit: this thing
covers the screen AND holds the pad, so a curtain that can get stuck is a game that cannot be
played.

**A save the player asks for is not covered.** `RequestSaveMenu` raises nothing - somebody who
opened the save menu went looking for it. Only the auto-save does.

### The save menu is ours now, and the game loads a save by itself at boot

`GAME` on the pause root opens this fork's own page - NEW GAME, LOAD GAME, DELETE GAME - and the
two lists are ours as well: eight rows built from `PARAM.SFO`, the mission each save is named
after as the label and the game's own description as the help line, with a `*` against whichever
is newest. Everything on screen there was written by the game; only the drawing is this port's.

Loading still goes through the game, because only the game can put a world back together: the row
asks for a slot, the bridge walks the front end to LOAD GAME and steers the firmware's list to
that entry, all behind the loading screen. Deleting does not - it is a directory, and the game
re-enumerates the memory stick every time it opens a list, so `VCS::DeleteSave` removes it here
rather than walking into a third firmware dialog to do the same thing.

**The finding that cost the most, and explains three earlier confusions: VCS loads a save by
itself at boot.** Not through its menu, and not through this fork - the game asks the firmware for
a silent AUTOLOAD of a save it names, and the world that comes up is that save's world. It is why
a first run with an empty memory stick starts the story and a later run does not, and it is why
the fork's own auto-load usually looks like it "worked" even in builds where it had not run at all.

That is what defeated the first two attempts at NEW GAME:

- **Walking the game's own NEW GAME hung it, until the bridge let go of the world first.**
  Confirming that entry tears the world down, and the bridge was standing in the middle of the
  teardown holding pointers into it. The press was paced on the game's logic clock, which stops
  while the world is being rebuilt, so it never finished: the game came back with its own confirm
  page open on a black screen, its frame counter racing, and nothing recovered it. The same
  presses injected from outside, released on a wall clock, started a new game every time - that
  control run is what separated our timing from the game's behaviour. A wall-clock watchdog on the
  press was tried and did NOT fix it, which ruled out the stuck press and left the teardown itself
  as the thing to get out of the way of.

  What works is releasing everything that points into the front end BEFORE the confirming press -
  `ShowPagesAgain`, `RestoreChrome`, `g_frontEndVolatile`, and clearing any queued request so a
  stale Close cannot fire into the new world - then pacing that last press in VBLANKS and letting
  `Phase::EndSequence` dispatch ahead of the game-frame gate, so the pad is handed back even
  though the game's clock has stopped. Verified from the other end: the mission key empty,
  `$ONMISSION` 1 with mission 8 running, the player at Fort Baxter, the front end closed and the
  frame counter climbing - and on screen, Martinez's office, with no logos and no credit roll.
  That last part is the whole point of walking rather than rebooting.
- **Rebooting the disc is not enough on its own**, because the boot autoload then quietly
  continues from a save again. Twice this looked like "the reset did not happen".

The reboot survives as the FALLBACK, for the case the walk cannot start at all: the game keeps its
menu to itself during a cutscene, which is exactly where a player is most likely to ask for
another new game - the opening scene of the one they just started. Start is declined, the walk
gives up, and `AbandonSequence` boots the disc and tells that one autoload to find nothing
(`TakeNewGameBoot`, consumed by `PSPSaveDialog`'s autoload path). It costs the intro, which is
what walking was worth avoiding, and it beats a row that does nothing. That give-up used to stop
at an error toast, which is how "NEW GAME during a cutscene" came to do nothing at all.

### The save icon opens this fork's SAVE GAME page

**Status: working, measured.** Walking into the save icon at a safe house used to put up the
firmware's savedata list - PPSSPP's own slot browser in PPSSPP's own look, the last screen in
ordinary play that still looked like an emulator. Now this fork's menu goes up over it on a SAVE
GAME page: the eight slots as the load page shows them, EMPTY ones included because an empty slot is
somewhere to save, and an occupied one asking "overwrite?" on the confirmation page first. The pick
is walked through the firmware list behind a SAVING curtain - steered to the slot, its own overwrite
prompt answered, "save completed" dismissed - which is the auto-save's dialog phase with a slot of
the player's choosing (`g_saveSlot`). BACK, Escape, or any other way out of the page cancels: the
walk presses the dialog's own Back until it is gone.

**How it knows the icon opened it**: a firmware SAVE list on screen while the bridge is Idle. Every
sequence of ours that opens that dialog is in a phase of its own by then, so a list that arrives with
nothing running can only be the game's. The tick sets `g_iconSaveAsk`, `EmuScreen::update` takes it
and pushes `CreateIconSaveScreen`, and the page's answer comes back through `AnswerIconSave`. The
menu's destructor answers "cancel" if nothing else did, so the firmware dialog is never left waiting.

**Nothing of the script's preamble is repeated**, unlike `RequestSaveMenu`: the safe house routine
has already done it before it called `0260` - see "What a save is actually made of" above.

**Tested by writing the request byte (`gp+0x1076`) over the debugger** rather than walking to an
icon, which opens the same UI but skips that preamble - so the file it writes is the broken kind. The
memory stick's saves were copied aside first and the slot used was put back afterwards. Measured: the
page came up over the list, slot 8 picked and confirmed, SAVING, and `ULUS10160S92F7/PARAM.SFO`
rewritten; cancelled, the world was back within a second with the game's menu closed.

`ownSaveMenu` on `VCSFrontEndSettings` hands the icon back to the firmware dialog.

### The questions are pages, not popups

Deleting a save, starting a new game and quitting each ask first, and all three ask on a page of
this menu rather than through `MessagePopupScreen`. PPSSPP's popup is a perfectly good dialog and
it is the wrong one here: it is a blue box in PPSSPP's own font over a menu that is neither.

One page serves all three, because a confirmation IS a page with two rows on it and this menu
already knows how to draw one. It carries the heading of whatever page asked - Delete Game, Game,
Quit Game - so the screen does not appear to jump somewhere else to ask, and Back on it goes to
the page that asked, which the page table expresses by making `ParentPage` a member rather than a
static: the parent of a question is wherever it was asked from.

The shape is San Andreas's, from the screenshots that prompted it: the question left-aligned under
the heading, wrapped in a box so a save's name can be as long as it likes, and NO above YES,
centred, with NO focused. **NO first is not a style choice** - the default answer to a question
nobody has read yet should be the one that changes nothing, and the game's own confirm page does
the same.

Every affirmative only RECORDS what to do; `update` acts on it a frame later. That is the same
rule the popups needed and for the same reason: a row that finishes the screen or rebuilds it from
inside its own click handler leaves the menu rendering and taking no input at all.

### The save list is a column, not a stack of centred names

Rows on the two save pages start at ONE x rather than being centred on their own length -
`VCSMenuItem::SetLeftAligned`, and it is the only page that asks for it. Eight titles of eight
different lengths, each centred on itself, read as a ragged pile; started at the same x they read
as the list they are. BACK stays centred, because it is an action rather than an entry in the
list.

### Saving after a mission, and how the game says one was passed

`01EB register_mission_passed` is the trigger, resolved from the script command table the usual
way - `u32(0x08b846e0 + 0x1eb*8 + 4)` = `0x08886064`. It memcpys eight bytes of GXT key to
`gp+0x1fc0` and increments `gp+0x1fc8`, and **the decompiled MAIN.SCM calls it from exactly one
place** - a shared subroutine every mission ends through. That is the game's own definition of a
mission being passed rather than a correlation with one.

`036A register_oddjob_mission_passed` bumps the counter and leaves the key alone, which is what
separates the two: a story mission changes the key, a race or an empire job only moves the count.
The auto-save watches the KEY, so odd jobs deliberately do not fire it.

Eight bytes get compared, not four. `LAN_C01` and `LAN_C02` share their first word.

**The counter is not a total.** It reads 0 on a 14.4% save, because a load does not restore it -
it counts the session. Only the edge is used, so that costs nothing, but do not put it on screen
as "missions passed".

**A load looks exactly like a mission being passed**, and that is the one trap in the watch: the
world restarting brings a different key with it. The frame counter rewinding is what tells the two
apart, and without that check the first thing an auto-load does is fire an auto-save over the save
it just loaded.

### Driving a dialog the game does not own

The load and the save both end at the firmware's savedata dialog - the slot list, the "overwrite?"
prompt, the progress bar - and PPSSPP draws all of it. So the doctrine the rest of this fork
follows, drive the mechanic and watch the state, runs out of state to watch: `FrontEndMenuManager`
freezes at whatever page it was on and nothing in PSP memory says which screen is up.

`Core/VCS/VCSSaveDialog.h` is the answer - a read-only report of what the dialog is showing,
filled in by `PSPSaveDialog::VCSPeek`. The alternative was pressing Cross on a timer, which is the
class of thing that works on the machine it was written on. Three things it has to carry:

- **The buttons, because which is which is a setting.** "Save completed" offers Back alone
  (`DS_SAVE_DONE` accepts only the cancel button), so a sequence pressing OK at it sits there with
  the save already written until it times out. Measured, from a run that did exactly that.
- **`yesnoChoice`, because the overwrite prompt opens on NO.** LEFT is what moves it to YES, and
  an OK sent without that press cancels the save it was meant to confirm.
- **Busy, which folds the IO thread and the fades into one answer**, because the caller does the
  same thing with both: wait. A press that lands in a fade is a press the screen underneath never
  sees, and the sequence goes on believing it was made.

**The dialog phases are paced in vblanks, not game frames**, and that is not a preference: the
world is stopped behind that dialog, so the logic clock every other phase here runs on has stopped
with it and would never hand out another frame.

### A menu we own sidesteps the GameState problem entirely

reVC's menu knows the game is paused because it *is* the game. This fork cannot ask VCS the same
question - `GameState` was hunted for and never found, and the `FrameCounter`-stall shortcut was
shipped and reverted for breaking the controls (see the `Menu` context note below).

None of that matters here, and it is worth being clear about why: the pause menu is a PPSSPP
`UIDialogScreen`, so PPSSPP owns its open/closed state and pauses emulation for it the same way
it does for its own. The unsolved problem is only reachable if a menu tries to sit on top of the
*game's* front end and mirror it. **A launch-time main menu - New Game, Load Game - is the case
that does need a bridge into the game's own front end**, and that bridge is a one-shot scripted
input sequence right after boot, not a per-frame state read. That is the narrow version of the
problem and it is still open.

Being a normal PPSSPP screen also means it runs on the same thread as `VCS::Tick()`, for the same
reason `UI/ImDebugger` does - see Threading above - so it reads and writes the settings structs
directly, with no marshalling.

### Hover is the one thing PPSSPP does not give you

PPSSPP moves focus on click and on keyboard/pad navigation, and has no notion of a hovered row -
`TouchInputFlags::HOVER` exists in the enum and nothing uses it. A menu whose rows do not light up
under the mouse reads as broken on a PC, so `VCSMenuItem::Touch` sets focus on a buttonless
`MOVE` inside its bounds. That is exactly what reVC's `UserInput()` does when it hit-tests the
pointer against every row, and it is the only piece of its input handling that genuinely had to be
written again. The buttonless check is what keeps it from firing mid-drag on a value.

### Five bugs this layout produced, all worth recognising again

- **The row you can see is not the row you can click.** Rows are laid out at the full screen
  width, because the two-column settings layout aligns on the screen's centre line rather than on
  anything the row owns. That makes `bounds_` a band the entire width of the display, so
  hit-testing it meant any pointer at the same *height* as a label counted as being on it, however
  far away horizontally. Measured, not guessed: a trace of the touch events showed every row as
  `bounds = 0.0, y, 1920.0 x 76.0`. `VCSMenuItem` now measures the drawn text during `Draw` and
  hit-tests that box, and reproduces `Clickable::Touch`'s press/release handling against it -
  delegating to the base is not possible, because every containment test in it reads `bounds_`.

- **Centre on the screen, not on the row.** Every row is laid out full width, so `bounds_.centerX()`
  looks like the screen's midpoint and reads correctly in code. It was not - the rows came out
  narrower than the screen - and the entire menu sat 80 pixels left of centre while every
  individual piece of it looked right. `ScreenCenterX()` asks `g_display` instead. Any layout
  built around a centre line wants the screen's, not a view's.
- **A screen outlives the graphics device, so its GPU objects cannot be freed in its
  destructor.** The title textures were released in `~VCSMenuScreen` and cached in a file-scope
  static, and both are too late: `NativeShutdownGraphics` calls `g_screenManager->deviceLost()`
  and then tears Vulkan down, while `NativeShutdown` deletes the screen manager much later
  still - and a static's destructor is later than that again. Quitting with the menu open
  therefore left textures alive past the end of the allocator, which VMA reports as
  `m_pMetadata->IsEmpty() && "Some allocations were not freed before destruction of this memory
  block!"`. Quitting from gameplay did not, because closing the menu had already released them -
  which is what made it look intermittent. The cache is a member now and `deviceLost()` is what
  empties it. Two things follow that are easy to miss: the release path has to be idempotent,
  since the destructor still runs afterwards, and a lazy loader has to be **gated** on the
  device being present - `Title()` creates a texture whenever the map lacks one, and `Release()`
  empties the map, so without the flag the first draw after a device loss would rebuild them all
  on a dead device.

  Still open, and deliberately not chased: closing the window with the menu up has once produced
  `vkQueuePresentKHR failed! result=VK_ERROR_DEVICE_LOST` from `VulkanRenderManager::Run`. It did
  not reproduce on the next attempt, it is a different failure from the VMA one above, and one
  occurrence is not enough to act on. Note it if it recurs; do not assume the fix above covers it.

  **Second sighting, 2026-08-19**, and this one was NOT at a menu: the process died mid-session
  during the vaulting work, a few minutes after an experiment that dropped the player through the
  world (writing the ped's climb state on land - see the vaulting section). Whether the fall is
  related is unknown; a ped below the map does put the camera somewhere the renderer never expects.
  Two occurrences, two different contexts, still not enough to act on - but if a third arrives,
  the common factor to look at first is the camera being somewhere absurd rather than anything
  about menus.

- **A "did we try yet" flag has to be cleared with the thing it guards.** `backgroundTried` was
  set on the first load attempt and never reset by `Release()`, so the backdrop appeared once per
  run and every menu after the first came up on the fallback colour - which looks enough like a
  deliberate flat background to not read as a bug. The flag and the texture are both gone now
  that the background really is a flat fill, but the shape of the mistake is not specific to
  textures: any cache with a companion "already tried" flag has to reset both together.
- **Taking focus on hover means taking it *forced* on click.** With hover focus added and
  `Clickable::Touch` reimplemented, the mouse stopped selecting anything at all: every row
  highlighted correctly and no click ever fired. `TouchEvent` calls `EnableFocusMovement(false)`
  on any press that did not force focus, which sends `LOST_FOCUS` to the focused view, and
  `Clickable::FocusChanged` clears `down_` and `dragging_` - so the release found nothing pressed.
  Stock survives this because it claims focus only `if (IsFocusMovementEnabled())`, and after the
  first press there is no focused view left to lose. Hover puts one back every frame, so that
  escape hatch is gone. `VCSMenuItem::ClaimFocus` uses `SetFocusedView(this, CAUSE_FORCED, true)`,
  which is what `TextEdit::Touch` uses to keep the focus it takes - and it has to run *before*
  `down_` is set, because `SetFocusedView` sends `LOST_FOCUS` to the outgoing view even when the
  outgoing view is this one.
