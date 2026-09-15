#include "overlay.h"

#include <cstdio>
#include <cstring>

#include "cheats.h"
#include "config.h"
#include "dispatch.h"
#include "game.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "input.h"
#include "inventory.h"
#include "items.h"
#include "log.h"
#include "mem.h"
#include "proxy.h"
#include "savefiles.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace re1cc::overlay {
namespace {

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_orig_gpa = nullptr;
uintptr_t g_gpa_slot = 0;  // found once: matched by value, it cannot be found again while it holds our hook

HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
bool g_context_ready = false;
bool g_visible = false;
bool g_user_hidden = false;  // the toggle key, while the panel's screen is up

// The context lock (see overlay.h). Created in install(), from DllMain, before
// either path that takes it exists; never destroyed, because a frame can be in
// progress while the process tears down.
CRITICAL_SECTION g_imgui_cs;
bool g_imgui_cs_ready = false;

// What the panel is taking this frame, published for the input guard.
volatile LONG g_capture_mouse = 0;
volatile LONG g_capture_keyboard = 0;

// With Trace=1 the distinct names resolved through the exe's GetProcAddress
// import are logged: that slot is Enigma's loader's, so this shows whether it
// resolves the game's own imports through it - and which of them we can reach.
constexpr int kGpaSeen = 2048;  // Enigma's own runtime alone resolves more than 500 names
char g_gpa_seen[kGpaSeen][48];
volatile LONG g_gpa_n = 0;
volatile LONG g_gpa_calls = 0;

void trace_gpa(HMODULE module, const char* name) {
  if (!config::get().trace) return;
  const LONG n = g_gpa_n;
  for (LONG i = 0; i < n && i < kGpaSeen; ++i)
    if (!std::strncmp(g_gpa_seen[i], name, 47)) return;
  const LONG idx = InterlockedIncrement(&g_gpa_n) - 1;
  if (idx >= kGpaSeen) return;
  std::snprintf(g_gpa_seen[idx], sizeof(g_gpa_seen[idx]), "%s", name);
  char path[MAX_PATH] = "?";
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* leaf = std::strrchr(path, '\\');
  logf("trace: GetProcAddress(%s, %s) thread %lu", leaf ? leaf + 1 : path, name, GetCurrentThreadId());
}

FARPROC WINAPI hk_get_proc_address(HMODULE module, LPCSTR name) {
  FARPROC real = g_orig_gpa(module, name);
  InterlockedIncrement(&g_gpa_calls);
  if (!real || !name || !HIWORD(reinterpret_cast<uintptr_t>(name))) return real;
  trace_gpa(module, name);
  if (!config::get().disable_input)
    if (FARPROC w = input::wrap(name, real)) return w;
  return real;
}

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool showable = s.show_panel || config::get().always_show;
  // F-keys above F9 arrive as WM_SYSKEYDOWN; accept both, ignore auto-repeat.
  if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) &&
      static_cast<int>(wp) == config::get().toggle_key && showable) {
    ImGuiLock guard;  // wants_draw reads and clears this
    g_user_hidden = !g_user_hidden;
    logf("panel %s by the toggle key", g_user_hidden ? "hidden" : "shown");
    return 0;
  }
  // ImGui sees every message, visible or not: a button release that arrives
  // after the panel is hidden would otherwise never be delivered, leaving it
  // convinced the button is still held (and the window stuck to the pointer).
  if (g_context_ready) {
    bool swallow = false;
    {
      ImGuiLock guard;
      if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
      // Swallowing is the visible panel's business, and only for what it is
      // using. The game reads its keys with GetAsyncKeyState rather than from
      // here (input.cpp does the real work), and it reads no mouse at all.
      if (g_visible) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) swallow = true;
        if (io.WantTextInput && msg >= WM_KEYFIRST && msg <= WM_KEYLAST && wp != VK_ESCAPE) swallow = true;
      }
    }
    if (swallow) return 0;
  }
  // The game's own window procedure runs outside the lock.
  return CallWindowProcA(g_orig_wndproc, hwnd, msg, wp, lp);
}

// --- the pointer -------------------------------------------------------------------
// GOG's wrapper keeps the pointer inside the game picture with ClipCursor: the
// picture's rectangle (the centred 4:3 part of a widescreen window), set when
// the window activates and released when it deactivates - and the game's own
// ClipCursor calls it turns into no-ops. That leaves the panel, in the bars
// either side of the picture, out of the pointer's reach, and every other
// window and monitor too. So the wrapper's own ClipCursor import is hooked:
// while the panel is on screen the pointer is let go altogether - a clip the
// wrapper sets meanwhile is remembered and answered with a release - and when
// the panel goes, the wrapper's last clip is put back. Its releases always go
// through, and it releases when the window deactivates, so a clip is only ever
// put back while the game is the active window, never over whatever the player
// went to.
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
ClipCursorFn g_real_clip_cursor = nullptr;  // user32's, as the wrapper's import slot held it
uintptr_t g_clip_slot = 0;
CRITICAL_SECTION g_cursor_cs;     // created in install(), before the hook
bool g_wrapper_clipping = false;  // the wrapper's last call set a clip rather than releasing it
RECT g_wrapper_clip{};            // ...and this was the clip
bool g_cursor_free = false;       // the panel is up: nothing of ours or the wrapper's holds the pointer
volatile LONG g_clip_logs = 0;

// The whole desktop, every monitor, in screen coordinates: what GetClipCursor
// reports when nothing holds the pointer.
RECT desktop_rect() {
  const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  return RECT{x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

BOOL WINAPI hk_clip_cursor(const RECT* r) {
  EnterCriticalSection(&g_cursor_cs);
  g_wrapper_clipping = r != nullptr;
  if (r) g_wrapper_clip = *r;
  // A clip held back is still a success to the wrapper. It is answered with a
  // release rather than nothing, so a hold something else took on the same
  // occasion (Wine can confine a fullscreen window as it activates) goes too.
  const bool held_back = r && g_cursor_free;
  const BOOL ok = g_real_clip_cursor(held_back ? nullptr : r);
  LeaveCriticalSection(&g_cursor_cs);
  if (InterlockedIncrement(&g_clip_logs) <= 4) {
    if (r)
      logf("pointer: the wrapper clips it to %ld,%ld-%ld,%ld%s", r->left, r->top, r->right, r->bottom,
           held_back ? " - held back while the panel is up" : "");
    else
      logf("pointer: the wrapper releases its clip");
  }
  return ok;
}

// The cursor itself. The game hides it once, as its renderer starts full
// screen (ShowCursor(FALSE) at 0x497230 - its window class has the arrow), and
// the panel used to draw ImGui's own in its place. But an invisible cursor is
// also what makes gamescope hold the pointer: it switches to relative mouse
// mode, which locks the pointer to its window, whenever the focused window's
// cursor is hidden. So while the panel is on screen the game's own cursor is
// shown - ShowCursor(TRUE) until the display count is back at 0, looked at
// every frame, each call counted - and every call is taken back when the panel
// goes, which leaves the game's count exactly as it was. ImGui sets the
// cursor's shape instead of drawing one (io.MouseDrawCursor, wants_draw).
int g_cursor_shows = 0;  // ShowCursor(TRUE) calls of ours still to take back

void show_game_cursor(bool show) {
  if (show) {
    // The display count, read without changing it: one up, one down.
    int count = ShowCursor(TRUE) - 1;
    ShowCursor(FALSE);
    const bool first = g_cursor_shows == 0;
    while (count < 0 && g_cursor_shows < 64) {
      count = ShowCursor(TRUE);
      ++g_cursor_shows;
    }
    if (first && g_cursor_shows)
      logf("pointer: the game's cursor is shown while the panel is up (%d ShowCursor call(s), display count %d)",
           g_cursor_shows, count);
  } else if (g_cursor_shows) {
    const int n = g_cursor_shows;
    for (; g_cursor_shows > 0; --g_cursor_shows) ShowCursor(FALSE);
    logf("pointer: the game's cursor is hidden again (%d ShowCursor call(s) taken back)", n);
  }
}

// From wants_draw, every frame: the panel is on screen, or not.
void free_cursor(bool free) {
  if (config::get().disable_cursor) return;
  show_game_cursor(free);
  if (!g_real_clip_cursor) return;
  EnterCriticalSection(&g_cursor_cs);
  if (free != g_cursor_free) {
    g_cursor_free = free;
    if (free) g_real_clip_cursor(nullptr);
    else if (g_wrapper_clipping) g_real_clip_cursor(&g_wrapper_clip);
    // Whether it is really free is the system's answer, not ours: say so when
    // something else still holds it.
    const RECT desk = desktop_rect();
    RECT now{};
    const bool held = GetClipCursor(&now) && !EqualRect(&now, &desk);
    if (free && !held)
      logf("pointer: let go while the panel is up - it can leave the game for other windows and monitors");
    else if (free)
      logf("pointer: let go while the panel is up, but something else still holds it to %ld,%ld-%ld,%ld (the "
           "desktop is %ld,%ld-%ld,%ld)",
           now.left, now.top, now.right, now.bottom, desk.left, desk.top, desk.right, desk.bottom);
    else
      logf("pointer: %s", g_wrapper_clipping ? "back in the wrapper's clip" : "the wrapper holds no clip to put back");
  }
  LeaveCriticalSection(&g_cursor_cs);
}

void install_cursor_hook(HMODULE wrapper) {
  const uintptr_t slot = mem::iat_slot(wrapper, "USER32.dll", "ClipCursor");
  void* prev = slot ? mem::read<void*>(slot) : nullptr;
  if (!prev) {
    logf("overlay: ClipCursor is not in ddraw_orig.dll's import table - the pointer stays in the game picture");
    return;
  }
  g_real_clip_cursor = reinterpret_cast<ClipCursorFn>(prev);
  if (!mem::write<uintptr_t>(slot, reinterpret_cast<uintptr_t>(&hk_clip_cursor))) {
    g_real_clip_cursor = nullptr;
    logf("overlay: ddraw_orig.dll's ClipCursor import could not be written - the pointer stays in the game picture");
    return;
  }
  g_clip_slot = slot;
  logf("overlay: ddraw_orig.dll's ClipCursor import hooked (original %p)", prev);
}

// The part of the window the pointer can reach: all of it, or what a cursor
// clip leaves of it (the game picture, should letting the pointer go have
// failed, or something else still hold it). A panel outside it could never be
// clicked or dragged back.
ImVec4 reachable_rect(const ImVec2& display) {
  ImVec4 r(0.0f, 0.0f, display.x, display.y);
  RECT clip{};
  if (!g_hwnd || !GetClipCursor(&clip)) return r;
  POINT a{clip.left, clip.top}, b{clip.right, clip.bottom};
  ScreenToClient(g_hwnd, &a);
  ScreenToClient(g_hwnd, &b);
  const float l = static_cast<float>(a.x), t = static_cast<float>(a.y);
  const float rt = static_cast<float>(b.x), bm = static_cast<float>(b.y);
  const ImVec4 c(l > 0.0f ? l : 0.0f, t > 0.0f ? t : 0.0f, rt < display.x ? rt : display.x,
                 bm < display.y ? bm : display.y);
  if (c.z - c.x < 240.0f || c.w - c.y < 240.0f) return r;  // a clip that makes no sense is ignored
  return c;
}

// Keep the current window inside `r`: moved back whenever any part of it has
// ended up outside (a saved position from a wider reach, a panel that grew).
void keep_in_reach(const ImVec4& r) {
  const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
  ImVec2 want = pos;
  if (want.x + size.x > r.z) want.x = r.z - size.x;
  if (want.y + size.y > r.w) want.y = r.w - size.y;
  if (want.x < r.x) want.x = r.x;
  if (want.y < r.y) want.y = r.y;
  if (want.x != pos.x || want.y != pos.y) ImGui::SetWindowPos(want);
}

void cheat_row(cheats::Kind k, const char* label, const char* help, bool available, const char* unavailable_text) {
  bool v = cheats::enabled(k);
  if (!available) ImGui::BeginDisabled();
  if (ImGui::Checkbox(label, &v)) {
    cheats::set_enabled(k, v);
    logf("%s %s (panel)", cheats::name(k), v ? "ON" : "OFF");
  }
  if (!available) ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(380.0f);
    ImGui::TextUnformatted(help);
    if (!available && unavailable_text) {
      ImGui::Separator();
      ImGui::TextUnformatted(unavailable_text);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!available && unavailable_text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", unavailable_text);
  }
}

void hms(float seconds, char* out, size_t n) {
  if (seconds < 0.0f) {
    std::snprintf(out, n, "?");
    return;
  }
  const int t = static_cast<int>(seconds);
  std::snprintf(out, n, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
}

// One save file in a line: "Jill, 2:31:07, 12 saves", or "no data".
void describe_file(const savefiles::File& f, char* out, size_t n) {
  if (!f.known) {
    std::snprintf(out, n, "?");
    return;
  }
  if (!f.used) {
    std::snprintf(out, n, "no data");
    return;
  }
  if (f.character < 0) {
    std::snprintf(out, n, "%d bytes - not a save the game wrote?", f.size);
    return;
  }
  char t[32];
  hms(f.seconds, t, sizeof(t));
  std::snprintf(out, n, "%s, %s, %d save%s", f.character == 0 ? "Chris" : f.character == 1 ? "Jill" : "?", t, f.saves,
                f.saves == 1 ? "" : "s");
}

// The save file a Delete or Copy asked about, and what the panel saw of the
// files when it was clicked. Like RE0's, it belongs to the panel it was clicked
// in: a menu that closed and came back is asked again.
enum { kAskNone = 0, kAskDelete, kAskCopy };
int g_file_ask = kAskNone;
int g_file_from = -1, g_file_to = -1;
savefiles::File g_file_from_seen, g_file_to_seen;

void forget_file_dialogs() {
  g_file_ask = kAskNone;
  g_file_from = g_file_to = -1;
}

// The title menu's save files: every slot, with a Delete and a Copy for each one
// in use. Both change the file on disk the moment they are confirmed, so both
// ask first, and the request carries what was seen so a file that changed in
// between is left alone (savefiles.cpp).
void draw_save_files() {
  const savefiles::View v = savefiles::view();
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  bool open_delete = false, open_copy = false;

  ImGui::SeparatorText("Save files");
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(400.0f);
    ImGui::TextUnformatted(
        "The game's eight save files (SAVE\\savedat1..8.dat beside the game).\n\n"
        "Delete removes a file: the Load Game list shows NO DATA for it from then on, exactly like a file that was "
        "never used, and the next save into it fills it again. Copy to... puts a copy of a save into another file, "
        "empty or not.\n\n"
        "Both change the files on disk straight away, so neither can be undone - back up the SAVE folder first if in "
        "doubt. The game's list beside this one shows each change at once.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!v.ready) ImGui::TextColored(orange, "The save folder could not be found - see the log.");
  else if (!v.list_table) ImGui::TextDisabled("(looking for the game's list...)");
  const bool can = v.ready && v.up;

  if (v.files > 0 && ImGui::BeginTable("files", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("file");
    ImGui::TableSetupColumn("save");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (int i = 0; i < v.files; ++i) {
      const savefiles::File& f = v.file[i];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%d", i + 1);
      ImGui::TableNextColumn();
      char d[96];
      describe_file(f, d, sizeof(d));
      if (f.used) ImGui::TextUnformatted(d);
      else ImGui::TextDisabled("%s", d);
      ImGui::TableNextColumn();
      if (f.used) {
        ImGui::BeginDisabled(!can);
        if (ImGui::SmallButton("Copy to...")) ImGui::OpenPopup("copy to");
        ImGui::SameLine();
        // Without the list's own table a deleted file would still be offered for loading.
        ImGui::BeginDisabled(!v.list_table);
        const bool del = ImGui::SmallButton("Delete");
        ImGui::EndDisabled();
        if (del) {
          g_file_ask = kAskDelete;
          g_file_to = i;
          g_file_to_seen = f;
          open_delete = true;
        }
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("copy to")) {
          ImGui::TextDisabled("Copy save file %d to:", i + 1);
          ImGui::Separator();
          for (int j = 0; j < v.files; ++j) {
            if (j == i) continue;
            char dj[96], label[128];
            describe_file(v.file[j], dj, sizeof(dj));
            std::snprintf(label, sizeof(label), "File %d - %s", j + 1, dj);
            if (ImGui::Selectable(label)) {
              g_file_ask = kAskCopy;
              g_file_from = i;
              g_file_to = j;
              g_file_from_seen = f;
              g_file_to_seen = v.file[j];
              open_copy = true;
            }
          }
          ImGui::EndPopup();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (v.note[0]) {
    if (v.note_error) ImGui::PushStyleColor(ImGuiCol_Text, orange);
    ImGui::TextWrapped("%s", v.note);
    if (v.note_error) ImGui::PopStyleColor();
  }

  // Asked outside the table, so neither dialog belongs to a row.
  if (open_delete) ImGui::OpenPopup("delete this save?");
  if (open_copy) ImGui::OpenPopup("copy this save?");
  char d[96];
  if (ImGui::BeginPopupModal("delete this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_file_ask != kAskDelete || g_file_to < 0 || g_file_to >= v.files || !can) {
      forget_file_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      describe_file(g_file_to_seen, d, sizeof(d));
      ImGui::Text("Delete save file %d (%s)?", g_file_to + 1, d);
      ImGui::TextDisabled("The file is removed from the SAVE folder straight away, so this cannot be undone;");
      ImGui::TextDisabled("the game's list shows NO DATA for it at once.");
      if (ImGui::Button("Delete")) {
        savefiles::request_delete(g_file_to, g_file_to_seen);
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("copy this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_file_ask != kAskCopy || g_file_from < 0 || g_file_from >= v.files || g_file_to < 0 || g_file_to >= v.files ||
        !can) {
      forget_file_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      describe_file(g_file_from_seen, d, sizeof(d));
      if (g_file_to_seen.used) ImGui::Text("Copy save file %d (%s) over save file %d?", g_file_from + 1, d, g_file_to + 1);
      else ImGui::Text("Copy save file %d (%s) to the empty save file %d?", g_file_from + 1, d, g_file_to + 1);
      if (g_file_to_seen.used) {
        describe_file(g_file_to_seen, d, sizeof(d));
        ImGui::TextDisabled("The save in file %d (%s) is replaced.", g_file_to + 1, d);
      }
      ImGui::TextDisabled("The file on disk changes straight away, so this cannot be undone.");
      if (ImGui::Button("Copy")) {
        savefiles::request_copy(g_file_from, g_file_to, g_file_from_seen, g_file_to_seen);
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// --- the inventory and the item box ---------------------------------------------------
// An inventory row being edited (RE0's buffer). A dirty row stops following the
// live inventory - that is the point of it - so it must not outlive the panel
// it was typed into: reopening the option screen shows the inventory as it is
// now, not an edit abandoned before a save, a load or a pickup moved it on.
int g_edit_id[inventory::kBagMax], g_edit_cnt[inventory::kBagMax];
bool g_edit_dirty[inventory::kBagMax];
int g_ask_slot = -1;           // the row a confirmation is open for
int g_drop_slot = -1;          // the box slot the x button asked about...
inventory::Slot g_drop_seen;   // ...and what it held

void forget_item_dialogs() {
  std::memset(g_edit_dirty, 0, sizeof(g_edit_dirty));
  g_ask_slot = -1;
  g_drop_slot = -1;
}

// An item as the panel names it: its name, or its number for an id the table
// does not know (one of the game's unused items, say).
const char* item_label(int id, char* buf, size_t n) {
  if (id == 0) return "(empty)";
  if (items::find(id)) return items::name(id);
  std::snprintf(buf, n, "item 0x%02X", id);
  return buf;
}

// Case-insensitive substring match against "<id> 0x<id> <name>", so a list can
// be narrowed by typing a name, the bracketed name an item shows before it is
// examined, or an item number. The picker and the item box share it.
bool item_matches(int id, const char* name, const char* needle) {
  if (!needle || !needle[0]) return true;
  char hay[112];
  std::snprintf(hay, sizeof(hay), "%d 0x%02X %s", id, id, name);
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (needle[i] && lower(p[i]) == lower(needle[i])) ++i;
    if (!needle[i]) return true;
  }
  return false;
}

// Where a move would put an item, for a button's tooltip: the same answer the
// move itself acts on (inventory::fit_into_*).
void describe_fit(const inventory::Fit& f, int id, int count, const char* dest, const char* free_name) {
  const int topped = f.topped();
  const char* name = items::name(id);
  if (topped > 0 && f.slot >= 0)
    ImGui::Text("Put %d onto the %s %s already holds and %d into %s %d", topped, name, dest, f.into, free_name, f.slot + 1);
  else if (topped > 0)
    ImGui::Text("Put %d onto the %s %s already holds", topped, name, dest);
  else
    ImGui::Text("Put %s%s in %s %d", name, items::quantity(id, count).text, free_name, f.slot + 1);
  if (f.left > 0) ImGui::TextDisabled("The other %d do not fit and stay where they are", f.left);
}

// The inventory of the character in play, as an editable table. Edits live in
// a buffer until Apply, and both Apply and the discard ask first: they write to
// the game's inventory or throw away what was typed.
void draw_inventory(const inventory::View& v) {
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  const char* who = inventory::who_name(v.who);
  bool ask_apply = false, ask_discard = false;
  char lb[24];

  ImGui::SeparatorText("Inventory");
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(400.0f);
    ImGui::TextUnformatted(
        "The inventory of the character you are playing, straight from the game. Pick an item and a count for a slot "
        "and press Apply; Store moves the item into the item box below.\n\n"
        "The game keeps the inventory packed: an item put in an empty slot lands in the first empty one, and emptying "
        "a slot moves the items after it up, as the game's own item box does.\n\n"
        "E marks the equipped weapon. It can be stored or replaced like anything else: when the option screen closes "
        "the game equips whatever weapon is in that slot, and nothing if the slot no longer holds one.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!v.known) {
    ImGui::TextColored(orange, "The inventory was not found in this build - see the log.");
    return;
  }
  if (v.size == 0) {
    ImGui::TextDisabled("(the inventory cannot be read right now - see the log)");
    return;
  }
  ImGui::Text("%s - %d of %d slots used", who, v.held, v.size);
  if (!v.open) {
    ImGui::SameLine();
    ImGui::TextDisabled("(open the option screen to change it)");
  } else if (!v.frame) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520.0f);
    ImGui::TextColored(orange, "The option screen's record of the equipped weapon was not found: a change that would "
                               "unequip it or move it to another slot is refused (see the log).");
    ImGui::PopTextWrapPos();
  }

  ImGui::BeginDisabled(!v.open);
  if (ImGui::BeginTable("bag", 5, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("slot");
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn("");
    for (int s = 0; s < v.size && s < inventory::kBagMax; ++s) {
      const inventory::Slot live = v.bag[s];
      if (!g_edit_dirty[s]) {
        g_edit_id[s] = live.id;
        g_edit_cnt[s] = items::shown_count(live.id, live.count);
      }
      const int id = g_edit_id[s];
      ImGui::PushID(s);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%d%s", s + 1, s == v.equipped ? " E" : "");
      if (s == v.equipped && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("The equipped weapon");
        ImGui::EndTooltip();
      }

      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(250.0f);
      if (ImGui::BeginCombo("##item", item_label(id, lb, sizeof(lb)), ImGuiComboFlags_HeightLarge)) {
        auto pick = [&](int nid) {
          if (nid == g_edit_id[s]) return;
          g_edit_id[s] = nid;
          g_edit_cnt[s] = inventory::default_count(nid);
          g_edit_dirty[s] = true;
        };
        // The filter belongs to whichever list is open, so it starts empty and
        // with the caret in it every time one does.
        static char filter[48] = {};
        if (ImGui::IsWindowAppearing()) {
          filter[0] = '\0';
          ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputTextWithHint("##filter", "type to narrow the list", filter, sizeof(filter),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Separator();
        int first = -1, shown = 0;
        for (const items::Def& d : items::kTable) {
          const char* label = d.id ? d.name : "(empty)";
          if (!item_matches(d.id, label, filter)) continue;
          if (first < 0) first = d.id;
          ++shown;
          const bool sel = d.id == g_edit_id[s];
          if (ImGui::Selectable(label, sel)) pick(d.id);
          if (sel && !filter[0]) ImGui::SetItemDefaultFocus();
        }
        if (!shown) ImGui::TextDisabled("nothing matches \"%s\"", filter);
        if (enter && first >= 0) {  // Enter takes the first match
          pick(first);
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndCombo();
      }

      // The count, held to the game's own limits as it is typed, so the dialog
      // asks about the number that will actually be written.
      ImGui::TableNextColumn();
      const int limit = inventory::count_limit(id);
      if (id && items::infinite(id)) {
        ImGui::TextUnformatted("infinite");
      } else if (!id || !limit) {
        ImGui::TextDisabled("-");
      } else {
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("##cnt", &g_edit_cnt[s])) {
          const int lo = inventory::count_floor(id);
          if (g_edit_cnt[s] < lo) g_edit_cnt[s] = lo;
          if (g_edit_cnt[s] > limit) g_edit_cnt[s] = limit;
          g_edit_dirty[s] = true;
        }
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
          if (items::is_weapon(id)) ImGui::Text("Rounds loaded - the %s takes up to %d", items::name(id), limit);
          else if (game::item_stacks(id)) ImGui::Text("One slot holds up to %d %s", limit, items::name(id));
          else ImGui::TextUnformatted("How many uses are left - a key's count is the doors it still opens");
          ImGui::EndTooltip();
        }
      }

      ImGui::TableNextColumn();
      if (g_edit_dirty[s]) {
        if (ImGui::SmallButton("Apply")) {
          g_ask_slot = s;
          ask_apply = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
          g_ask_slot = s;
          ask_discard = true;
        }
      }

      // Into the item box, as the move itself would lay it out.
      ImGui::TableNextColumn();
      if (s < v.held && live.id) {
        const inventory::BoxFit f = inventory::fit_into_box(v, live.id, live.count);
        ImGui::BeginDisabled(!f.ok);
        if (ImGui::SmallButton("Store")) inventory::request_store(s, live);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
          const char* name = items::name(live.id);
          if (!f.ok)
            ImGui::Text("The item box has no free slot%s", game::item_stacks(live.id) ? ", and no stack of it with room" : "");
          else if (f.joined)
            ImGui::Text("Put %d in the item box with the %s there: x%d in %d stack%s", f.stored, name, f.total, f.stacks,
                        f.stacks == 1 ? "" : "s");
          else
            ImGui::Text("Put %s%s in box slot %d", name, items::quantity(live.id, f.stored).text, f.slot + 1);
          if (f.left > 0) ImGui::TextDisabled("The other %d do not fit and stay in the inventory", f.left);
          if (f.ok && s == v.equipped) ImGui::TextDisabled("It is equipped: storing it unequips it");
          ImGui::EndTooltip();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndDisabled();

  // Asked outside the table, so the dialog does not belong to a row that may
  // have been redrawn differently by the time it is answered - and outside the
  // disabled block, so an open dialog can always be answered or dismissed.
  if (ask_apply) ImGui::OpenPopup("apply this change?");
  if (ask_discard) ImGui::OpenPopup("discard this edit?");
  const int as = g_ask_slot;
  if (ImGui::BeginPopupModal("apply this change?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (as < 0 || as >= v.size || !g_edit_dirty[as] || !v.open) {
      g_ask_slot = -1;
      ImGui::CloseCurrentPopup();
    } else {
      const inventory::Slot live = as < v.held ? v.bag[as] : inventory::Slot{};
      const int id = g_edit_id[as], cnt = g_edit_cnt[as];
      if (!id)
        ImGui::Text("Empty %s slot %d (%s%s)?", who, as + 1, item_label(live.id, lb, sizeof(lb)),
                    items::quantity(live.id, live.count).text);
      else if (as >= v.held)
        ImGui::Text("Put %s%s in %s's first empty slot (slot %d)?", items::name(id), items::quantity(id, cnt).text, who,
                    v.held + 1);
      else
        ImGui::Text("Set %s slot %d to %s%s?", who, as + 1, items::name(id), items::quantity(id, cnt).text);
      ImGui::TextDisabled("This writes straight into the game's inventory.");
      if (as == v.equipped && live.id)
        ImGui::TextDisabled(items::is_weapon(id) ? "The slot is equipped: the game equips this weapon when the option "
                                                   "screen closes."
                                                 : "The slot is equipped: this unequips it.");
      if (ImGui::Button("Apply")) {
        inventory::request_set(as, id, cnt, live);
        g_edit_dirty[as] = false;
        g_ask_slot = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        g_ask_slot = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("discard this edit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (as < 0 || as >= v.size || !g_edit_dirty[as] || !v.open) {
      g_ask_slot = -1;
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::Text("Throw away the edit to %s slot %d?", who, as + 1);
      ImGui::TextDisabled("The row goes back to showing what the inventory holds.");
      if (ImGui::Button("Discard")) {
        g_edit_dirty[as] = false;
        g_ask_slot = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Keep editing")) {
        g_ask_slot = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// The game's item box, listed one bank at a time and sorted by name - the game
// keeps its 48 slots in whatever order things were put in, and # says which
// slot an entry is in on the game's own item box screen.
void draw_item_box(const inventory::View& v) {
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  bool ask_drop = false;
  char lb[24];

  ImGui::SeparatorText("Item box");
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(400.0f);
    ImGui::TextUnformatted(
        "The game's own item box: the same 48 slots every item box in the game opens, saved with your game. Take puts "
        "an item into the inventory of the character you are playing, x throws it away, and Store beside an inventory "
        "slot puts one in.\n\n"
        "Ammo and ink ribbons stored here are combined with the same item already in the box, in stacks as big as one "
        "slot holds; taken out, they top up the stack you carry first, as a pickup does. Everything else takes a slot "
        "of its own. What fits nowhere stays where it was.\n\n"
        "The list is filed under five banks - key items, weapons, ammo, heals, and everything else - and each bank is "
        "sorted by name. # is the slot an item is in on the game's item box screen.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!v.known || v.box_size == 0) {
    ImGui::TextDisabled("(the item box cannot be read right now - see the log)");
    return;
  }
  // The used slots in listing order: bank, name, the fullest stack first, slot.
  int order[inventory::kBoxMax];
  int n = 0;
  for (int i = 0; i < v.box_size; ++i)
    if (v.box[i].id) order[n++] = i;
  const auto before = [&](int a, int b) {
    const inventory::Slot &x = v.box[a], &y = v.box[b];
    if (x.id != y.id) return items::listed_before(x.id, y.id);
    if (x.count != y.count) return x.count > y.count;
    return a < b;
  };
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0 && before(order[j], order[j - 1]); --j) std::swap(order[j], order[j - 1]);

  ImGui::Text("%d of %d slots used", n, v.box_size);
  if (!v.rules_known)
    ImGui::TextColored(orange, "Not all of the game's item rules could be read - the mod's own list stands in (see the log).");

  // One bank on show at a time, with every tab carrying its count, and the
  // item picker's filter, kept across frames and banks and cleared by hand.
  static int shown_bank = items::kBankKey;
  static char filter[48] = {};
  int held[items::kBankCount] = {}, matched[items::kBankCount] = {};
  for (int k = 0; k < n; ++k) {
    const int id = v.box[order[k]].id;
    const int bk = items::bank(id);
    ++held[bk];
    if (item_matches(id, item_label(id, lb, sizeof(lb)), filter)) ++matched[bk];
  }
  if (ImGui::BeginTabBar("banks")) {
    for (int k = 0; k < items::kBankCount; ++k) {
      char tab[48];
      std::snprintf(tab, sizeof(tab), "%s %d###bank%d", items::bank_name(k), held[k], k);
      if (ImGui::BeginTabItem(tab)) {
        shown_bank = k;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  const int bank = shown_bank;
  const int shown = matched[bank];
  if (n > 0) {
    ImGui::SetNextItemWidth(230.0f);
    ImGui::InputTextWithHint("##box filter", "type to narrow the list", filter, sizeof(filter));
    if (filter[0]) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) filter[0] = '\0';
      ImGui::SameLine();
      ImGui::TextDisabled("%d of %d shown", shown, held[bank]);
    }
  }

  char table_id[16];  // each bank keeps its own scroll position
  std::snprintf(table_id, sizeof(table_id), "box%d", bank);
  if (n == 0) {
    ImGui::TextDisabled("The item box is empty - use Store beside an inventory slot to put something in it.");
  } else if (held[bank] == 0) {
    ImGui::TextDisabled("Nothing in the box is filed under %s.", items::bank_name(bank));
  } else if (shown == 0) {
    char elsewhere[160] = "";
    int len = 0;
    for (int k = 0; k < items::kBankCount; ++k)
      if (k != bank && matched[k] && len < static_cast<int>(sizeof(elsewhere)))
        len += std::snprintf(elsewhere + len, sizeof(elsewhere) - len, "%s%d under %s", len ? ", " : " - ", matched[k],
                             items::bank_name(k));
    ImGui::TextDisabled("nothing under %s matches \"%s\"%s", items::bank_name(bank), filter, elsewhere);
  } else if (ImGui::BeginTable(table_id, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                               ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * (shown > 12 ? 12.5f : shown + 0.5f)))) {
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("");
    const char* who = inventory::who_name(v.who);
    for (int k = 0; k < n; ++k) {
      const int i = order[k];
      const inventory::Slot e = v.box[i];
      const char* label = item_label(e.id, lb, sizeof(lb));
      if (items::bank(e.id) != bank || !item_matches(e.id, label, filter)) continue;
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(label);
      ImGui::TableNextColumn();
      if (items::infinite(e.id)) ImGui::TextUnformatted("infinite");
      else if (e.id == items::kKnifeId) ImGui::TextDisabled("-");
      else ImGui::Text("%d", items::shown_count(e.id, e.count));
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%d", i + 1);
      ImGui::TableNextColumn();
      const inventory::Fit f = v.size ? inventory::fit_into_bag(v, e.id, e.count) : inventory::Fit{};
      ImGui::BeginDisabled(!v.open || !f.ok());
      if (ImGui::SmallButton("Take")) inventory::request_take(i, e);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
        if (!v.open) ImGui::TextUnformatted("Open the option screen to take it");
        else if (!f.ok())
          ImGui::Text("%s has no free slot%s", who, game::item_stacks(e.id) ? ", and no stack of it with room" : "");
        else describe_fit(f, e.id, e.count, who, "slot");
        ImGui::EndTooltip();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!v.open);
      if (ImGui::SmallButton("x")) {
        g_drop_slot = i;
        g_drop_seen = e;
        ask_drop = true;
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("Throw this item away");
        ImGui::EndTooltip();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // Asked outside the table so the dialog is not scoped to a row that may be
  // gone by the time it is answered.
  if (ask_drop) ImGui::OpenPopup("throw item away?");
  if (ImGui::BeginPopupModal("throw item away?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_drop_slot < 0 || !v.open) {
      g_drop_slot = -1;
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::Text("Throw away %s%s from box slot %d?", item_label(g_drop_seen.id, lb, sizeof(lb)),
                  items::quantity(g_drop_seen.id, g_drop_seen.count).text, g_drop_slot + 1);
      ImGui::TextDisabled("It is gone for good once the game is saved.");
      if (ImGui::Button("Throw away")) {
        inventory::request_drop(g_drop_slot, g_drop_seen);
        g_drop_slot = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        g_drop_slot = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

}  // namespace

bool ensure_context(HWND hwnd) {
  if (g_context_ready) return true;
  if (!hwnd) return false;
  g_hwnd = hwnd;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // Window position persists beside the DLL, under the mod's own name rather
  // than a stray imgui.ini in the game folder.
  static char ini_path[MAX_PATH] = {};
  std::snprintf(ini_path, sizeof(ini_path), "%sRE1-1996CabbyCodes.imgui.ini", config::dir());
  io.IniFilename = ini_path;
  // No keyboard navigation: the game's menus keep their keys. Dragging is
  // limited to the title bar so a click on the panel's background cannot pick
  // the window up by accident.
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().WindowRounding = 4.0f;

  if (!ImGui_ImplWin32_Init(hwnd)) {
    logf("ERROR: ImGui Win32 backend init failed");
    ImGui::DestroyContext();
    return false;
  }
  g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));
  g_context_ready = true;
  char cls[64] = "?";
  GetClassNameA(hwnd, cls, sizeof(cls));
  logf("overlay ready (hwnd=%p class '%s', present thread %lu, window thread %lu)", static_cast<void*>(hwnd), cls,
       GetCurrentThreadId(), GetWindowThreadProcessId(hwnd, nullptr));
  return true;
}

// Published by the frame that drew (draw_panel), cleared by the frame that did
// not (wants_draw): a plain read, from any thread, of what the panel took.
bool capturing_mouse() { return g_capture_mouse != 0; }
bool capturing_keyboard() { return g_capture_keyboard != 0; }

void lock_imgui() {
  if (g_imgui_cs_ready) EnterCriticalSection(&g_imgui_cs);
}

void unlock_imgui() {
  if (g_imgui_cs_ready) LeaveCriticalSection(&g_imgui_cs);
}

// Called from the present hook with the context lock held.
bool wants_draw() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool showable = s.show_panel || config::get().always_show;
  if (!showable) {
    g_user_hidden = false;  // the next time the screen comes up the panel is back
    forget_file_dialogs();
    forget_item_dialogs();
  }
  g_visible = g_context_ready && showable && !g_user_hidden;
  free_cursor(g_visible);
  if (!g_visible) {  // nothing is being taken from the game until it draws again
    InterlockedExchange(&g_capture_mouse, 0);
    InterlockedExchange(&g_capture_keyboard, 0);
  }
  ImGuiIO& io = ImGui::GetIO();
  // While the panel is up the game's own cursor is shown (show_game_cursor) and
  // ImGui only sets its shape; it draws one of its own only when that is turned
  // off (Disable = cursor), the game's cursor then staying hidden.
  io.MouseDrawCursor = g_visible && config::get().disable_cursor;
  if (g_visible) {
    // If a release was ever missed the panel would follow the pointer for ever;
    // the physical button is the truth.
    static const int kVk[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    for (int b = 0; b < 3; ++b)
      if (io.MouseDown[b] && !(GetAsyncKeyState(kVk[b]) & 0x8000)) io.AddMouseButtonEvent(b, false);
  } else if (g_context_ready) {
    // Input made while the panel is away is not the panel's: ImGui only queues
    // events and drains them in NewFrame, which does not run while the panel is
    // hidden, so they would be replayed onto it the moment it came back (RE0).
    io.ClearEventsQueue();
    io.ClearInputMouse();
    io.ClearInputKeys();
  }
  return g_visible;
}

// Called from the present hook, between NewFrame and Render, with the context
// lock held.
void draw_panel() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const cheats::Status st = cheats::status();
  const ImGuiIO& io = ImGui::GetIO();
  // NewFrame has run, so what ImGui wants this frame is settled; the input
  // guard reads these from the game's thread.
  InterlockedExchange(&g_capture_mouse, io.WantCaptureMouse ? 1 : 0);
  InterlockedExchange(&g_capture_keyboard, io.WantTextInput ? 1 : 0);
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  const ImVec4 reach = reachable_rect(io.DisplaySize);
  ImGui::SetNextWindowPos(ImVec2(reach.z - 460, reach.y + 60), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Resident Evil - Cabby Codes  v" RE1CC_VERSION, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  keep_in_reach(reach);
  if (!s.unpacked) {
    ImGui::TextColored(orange, "Waiting for the game to unpack...");
    ImGui::End();
    return;
  }
  if (!s.game_ready) {
    ImGui::TextColored(orange, "Game hooks: %s", game::status_text());
    ImGui::End();
    return;
  }
  if (s.load_list) {
    // The title's Load Game list: the panel is the save file manager, and
    // nothing else on it applies before a game is running.
    draw_save_files();
    ImGui::Separator();
    ImGui::TextDisabled("F%d hides this panel", config::get().toggle_key - 0x6F);
    ImGui::End();
    return;
  }
  if (!s.in_game) {
    ImGui::TextDisabled("Waiting for a game (start a new game or load a save)...");
    ImGui::Separator();
  }

  ImGui::BeginDisabled(!s.in_game);
  ImGui::SeparatorText("Player");
  cheat_row(cheats::kGodMode, "God mode",
            "Your health is held at its maximum and the poison that drains it is cured, every frame. Yawn's poison is "
            "left for the serum - the story needs it - and with your health held it cannot hurt you.",
            st.player_ok, "player not found");
  if (st.player_ok && st.player.max_hp > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d / %d HP)", st.player.hp, st.player.max_hp);
  }
  cheat_row(cheats::kOneHitKills, "One hit kills",
            "Every live enemy is held at 0 HP - the last value the game still counts as alive - so your next hit kills "
            "it; deaths and drops play out through the game's own code. Bosses whose fights are scripted around their "
            "health may end sooner.",
            st.enemies_ok, "enemy table not found");
  if (st.enemies_ok) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d enemies)", st.enemies);
  }
  cheat_row(cheats::kInfiniteAmmo, "Infinite ammo",
            "Firing no longer uses up ammo: every gun keeps its loaded rounds, the flamethrower its fuel. Pickups and "
            "reloads still work as usual.",
            st.site_ammo, "patch sites not found");

  ImGui::SeparatorText("Saves & time");
  cheat_row(cheats::kInfiniteInk, "Infinite ink ribbons",
            "Saving at a typewriter never uses up a ribbon, and you can save without carrying one at all - the "
            "typewriter takes the game's own no-ribbon path.",
            st.site_ink, "patch sites not found");
  cheat_row(cheats::kNoSaveCount, "Save without counting",
            "Saving does not add to the save count the ending shows. The game counts a save again when its file is "
            "loaded, so a save made with this on is written one lower and loads back the count you had.",
            st.site_savecount, "patch site not found");
  {
    static int edit_saves = -1;
    if (edit_saves < 0 && st.saves >= 0) edit_saves = st.saves;
    ImGui::BeginDisabled(!st.counters_ok);
    ImGui::Text("Saves: %d", st.saves);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt("##saves", &edit_saves);
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply##saves")) cheats::request_saves(edit_saves < 0 ? 0 : edit_saves);
    ImGui::EndDisabled();
  }
  {
    char t[32];
    hms(st.counters_ok ? static_cast<float>(st.timer) / 30.0f : -1.0f, t, sizeof(t));
    cheat_row(cheats::kFreezePlaytime, "Freeze play time",
              "The play-time clock the ending ranks you on stops - in the inventory too.", st.site_playtime || st.counters_ok,
              "clock not found");
    if (st.playtime_hold) {
      ImGui::SameLine();
      ImGui::TextColored(orange, "(hold mode)");
    }
    static int eh = 0, em = 0, es = 0;
    ImGui::BeginDisabled(!st.counters_ok);
    ImGui::Text("Play time: %s", t);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("h", &eh, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("m", &em, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("s", &es, 0, 0);
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##pt")) {
      eh = eh < 0 ? 0 : eh;
      em = em < 0 ? 0 : (em > 59 ? 59 : em);
      es = es < 0 ? 0 : (es > 59 ? 59 : es);
      cheats::request_playtime_seconds(eh * 3600 + em * 60 + es);
    }
    ImGui::EndDisabled();
  }
  {
    // The self-destruct countdown: it only exists while the sequence runs, so the
    // editor stays disabled until the game starts it.
    char t[32] = "-";
    if (st.countdown_active && st.countdown_left >= 0)
      std::snprintf(t, sizeof(t), "%d:%02d", st.countdown_left / 60, st.countdown_left % 60);
    cheat_row(cheats::kFreezeCountdown, "Freeze countdown timer",
              "The self-destruct countdown stops where it is while this is on, and runs on the moment you switch it "
              "off. Nothing happens while no countdown is running.",
              st.site_countdown || st.countdown_known, "countdown not found");
    if (st.countdown_hold) {
      ImGui::SameLine();
      ImGui::TextColored(orange, "(hold mode)");
    }
    static int cm = 0, cs = 0;
    ImGui::BeginDisabled(!st.countdown_active);
    ImGui::Text("Countdown: %s", t);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("m##cd", &cm, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("s##cd", &cs, 0, 0);
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##cd")) {
      int left = (cm < 0 ? 0 : cm) * 60 + (cs < 0 ? 0 : (cs > 59 ? 59 : cs));
      if (left > game::kCountdownLimit) left = game::kCountdownLimit;
      cheats::request_countdown_left(left);
    }
    ImGui::EndDisabled();
    if (st.countdown_known && !st.countdown_active) ImGui::TextDisabled("(no countdown is running)");
  }
  if (st.last_action[0]) ImGui::TextWrapped("%s", st.last_action);
  {
    // The inventory and the item box: the game's own, changed only while the
    // option screen is up (inventory.cpp).
    const inventory::View iv = inventory::view();
    draw_inventory(iv);
    draw_item_box(iv);
    // Only a move that was refused says so here: what a move did shows in the
    // lists themselves, and the log keeps every one.
    if (iv.note_error && iv.note[0]) {
      ImGui::PushStyleColor(ImGuiCol_Text, orange);
      ImGui::TextWrapped("%s", iv.note);
      ImGui::PopStyleColor();
    }
  }
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::TextDisabled("F%d hides this panel", config::get().toggle_key - 0x6F);
  ImGui::End();
}

void shutdown_imgui() {
  if (!g_context_ready) return;
  ImGui_ImplWin32_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  g_context_ready = false;
  g_visible = false;
  InterlockedExchange(&g_capture_mouse, 0);
  InterlockedExchange(&g_capture_keyboard, 0);
}

bool install() {
  if (!g_imgui_cs_ready) {  // before the window procedure, the present hook or the ClipCursor hook exists
    InitializeCriticalSection(&g_imgui_cs);
    InitializeCriticalSection(&g_cursor_cs);
    g_imgui_cs_ready = true;
  }
  bool d3d = false;
  if (HMODULE wrapper = proxy::original()) {
    d3d = dx9::install_import(wrapper);
    if (!config::get().disable_cursor) install_cursor_hook(wrapper);
  }
  if (!config::get().disable_gpa) {
    HMODULE exe = GetModuleHandleA(nullptr);
    const uintptr_t slot = mem::iat_slot(exe, "kernel32.dll", "GetProcAddress");
    void* prev = slot ? mem::read<void*>(slot) : nullptr;
    if (prev && mem::write<uintptr_t>(slot, reinterpret_cast<uintptr_t>(&hk_get_proc_address))) {
      g_orig_gpa = reinterpret_cast<GetProcAddressFn>(prev);
      g_gpa_slot = slot;
      game::note_iat_slot(slot, reinterpret_cast<uintptr_t>(prev));
      logf("overlay: the exe's GetProcAddress import (Enigma's loader) hooked at exe+0x%X (original %p)%s",
           static_cast<unsigned>(slot - reinterpret_cast<uintptr_t>(exe)), prev,
           config::get().trace ? " - tracing names" : "");
    } else {
      logf("overlay: GetProcAddress is not in the exe's import table - no view of what Enigma resolves");
    }
  }
  return d3d;
}

void uninstall() {
  // Window procedure first: a message arriving after our image is gone would
  // jump into freed memory.
  if (g_orig_wndproc && g_hwnd && IsWindow(g_hwnd)) {
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }
  dx9::uninstall();
  shutdown_imgui();
  if (g_clip_slot && mem::read<uintptr_t>(g_clip_slot) == reinterpret_cast<uintptr_t>(&hk_clip_cursor))
    mem::write<uintptr_t>(g_clip_slot, reinterpret_cast<uintptr_t>(g_real_clip_cursor));
  g_clip_slot = 0;
  if (g_gpa_slot && g_orig_gpa &&
      mem::read<uintptr_t>(g_gpa_slot) == reinterpret_cast<uintptr_t>(&hk_get_proc_address))
    mem::write<uintptr_t>(g_gpa_slot, reinterpret_cast<uintptr_t>(g_orig_gpa));
  g_orig_gpa = nullptr;
  g_gpa_slot = 0;
  logf("overlay hooks removed");
}

}  // namespace re1cc::overlay
