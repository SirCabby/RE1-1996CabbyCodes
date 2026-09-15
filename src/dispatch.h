#pragma once

#include <windows.h>

// The main-thread tick. ResidentEvil.exe runs everything on its primary thread
// (a cooperative task scheduler switches stacks there; the decomp documents no
// other thread), and its message pump calls PeekMessageA every time round
// (WinMain, 0x441D71 in build 21744136), menus and all. Enigma rebuilds the
// game's import table with the real API addresses, so once the game is
// unpacked its PeekMessageA slot is re-pointed here, and everything that
// touches game memory runs from that callback - as in RE0CabbyCodes.
namespace re1cc::dispatch {

void init();                // from DllMain: records the primary thread
bool hook_message_pump();   // mod thread, once the game is unpacked
void unhook_message_pump();
void tick();                // from the PeekMessageA hook; ignored on any other thread

struct Snapshot {
  bool unpacked = false;      // the mod thread confirmed the game's code is unpacked
  bool game_ready = false;    // discovery finished
  bool in_game = false;       // a game is being played
  bool options_open = false;  // the game's option screen is up
  bool load_list = false;     // the title's Load Game list is up (the panel manages the save files)
  bool show_panel = false;    // the visibility rule, evaluated on the main thread
  unsigned long main_thread = 0;
  unsigned long long ticks = 0;
};
Snapshot snapshot();

bool on_main_thread();
HANDLE first_tick_event();    // signalled once, from the first tick
void set_unpacked(bool on);   // the mod thread's gate result

}  // namespace re1cc::dispatch
