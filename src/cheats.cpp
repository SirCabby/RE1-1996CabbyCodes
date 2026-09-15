#include "cheats.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "log.h"
#include "mem.h"
#include "savefiles.h"

namespace re1cc::cheats {
namespace {

volatile LONG g_enabled[kCount] = {};
// g_status is the main thread's working copy; g_published is the only thing
// another thread may look at, swapped over under the lock (RE0: an unlocked copy
// of a struct this size can pair one tick's values with another's).
Status g_status;
Status g_published;
CRITICAL_SECTION g_status_lock;
bool g_status_lock_ready = false;
const char* kNames[kCount] = {"God mode",           "One hit kills",    "Infinite ammo",         "Infinite ink ribbons",
                              "Save without counting", "Freeze play time", "Freeze countdown timer"};

bool on(Kind k) { return g_enabled[k] != 0; }

// --- requests (panel -> main thread) ----------------------------------------------------
struct Request {
  enum Type { kSaves, kPlaytime, kCountdown } type;
  int a;
};
Request g_req[16];
int g_req_n = 0;
CRITICAL_SECTION g_req_lock;
bool g_lock_ready = false;

void push(Request r) {
  if (!g_lock_ready) return;
  EnterCriticalSection(&g_req_lock);
  if (g_req_n < 16) g_req[g_req_n++] = r;
  LeaveCriticalSection(&g_req_lock);
}
int drain(Request* out, int max) {
  if (!g_lock_ready) return 0;
  EnterCriticalSection(&g_req_lock);
  const int n = g_req_n < max ? g_req_n : max;
  std::memcpy(out, g_req, sizeof(Request) * static_cast<size_t>(n));
  g_req_n = 0;
  LeaveCriticalSection(&g_req_lock);
  return n;
}

void publish(const Status& st) {
  g_status = st;
  if (!g_status_lock_ready) return;
  EnterCriticalSection(&g_status_lock);
  g_published = st;
  LeaveCriticalSection(&g_status_lock);
}

void note(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_status.last_action, sizeof(g_status.last_action), fmt, args);
  va_end(args);
  logf("%s", g_status.last_action);
}

void toggle_patch(game::Site s, bool want, bool* applied_out) {
  mem::Patch* p = game::site(s);
  if (!p) {
    *applied_out = false;
    return;
  }
  if (want && !p->applied) {
    if (p->apply()) logf("patch applied: %s", p->label);
    else logf("ERROR: patch %s could not be applied (site changed?)", p->label);
  } else if (!want && p->applied) {
    if (p->revert()) logf("patch restored: %s", p->label);
    else logf("ERROR: patch %s could not be restored (site changed?)", p->label);
  }
  *applied_out = p->applied;
}

// --- holds ---------------------------------------------------------------------------------
// God mode: HP back to its maximum and the poisons that drain it cleared. Yawn's
// poison (0x20) is left alone: it is paired with a scenario flag that only the
// serum clears, and with HP held it cannot hurt.
constexpr int kPoison = 0x02, kFastDrain = 0x40;
bool g_god_logged = false;
void hold_god(const game::Player& p) {
  if (p.max_hp > 0 && p.hp < p.max_hp) {
    game::set_player_hp(p.max_hp);
    if (!g_god_logged) {
      g_god_logged = true;
      logf("god mode: holding the player at %d HP", p.max_hp);
    }
  }
  if (p.status & (kPoison | kFastDrain)) game::set_player_status(p.status & ~(kPoison | kFastDrain));
}

// One hit kills: every live enemy is held at 0 HP - the last value that still
// counts as alive: RE1 kills an enemy when a hit takes its HP below 0, so any
// hit now does, and death, drops and scripts run through the game's own code.
int g_ohk_logged = 0;
int hold_enemies(bool hold) {
  game::Enemy list[64];
  const int n = game::enemies(list, 64);
  int live = 0;
  for (int i = 0; i < n; ++i) {
    const game::Enemy& e = list[i];
    if (e.state == 3 || e.hp < 0) continue;  // dying or dead
    ++live;
    if (hold && e.hp > 0) {
      game::set_enemy_hp(e.index, 0);
      if (g_ohk_logged < 8) {
        ++g_ohk_logged;
        logf("one hit kills: enemy slot %d (type 0x%02X) held at 0 HP (was %d)", e.index, e.id, e.hp);
      }
    }
  }
  return live;
}

// Freeze without the patch sites: the value is put back whenever it moves on.
bool g_timer_hold = false;
uint32_t g_frozen_timer = 0;
bool g_countdown_hold = false;
int g_frozen_countdown = -1;

// Save without counting: the save screen runs while the typewriter is in state
// 3. The files are stamped when it opens; when it closes, a file written while
// the counter did not move is a save the counter skipped, and it goes to disk
// one lower, because loading a file adds one (see savefiles.h).
bool g_save_screen = false;
savefiles::Stamp g_stamp;
int g_count_at_open = -1;

void watch_save_screen() {
  const bool screen = game::typewriter_state() == 3;
  if (screen && !g_save_screen) {
    g_stamp = savefiles::stamp();
    g_count_at_open = game::save_count();
  } else if (!screen && g_save_screen) {
    const int slot = savefiles::written_since(g_stamp);
    const int count = game::save_count();
    if (slot >= 0 && count == g_count_at_open && count > 0) {
      if (savefiles::lower_save_count(slot, count))
        note("Saved to file %d without counting it: that file keeps %d save%s, and loads back the same.", slot + 1,
             count - 1, count - 1 == 1 ? "" : "s");
      else
        logf("ERROR: save without counting: save file %d could not be brought down to %d saves", slot + 1, count - 1);
    }
  }
  g_save_screen = screen;
}

}  // namespace

void init() {
  if (g_lock_ready) return;
  InitializeCriticalSection(&g_req_lock);
  InitializeCriticalSection(&g_status_lock);
  g_status_lock_ready = true;
  g_lock_ready = true;
}

const char* name(Kind k) { return k >= 0 && k < kCount ? kNames[k] : "?"; }
bool enabled(Kind k) { return k >= 0 && k < kCount && g_enabled[k] != 0; }
void set_enabled(Kind k, bool v) {
  if (k >= 0 && k < kCount) InterlockedExchange(&g_enabled[k], v ? 1 : 0);
}
Status status() {
  if (!g_status_lock_ready) return Status{};
  EnterCriticalSection(&g_status_lock);
  const Status s = g_published;
  LeaveCriticalSection(&g_status_lock);
  return s;
}

void request_saves(int saves) { push({Request::kSaves, saves}); }
void request_playtime_seconds(int seconds) { push({Request::kPlaytime, seconds}); }
void request_countdown_left(int seconds) { push({Request::kCountdown, seconds}); }

void tick(bool in_game, bool options_open) {
  static bool init_done = false;
  if (!init_done) {
    init_done = true;
    const config::Settings& c = config::get();
    set_enabled(kGodMode, c.god_mode);
    set_enabled(kOneHitKills, c.one_hit_kills);
    set_enabled(kInfiniteAmmo, c.infinite_ammo);
    set_enabled(kInfiniteInk, c.infinite_ink);
    set_enabled(kNoSaveCount, c.no_save_count);
    set_enabled(kFreezePlaytime, c.freeze_playtime);
    set_enabled(kFreezeCountdown, c.freeze_countdown);
  }

  Status st;
  std::memcpy(st.last_action, g_status.last_action, sizeof(st.last_action));
  st.in_game = in_game;
  st.site_ammo = game::site(game::kSiteAmmoGuns) && game::site(game::kSiteAmmoFlame);
  st.site_ink = game::site(game::kSiteRibbonCheck) && game::site(game::kSiteRibbonTake);
  st.site_savecount = game::site(game::kSiteSaveCount) != nullptr;
  // Both increments or neither: freezing only one would leave the clock running
  // in the inventory (or out of it).
  st.site_playtime = game::site(game::kSiteTimerGame) && game::site(game::kSiteTimerInventory);
  st.site_countdown = game::site(game::kSiteCountdown) != nullptr;
  st.counters_ok = game::counters_ready();
  st.countdown_known = game::countdown_known();
  st.countdown_active = game::countdown_active();

  // Patches are independent of a session: toggle them whenever asked.
  if (st.site_ammo) {
    bool a = false, b = false;
    toggle_patch(game::kSiteAmmoGuns, on(kInfiniteAmmo), &a);
    toggle_patch(game::kSiteAmmoFlame, on(kInfiniteAmmo), &b);
    st.ammo_patched = a && b;
  }
  if (st.site_ink) {
    bool a = false, b = false;
    toggle_patch(game::kSiteRibbonCheck, on(kInfiniteInk), &a);
    toggle_patch(game::kSiteRibbonTake, on(kInfiniteInk), &b);
    st.ink_patched = a && b;
  }
  if (st.site_savecount) toggle_patch(game::kSiteSaveCount, on(kNoSaveCount), &st.savecount_patched);
  const bool freeze = on(kFreezePlaytime);
  if (st.site_playtime) {
    bool a = false, b = false;
    toggle_patch(game::kSiteTimerGame, freeze, &a);
    toggle_patch(game::kSiteTimerInventory, freeze, &b);
    st.playtime_patched = a && b;
    g_timer_hold = false;
  }
  // The self-destruct counter also counts outside the countdown, and room
  // scripts read it, so its +1 is only skipped while a countdown runs.
  if (st.site_countdown) {
    toggle_patch(game::kSiteCountdown, on(kFreezeCountdown) && st.countdown_active, &st.countdown_patched);
    g_countdown_hold = false;
  }
  watch_save_screen();

  if (!in_game) {
    g_god_logged = false;
    g_ohk_logged = 0;
    g_timer_hold = false;
    g_countdown_hold = false;
    publish(st);
    return;
  }

  // Requests first, so the readings below reflect them.
  Request reqs[16];
  const int n = drain(reqs, 16);
  for (int i = 0; i < n; ++i) {
    const Request& r = reqs[i];
    switch (r.type) {
      case Request::kSaves: {
        // The counter runs one ahead of the saves made: a new game starts it at 1.
        const int saves = r.a < 0 ? 0 : r.a > 98 ? 98 : r.a;
        if (game::set_save_count(saves + 1)) note("saves set to %d", saves);
        else note("saves not set: the save counter was not found");
        break;
      }
      case Request::kPlaytime:
        if (game::set_game_timer(static_cast<uint32_t>(r.a) * 30u)) {
          g_frozen_timer = static_cast<uint32_t>(r.a) * 30u;
          note("play time set to %d:%02d:%02d", r.a / 3600, (r.a / 60) % 60, r.a % 60);
        } else {
          note("play time not set: the clock was not found");
        }
        break;
      case Request::kCountdown:
        // Only a countdown the game itself started can be edited.
        if (!game::countdown_active()) {
          note("countdown not set: no countdown is running");
        } else if (game::set_countdown_elapsed(game::kCountdownLimit - r.a)) {
          g_frozen_countdown = game::countdown_elapsed();
          note("countdown set to %d:%02d left", r.a / 60, r.a % 60);
        }
        break;
    }
  }

  st.player_ok = game::player(&st.player);
  // The option screen borrows the player entity for its background model and
  // copies it back when it closes; a dead player is the game's to finish.
  if (on(kGodMode) && st.player_ok && !options_open && !game::player_dead()) hold_god(st.player);
  else if (!on(kGodMode)) g_god_logged = false;

  st.enemies_ok = game::enemies_known();
  st.enemies = hold_enemies(on(kOneHitKills));
  if (!on(kOneHitKills)) g_ohk_logged = 0;

  // Freeze play time without the patch sites: put the value back when it moves.
  if (!st.site_playtime && st.counters_ok) {
    if (freeze) {
      const uint32_t t = game::game_timer();
      if (!g_timer_hold) {
        g_timer_hold = true;
        g_frozen_timer = t;
        logf("play time frozen (hold) at %u", t);
      } else if (t + 60 < g_frozen_timer) {
        g_frozen_timer = t;  // a load moved it back: hold the new value
      } else if (t > g_frozen_timer) {
        game::set_game_timer(g_frozen_timer);
      }
      st.playtime_hold = true;
    } else {
      g_timer_hold = false;
    }
  }
  // The same for the countdown, per countdown: a new one is held where it starts.
  if (!st.site_countdown && st.countdown_known) {
    if (on(kFreezeCountdown) && st.countdown_active) {
      const int el = game::countdown_elapsed();
      if (!g_countdown_hold || el < g_frozen_countdown) {
        g_countdown_hold = true;
        g_frozen_countdown = el;
      } else if (el > g_frozen_countdown) {
        game::set_countdown_elapsed(g_frozen_countdown);
      }
      st.countdown_hold = true;
    } else {
      g_countdown_hold = false;
    }
  }

  if (st.counters_ok) {
    st.saves = game::save_count() - 1;
    st.timer = game::game_timer();
  }
  if (st.countdown_active) st.countdown_left = game::kCountdownLimit - game::countdown_elapsed();
  publish(st);
}

void remove_hooks() {
  for (int s = 0; s < game::kSiteCount; ++s)
    if (mem::Patch* p = game::site(static_cast<game::Site>(s)))
      if (p->applied) p->revert();
}

}  // namespace re1cc::cheats
