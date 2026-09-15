#pragma once

namespace re1cc {

// Install a last-resort exception filter that records where a fatal fault
// happened, and in which module. Injected mods are the usual suspect for
// crashes at shutdown, so it needs to be possible to tell from the log whether
// the fault is in this DLL or somewhere else entirely.
void install_crash_logger();

// What the game's own SetUnhandledExceptionFilter import should point at once
// it can be reached (Enigma resolves the game's imports at run time): the game
// then sets the filter we chain to, and ours stays on top.
void* crash_filter_hook();

// Main thread, about once a second: put our filter back on top if something
// replaced it without going through the game's import.
void reassert_crash_filter();

// "module+0xRVA" for an address, for log lines.
void describe_address(void* addr, char* out, unsigned n);

}  // namespace re1cc
