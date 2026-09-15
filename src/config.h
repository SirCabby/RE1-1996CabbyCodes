#pragma once

namespace re1cc::config {

// One value the Watch diagnostic logs whenever it changes (reverse engineering
// aid only: the addresses are the user's, typed into the ini; nothing the mod
// does depends on them).
struct Watch {
  unsigned addr = 0;
  unsigned size = 0;  // 1, 2 or 4 bytes
};
constexpr int kMaxWatch = 24;

struct Settings {
  int toggle_key = 0x76;      // VK_F7: hide/show the panel while it is up
  // Initial state of each cheat when the game starts; the panel changes them
  // for the session.
  bool god_mode = false;
  bool one_hit_kills = false;
  bool infinite_ammo = false;
  bool infinite_ink = false;
  bool no_save_count = false;
  bool freeze_playtime = false;
  bool freeze_countdown = false;
  bool trace = false;         // verbose diagnostics
  bool always_show = false;   // debug: draw the panel everywhere, not only on its screens
  bool dump_image = false;    // write the unpacked exe image beside the DLL once
  int nwatch = 0;             // "Watch = 0xBE41C0:4,0xD22777:1" - log those values as they change
  Watch watch[kMaxWatch];
  // "Disable = overlay,dispatch,game,gpa,input,cursor" turns whole subsystems
  // off so a fault can be bisected to one of them.
  bool disable_overlay = false;
  bool disable_dispatch = false;
  bool disable_game = false;
  bool disable_gpa = false;     // the exe's GetProcAddress import hook (Enigma resolves through it)
  bool disable_input = false;   // the GetAsyncKeyState guard while the panel captures input
  bool disable_cursor = false;  // letting the pointer go while the panel is up (GOG's wrapper keeps it in the picture)
};

const Settings& get();

// RE1-1996CabbyCodes.ini next to the DLL. Written with documented defaults the
// first time so the options are discoverable.
void load(const char* dir);
const char* dir();  // the DLL's directory, with a trailing separator

}  // namespace re1cc::config
