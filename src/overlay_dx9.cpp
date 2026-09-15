// D3D9 backend. The game draws through GOG's DirectDraw wrapper
// (ddraw_orig.dll), which imports Direct3DCreate9 statically: that import slot
// is patched right after the proxy loads the wrapper, and from the returned
// IDirect3D9 we hook CreateDevice (slot 16). On the device: Present (17),
// CreateAdditionalSwapChain (13), EndScene (42) and Reset (16); on its swap
// chains: Present (3).
//
// The wrapper does not present through IDirect3DDevice9::Present - a whole
// session drew the game without one call to it (2026-09-13) - so every present
// path is hooked, the panel is drawn by whichever one the wrapper uses, once
// per frame, into that swap chain's back buffer, and the log says which path
// it is. The main-thread tick does not come from here: dispatch.cpp hooks the
// game's own PeekMessageA.

#include <windows.h>
#include <d3d9.h>

#include "overlay.h"

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "config.h"
#include "crash.h"
#include "log.h"
#include "mem.h"

namespace re1cc::overlay::dx9 {
namespace {

using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(__stdcall*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                           IDirect3DDevice9**);
using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SwapPresentFn = HRESULT(__stdcall*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using CreateSwapFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**);
using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

Create9Fn g_real_create9 = nullptr;
CreateDeviceFn g_orig_create_device = nullptr;
PresentFn g_orig_present = nullptr;
SwapPresentFn g_orig_swap_present = nullptr;
CreateSwapFn g_orig_create_swap = nullptr;
EndSceneFn g_orig_end_scene = nullptr;
ResetFn g_orig_reset = nullptr;
uintptr_t g_d3d_vt = 0;
uintptr_t g_device_vt = 0;
uintptr_t g_swap_vt = 0;
uintptr_t g_iat_slot = 0;
IDirect3DDevice9* g_device = nullptr;
HWND g_hwnd = nullptr;
bool g_renderer_ready = false;
// The device's own Present may hand over to its swap chain's through the
// vtable; the panel must not be drawn twice in one frame.
bool g_in_device_present = false;

// Per-second call counts (dispatch.cpp's tick reads them once a second).
Stats g_stats;
volatile LONG g_presents = 0, g_swap_presents = 0, g_end_scenes = 0;
volatile LONG g_first_present = 0, g_first_swap_present = 0, g_first_end_scene = 0;
int g_rate_logs = 0;

bool ensure_renderer(IDirect3DDevice9* device) {
  if (g_renderer_ready) return true;
  if (!g_hwnd) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(device->GetCreationParameters(&cp))) g_hwnd = cp.hFocusWindow;
  }
  if (!ensure_context(g_hwnd)) return false;
  if (!ImGui_ImplDX9_Init(device)) {
    logf("ERROR: dx9: ImGui DX9 backend init failed");
    return false;
  }
  g_renderer_ready = true;
  return true;
}

// The panel, into `back` (the back buffer of the swap chain being presented),
// in a scene of its own. The state block puts the device state back; it does
// not cover render targets, so the old one is restored first - and the viewport
// with the state block after it.
void draw_into(IDirect3DDevice9* device, IDirect3DSurface9* back) {
  IDirect3DStateBlock9* state = nullptr;
  device->CreateStateBlock(D3DSBT_ALL, &state);
  IDirect3DSurface9* old_rt = nullptr;
  device->GetRenderTarget(0, &old_rt);
  device->SetRenderTarget(0, back);
  if (SUCCEEDED(device->BeginScene())) {
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_panel();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    device->EndScene();
  }
  if (old_rt) {
    device->SetRenderTarget(0, old_rt);
    old_rt->Release();
  }
  if (state) {
    state->Apply();
    state->Release();
  }
}

void panel_frame(IDirect3DDevice9* device, IDirect3DSurface9* back) {
  // The window procedure feeds this same context.
  ImGuiLock guard;
  if (ensure_renderer(device) && wants_draw()) draw_into(device, back);
}

void note_first(volatile LONG* flag, const char* what, HWND wnd, const RECT* dst) {
  if (InterlockedExchange(flag, 1)) return;
  if (dst)
    logf("dx9: first %s on thread %lu (hwnd=%p, dst %ld,%ld-%ld,%ld)", what, GetCurrentThreadId(), static_cast<void*>(wnd),
         dst->left, dst->top, dst->right, dst->bottom);
  else
    logf("dx9: first %s on thread %lu (hwnd=%p)", what, GetCurrentThreadId(), static_cast<void*>(wnd));
}

HRESULT __stdcall hk_present(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND wnd,
                             const RGNDATA* dirty) {
  InterlockedIncrement(&g_presents);
  g_stats.present_thread = GetCurrentThreadId();
  note_first(&g_first_present, "IDirect3DDevice9::Present", wnd, dst);
  IDirect3DSurface9* back = nullptr;
  if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
    panel_frame(device, back);
    back->Release();
  }
  g_in_device_present = true;
  const HRESULT hr = g_orig_present(device, src, dst, wnd, dirty);
  g_in_device_present = false;
  return hr;
}

HRESULT __stdcall hk_swap_present(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd,
                                  const RGNDATA* dirty, DWORD flags) {
  InterlockedIncrement(&g_swap_presents);
  g_stats.present_thread = GetCurrentThreadId();
  note_first(&g_first_swap_present, "IDirect3DSwapChain9::Present", wnd, dst);
  if (!g_in_device_present) {
    IDirect3DDevice9* device = nullptr;
    if (SUCCEEDED(sc->GetDevice(&device)) && device) {
      IDirect3DSurface9* back = nullptr;
      if (SUCCEEDED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
        panel_frame(device, back);
        back->Release();
      }
      device->Release();
    }
  }
  return g_orig_swap_present(sc, src, dst, wnd, dirty, flags);
}

HRESULT __stdcall hk_end_scene(IDirect3DDevice9* device) {
  InterlockedIncrement(&g_end_scenes);
  note_first(&g_first_end_scene, "IDirect3DDevice9::EndScene", nullptr, nullptr);
  return g_orig_end_scene(device);
}

// Every swap chain of the device shares one class, so the first one's vtable
// covers the rest; a different vtable is logged rather than hooked.
void hook_swap_chain(IDirect3DSwapChain9* sc, const char* which) {
  const auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(sc));
  if (!g_orig_swap_present) {
    g_orig_swap_present =
        reinterpret_cast<SwapPresentFn>(mem::hook_vtable(vt, 3, reinterpret_cast<void*>(&hk_swap_present)));
    g_swap_vt = vt;
    logf("dx9: %s swap chain %p: Present hooked", which, static_cast<void*>(sc));
  } else if (vt != g_swap_vt) {
    logf("dx9: %s swap chain %p has another vtable %p - not hooked", which, static_cast<void*>(sc),
         reinterpret_cast<void*>(vt));
  }
}

HRESULT __stdcall hk_create_swap(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** out) {
  const HRESULT hr = g_orig_create_swap(device, pp, out);
  if (SUCCEEDED(hr) && out && *out) {
    logf("dx9: the wrapper created an additional swap chain (%ux%u, hwnd=%p)", pp ? pp->BackBufferWidth : 0u,
         pp ? pp->BackBufferHeight : 0u, pp ? static_cast<void*>(pp->hDeviceWindow) : nullptr);
    hook_swap_chain(*out, "additional");
  }
  return hr;
}

HRESULT __stdcall hk_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
  // The real Reset stays outside the lock: it can block on the GPU, and the
  // window procedure must not wait that out.
  if (g_renderer_ready) {
    ImGuiLock guard;
    ImGui_ImplDX9_InvalidateDeviceObjects();
  }
  const HRESULT hr = g_orig_reset(device, pp);
  if (pp) {
    g_stats.backbuffer_w = pp->BackBufferWidth;
    g_stats.backbuffer_h = pp->BackBufferHeight;
    g_stats.windowed = pp->Windowed != FALSE;
    logf("dx9: Reset to %ux%u %s (hr=0x%08lX)", pp->BackBufferWidth, pp->BackBufferHeight,
         pp->Windowed ? "windowed" : "fullscreen", static_cast<unsigned long>(hr));
  }
  if (g_renderer_ready) {
    ImGuiLock guard;
    ImGui_ImplDX9_CreateDeviceObjects();
  }
  return hr;
}

HRESULT __stdcall hk_create_device(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                   D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
  const HRESULT hr = g_orig_create_device(self, adapter, type, focus, flags, pp, out);
  if (FAILED(hr) || !out || !*out) return hr;
  g_device = *out;
  if (focus) g_hwnd = focus;
  else if (pp && pp->hDeviceWindow) g_hwnd = pp->hDeviceWindow;
  if (pp) {
    g_stats.backbuffer_w = pp->BackBufferWidth;
    g_stats.backbuffer_h = pp->BackBufferHeight;
    g_stats.windowed = pp->Windowed != FALSE;
  }
  char cls[64] = "?";
  if (g_hwnd) GetClassNameA(g_hwnd, cls, sizeof(cls));
  RECT rc{};
  if (g_hwnd) GetClientRect(g_hwnd, &rc);
  const auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(g_device));
  if (!g_orig_present) {
    g_orig_present = reinterpret_cast<PresentFn>(mem::hook_vtable(vt, 17, reinterpret_cast<void*>(&hk_present)));
    g_orig_create_swap =
        reinterpret_cast<CreateSwapFn>(mem::hook_vtable(vt, 13, reinterpret_cast<void*>(&hk_create_swap)));
    g_orig_end_scene = reinterpret_cast<EndSceneFn>(mem::hook_vtable(vt, 42, reinterpret_cast<void*>(&hk_end_scene)));
    g_orig_reset = reinterpret_cast<ResetFn>(mem::hook_vtable(vt, 16, reinterpret_cast<void*>(&hk_reset)));
    g_device_vt = vt;
    char where[MAX_PATH + 32];
    re1cc::describe_address(reinterpret_cast<void*>(g_orig_present), where, sizeof(where));
    logf("dx9: device %p created on thread %lu (hwnd=%p class '%s' client %ldx%ld, backbuffer %ux%u %s, fmt %u, "
         "flags 0x%lX); hooked Present/CreateAdditionalSwapChain/EndScene/Reset (Present was %s)",
         static_cast<void*>(g_device), GetCurrentThreadId(), static_cast<void*>(g_hwnd), cls, rc.right - rc.left,
         rc.bottom - rc.top, pp ? pp->BackBufferWidth : 0u, pp ? pp->BackBufferHeight : 0u,
         pp && pp->Windowed ? "windowed" : "fullscreen", pp ? static_cast<unsigned>(pp->BackBufferFormat) : 0u,
         static_cast<unsigned long>(flags), where);
  } else if (vt != g_device_vt) {
    logf("dx9: another device %p with a different vtable - not hooked", static_cast<void*>(g_device));
  }
  IDirect3DSwapChain9* sc = nullptr;
  if (SUCCEEDED(g_device->GetSwapChain(0, &sc)) && sc) {
    hook_swap_chain(sc, "implicit");
    sc->Release();
  }
  return hr;
}

IDirect3D9* WINAPI hk_create9(UINT sdk) {
  IDirect3D9* d3d = g_real_create9(sdk);
  if (d3d && !g_orig_create_device) {
    const auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(d3d));
    g_orig_create_device =
        reinterpret_cast<CreateDeviceFn>(mem::hook_vtable(vt, 16, reinterpret_cast<void*>(&hk_create_device)));
    g_d3d_vt = vt;
    logf("dx9: the wrapper called Direct3DCreate9 (thread %lu); hooked IDirect3D9::CreateDevice", GetCurrentThreadId());
  }
  return d3d;
}

}  // namespace

bool install_import(HMODULE wrapper) {
  void* prev = mem::iat_hook(wrapper, "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&hk_create9));
  if (!prev) {
    logf("ERROR: could not hook Direct3DCreate9 in ddraw_orig.dll's import table - no overlay");
    return false;
  }
  g_real_create9 = reinterpret_cast<Create9Fn>(prev);
  g_iat_slot = mem::iat_slot(wrapper, "d3d9.dll", "Direct3DCreate9");
  logf("overlay: ddraw_orig.dll's Direct3DCreate9 import hooked (original %p)", prev);
  return true;
}

Stats stats() { return g_stats; }

// Main thread, once a second: the counts of the last second, logged for the
// first ten seconds with a device (and whenever they change, with Trace = 1).
void log_rates() {
  const auto p = static_cast<unsigned>(InterlockedExchange(&g_presents, 0));
  const auto sp = static_cast<unsigned>(InterlockedExchange(&g_swap_presents, 0));
  const auto es = static_cast<unsigned>(InterlockedExchange(&g_end_scenes, 0));
  const bool changed = p != g_stats.present_per_s || sp != g_stats.swap_present_per_s || es != g_stats.end_scene_per_s;
  g_stats.present_per_s = p;
  g_stats.swap_present_per_s = sp;
  g_stats.end_scene_per_s = es;
  if (!g_device) return;
  if (g_rate_logs < 10 || (config::get().trace && changed)) {
    ++g_rate_logs;
    logf("dx9: last second: %u device Present, %u swap-chain Present, %u EndScene", p, sp, es);
  }
}

void uninstall() {
  if (g_device_vt && mem::readable(reinterpret_cast<void*>(g_device_vt), 64 * sizeof(void*))) {
    if (g_orig_present) mem::write<void*>(g_device_vt + 17 * sizeof(void*), reinterpret_cast<void*>(g_orig_present));
    if (g_orig_create_swap)
      mem::write<void*>(g_device_vt + 13 * sizeof(void*), reinterpret_cast<void*>(g_orig_create_swap));
    if (g_orig_end_scene)
      mem::write<void*>(g_device_vt + 42 * sizeof(void*), reinterpret_cast<void*>(g_orig_end_scene));
    if (g_orig_reset) mem::write<void*>(g_device_vt + 16 * sizeof(void*), reinterpret_cast<void*>(g_orig_reset));
  }
  if (g_swap_vt && g_orig_swap_present && mem::readable(reinterpret_cast<void*>(g_swap_vt), 4 * sizeof(void*)))
    mem::write<void*>(g_swap_vt + 3 * sizeof(void*), reinterpret_cast<void*>(g_orig_swap_present));
  if (g_d3d_vt && mem::readable(reinterpret_cast<void*>(g_d3d_vt), 17 * sizeof(void*)) && g_orig_create_device)
    mem::write<void*>(g_d3d_vt + 16 * sizeof(void*), reinterpret_cast<void*>(g_orig_create_device));
  if (g_iat_slot && g_real_create9 && mem::read<uintptr_t>(g_iat_slot) == reinterpret_cast<uintptr_t>(&hk_create9))
    mem::write<uintptr_t>(g_iat_slot, reinterpret_cast<uintptr_t>(g_real_create9));
  g_orig_present = nullptr;
  g_orig_swap_present = nullptr;
  g_orig_create_swap = nullptr;
  g_orig_end_scene = nullptr;
  g_orig_reset = nullptr;
  g_orig_create_device = nullptr;
  if (g_renderer_ready) {
    ImGui_ImplDX9_Shutdown();
    g_renderer_ready = false;
  }
}

}  // namespace re1cc::overlay::dx9
