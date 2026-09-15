#pragma once

#include <windows.h>

// The input guard. The game reads the keyboard by polling GetAsyncKeyState
// every frame (the decomp's UpdateKeyboardInputState) and the pad through
// winmm; it reads no mouse, and its window procedure only acts on F1 and F9.
// So while a panel field has keyboard focus, the game's GetAsyncKeyState reads
// every key as up - except Escape, which always leaves the option screen.
//
// Enigma resolves the game's imports at run time, so the hook reaches the game
// either through the exe's GetProcAddress hook (wrap) or, once the image is
// unpacked, by re-pointing its slot in the rebuilt import table by value
// (after_unpack). `Disable = input` in the ini turns the guard off.
namespace re1cc::input {

FARPROC wrap(const char* name, FARPROC real);  // from the GetProcAddress hook
void after_unpack();                            // main thread, once
void uninstall();

}  // namespace re1cc::input
