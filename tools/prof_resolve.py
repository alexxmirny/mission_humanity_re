"""Turn the sampling profiler's raw RVAs into names.

The profiler in harness.cpp emits module-relative addresses because that is all it can know at
runtime -- resolving in-process would mean shipping a symbol reader inside the injected DLL. The two
modules resolve through completely different sources, which is the whole reason this script exists:

  mh.dll  -> the linker MAP (src/mh_dll/Release/mh.map, GenerateMapFile in mh.vcxproj). A PDB cannot
             be read without WinDbg/DIA or llvm-symbolizer, none of which is on this machine; a map
             is a text file.
  mh.exe  -> docs/symbols.md, this project's own Ghidra export. The retail binary is 32-bit Watcom
             with no PDB, so no Microsoft tool could name these ANYWAY -- an ETL profile opened in
             WPA would show the same bare addresses. That is why a local sampler plus this script
             beats the "proper" toolchain here rather than merely substituting for it.

An mh.exe RVA is its Ghidra VA directly (image base 0x400000 is already folded into the VA space the
symbol table uses), so the lookup is a range search over the exported function list.

Usage:  python tools/prof_resolve.py <run-dir-or-log>
"""

import bisect
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(REPO, "src", "mh_dll", "Release", "mh.map")
SYMS = os.path.join(REPO, "docs", "symbols.md")
EXE_BASE = 0x400000


def load_map():
    """[(rva, name)] sorted, from the linker map's 'Publics by Value' table."""
    if not os.path.isfile(MAP):
        return []
    out, seen = [], False
    with open(MAP, encoding="utf-8", errors="replace") as f:
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


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    if os.path.isdir(path):
        path = os.path.join(path, "mh_harness.log")
    if not os.path.isfile(path):
        print("no such log: %s" % path)
        return 1

    dll_base = None
    dll = load_map()
    exe = load_exe_syms()
    if dll:
        # The map's preferred image base, rounded down from its lowest real code symbol. The DLL is
        # relocated at load time, so this is exactly the offset the profiler's RVAs are missing.
        dll_base = min(k for k, _v in dll) & ~0xFFFF
    print("symbols: %d from mh.map, %d from docs/symbols.md" % (len(dll), len(exe)))
    print("")

    for ln in open(path, encoding="utf-8", errors="replace"):
        if not ln.startswith("; PROF"):
            continue
        t = ln.strip().lstrip("; ")
        m = re.match(r"PROF\s+(mh\.dll|mh\.exe|\?)\s+\+([0-9A-Fa-f]{8})\s+(\d+)\s+(\S+)", t)
        if not m:
            print(t)
            continue
        mod, rva, hits, pct = m.group(1), int(m.group(2), 16), m.group(3), m.group(4)
        name, off = None, 0
        if mod == "mh.dll" and dll:
            # The map's addresses are Rva+Base at the PREFERRED base; the profiler's are offsets
            # from the real one. Rebase by the map's own lowest public.
            name, off = nearest(dll, rva + dll_base)
        elif mod == "mh.exe" and exe:
            name, off = nearest(exe, rva + EXE_BASE)
        where = "%s+0x%x" % (name, off) if name else ""
        # ntdll and friends are left as raw addresses ON PURPOSE: naming them would need symbol
        # servers, and what matters about them here is only that they are NOT our code.
        print("  %-7s +%08X  %6s  %6s  %s" % (mod, rva, hits, pct, where))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
