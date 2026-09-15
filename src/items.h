#pragma once

#include <cstdio>

// Resident Evil (1996) item ids, as an inventory or item-box slot stores them
// ({id, count}, one byte each). The names are the game's own, from its item
// name table (0x4BF0A0, indexed by id - 1), in the case a reader expects; an
// item the game shows under another name until it is examined carries that
// name in brackets, so typing either one finds it. The ids the game uses for
// something else are left out: 0x32 and 0x4C have no name, 0x4D (the radio)
// only raises a flag, 0x4E..0x53 are the maps and 0x5F..0x6E the files.
//
// Two ids share a name in the game and are told apart here: a Colt Python and
// a Bazooka are a different item for each kind of round they are loaded with.
// The six mixed herbs are named from the game's combine table (0x4BD768): what
// the herb picker makes of which herbs.
//
// `kind` is the mod's own grouping: it picks the item-box bank an item is
// listed under (Bank, below). Which items stack, how much one slot of them
// holds and how many rounds a gun takes are the game's, read out of its code
// (game::item_stacks, game::stack_cap, game::item_max); `stacks` and
// `magazine` here are only the fallback for a build where that code is not
// found.
namespace re1cc::items {

enum Kind { kNone = 0, kWeapon, kAmmo, kHeal, kKey, kOther };

struct Def {
  int id;
  const char* name;
  Kind kind;
};

inline const Def kTable[] = {
    {0x00, "None", kNone},
    {0x01, "Combat Knife", kWeapon},
    {0x02, "Beretta", kWeapon},
    {0x03, "Shotgun", kWeapon},
    {0x04, "Colt Python (dumdum rounds)", kWeapon},
    {0x05, "Colt Python (magnum rounds)", kWeapon},
    {0x06, "Flamethrower", kWeapon},
    {0x07, "Bazooka (explosive rounds)", kWeapon},
    {0x08, "Bazooka (acid rounds)", kWeapon},
    {0x09, "Bazooka (flame rounds)", kWeapon},
    {0x0A, "Rocket Launcher", kWeapon},
    {0x0B, "Clip", kAmmo},
    {0x0C, "Shells", kAmmo},
    {0x0D, "Dumdum Rounds", kAmmo},
    {0x0E, "Magnum Rounds", kAmmo},
    {0x0F, "Fuel", kAmmo},
    {0x10, "Explosive Rounds", kAmmo},
    {0x11, "Acid Rounds", kAmmo},
    {0x12, "Flame Rounds", kAmmo},
    {0x13, "Empty Bottle", kOther},
    {0x14, "Water", kOther},
    {0x15, "UMB No.2", kOther},
    {0x16, "UMB No.4", kOther},
    {0x17, "UMB No.7", kOther},
    {0x18, "UMB No.13", kOther},
    {0x19, "Yellow-6", kOther},
    {0x1A, "NP-003", kOther},
    {0x1B, "V-JOLT", kOther},
    {0x1C, "Broken Shotgun", kKey},
    {0x1D, "Square Crank", kKey},
    {0x1E, "Hex. Crank", kKey},
    {0x1F, "Emblem", kKey},
    {0x20, "Gold Emblem", kKey},
    {0x21, "Blue Jewel", kKey},
    {0x22, "Red Jewel", kKey},
    {0x23, "Music Notes", kKey},
    {0x24, "Wolf Medal", kKey},
    {0x25, "Eagle Medal", kKey},
    {0x26, "Herbicide (Chemical)", kKey},
    {0x27, "Battery", kKey},
    {0x28, "MO Disk", kKey},
    {0x29, "Wind Crest", kKey},
    {0x2A, "Flare", kKey},
    {0x2B, "Slides", kKey},
    {0x2C, "Moon Crest", kKey},
    {0x2D, "Star Crest", kKey},
    {0x2E, "Sun Crest", kKey},
    {0x2F, "Ink Ribbon", kOther},
    {0x30, "Lighter", kKey},
    {0x31, "Lockpick", kKey},
    {0x33, "Sword Key", kKey},
    {0x34, "Armor Key", kKey},
    {0x35, "Shield Key", kKey},
    {0x36, "Helmet Key", kKey},
    {0x37, "Master Key (Lab Key)", kKey},
    {0x38, "Closet Key (Special Key)", kKey},
    {0x39, "002 Key (Dormitory Key)", kKey},
    {0x3A, "003 Key (Dormitory Key)", kKey},
    {0x3B, "C. Room Key", kKey},
    {0x3C, "P. Room Key (Lab Key)", kKey},
    {0x3D, "Desk Key (Small Key)", kKey},
    {0x3E, "Blank Book (Red Book)", kKey},
    {0x3F, "Doom Book 2", kKey},
    {0x40, "Doom Book 1", kKey},
    {0x41, "First Aid Spray", kHeal},
    {0x42, "Serum", kHeal},
    {0x43, "Red Herb", kHeal},
    {0x44, "Green Herb", kHeal},
    {0x45, "Blue Herb", kHeal},
    {0x46, "Mixed Herbs (green + red)", kHeal},
    {0x47, "Mixed Herbs (green + green)", kHeal},
    {0x48, "Mixed Herbs (green + blue)", kHeal},
    {0x49, "Mixed Herbs (green + red + blue)", kHeal},
    {0x4A, "Mixed Herbs (green x3)", kHeal},
    {0x4B, "Mixed Herbs (green x2 + blue)", kHeal},
    {0x6F, "Ingram", kWeapon},
    {0x70, "Minimi", kWeapon},
};
inline constexpr int kCount = sizeof(kTable) / sizeof(kTable[0]);
inline constexpr int kKnifeId = 0x01;
inline constexpr int kFlamethrowerId = 0x06;
inline constexpr int kRocketLauncherId = 0x0A;
inline constexpr int kInkRibbonId = 0x2F;

inline const Def* find(int id) {
  for (const Def& d : kTable)
    if (d.id == id) return &d;
  return nullptr;
}
inline const char* name(int id) {
  const Def* d = find(id);
  return d ? d->name : "(unknown item)";
}

// The PC version's two reward weapons (the Ingram and the Minimi) sit above
// every other item id, and the game treats everything above 0x6E as infinite:
// the inventory draws their count as the infinity sign (display_item_qty,
// 0x4645B0) and the fire gate tops their slot back up to 4 (0x45A4B0).
inline bool infinite(int id) { return id > 0x6E && id <= 0x70; }
inline bool is_weapon(int id) { return (id >= kKnifeId && id <= kRocketLauncherId) || infinite(id); }

// A gun's count is its loaded rounds, and the hold-fire guns keep a flag in the
// count's top bit (0x45A550 sets it once per trigger pull), which the game's
// own count display and fire gate mask off - all but the flamethrower, whose
// fuel runs to 240.
inline int shown_count(int id, int count) {
  return is_weapon(id) && !infinite(id) && id != kFlamethrowerId ? (count & 0x7F) : count;
}

// Which items stack, as build 21744136's pickup code has it (0x4517AB): the
// eight ammo items and ink ribbons.
inline bool stacks(int id) { return (id >= 0x0B && id < 0x13) || id == kInkRibbonId; }
inline constexpr int kStackCap = 250;  // ...up to this many in one slot (the same pickup code)
// How many rounds a gun takes, as build 21744136's item table has it (0x4BD81C).
inline int magazine(int id) {
  switch (id) {
    case 0x02: return 15;
    case 0x03: return 7;
    case 0x04:
    case 0x05:
    case 0x07:
    case 0x08:
    case 0x09: return 6;
    case 0x06: return 240;
    case 0x0A: return 4;
    default: break;
  }
  return 0;
}

// " x12" after an item's name, or nothing for an item whose count means
// nothing (the knife) and " (infinite)" for the two reward weapons.
struct Quantity {
  char text[20];
};
inline Quantity quantity(int id, int count) {
  Quantity q;
  if (infinite(id)) std::snprintf(q.text, sizeof(q.text), " (infinite)");
  else if (id == kKnifeId) q.text[0] = '\0';
  else std::snprintf(q.text, sizeof(q.text), " x%d", shown_count(id, count));
  return q;
}

// The item box's banks, in the order the panel's tabs show them. A bank is
// only where an entry is listed - the box is the game's 48 slots, kept in the
// order the game keeps them - so an item filed under the wrong bank is
// misplaced, never mishandled. Anything the table does not know is listed
// under Other.
enum Bank { kBankKey = 0, kBankWeapons, kBankAmmo, kBankHeal, kBankOther, kBankCount };
inline const char* bank_name(int b) {
  static const char* const kNames[kBankCount] = {"Key items", "Weapons", "Ammo", "Heals", "Other"};
  return b >= 0 && b < kBankCount ? kNames[b] : kNames[kBankOther];
}
inline int bank(int id) {
  const Def* d = find(id);
  switch (d ? d->kind : kOther) {
    case kKey: return kBankKey;
    case kWeapon: return kBankWeapons;
    case kAmmo: return kBankAmmo;
    case kHeal: return kBankHeal;
    default: return kBankOther;
  }
}

// Names compared the way a reader sorts them, case aside.
inline int compare_names(const char* a, const char* b) {
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + 32 : static_cast<int>(c); };
  for (;; ++a, ++b) {
    const int d = lower(*a) - lower(*b);
    if (d || !*a) return d;
  }
}

// The order the panel lists the item box in: bank by bank, and by name within
// a bank. An id the table has no name for goes last, by number.
inline bool listed_before(int a, int b) {
  const int ka = bank(a), kb = bank(b);
  if (ka != kb) return ka < kb;
  const Def* da = find(a);
  const Def* db = find(b);
  if (!da || !db) return da ? true : db ? false : a < b;
  const int c = compare_names(da->name, db->name);
  return c ? c < 0 : a < b;
}

}  // namespace re1cc::items
