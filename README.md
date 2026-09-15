# RE1-1996CabbyCodes

A cheat panel for **Resident Evil (1996)** on Steam - the classic 1997 PC version. Open the game's
option screen while playing (`A` on the keyboard by default) and a small panel appears beside it - and
on the title screen it comes up beside the *Load Game* list to manage your save files:

- **God mode** - your health is held at its maximum and the poison that drains it is cured, every
  frame. Yawn's poison is left for the serum - the story needs it - and with your health held it
  cannot hurt you.
- **One hit kills** - every live enemy is held at 0 HP, the last value the game still counts as alive,
  so your next hit kills it (deaths and drops still play out through the game's own code).
- **Infinite ammo** - firing no longer uses up ammo: every gun keeps its loaded rounds, the
  flamethrower its fuel. Pickups and reloads still work as usual.
- **Infinite ink ribbons** - saving at a typewriter never uses up a ribbon, and you can save without
  carrying one at all (the typewriter takes the game's own no-ribbon path).
- **Save without counting** - saving does not add to the save count the ending shows. The game adds one
  again when a file is loaded, so a save made with this on is written one lower, and loading it gives
  back the count you had.
- **Save count** and **play time** editors - change the counter and the clock (hours, minutes,
  seconds) the ending shows and ranks you on.
- **Freeze play time** - the play-time clock stops, in the inventory too.
- **Freeze countdown timer** and a countdown editor - the self-destruct countdown stops where it is
  while the switch is on and runs on the moment you switch it off; the editor sets what is left of it
  (up to its full 3:00). Both only act while the countdown is actually running.
- **Inventory editor** - the inventory of the character you are playing (six slots for Chris and
  Rebecca, eight for Jill): pick any item for a slot, set its count and *Apply*. Open a slot's list and
  type to narrow it (by name or item number); Enter takes the first match. An item the game shows under
  another name until it is examined is listed with that name in brackets - *Closet Key (Special Key)* -
  so typing either one finds it. Counts are held to what the game itself fits in one slot, read out of
  the running game: a gun's loaded rounds (15 for the Beretta, 7 for the Shotgun, 6 for the Colt Python
  and the Bazooka, 4 for the Rocket Launcher, 240 fuel for the Flamethrower), up to 250 of an ammo or of
  ink ribbons, and 1 to 255 of anything else - a key's count is how many more doors it opens. The PC
  version's two reward weapons, the Ingram and the Minimi, show as *infinite*. The inventory stays
  packed the way the game keeps it: an item put in an empty slot lands in the first empty one, and
  emptying a slot moves the items after it up. **E** marks the equipped weapon; it can be replaced or
  stored like anything else, and when the option screen closes the game equips whatever weapon is in
  that slot - nothing, if the slot no longer holds one. The inventory screen's pictures follow the
  change.
- **Item box** - the game's own item box: the same 48 slots every item box in the game opens, saved
  with your game, and reachable from the panel wherever you open the option screen. *Store* beside an
  inventory slot puts that item in the box, *Take* puts a box item into the inventory of the character
  you are playing, and *x* throws one away. Ammo and ink ribbons are combined both ways, in stacks as
  big as one slot holds (250): storing a few rounds joins them with every stack of that ammo already in
  the box, and taking a stack out tops up the one you carry first, as a pickup does, before a free slot
  is used. Everything else takes a slot of its own, and whatever fits nowhere stays where it was. The
  panel lists the box under five tabs - key items, weapons, ammo, heals, and other - each with a count
  of what it holds and sorted by name, with a filter box that works like the item lists; **#** is the
  slot an item is in on the game's own item box screen.
- **Save file manager** - on the title screen's *Load Game* list the panel shows the game's eight save
  files (character, play time and save count). **Delete** removes a file: the game shows it as NO DATA
  from then on, exactly like a file that was never used, and the next save into it fills it again.
  **Copy to...** puts a copy of a save into any other file, empty or not. Both ask first, both change
  the files in the game's `SAVE` folder straight away, and the game's list beside the panel shows the
  change at once. Delete is only offered once the mod has found the list's own record of the files,
  which it keeps in step - without it the game would still offer a deleted file for loading.

Tick or edit what you want and close the option screen. The switches keep their state for the session
(start-up defaults live in the ini). While the panel is up the game's mouse pointer is shown and free to
reach it, and the game does not see what you type into the panel's boxes (Escape still closes the
option screen). **F7** hides or shows the panel; it is back the next time its screen opens.

Works on Windows and on Linux/Proton with no launch options; it is developed and tested on
Linux/Proton.

## Install

1. Open the game's folder (in Steam: right-click the game, *Manage*, *Browse local files*), then its
   `english` folder - the one containing `ResidentEvil.exe`.
2. Rename the existing `ddraw.dll` to `ddraw_orig.dll`.
3. Copy the mod's `ddraw.dll` in beside it.

The game already loads the `ddraw.dll` in that folder - GOG's DirectDraw wrapper, which the mod stands
in front of - and Proton loads it natively for this game on its own, so there is nothing else to set
up: no launch options, no `WINEDLLOVERRIDES`. Steam's *Verify integrity of game files* puts the stock
DLL back; just repeat step 3 if that happens. To uninstall, delete the mod's `ddraw.dll` and rename
`ddraw_orig.dll` back to `ddraw.dll`.

If the game stops with *ddraw_orig.dll is missing or broken*, step 2 was skipped: verify the game files
in Steam (that brings the stock `ddraw.dll` back), then do steps 2 and 3 again.

## Please read before using

- The cheats change what the ending shows and ranks you on (clear time, saves), and a save made while
  cheating records the game as it was - back up the `SAVE` folder inside `english` before editing
  counters or inventories.
- **Deleting or copying a save file cannot be undone**: the file in `SAVE` changes the moment you
  confirm. Back that folder up first if in doubt.
- *Save without counting* edits the save file the game has just written - one byte, its save count -
  so that loading it gives back the count you had.
- God mode tops your health up every frame; a hit that takes all of it at once can still kill you.
- One hit kills changes boss fights: a boss whose fight is scripted around its health may end sooner.
- The inventory and the item box are the game's own: a change is kept by your next save and lost, like
  anything else, if you quit without saving - an item thrown away is gone for good once you save. They
  change only while the option screen is up, and a change the mod cannot make safely is refused, with
  the reason in the panel.
- The mod takes the game's `ddraw.dll`, so it cannot be combined with another mod that replaces
  `ddraw.dll`.
- Only the English version (Steam build 21744136) has been tested. Every piece of game code the mod
  uses is checked before it is touched, so in a build where something cannot be found, that feature is
  greyed out with the reason beside it - or, if the screens themselves are not found, the panel does not
  appear - and the log says what was missing.

## Config

`RE1-1996CabbyCodes.ini` is created next to the DLL on first run:

| Key | Default | Meaning |
| --- | --- | --- |
| `ToggleKey` | `0x76` (F7) | virtual-key code that hides/shows the panel |
| `GodMode`, `OneHitKills`, `InfiniteAmmo`, `InfiniteInkRibbons`, `NoSaveCount`, `FreezePlaytime`, `FreezeCountdown` | `0` | cheats switched on when the game starts |
| `AlwaysShow` | `0` | debug: draw the panel on every screen, not only on its two |
| `Trace` | `0` | verbose diagnostics in `RE1-1996CabbyCodes.log` |
| `DumpImage` | `0` | write the unpacked game image beside the DLL once (for reverse engineering) |
| `Watch` | | debug: `0xBE41C0:4,0xD22777:1` logs those values as they change |
| `Disable` | | comma list of subsystems to turn off: `overlay,dispatch,game,gpa,input,cursor` |

`RE1-1996CabbyCodes.log` beside the DLL records what the mod found and did, and the run before it is
kept as `RE1-1996CabbyCodes.prev.log` - attach both when reporting a problem.
`RE1-1996CabbyCodes.imgui.ini` remembers where you left the panel.

## How it works

The mod ships as a proxy `ddraw.dll` in front of the one the game ships (GOG's DirectDraw → Direct3D 9
wrapper, renamed `ddraw_orig.dll`): every export is forwarded untouched through generated jump thunks,
so it loads with the game and needs no injector. The Enigma Protector-wrapped `ResidentEvil.exe` is
never modified on disk; the mod waits for the wrapper to unpack the game in memory, then finds
everything it uses by short byte signatures, each checked where it matches, with the game's variables
read out of the matched instructions and cross-checked between sites. Nothing is a fixed address. The
cheats are holds (your health, the enemies' HP) and small in-memory patches of the code they change
(the two ammo decrements, the typewriter's ribbon check and the save's ribbon take, the save counter,
the play-time and countdown clocks), each put back when its switch goes off. Everything that touches
the game runs on the game's own thread, from its message loop. The inventory and the item box are the
game's own save data, changed only while the option screen has the game paused; the one game function
the mod calls is the inventory screen's icon rebuild, run once after the option screen closes if the
panel changed the inventory. The save file manager works on the files in `SAVE` and keeps the Load
Game list's own table of them in step, so the list never offers a file that is gone. The panel is Dear
ImGui, drawn into the Direct3D 9 swap chain GOG's wrapper presents the game through. See `CLAUDE.md`
for the reverse-engineering record.

The reverse engineering leaned on ecruells'
[resident-evil-pc-decomp](https://github.com/ecruells/resident-evil-pc-decomp), a decompilation of
this very executable, and on the fearlessrevolution Cheat Engine tables and the RE1 LiveSplit
autosplitter.

## Building from source

Linux with mingw-w64 (`i686-w64-mingw32-g++`); no Windows or MSVC needed.

```sh
cp config.mk.example config.mk   # set GAME_DIR (the game's english folder)
make                             # build/ddraw.dll
make install                     # deploy into GAME_DIR (renames the stock DLL once)
make uninstall                   # put the stock DLL back
make version 1.1.0               # set the version (VERSION file, baked into the DLL)
make package                     # dist/RE1-1996CabbyCodes_v<version>.zip
make proxy                       # regenerate the export list from the stock DLL
```

`tools/` holds the reverse-engineering helpers - fixing up a dumped game image, checking every
signature against it, Ghidra scripts - described in `CLAUDE.md`. Dear ImGui (MIT) is vendored under
`contrib/imgui`. Licensed under the GPL-3.0; see `LICENSE`.
