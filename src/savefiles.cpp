#include "savefiles.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"
#include "mem.h"

namespace re1cc::savefiles {

bool same_save(const File& a, const File& b) {
  return a.known == b.known && a.used == b.used && a.size == b.size && a.hash == b.hash;
}

namespace {

CRITICAL_SECTION g_lock;  // the view, the note and the request queue
bool g_ready = false;
View g_view;              // what the panel was last handed
char g_note[200] = {};
bool g_note_error = false;
bool g_showing = false;   // main thread only
char g_folder[MAX_PATH] = {};
File g_files[kFiles];     // the last reading, main thread only
DWORD g_last_read = 0;
// The Load Game list's own table (see savefiles.h): 9 entries of these, found by
// content in [g_search_lo, g_search_hi) while the list is up.
struct GameEntry {
  uint32_t character, saves, stage, room, has_data;
};
constexpr int kGameEntries = 9;  // the eight files and savedat9.dat, which the list probes too
uintptr_t g_table = 0;
uintptr_t g_search_lo = 0, g_search_hi = 0;
DWORD g_last_search = 0;

struct Request {
  enum Type { kDelete, kCopy } type;
  int from, to;
  File from_seen, to_seen;
};
Request g_req[8];
int g_req_n = 0;

void note(bool error, const char* fmt, ...) {
  char text[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  logf("save files: %s", text);
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  std::snprintf(g_note, sizeof(g_note), "%s", text);
  g_note_error = error;
  LeaveCriticalSection(&g_lock);
}

// The game opens SAVE\savedat%d.dat relative to its working directory (the
// decomp's SaveLoadScreen.cpp); the panel reads the very same files.
void resolve_folder() {
  char cwd[MAX_PATH] = {};
  const DWORD n = GetCurrentDirectoryA(MAX_PATH, cwd);
  if (!n || n >= MAX_PATH - 8) {
    g_folder[0] = '\0';
    return;
  }
  std::snprintf(g_folder, sizeof(g_folder), "%s%sSAVE\\", cwd, cwd[n - 1] == '\\' ? "" : "\\");
}

void path_of(int slot, char* out, size_t n) { std::snprintf(out, n, "%ssavedat%d.dat", g_folder, slot + 1); }

// false with ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND in GetLastError for a
// missing file; any other failure (a sharing violation mid-write) is not a
// missing file.
bool read_all(const char* path, std::vector<uint8_t>* out) {
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const DWORD size = GetFileSize(h, nullptr);
  bool ok = size != INVALID_FILE_SIZE && size <= 0x10000;
  if (ok) {
    out->resize(size);
    DWORD got = 0;
    ok = !size || (ReadFile(h, out->data(), size, &got, nullptr) && got == size);
  }
  CloseHandle(h);
  if (!ok) SetLastError(ERROR_READ_FAULT);
  return ok;
}

uint32_t fnv1a(const std::vector<uint8_t>& b) {
  uint32_t h = 2166136261u;
  for (uint8_t c : b) h = (h ^ c) * 16777619u;
  return h;
}

File describe_bytes(const std::vector<uint8_t>& b) {
  File f;
  f.known = true;
  f.used = true;
  f.size = static_cast<int>(b.size());
  f.hash = fnv1a(b);
  if (b.size() >= 0x22C) {
    f.stage = b[0x200];
    f.room = b[0x201];
    uint32_t t = 0;
    std::memcpy(&t, &b[0x224], 4);
    f.seconds = static_cast<float>(t) / 30.0f;
    f.saves = b[0x228];
    f.char_byte = b[0x22B];
    f.character = b[0x22B] & 3;
  }
  return f;
}

File read_slot(int slot, std::vector<uint8_t>* bytes = nullptr) {
  char p[MAX_PATH];
  path_of(slot, p, sizeof(p));
  std::vector<uint8_t> b;
  if (read_all(p, &b)) {
    if (bytes) *bytes = b;
    return describe_bytes(b);
  }
  const DWORD e = GetLastError();
  File f;
  f.known = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
  return f;
}

// The bytes into `path` through a temp file in the same folder and one rename,
// so the file is either what it was or all of the new one.
bool write_atomic(const char* path, const std::vector<uint8_t>& b) {
  char tmp[MAX_PATH + 16];
  std::snprintf(tmp, sizeof(tmp), "%s.re1cc-new", path);
  HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &put, nullptr) && put == b.size();
  ok = FlushFileBuffers(h) && ok;
  CloseHandle(h);
  if (ok) ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!ok) DeleteFileA(tmp);
  return ok;
}

// "file 3 (Jill, 1:02:03, 7 saves)", for the log and the notes.
const char* describe(int slot, const File& f, char* buf, size_t n) {
  if (!f.used) {
    std::snprintf(buf, n, "file %d (empty)", slot + 1);
    return buf;
  }
  char t[24] = "?";
  if (f.seconds >= 0.0f) {
    const int s = static_cast<int>(f.seconds);
    std::snprintf(t, sizeof(t), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  }
  std::snprintf(buf, n, "file %d (%s, %s, %d save%s)", slot + 1,
                f.character == 0 ? "Chris" : f.character == 1 ? "Jill" : "?", t, f.saves, f.saves == 1 ? "" : "s");
  return buf;
}

bool has_ninth() {
  char p[MAX_PATH];
  std::snprintf(p, sizeof(p), "%ssavedat9.dat", g_folder);
  return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

// Does `a` hold a table that says what the files say?
bool table_matches(uintptr_t a, const File* files, bool ninth) {
  if (!mem::readable(a, sizeof(GameEntry) * kGameEntries)) return false;
  const auto* e = reinterpret_cast<const GameEntry*>(a);
  for (int i = 0; i < kGameEntries; ++i) {
    const bool used = i < kFiles ? files[i].used : ninth;
    if (e[i].has_data != (used ? 1u : 0u)) return false;
    if (i >= kFiles || !used) continue;
    const File& f = files[i];
    if (f.char_byte < 0) continue;  // a file too short to say: only its presence counts
    if (e[i].character != static_cast<uint32_t>(f.char_byte) || e[i].saves != static_cast<uint32_t>(f.saves) ||
        e[i].stage != static_cast<uint32_t>(f.stage) || e[i].room != static_cast<uint32_t>(f.room))
      return false;
  }
  return true;
}

// The one place in [lo, hi) that matches, or 0 (none, or more than one - a
// table of nothing but empty slots cannot be told from any other zeros).
uintptr_t find_table(uintptr_t lo, uintptr_t hi, const File* files) {
  const bool ninth = has_ninth();
  uintptr_t found = 0;
  for (uintptr_t a = (lo + 3) & ~uintptr_t(3); a + sizeof(GameEntry) * kGameEntries <= hi; a += 4) {
    if (!table_matches(a, files, ninth)) continue;
    if (found) return 0;
    found = a;
  }
  return found;
}

// The entry for `slot` after a change, now that the disk says `now`.
bool sync_entry(int slot, const File& now) {
  if (!g_table || !mem::readable(g_table, sizeof(GameEntry) * kGameEntries)) return false;
  auto* e = reinterpret_cast<GameEntry*>(g_table) + slot;
  if (now.used && now.char_byte >= 0) {
    e->character = static_cast<uint32_t>(now.char_byte);
    e->saves = static_cast<uint32_t>(now.saves);
    e->stage = static_cast<uint32_t>(now.stage);
    e->room = static_cast<uint32_t>(now.room);
  }
  e->has_data = now.used ? 1u : 0u;
  return true;
}

void push(const Request& r) {
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  if (g_req_n < 8) g_req[g_req_n++] = r;
  LeaveCriticalSection(&g_lock);
}

int drain(Request* out, int max) {
  if (!g_ready) return 0;
  EnterCriticalSection(&g_lock);
  const int n = g_req_n < max ? g_req_n : max;
  for (int i = 0; i < n; ++i) out[i] = g_req[i];
  g_req_n = 0;
  LeaveCriticalSection(&g_lock);
  return n;
}

void apply(const Request& r) {
  if (!g_showing) {
    note(true, "The Load Game list is not on screen any more - nothing was changed.");
    return;
  }
  if (!g_folder[0]) {
    note(true, "The save folder could not be found - nothing was changed.");
    return;
  }
  const bool copy = r.type == Request::kCopy;
  if (r.to < 0 || r.to >= kFiles || (copy && (r.from < 0 || r.from >= kFiles || r.from == r.to))) {
    note(true, "That is not a save file this game has - nothing was changed.");
    return;
  }
  std::vector<uint8_t> src, before;
  const File to_now = read_slot(r.to, &before);
  const File from_now = copy ? read_slot(r.from, &src) : File{};
  if (!same_save(to_now, r.to_seen) || (copy && !same_save(from_now, r.from_seen))) {
    note(true, "The save files changed since that was clicked - nothing was changed.");
    return;
  }
  // The list's table has to still say what the disk says, or it is not the one
  // this code found (or not the list's any more).
  File all[kFiles];
  for (int i = 0; i < kFiles; ++i) all[i] = read_slot(i);
  if (g_table && !table_matches(g_table, all, has_ninth())) {
    logf("save files: the Load Game list's table at %p no longer matches the files - dropped", reinterpret_cast<void*>(g_table));
    g_table = 0;
  }
  if (!copy && !g_table) {
    note(true, "The game's list could not be kept in step (see the log), and it would still offer a deleted file for "
               "loading - nothing was deleted.");
    return;
  }
  char a[96], b[96], to_path[MAX_PATH];
  path_of(r.to, to_path, sizeof(to_path));
  if (!copy) {
    if (!to_now.used) {
      note(true, "Save file %d is already empty.", r.to + 1);
      return;
    }
    if (!DeleteFileA(to_path)) {
      note(true, "Save file %d could not be deleted (error %lu) - nothing was changed.", r.to + 1, GetLastError());
      return;
    }
    logf("save files: deleted %s", describe(r.to, to_now, a, sizeof(a)));
    File gone;
    gone.known = true;
    sync_entry(r.to, gone);
    note(false, "Save file %d was deleted.", r.to + 1);
    return;
  }
  if (!from_now.used) {
    note(true, "Save file %d has no save to copy.", r.from + 1);
    return;
  }
  if (!write_atomic(to_path, src)) {
    note(true, "Save file %d could not be written (error %lu) - nothing was changed.", r.to + 1, GetLastError());
    return;
  }
  // Read it back: what the game will load is what is on disk.
  std::vector<uint8_t> back;
  char p[MAX_PATH];
  path_of(r.to, p, sizeof(p));
  if (!read_all(p, &back) || back != src) {
    const bool restored = to_now.used ? write_atomic(to_path, before) : DeleteFileA(to_path) != 0;
    note(true, "Save file %d did not read back as the copy - %s.", r.to + 1,
         restored ? "it was put back as it was" : "it could not be put back; check the SAVE folder");
    return;
  }
  logf("save files: copied %s over %s", describe(r.from, from_now, a, sizeof(a)), describe(r.to, to_now, b, sizeof(b)));
  const bool synced = sync_entry(r.to, from_now);
  if (synced) note(false, "Save file %d was copied to save file %d.", r.from + 1, r.to + 1);
  else note(false, "Save file %d was copied to save file %d - the game's list shows it the next time LOAD GAME opens.",
            r.from + 1, r.to + 1);
}

}  // namespace

void init() {
  if (g_ready) return;
  InitializeCriticalSection(&g_lock);
  g_ready = true;
}

View view() {
  View v;
  if (!g_ready) return v;
  EnterCriticalSection(&g_lock);
  v = g_view;
  std::snprintf(v.note, sizeof(v.note), "%s", g_note);
  v.note_error = g_note_error;
  LeaveCriticalSection(&g_lock);
  return v;
}

bool showing() { return g_showing; }

void request_delete(int slot, const File& seen) { push({Request::kDelete, -1, slot, File{}, seen}); }
void request_copy(int from, int to, const File& from_seen, const File& to_seen) {
  push({Request::kCopy, from, to, from_seen, to_seen});
}

Stamp stamp() {
  Stamp st;
  if (!g_folder[0]) resolve_folder();
  if (!g_folder[0]) return st;
  st.ok = true;
  for (int i = 0; i < kFiles; ++i) {
    char p[MAX_PATH];
    path_of(i, p, sizeof(p));
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    st.exists[i] = GetFileAttributesExA(p, GetFileExInfoStandard, &fa) != 0;
    if (st.exists[i]) st.write[i] = fa.ftLastWriteTime;
  }
  return st;
}

int written_since(const Stamp& before) {
  const Stamp now = stamp();
  if (!before.ok || !now.ok) return -1;
  int found = -1;
  for (int i = 0; i < kFiles; ++i) {
    if (!now.exists[i]) continue;
    if (before.exists[i] && CompareFileTime(&before.write[i], &now.write[i]) == 0) continue;
    if (found >= 0) return -1;  // more than one: not a save this code can vouch for
    found = i;
  }
  return found;
}

bool lower_save_count(int slot, int expect) {
  if (slot < 0 || slot >= kFiles || expect <= 0) return false;
  char p[MAX_PATH];
  path_of(slot, p, sizeof(p));
  std::vector<uint8_t> b;
  if (!read_all(p, &b) || b.size() < 0x229 || b[0x228] != expect) return false;
  b[0x228] = static_cast<uint8_t>(expect - 1);
  return write_atomic(p, b);
}

void tick(bool list_up, uintptr_t search_lo, uintptr_t search_hi) {
  if (!g_ready) return;
  if (list_up && !g_showing) {
    resolve_folder();
    g_last_read = 0;
    g_last_search = 0;
    g_table = 0;
    logf("save files: the Load Game list is up - the saves are in %s", g_folder[0] ? g_folder : "(unknown)");
  } else if (!list_up && g_showing) {
    g_table = 0;
    logf("save files: the Load Game list is gone");
  }
  g_showing = list_up;
  g_search_lo = search_lo;
  g_search_hi = search_hi;
  // The list's table: looked for (a few times a second) until it is found.
  if (g_showing && !g_table && g_search_lo && g_search_hi > g_search_lo && g_folder[0]) {
    const DWORD t = GetTickCount();
    if (!g_last_search || t - g_last_search > 250) {
      g_last_search = t;
      File all[kFiles];
      for (int i = 0; i < kFiles; ++i) all[i] = read_slot(i);
      g_table = find_table(g_search_lo, g_search_hi, all);
      if (g_table) logf("save files: the Load Game list's table of the files is at %p", reinterpret_cast<void*>(g_table));
    }
  }

  Request reqs[8];
  const int n = drain(reqs, 8);
  for (int i = 0; i < n; ++i) apply(reqs[i]);

  View v;
  v.up = g_showing;
  v.ready = g_folder[0] != '\0';
  v.list_table = g_table != 0;
  std::snprintf(v.folder, sizeof(v.folder), "%s", g_folder);
  if (g_showing && v.ready) {
    // The disk is read twice a second while the menu is up, and right after a
    // change - not every frame.
    const DWORD now = GetTickCount();
    if (n || !g_last_read || now - g_last_read > 500) {
      g_last_read = now;
      for (int i = 0; i < kFiles; ++i) g_files[i] = read_slot(i);
    }
    v.files = kFiles;
    for (int i = 0; i < kFiles; ++i) v.file[i] = g_files[i];
  }
  EnterCriticalSection(&g_lock);
  g_view = v;
  LeaveCriticalSection(&g_lock);
}

}  // namespace re1cc::savefiles
