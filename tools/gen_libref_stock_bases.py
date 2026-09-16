#!/usr/bin/env python3
"""gen_libref_stock_bases.py -- the IMPORTING HOST's view of the recording's address space.

WHAT THIS IS FOR, and why it is not a hole in LIB-REF-SPLIT's zero-VA ruling.

A world blob carries bound regions byte for byte, pointers included -- and a pointer is the one kind
of content that is wrong in another process however faithfully it is copied. The blob's own header
counts the ones it can see (`region_head_ptrs`), but that census only counts dwords whose value
EQUALS some carried region's live base; a pointer INTO a region, at an interior offset, is invisible
to it. `tools/data/world_snapshot_dispositions.json` has always named the class and named the owner:
the link-time-baked pointers into bound regions "are wrong the moment a host binds the pointee
anywhere but its stock .bss ... obligations on the importing host, not gaps."

Re-stamping such a pointer needs one fact the importer does not otherwise have: where the RECORDING
process kept the pointee. That is a relocation reader's input, and this file is how the host gets it.

SCOPE, STATED PRECISELY (conductor ruling, 2026-09-11). LIB-REF-SPLIT S5 -- `MH_STOCK_BASE(va)` is
`0u` under `MH_LIBMH_BUILD`, "what standalone code may do with a zero base: nothing" -- protects the
LIBMH ARTIFACT: the static library must carry no original-image address, which is what makes it
portable, and which `tools/scan_libmh_vas.py` enforces over `libmh.lib`. **That guarantee is
untouched by this file and its scope is unchanged.** This table is compiled into the HOST EXE only,
never into libmh, and the enforcement is structural rather than a convention:

  * the header refuses to compile unless `MH_LIBREF_HOST_TU` is defined, which only libref_host's own
    translation units do; and
  * `tools/lint_libmh_layering.py` fails if any file under `src/mh_dll/mh/` so much as names it.

An importer that knows the recording's layout is doing a reader's job. A module that knows it is a
module that cannot be relocated -- and nothing here lets one.

WHAT IT EMITS
  * `LIBREF_STOCK_BASE[]` -- one entry per region id, the ORIGINAL-IMAGE base the registry records,
    or 0 for a region that has none. Parsed out of `mh/addr/mh_regions.gen.h`, which is itself
    generated from the Ghidra DB -- so this is a second projection of one source, never a second
    derivation of the same fact.
  * `LIBREF_RDATA_CONSTANTS[]` -- the handful of .rdata string constants that carried pointer tables
    point AT rather than into. A .rdata address is not a bound region, so no arithmetic can retarget
    it; the host needs the CONTENT. The bytes are read out of the frozen EN image at generation time
    and baked in here, so the build needs no game binary and so the host formats the REAL string
    rather than an invented empty one -- if that content ever reaches hashed state, we are faithful
    by construction instead of by luck.

Usage:
    python tools/gen_libref_stock_bases.py            # regenerate
    python tools/gen_libref_stock_bases.py --check    # drift gate: fail if the file is stale
"""

from __future__ import annotations

import argparse
import io
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGIONS_H = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_regions.gen.h")
OUT_H = os.path.join(REPO, "src", "mh_dll", "libref_host", "host_stock_bases.gen.h")

# The .rdata constants carried pointer tables point AT. One entry per distinct address; the comment
# is what the table that points there is for, so a reader does not have to rediscover it.
RDATA_CONSTANTS = [
    (
        0x0050108F,
        "G_TEXT_PTRS' shared EMPTY-STRING default -- 85 of the 806 entries hold this address "
        "instead of an offset into G_TEXT_BLOCK. NOT llm_strat_empty_name_str, which is a "
        "different constant at 0x0050109D (char[1] \"\"); this one is the WIDE empty string the "
        "text pool leaves in unassigned slots.",
    ),
]
RDATA_MAX_WCHARS = 64  # a sanity bound; a "constant" longer than this is a finding, not a string


def parse_regions():
    """-> (rid_count, [(rid, name, stock_base)]) from the generated registry header.

    KEYED ON POSITION, NOT ON THE DISPLAY NAME. `REGIONS[]` is declared `region REGIONS[RID_COUNT]`
    and dense, so row i IS rid i -- the header's own static_asserts (`REGIONS[RID_X].base == ...`)
    are what make that a guarantee rather than a convention. A first draft matched the row's display
    string against the `RID_` enumerator and died on `_G_LLM_STRAT_DEPLOY_FORMATION_STENCIL`: the
    display name keeps the `_G_LLM_` prefix that the enumerator drops, for some rows and not others.
    Position has no such failure mode, and the row count is cross-checked against RID_COUNT below.
    """
    txt = io.open(REGIONS_H, encoding="utf-8", errors="replace").read()
    name_by_rid = {int(m.group(2)): m.group(1) for m in re.finditer(r"RID_(\w+)\s*=\s*(\d+)", txt)}
    body = txt.split("REGIONS[RID_COUNT] = {", 1)
    if len(body) != 2:
        sys.exit("gen_libref_stock_bases: could not find the REGIONS[RID_COUNT] table")
    rows = re.findall(r'\{"([^"]+)",\s*MH_STOCK_BASE\(0x([0-9a-fA-F]+)u\)', body[1])
    out = [(i, name_by_rid.get(i, n), int(b, 16)) for i, (n, b) in enumerate(rows)]
    count = len(out)
    declared = re.search(r"RID_COUNT\s*=\s*(\d+)", txt)
    if declared and int(declared.group(1)) != count:
        sys.exit(
            "gen_libref_stock_bases: parsed %d REGIONS rows but RID_COUNT is %s -- the position "
            "keying this generator depends on is broken, which is a finding, not a row to pad"
            % (count, declared.group(1))
        )
    return count, out


def pe_reader(path):
    """-> a VA -> bytes reader over a PE on disk. Section-mapped, not a flat offset."""
    exe = open(path, "rb").read()
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    if exe[pe : pe + 4] != b"PE\0\0":
        sys.exit("gen_libref_stock_bases: %s is not a PE" % path)
    nsec = struct.unpack_from("<H", exe, pe + 6)[0]
    optsz = struct.unpack_from("<H", exe, pe + 20)[0]
    imgbase = struct.unpack_from("<I", exe, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        vsz, va, rawsz, rawptr = struct.unpack_from("<IIII", exe, o + 8)
        secs.append((va, max(vsz, rawsz), rawptr, rawsz))

    def read(va, n):
        r = va - imgbase
        for sva, vsz, rp, rs in secs:
            if sva <= r < sva + vsz:
                off = rp + (r - sva)
                return exe[off : off + n]
        return None

    return read


def wide_string_at(read, va):
    """-> (list of UTF-16 code units WITHOUT the terminator, raw bytes incl. terminator)."""
    raw = read(va, (RDATA_MAX_WCHARS + 1) * 2)
    if raw is None:
        sys.exit("gen_libref_stock_bases: VA %08X is not mapped by any section" % va)
    units = []
    for i in range(RDATA_MAX_WCHARS):
        (u,) = struct.unpack_from("<H", raw, i * 2)
        if u == 0:
            return units, raw[: (i + 1) * 2]
        units.append(u)
    sys.exit(
        "gen_libref_stock_bases: the constant at %08X is not NUL-terminated within %d wchars -- "
        "that is a finding about the address, not a string to truncate" % (va, RDATA_MAX_WCHARS)
    )


def render(count, rows, consts):
    L = []
    a = L.append
    a("//")
    a("// host_stock_bases.gen.h -- GENERATED by tools/gen_libref_stock_bases.py. DO NOT EDIT.")
    a("//")
    a(
        "// The importing host's view of the RECORDING process's address space: where mh.exe kept each"
    )
    a("// bound region, so a carried pointer INTO one can be re-stamped onto this host's arena")
    a("// binding. See the generator's banner for the full rationale.")
    a("//")
    a("// THE LIBMH ZERO-VA GUARANTEE IS UNTOUCHED AND ITS SCOPE IS UNCHANGED. LIB-REF-SPLIT S5")
    a(
        "// protects the libmh ARTIFACT -- `scan_libmh_vas.py` over `libmh.lib` -- because a module that"
    )
    a(
        "// knows an original address cannot be relocated. This table is compiled into the HOST EXE and"
    )
    a(
        "// never into libmh: the #error below refuses any translation unit that is not libref_host's,"
    )
    a("// and lint_libmh_layering.py fails if any file under src/mh_dll/mh/ so much as names this")
    a("// header. An importer knowing the recording's layout is a relocation reader's job.")
    a("//")
    a("#pragma once")
    a("")
    a("#ifndef MH_LIBREF_HOST_TU")
    a(
        '#error "host_stock_bases.gen.h is HOST-ONLY: it carries original-image addresses, which the '
        'libmh artifact must not. Only libref_host translation units may include it (they define '
        'MH_LIBREF_HOST_TU). See LIB-REF-SPLIT S5 and the generator banner."'
    )
    a("#endif")
    a("")
    a("#include <cstdint>")
    a("")
    a("namespace libref {")
    a("")
    a("// One entry per mh::state::region_id. 0 == the registry records no original-image base for")
    a("// that region, and a re-stamp keyed on one MUST refuse rather than treat 0 as an answer.")
    a("inline constexpr uint32_t STOCK_BASE[%d] = {" % count)
    by_rid = {r: (n, b) for r, n, b in rows}
    for rid in range(count):
        n, b = by_rid.get(rid, ("<none>", 0))
        a("    0x%08xu, // %4d %s" % (b, rid, n))
    a("};")
    a("")
    a("// .rdata string constants that carried pointer tables point AT rather than into. A .rdata")
    a("// address is not a bound region, so arithmetic cannot retarget it -- the host needs the")
    a(
        "// CONTENT, and these are the REAL bytes, read out of the frozen EN image at generation time."
    )
    a("struct rdata_constant {")
    a("    uint32_t       va;    // the original-image address the carried pointer holds")
    a("    const wchar_t *text;  // the bytes that lived there, verbatim")
    a("    const char    *why;")
    a("};")
    a("")
    a("inline constexpr rdata_constant RDATA_CONSTANTS[] = {")
    for va, why, units, raw in consts:
        lit = "".join("\\x%04x" % u for u in units)
        a("    {0x%08xu," % va)
        a('     L"%s",' % lit)
        a('     "%s"},' % why.replace('"', '\\"'))
        a(
            "    // raw bytes at %08X, incl. terminator: %s%s"
            % (va, raw.hex(), "   (an EMPTY wide string)" if not units else "")
        )
    a("};")
    a("inline constexpr int RDATA_CONSTANT_COUNT =")
    a("    (int)(sizeof(RDATA_CONSTANTS) / sizeof(RDATA_CONSTANTS[0]));")
    a("")
    a("} // namespace libref")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the generated header is stale")
    ap.add_argument(
        "--exe", default=None, help="the frozen EN image (default: machine POLYGON/mh.exe)"
    )
    args = ap.parse_args()

    count, rows = parse_regions()

    exe = args.exe
    if exe is None:
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import machine_config as machine

        exe = os.path.join(machine.POLYGON, "mh.exe")
    consts = []
    if os.path.isfile(exe):
        read = pe_reader(exe)
        for va, why in RDATA_CONSTANTS:
            units, raw = wide_string_at(read, va)
            consts.append((va, why, units, raw))
    elif args.check:
        # No image on this machine: the drift gate can still compare the REGION table, which is the
        # half that moves. Read the constants back out of the committed header rather than refusing
        # -- a lint that needs a game binary is a lint that does not run on a fresh clone.
        old = io.open(OUT_H, encoding="utf-8").read() if os.path.isfile(OUT_H) else ""
        for va, why in RDATA_CONSTANTS:
            m = re.search(r"raw bytes at %08X, incl\. terminator: ([0-9a-f]*)" % va, old)
            if not m:
                sys.exit(
                    "gen_libref_stock_bases --check: no image at %s and the committed header "
                    "carries no recorded bytes for %08X" % (exe, va)
                )
            raw = bytes.fromhex(m.group(1))
            units = [struct.unpack_from("<H", raw, i * 2)[0] for i in range(len(raw) // 2 - 1)]
            consts.append((va, why, units, raw))
    else:
        sys.exit("gen_libref_stock_bases: no image at %s (pass --exe)" % exe)

    text = render(count, rows, consts)
    if args.check:
        old = io.open(OUT_H, encoding="utf-8").read() if os.path.isfile(OUT_H) else ""
        if old != text:
            print(
                "[gen_libref_stock_bases] STALE: %s does not match the registry -- rerun without "
                "--check" % os.path.relpath(OUT_H, REPO)
            )
            return 1
        print(
            "[gen_libref_stock_bases] OK: %d region base(s), %d .rdata constant(s), header current"
            % (sum(1 for _, _, b in rows if b), len(consts))
        )
        return 0
    io.open(OUT_H, "w", encoding="utf-8", newline="\n").write(text)
    print(
        "wrote %s -- %d rid slot(s), %d with a stock base, %d .rdata constant(s)"
        % (os.path.relpath(OUT_H, REPO), count, sum(1 for _, _, b in rows if b), len(consts))
    )
    for va, _, units, raw in consts:
        print(
            "  %08X -> %d wchar(s) %r  raw %s"
            % (va, len(units), "".join(chr(u) for u in units), raw.hex())
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
