#include "game.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "image.h"
#include "items.h"
#include "log.h"

namespace re1cc::game {
namespace {

// Our own patches of the game's import slots, so a dump shows what the game had.
struct IatNote {
  uintptr_t slot;
  uintptr_t original;
};
IatNote g_iat[16];
int g_iat_n = 0;

// --- export names, for the rebuilt import table --------------------------------
// The game's import slots hold bare addresses once Enigma has rebuilt them; this
// says which export of which loaded module one of them is.
struct ModuleExports {
  HMODULE mod = nullptr;
  char name[64] = {};
  std::vector<std::pair<uint32_t, const char*>> by_rva;
};
ModuleExports g_exports[64];
int g_nexports = 0;

const ModuleExports* exports_of(HMODULE m) {
  for (int i = 0; i < g_nexports; ++i)
    if (g_exports[i].mod == m) return &g_exports[i];
  if (g_nexports >= 64) return nullptr;
  ModuleExports& e = g_exports[g_nexports++];
  e.mod = m;
  char path[MAX_PATH] = "?";
  GetModuleFileNameA(m, path, MAX_PATH);
  const char* leaf = std::strrchr(path, '\\');
  std::snprintf(e.name, sizeof(e.name), "%s", leaf ? leaf + 1 : path);
  const auto base = reinterpret_cast<uintptr_t>(m);
  if (IMAGE_NT_HEADERS* nt = mem::nt_headers(m)) {
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress && dir.Size) {
      const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
      const auto* funcs = reinterpret_cast<const DWORD*>(base + ed->AddressOfFunctions);
      const auto* names = reinterpret_cast<const DWORD*>(base + ed->AddressOfNames);
      const auto* ords = reinterpret_cast<const WORD*>(base + ed->AddressOfNameOrdinals);
      for (DWORD i = 0; i < ed->NumberOfNames; ++i)
        if (ords[i] < ed->NumberOfFunctions)
          e.by_rva.emplace_back(funcs[ords[i]], reinterpret_cast<const char*>(base + names[i]));
    }
  }
  return &e;
}

// "USER32.dll!GetAsyncKeyState" (true), "module+0xRVA" or "?" (false).
bool export_name(uintptr_t v, char* out, size_t n) {
  std::snprintf(out, n, "?");
  HMODULE m = nullptr;
  if (!v || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(v), &m) ||
      !m)
    return false;
  const ModuleExports* e = exports_of(m);
  if (!e) return false;
  const auto rva = static_cast<uint32_t>(v - reinterpret_cast<uintptr_t>(m));
  for (const auto& [r, name] : e->by_rva)
    if (r == rva) {
      std::snprintf(out, n, "%s!%s", e->name, name);
      return true;
    }
  std::snprintf(out, n, "%s+0x%X", e->name, rva);
  return false;
}

// Every pointer in the rebuilt import table, resolved to a name where it can be:
// counted in the log, listed one per line in `map` when one is open.
void survey_imports(FILE* map) {
  const mem::Range& r = image::idata();
  int total = 0, named = 0;
  for (uintptr_t a = r.begin; a + 4 <= r.end; a += 4) {
    uint32_t v = 0;
    if (!mem::read_safe(a, &v) || !v || image::game_image().contains(v)) continue;  // RVAs of names, not pointers
    ++total;
    char name[160];
    if (export_name(v, name, sizeof(name))) ++named;
    if (map) std::fprintf(map, "%08X %08X %s\n", static_cast<unsigned>(a - image::base()), v, name);
  }
  logf("imports: .idata (0x%X bytes) holds %d pointer(s) outside the image: %d resolve to DLL exports, %d do not",
       static_cast<unsigned>(r.size()), total, named, total - named);
  // The three this mod cares about, by the address a direct resolution leaves.
  struct Want {
    const char* dll;
    const char* fn;
  } const wants[] = {{"user32.dll", "PeekMessageA"},
                     {"user32.dll", "GetAsyncKeyState"},
                     {"kernel32.dll", "SetUnhandledExceptionFilter"}};
  for (const Want& w : wants) {
    HMODULE m = GetModuleHandleA(w.dll);
    const auto real = m ? reinterpret_cast<uint32_t>(GetProcAddress(m, w.fn)) : 0u;
    const auto hits = real ? mem::find_dwords(r, real, 4) : std::vector<uintptr_t>{};
    if (hits.empty())
      logf("imports:   %s!%s (%p) is not in the table", w.dll, w.fn, reinterpret_cast<void*>(real));
    else
      logf("imports:   %s!%s (%p) is in the table at exe+0x%X%s", w.dll, w.fn, reinterpret_cast<void*>(real),
           static_cast<unsigned>(hits[0] - image::base()), hits.size() > 1 ? " (and more)" : "");
  }
}

// The game's save folder: it opens SAVE\savedat%d.dat relative to its working
// directory (the decomp's SaveLoadScreen.cpp).
void survey_saves() {
  char cwd[MAX_PATH] = "?";
  GetCurrentDirectoryA(MAX_PATH, cwd);
  char exe[MAX_PATH] = "?";
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  logf("recon: exe %s, working directory %s", exe, cwd);
  int found = 0;
  for (int i = 1; i <= 9; ++i) {
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\SAVE\\savedat%d.dat", cwd, i);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) continue;
    ++found;
    SYSTEMTIME st{};
    FileTimeToSystemTime(&fa.ftLastWriteTime, &st);
    logf("recon:   savedat%d.dat: %lu bytes, written %04u-%02u-%02u %02u:%02u:%02u UTC", i, fa.nFileSizeLow, st.wYear,
         st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  }
  logf("recon: %d save file(s) in %s\\SAVE", found, cwd);
}

// Watch: log the configured values whenever they change, at most ~60 lines a
// second so a counter left in the list cannot flood the log.
uint32_t g_watch_last[config::kMaxWatch];
bool g_watch_seen[config::kMaxWatch];
DWORD g_watch_second = 0;
int g_watch_lines = 0;

}  // namespace

bool unpacked(int* prologues, int* pads) {
  image::init();
  return mem::text_looks_decrypted(image::text(), prologues, pads);
}

void note_iat_slot(uintptr_t slot, uintptr_t original) {
  if (!slot || g_iat_n >= 16) return;
  g_iat[g_iat_n++] = IatNote{slot, original};
}

int hook_import(const char* what, void* real, void* hook) {
  image::init();
  if (!real || !hook || image::idata().empty()) return 0;
  const auto from = reinterpret_cast<uint32_t>(real), to = reinterpret_cast<uint32_t>(hook);
  uintptr_t first = 0;
  const int n = mem::replace_dwords(image::idata(), from, to, &first);
  if (n) {
    note_iat_slot(first, from);
    logf("imports: the game's %s slot (exe+0x%X) now points at our hook%s", what,
         static_cast<unsigned>(first - image::base()), n > 1 ? " (more than one slot)" : "");
  } else if (!mem::find_dwords(image::idata(), to, 1).empty()) {
    logf("imports: the game's %s slot already holds our hook (handed over through GetProcAddress)", what);
  } else {
    logf("imports: %s is not in the game's import table as %p - Enigma resolved it some other way", what, real);
  }
  return n;
}

void recon(const char* dir) {
  image::init();
  image::log_sections();
  survey_saves();
  FILE* map = nullptr;
  char path[MAX_PATH];
  if (config::get().dump_image) {
    std::snprintf(path, sizeof(path), "%sResidentEvil.imports.txt", dir);
    map = std::fopen(path, "w");
    if (map) std::fprintf(map, "# RVA of the slot, its value, the export it resolves to\n");
  }
  survey_imports(map);
  if (map) {
    std::fclose(map);
    logf("imports: map written to %s", path);
  }
  if (config::get().dump_image) dump_image(dir);
}

// The unpacked 1997 image - its six sections only, not Enigma's - written with
// raw offsets == RVAs, so the file maps 1:1 to the loaded image. The entry point
// and the data directories still describe Enigma's wrapper; tools/fix_dump.py
// points the entry at the game's own and clears the rest.
bool dump_image(const char* dir) {
  image::init();
  IMAGE_NT_HEADERS* nt = mem::nt_headers(image::module());
  if (!nt || !image::layout_ok()) {
    logf("ERROR: dump: the image layout is not the expected one - not dumping");
    return false;
  }
  const size_t size = image::game_image().size();
  std::vector<uint8_t> buf(size, 0);
  SIZE_T got = 0;
  ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(image::base()), buf.data(),
                    nt->OptionalHeader.SizeOfHeaders, &got);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < 6; ++i) {
    const uintptr_t va = image::base() + sec[i].VirtualAddress;
    const size_t n = sec[i].Misc.VirtualSize;
    for (size_t off = 0; off < n && sec[i].VirtualAddress + off < size; off += 0x1000) {
      size_t chunk = (n - off) < 0x1000 ? (n - off) : 0x1000;
      if (sec[i].VirtualAddress + off + chunk > size) chunk = size - (sec[i].VirtualAddress + off);
      ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(va + off),
                        buf.data() + sec[i].VirtualAddress + off, chunk, &got);
    }
  }
  // Put our own import-slot patches back so the dump is the game's image, not ours.
  for (int i = 0; i < g_iat_n; ++i) {
    const uintptr_t off = g_iat[i].slot - image::base();
    if (off + 4 <= size) std::memcpy(buf.data() + off, &g_iat[i].original, 4);
  }
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(buf.data());
  auto* nt2 = reinterpret_cast<IMAGE_NT_HEADERS*>(buf.data() + dos->e_lfanew);
  nt2->FileHeader.NumberOfSections = 6;
  nt2->OptionalHeader.SizeOfImage = static_cast<DWORD>(size);
  nt2->OptionalHeader.FileAlignment = 0x1000;
  auto* s2 = IMAGE_FIRST_SECTION(nt2);
  for (unsigned i = 0; i < 6; ++i) {
    s2[i].PointerToRawData = s2[i].VirtualAddress;
    s2[i].SizeOfRawData = (s2[i].Misc.VirtualSize + 0xFFF) & ~0xFFFu;
  }
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%sResidentEvil.dumped.exe", dir);
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    logf("ERROR: dump: cannot write %s", path);
    return false;
  }
  std::fwrite(buf.data(), 1, size, f);
  std::fclose(f);
  logf("dump: wrote %s (%u bytes; entry point and data directories are still Enigma's - run tools/fix_dump.py)", path,
       static_cast<unsigned>(size));
  return true;
}

namespace {

// --- what discovery found ------------------------------------------------------------
// Every one of these is read out of an instruction operand and cross-checked;
// CLAUDE.md lists the values build 21744136 gives.
struct Anchors {
  uintptr_t flags = 0;         // the menu flags word; the second one follows at +4
  uintptr_t load_save = 0;     // 1 while the option screen (or a save/load screen) runs
  uintptr_t options_menu = 0;  // the option screen's task entry
  uintptr_t main_menu = 0;     // the inventory's
  uintptr_t game_start = 0;    // task 0's entry while a game is on
  uintptr_t task_eip = 0;      // the tasks' entry table, 4 bytes each
  uintptr_t task_tcb = 0;      // the task blocks (+0: the 16-bit state)
  uintptr_t task_esp = 0;      // the suspended tasks' saved ESPs, 4 bytes each
  uintptr_t title_flag = 0;    // 1 while the title's menu runs
  uintptr_t hp = 0, max_hp = 0, status = 0, character = 0;
  uintptr_t enemies = 0;
  uint32_t enemy_stride = 0;
  int enemy_count = 0;
  uintptr_t timer = 0, countdown = 0, save_count = 0, typewriter = 0;
  // The inventory and the item box (see game.h).
  uintptr_t slots_ptr = 0;         // the slot pointer: the inventory in play
  uintptr_t equipped = 0;          // its equipped slot, 1-based
  uintptr_t held = 0;              // how many of its slots are in use
  uintptr_t rows = 0;              // the icon sheet row of each slot (8 bytes)
  uintptr_t rows_used = 0;         // a bit per row taken (a byte, as the game uses it)
  uintptr_t entity_id = 0;         // the player entity's character byte
  uintptr_t items = 0, rebecca = 0, box = 0;
  int box_size = 0;
  uintptr_t item_table = 0;        // 4-byte records by item id: +0 the combine's per-slot maximum, +1 the icon
  int stack_lo = 0, stack_hi = 0, stack_ribbon = 0, stack_cap = 0;
  uintptr_t icons = 0;             // LoadHeldItemsImages
  uintptr_t count_held = 0;        // CountHeldItems, which it calls first
  // options_menu's frame (see find_options_frame).
  uint32_t opt_handler[3] = {};
  bool opt_frame = false;
};
Anchors g;
volatile LONG g_ready = 0;
char g_status[160] = "waiting for the game to unpack";
mem::Patch g_sites[kSiteCount];
bool g_site_ok[kSiteCount] = {};
const char* const kSiteNames[kSiteCount] = {
    "ammo decrement (guns)", "ammo decrement (flamethrower)", "play time +1 (game)", "play time +1 (inventory)",
    "self-destruct counter +1", "save count +1", "typewriter ribbon check", "save's ribbon take"};

constexpr uint32_t kDeadBit = 0x01000000;       // the menu flags: the player is dead
constexpr uint32_t kCountdownBit = 0x08000000;  // the second flags word: the self-destruct runs
constexpr uint32_t kTaskStride = 0x7C;          // Task_execute: [id * 31 * 4 + blocks]
constexpr int kEntityHp = 0x88, kEntityMaxHp = 0x175, kEntityStatus = 0xDC, kEntityCharacter = 0x01;
constexpr int kEnemyId = 0x01, kEnemyState = 0x84, kEnemyHp = 0x88, kEnemyNpcFrom = 0x14;

// The signatures (tools/check_sigs.py runs the same ones against the dump).
const char* const kSigMenus =
    "C7 05 ?? ?? ?? ?? 01 00 00 00 83 C4 04 F6 05 ?? ?? ?? ?? 40 74 1B 81 25 ?? ?? ?? ?? FF FF BF FF "
    "C7 05 ?? ?? ?? ?? 01 00 00 00 68 ?? ?? ?? ?? EB 05 68 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 08 81 0D ?? ?? ?? ?? 00 80 00 00";
const char* const kSigTaskExecute = "8B 44 24 08 8B 4C 24 04 89 04 8D ?? ?? ?? ?? 8B C1 C1 E0 05 2B C1 66 C7 04 85 ?? ?? ?? ?? 02 00 C3";
const char* const kSigTaskChain = "8B 0D ?? ?? ?? ?? 8B 44 24 04 8B 15 ?? ?? ?? ?? 89 04 8D ?? ?? ?? ?? 66 C7 02 02 00";
const char* const kSigTaskSwitch = "A1 ?? ?? ?? ?? 89 25 ?? ?? ?? ?? 8B 24 85 ?? ?? ?? ?? 90 C3";
const char* const kSigHealthBar = "66 39 05 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 8A 0D ?? ?? ?? ?? 33 DB 0F BF 05 ?? ?? ?? ?? C0 E9 02";
const char* const kSigPoison = "F6 05 ?? ?? ?? ?? 62 74 3D A0 ?? ?? ?? ?? FE 0D ?? ?? ?? ?? 84 C0 75 2E C6 05 ?? ?? ?? ?? 78";
const char* const kSigEnemies = "B8 ?? ?? ?? ?? C6 00 00 05 ?? ?? ?? ?? 3D ?? ?? ?? ?? 72 F1";
const char* const kSigAmmoGuns = "33 C0 8B 0D ?? ?? ?? ?? A0 ?? ?? ?? ?? FE 4C 41 FF 66 83 FB 08";
const char* const kSigAmmoFlame = "A1 ?? ?? ?? ?? 8A 0D ?? ?? ?? ?? FE 4C 48 FF F6 05";
const char* const kSigTimerGame = "6A 01 E8 ?? ?? ?? ?? 83 C4 04 FF 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 C0 A0";
const char* const kSigTimerCave = "60 A0 ?? ?? ?? ?? B4 01 84 C0 74 06 FF 05 ?? ?? ?? ?? 30 E0 A2";
const char* const kSigCountdown =
    "66 81 3D ?? ?? ?? ?? FE 7F 89 1D ?? ?? ?? ?? 73 11 85 35 ?? ?? ?? ?? 75 10 66 FF 05 ?? ?? ?? ?? EB 07 "
    "66 89 1D ?? ?? ?? ?? F6 05 ?? ?? ?? ?? 08";
const char* const kSigSaveCount = "7C F2 FE 05 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 64 72 07 C6 05 ?? ?? ?? ?? 63";
const char* const kSigRibbonCheck =
    "6A 2F E8 ?? ?? ?? ?? 83 C4 04 8B F0 85 F6 7C 21 8B 44 24 08 A3 ?? ?? ?? ?? 66 89 70 02 33 C0 C6 05 ?? ?? ?? ?? 01";
const char* const kSigRibbonTake =
    "83 BC 24 ?? ?? 00 00 00 74 2A A0 ?? ?? ?? ?? 24 03 3C 01 75 13 6A 7B 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 85 C0 74 0C "
    "C6 05 ?? ?? ?? ?? 2F E8";
// The inventory and the item box (step 11). rearrange_item_slots packs the
// inventory in play: equipped byte +4, character byte +19, slot pointer
// +51/+74/+91, icon rows +105/+111/+139, count +129, row bits +149.
const char* const kSigRearrange =
    "83 EC 04 A0 ?? ?? ?? ?? FE C8 B1 04 88 44 24 02 53 56 A0 ?? ?? ?? ?? 24 03 3C 01 0F 95 C2 2A CA 33 D2 02 C9 "
    "88 54 24 0B 88 4C 24 08 88 4C 24 09 33 C9 A1 ?? ?? ?? ?? 8A CA 8A 04 48 84 C0 74 3F 38 54 24 0B 74 33 33 DB "
    "8B 35 ?? ?? ?? ?? 8A 5C 24 0B 38 54 24 0A 88 04 5E 8B 35 ?? ?? ?? ?? 8A 44 4E 01 88 44 5E 01 8A 89 ?? ?? ?? ?? "
    "88 8B ?? ?? ?? ?? 75 04 88 5C 24 0A FE 44 24 0B EB 1A 38 15 ?? ?? ?? ?? 76 12 B0 01 8A 89 ?? ?? ?? ?? D2 E0 "
    "F6 D0 20 05 ?? ?? ?? ??";
// The item box screen's swap of a box slot with an inventory slot: equipped
// byte +2/+17, the box's cursor +26, the box +33/+68 (and +1 at +47/+77), slot
// pointer +57/+89, count +99.
const char* const kSigBoxSwap =
    "8A 0D ?? ?? ?? ?? 8A D8 2B CB 83 F9 01 75 07 C6 05 ?? ?? ?? ?? 00 33 C9 8A 0D ?? ?? ?? ?? 8A 14 4D ?? ?? ?? ?? "
    "8D 34 4D 00 00 00 00 8A 0C 4D ?? ?? ?? ?? 88 4C 24 0F 8B 0D ?? ?? ?? ?? 8D 3C 59 8A 0F 88 8E ?? ?? ?? ?? 8A 4F 01 "
    "88 8E ?? ?? ?? ?? 8A 4C 24 0F 88 17 8B 35 ?? ?? ?? ?? 88 4C 5E 01 38 05 ?? ?? ?? ??";
// LoadHeldItemsImages: CountHeldItems (call at +1), count +8, row bits +23,
// icon source +36, slot pointer +42, icon rows +50, the item table's icon
// column +62, LoadItemImage (call at +68).
const char* const kSigIcons =
    "53 E8 ?? ?? ?? ?? 8A 1D ?? ?? ?? ?? B0 01 8A CB D2 E0 FE C8 84 DB A2 ?? ?? ?? ?? 74 33 FE CB 33 D2 8A D3 68 "
    "?? ?? ?? ?? 52 A1 ?? ?? ?? ?? 33 C9 88 9A ?? ?? ?? ?? 8A 0C 50 33 C0 8A 04 8D ?? ?? ?? ?? 48 50 E8 ?? ?? ?? ?? "
    "83 C4 0C 84 DB 75 CD 5B C3";
// The combine's reload helpers cap a gun at its item table entry (+17); there
// are two of them, and they must agree.
const char* const kSigItemMax = "66 83 E3 7F 66 03 D3 33 DB 8A 19 0F B7 FA 8A 0C 9D ?? ?? ?? ?? 33 DB 8A D9 3B DF 73 12";
// The pickup's stacking: the item picked up (+2/+11/+20) between +6 and +15, or
// +24; the character byte +34, the slot pointer +70, the cap +95 (a word).
const char* const kSigStackRule =
    "80 3D ?? ?? ?? ?? ?? 72 09 80 3D ?? ?? ?? ?? ?? 72 0D 80 3D ?? ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? B2 04 A0 ?? ?? ?? ?? "
    "24 03 C6 44 24 0A 00 3C 01 0F 95 C3 2A D3 02 D2 8A DA 88 54 24 09 33 C0 8A 44 24 0A 03 C0 03 05 ?? ?? ?? ?? 38 08 "
    "75 21 66 0F B6 50 01 66 0F B6 44 24 0B 66 03 D0 66 81 FA ?? ?? 76 18";
// options_menu (step 12): its prologue, at the address check_menus_state gives;
// the copy of the player entity and of the equipped byte it makes as it opens
// (entity copy [esp+disp] at +3 with one argument still pushed, entity +13,
// equipped byte +28/+34, its copy [esp+disp] at +42); and the copy back as it
// closes (entity copy +3, entity +11, the byte [esp+disp] at +23 with three
// arguments pushed, equipped byte +28).
const char* const kSigOptionsPrologue =
    "81 EC ?? ?? 00 00 53 56 57 33 DB 89 5C 24 1C 55 89 5C 24 24 89 5C 24 2C 89 5C 24 30 89 5C 24 34 89 5C 24 38 "
    "89 5C 24 3C C7 44 24 28 88 13 00 00 53 88 1D ?? ?? ?? ?? C7 44 24 18 ?? ?? ?? ?? C7 44 24 1C ?? ?? ?? ?? "
    "C7 44 24 20 ?? ?? ?? ?? E8";
const char* const kSigOptionsSave =
    "8D 4C 24 ?? 83 C4 04 68 80 01 00 00 68 ?? ?? ?? ?? 51 E8 ?? ?? ?? ?? 83 C4 0C 8A 0D ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 "
    "88 4C 24 ??";
const char* const kSigOptionsRestore =
    "8D 44 24 ?? 68 80 01 00 00 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8A 44 24 ?? 83 C4 0C A2 ?? ?? ?? ?? E8";

// options_menu's frame, measured from ESP once its four registers are pushed,
// as its prologue lays it out: the three sub-menu handlers are stored with one
// argument pushed ([esp+0x18] -> +0x14), and the camera matrix is cleared from
// [esp+0x1C] with three registers pushed (+0x20, eight dwords) with m[1][1] =
// 5000 at +0x28. The entity copy and the equipped byte are read out of the save.
constexpr int kOptHandlers = 0x14, kOptMatrix = 0x20, kOptMatrixSize = 0x20, kOptMatrixM11 = 0x28;
constexpr uint32_t kOptMatrixM11Value = 0x1388;
constexpr int kEntitySize = 0x180;
int g_opt_entity = 0, g_opt_equipped = 0;
// The item table has a record for every id up to the radio; past that it is
// other data.
constexpr int kItemTableLast = 0x4D;

uint32_t u32(uintptr_t a) { return mem::read<uint32_t>(a); }
int32_t i32(uintptr_t a) { return mem::read<int32_t>(a); }
template <typename T>
T peek(uintptr_t a) {
  return *reinterpret_cast<volatile T*>(a);
}
// Game data is plain writable memory: no VirtualProtect for it (that is for code).
template <typename T>
void poke(uintptr_t a, T v) {
  *reinterpret_cast<volatile T*>(a) = v;
}

// "XX XX XX XX": a global already found, spelled into a signature.
std::string hex4(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%02X %02X %02X %02X", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
  return b;
}

// The one match of `sig` in [lo, hi) (default: the code), or 0.
uintptr_t find_one(const char* name, const std::string& sig, uintptr_t lo = 0, uintptr_t hi = 0) {
  const mem::Range r = lo ? mem::Range{lo, hi} : image::text();
  const auto hits = mem::find_all(r, sig.c_str(), 4);
  if (hits.size() == 1) return hits[0];
  logf("game: %s: %s", name, hits.empty() ? "not found" : "more than one match - not used");
  return 0;
}

void prepare(Site s, uintptr_t at, const char* expect, const std::vector<uint8_t>& bytes) {
  if (!at) return;
  if (!g_sites[s].prepare(at, expect, bytes, kSiteNames[s])) {
    logf("game: site %s at exe+0x%X does not hold the expected bytes - not used", kSiteNames[s], image::rva(at));
    return;
  }
  g_site_ok[s] = true;
  logf("game: site %s at exe+0x%X", kSiteNames[s], image::rva(at));
}

}  // namespace

bool discover() {
  if (g_ready) return true;
  image::init();
  int found = 0, wanted = 0;
  auto count = [&](bool ok) {
    ++wanted;
    found += ok ? 1 : 0;
  };
  // 1. check_menus_state: the menu flags, the load/save flag, both menu tasks, Task_execute's tables.
  if (const uintptr_t a = find_one("check_menus_state", kSigMenus)) {
    const uint32_t flags = u32(a + 24);
    if (u32(a + 15) == flags + 2 && u32(a + 66) == flags) {
      g.flags = flags;
      g.load_save = u32(a + 34);
      g.options_menu = u32(a + 43);
      g.main_menu = u32(a + 50);
      const uintptr_t te = a + 61 + i32(a + 57);
      if (find_one("Task_execute", kSigTaskExecute, te, te + 40) == te) {
        g.task_eip = u32(te + 11);
        g.task_tcb = u32(te + 26);
      }
    } else {
      logf("game: check_menus_state: its flag operands disagree - not used");
    }
  }
  count(g.task_eip != 0);
  // 2. Task_chain, and title_state's chain to game_start (it restores the play time first).
  uint32_t timer_hint = 0;
  if (g.task_eip) {
    const uintptr_t c = find_one("Task_chain", kSigTaskChain);
    // The task switch loads a task's saved ESP by the same current-task id Task_chain reads.
    if (c)
      if (const uintptr_t sw = find_one("task switch", kSigTaskSwitch))
        if (u32(sw + 1) == u32(c + 2)) g.task_esp = u32(sw + 14);
    if (c && u32(c + 19) == g.task_eip) {
      const std::string sig = "A1 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 00 00 00 00 A3 ?? ?? ?? ?? A1 " + hex4(g.flags) +
                              " 25 FF FF FF 3F 0D 00 00 00 40 A3 " + hex4(g.flags) + " E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8";
      const uintptr_t q = find_one("title_state -> game_start", sig);
      if (q && u32(q + 7) == g.load_save && q + 55 + i32(q + 51) == c) {
        g.game_start = u32(q + 46);
        timer_hint = u32(q + 16);
      }
    }
  }
  count(g.game_start != 0);
  // 3. The title menu's loop flag: title_exit_loop clears it and reads the menu flags; one site sets it.
  if (g.flags) {
    if (const uintptr_t t = find_one("title_exit_loop", "C6 05 ?? ?? ?? ?? 00 A1 " + hex4(g.flags) + " 25")) {
      const uint32_t f = u32(t + 2);
      if (mem::find_all(image::text(), ("C6 05 " + hex4(f) + " 01").c_str(), 4).size() == 1) g.title_flag = f;
      else logf("game: title flag %#x is not set to 1 at exactly one site - not used", f);
    }
  }
  count(g.title_flag != 0);
  // 4. The player: HP and max HP from the health bar, the status byte from the poison tick.
  if (const uintptr_t h = find_one("health bar", kSigHealthBar)) {
    const uint32_t hp = u32(h + 3), mx = u32(h + 15);
    if (u32(h + 24) == hp && mx - hp == static_cast<uint32_t>(kEntityMaxHp - kEntityHp)) {
      g.hp = hp;
      g.max_hp = mx;
    }
  }
  if (g.hp)
    if (const uintptr_t p = find_one("poison tick", kSigPoison))
      if (u32(p + 2) == g.hp - kEntityHp + kEntityStatus) g.status = u32(p + 2);
  count(g.hp && g.status);
  // 5. The enemy slots: the loop that clears each one's status byte gives base, stride and end.
  if (const uintptr_t e = find_one("enemy slot clear loop", kSigEnemies)) {
    const uint32_t base = u32(e + 1), stride = u32(e + 9), end = u32(e + 14);
    if (stride > kEnemyHp + 1 && end > base && (end - base) % stride == 0 && (end - base) / stride <= 64) {
      g.enemies = base;
      g.enemy_stride = stride;
      g.enemy_count = static_cast<int>((end - base) / stride);
    }
  }
  count(g.enemies != 0);
  // 6. Ammo: both firing decrements read the same slot pointer and equipped slot.
  const uintptr_t x = find_one("ammo decrement (guns)", kSigAmmoGuns);
  const uintptr_t y = find_one("ammo decrement (flamethrower)", kSigAmmoFlame);
  if (x && y && u32(x + 4) == u32(y + 1) && u32(x + 9) == u32(y + 7)) {
    prepare(kSiteAmmoGuns, x + 13, "FE 4C 41 FF", mem::nops(4));
    prepare(kSiteAmmoFlame, y + 11, "FE 4C 48 FF", mem::nops(4));
  }
  count(g_site_ok[kSiteAmmoGuns] && g_site_ok[kSiteAmmoFlame]);
  // 7. Play time: game_loop's +1 and GOG's inventory code cave, one counter.
  if (const uintptr_t gl = find_one("play time +1 (game)", kSigTimerGame)) {
    g.timer = u32(gl + 12);
    if (timer_hint && timer_hint != g.timer) {
      logf("game: play time %#x is not the one title_state restores (%#x) - not used", g.timer, timer_hint);
      g.timer = 0;
    } else {
      prepare(kSiteTimerGame, gl + 10, "FF 05 ?? ?? ?? ??", mem::nops(6));
      if (const uintptr_t cave = find_one("play time +1 (inventory)", kSigTimerCave))
        if (u32(cave + 14) == g.timer) prepare(kSiteTimerInventory, cave + 12, "FF 05 ?? ?? ?? ??", mem::nops(6));
    }
  }
  count(g.timer != 0);
  // 8. The self-destruct counter: its +1 and the bit that says the countdown runs.
  if (g.flags)
    if (const uintptr_t k = find_one("self-destruct counter", kSigCountdown)) {
      const uint32_t cd = u32(k + 3);
      if (u32(k + 28) == cd && u32(k + 37) == cd && u32(k + 43) == g.flags + 7) {
        g.countdown = cd;
        prepare(kSiteCountdown, k + 25, "66 FF 05 ?? ?? ?? ??", mem::nops(7));
      }
    }
  count(g.countdown != 0);
  // 9. The save count: its +1 after the file is written (capped at 99 right after).
  if (const uintptr_t s = find_one("save count +1", kSigSaveCount)) {
    const uint32_t sc = u32(s + 4);
    if (u32(s + 10) == sc && u32(s + 19) == sc) {
      g.save_count = sc;
      prepare(kSiteSaveCount, s + 2, "FE 05 ?? ?? ?? ??", mem::nops(6));
    }
  }
  count(g.save_count != 0);
  // 10. Ink ribbons: the typewriter's "no ribbon" branch goes to the game's own
  //     no-ribbon save (the one Jill's first playthrough uses), and the save's
  //     take of one ribbon is jumped over. use_room_action_item itself is shared
  //     with keys and never touched.
  if (const uintptr_t w = find_one("typewriter ribbon check", kSigRibbonCheck)) {
    const uintptr_t branch = w + 16 + 0x21;
    const bool shape = mem::matches(branch + 0x25, mem::parse_pattern("8B 44 24 08 A3"));
    const bool same_player = !g.hp || u32(branch + 2) == g.hp - kEntityHp + kEntityCharacter;
    if (shape && same_player) {
      g.typewriter = u32(w + 33);
      g.character = u32(branch + 2);
      prepare(kSiteRibbonCheck, branch, "80 3D", {0xEB, 0x23});
    } else {
      logf("game: the typewriter's no-ribbon branch is not the expected one - not used");
    }
  }
  if (const uintptr_t r = find_one("save's ribbon take", kSigRibbonTake)) prepare(kSiteRibbonTake, r + 8, "74 2A", {0xEB, 0x2A});
  count(g_site_ok[kSiteRibbonCheck] && g_site_ok[kSiteRibbonTake]);
  // 11. The inventory and the item box. The slot pointer and the equipped byte
  //     are the ammo decrements'; rearrange_item_slots, which packs the
  //     inventory, adds its count, the icon rows and the character byte it
  //     sizes the inventory by. The slot pointer is only ever set to the two
  //     inventories, and the item box screen's swap gives the box, which sits
  //     right in front of them - the game itself reaches the inventory as the
  //     box's slot 48 on (SetupCharacterData, 0x494FC0).
  if (x && y && u32(x + 4) == u32(y + 1) && u32(x + 9) == u32(y + 7) && g.hp) {
    const uint32_t ptr = u32(x + 4), eq = u32(x + 9);
    if (const uintptr_t r = find_one("rearrange_item_slots", kSigRearrange)) {
      const uint32_t rows = u32(r + 105);
      if (u32(r + 4) == eq && u32(r + 19) == g.hp - kEntityHp + kEntityCharacter && u32(r + 51) == ptr &&
          u32(r + 74) == ptr && u32(r + 91) == ptr && u32(r + 111) == rows && u32(r + 139) == rows) {
        g.slots_ptr = ptr;
        g.equipped = eq;
        g.held = u32(r + 129);
        g.rows = rows;
        g.rows_used = u32(r + 149);
        g.entity_id = u32(r + 19);
      } else {
        logf("game: rearrange_item_slots: its operands disagree with the ammo decrement's and the player's - not used");
      }
    }
  }
  if (g.held) {
    uint32_t arrays[2] = {};
    int n = 0;
    bool extra = false;
    for (uintptr_t hit : mem::find_all(image::text(), ("C7 05 " + hex4(g.slots_ptr) + " ?? ?? ?? ??").c_str(), 16)) {
      const uint32_t v = u32(hit + 6);
      if ((n > 0 && arrays[0] == v) || (n > 1 && arrays[1] == v)) continue;
      if (n < 2) arrays[n++] = v;
      else extra = true;
    }
    const uint32_t lo = arrays[0] < arrays[1] ? arrays[0] : arrays[1];
    const uint32_t hi = arrays[0] < arrays[1] ? arrays[1] : arrays[0];
    if (n == 2 && !extra && hi - lo == 6 * 2) {
      g.items = lo;
      g.rebecca = hi;
    } else {
      logf("game: the slot pointer is not set to exactly two inventories six slots apart - not used");
    }
  }
  if (g.items) {
    if (const uintptr_t b = find_one("item box swap", kSigBoxSwap)) {
      const uint32_t box = u32(b + 33), pos = u32(b + 26);
      int top = -1;
      for (uintptr_t hit : mem::find_all(image::text(), ("C6 05 " + hex4(pos) + " ??").c_str(), 16))
        if (mem::read<uint8_t>(hit + 6) > top) top = mem::read<uint8_t>(hit + 6);
      const bool same = u32(b + 2) == g.equipped && u32(b + 17) == g.equipped && u32(b + 68) == box &&
                        u32(b + 47) == box + 1 && u32(b + 77) == box + 1 && u32(b + 57) == g.slots_ptr &&
                        u32(b + 89) == g.slots_ptr && u32(b + 99) == g.held;
      // Its size is the slot the box screen's cursor wraps back to, + 1 - and
      // that has to be exactly the room between the box and the inventory.
      if (same && top > 0 && g.items - box == static_cast<uint32_t>(top + 1) * 2) {
        g.box = box;
        g.box_size = top + 1;
      } else {
        logf("game: item box swap: the box at %#x (%d slots) does not line up with the inventory at %#x - not used", box,
             top + 1, g.items);
      }
    }
  }
  // The game's item rules and the icon rebuild. The combine's two reload
  // helpers cap a gun at its item table record and must agree on the table;
  // LoadHeldItemsImages reads the same records one byte on (the icon) and
  // calls CountHeldItems, which is checked against the globals found above: it
  // is the one game function the mod calls, so all of it is verified.
  if (g.held) {
    uint32_t table = 0;
    bool agree = true;
    for (uintptr_t m : mem::find_all(image::text(), kSigItemMax, 8)) {
      if (!table) table = u32(m + 17);
      else if (u32(m + 17) != table) agree = false;
    }
    if (const uintptr_t l = find_one("LoadHeldItemsImages", kSigIcons)) {
      const uintptr_t count_held = l + 6 + i32(l + 2);
      const std::string cs = "53 B1 04 A0 " + hex4(g.entity_id) + " 24 03 3C 01 A1 " + hex4(g.slots_ptr) +
                             " 0F 95 C2 2A CA 8D 14 4D 00 00 00 00 32 C9 38 08 74 15 3A CA 73 11 FE C1 33 DB 8A D9 A1 " +
                             hex4(g.slots_ptr) + " 80 3C 58 00 75 EB 5B 88 0D " + hex4(g.held) + " C3";
      if (u32(l + 8) == g.held && u32(l + 23) == g.rows_used && u32(l + 42) == g.slots_ptr && u32(l + 50) == g.rows &&
          mem::matches(count_held, mem::parse_pattern(cs.c_str())) && (!table || u32(l + 62) == table + 1)) {
        g.icons = l;
        g.count_held = count_held;
      } else {
        logf("game: LoadHeldItemsImages: it does not read the inventory found above - the icon rebuild is not used");
      }
    }
    if (table && agree) g.item_table = table;
    else logf("game: the combine's item table was %s - the mod's own magazine sizes stand in",
              table ? "read differently by its two helpers" : "not found");
    if (const uintptr_t p = find_one("pickup stacking", kSigStackRule)) {
      const uint32_t sel = u32(p + 2);
      const int lo = mem::read<uint8_t>(p + 6), hi = mem::read<uint8_t>(p + 15), ribbon = mem::read<uint8_t>(p + 24);
      const int cap = mem::read<uint16_t>(p + 95);
      if (u32(p + 11) == sel && u32(p + 20) == sel && u32(p + 34) == g.entity_id && u32(p + 70) == g.slots_ptr &&
          lo < hi && hi <= ribbon && cap > 0 && cap < 256) {
        g.stack_lo = lo;
        g.stack_hi = hi;
        g.stack_ribbon = ribbon;
        g.stack_cap = cap;
      } else {
        logf("game: pickup stacking: its operands do not line up - the mod's own list of stacking items stands in");
      }
    }
  }
  count(items_known());
  // 12. The option screen's frame, which holds the equipped byte it puts back
  //     as it closes (see find_options_frame). Its save and its restore must
  //     name the same two slots of the frame.
  if (g.options_menu && g.equipped && g.hp) {
    const uintptr_t o = g.options_menu;
    const uint32_t entity = g.hp - kEntityHp;
    if (!mem::matches(o, mem::parse_pattern(kSigOptionsPrologue))) {
      logf("game: options_menu does not open the way this build's does - its saved equipped slot is not used");
    } else {
      const uintptr_t s = find_one("options_menu's save", kSigOptionsSave, o, o + 0x400);
      const uintptr_t t = find_one("options_menu's restore", kSigOptionsRestore, o, o + 0x1000);
      if (s && t) {
        const int ent = mem::read<uint8_t>(s + 3) - 4, eq = mem::read<uint8_t>(s + 42);
        bool ok = u32(s + 13) == entity && u32(s + 28) == g.equipped && u32(s + 34) == g.equipped &&
                  u32(t + 11) == entity && u32(t + 28) == g.equipped && mem::read<uint8_t>(t + 3) == ent &&
                  mem::read<uint8_t>(t + 23) - 12 == eq && ent >= kOptMatrix + kOptMatrixSize && eq < kOptHandlers;
        for (int i = 0; i < 3; ++i) {
          g.opt_handler[i] = u32(o + 59 + 8 * i);
          ok = ok && image::text().contains(g.opt_handler[i]);
        }
        if (ok) {
          g_opt_entity = ent;
          g_opt_equipped = eq;
          g.opt_frame = true;
        } else {
          logf("game: options_menu's copies of the entity and the equipped byte do not line up - not used");
        }
      }
    }
  }
  count(g.opt_frame);
  logf("game: inventory: slot pointer %#x (main %#x, Rebecca %#x), equipped %#x, count %#x, icon rows %#x/%#x, "
       "item box %#x (%d slots), item table %#x, stacking %#x..%#x and %#x up to %d, icon rebuild %#x, option screen "
       "frame %s",
       g.slots_ptr, g.items, g.rebecca, g.equipped, g.held, g.rows, g.rows_used, g.box, g.box_size, g.item_table,
       g.stack_lo, g.stack_hi - 1, g.stack_ribbon, g.stack_cap, g.icons,
       g.opt_frame ? "derived" : "not derived");

  std::snprintf(g_status, sizeof(g_status), "%d of %d anchor groups found", found, wanted);
  logf("game: discovery: %s - flags %#x, task tables %#x/%#x/%#x, options_menu %#x, main_menu %#x, game_start %#x, "
       "title flag %#x, HP %#x, enemies %#x (%d x %#x), play time %#x, countdown %#x, save count %#x, typewriter %#x",
       g_status, g.flags, g.task_eip, g.task_tcb, g.task_esp, g.options_menu, g.main_menu, g.game_start, g.title_flag, g.hp,
       g.enemies, g.enemy_count, g.enemy_stride, g.timer, g.countdown, g.save_count, g.typewriter);
  // The screens are what the panel needs to come up at all; each cheat checks its own anchors.
  if (!(g.task_eip && g.task_tcb && g.options_menu && g.title_flag)) return false;
  InterlockedExchange(&g_ready, 1);
  return true;
}

bool ready() { return g_ready != 0; }
const char* status_text() { return g_status; }

bool in_game() { return g_ready && g.game_start && u32(g.task_eip) == g.game_start; }
bool options_open() {
  return g_ready && u32(g.task_eip + 4) == g.options_menu && peek<uint16_t>(g.task_tcb + kTaskStride) != 0;
}
bool title_menu() { return g_ready && peek<uint8_t>(g.title_flag) == 1; }
bool player_dead() { return g.flags && (u32(g.flags) & kDeadBit) != 0; }
bool load_list() {
  return g_ready && g.load_save && !in_game() && peek<uint32_t>(g.load_save) == 1 && peek<uint8_t>(g.title_flag) == 0;
}
uintptr_t task_saved_esp(int id) { return g.task_esp && id >= 0 && id < 3 ? u32(g.task_esp + 4u * id) : 0; }

bool player(Player* out) {
  if (!g.hp || !g.status) return false;
  out->hp = peek<int16_t>(g.hp);
  out->max_hp = peek<uint8_t>(g.max_hp);
  out->status = peek<uint8_t>(g.status);
  out->character = g.character ? peek<uint8_t>(g.character) : -1;
  return true;
}
bool set_player_hp(int hp) {
  if (!g.hp) return false;
  poke<int16_t>(g.hp, static_cast<int16_t>(hp));
  return true;
}
bool set_player_status(int status) {
  if (!g.status) return false;
  poke<uint8_t>(g.status, static_cast<uint8_t>(status));
  return true;
}

bool enemies_known() { return g.enemies != 0; }

int enemies(Enemy* out, int max) {
  if (!g.enemies) return 0;
  int n = 0;
  for (int i = 0; i < g.enemy_count && n < max; ++i) {
    const uintptr_t e = g.enemies + static_cast<uintptr_t>(i) * g.enemy_stride;
    if (!(peek<uint8_t>(e) & 1)) continue;
    const int id = peek<uint8_t>(e + kEnemyId);
    if (id >= kEnemyNpcFrom) continue;
    out[n].index = i;
    out[n].id = id;
    out[n].state = peek<uint8_t>(e + kEnemyState);
    out[n].hp = peek<int16_t>(e + kEnemyHp);
    ++n;
  }
  return n;
}
bool set_enemy_hp(int index, int hp) {
  if (!g.enemies || index < 0 || index >= g.enemy_count) return false;
  poke<int16_t>(g.enemies + static_cast<uintptr_t>(index) * g.enemy_stride + kEnemyHp, static_cast<int16_t>(hp));
  return true;
}

bool counters_ready() { return g.timer && g.save_count; }
int save_count() { return g.save_count ? peek<uint8_t>(g.save_count) : -1; }
bool set_save_count(int raw) {
  if (!g.save_count) return false;
  poke<uint8_t>(g.save_count, static_cast<uint8_t>(raw < 0 ? 0 : raw > 99 ? 99 : raw));
  return true;
}
int typewriter_state() { return g.typewriter ? peek<uint8_t>(g.typewriter) : -1; }
uint32_t game_timer() { return g.timer ? peek<uint32_t>(g.timer) : 0; }
bool set_game_timer(uint32_t v) {
  if (!g.timer) return false;
  poke<uint32_t>(g.timer, v);
  return true;
}
bool countdown_known() { return g.countdown && g.flags; }
bool countdown_active() { return countdown_known() && (u32(g.flags + 4) & kCountdownBit) != 0; }
int countdown_elapsed() { return g.countdown ? peek<int16_t>(g.countdown) : -1; }
bool set_countdown_elapsed(int s) {
  if (!g.countdown) return false;
  poke<int16_t>(g.countdown, static_cast<int16_t>(s < 0 ? 0 : s >= kCountdownLimit ? kCountdownLimit - 1 : s));
  return true;
}

mem::Patch* site(Site s) { return s >= 0 && s < kSiteCount && g_site_ok[s] ? &g_sites[s] : nullptr; }
const char* site_name(Site s) { return s >= 0 && s < kSiteCount ? kSiteNames[s] : "?"; }

// --- the inventory and the item box -----------------------------------------------------
bool items_known() {
  return g.slots_ptr && g.equipped && g.held && g.rows && g.rows_used && g.entity_id && g.items && g.rebecca && g.box &&
         g.box_size > 0;
}
int box_size() { return items_known() ? g.box_size : 0; }

bool read_box(ItemSlot* out, int n) {
  if (!items_known() || n < 0) return false;
  for (int i = 0; i < n && i < g.box_size; ++i) {
    out[i].id = peek<uint8_t>(g.box + 2u * i);
    out[i].count = peek<uint8_t>(g.box + 2u * i + 1);
  }
  return true;
}

bool write_box_slot(int i, int id, int count) {
  if (!items_known() || i < 0 || i >= g.box_size || id < 0 || id > 0xFF || count < 0 || count > 0xFF) return false;
  poke<uint8_t>(g.box + 2u * i, static_cast<uint8_t>(id));
  poke<uint8_t>(g.box + 2u * i + 1, static_cast<uint8_t>(count));
  return true;
}

namespace {
// The array the slot pointer names, or 0 for anything but the two inventories.
uintptr_t bag_array() {
  const uint32_t p = u32(g.slots_ptr);
  return p == g.items || p == g.rebecca ? p : 0;
}
}  // namespace

bool read_bag(Bag* out) {
  if (!items_known()) return false;
  const uintptr_t p = bag_array();
  if (!p) return false;
  out->rebecca = p == g.rebecca;
  out->held = peek<uint8_t>(g.held);
  out->equipped = peek<uint8_t>(g.equipped);
  for (int i = 0; i < kMaxBagSlots; ++i) {
    out->slot[i].id = peek<uint8_t>(p + 2u * i);
    out->slot[i].count = peek<uint8_t>(p + 2u * i + 1);
    out->row[i] = peek<uint8_t>(g.rows + i);
  }
  out->rows_used = peek<uint8_t>(g.rows_used);
  return true;
}

bool write_bag(const Bag& b, int size) {
  if (!items_known() || size < 0 || size > kMaxBagSlots || b.held < 0 || b.held > size) return false;
  const uintptr_t p = bag_array();
  if (!p || (p == g.rebecca) != b.rebecca) return false;
  for (int i = 0; i < size; ++i)
    if (b.slot[i].id < 0 || b.slot[i].id > 0xFF || b.slot[i].count < 0 || b.slot[i].count > 0xFF) return false;
  for (int i = 0; i < size; ++i) {
    poke<uint8_t>(p + 2u * i, static_cast<uint8_t>(b.slot[i].id));
    poke<uint8_t>(p + 2u * i + 1, static_cast<uint8_t>(b.slot[i].count));
    poke<uint8_t>(g.rows + i, static_cast<uint8_t>(b.row[i]));
  }
  poke<uint8_t>(g.held, static_cast<uint8_t>(b.held));
  poke<uint8_t>(g.rows_used, static_cast<uint8_t>(b.rows_used));
  return true;
}

int character_id() { return g.entity_id ? (peek<uint8_t>(g.entity_id) & 3) : -1; }

bool item_rules_known() { return g.item_table && g.stack_hi; }
int item_max(int id) {
  return g.item_table && id > 0 && id <= kItemTableLast ? peek<uint8_t>(g.item_table + 4u * id) : -1;
}
bool item_stacks(int id) {
  if (!g.stack_hi) return items::stacks(id);
  return (id >= g.stack_lo && id < g.stack_hi) || id == g.stack_ribbon;
}
int stack_cap() { return g.stack_cap ? g.stack_cap : items::kStackCap; }

bool options_frame_known() { return g.opt_frame && g.task_esp; }

// options_menu runs as task 1, entered with no return address at the top of
// its own stack, so its frame sits a little way above the saved stack pointer
// of the task suspended in Task_sleep - which is where every task is while the
// tick runs. It is found by what the prologue put there - the three sub-menu
// handlers side by side and the camera matrix it cleared - and it has to be the
// only such place.
bool find_options_frame(OptionsFrame* out) {
  if (!options_frame_known() || !options_open()) return false;
  const uintptr_t esp = task_saved_esp(1);
  if (!esp) return false;
  size_t span = 0x2000;
  while (span > 0x100 && !mem::readable(esp, span)) span -= 0x100;
  if (!mem::readable(esp, span)) return false;
  uintptr_t found = 0;
  for (uintptr_t a = (esp + 3) & ~uintptr_t(3); a + 12 <= esp + span; a += 4) {
    if (u32(a) != g.opt_handler[0] || u32(a + 4) != g.opt_handler[1] || u32(a + 8) != g.opt_handler[2]) continue;
    const uintptr_t base = a - kOptHandlers;
    if (base < esp || !mem::readable(base, static_cast<size_t>(g_opt_entity) + kEntitySize)) continue;
    bool matrix = true;
    for (int m = 0; m < kOptMatrixSize; m += 4)
      matrix = matrix && u32(base + kOptMatrix + m) == (kOptMatrix + m == kOptMatrixM11 ? kOptMatrixM11Value : 0u);
    if (!matrix) continue;
    if (found) return false;  // two frames that look alike: neither can be trusted
    found = base;
  }
  if (!found) return false;
  out->equipped = found + g_opt_equipped;
  out->entity = found + g_opt_entity;
  return true;
}

int saved_equipped(const OptionsFrame& f) {
  uint8_t v = 0;
  return f.equipped && mem::read_safe(f.equipped, &v) ? v : -1;
}

bool set_saved_equipped(const OptionsFrame& f, int equipped) {
  if (!f.equipped || equipped < 0 || equipped > kMaxBagSlots || !mem::readable(f.equipped, 1)) return false;
  poke<uint8_t>(f.equipped, static_cast<uint8_t>(equipped));
  return true;
}

int saved_character(const OptionsFrame& f) {
  uint8_t v = 0;
  return f.entity && mem::read_safe(f.entity + kEntityCharacter, &v) ? (v & 3) : -1;
}

// The one game function the mod calls. Its bytes are verified again at the
// moment of use, like a patch site's, and it is only ever run from the
// main-thread tick, between frames - the thread a pickup runs it on, with the
// inventory in the same packed state: it recounts the inventory in play
// (CountHeldItems), numbers the icon rows afresh and draws each slot's item
// into its row (LoadItemImage).
bool rebuild_item_icons() {
  if (!g.icons || !g.count_held || !bag_array()) return false;
  if (!mem::matches(g.icons, mem::parse_pattern(kSigIcons)) || g.icons + 6 + i32(g.icons + 2) != g.count_held)
    return false;
  reinterpret_cast<void(__cdecl*)()>(g.icons)();
  return true;
}

void trace_tick() {
  const config::Settings& c = config::get();
  if (!c.nwatch) return;
  const DWORD now = GetTickCount();
  if (now - g_watch_second >= 1000) {
    if (g_watch_lines > 60) logf("watch: %d change(s) in the last second were not logged", g_watch_lines - 60);
    g_watch_second = now;
    g_watch_lines = 0;
  }
  for (int i = 0; i < c.nwatch; ++i) {
    const uintptr_t a = c.watch[i].addr;
    uint32_t v = 0;
    bool ok = false;
    if (c.watch[i].size == 1) {
      uint8_t b = 0;
      ok = mem::read_safe(a, &b);
      v = b;
    } else if (c.watch[i].size == 2) {
      uint16_t w = 0;
      ok = mem::read_safe(a, &w);
      v = w;
    } else {
      ok = mem::read_safe(a, &v);
    }
    if (!ok || (g_watch_seen[i] && v == g_watch_last[i])) continue;
    if (++g_watch_lines <= 60) {
      if (g_watch_seen[i]) logf("watch: 0x%08X = 0x%X (was 0x%X)", static_cast<unsigned>(a), v, g_watch_last[i]);
      else logf("watch: 0x%08X = 0x%X", static_cast<unsigned>(a), v);
    }
    g_watch_seen[i] = true;
    g_watch_last[i] = v;
  }
}

}  // namespace re1cc::game
