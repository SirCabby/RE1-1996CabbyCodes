// RE1-1996CabbyCodes - proxy entry point.
//
// We ship as ddraw.dll (see proxy.cpp for why). The Enigma-wrapped exe imports
// DDRAW statically, so DllMain runs before the wrapper has unpacked the game.
// DllMain therefore only loads the original wrapper, patches import slots (the
// wrapper's Direct3DCreate9, the exe's GetProcAddress) and starts a thread;
// that thread polls the game's code section until Enigma has unpacked it into
// plain x86, and only then reads anything of the game - and re-points the
// game's own PeekMessageA import, which is where the main-thread tick runs.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "cheats.h"
#include "config.h"
#include "crash.h"
#include "dispatch.h"
#include "game.h"
#include "input.h"
#include "inventory.h"
#include "log.h"
#include "overlay.h"
#include "proxy.h"
#include "savefiles.h"
#include "version.h"

namespace {

HMODULE g_self = nullptr;
char g_dir[MAX_PATH] = {};

// Directory this DLL was loaded from, with a trailing separator.
void resolve_own_dir() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(g_self, path, MAX_PATH);
  char* slash = std::strrchr(path, '\\');
  if (!slash) slash = std::strrchr(path, '/');
  if (slash) {
    size_t len = static_cast<size_t>(slash - path) + 1;
    if (len < sizeof(g_dir)) {
      std::memcpy(g_dir, path, len);
      g_dir[len] = '\0';
    }
  }
}

// A window's text without sending it a message: GetWindowText asks the
// window's thread for it and waits, and a thread that is not pumping would hold
// the mod thread for ever. InternalGetWindowText reads the stored text.
void window_text(HWND h, char* out, int n) {
  wchar_t w[256] = {};
  InternalGetWindowText(h, w, 256);
  if (!WideCharToMultiByte(CP_ACP, 0, w, -1, out, n, nullptr, nullptr)) out[0] = '\0';
  out[n - 1] = '\0';
}

// "class 'title'" for each of this process's top-level windows, and for a
// dialog the text of its static controls - an error box from Enigma or from
// the game says here what it says on screen.
struct WindowList {
  char text[512];
  size_t len;
};

BOOL CALLBACK list_child(HWND child, LPARAM lp) {
  auto* w = reinterpret_cast<WindowList*>(lp);
  char cls[32] = {}, t[160] = {};
  GetClassNameA(child, cls, sizeof(cls));
  if (_stricmp(cls, "Static") != 0 || w->len >= sizeof(w->text) - 1) return TRUE;
  window_text(child, t, sizeof(t));
  if (!t[0]) return TRUE;
  w->len += std::snprintf(w->text + w->len, sizeof(w->text) - w->len, " \"%s\"", t);
  return TRUE;
}

BOOL CALLBACK list_window(HWND hwnd, LPARAM lp) {
  auto* w = reinterpret_cast<WindowList*>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || w->len >= sizeof(w->text) - 1) return TRUE;
  char cls[64] = {}, title[96] = {};
  GetClassNameA(hwnd, cls, sizeof(cls));
  window_text(hwnd, title, sizeof(title));
  w->len += std::snprintf(w->text + w->len, sizeof(w->text) - w->len, "%s[%s '%s'", w->len ? "; " : "", cls, title);
  if (!std::strcmp(cls, "#32770")) EnumChildWindows(hwnd, list_child, lp);
  if (w->len < sizeof(w->text) - 1) w->len += std::snprintf(w->text + w->len, sizeof(w->text) - w->len, "]");
  return TRUE;
}

DWORD WINAPI mod_thread(LPVOID) {
  using namespace re1cc;
  logf("mod thread started (thread %lu)", GetCurrentThreadId());
  // 1. Enigma unpacks the game: its code section turns from compressed bytes
  //    into x86 (prologues), and the game's first present follows. Nothing
  //    here waits for a frame - a game that is not drawing (its main loop
  //    stops while its window has no focus, and an error box stops
  //    everything) still gets its image surveyed - and every 5 s the log says
  //    what the process is doing, so a hang is never a log that just stops.
  HANDLE ev = dispatch::first_tick_event();
  int prologues = 0, pads = 0;
  bool unpacked = false;
  for (int s = 1; s <= 300 && !unpacked; ++s) {
    Sleep(1000);
    unpacked = game::unpacked(&prologues, &pads);
    if (!unpacked && (s == 1 || s % 5 == 0)) {
      WindowList w{};
      EnumWindows(list_window, reinterpret_cast<LPARAM>(&w));
      logf("after %d s: game code still packed (%d prologues); windows: %s", s, prologues, w.len ? w.text : "none");
    }
  }
  if (!unpacked) {
    logf("game code STILL PACKED after 300 s - no game hooks");
    return 0;
  }
  // Enigma rebuilds the game's import table right after it unpacks the code;
  // give it a moment before anything reads the table.
  Sleep(2000);
  logf("game code unpacked: %d prologues, %d int3 pads in the first 256 KB", prologues, pads);
  dispatch::set_unpacked(true);
  if (!config::get().disable_dispatch) {
    if (dispatch::hook_message_pump()) logf("dispatch: the game's PeekMessageA now ticks the mod");
    else logf("ERROR: dispatch: the game's PeekMessageA slot was not found - no main-thread tick");
  }
  if (config::get().disable_game) {
    logf("game hooks disabled by config");
    return 0;
  }
  game::recon(g_dir);
  for (int attempt = 1; attempt <= 10; ++attempt) {
    if (game::discover()) break;
    logf("discovery attempt %d did not find the screens - retrying in 2 s", attempt);
    Sleep(2000);
  }
  // 2. Until the first tick, keep saying what is on screen.
  for (int s = 5; ev && s <= 120 && WaitForSingleObject(ev, 5000) == WAIT_TIMEOUT; s += 5) {
    WindowList w{};
    EnumWindows(list_window, reinterpret_cast<LPARAM>(&w));
    logf("%d s after the unpack: no tick yet; windows: %s", s, w.len ? w.text : "none");
  }
  return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  using namespace re1cc;
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      g_self = module;
      DisableThreadLibraryCalls(module);
      resolve_own_dir();
      log_init(g_dir);
      logf("RE1-1996CabbyCodes " RE1CC_VERSION " loaded (ddraw.dll proxy), dir=%s pid=%lu thread=%lu", g_dir,
           GetCurrentProcessId(), GetCurrentThreadId());

      if (!proxy::load_original(g_dir)) {
        MessageBoxA(nullptr,
                    "RE1-1996CabbyCodes: ddraw_orig.dll is missing or broken.\n\n"
                    "The mod ships as ddraw.dll and needs the game's original ddraw.dll beside it, "
                    "renamed to ddraw_orig.dll. See INSTALL.txt / README.md, or verify the game files "
                    "in Steam and reinstall the mod.",
                    "RE1-1996CabbyCodes", MB_OK | MB_ICONERROR);
        return FALSE;
      }

      install_crash_logger();
      config::load(g_dir);
      cheats::init();
      inventory::init();
      savefiles::init();
      dispatch::init();

      // Import-table patches only - no game reads under the loader lock.
      if (!config::get().disable_overlay) overlay::install();

      if (HANDLE t = CreateThread(nullptr, 0, mod_thread, nullptr, 0, nullptr)) CloseHandle(t);
      break;
    }
    case DLL_PROCESS_DETACH:
      // Undo every patch before this image goes away: anything still pointing
      // into the DLL would fault the moment the game touched it during its own
      // teardown.
      logf("unloading - removing hooks");
      dispatch::unhook_message_pump();
      cheats::remove_hooks();
      input::uninstall();
      overlay::uninstall();
      logf("hooks removed cleanly");
      log_shutdown();
      break;
    default:
      break;
  }
  return TRUE;
}
