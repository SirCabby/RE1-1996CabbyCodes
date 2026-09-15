#pragma once

#include <windows.h>

namespace re1cc::proxy {

// Load ddraw_orig.dll from `dir` (absolute path), pin it, and resolve every
// export the proxy forwards into the jump table the thunks use. False means
// the game cannot run: the caller should tell the user and refuse to start.
bool load_original(const char* dir);

// ddraw_orig.dll once it is loaded (null before): its import table is where the
// overlay finds Direct3DCreate9.
HMODULE original();

}  // namespace re1cc::proxy
