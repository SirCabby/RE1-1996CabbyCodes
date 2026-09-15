#pragma once

#include "game.h"

// The inventory editor and the item box, on the option screen's panel. Unlike
// RE0CabbyCodes' storage box, which the mod kept in a file of its own, the box
// here is the game's: the 48 slots every item box in the mansion opens, saved
// with the game. The panel shows the inventory of the character in play and the
// box, edits an inventory slot, stores an item from the inventory in the box,
// takes one back, or throws a box item away.
//
// Everything is a write to the save block, made between frames while the
// option screen is up and the game's own task is suspended - never while the
// game itself could be using the inventory. Moves follow the game's rules: the
// inventory stays packed as rearrange_item_slots keeps it, ammo and ink ribbons
// taken out top up a stack the character already carries (as a pickup does)
// before taking a slot of their own, and anything else takes a slot of its own.
// The box is kept tidier than the game's own box screen keeps it, which never
// merges anything: ammo and ink ribbons stored in it are combined with every
// stack of the same item there. What fits nowhere stays where it was.
//
// The equipped weapon is the one thing the option screen gets in the way of:
// it replaces the equipped-slot byte with a stand-in and puts its own saved
// copy back when it closes, then equips whatever that slot holds. So every
// move that empties or shifts the equipped slot writes the screen's saved copy
// instead (game::find_options_frame); if that copy cannot be found, such a
// move is refused. When the screen closes after the panel changed the
// inventory, the game's own icon rebuild is run once (game::rebuild_item_icons)
// so its inventory screen shows the new items.
namespace re1cc::inventory {

using Slot = game::ItemSlot;
constexpr int kBagMax = game::kMaxBagSlots;
constexpr int kBoxMax = 64;  // the box is 48 slots; room to spare

// A copy of everything the panel draws, taken under the lock.
struct View {
  bool known = false;        // the inventory and the box were found in the game
  bool in_game = false;
  bool open = false;         // the option screen is up: the only time anything is changed
  bool frame = false;        // the option screen's saved equipped slot was found
  bool rules_known = false;  // the game's item rules were read out of its code
  int who = -1;              // 0 Chris, 1 Jill, 3 Rebecca (-1 = not readable now)
  int size = 0;              // the inventory's slots (6, or 8 for Jill)
  int held = 0;              // slots in use; the rest are empty
  int equipped = -1;         // the equipped slot, 0-based (-1 = none)
  Slot bag[kBagMax];
  int box_size = 0;
  Slot box[kBoxMax];
  int stack_cap = 0;         // what one slot of ammo or ink ribbons holds
  char note[200] = {};       // what the last change came to (logged; the panel shows a refusal only)
  bool note_error = false;
};
View view();                  // any thread
const char* who_name(int who);

// What the editor lets a slot of an item hold. 0 = the game keeps no count for
// it (the knife, the two infinite weapons); a gun holds 0 (empty) up to its
// magazine, ammo and ink ribbons 1 up to a stack, anything else 1 up to 255
// (a key's count is how many more doors it opens).
int count_limit(int id);
int count_floor(int id);
int default_count(int id);  // what an item starts with when the editor picks it

// Where a box item goes when it is taken: onto the stacks of it the character
// already carries (ammo and ink ribbons, each up to one slot's worth, in slot
// order - what a pickup does), then into the first free slot. What fits
// nowhere stays in the box, combined again with the rest of that item. The
// panel asks this for what a button offers, and the move does what it answered.
struct Fit {
  int onto[kBagMax] = {};  // how many go onto the stack in each slot
  int slot = -1;           // the free slot the rest goes into (-1 = none, or none needed)
  int into = 0;            // how many go into it
  int left = 0;            // how many stay in the box
  int topped() const;
  bool ok() const { return slot >= 0 || topped() > 0; }  // the move moves anything at all
};
Fit fit_into_bag(const View& v, int id, int count);

// Where an inventory item goes when it is stored. Ammo and ink ribbons are
// combined with every stack of the same item in the box, and the lot is laid
// out again the way the game's slots hold ammo - full stacks of what one slot
// holds and at most one short one - over the slots that item already had, then
// the first free ones; a slot it no longer needs is emptied. Anything else
// takes the first free slot, whole. What has no room stays in the inventory.
struct BoxFit {
  Slot box[kBoxMax];  // the box afterwards
  int stored = 0;     // how many go in
  int left = 0;       // how many stay in the inventory
  int slot = -1;      // the first box slot holding it afterwards
  int joined = 0;     // stacks of it that were in the box already
  int total = 0;      // how many of it the box holds afterwards
  int stacks = 0;     // ...in how many slots
  bool ok = false;    // anything goes in at all
};
BoxFit fit_into_box(const View& v, int id, int count);

// Requests from the panel, carried out on the next main-thread tick while the
// option screen is up. `seen` is what the panel showed in that slot: a slot
// that holds something else by then is left alone - acting on the wrong item is
// worse than not acting.
void request_set(int slot, int id, int count, const Slot& seen);  // the editor: slot 0..size-1
void request_store(int slot, const Slot& seen);                    // inventory slot -> the box
void request_take(int box_slot, const Slot& seen);                 // box slot -> the inventory
void request_drop(int box_slot, const Slot& seen);                 // throw a box item away

void init();                                  // from DllMain: the lock
void tick(bool in_game, bool options_open);  // main thread, every tick

}  // namespace re1cc::inventory
