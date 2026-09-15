#!/usr/bin/env python3
"""Fix up the image RE1-1996CabbyCodes dumps (DumpImage=1) for Ghidra.

    python3 tools/fix_dump.py --dump "$GAME_DIR/ResidentEvil.dumped.exe" [--out ResidentEvil.fixed.exe] [--oep 0x441350]

The mod writes the unpacked 1997 image - its six sections only, with raw
offsets == RVAs - but the PE headers are still Enigma's: the entry point is the
wrapper's, and the data directories point into sections the dump does not
have. This points the entry at the game's own code (WinMain, 0x441350 in build
21744136 per the decomp, unless --oep says otherwise), clears every data
directory, and - when .idata still holds the game's import descriptors -
points the import directory at them so Ghidra names the imports. Pure stdlib.
"""
import argparse, struct

IMAGE_BASE = 0x400000


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    opt_sz = struct.unpack_from("<H", d, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = opt + opt_sz + i * 40
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
        out.append((va, vsz, rptr, rsz))
    return pe, opt, out


def cstr(d, off, limit=64):
    end = d.find(b"\0", off, off + limit)
    return d[off:end] if end > off else b""


def find_import_descriptors(d, idata_rva, idata_size):
    """The first run of IMAGE_IMPORT_DESCRIPTORs in .idata whose names read as DLL names."""
    size = len(d)
    for start in range(idata_rva, idata_rva + idata_size - 20, 4):
        n = 0
        o = start
        while o + 20 <= idata_rva + idata_size:
            oft, _, _, name, ft = struct.unpack_from("<IIIII", d, o)
            if not (oft or name or ft):
                break
            if not (0 < name < size and 0 < ft < size):
                n = 0
                break
            if not cstr(d, name).lower().endswith(b".dll"):
                n = 0
                break
            n += 1
            o += 20
        if n >= 3:
            return start, n
    return None, 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--out")
    ap.add_argument("--oep", type=lambda s: int(s, 0), default=0x441350, help="entry point VA (default WinMain)")
    a = ap.parse_args()
    d = bytearray(open(a.dump, "rb").read())
    pe, opt, secs = sections(d)
    if len(secs) != 6:
        raise SystemExit(f"expected the mod's 6-section dump, found {len(secs)} sections")
    oep_rva = a.oep - IMAGE_BASE if a.oep >= IMAGE_BASE else a.oep
    struct.pack_into("<I", d, opt + 16, oep_rva)
    ndirs = struct.unpack_from("<I", d, opt + 92)[0]
    for i in range(min(ndirs, 16)):
        struct.pack_into("<II", d, opt + 96 + i * 8, 0, 0)
    idata_rva, idata_size = secs[3][0], secs[3][1]
    imp, n = find_import_descriptors(d, idata_rva, idata_size)
    if imp is not None:
        struct.pack_into("<II", d, opt + 96 + 8, imp, (n + 1) * 20)
        names = []
        for i in range(n):
            name = struct.unpack_from("<I", d, imp + i * 20 + 12)[0]
            names.append(cstr(d, name).decode(errors="replace"))
        print(f"import directory: {n} descriptors at RVA {imp:#x}: {', '.join(names)}")
    else:
        print("import directory: no descriptors found in .idata - imports stay unnamed (see ResidentEvil.imports.txt)")
    out = a.out or a.dump.rsplit(".", 1)[0].replace(".dumped", "") + ".fixed.exe"
    open(out, "wb").write(d)
    print(f"wrote {out}: entry point RVA {oep_rva:#x} (VA {IMAGE_BASE + oep_rva:#x}); sections:")
    for va, vsz, rptr, rsz in secs:
        print(f"  va={IMAGE_BASE + va:#x} vsize={vsz:#x} raw={rptr:#x} rsize={rsz:#x}")


if __name__ == "__main__":
    main()
