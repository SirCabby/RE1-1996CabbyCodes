#pragma once

#include <cstdint>

#include "game.h"

// The cheats. Their switches are shared between the panel (it runs in the
// present hook) and the tick that applies them (the game's PeekMessageA), so
// the switches are plain atomics, edits travel through a small request queue,
// and everything that touches the game runs from tick() on the main thread.
namespace re1cc::cheats {

enum Kind : int {
  kGodMode = 0,
  kOneHitKills,
  kInfiniteAmmo,
  kInfiniteInk,
  kNoSaveCount,
  kFreezePlaytime,
  kFreezeCountdown,
  kCount
};

void init();  // from DllMain: the locks
const char* name(Kind k);
bool enabled(Kind k);
void set_enabled(Kind k, bool on);  // any thread; takes effect on the next tick

// What the panel shows (built by tick, handed over under a lock).
struct Status {
  bool in_game = false;
  bool player_ok = false;
  game::Player player;
  bool enemies_ok = false;       // the enemy slots are known
  int enemies = 0;               // live enemies this tick
  bool site_ammo = false;        // both firing decrements are known
  bool ammo_patched = false;
  bool site_ink = false;         // the typewriter's check and the save's take are both known
  bool ink_patched = false;
  bool site_savecount = false;
  bool savecount_patched = false;
  bool site_playtime = false;    // both play-time increments are known
  bool playtime_patched = false;
  bool playtime_hold = false;    // freeze is running as a hold (no patch sites)
  bool countdown_known = false;
  bool site_countdown = false;
  bool countdown_patched = false;
  bool countdown_hold = false;
  bool countdown_active = false;
  int countdown_left = -1;       // seconds left on the self-destruct timer
  bool counters_ok = false;
  int saves = -1;                // saves made (the raw counter is one more)
  uint32_t timer = 0;            // play time, 30 per second
  char last_action[120] = {};
};
Status status();

// Requests from the panel (applied on the next tick, on the main thread).
void request_saves(int saves);             // the saves-made count
void request_playtime_seconds(int seconds);
void request_countdown_left(int seconds);  // only while a countdown runs

// Main thread.
void tick(bool in_game, bool options_open);
void remove_hooks();  // teardown: revert every patch

}  // namespace re1cc::cheats
