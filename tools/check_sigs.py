#!/usr/bin/env python3
"""Run every signature src/game.cpp relies on against the dump, before a launch.

    python3 tools/check_sigs.py [ghidra_proj/ResidentEvil.fixed.exe]

Each signature must match exactly once in the code section; the values it
yields (globals read out of instruction operands, patch sites) are cross-checked
the way game.cpp checks them, and printed. Keep this file and game.cpp's
signatures in step: this is the only place a signature can be proven wrong
without starting the game. Pure stdlib.
"""
import re, struct, sys

BASE = 0x400000


def load(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    opt = pe + 24
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    vsz, va, _rsz, rptr = struct.unpack_from("<IIII", d, opt + optsz + 8)  # section 0: the code
    return d, rptr - va, (BASE + va, BASE + va + vsz)


D, OFF, (T0, T1) = load(sys.argv[1] if len(sys.argv) > 1 else "ghidra_proj/ResidentEvil.fixed.exe")


def rx(sig):
    return re.compile(b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in sig.split()), re.S)


def find(sig, lo=None, hi=None):
    """Every match of `sig` in [lo, hi) (default: the code section), as VAs."""
    lo, hi = lo or T0, hi or T1
    blob = D[lo - BASE + OFF: hi - BASE + OFF]
    return [lo + m.start() for m in rx(sig).finditer(blob)]


def u32(va): return struct.unpack_from("<I", D, va - BASE + OFF)[0]
def i32(va): return struct.unpack_from("<i", D, va - BASE + OFF)[0]
def byte(va): return D[va - BASE + OFF]


fails = 0
found = {}


def one(name, sig, lo=None, hi=None):
    global fails
    hits = find(sig, lo, hi)
    ok = len(hits) == 1
    print(f"{'ok  ' if ok else 'FAIL'} {name:34} {len(hits)} match(es)" + (f" at {hits[0]:#x}" if ok else ""))
    if not ok:
        fails += 1
        return None
    found[name] = hits[0]
    return hits[0]


def check(cond, what):
    global fails
    print(f"       {'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        fails += 1


# --- check_menus_state: the menu flags, the load/save flag, the two menu tasks, Task_execute
a = one("check_menus_state", "C7 05 ?? ?? ?? ?? 01 00 00 00 83 C4 04 F6 05 ?? ?? ?? ?? 40 74 1B 81 25 ?? ?? ?? ?? FF FF BF FF "
        "C7 05 ?? ?? ?? ?? 01 00 00 00 68 ?? ?? ?? ?? EB 05 68 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 08 81 0D ?? ?? ?? ?? 00 80 00 00")
if a:
    flags, loadsave, options_menu, main_menu = u32(a + 24), u32(a + 34), u32(a + 43), u32(a + 50)
    task_execute = a + 61 + i32(a + 57)
    check(u32(a + 15) == flags + 2 and u32(a + 66) == flags, f"flags {flags:#x} (tested byte +2, or'd 0x8000)")
    print(f"       load/save flag {loadsave:#x}, options_menu {options_menu:#x}, main_menu {main_menu:#x}, Task_execute {task_execute:#x}")
    # --- Task_execute: the task tables
    sig = "8B 44 24 08 8B 4C 24 04 89 04 8D ?? ?? ?? ?? 8B C1 C1 E0 05 2B C1 66 C7 04 85 ?? ?? ?? ?? 02 00 C3"
    t = one("Task_execute (at the call target)", sig, task_execute, task_execute + 40)
    if t:
        eip_table, tcb_table = u32(t + 11), u32(t + 26)
        print(f"       task entry table {eip_table:#x}, task blocks {tcb_table:#x} (stride 0x7C)")
        # --- Task_chain: sets the running task's entry; its pushes name game_start / title_state
        c = one("Task_chain", "8B 0D ?? ?? ?? ?? 8B 44 24 04 8B 15 ?? ?? ?? ?? 89 04 8D ?? ?? ?? ?? 66 C7 02 02 00")
        if c:
            check(u32(c + 19) == eip_table, "Task_chain writes the same entry table")
            found["Task_chain_va"] = c

def hexbytes(v): return " ".join(f"{b:02X}" for b in struct.pack("<I", v))


# --- the task switch: every suspended task's saved ESP (the load list's slot table lives
#     in task 0's stack, found by content from there)
if "Task_chain_va" in found:
    sw = one("task switch (load a task's ESP)", "A1 ?? ?? ?? ?? 89 25 ?? ?? ?? ?? 8B 24 85 ?? ?? ?? ?? 90 C3")
    if sw:
        cur = u32(found["Task_chain_va"] + 2)
        check(u32(sw + 1) == cur, f"current task {cur:#x} (as Task_chain reads it); saved ESPs {u32(sw + 14):#x}, scheduler ESP {u32(sw + 7):#x}")

# --- the title menu's loop flag: title_exit_loop clears it and then reads the menu flags
#     (the signature is built from the flags address check_menus_state gave), init_title_screen sets it
if a:
    t = one("title_exit_loop", "C6 05 ?? ?? ?? ?? 00 A1 " + hexbytes(flags) + " 25")
    if t:
        title_flag = u32(t + 2)
        sets = find("C6 05 " + hexbytes(title_flag) + " 01")
        check(len(sets) == 1, f"title flag {title_flag:#x}; one site sets it to 1 ({', '.join(hex(s) for s in sets)})")

# --- game_start: title_state restores the play time from the save and chains its task to it
if a and "Task_chain_va" in found:
    q = one("title_state -> game_start", "A1 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 00 00 00 00 A3 ?? ?? ?? ?? A1 " + hexbytes(flags) +
            " 25 FF FF FF 3F 0D 00 00 00 40 A3 " + hexbytes(flags) + " E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8")
    if q:
        game_start = u32(q + 46)
        chain = q + 55 + i32(q + 51)
        check(u32(q + 7) == loadsave and chain == found["Task_chain_va"],
              f"game_start {game_start:#x}; it clears the load/save flag and calls Task_chain; play time {u32(q + 16):#x}")

# --- player: HP and max HP from the health bar, the status byte from the poison tick
h = one("menu_draw_health_bar", "66 39 05 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 8A 0D ?? ?? ?? ?? 33 DB 0F BF 05 ?? ?? ?? ?? C0 E9 02")
if h:
    hp, maxhp = u32(h + 3), u32(h + 15)
    check(u32(h + 24) == hp and maxhp - hp == 0x175 - 0x88, f"HP {hp:#x}, max HP {maxhp:#x} (entity {hp - 0x88:#x})")
    p = one("poison tick", "F6 05 ?? ?? ?? ?? 62 74 3D A0 ?? ?? ?? ?? FE 0D ?? ?? ?? ?? 84 C0 75 2E C6 05 ?? ?? ?? ?? 78")
    if p:
        check(u32(p + 2) == hp + 0x54, f"status byte {u32(p + 2):#x} = entity+0xDC")

# --- enemies: the loop that clears every slot's status byte
e = one("enemy slot clear loop", "B8 ?? ?? ?? ?? C6 00 00 05 ?? ?? ?? ?? 3D ?? ?? ?? ?? 72 F1")
if e:
    ebase, stride, end = u32(e + 1), u32(e + 9), u32(e + 14)
    check(stride and (end - ebase) % stride == 0 and (end - ebase) // stride == 30, f"enemies {ebase:#x}, stride {stride:#x}, {(end - ebase) // stride if stride else 0} slots")

# --- ammo: the two firing decrements, both reading the same slot pointer and equipped slot
x = one("ammo decrement (guns)", "33 C0 8B 0D ?? ?? ?? ?? A0 ?? ?? ?? ?? FE 4C 41 FF 66 83 FB 08")
y = one("ammo decrement (flamethrower)", "A1 ?? ?? ?? ?? 8A 0D ?? ?? ?? ?? FE 4C 48 FF F6 05")
if x and y:
    check(u32(x + 4) == u32(y + 1) and u32(x + 9) == u32(y + 7), f"slot pointer {u32(x + 4):#x}, equipped slot {u32(x + 9):#x}; patch sites {x + 13:#x}, {y + 11:#x}")

# --- play time: game_loop's increment and GOG's inventory code cave
g = one("Game_timer in game_loop", "6A 01 E8 ?? ?? ?? ?? 83 C4 04 FF 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 C0 A0")
c = one("Game_timer in the inventory (GOG cave)", "60 A0 ?? ?? ?? ?? B4 01 84 C0 74 06 FF 05 ?? ?? ?? ?? 30 E0 A2")
if g and c:
    check(u32(g + 12) == u32(c + 14), f"Game_timer {u32(g + 12):#x}; patch sites {g + 10:#x}, {c + 12:#x}")

# --- the self-destruct counter's +1 and its active bit
k = one("countdown step", "66 81 3D ?? ?? ?? ?? FE 7F 89 1D ?? ?? ?? ?? 73 11 85 35 ?? ?? ?? ?? 75 10 66 FF 05 ?? ?? ?? ?? EB 07 "
        "66 89 1D ?? ?? ?? ?? F6 05 ?? ?? ?? ?? 08")
if k and a:
    cd = u32(k + 3)
    check(u32(k + 28) == cd and u32(k + 37) == cd, f"countdown {cd:#x}; patch site {k + 25:#x}")
    check(u32(k + 43) == flags + 7, f"its active bit is in the second flags word ({flags + 4:#x}, 0x08000000)")

# --- save count: the increment after the file is written
s = one("save count increment", "7C F2 FE 05 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 64 72 07 C6 05 ?? ?? ?? ?? 63")
if s:
    sc = u32(s + 4)
    check(u32(s + 10) == sc and u32(s + 19) == sc, f"save count {sc:#x}; patch site {s + 2:#x}")

# --- ink ribbons: the typewriter's check and the save's take
w = one("typewriter ribbon check", "6A 2F E8 ?? ?? ?? ?? 83 C4 04 8B F0 85 F6 7C 21 8B 44 24 08 A3 ?? ?? ?? ?? 66 89 70 02 33 C0 C6 05 ?? ?? ?? ?? 01")
if w:
    branch = w + 16 + 0x21
    ok = D[branch - BASE + OFF: branch - BASE + OFF + 2] == b"\x80\x3D" and D[branch + 0x25 - BASE + OFF: branch + 0x2A - BASE + OFF] == b"\x8B\x44\x24\x08\xA3"
    check(ok, f"typewriter state {u32(w + 33):#x}; no-ribbon branch {branch:#x} -> the game's no-ribbon save at {branch + 0x25:#x}")
    if h:
        check(u32(branch + 2) == hp - 0x88 + 1, f"the branch tests the player's character byte {u32(branch + 2):#x}")
r = one("save's ribbon take", "83 BC 24 ?? ?? 00 00 00 74 2A A0 ?? ?? ?? ?? 24 03 3C 01 75 13 6A 7B 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 85 C0 74 0C C6 05 ?? ?? ?? ?? 2F E8")
if r:
    print(f"       patch site {r + 8:#x} (74 -> EB)")


def u16(va): return struct.unpack_from("<H", D, va - BASE + OFF)[0]


# --- the inventory: rearrange_item_slots packs it (the count, the icon rows and the character
#     byte it sizes the inventory by), checked against the ammo decrement's slot pointer and
#     equipped byte
inv = {}
if x and y and h:
    ptr, eq = u32(x + 4), u32(x + 9)
    r = one("rearrange_item_slots",
            "83 EC 04 A0 ?? ?? ?? ?? FE C8 B1 04 88 44 24 02 53 56 A0 ?? ?? ?? ?? 24 03 3C 01 0F 95 C2 2A CA 33 D2 02 C9 "
            "88 54 24 0B 88 4C 24 08 88 4C 24 09 33 C9 A1 ?? ?? ?? ?? 8A CA 8A 04 48 84 C0 74 3F 38 54 24 0B 74 33 33 DB "
            "8B 35 ?? ?? ?? ?? 8A 5C 24 0B 38 54 24 0A 88 04 5E 8B 35 ?? ?? ?? ?? 8A 44 4E 01 88 44 5E 01 8A 89 ?? ?? ?? ?? "
            "88 8B ?? ?? ?? ?? 75 04 88 5C 24 0A FE 44 24 0B EB 1A 38 15 ?? ?? ?? ?? 76 12 B0 01 8A 89 ?? ?? ?? ?? D2 E0 "
            "F6 D0 20 05 ?? ?? ?? ??")
    if r:
        rows = u32(r + 105)
        ok = (u32(r + 4) == eq and u32(r + 19) == hp - 0x88 + 1 and u32(r + 51) == ptr and u32(r + 74) == ptr
              and u32(r + 91) == ptr and u32(r + 111) == rows and u32(r + 139) == rows)
        check(ok, f"slot pointer {ptr:#x}, equipped {eq:#x}, count {u32(r + 129):#x}, icon rows {rows:#x}, "
                  f"row bits {u32(r + 149):#x}, character byte {u32(r + 19):#x}")
        if ok:
            inv = dict(ptr=ptr, eq=eq, held=u32(r + 129), rows=rows, used=u32(r + 149), eid=u32(r + 19))

# --- the two arrays the slot pointer is ever set to, and the item box in front of them
if inv:
    arrays = sorted({u32(v + 6) for v in find("C7 05 " + hexbytes(inv["ptr"]) + " ?? ?? ?? ??")})
    ok = len(arrays) == 2 and arrays[1] - arrays[0] == 12
    check(ok, "the slot pointer is set to " + ", ".join(hex(v) for v in arrays) + " (main inventory, Rebecca's)")
    b = one("item box swap",
            "8A 0D ?? ?? ?? ?? 8A D8 2B CB 83 F9 01 75 07 C6 05 ?? ?? ?? ?? 00 33 C9 8A 0D ?? ?? ?? ?? 8A 14 4D ?? ?? ?? ?? "
            "8D 34 4D 00 00 00 00 8A 0C 4D ?? ?? ?? ?? 88 4C 24 0F 8B 0D ?? ?? ?? ?? 8D 3C 59 8A 0F 88 8E ?? ?? ?? ?? 8A 4F 01 "
            "88 8E ?? ?? ?? ?? 8A 4C 24 0F 88 17 8B 35 ?? ?? ?? ?? 88 4C 5E 01 38 05 ?? ?? ?? ??")
    if b and ok:
        box, pos = u32(b + 33), u32(b + 26)
        same = (u32(b + 2) == inv["eq"] and u32(b + 17) == inv["eq"] and u32(b + 68) == box and u32(b + 47) == box + 1
                and u32(b + 77) == box + 1 and u32(b + 57) == inv["ptr"] and u32(b + 89) == inv["ptr"]
                and u32(b + 99) == inv["held"])
        tops = [byte(v + 6) for v in find("C6 05 " + hexbytes(pos) + " ??")]
        size = max(tops) + 1 if tops else 0
        check(same and arrays[0] - box == size * 2,
              f"item box {box:#x}, {size} slots (its cursor {pos:#x} wraps to {', '.join(hex(t) for t in tops)}); "
              f"the inventory follows it at {arrays[0]:#x}")

# --- the game's item rules: the per-slot maximum (the combine's two reload helpers), the icon
#     rebuild and the CountHeldItems it calls, which items stack and how far (the pickup)
if inv:
    l = one("LoadHeldItemsImages",
            "53 E8 ?? ?? ?? ?? 8A 1D ?? ?? ?? ?? B0 01 8A CB D2 E0 FE C8 84 DB A2 ?? ?? ?? ?? 74 33 FE CB 33 D2 8A D3 68 "
            "?? ?? ?? ?? 52 A1 ?? ?? ?? ?? 33 C9 88 9A ?? ?? ?? ?? 8A 0C 50 33 C0 8A 04 8D ?? ?? ?? ?? 48 50 E8 ?? ?? ?? ?? "
            "83 C4 0C 84 DB 75 CD 5B C3")
    m = find("66 83 E3 7F 66 03 D3 33 DB 8A 19 0F B7 FA 8A 0C 9D ?? ?? ?? ?? 33 DB 8A D9 3B DF 73 12")
    tables = sorted({u32(v + 17) for v in m})
    if l:
        cnt = l + 6 + i32(l + 2)
        cs = ("53 B1 04 A0 " + hexbytes(inv["eid"]) + " 24 03 3C 01 A1 " + hexbytes(inv["ptr"]) +
              " 0F 95 C2 2A CA 8D 14 4D 00 00 00 00 32 C9 38 08 74 15 3A CA 73 11 FE C1 33 DB 8A D9 A1 " +
              hexbytes(inv["ptr"]) + " 80 3C 58 00 75 EB 5B 88 0D " + hexbytes(inv["held"]) + " C3")
        ok = (u32(l + 8) == inv["held"] and u32(l + 23) == inv["used"] and u32(l + 42) == inv["ptr"]
              and u32(l + 50) == inv["rows"] and find(cs, cnt, cnt + 64) == [cnt])
        check(ok, f"icon rebuild {l:#x}, calling CountHeldItems {cnt:#x}; icon column {u32(l + 62):#x}")
    ok = len(m) >= 1 and len(tables) == 1 and (not l or u32(l + 62) == tables[0] + 1)
    mags = {i: byte(tables[0] + 4 * i) for i in range(2, 11)} if len(tables) == 1 else {}
    check(ok, f"item table {', '.join(hex(t) for t in tables)} ({len(m)} reader(s)); magazines " +
              ", ".join(f"{i:#x}:{v}" for i, v in mags.items()))
    p = one("pickup stacking",
            "80 3D ?? ?? ?? ?? ?? 72 09 80 3D ?? ?? ?? ?? ?? 72 0D 80 3D ?? ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? B2 04 A0 ?? ?? ?? ?? "
            "24 03 C6 44 24 0A 00 3C 01 0F 95 C3 2A D3 02 D2 8A DA 88 54 24 09 33 C0 8A 44 24 0A 03 C0 03 05 ?? ?? ?? ?? 38 08 "
            "75 21 66 0F B6 50 01 66 0F B6 44 24 0B 66 03 D0 66 81 FA ?? ?? 76 18")
    if p:
        sel = u32(p + 2)
        lo, hi, rib, cap = byte(p + 6), byte(p + 15), byte(p + 24), u16(p + 95)
        ok = (u32(p + 11) == sel and u32(p + 20) == sel and u32(p + 34) == inv["eid"] and u32(p + 70) == inv["ptr"]
              and lo < hi <= rib and 0 < cap < 256)
        check(ok, f"items {lo:#x}..{hi - 1:#x} and {rib:#x} stack, up to {cap} in a slot")

# --- the option screen's frame: its prologue (the sub-menu handler table and the camera matrix
#     that pin the frame down), the copy of the entity and the equipped byte it makes as it opens,
#     and the copy back as it closes
if a and inv:
    o = options_menu
    pro = find("81 EC ?? ?? 00 00 53 56 57 33 DB 89 5C 24 1C 55 89 5C 24 24 89 5C 24 2C 89 5C 24 30 89 5C 24 34 89 5C 24 38 "
               "89 5C 24 3C C7 44 24 28 88 13 00 00 53 88 1D ?? ?? ?? ?? C7 44 24 18 ?? ?? ?? ?? C7 44 24 1C ?? ?? ?? ?? "
               "C7 44 24 20 ?? ?? ?? ?? E8", o, o + 90)
    check(pro == [o], f"options_menu {o:#x} opens with its handler table {u32(o + 59):#x}, {u32(o + 67):#x}, "
                      f"{u32(o + 75):#x} and its camera matrix")
    s = one("options_menu's save", "8D 4C 24 ?? 83 C4 04 68 80 01 00 00 68 ?? ?? ?? ?? 51 E8 ?? ?? ?? ?? 83 C4 0C 8A 0D ?? ?? ?? ?? "
            "C6 05 ?? ?? ?? ?? 01 88 4C 24 ??", o, o + 0x400)
    t = one("options_menu's restore", "8D 44 24 ?? 68 80 01 00 00 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8A 44 24 ?? 83 C4 0C A2 ?? ?? ?? ?? E8",
            o, o + 0x1000)
    if pro == [o] and s and t:
        ent, eqo = byte(s + 3) - 4, byte(s + 42)
        ok = (u32(s + 13) == hp - 0x88 and u32(s + 28) == inv["eq"] and u32(s + 34) == inv["eq"] and u32(t + 11) == hp - 0x88
              and u32(t + 28) == inv["eq"] and byte(t + 3) == ent and byte(t + 23) - 12 == eqo and ent >= 0x40 and eqo < 0x14)
        check(ok, f"frame: handlers at +0x14, camera matrix at +0x20, saved entity at +{ent:#x}, saved equipped byte at +{eqo:#x}")

print(f"\n{'all signatures verified' if not fails else f'{fails} FAILURE(S)'}")
sys.exit(1 if fails else 0)
