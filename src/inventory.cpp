#include "inventory.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "items.h"
#include "log.h"

namespace re1cc::inventory {

int Fit::topped() const {
  int n = 0;
  for (int v : onto) n += v;
  return n;
}

const char* who_name(int who) {
  switch (who) {
    case 0: return "Chris";
    case 1: return "Jill";
    case 3: return "Rebecca";
    default: break;
  }
  return "the player";
}

int count_limit(int id) {
  if (id <= 0 || id == items::kKnifeId || items::infinite(id)) return 0;
  if (game::item_stacks(id)) return game::stack_cap();
  if (items::is_weapon(id)) {
    const int m = game::item_max(id);
    return m > 0 ? m : items::magazine(id);
  }
  return 255;
}

int count_floor(int id) { return items::is_weapon(id) ? 0 : 1; }

// A gun comes loaded, everything else as one.
int default_count(int id) {
  const int limit = count_limit(id);
  if (!limit) return 0;
  return items::is_weapon(id) ? limit : 1;
}

namespace {

int clamp_count(int id, int count) {
  const int limit = count_limit(id);
  if (!limit) return 0;
  const int lo = count_floor(id);
  return count < lo ? lo : count > limit ? limit : count;
}

int first_free(const Slot* slots, int n) {
  for (int i = 0; i < n; ++i)
    if (slots[i].id == 0) return i;
  return -1;
}

// A take into the inventory, as a pickup does it: the stacks of an item the
// game stacks are topped up first, slot by slot, each to what one slot holds,
// and the rest goes into the free slot, one slot's worth. Everything else goes
// whole into the free slot - a gun's count is its magazine, not a number of
// guns - and a count of 0 (an empty gun) still moves.
Fit bag_fit(const Slot* bag, int held, int size, int id, int count) {
  Fit f;
  const int want = count < 0 ? 0 : count;
  int rest = want;
  const int cap = game::item_stacks(id) ? game::stack_cap() : 0;
  for (int s = 0; cap && s < held && s < kBagMax && rest > 0; ++s) {
    if (bag[s].id != id || bag[s].count >= cap) continue;
    f.onto[s] = cap - bag[s].count < rest ? cap - bag[s].count : rest;
    rest -= f.onto[s];
  }
  if ((rest > 0 || rest == want) && held < size) {
    f.slot = held;
    f.into = cap && rest > cap ? cap : rest;
    rest -= f.into;
  }
  f.left = rest;
  return f;
}

// Every stack of `id` in the box, with `add` more of it, laid out again as the
// game's slots hold ammo: full stacks of `cap` and at most one short one, over
// the slots that already held it (in slot order) and then the first free ones;
// a slot it no longer needs is emptied. Returns how many found no room - never
// more than `add` while every stack is within `cap`, as the game's own pickup
// and combine keep them.
int combine(Slot* box, int n, int id, int add, int cap) {
  int total = add, k = 0;
  int slots[kBoxMax];
  for (int s = 0; s < n && s < kBoxMax; ++s)
    if (box[s].id == id) {
      total += box[s].count;
      slots[k++] = s;
    }
  const int need = (total + cap - 1) / cap;
  for (int s = 0; s < n && s < kBoxMax && k < need; ++s)
    if (box[s].id == 0) slots[k++] = s;
  const int kept = total < k * cap ? total : k * cap;
  int rest = kept;
  for (int i = 0; i < k; ++i) {
    const int c = rest > cap ? cap : rest;
    box[slots[i]] = c > 0 ? Slot{id, c} : Slot{};
    rest -= c;
  }
  return total - kept;
}

// A store into the box: an item the game stacks is combined with every stack
// of it there (combine), anything else takes the first free slot, whole.
BoxFit box_fit(const Slot* box, int n, int id, int count) {
  BoxFit f;
  for (int i = 0; i < n && i < kBoxMax; ++i) f.box[i] = box[i];
  const int cap = game::item_stacks(id) ? game::stack_cap() : 0;
  if (cap && count > 0) {
    for (int s = 0; s < n && s < kBoxMax; ++s) f.joined += box[s].id == id ? 1 : 0;
    f.left = combine(f.box, n, id, count, cap);
    if (f.left > count) {  // it would cost an item already in the box: nothing moves
      BoxFit none;
      for (int i = 0; i < n && i < kBoxMax; ++i) none.box[i] = box[i];
      none.left = count;
      return none;
    }
    f.stored = count - f.left;
    for (int s = 0; s < n && s < kBoxMax; ++s)
      if (f.box[s].id == id) {
        if (f.slot < 0) f.slot = s;
        ++f.stacks;
        f.total += f.box[s].count;
      }
    f.ok = f.stored > 0;
  } else {
    f.slot = first_free(box, n);
    if (f.slot >= 0) {
      f.box[f.slot] = {id, count};
      f.stored = f.total = count;
      f.stacks = 1;
      f.ok = true;
    } else {
      f.left = count;
    }
  }
  return f;
}

}  // namespace

Fit fit_into_bag(const View& v, int id, int count) { return bag_fit(v.bag, v.held, v.size, id, count); }

BoxFit fit_into_box(const View& v, int id, int count) { return box_fit(v.box, v.box_size, id, count); }

namespace {

CRITICAL_SECTION g_lock;  // the view, the note and the request queue
bool g_ready = false;
View g_view;              // what the panel was last handed
char g_note[200] = {};
bool g_note_error = false;

// Main thread only.
bool g_was_open = false;
bool g_changed = false;       // the panel changed the inventory while the option screen was up
int g_seen_who = -1;          // the character, as it was before the option screen masked it
int g_seen_equipped = -1;     // the equipped byte, as it was before the option screen put its stand-in there
game::OptionsFrame g_frame;   // the option screen's saved state, found once per opening
DWORD g_frame_search = 0;
bool g_frame_logged = false;
char g_unreadable[160] = {};  // why the inventory could not be read, logged once per reason

struct Request {
  enum Type { kSet, kStore, kTake, kDrop } type;
  int slot, id, count;
  Slot seen;
};
Request g_req[16];
int g_req_n = 0;

void note(bool error, const char* fmt, ...) {
  char text[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  logf("%sinventory: %s", error ? "ERROR: " : "", text);
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  std::snprintf(g_note, sizeof(g_note), "%s", text);
  g_note_error = error;
  LeaveCriticalSection(&g_lock);
}

void push(const Request& r) {
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  if (g_req_n < 16) g_req[g_req_n++] = r;
  LeaveCriticalSection(&g_lock);
}

int drain(Request* out, int max) {
  if (!g_ready) return 0;
  EnterCriticalSection(&g_lock);
  const int n = g_req_n < max ? g_req_n : max;
  for (int i = 0; i < n; ++i) out[i] = g_req[i];
  g_req_n = 0;
  LeaveCriticalSection(&g_lock);
  return n;
}

// What a move works on: the game's own state, with the option screen's
// stand-ins replaced by what it saved.
struct State {
  game::Bag bag;
  int who = -1;
  int size = 0;
  int equipped = 0;    // the real equipped slot, 1-based (0 = none)
  bool frame = false;  // `equipped` is the option screen's saved byte, which a move may change
  int box_size = 0;
  Slot box[kBoxMax];
};

bool unreadable(const char* why) {
  if (std::strcmp(why, g_unreadable) != 0) {
    std::snprintf(g_unreadable, sizeof(g_unreadable), "%s", why);
    logf("inventory: the inventory cannot be read right now: %s", why);
  }
  return false;
}

bool load(State* s, bool open) {
  if (!game::items_known()) return false;
  if (!game::read_bag(&s->bag)) return unreadable("the game's slot pointer names neither inventory");
  s->box_size = game::box_size() < kBoxMax ? game::box_size() : kBoxMax;
  if (!game::read_box(s->box, s->box_size)) return unreadable("the item box is not readable");
  if (open) {
    // The screen masks the character byte and puts a 1 in the equipped byte:
    // what it saved, or what the game said just before it opened.
    s->frame = g_frame.equipped != 0;
    s->who = s->frame ? game::saved_character(g_frame) : g_seen_who;
    s->equipped = s->frame ? game::saved_equipped(g_frame) : g_seen_equipped;
  } else {
    s->who = game::character_id();
    s->equipped = s->bag.equipped;
  }
  if (s->who < 0 || s->equipped < 0) return unreadable("the character or the equipped slot is not known yet");
  // The game's own rule (rearrange_item_slots): 8 slots for Jill, 6 otherwise.
  s->size = s->who == 1 ? 8 : 6;
  if (s->bag.rebecca != (s->who == 3))
    return unreadable(s->bag.rebecca ? "the slot pointer names Rebecca's inventory, but Rebecca is not in play"
                                     : "Rebecca is in play, but the slot pointer names the main inventory");
  if (s->bag.held > s->size) return unreadable("the game counts more items than the inventory has slots");
  g_unreadable[0] = '\0';
  return true;
}

// A slot at the end of the packed inventory, with a free row of the icon sheet
// - the first one, as the game's own pickup and item box pick it.
void add_slot(State& s, int id, int count) {
  game::Bag& b = s.bag;
  int row = 0;
  while (row < game::kMaxBagSlots && (b.rows_used & (1 << row))) ++row;
  if (row >= game::kMaxBagSlots) row = b.held;  // bits the game would not have left: keep to the sheet
  b.slot[b.held] = {id, count};
  b.row[b.held] = row;
  b.rows_used |= 1 << row;
  ++b.held;
}

// rearrange_item_slots (0x451510), step for step: the items move down over the
// empty slots in order, each taking its icon row along; an emptied slot gives
// its row back; the equipped slot follows its weapon; the count is what is left.
void compact(State& s) {
  game::Bag& b = s.bag;
  int eq = s.equipped - 1;
  int w = 0;
  for (int r = 0; r < s.size; ++r) {
    if (b.slot[r].id == 0) {
      if (r < b.held) b.rows_used &= ~(1 << (b.row[r] & 31));
      continue;
    }
    if (w != r) {
      b.slot[w] = b.slot[r];
      b.row[w] = b.row[r];
      if (eq == r) eq = w;
    }
    ++w;
  }
  s.equipped = eq + 1;
  b.held = w;
  for (int i = w; i < s.size; ++i) b.slot[i] = {};
}

bool same(const Slot& a, const Slot& b) { return a.id == b.id && a.count == b.count; }

// Hand the finished move to the game. The equipped slot is checked first: the
// option screen gives the player whatever is in it when it closes, so it must
// still hold a weapon, and it may only move where its saved copy can follow.
bool commit(const State& before, const State& after, const char* what) {
  const Slot none{};
  const Slot& eq_before = before.equipped > 0 ? before.bag.slot[before.equipped - 1] : none;
  const Slot& eq_after = after.equipped > 0 ? after.bag.slot[after.equipped - 1] : none;
  const bool touched = after.equipped != before.equipped || !same(eq_before, eq_after);
  if (touched && after.equipped > 0 && (after.equipped > after.bag.held || !items::is_weapon(eq_after.id))) {
    note(true, "%s would leave something that is not a weapon in the equipped slot - nothing was changed.", what);
    return false;
  }
  if (after.equipped != before.equipped && !before.frame) {
    note(true,
         "%s would change which slot is equipped, and the option screen's own record of it was not found (see the "
         "log) - nothing was changed.",
         what);
    return false;
  }
  for (int i = 0; i < after.box_size; ++i)
    if (!same(after.box[i], before.box[i]) && !game::write_box_slot(i, after.box[i].id, after.box[i].count)) {
      note(true, "%s: item box slot %d could not be written.", what, i + 1);
      return false;
    }
  if (!game::write_bag(after.bag, after.size)) {
    note(true, "%s: the inventory could not be written.", what);
    return false;
  }
  if (after.equipped != before.equipped && !game::set_saved_equipped(g_frame, after.equipped)) {
    note(true, "%s: the option screen's record of the equipped slot could not be written.", what);
    return false;
  }
  g_changed = true;
  return true;
}

// " - it is unequipped" / " - the equipped weapon is now in slot 2", or nothing.
const char* equip_change(const State& before, const State& after, char* buf, size_t n) {
  buf[0] = '\0';
  if (after.equipped == before.equipped) return buf;
  if (after.equipped == 0) std::snprintf(buf, n, " - nothing is equipped now");
  else std::snprintf(buf, n, " - the equipped %s is in slot %d now", items::name(after.bag.slot[after.equipped - 1].id),
                     after.equipped);
  return buf;
}

void do_set(const Request& r, bool open) {
  State before;
  if (!load(&before, open)) {
    note(true, "The inventory could not be read (see the log) - nothing was changed.");
    return;
  }
  const char* who = who_name(before.who);
  const int k = r.slot;
  if (k < 0 || k >= before.size) {
    note(true, "%s has no slot %d.", who, k + 1);
    return;
  }
  const Slot cur = k < before.bag.held ? before.bag.slot[k] : Slot{};
  if (!same(cur, r.seen)) {
    note(true, "%s slot %d changed since that was clicked - nothing was changed.", who, k + 1);
    return;
  }
  if (r.id != 0 && !items::find(r.id)) {
    note(true, "Item %d is not one this panel knows - nothing was changed.", r.id);
    return;
  }
  const int count = clamp_count(r.id, r.count);
  State after = before;
  char what[64], eq[96];
  std::snprintf(what, sizeof(what), "Changing %s slot %d", who, k + 1);
  if (k >= before.bag.held) {
    // An empty slot: the inventory is packed, so the item goes into the first
    // empty one, which is where the game puts every new item too.
    if (r.id == 0) {
      note(false, "%s slot %d is empty already.", who, k + 1);
      return;
    }
    add_slot(after, r.id, count);
    if (!commit(before, after, what)) return;
    note(false, "%s slot %d: %s%s.", who, after.bag.held, items::name(r.id), items::quantity(r.id, count).text);
  } else if (r.id == 0) {
    if (before.equipped == k + 1) after.equipped = 0;
    after.bag.slot[k] = {};
    compact(after);
    if (!commit(before, after, what)) return;
    note(false, "%s slot %d emptied (it held %s%s)%s.", who, k + 1, items::name(cur.id),
         items::quantity(cur.id, cur.count).text, equip_change(before, after, eq, sizeof(eq)));
  } else {
    after.bag.slot[k] = {r.id, count};
    // A weapon put in the equipped slot is equipped when the option screen
    // closes (it equips whatever the slot holds); anything else unequips.
    if (before.equipped == k + 1 && !items::is_weapon(r.id)) after.equipped = 0;
    if (!commit(before, after, what)) return;
    note(false, "%s slot %d: %s%s%s.", who, k + 1, items::name(r.id), items::quantity(r.id, count).text,
         equip_change(before, after, eq, sizeof(eq)));
  }
}

void do_store(const Request& r, bool open) {
  State before;
  if (!load(&before, open)) {
    note(true, "The inventory could not be read (see the log) - nothing was stored.");
    return;
  }
  const char* who = who_name(before.who);
  const int k = r.slot;
  if (k < 0 || k >= before.bag.held) {
    note(true, "%s slot %d is empty.", who, k + 1);
    return;
  }
  const Slot cur = before.bag.slot[k];
  if (!same(cur, r.seen)) {
    note(true, "%s slot %d changed since that was clicked - nothing was stored.", who, k + 1);
    return;
  }
  const bool stacking = game::item_stacks(cur.id);
  const BoxFit f = box_fit(before.box, before.box_size, cur.id, cur.count);
  if (!f.ok) {
    note(true, "The item box has no free slot%s - %s stays with %s.", stacking ? ", and no stack of it with room" : "",
         items::name(cur.id), who);
    return;
  }
  State after = before;
  for (int s = 0; s < after.box_size; ++s) after.box[s] = f.box[s];
  if (f.left == 0) {
    if (before.equipped == k + 1) after.equipped = 0;
    after.bag.slot[k] = {};
    compact(after);
  } else {
    after.bag.slot[k].count = f.left;
  }
  char what[64], left[64] = "", eq[96];
  std::snprintf(what, sizeof(what), "Storing %s slot %d", who, k + 1);
  if (!commit(before, after, what)) return;
  if (f.left > 0) std::snprintf(left, sizeof(left), " - x%d had no room and stayed with %s", f.left, who);
  const char* name = items::name(cur.id);
  if (stacking && f.joined)
    note(false, "Stored %s x%d from %s slot %d with the %s in the box: x%d now, in %d stack%s%s.", name, f.stored, who,
         k + 1, name, f.total, f.stacks, f.stacks == 1 ? "" : "s", left);
  else
    note(false, "Stored %s%s from %s slot %d in box slot %d%s%s.", name, items::quantity(cur.id, f.stored).text, who,
         k + 1, f.slot + 1, left, equip_change(before, after, eq, sizeof(eq)));
}

void do_take(const Request& r, bool open) {
  State before;
  if (!load(&before, open)) {
    note(true, "The inventory could not be read (see the log) - nothing was taken.");
    return;
  }
  const char* who = who_name(before.who);
  const int j = r.slot;
  if (j < 0 || j >= before.box_size || before.box[j].id == 0) {
    note(true, "Item box slot %d is empty.", j + 1);
    return;
  }
  const Slot cur = before.box[j];
  if (!same(cur, r.seen)) {
    note(true, "Item box slot %d changed since that was clicked - nothing was taken.", j + 1);
    return;
  }
  const Fit f = bag_fit(before.bag.slot, before.bag.held, before.size, cur.id, cur.count);
  if (!f.ok()) {
    note(true, "%s has no free slot%s - %s stays in the box.", who,
         game::item_stacks(cur.id) ? ", and no stack of it with room" : "", items::name(cur.id));
    return;
  }
  State after = before;
  for (int s = 0; s < after.bag.held; ++s) after.bag.slot[s].count += f.onto[s];
  if (f.slot >= 0) add_slot(after, cur.id, f.into);
  if (f.left > 0) after.box[j].count = f.left;
  else after.box[j] = {};
  // What stays in the box of an item the game stacks goes back together with
  // the rest of it, as a store would leave it - never at the cost of one.
  if (game::item_stacks(cur.id)) {
    Slot tidy[kBoxMax];
    for (int s = 0; s < after.box_size; ++s) tidy[s] = after.box[s];
    if (combine(tidy, after.box_size, cur.id, 0, game::stack_cap()) == 0)
      for (int s = 0; s < after.box_size; ++s) after.box[s] = tidy[s];
  }
  char what[64];
  std::snprintf(what, sizeof(what), "Taking item box slot %d", j + 1);
  if (!commit(before, after, what)) return;
  const int moved = f.topped() + f.into;
  if (f.topped() == 0 && f.left == 0)
    note(false, "Took %s%s from box slot %d into %s slot %d.", items::name(cur.id),
         items::quantity(cur.id, cur.count).text, j + 1, who, after.bag.held);
  else if (f.left > 0)
    note(false, "Took %s x%d from box slot %d into %s's inventory - x%d had no room and stayed in the box.",
         items::name(cur.id), moved, j + 1, who, f.left);
  else
    note(false, "Took %s x%d from box slot %d onto the stack%s %s already carries%s.", items::name(cur.id), moved, j + 1,
         f.topped() == moved ? "" : "s", who, f.slot >= 0 ? " and a slot of its own" : "");
}

void do_drop(const Request& r, bool open) {
  State before;
  if (!load(&before, open)) {
    note(true, "The item box could not be read (see the log) - nothing was thrown away.");
    return;
  }
  const int j = r.slot;
  if (j < 0 || j >= before.box_size || before.box[j].id == 0) {
    note(true, "Item box slot %d is empty.", j + 1);
    return;
  }
  if (!same(before.box[j], r.seen)) {
    note(true, "Item box slot %d changed since that was clicked - nothing was thrown away.", j + 1);
    return;
  }
  State after = before;
  after.box[j] = {};
  char what[48];
  std::snprintf(what, sizeof(what), "Throwing away box slot %d", j + 1);
  if (!commit(before, after, what)) return;
  note(false, "Threw away %s%s from box slot %d.", items::name(r.seen.id), items::quantity(r.seen.id, r.seen.count).text,
       j + 1);
}

void apply(const Request& r, bool in_game, bool open) {
  if (!in_game || !open) {
    note(true, "The inventory and the item box can only be changed while the option screen is up - nothing was changed.");
    return;
  }
  switch (r.type) {
    case Request::kSet: do_set(r, open); break;
    case Request::kStore: do_store(r, open); break;
    case Request::kTake: do_take(r, open); break;
    case Request::kDrop: do_drop(r, open); break;
  }
}

// The option screen's saved state, looked for a few times a second until it is
// found. It must say what the game said just before the screen opened: a copy
// that disagrees is somebody else's.
void find_frame() {
  const DWORD now = GetTickCount();
  if (g_frame_search && now - g_frame_search < 250) return;
  g_frame_search = now;
  game::OptionsFrame f;
  if (!game::find_options_frame(&f)) {
    if (!g_frame_logged) {
      g_frame_logged = true;
      logf("inventory: the option screen's saved equipped slot was not found in its stack - moves that would unequip "
           "or move the equipped weapon are refused while it is up");
    }
    return;
  }
  const int eq = game::saved_equipped(f), who = game::saved_character(f);
  if ((g_seen_equipped >= 0 && eq != g_seen_equipped) || (g_seen_who >= 0 && who != g_seen_who)) {
    if (!g_frame_logged) {
      g_frame_logged = true;
      logf("inventory: the option screen's saved state at %p says equipped slot %d and %s, but the game said %d and %s "
           "just before it opened - not used",
           reinterpret_cast<void*>(f.equipped), eq, who_name(who), g_seen_equipped, who_name(g_seen_who));
    }
    return;
  }
  g_frame = f;
  logf("inventory: the option screen's saved equipped slot is at %p (slot %d, %s)", reinterpret_cast<void*>(f.equipped),
       eq, who_name(who));
}

}  // namespace

void init() {
  if (g_ready) return;
  InitializeCriticalSection(&g_lock);
  g_ready = true;
}

View view() {
  View v;
  if (!g_ready) return v;
  EnterCriticalSection(&g_lock);
  v = g_view;
  std::snprintf(v.note, sizeof(v.note), "%s", g_note);
  v.note_error = g_note_error;
  LeaveCriticalSection(&g_lock);
  return v;
}

void request_set(int slot, int id, int count, const Slot& seen) { push({Request::kSet, slot, id, count, seen}); }
void request_store(int slot, const Slot& seen) { push({Request::kStore, slot, 0, 0, seen}); }
void request_take(int box_slot, const Slot& seen) { push({Request::kTake, box_slot, 0, 0, seen}); }
void request_drop(int box_slot, const Slot& seen) { push({Request::kDrop, box_slot, 0, 0, seen}); }

void tick(bool in_game, bool options_open) {
  if (!g_ready) return;
  const bool known = game::items_known();
  const bool open = in_game && options_open;

  // What the option screen is about to mask, seen while it is not up.
  if (in_game && !options_open && known) {
    game::Bag b;
    if (game::read_bag(&b)) {
      g_seen_who = game::character_id();
      g_seen_equipped = b.equipped;
    }
  }
  if (open != g_was_open) {
    if (open) {
      // A note belongs to the opening it was made in: a refusal from the last
      // one says nothing about this one.
      EnterCriticalSection(&g_lock);
      g_note[0] = '\0';
      g_note_error = false;
      LeaveCriticalSection(&g_lock);
    }
    if (!open && g_changed) {
      // The screen has put the player entity and the equipped byte back and
      // equipped what that slot holds; its inventory screen's icons are the
      // one thing left, and only the game can draw them.
      if (!in_game) logf("inventory: the game left the session before the icons were rebuilt - the next load does it");
      else if (game::rebuild_item_icons()) logf("inventory: the option screen closed - the inventory screen's icons were rebuilt");
      else logf("ERROR: inventory: the icon rebuild could not be run - the inventory screen's pictures catch up at the next pickup or load");
    }
    g_changed = false;
    g_frame = {};
    g_frame_search = 0;
    g_frame_logged = false;
    g_was_open = open;
  }
  if (open && known && game::options_frame_known() && !g_frame.equipped) find_frame();

  Request reqs[16];
  const int n = drain(reqs, 16);
  for (int i = 0; i < n; ++i) apply(reqs[i], in_game, options_open);

  View v;
  v.known = known;
  v.in_game = in_game;
  v.open = open;
  v.frame = g_frame.equipped != 0;
  v.rules_known = game::item_rules_known();
  v.stack_cap = game::stack_cap();
  State s;
  if (in_game && known && load(&s, open)) {
    v.who = s.who;
    v.size = s.size;
    v.held = s.bag.held;
    v.equipped = s.equipped - 1;
    for (int i = 0; i < s.size && i < kBagMax; ++i) v.bag[i] = i < s.bag.held ? s.bag.slot[i] : Slot{};
    v.box_size = s.box_size;
    for (int i = 0; i < s.box_size; ++i) v.box[i] = s.box[i];
  }
  EnterCriticalSection(&g_lock);
  g_view = v;
  LeaveCriticalSection(&g_lock);
}

}  // namespace re1cc::inventory
