#include "dispatch.h"

#include "cheats.h"
#include "config.h"
#include "crash.h"
#include "game.h"
#include "input.h"
#include "inventory.h"
#include "log.h"
#include "overlay.h"
#include "savefiles.h"

namespace re1cc::dispatch {
namespace {

DWORD g_main_thread = 0;  // the process's primary thread: DllMain(PROCESS_ATTACH) runs on it
using PeekMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
PeekMessageAFn g_real_peek = nullptr;
DWORD g_last_rates = 0;
Snapshot g_snap;
DWORD g_last_tick = 0;
DWORD g_last_filter = 0;
volatile LONG g_first_tick = 0;
volatile LONG g_unpacked = 0;
volatile LONG g_foreign_logged = 0;
HANDLE g_first_event = nullptr;
bool g_imports_done = false;
bool g_in_game_prev = false;
bool g_options_prev = false;
bool g_list_prev = false;

// Once the image is unpacked, the game's rebuilt import table can be reached:
// its GetAsyncKeyState and SetUnhandledExceptionFilter slots are re-pointed to
// our hooks when Enigma left the real addresses there. (The GetProcAddress
// hook may already have handed the game ours; then nothing matches.)
void hooks_after_unpack() {
  if (!config::get().disable_input) input::after_unpack();
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  if (void* real = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "SetUnhandledExceptionFilter")) : nullptr)
    game::hook_import("SetUnhandledExceptionFilter", real, crash_filter_hook());
}

BOOL WINAPI hk_peek_message(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
  // Other threads pump messages too; the game runs on the primary thread, and
  // so must everything that touches it.
  if (GetCurrentThreadId() == g_main_thread) tick();
  return g_real_peek(msg, hwnd, min, max, remove);
}

}  // namespace

bool hook_message_pump() {
  HMODULE u32 = GetModuleHandleA("user32.dll");
  FARPROC real = u32 ? GetProcAddress(u32, "PeekMessageA") : nullptr;
  if (!real) return false;
  g_real_peek = reinterpret_cast<PeekMessageAFn>(real);
  return game::hook_import("PeekMessageA", reinterpret_cast<void*>(real), reinterpret_cast<void*>(&hk_peek_message)) > 0;
}

void unhook_message_pump() {
  if (!g_real_peek) return;
  game::hook_import("PeekMessageA (restore)", reinterpret_cast<void*>(&hk_peek_message), reinterpret_cast<void*>(g_real_peek));
}

void init() {
  g_main_thread = GetCurrentThreadId();
  g_first_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
}

bool on_main_thread() { return GetCurrentThreadId() == g_main_thread; }

void tick() {
  if (GetCurrentThreadId() != g_main_thread) {
    if (!InterlockedExchange(&g_foreign_logged, 1))
      logf("dispatch: a tick came in on thread %lu, not the game's %lu - ignored", GetCurrentThreadId(), g_main_thread);
    return;
  }
  // The pump spins many times per frame.
  const DWORD now = GetTickCount();
  if (now - g_last_tick < 16) return;
  g_last_tick = now;

  if (!g_first_tick) {
    InterlockedExchange(&g_first_tick, 1);
    if (g_first_event) SetEvent(g_first_event);
    logf("main-thread tick running on thread %lu (the game's PeekMessageA)", GetCurrentThreadId());
  }
  if (now - g_last_filter > 1000) {
    g_last_filter = now;
    reassert_crash_filter();
  }
  if (now - g_last_rates >= 1000) {
    g_last_rates = now;
    overlay::dx9::log_rates();
  }

  Snapshot s;
  s.main_thread = g_main_thread;
  s.ticks = g_snap.ticks + 1;
  s.unpacked = g_unpacked != 0;
  if (s.unpacked && !g_imports_done) {
    g_imports_done = true;
    hooks_after_unpack();
  }
  if (s.unpacked) game::trace_tick();
  s.game_ready = game::ready();
  if (s.game_ready) {
    s.in_game = game::in_game();
    s.options_open = s.in_game && game::options_open();
    s.load_list = !s.in_game && game::load_list();
    if (s.in_game != g_in_game_prev) {
      logf("game session %s", s.in_game ? "started" : "ended");
      g_in_game_prev = s.in_game;
    }
    if (s.options_open != g_options_prev) {
      if (config::get().trace) logf("option screen %s", s.options_open ? "opened" : "closed");
      g_options_prev = s.options_open;
    }
    if (s.load_list != g_list_prev) {
      logf("the Load Game list is %s", s.load_list ? "up" : "gone");
      g_list_prev = s.load_list;
    }
    // The cheats run every tick, menus or not: holds are re-asserted every
    // frame, and a switch flipped on the option screen takes effect at once.
    cheats::tick(s.in_game, s.options_open);
    // The inventory and the item box change only while the option screen is
    // up; the tick after it closes is the one that rebuilds the icons.
    inventory::tick(s.in_game, s.options_open);
    // The list keeps its own table of the files in task 0's stack (see savefiles.h).
    const uintptr_t esp = s.load_list ? game::task_saved_esp(0) : 0;
    savefiles::tick(s.load_list, esp, esp ? esp + 0x4000 : 0);
  }
  s.show_panel = s.options_open || s.load_list;
  g_snap = s;
}

Snapshot snapshot() { return g_snap; }
HANDLE first_tick_event() { return g_first_event; }
void set_unpacked(bool on) { InterlockedExchange(&g_unpacked, on ? 1 : 0); }

}  // namespace re1cc::dispatch
