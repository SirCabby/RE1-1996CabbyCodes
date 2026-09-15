[size=6][b]RE1-1996CabbyCodes[/b][/size]

A [b]cheat panel[/b] for Resident Evil (1996) on Steam: open the game's option screen while playing (A on the keyboard by default) and tick what you want - God mode, One hit kills, Infinite ammo, Infinite ink ribbons, Save without counting, save count, play time and self-destruct countdown editors, an inventory editor, and the game's own item box, from anywhere. On the title screen's Load Game list it also lets you delete a save file or copy one onto another slot.

This mod is open source! Check it out at [url=https://github.com/SirCabby/RE1-1996CabbyCodes]https://github.com/SirCabby/RE1-1996CabbyCodes[/url]

[size=5][b]What it does[/b][/size]
[list]
[*][b]God mode[/b] - your health held at its maximum and poison cured every frame.
[*][b]One hit kills[/b] - every live enemy is held at 0 HP, the last value the game counts as alive; your next hit kills it.
[*][b]Infinite ammo[/b] - guns keep their loaded rounds, the flamethrower its fuel; pickups and reloads still work.
[*][b]Infinite ink ribbons[/b] - typewriter saves never use up a ribbon, and you can save without carrying one at all.
[*][b]Save without counting[/b] - the save count the ending shows is not incremented.
[*][b]Save count / Play time[/b] - edit the counter and the clock; freeze the clock.
[*][b]Countdown timer[/b] - freeze or set the self-destruct countdown while it is running.
[*][b]Inventory editor[/b] - every slot of the character you are playing (Chris, Jill or Rebecca): any item, with any count the game allows - a gun's loaded rounds, up to 250 of an ammo, a key's remaining uses. Type to narrow the item list. The equipped weapon can be swapped too: the game equips whatever weapon is in that slot when the option screen closes.
[*][b]Item box[/b] - the game's own 48-slot item box, saved with your game, from anywhere: store an inventory item in it, take one out or throw one away. Ammo and ink ribbons are combined both ways into stacks of up to 250 - storing a few rounds joins the stacks already in the box, and taking them out tops up the stack you carry. The box is listed under five tabs - key items, weapons, ammo, heals and other - each sorted by name.
[*][b]Save file manager[/b] - on the title screen's Load Game list: all eight save files with their character, play time and save count. [b]Delete[/b] removes a file (it shows NO DATA, like a file that was never used); [b]Copy to...[/b] copies a save into any other file, empty or not. Both ask first and change the SAVE folder at once, and the game's list shows the change straight away.
[*][b]F7[/b] hides or shows the panel. Start-up defaults live in the ini.
[/list]

[size=5][b]Install[/b][/size]

Copy into the game's [b]english[/b] folder - the one with ResidentEvil.exe (in Steam: right-click the game > Manage > Browse local files):
[list=1]
[*]Rename the stock [b]ddraw.dll[/b] to [b]ddraw_orig.dll[/b]
[*]Drop this mod's [b]ddraw.dll[/b] in its place
[/list]

That is the whole install, on [b]Windows and Linux/Proton alike[/b] - no launch options, no WINEDLLOVERRIDES: Proton already loads the game's ddraw.dll natively.

To uninstall: delete ddraw.dll and rename ddraw_orig.dll back.

[size=5][b]Please read before using[/b][/size]
[list]
[*][b]The cheats change what the ending shows and ranks you on[/b] (clear time, saves). Back up the SAVE folder in the english folder before editing counters or inventories.
[*][b]Deleting or copying a save file cannot be undone[/b] - the file changes the moment you confirm. Back up the SAVE folder first if in doubt.
[*]One hit kills can cut short a boss fight that is scripted around the boss's health.
[*]Tested with the English version, on Linux/Proton.
[*][b]Steam's "Verify integrity of game files" removes the mod[/b] by restoring the stock DLL. Just re-copy the file.
[/list]

[size=5][b]Config[/b][/size]

[code]
ToggleKey          = 0x76  ; virtual-key code that hides/shows the panel (0x76 = F7)
GodMode            = 0     ; cheats switched on when the game starts
OneHitKills        = 0
InfiniteAmmo       = 0
InfiniteInkRibbons = 0
NoSaveCount        = 0     ; save without incrementing the save counter
FreezePlaytime     = 0
FreezeCountdown    = 0     ; stop the self-destruct countdown
[/code]

Lives in [b]RE1-1996CabbyCodes.ini[/b] next to ResidentEvil.exe (created on first run). [b]RE1-1996CabbyCodes.log[/b] beside it records what the mod found and did - attach it when reporting a problem.
