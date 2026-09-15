#pragma once

#include <windows.h>

#include <cstdint>

// The save files, managed from the title's Load Game list: while the list is up
// the panel shows every file and can delete one or copy one over another. RE1
// keeps each save in a file of its own - SAVE\savedat1..8.dat beside the game,
// 0xA82 bytes, no checksum - so a change is a plain file operation, and a
// missing file is the game's own empty slot.
//
// The list itself reads the files once, when it opens, into a table of its own
// (9 x {character, saves, stage, room, has-data} dwords in LoadSaveGameState's
// frame, in task 0's stack), draws its rows from that table every frame, and
// trusts it when a file is picked: its load reads the file without checking.
// So after a change the mod writes the new entry into that table as well - the
// list shows the change at once and never tries to load a file that is gone.
// The table is found by content (its entries are exactly what the files say)
// and checked again before every write; without it, Delete is refused.
namespace re1cc::savefiles {

constexpr int kFiles = 8;         // savedat1..8.dat (the list's eight rows)
constexpr int kFileSize = 0xA82;  // what the game writes

struct File {
  bool known = false;     // the slot could be read (a missing file is known and empty)
  bool used = false;      // the file exists - the game's own test for a slot with a save
  int size = 0;           // bytes on disk
  int character = -1;     // 0 Chris, 1 Jill (byte 0x22B & 3; -1 = not known)
  int char_byte = -1;     // byte 0x22B as it is (what the game's table holds)
  int saves = -1;         // the save count it records (byte 0x228)
  float seconds = -1.0f;  // its play time (u32 at 0x224, 30 per second)
  int stage = -1;         // byte 0x200
  int room = -1;          // byte 0x201
  uint32_t hash = 0;      // of the whole file
};
// Two readings of a slot are the same save: what a request checks before it acts.
bool same_save(const File& a, const File& b);

// A copy of everything the panel draws, taken under the lock.
struct View {
  bool up = false;          // the Load Game list is on screen
  bool ready = false;       // the save folder is known
  bool list_table = false;  // the list's own table of the files was found (Delete needs it)
  int files = 0;            // slots listed (kFiles once the folder is read)
  File file[kFiles];
  char folder[MAX_PATH] = {};
  char note[200] = {};      // what the last change came to
  bool note_error = false;
};

void init();     // from DllMain: the lock
View view();     // any thread
bool showing();  // main thread: the list is up, so the panel belongs on screen

// Requests from the panel, carried out on the next main-thread tick. What the
// panel saw of the files goes with them: a file can change between the click
// and the tick, and changing a file that is not the one clicked is worse than
// not changing anything.
void request_delete(int slot, const File& seen);
void request_copy(int from, int to, const File& from_seen, const File& to_seen);

// Main thread, every tick. `list_up`: the Load Game list is on screen;
// [search_lo, search_hi): where to look for its table (task 0's stack, above its
// saved ESP), empty when not known.
void tick(bool list_up, uintptr_t search_lo, uintptr_t search_hi);

// For Save without counting (cheats.cpp). The game writes a save file and only
// then adds the save to its counter - and it adds one again to a file it
// loads, since the file holds the count from before its own save. A save the
// counter skipped therefore has to reach the disk one lower to load back the
// same count: the files are stamped when the save screen opens, the one written
// is found when it closes, and its count byte is lowered.
struct Stamp {
  bool ok = false;
  FILETIME write[kFiles] = {};
  bool exists[kFiles] = {};
};
Stamp stamp();                                // main thread: the files' write times now
int written_since(const Stamp& before);       // the one slot written since, or -1
bool lower_save_count(int slot, int expect);  // byte 0x228 from `expect` to expect-1, atomically

}  // namespace re1cc::savefiles
