#include "input.h"

#include <cstring>

#include "game.h"
#include "image.h"
#include "log.h"
#include "mem.h"
#include "overlay.h"

namespace re1cc::input {
namespace {

using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
GetAsyncKeyStateFn g_real = nullptr;
volatile LONG g_wrapped = 0;

SHORT WINAPI hk_get_async_key_state(int vk) {
  // Escape stays the game's: it is how the option screen is left.
  if (vk != VK_ESCAPE && overlay::capturing_keyboard()) return 0;
  return g_real ? g_real(vk) : 0;
}

}  // namespace

FARPROC wrap(const char* name, FARPROC real) {
  if (std::strcmp(name, "GetAsyncKeyState") != 0 || !real) return nullptr;
  g_real = reinterpret_cast<GetAsyncKeyStateFn>(real);
  if (!InterlockedExchange(&g_wrapped, 1))
    logf("input: the game resolved GetAsyncKeyState through GetProcAddress - handed it the guard");
  return reinterpret_cast<FARPROC>(&hk_get_async_key_state);
}

void after_unpack() {
  HMODULE u32 = GetModuleHandleA("user32.dll");
  FARPROC real = u32 ? GetProcAddress(u32, "GetAsyncKeyState") : nullptr;
  if (!real) return;
  if (!g_real) g_real = reinterpret_cast<GetAsyncKeyStateFn>(real);
  game::hook_import("GetAsyncKeyState", reinterpret_cast<void*>(real), reinterpret_cast<void*>(&hk_get_async_key_state));
}

void uninstall() {
  // Wherever the guard sits in the game's import table, the real one goes back.
  if (!g_real) return;
  image::init();
  if (!image::idata().empty())
    mem::replace_dwords(image::idata(), reinterpret_cast<uint32_t>(&hk_get_async_key_state),
                        reinterpret_cast<uint32_t>(g_real));
}

}  // namespace re1cc::input
