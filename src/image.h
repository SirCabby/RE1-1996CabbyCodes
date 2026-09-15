#pragma once

#include <windows.h>

#include <cstdint>

#include "mem.h"

// The game executable's sections, resolved once. Enigma blanks the section
// names, so they are taken by position: the wrapper keeps the 1997 image's own
// sections first, at their original addresses (code, .rdata, .data, .idata,
// .rsrc, .reloc), and appends its own after them. Everything that scans the
// image goes through here so the bounds are consistent and cheap to reuse.
namespace re1cc::image {

void init();
HMODULE module();
uintptr_t base();
bool layout_ok();                // the first four sections look like code, rdata, data, idata
const mem::Range& text();        // the game's code (section 0)
const mem::Range& rdata();       // section 1
const mem::Range& data();        // section 2, .bss included (VirtualSize > SizeOfRawData)
const mem::Range& idata();       // section 3: the import table Enigma rebuilds at run time
const mem::Range& game_image();  // base .. the end of the 1997 image (before Enigma's sections)
const mem::Range& all();         // the whole module, Enigma's sections included
uint32_t rva(uintptr_t va);      // 0 when outside the image
void log_sections();

}  // namespace re1cc::image
