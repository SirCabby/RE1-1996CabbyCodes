# RE1-1996CabbyCodes — project guide

A client-side mod for **Resident Evil (1996)** — Steam app 4249100 (`4249100_Biohazard`), the 1997 PC
`ResidentEvil.exe` wrapped by Enigma Protector, drawn through GOG's DirectDraw → Direct3D 9 wrapper —
cross-built on Linux with mingw-w64. It draws a Dear ImGui panel beside the game's **option screen**
with God mode, One hit kills, Infinite ammo, Infinite ink ribbons, Save without counting, save-count /
play-time / self-destruct-countdown editors, an **inventory editor** for the character in play and the
game's own **item box** (store, take, throw away), and on the **title menu** a save-file manager that
deletes a save file or copies one over another. `README.md` is the user-facing doc; this file records
**what was reverse-engineered**. It is a sibling of `../RE0CabbyCodes` (same architecture: proxy DLL,
IAT + vtable hooks in `src/mem.h`, a main-thread tick, ImGui from the D3D9 device). RE0's inventory
editor and storage box are here too, reworked: RE0 kept its box in a file of the mod's own, and RE1's
is the game's item box itself, which is saved with the game.

## Build & deploy

```sh
make            # -> build/ddraw.dll   (config.mk sets GAME_DIR = .../4249100_Biohazard/english; gitignored)
make install    # rename stock ddraw.dll -> ddraw_orig.dll (once), deploy ours atomically
make uninstall  # restore the stock DLL
make version X.Y.Z   # set the version;  make package -> dist/RE1-1996CabbyCodes_vX.Y.Z.zip
make proxy      # regenerate ddraw.def + src/proxy_exports.inc from the stock DLL's export table
python3 tools/check_sigs.py   # every signature game.cpp uses, against the dump, before a launch
# reverse engineering (DumpImage=1 in the ini writes ResidentEvil.dumped.exe + ResidentEvil.imports.txt):
python3 tools/fix_dump.py --dump "$GAME_DIR/ResidentEvil.dumped.exe" --out ResidentEvil.fixed.exe
MAXMEM=8G /opt/ghidra/support/analyzeHeadless ghidra_proj RE1 -import ResidentEvil.fixed.exe -overwrite -loader PeLoader
MAXMEM=8G /opt/ghidra/support/analyzeHeadless ghidra_proj RE1 -process ResidentEvil.fixed.exe -noanalysis \
    -scriptPath tools/ghidra -postScript DecompileAddrs.java 0x4815F0   # decompile a reading list
i686-w64-mingw32-objdump -d -M intel --start-address=0x4815F0 --stop-address=0x481660 ResidentEvil.fixed.exe
```

The log (`RE1-1996CabbyCodes.log`, the run before it in `.prev.log`), the ini
(`RE1-1996CabbyCodes.ini`) and the ImGui layout file sit beside the DLL in the game's `english` folder.
`Trace = 1` adds diagnostics (every name Enigma resolves through the exe's GetProcAddress slot, state
transitions); `AlwaysShow = 1` draws the panel on every screen; `Watch = 0xBE41C0:4,...` logs values
as they change; `Disable = overlay,dispatch,game,gpa,input` bisects a fault. Keep them on only for a
diagnostic run and reset them after.

## ⛔ Rules

- **Never patch `ResidentEvil.exe` on disk.** It is Enigma-wrapped, its code is compressed on disk, and
  the wrapper unpacks it in memory after our `DllMain` has run. `DllMain` only loads the original
  wrapper, patches import slots and starts a thread; nothing reads game memory before the first
  present (the game's own code is drawing) and the prologue check in `mem::text_looks_decrypted`.
- **Never use absolute addresses in code.** Everything is found at run time by byte signature
  verified at the match, with globals read out of the matched instruction's operands and cross-checked
  between sites; the VAs in this file document build 21744136 (English) only. (`Watch` is a diagnostic
  that reads addresses typed into the ini; nothing the mod does depends on it.)
- **Game memory only from the main thread** (the D3D9 present hook's tick, `dispatch.cpp`). The
  panel flips atomics and posts requests; the tick applies them.
- **Never call a game function** - with one exception the user approved on 2026-09-13: the inventory
  screen's icon rebuild, `LoadHeldItemsImages`, run once from the tick after the option screen closes
  when the panel changed the inventory (see Inventory and item box). Every other feature is a field
  write, a hold, or a byte patch of a site whose bytes were verified first (`mem::Patch::prepare`);
  patches are reverted on toggle-off and unload. Do not add a second call without asking.
- **Never write the player entity, or the equipped-slot byte, while the option screen is up.** The
  option screen copies both away, reuses them for its background model and copies them back when it
  closes. The panel writes the screen's own saved copy of the equipped byte instead.
- Back up `english\SAVE\` into `.save-backup-<date>/` (gitignored) before experiments that write saves.
- The user commits every repo themselves — do not `git commit`/`push` unless asked.

## Why ddraw.dll

The Steam exe is Enigma Protector-wrapped: its on-disk import table holds only the wrapper's loader
imports (kernel32 `GetModuleHandleA/GetProcAddress/ExitProcess/LoadLibraryA`, `user32!MessageBoxA`, ...)
plus one function per original DLL (`DDRAW!DirectDrawCreate`, `DSOUND!DirectSoundCreate`,
`WINMM!waveOutGetNumDevs`, `MSACM32!acmMetrics`, `IMM32!ImmAssociateContext`). Of those DLLs only
`ddraw.dll` ships with the game (GOG's wrapper), and **Proton loads a native ddraw.dll for this game
on its own**: Proton Experimental's `proton` script sets `dlloverrides["ddraw"] = "n,b"` for 4249100
(with Arcanum, RE2 1998, Dino Crisis and Breath of Fire IV), for GOG's wrapper. `version`, `winmm`,
`dsound` or `msacm32` would need `WINEDLLOVERRIDES`. GOG's `ddraw.dll` (base 0x18000000, 2024-07-04)
imports `d3d9.dll!Direct3DCreate9` by name (no D3D9Ex), creates no threads (timeSetEvent only paces
frames), has 14 named exports (the 7 unnamed ordinals are empty) and never refers to itself by name, so
it runs renamed as `ddraw_orig.dll`; `tools/gen_proxy.py --from-dll` thunks all 14.

## What the game does

Sources: https://github.com/ecruells/resident-evil-pc-decomp (GPL-3.0; a C++ decompilation of this
exact 1997 binary - 1716 of 1717 functions - with every original function and global address), the
fearlessrevolution Cheat Engine tables t=29932 (GOG) and t=38880 (Steam), and the RE1 LiveSplit
autosplitter (`RE1aio.asl`). Everything below is a lead until a signature in the dump confirms it.

### Executable
- Enigma keeps the 1997 image's sections at their original addresses - code 0x401000, `.rdata`
  0x4AF000, `.data` 0x4B1000, `.idata` 0xD93000, `.rsrc` 0xD95000, `.reloc` 0xD97000 - with blank
  names, and appends its own from 0xDB2000 (entry 0x53A991C, SizeOfImage 0x53AD000). Image base
  0x400000, no ASLR. The game's import table (`.idata`) is rebuilt at run time.
- No threads: a cooperative task scheduler switches stacks on the main thread. The frame goes
  WinMain's pump (0x441350: `PeekMessageA`/`GetMessageA`) → `main_loop` 0x428EB0 → `FrameRateGovernor`
  0x4973D0 → the renderer's Present (vtable 0x4AF230 slot 4, 0x448FF0) → DirectDraw → GOG's wrapper →
  `IDirect3DDevice9::Present`, so the device's Present hook runs on the game's thread once per frame.
- Input: the keyboard is polled with `GetAsyncKeyState` (0x4202F0); the pad through winmm
  `joyGetPosEx`; the window procedure (0x441170) acts on F1 and F9 only; no mouse.

### Screens
- **Option screen** `options_menu` (0x4761B0) runs as task 1 with the gameplay task suspended,
  launched by `check_menus_state` (0x4815F0, `push 0x4761B0` at 0x481621). It opens on the pad word
  0x900 = "Key Def" slot 27 (`A` by default). While it is up: bit 0x8000 of `g_main_state_flags`
  (0xBE41C0), `g_loadSaveStateFlag` (0x4D4678) = 1, `g_bGameActive` (0xBE41DC) != 0 (the inventory sets
  it to 0). It copies the player entity (0x180 bytes at 0xBE62E4) and the equipped-slot byte
  `g_EquippedItemId` (0xBE9849) into its own frame, puts a 1 in the byte, masks the entity's character
  byte to `id & 1` (Rebecca reads as Jill) and gives the entity a Beretta for its background model.
  As it closes (state 4) it copies both back, then calls `menu_update_equipped_weapon` (0x463EC0) and
  `LoadEquippedWeaponAnimation` (0x462620): **the player is handed whatever item the restored slot
  holds** (see Inventory and item box).
- **Title menu**: `g_titleLoopFlag` (0xD22777) = 1 while NEW GAME / LOAD GAME runs.
- **Load list**: `LoadSaveGameState` (0x493310) sets `g_loadSaveStateFlag` on entry (the in-game
  typewriter runs it too), so the title's list is up while that flag is 1, no game runs and the title
  loop flag is 0 (`game::load_list`). It reads `SAVE\savedat1..9.dat` once, in `STATE_INIT`, into a
  table in its own frame - 9 x {character byte, saves, stage, room, has-data} dwords - draws its rows
  from that table every frame, and trusts it on a load: the load reads the chosen file and copies it
  into the save block without checking the read. The save-file manager therefore finds the table by
  content in task 0's stack (just above the task's saved ESP, `g_TasksESP` 0xD91A70, read out of the
  task switch at 0x475788: `A1 <current> 89 25 <scheduler esp> 8B 24 85 <saved esps> 90 C3`, its
  current-task operand checked against `Task_chain`'s) and writes each change into it; without it,
  Delete is refused (a deleted file would still be offered and "loaded").

### Overlay, tick and input (confirmed in play, 2026-09-13)
- GOG's wrapper presents through **`IDirect3DSwapChain9::Present`** of the implicit swap chain, never
  `IDirect3DDevice9::Present` (10-60/s; 2-3 `EndScene` per present), on the game's thread. The panel
  is drawn there, into that swap chain's back buffer.
- Enigma leaves the **real API addresses** in the rebuilt `.idata` (161 slots, all exports), so the
  tick re-points the game's `PeekMessageA` slot by value once the code is unpacked (as RE0 hooks its
  IAT); `GetAsyncKeyState` (the input guard) and `SetUnhandledExceptionFilter` likewise.
- The wrapper **confines the cursor to the 4:3 picture** with `ClipCursor` (1440x1080 at x 240 in a
  1920x1080 window; a panel clamped to `GetClipCursor`'s rectangle ended exactly at x 1680). Its own
  code clips at six sites (the picture rect through `ClientToScreen`) and releases at others (one on
  deactivation). The *game's* user32 imports it hooks, in the exe only (it imports nothing that
  enumerates modules, so the mod's own user32 calls are real): `ClipCursor` becomes a no-op;
  `GetCursorPos`/`SetCursorPos` and a `WH_MOUSE` hook proc are rescaled to 640x480
  (ddraw_orig.dll 0x1804c1c0/0x1804c470/0x1804e310). The game uses none of them. The mod hooks the
  **wrapper's own `ClipCursor` import** and **lets the pointer go altogether while the panel is on
  screen** (since 2026-09-13, at the user's request; before that it only widened the clip to the
  window), so the player can reach the panel in the bars and use other windows and monitors. A clip
  the wrapper sets meanwhile is remembered and answered with a release, its releases pass through,
  and its last clip is put back when the panel goes - F7 hiding it included; `Disable = cursor`
  leaves the wrapper's clip in place throughout. The wrapper releases on deactivation, so a clip is
  only ever put back while the game is the active window. The log says whether anything else still
  holds the pointer once it is let go (Wine can confine a window that covers the whole desktop - with
  one monitor there is nowhere else to go anyway), and the panel is kept inside `GetClipCursor`'s
  rectangle every frame for that case. Clicking another window pauses the game - WinMain calls
  `main_loop` only while its window has focus - and the D3D9 device is fullscreen (`Windowed` FALSE),
  so the game window may minimise until it is clicked again.
- **gamescope holds the pointer on its own** (2026-09-13: the log said the pointer was let go, and it
  was still held). The user runs the game in gamescope 3.16.25 - the Steam launch options go through
  `~/.local/bin/gamescope-launch`, nested in KDE Wayland at `-W 1600 -H 900` - which uses relative
  mouse mode, the pointer locked to its window, whenever the focused window's cursor is hidden
  (`--force-grab-cursor` makes that permanent, and then nothing inside the game can let it go). The
  game hides its cursor once, as its renderer starts full screen (`ShowCursor(FALSE)` at 0x497230; its
  window class has the arrow; its other `ShowCursor` calls are the pair around its message boxes,
  0x497EED/0x497EF7, one at exit, 0x441FB1, and two at 0x49778E/0x4977B4), and the panel used to draw
  ImGui's own cursor, which `SetCursor(NULL)`s the real one. So while the panel is up the mod shows the
  game's cursor - counted `ShowCursor(TRUE)` calls until the display count is back at 0, looked at
  every frame - lets ImGui set its shape instead of drawing one, and takes every call back when the
  panel goes (`Disable = cursor` keeps the drawn cursor and the wrapper's clip).

### Save files
`SAVE\savedat%d.dat` (1..8) relative to the working directory, 0xA82 bytes, no checksum; bytes
0..0x1FF are a constant PS1 memory-card header (`data\bio_card.dat`); from 0x200 the file maps 1:1 onto
memory at 0xBE9620: 0x200/0x201 stage/room, 0x224 play time (u32, 30/s), 0x228 save count, 0x22B
character (`& 3`: 0 Chris, 1 Jill).

### Inventory and item box (src/inventory.cpp; game.cpp steps 11-12)
Everything lives in the save block, so every save writes it and every load brings it back. A slot is
{item id, count}, a byte each.

| what | where (save offset) | notes |
| --- | --- | --- |
| item box | 0xBE98E4 (+0x2C4) | 48 slots, directly in front of the inventory: `SetupCharacterData` (0x494FC0) itself reads the inventory as `g_itemboxSlots[E + 47]` |
| main inventory | 0xBE9944 (+0x324) | 6 slots for Chris, 8 for Jill |
| Rebecca's inventory | 0xBE9950 (+0x330) | 6 slots; Jill's slots 7-8 are its first two |
| slot pointer | 0xD22768 | the inventory in play; set only to the two arrays above (room init's Rebecca switch 0x477908/0x47793F, `InitializeGame` 0x4809C5) |
| held count | 0xBE9827 (+0x207) | the inventory is **packed**: the first `held` slots hold items, the rest are empty |
| equipped slot | 0xBE9849 (+0x229) | 1-based, 0 = none; the ammo decrement is `dec byte [slots + E*2 - 1]` |
| icon rows | 0xD21CD0 (8 bytes), bits 0xD22734 (a byte) | the inventory screen's icon sheet row of each held slot |

- **Packing** is `rearrange_item_slots` (0x451510): items move down over empty slots in order, each
  taking its icon row along, an emptied slot gives its row back, the equipped slot follows its weapon,
  `held` is what is left. The slot count is `(4 - ((id & 3) != 1)) * 2` of the entity's character
  byte (0xBE62E5): 8 for Jill, 6 for Chris and Rebecca. A new item goes into slot `held` with the
  first free icon row (the pickup, 0x451852, and the item box both do this). Many readers walk only
  `[0, held)` (`get_item_slot` 0x4516A0, the inventory screen), so the mod keeps the same invariant
  (`inventory.cpp` `compact`/`add_slot`).
- **The item box screen** (`menu_itembox_interaction`, 0x4941F0 in `main_menu` mode 2) swaps one box
  slot with one inventory slot - unequipping if that was the equipped slot - and packs; it never
  merges. The box keeps its gaps and its order; its cursor wraps 0..0x2F. So a box the game has been
  using holds a separate stack for every time a kind of ammo went in. **The panel combines**
  (2026-09-13, at the user's request): a store of ammo or ink ribbons joins every stack of that item
  in the box and lays the lot out again - full stacks of 250 and at most one short one, over the
  slots the item already had and then the first free ones, emptying any it no longer needs - and a
  take re-combines what it leaves of that item (`inventory.cpp` `combine`). A take into the inventory
  stays the pickup's rule: top up the stacks carried, then one free slot.
- **Item rules**, all read out of the code: the item table 0x4BD81C holds a 4-byte record per id up to
  the radio (0x4D) - {per-slot maximum, icon, combine index, unexamined-name category}. The combine's
  reload helpers (0x401BF0/0x401C60) cap a gun at its maximum: Beretta 15, Shotgun 7, both Colt Pythons
  and all three Bazookas 6, Flamethrower 240, Rocket Launcher 4. The pickup (0x4517AB in
  `room_event_item_pickup` 0x451700) stacks ids 0x0B..0x12 (ammo) and 0x2F (ink ribbons) into a slot
  already holding them up to **250** (0xFA), the overflow into a new slot; everything else takes a slot
  of its own. A gun's count carries a flag in bit 0x80 (the hold-fire gate, 0x45A550), which the
  count display (0x4645B0) and the fire gate (0x45A4B0) mask - all but the flamethrower. Ids above 0x6E
  (the Ingram 0x6F, the Minimi 0x70) are infinite: the display draws the infinity sign and the fire
  gate tops their slot up to 4. Names: 0x4BF0A0 (by id - 1); an item with a category shows a generic
  name until examined (0x4BF260: 0x38 CLOSET KEY reads SPECIAL KEY, the four mansion keys MANSION KEY,
  ...). The mixed herbs by the combine table (0x4BD768) and the heal table (0x4BD927): 0x46 green+red,
  0x47 green+green, 0x48 green+blue, 0x49 green+red+blue, 0x4A green x3, 0x4B green x2+blue.
- **Every character can hold every weapon**: `LoadEquippedWeaponAnimation` indexes a 14-entry file
  table per character (Chris, Jill, an unused one, Rebecca - 0x6F/0x70 map to entries 0xC/0xD), and
  every file it names is in `USA\Players` (`W30`/`W32` are Rebecca's own, the rest shared).
- **The equipped weapon and the option screen.** `options_menu`'s frame (`sub esp,0x1B0` and four
  pushes; offsets from the ESP after them): the three sub-menu handlers at +0x14 (0x452C50, 0x451960,
  0x453A80), the camera matrix it clears at +0x20 (eight dwords, `m[1][1]` = 5000 at +0x28), the saved
  entity at +0x40 (copy 0x476398, copy back 0x47656F) and the **saved equipped byte at +0x12**. Task 1
  is entered with no return address at the top of its stack, so the tick finds the frame just above
  task 1's saved ESP (`g_TasksESP[1]`) by the handler triple and the matrix, requires it unique, and
  requires it to agree with the equipped byte and the character the game had just before the screen
  opened (`game::find_options_frame`, `inventory.cpp` `find_frame`). Every move follows the game's
  rules for that byte: it follows its weapon through packing, becomes 0 when the slot is emptied or
  given something that is not a weapon, and stays when a weapon replaces a weapon - the screen equips
  whatever the slot holds when it closes. A move that would change the byte is refused when the
  frame was not found, and so is anything that would leave a non-weapon in the equipped slot (the
  close would "equip" it and load an animation file that does not exist).
- **Icons.** `LoadHeldItemsImages` (0x451640) calls `CountHeldItems` (0x451600), numbers the rows
  `0..held-1` and draws each slot's item into its row with `LoadItemImage` (0x443000) → `LoadImage`
  (0x46D3B0), which locks a DirectDraw texture surface and copies pixels - nothing a field write can
  reproduce. The game calls it on a pickup (0x4518B9), on the Rebecca switch (0x477956) and in
  `InitializeGame` (0x4809F4), and nowhere else, so an item the panel put in the inventory would keep
  a stale picture until the next pickup or load. The mod runs it once, from the tick, when the option
  screen closes after the panel changed the inventory (the one game call, see Rules), verifying its
  bytes and `CountHeldItems`' again first. The timing is safe by the scheduler: WinMain's pump calls
  `PeekMessageA` (our tick) before every `main_loop`; the screen closes inside task 1 in frame N
  (`Task_Resume(0)`, `Task_exit`), and task 0 next runs in frame N+1, after our tick.

Signatures (all unique in the dump; `tools/check_sigs.py` runs them with their cross-checks):

| what | signature | yields |
| --- | --- | --- |
| `rearrange_item_slots` | `83 EC 04 A0 <eq> FE C8 B1 04 88 44 24 02 53 56 A0 <char> 24 03 3C 01 0F 95 C2 ...` | the count, the icon rows and bits, the character byte; the slot pointer and equipped byte must be the ammo decrement's |
| slot arrays | `C7 05 <slot pointer> imm32`, every match | exactly two values, 12 bytes apart: the main inventory and Rebecca's |
| item box swap | `8A 0D <eq> 8A D8 2B CB 83 F9 01 75 07 C6 05 <eq> 00 ... 8A 14 4D <box> ...` | the box; its size is where its cursor wraps (`C6 05 <cursor> 2F`) + 1, which must be the room up to the inventory |
| `LoadHeldItemsImages` | `53 E8 <CountHeldItems> 8A 1D <held> B0 01 8A CB D2 E0 FE C8 84 DB A2 <bits> ...` | the icon rebuild; `CountHeldItems` is matched in full from the globals found |
| the combine's cap | `66 83 E3 7F 66 03 D3 33 DB 8A 19 0F B7 FA 8A 0C 9D <table> 33 DB 8A D9 3B DF 73 12` (2 matches, must agree) | the item table; the icon column `LoadHeldItemsImages` reads must be one byte on |
| pickup stacking | `80 3D <sel> 0B 72 09 80 3D <sel> 13 72 0D 80 3D <sel> 2F 0F 85 ... 66 81 FA FA 00 76 18` | which items stack and how far |
| `options_menu` | prologue at the address `check_menus_state` gives; save `8D 4C 24 44 83 C4 04 68 80 01 00 00 68 <entity> 51 E8 .. 83 C4 0C 8A 0D <eq> C6 05 <eq> 01 88 4C 24 12`; restore `8D 44 24 40 68 80 01 00 00 50 68 <entity> E8 .. 8A 44 24 1E 83 C4 0C A2 <eq> E8` | the handler triple and the frame offsets; save and restore must name the same two frame slots |

## Dead ends worth not repeating
- **Enigma's import table has no OriginalFirstThunk.** Every descriptor of the wrapped exe's on-disk
  table has OFT 0, so after the loader has bound it the FirstThunk slots hold addresses, not name
  RVAs. RE0's `iat_hook` then read a function address as a name pointer inside DllMain, and Wine fails
  a process whose DllMain faults: the game "crashed immediately" (2026-09-13, both launches, the log
  ending at the Direct3DCreate9 hook line). `mem::iat_slot` now matches an OFT-less table's slot by the
  address the named export resolves to, and range-checks everything it reads out of any table.
- **Plain Wine is not a test bed past DllMain.** A scratch copy of `english\` under system Wine (a
  headless gamescope, the launcher's registry keys imported) loads the mod and runs Enigma's first
  loader stage - its Delphi runtime resolves ~540 names through the hooked GetProcAddress slot - but
  the game never reaches `Direct3DCreate9`, with or without the mod (`WINEDEBUG=trace+d3d9`: 0 lines).
  In-game behaviour is only checked in the user's Steam/Proton session. What Wine can run is the mod's
  own modules against a fake game side: `savefiles.cpp` against a scratch `SAVE\` folder, and
  `inventory.cpp` against a fake save block and option-screen frame (2026-09-13, every move and
  refusal checked).
- **The equipped byte cannot be written while the option screen is up.** It holds the screen's
  stand-in 1, and the screen's copy overwrites it as it closes; the saved copy in its frame is the one
  to write.

## Tooling notes
- x86 encodes small displacements in one byte: write patterns from the raw bytes (`objdump -d` on the
  fixed dump), never from mnemonics.
