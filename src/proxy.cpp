// The mod ships as ddraw.dll, standing in front of the DirectDraw wrapper the
// game ships with (GOG's DirectDraw -> Direct3D 9 wrapper, renamed
// ddraw_orig.dll by `make install`). Every function the original exports (14 of
// them; the exe imports DirectDrawCreate) is a one-instruction thunk that jumps
// through a table of the real addresses - the stack is untouched, so the callee
// cleans up exactly as before, whatever its convention. The list of names lives
// in proxy_exports.inc, generated from the original DLL's export table by
// tools/gen_proxy.py; the .def aliases each name onto its thunk.
//
// Why ddraw: ResidentEvil.exe is Enigma Protector-wrapped, and of the DLLs its
// on-disk import table names, DDRAW is the only one that ships with the game.
// Proton loads a native ddraw.dll for this game on its own (its `proton` script
// sets ddraw=n,b for app 4249100, for GOG's wrapper), so the mod needs no
// WINEDLLOVERRIDES - version, winmm, dsound or msacm32 would.

#include "proxy.h"

#include <cstdio>

#include "log.h"
#include "proxy_exports.inc"

// Plain C symbol so the assembly below can name it as _g_proxy_orig.
extern "C" {
void* g_proxy_orig[PROXY_EXPORT_COUNT] = {};
}

// One thunk per export: `jmp [g_proxy_orig + i*4]`, defined at file scope in
// assembly so no prologue/epilogue is ever emitted around the jump.
#define PROXY_THUNK(i, name)                                   \
  asm(".text\n"                                                \
      ".globl _ddraw_" #i "\n"                                 \
      "_ddraw_" #i ":\n"                                       \
      "\tjmp *_g_proxy_orig+" #i "*4\n");
PROXY_EXPORTS(PROXY_THUNK)
#undef PROXY_THUNK

namespace re1cc::proxy {
namespace {

HMODULE g_orig = nullptr;

}  // namespace

HMODULE original() { return g_orig; }

bool load_original(const char* dir) {
  char path[MAX_PATH] = {};
  std::snprintf(path, sizeof(path), "%sddraw_orig.dll", dir);

  HMODULE orig = LoadLibraryA(path);
  if (!orig) {
    logf("FATAL: could not load %s (GetLastError=%lu). Did `make install` run?", path, GetLastError());
    return false;
  }
  // Pin it: nothing may drop the last reference while the game still uses it.
  HMODULE pinned = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &pinned);

  static const char* const kNames[] = {
#define PROXY_NAME(i, name) name,
      PROXY_EXPORTS(PROXY_NAME)
#undef PROXY_NAME
  };
  int missing = 0;
  for (int i = 0; i < PROXY_EXPORT_COUNT; ++i) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(orig, kNames[i]));
    g_proxy_orig[i] = fn;
    if (!fn) {
      ++missing;
      logf("FATAL: %s lacks export %s", path, kNames[i]);
    }
  }
  if (missing) return false;
  g_orig = orig;
  logf("forwarding %d DirectDraw exports -> %s", PROXY_EXPORT_COUNT, path);
  return true;
}

}  // namespace re1cc::proxy
