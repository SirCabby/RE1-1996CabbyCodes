#pragma once

#include <cstdint>

#include "mem.h"

// The game side: what the mod knows about ResidentEvil.exe at run time. The
// exe is Enigma Protector-wrapped, so nothing is read until the wrapper has
// unpacked it; after that everything is found by signature in the 1997 code
// (tools/check_sigs.py runs the same signatures against the dump), with each
// global read out of the matched instruction's operands and cross-checked
// between sites. Nothing here is an absolute address.
namespace re1cc::game {

// --- the unpack gate, recon and discovery (mod thread) -----------------------------
bool unpacked(int* prologues, int* pads);  // Enigma has decompressed the game's code
void recon(const char* dir);               // log the layout, the rebuilt import table and the save folder
bool dump_image(const char* dir);          // the unpacked 1997 image beside the DLL, plus an import map
void note_iat_slot(uintptr_t slot, uintptr_t original);  // so a dump carries the game's values, not our hooks
bool discover();                           // every signature; ready() once the screens are known
bool ready();
const char* status_text();

// --- the game's import table ------------------------------------------------------
// Enigma rebuilds it at run time with the real API addresses and its names are
// gone, so a slot is found by the address it holds: every slot in .idata
// holding `real` is re-pointed to `hook`. Returns how many changed.
int hook_import(const char* what, void* real, void* hook);

// --- screens (main thread) ------------------------------------------------------------
// The game runs its screens as cooperative tasks: task 0 is the title and then
// the game (Task_chain puts game_start there), task 1 the menus launched over
// it (check_menus_state puts options_menu or main_menu there).
bool in_game();       // task 0 runs game_start: a game is on (its menus included)
bool options_open();  // task 1 is running options_menu: the option screen is up
bool title_menu();    // the title's NEW GAME / LOAD GAME menu is up
bool load_list();     // the title's Load Game list is up (LoadSaveGameState, no game running)
// The saved stack pointer of a suspended task (ticks run between frames, when
// every task is suspended), or 0. The Load Game list keeps its table of the save
// files in task 0's stack, just above this.
uintptr_t task_saved_esp(int id);
bool player_dead();   // the menu flags' dead bit
void trace_tick();    // Watch values (main thread only)

// --- the player (the entity the health bar reads) ---------------------------------
// The option screen reuses the player entity for its background model and
// copies it back when it closes: nothing may write it while options_open().
struct Player {
  int hp = 0;         // s16; the player dies when it drops below 0
  int max_hp = 0;     // 140 Chris, 96 Jill, 88 Rebecca
  int status = 0;     // 0x02 poison, 0x20 Yawn's poison, 0x40 the fast drain
  int character = -1; // the model byte: & 3 = 0 Chris, 1 Jill
};
bool player(Player* out);
bool set_player_hp(int hp);
bool set_player_status(int status);

// --- enemies (30 slots) ----------------------------------------------------------------
struct Enemy {
  int index = -1;
  int id = -1;     // the enemy type, below 0x14 (0x14 and up are NPCs)
  int state = 0;   // 3 = dead
  int hp = 0;      // an enemy dies when a hit takes it below 0
};
bool enemies_known();              // the slots were found
int enemies(Enemy* out, int max);  // the active enemy slots (NPCs left out)
bool set_enemy_hp(int index, int hp);

// --- counters -------------------------------------------------------------------------
bool counters_ready();
int save_count();                  // raw: the saves made + 1 (a new game starts at 1), -1 unknown
bool set_save_count(int raw);
int typewriter_state();            // 3 while the save screen runs, -1 unknown
uint32_t game_timer();             // the play time, 30 per second
bool set_game_timer(uint32_t v);
bool countdown_known();
bool countdown_active();           // the self-destruct sequence is running
int countdown_elapsed();           // seconds of it gone (0..180), -1 unknown
bool set_countdown_elapsed(int s);
constexpr int kCountdownLimit = 180;  // the game's own limit (game_loop compares against it)

// --- the inventory and the item box ------------------------------------------------------
// Both live in the save block, so every save writes them and every load brings
// them back. A slot is {item id, count}, a byte each. The item box is 48 slots,
// directly in front of the inventories; the inventory in play is the array the
// game's slot pointer names: the main character's (6 slots for Chris, 8 for
// Jill) or, while Chris's partner is played, Rebecca's 6. The game keeps it
// packed - the first `held` slots hold items and the rest are empty
// (rearrange_item_slots) - and every held slot owns a row of the inventory
// screen's icon sheet: `row` says which, `rows_used` has a bit per row taken.
struct ItemSlot {
  int id = 0;
  int count = 0;
};
constexpr int kMaxBagSlots = 8;
struct Bag {
  bool rebecca = false;  // the slot pointer names Rebecca's array
  int held = 0;          // slots in use (the game's own count)
  int equipped = 0;      // the equipped-slot byte, 1-based (0 = none); the option screen puts a 1 there while it is up
  ItemSlot slot[kMaxBagSlots];
  int row[kMaxBagSlots] = {};
  int rows_used = 0;
};
bool items_known();                      // the inventories, the item box and the icon bookkeeping were all found
int box_size();                          // the item box's slots (48), 0 until known
bool read_box(ItemSlot* out, int n);     // the first n box slots
bool write_box_slot(int i, int id, int count);
bool read_bag(Bag* out);                 // the inventory in play (all eight slot pairs; Chris and Rebecca use six)
bool write_bag(const Bag& b, int size);  // slots and icon rows [0, size), the count, the row bits - never the equipped byte
int character_id();                      // the player's character byte & 3 (0 Chris, 1 Jill, 3 Rebecca); the option screen masks it

// The game's own item rules, read out of its code (items.h stands in for any
// that were not found).
bool item_rules_known();
int item_max(int id);      // how many rounds a gun takes (the combine's table), -1 when not known
bool item_stacks(int id);  // a pickup of this item tops up a slot already holding it: ammo, ink ribbons
int stack_cap();           // ...up to this many in one slot (250)

// The option screen keeps a copy of what it borrows in its own frame, at the
// top of task 1's stack: the player entity and the equipped-slot byte, both put
// back when it closes, after which it hands the player whatever weapon is in
// that slot. While the screen is up that saved byte is the only equipped slot
// there is.
struct OptionsFrame {
  uintptr_t equipped = 0;  // the saved equipped-slot byte
  uintptr_t entity = 0;    // the saved player entity (0x180 bytes)
};
bool options_frame_known();                  // its layout was derived from the code
bool find_options_frame(OptionsFrame* out);  // main thread, while options_open(); false when not found, or not unique
int saved_equipped(const OptionsFrame& f);   // -1 when unreadable
bool set_saved_equipped(const OptionsFrame& f, int equipped);
int saved_character(const OptionsFrame& f);  // the saved entity's character byte & 3, -1 when unreadable

// The one game function the mod calls (see CLAUDE.md): LoadHeldItemsImages, the
// inventory screen's icon rebuild that every pickup and every load runs.
bool rebuild_item_icons();  // main thread, between frames

// --- code sites --------------------------------------------------------------------------
enum Site {
  kSiteAmmoGuns = 0,     // dec of the equipped weapon's count, every gun
  kSiteAmmoFlame,        // the same, in the flamethrower's hold-fire loop
  kSiteTimerGame,        // Game_timer's +1 in game_loop
  kSiteTimerInventory,   // its +1 in the inventory (GOG's code cave: every other 60 fps frame)
  kSiteCountdown,        // the self-destruct counter's +1
  kSiteSaveCount,        // the save count's +1 after the file is written
  kSiteRibbonCheck,      // the typewriter's "no ribbon" branch -> the game's no-ribbon save
  kSiteRibbonTake,       // the save's ribbon take, skipped
  kSiteCount
};
mem::Patch* site(Site s);  // a prepared patch, or null when the site was not found
const char* site_name(Site s);

}  // namespace re1cc::game
