#!/usr/bin/env python3
"""Collect function and global names with their original addresses from the RE1 PC decomp
(https://github.com/ecruells/resident-evil-pc-decomp) into a symbol list for Ghidra.

    python3 tools/decomp_symbols.py --out ghidra_proj/decomp_symbols.txt [--ref main]

Reads the repo's sources over HTTPS (no clone). The decomp keeps the 1997
binary's addresses in comments: `// name (0x00XXXXXX)` heads each function,
other `name (0x00XXXXXX)` mentions name one in passing, and `extern ... name;
// 0x00XXXXXX` or `#define name (...) // 0x00XXXXXX` in a header names a global.
Comments also name addresses *inside* functions, so every mention is a
weighted vote (a header counts five times): each name goes to the address it
is most often given, each address keeps the best-voted name that went there,
and the few entry points the mod leans on are pinned. Output lines:
`<hex VA> <f|d> <name>`; ghidra/ApplyNames.java applies them to the dump.
Pure stdlib.
"""
import argparse, collections, json, re, urllib.request

REPO = "ecruells/resident-evil-pc-decomp"
TREE = "https://api.github.com/repos/" + REPO + "/git/trees/{ref}?recursive=1"
RAW = "https://raw.githubusercontent.com/" + REPO + "/{ref}/{path}"
ADDR = r"(0x00[4-9a-dA-D][0-9a-fA-F]{5})"
# `name (0xADDR)` and `name(...); // ... (0xADDR)` - a function
FUNC = re.compile(r"\b([A-Za-z_]\w{2,})\s*\(" + ADDR + r"\)")
DECL = re.compile(r"\b([A-Za-z_]\w{2,})\s*\([^()]*\)\s*;\s*//[^\n]*?" + ADDR)
# `// name (0xADDR)` opening a comment line - how the decomp heads each function
HEADER = re.compile(r"^[ \t]*//[ \t]*([A-Za-z_]\w{2,})\s*\(" + ADDR + r"\)", re.M)
# `extern TYPE name[...]; // ... 0xADDR` - a global
DATA = re.compile(r"extern\s+[^;\n]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]\n]*\])*\s*;\s*//[^\n]*?" + ADDR)
# `#define name (...)  // ... 0xADDR` - a global reached through a macro
DEFINE = re.compile(r"#define\s+([A-Za-z_]\w*)\s+[^\n]*?//[^\n]*?" + ADDR)
# Entry points the mod leans on, as the decomp's own analysis gives them; a comment
# elsewhere naming an address inside one of them must not move it.
PINNED = {0x441350: "WinMain", 0x428EB0: "main_loop", 0x4973D0: "FrameRateGovernor"}
STOP = {"cpp", "the", "and", "via", "see", "was", "from", "call", "calls", "called", "at", "in", "sub", "FUN"}
CODE_END = 0x4AF000  # the 1997 image's code section ends here; data lies above


def get(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read().decode("utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--ref", default="main")
    a = ap.parse_args()
    paths = [t["path"] for t in json.loads(get(TREE.format(ref=a.ref)))["tree"]
             if t["type"] == "blob" and t["path"].startswith("src/") and t["path"].endswith((".cpp", ".h"))]
    votes = collections.defaultdict(collections.Counter)
    for path in paths:
        text = get(RAW.format(ref=a.ref, path=path))
        # A function's own header comment outweighs a caller's passing mention.
        for rx, kind, weight in ((HEADER, "f", 5), (DECL, "f", 2), (FUNC, "f", 1), (DATA, "d", 1), (DEFINE, "d", 1)):
            for m in rx.finditer(text):
                name, addr = m.group(1), int(m.group(2), 16)
                if name in STOP or name.startswith(("DAT_", "LAB_")):
                    continue
                if (kind == "f") != (addr < CODE_END):
                    continue  # a "function" in data or a "global" in code is a stray match
                votes[addr][(kind, name)] += weight
    # A name belongs at one address - the one it is most often given (comments
    # also name addresses inside a function, "returns to game_loop (0x...)") -
    # and each address keeps the best-voted name that belongs there.
    by_name = collections.defaultdict(collections.Counter)
    for addr, c in votes.items():
        for key, n in c.items():
            by_name[key][addr] += n
    home = {key: c.most_common(1)[0][0] for key, c in by_name.items()}
    for addr, name in PINNED.items():
        home[("f", name)] = addr
        votes[addr][("f", name)] += 1000
    chosen = {}
    for addr, c in votes.items():
        for key, _ in c.most_common():
            if home[key] == addr:
                chosen[addr] = key
                break
    with open(a.out, "w") as f:
        f.write(f"# {REPO}@{a.ref}: <VA> <f=function|d=data> <name>\n")
        nf = nd = 0
        for addr in sorted(chosen):
            kind, name = chosen[addr]
            f.write(f"{addr:08X} {kind} {name}\n")
            nf += kind == "f"
            nd += kind == "d"
    print(f"{a.out}: {nf} functions, {nd} globals from {len(paths)} source files")


if __name__ == "__main__":
    main()
