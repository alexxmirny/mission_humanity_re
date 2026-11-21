"""Turn the sampling profiler's raw RVAs into names.

Runtime records module-relative addresses for the game/mh.dll and, when enabled, the same process's
loaded-module ranges for other samples. Never borrow module bases from another process:

  injected DLLs -> their matching Release linker MAP files.
  mh.exe       -> docs/symbols.md, the Ghidra export for the retail Watcom binary.

The retail executable has no PDB; its RVAs are rebased by 0x400000 before looking up the
English-build Ghidra VA. Use maps matching the sampled DLLs, not a later rebuild.

Usage:  python tools/prof_resolve.py <run-dir-or-log>
        python tools/prof_resolve.py --addrs <file>   (lines: `<module> <rva-hex> <count>`)
"""

import bisect
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(REPO, "src", "mh_dll", "Release", "mh.map")
SYMS = os.path.join(REPO, "docs", "symbols.md")
EXE_BASE = 0x400000
DLL_MAPS = {
    "mh.dll": MAP,
    "mh_harness.dll": os.path.join(os.path.dirname(MAP), "mh_harness.map"),
    "libmh.dll": os.path.join(os.path.dirname(MAP), "libmh.map"),
    "mh_net.dll": os.path.join(os.path.dirname(MAP), "mh_net.map"),
}


def load_map(path=MAP):
    """[(rva, name)] sorted, from the linker map's 'Publics by Value' table."""
    if not os.path.isfile(path):
        return []
    out, seen = [], False
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            if "Publics by Value" in ln:
                seen = True
                continue
            if not seen:
                continue
            # SECTION 0000 IS THE ABSOLUTES -- __tls_array, ___guard_flags, section counts. They
            # carry tiny addresses that are not code, and including them made every lookup resolve
            # to "___guard_flags + a huge offset", which is what a bisect does when the table it is
            # given is mostly noise below the real range.
            m = re.match(r"\s+([0-9a-f]{4}):[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8})\s", ln)
            if m and m.group(1) != "0000" and m.group(3) != "00000000":
                out.append((int(m.group(3), 16), m.group(2)))
    out.sort()
    return out


def load_exe_syms():
    """[(va, name)] sorted, from the generated Ghidra symbol table."""
    if not os.path.isfile(SYMS):
        return []
    out = []
    with open(SYMS, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = re.search(r"\b0x([0-9a-fA-F]{6,8})\b.*?\|\s*`?([A-Za-z_][A-Za-z0-9_:]*)`?", ln)
            if m:
                out.append((int(m.group(1), 16), m.group(2)))
            else:
                m = re.match(r"\|\s*`?([A-Za-z_][A-Za-z0-9_]*)`?\s*\|\s*`?0x([0-9a-fA-F]+)`?", ln)
                if m:
                    out.append((int(m.group(2), 16), m.group(1)))
    out.sort()
    return out


def nearest(table, addr):
    if not table:
        return None, 0
    keys = [k for k, _v in table]
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return None, 0
    return table[i][1], addr - table[i][0]


def resolve_addrs(path):
    """--addrs FILE: resolve `<module> <rva-hex> <count>` lines (one per line, e.g. exported from a WPR
    ETL by an external tool; the RVA is module-relative). Modules without a map are printed raw."""
    tables, bases = {}, {}
    for mod, mp in DLL_MAPS.items():
        t = load_map(mp)
        tables[mod] = t
        bases[mod] = (min(k for k, _v in t) & ~0xFFFF) if t else 0
    exe = load_exe_syms()
    for ln in open(path, encoding="utf-8", errors="replace"):
        f = ln.split()
        if len(f) < 3:
            continue
        mod, rva, hits = f[0].lower(), int(f[1], 16), f[2]
        name, off = None, 0
        if mod in ("mh.focus.exe", "mh.exe"):
            name, off = nearest(exe, rva + EXE_BASE)
        elif tables.get(mod):
            name, off = nearest(tables[mod], rva + bases[mod])
        where = "%s+0x%x" % (name, off) if name else ""
        print("%-14s +%08X  %7s  %s" % (mod, rva, hits, where))
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "--addrs" and len(sys.argv) > 2:
        return resolve_addrs(sys.argv[2])
    path = sys.argv[1]
    if os.path.isdir(path):
        path = os.path.join(path, "mh_harness.log")
    if not os.path.isfile(path):
        print("no such log: %s" % path)
        return 1

    tables = {mod: load_map(mp) for mod, mp in DLL_MAPS.items()}
    bases = {
        mod: (min(k for k, _v in table) & ~0xFFFF) if table else 0 for mod, table in tables.items()
    }
    modules = {}
    exe = load_exe_syms()
    print(
        "symbols: %s, %d from docs/symbols.md"
        % (
            ", ".join("%d from %s" % (len(table), mod) for mod, table in tables.items() if table),
            len(exe),
        )
    )
    print("")

    for ln in open(path, encoding="utf-8", errors="replace"):
        if not ln.startswith("; PROF"):
            continue
        t = ln.strip().lstrip("; ")
        catalog = re.fullmatch(r"PROF MODULE (\S+) ([0-9A-Fa-f]{8}) ([0-9A-Fa-f]{8})", t)
        if catalog:
            modules[catalog.group(1).lower()] = (
                int(catalog.group(2), 16),
                int(catalog.group(3), 16),
            )
            print(t)
            continue
        m = re.match(r"PROF\s+(mh\.dll|mh\.exe|\?)\s+\+([0-9A-Fa-f]{8})\s+(\d+)\s+(\S+)", t)
        if not m:
            print(t)
            continue
        mod, rva, hits, pct = m.group(1), int(m.group(2), 16), m.group(3), m.group(4)
        if mod == "?":
            for name, (lo, hi) in modules.items():
                if lo <= rva < hi:
                    mod, rva = name, rva - lo
                    break
        name, off = None, 0
        if tables.get(mod):
            name, off = nearest(tables[mod], rva + bases[mod])
        elif mod == "mh.exe" and exe:
            name, off = nearest(exe, rva + EXE_BASE)
        where = "%s+0x%x" % (name, off) if name else ""
        # Modules with no matching local map keep their relative address, not an invented symbol.
        print("  %-14s +%08X  %6s  %6s  %s" % (mod, rva, hits, pct, where))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
