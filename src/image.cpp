#include "image.h"

#include "log.h"

namespace re1cc::image {
namespace {

HMODULE g_module = nullptr;
uintptr_t g_base = 0;
mem::Range g_text, g_rdata, g_data, g_idata, g_game, g_all;
bool g_layout_ok = false;

}  // namespace

void init() {
  if (g_module) return;
  g_module = GetModuleHandleA(nullptr);
  g_base = reinterpret_cast<uintptr_t>(g_module);
  g_all = mem::module_range(g_module);
  IMAGE_NT_HEADERS* nt = mem::nt_headers(g_module);
  if (!nt || nt->FileHeader.NumberOfSections < 6) return;
  const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
  g_text = mem::section_range(g_module, s[0]);
  g_rdata = mem::section_range(g_module, s[1]);
  g_data = mem::section_range(g_module, s[2]);
  g_idata = mem::section_range(g_module, s[3]);
  // The 1997 image ends where its .reloc (section 5) ends; Enigma's sections
  // start after it.
  const mem::Range reloc = mem::section_range(g_module, s[5]);
  g_game = mem::Range{g_base, reloc.end};
  g_layout_ok = (s[0].Characteristics & IMAGE_SCN_MEM_EXECUTE) && g_text.begin < g_rdata.begin &&
                g_rdata.begin < g_data.begin && g_data.begin < g_idata.begin && !g_idata.empty();
}

HMODULE module() { return g_module; }
uintptr_t base() { return g_base; }
bool layout_ok() { return g_layout_ok; }
const mem::Range& text() { return g_text; }
const mem::Range& rdata() { return g_rdata; }
const mem::Range& data() { return g_data; }
const mem::Range& idata() { return g_idata; }
const mem::Range& game_image() { return g_game; }
const mem::Range& all() { return g_all; }
uint32_t rva(uintptr_t va) { return g_all.contains(va) ? static_cast<uint32_t>(va - g_base) : 0; }

void log_sections() {
  IMAGE_NT_HEADERS* nt = mem::nt_headers(g_module);
  if (!nt) {
    logf("image: no PE headers at %p", static_cast<void*>(g_module));
    return;
  }
  logf("image: base %p, SizeOfImage 0x%lX, entry RVA 0x%lX, %u sections (layout %s)", static_cast<void*>(g_module),
       nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.AddressOfEntryPoint, nt->FileHeader.NumberOfSections,
       g_layout_ok ? "as expected" : "NOT as expected");
  const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    char name[9] = {};
    std::memcpy(name, s[i].Name, 8);
    logf("image:   [%2u] '%s' va 0x%08lX vsize 0x%08lX raw 0x%08lX flags 0x%08lX", i, name, s[i].VirtualAddress,
         s[i].Misc.VirtualSize, s[i].SizeOfRawData, s[i].Characteristics);
  }
}

}  // namespace re1cc::image
