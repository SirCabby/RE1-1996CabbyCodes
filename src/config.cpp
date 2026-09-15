#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace re1cc::config {
namespace {

Settings g_settings;
char g_path[MAX_PATH] = {};
char g_dir[MAX_PATH] = {};

bool truthy(const char* v) {
  return !_stricmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "yes");
}

void write_defaults() {
  FILE* f = std::fopen(g_path, "w");
  if (!f) return;
  std::fprintf(f,
               "# RE1-1996CabbyCodes\n"
               "#\n"
               "#   ToggleKey          = 0x76  ; virtual-key code that hides/shows the panel (0x76 = F7)\n"
               "#   GodMode            = 0     ; cheats switched on when the game starts (the panel changes\n"
               "#   OneHitKills        = 0     ; them for the session)\n"
               "#   InfiniteAmmo       = 0\n"
               "#   InfiniteInkRibbons = 0\n"
               "#   NoSaveCount        = 0     ; save without incrementing the save counter\n"
               "#   FreezePlaytime     = 0\n"
               "#   FreezeCountdown    = 0     ; stop the self-destruct countdown\n"
               "#   AlwaysShow         = 0     ; debug: draw the panel everywhere, not only on its screens\n"
               "#   Trace              = 0     ; verbose diagnostics in RE1-1996CabbyCodes.log\n"
               "#   DumpImage          = 0     ; write the unpacked game image beside the DLL once\n"
               "#   Watch              =       ; debug: 0xBE41C0:4,0xD22777:1 - log those values as they change\n"
               "#   Disable            =       ; comma list of subsystems to turn off: overlay,dispatch,game,gpa,input,cursor\n"
               "ToggleKey = 0x76\n"
               "GodMode = 0\n"
               "OneHitKills = 0\n"
               "InfiniteAmmo = 0\n"
               "InfiniteInkRibbons = 0\n"
               "NoSaveCount = 0\n"
               "FreezePlaytime = 0\n"
               "FreezeCountdown = 0\n"
               "AlwaysShow = 0\n"
               "Trace = 0\n"
               "DumpImage = 0\n"
               "Watch =\n"
               "Disable =\n");
  std::fclose(f);
}

// "0xBE41C0:4, 0xD22777:1" -> the watch list (sizes 1, 2 or 4; 4 when omitted).
void parse_watch(const char* val) {
  g_settings.nwatch = 0;
  const char* p = val;
  while (*p && g_settings.nwatch < kMaxWatch) {
    while (*p == ' ' || *p == '\t' || *p == ',') ++p;
    if (!*p) break;
    char* end = nullptr;
    const unsigned long addr = std::strtoul(p, &end, 0);
    if (end == p) break;
    p = end;
    unsigned size = 4;
    if (*p == ':') {
      size = static_cast<unsigned>(std::strtoul(p + 1, &end, 0));
      p = end;
    }
    if (addr && (size == 1 || size == 2 || size == 4)) {
      g_settings.watch[g_settings.nwatch].addr = static_cast<unsigned>(addr);
      g_settings.watch[g_settings.nwatch].size = size;
      ++g_settings.nwatch;
    }
    while (*p && *p != ',') ++p;
  }
}

}  // namespace

const Settings& get() { return g_settings; }
const char* dir() { return g_dir; }

void load(const char* dir) {
  std::snprintf(g_dir, sizeof(g_dir), "%s", dir);
  std::snprintf(g_path, sizeof(g_path), "%sRE1-1996CabbyCodes.ini", dir);
  FILE* f = std::fopen(g_path, "r");
  if (!f) {
    write_defaults();
    logf("config: no %s - wrote one with the defaults", g_path);
    return;
  }
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\r' || !*p) continue;
    char* eq = std::strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = p;
    char* val = eq + 1;
    for (char* e = key + std::strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
    while (*val == ' ' || *val == '\t') ++val;
    // A trailing "; comment" is the file's own documentation style.
    if (char* c = std::strchr(val, ';')) *c = '\0';
    for (char* e = val + std::strlen(val);
         e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
      *--e = '\0';

    if (!_stricmp(key, "ToggleKey")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v > 0 && v < 256) g_settings.toggle_key = v;
    } else if (!_stricmp(key, "GodMode")) {
      g_settings.god_mode = truthy(val);
    } else if (!_stricmp(key, "OneHitKills")) {
      g_settings.one_hit_kills = truthy(val);
    } else if (!_stricmp(key, "InfiniteAmmo")) {
      g_settings.infinite_ammo = truthy(val);
    } else if (!_stricmp(key, "InfiniteInkRibbons")) {
      g_settings.infinite_ink = truthy(val);
    } else if (!_stricmp(key, "NoSaveCount")) {
      g_settings.no_save_count = truthy(val);
    } else if (!_stricmp(key, "FreezePlaytime")) {
      g_settings.freeze_playtime = truthy(val);
    } else if (!_stricmp(key, "FreezeCountdown")) {
      g_settings.freeze_countdown = truthy(val);
    } else if (!_stricmp(key, "AlwaysShow")) {
      g_settings.always_show = truthy(val);
    } else if (!_stricmp(key, "Trace")) {
      g_settings.trace = truthy(val);
    } else if (!_stricmp(key, "DumpImage")) {
      g_settings.dump_image = truthy(val);
    } else if (!_stricmp(key, "Watch")) {
      parse_watch(val);
    } else if (!_stricmp(key, "Disable")) {
      g_settings.disable_overlay = std::strstr(val, "overlay") != nullptr;
      g_settings.disable_dispatch = std::strstr(val, "dispatch") != nullptr;
      g_settings.disable_game = std::strstr(val, "game") != nullptr;
      g_settings.disable_gpa = std::strstr(val, "gpa") != nullptr;
      g_settings.disable_input = std::strstr(val, "input") != nullptr;
      g_settings.disable_cursor = std::strstr(val, "cursor") != nullptr;
    } else {
      logf("config: unknown key '%s' ignored", key);
    }
  }
  std::fclose(f);
  logf("config: ToggleKey=0x%02X GodMode=%d OneHitKills=%d InfiniteAmmo=%d InfiniteInkRibbons=%d NoSaveCount=%d "
       "FreezePlaytime=%d FreezeCountdown=%d AlwaysShow=%d Trace=%d DumpImage=%d Watch=%d value(s) Disable=%s%s%s%s%s%s",
       g_settings.toggle_key, g_settings.god_mode, g_settings.one_hit_kills, g_settings.infinite_ammo,
       g_settings.infinite_ink, g_settings.no_save_count, g_settings.freeze_playtime, g_settings.freeze_countdown,
       g_settings.always_show, g_settings.trace, g_settings.dump_image, g_settings.nwatch,
       g_settings.disable_overlay ? "overlay " : "", g_settings.disable_dispatch ? "dispatch " : "",
       g_settings.disable_game ? "game " : "", g_settings.disable_gpa ? "gpa " : "",
       g_settings.disable_input ? "input " : "", g_settings.disable_cursor ? "cursor" : "");
}

}  // namespace re1cc::config
