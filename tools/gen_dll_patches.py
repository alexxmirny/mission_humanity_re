#!/usr/bin/env python3
"""gen_dll_patches.py -- emit mh/addr/mh_patches.gen.h: the extents of every PROMOTABLE function.

The DLL needs to answer one question at runtime: *is this address inside a function body we have
replaced?* Because a promoted function's entry has been JMP'd away, every byte from its entry to its
end is dead -- so a byte patch aimed anywhere inside it writes bytes that will never execute, while
`patch_bytes_guarded` still returns true and the arming line still says "armed". That is the C1 bug.

Answering it needs function EXTENTS, which only Ghidra knows. This generator joins two committed
inputs so the answer is available with no Ghidra open and no ReVA connection:

  * the set of functions the tree can promote -- every `MH_EXPORT_REPLACE(<fn>, ...)` in src/mh_dll
  * their extents -- tools/data/en_functions.json (the Ghidra-side function dump)

Deriving the promotable set from the SOURCES rather than from a hand-kept list is deliberate: the
interlock has to fire the moment somebody writes a new MH_EXPORT_REPLACE, which is exactly the moment
a previously-safe patch becomes inert.

Usage:
  python tools/gen_dll_patches.py            # write the header
  python tools/gen_dll_patches.py --check    # drift gate (lint_repo.py runs this)
"""

import argparse
import bisect
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUNCS = os.path.join(REPO, "tools", "data", "en_functions.json")
MANIFEST = os.path.join(REPO, "tools", "data", "dll_patch_manifest.json")
DLL = os.path.join(REPO, "src", "mh_dll")
OUT = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_patches.gen.h")

REPLACE_RE = re.compile(r"^\s*MH_EXPORT_REPLACE\(\s*([A-Za-z_]\w*)\s*,", re.M)


def promotable_names():
    """Every game function the tree installs a C++ replacement for, in source order per file."""
    found = []
    for dirpath, _dirs, files in os.walk(DLL):
        if "attic" in dirpath:
            continue
        for fn in sorted(files):
            if not fn.endswith((".cpp", ".c")):
                continue
            path = os.path.join(dirpath, fn)
            src = open(path, encoding="utf-8", errors="replace").read()
            for m in REPLACE_RE.finditer(src):
                rel = os.path.relpath(path, DLL).replace(os.sep, "/")
                found.append((m.group(1), rel))
    return found


def load_extents():
    doc = json.load(open(FUNCS, encoding="utf-8"))
    by_name = {}
    for row in doc["functions"]:
        # A duplicated name would make "the" extent ambiguous; keep both so the caller can complain.
        by_name.setdefault(row["name"], []).append(row)
        # ALSO key by the C IDENTIFIER, because that is what MH_EXPORT_REPLACE is spelled with.
        # `en_functions.json` carries Ghidra's real names, so a C++-namespaced function is
        # `game::SaveGame` there and `game__SaveGame` in the source -- and this generator matched on
        # the source spelling only, so the first namespaced promotion (game::SaveGame, 2026-07-30)
        # failed with "no such function in en_functions.json". The sanitiser is the same rule
        # the Ghidra-side call-proto dump applies (non-identifier characters -> '_'), kept in step by being the
        # same one-liner rather than by being remembered.
        ident = re.sub(r"\W", "_", row["name"])
        if ident != row["name"]:
            by_name.setdefault(ident, []).append(row)
    return by_name


def load_patches(extents):
    """[(id, addr, owner, carrier)] for every statically-addressed registered patch."""
    flat = sorted(
        (int(r["entry"], 16), int(r["end"], 16), r["name"]) for rs in extents.values() for r in rs
    )
    starts = [f[0] for f in flat]

    def owner_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i < 0:
            return ""
        return flat[i][2] if addr <= flat[i][1] else ""

    out = []
    for p in json.load(open(MANIFEST, encoding="utf-8"))["patches"]:
        if p.get("dynamic"):
            continue  # no address list by construction; the runtime check is what covers these
        for a in p.get("addrs", []):
            out.append((p["id"], a, owner_of(int(a, 16)), p.get("carrier")))
    return out


def verified_rows(extents):
    """[(name, entry, end, ledger_domain)] for every migration-ledger row at state `verified`.

    THE PROMOTABLE TABLE IS NOT THE DOMAIN (TACT1-P C4, 2026-09-04). X-TOMB's per-domain force-arm
    walks the promotable table, whose rows are exactly the functions with an MH_EXPORT_REPLACE --
    ELEVEN of tact's 98 verified rows. Sweeping "the domain" over that table therefore arms the
    eleven and reports itself armed, which is the 0.8%-reads-as-the-closure shape TACT1-P was
    reopened for. A coverage clause needs the whole verified set, promoted or not: force-arming an
    UNPROMOTED verified row is a real question -- if our body is the one that runs, the original is
    never entered, and if the original still runs the trap says so BY NAME.

    The domain here is the LEDGER's (the file stem), not the TU path's, because that is the name the
    -P items, the liveness census and `force_arm_domain=` all speak in. The two agree for every
    domain whose TUs live in `mh/<domain>/` and differ for the ledgers that share a directory
    (`sim_resid` rows sit under `libmh/sim/resid/`, so the promotable table calls them "sim"). Both
    tables are swept, so a name is reachable either way; only the COUNT is per-table.
    """
    out = []
    data = os.path.join(REPO, "tools", "data")
    for fn in sorted(os.listdir(data)):
        if not fn.endswith("_migration.json"):
            continue
        doc = json.load(open(os.path.join(data, fn), encoding="utf-8"))
        if not isinstance(doc, dict) or "functions" not in doc:
            continue  # legacy_conv_migration.json is a group roster, not a per-function ledger
        domain = doc.get("_domain") or fn[: -len("_migration.json")]
        for row in doc["functions"]:
            if row.get("state") != "verified":
                continue
            hits = extents.get(row["name"])
            if not hits or len(hits) > 1:
                # Reported, never guessed: a verified row we cannot locate is a hole in the sweep,
                # and a silent hole is what makes an under-armed set read as armed.
                print(
                    "gen_dll_patches: %s verified row %s -- %s in en_functions.json"
                    % (domain, row["name"], "no extent" if not hits else "%d matches" % len(hits)),
                    file=sys.stderr,
                )
                continue
            out.append((row["name"], hits[0]["entry"], hits[0]["end"], domain))
    out.sort(key=lambda r: (r[3], int(r[1], 16)))
    return out


def _domain_of(src: str) -> str:
    """The migration domain a promotable row belongs to, from its owning TU's path.

    `libmh/sim/libtrans/sim_lt_promote.cpp` -> "sim", `libmh/tact/tact_frame.cpp` -> "tact". Mechanical on
    purpose: a new domain directory needs no edit here, and there is no hand list to fall out of date
    the way a name->domain map would. Anything that is not `mh/<dir>/...` reports "" rather than a
    guess -- an unattributed row must read as unattributed, never as somebody's domain.
    """
    parts = src.replace("\\", "/").split("/")
    if len(parts) >= 3 and parts[0] in ("mh", "libmh"):
        return parts[1]
    return ""


def render():
    extents = load_extents()
    patches = load_patches(extents)
    rows, problems = [], []
    seen = set()
    for name, src in promotable_names():
        if name in seen:
            continue
        seen.add(name)
        hits = extents.get(name)
        if not hits:
            problems.append(
                f"{src}: MH_EXPORT_REPLACE({name}) -- no such function in en_functions.json"
            )
            continue
        if len(hits) > 1:
            problems.append(
                f"{src}: MH_EXPORT_REPLACE({name}) -- {len(hits)} functions share that name"
            )
            continue
        rows.append((name, hits[0]["entry"], hits[0]["end"], src))
    if problems:
        for p in problems:
            print("gen_dll_patches: " + p, file=sys.stderr)
        return None
    rows.sort(key=lambda r: int(r[1], 16))

    w = max((len(r[0]) for r in rows), default=1)
    lines = [
        "// GENERATED by tools/gen_dll_patches.py -- DO NOT EDIT.",
        "// Regenerate after adding an MH_EXPORT_REPLACE or re-dumping tools/data/en_functions.json;",
        "// `gen_dll_patches.py --check` is the drift gate (lint_repo.py runs it).",
        "//",
        "// The extents of every function the DLL can PROMOTE (replace with a C++ body). A promoted",
        "// function's entry is JMP'd away, so EVERY byte from entry to end is dead -- which is why a",
        "// byte patch aimed anywhere inside one is inert while still reporting itself armed. This table",
        "// is what lets hook/promoted.cpp answer 'is this target inside a promoted body?' and refuse.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "#include \"hook/promoted.h\"",
        "",
        "namespace mh::addr {",
        "",
        "// Entry..end INCLUSIVE, EN build (/eng/mh.exe), image base 0x00400000, no ASLR.",
        "//",
        "// The fourth column is the MIGRATION DOMAIN, taken mechanically from the owning TU's",
        "// `mh/<domain>/...` path -- never a hand table, so a new domain directory needs no edit here.",
        "// X-TOMB reads it to force-arm and to COUNT per domain: a tombstone report that only totals",
        "// reads the same whether both domains contributed or one contributed nothing, and a domain",
        "// contributing nothing is exactly the coverage hole the instrument exists to find.",
        "inline constexpr mh::hook::owner_range promotable_ranges[] = {",
    ]
    dw = max((len(_domain_of(r[3])) for r in rows), default=1)
    for name, entry, end, src in rows:
        dom = _domain_of(src)
        lines.append(
            f'    {{"{name}",{" " * (w - len(name))} {entry}u, {end}u,'
            f' "{dom}"{" " * (dw - len(dom))}}}, // {src}'
        )
    vrows = verified_rows(extents)
    lines += [
        "};",
        f"inline constexpr int promotable_range_count = {len(rows)};",
        "",
        "// EVERY migration-ledger row at state `verified`, with its LEDGER domain -- the set X-TOMB's",
        "// per-domain force-arm has to sweep. The table above holds only what carries an",
        "// MH_EXPORT_REPLACE (tact: 11 of 98), so a domain sweep over it arms a tenth of the domain and",
        "// reports itself armed; that is what TACT1-P C4 exists to stop. Rows here are NOT claimed dead:",
        "// force-arming one asks whether the ORIGINAL is still entered, and the trap answers by name.",
        "inline constexpr mh::hook::owner_range verified_ranges[] = {",
    ]
    if vrows:
        vw = max(len(r[0]) for r in vrows)
        vdw = max(len(r[3]) for r in vrows)
        for name, entry, end, dom in vrows:
            lines.append(
                f'    {{"{name}",{" " * (vw - len(name))} {entry}u, {end}u,'
                f' "{dom}"{" " * (vdw - len(dom))}}},'
            )
    lines += [
        "};",
        f"inline constexpr int verified_range_count = {len(vrows)};",
        "",
        "// The registered byte patches, from tools/data/dll_patch_manifest.json -- statically-addressed",
        "// ones only (a `dynamic: true` entry has no address list by construction). `carrier` is how the",
        "// fix survives promotion; the arming report reads it so a suppressed patch can name the live",
        "// carrier rather than merely announcing that it suppressed itself.",
        "inline constexpr mh::hook::patch_decl registered_patches[] = {",
    ]
    pw = max((len(p[0]) for p in patches), default=1)
    for pid, addr, owner, carrier in patches:
        c = "nullptr" if carrier is None else f'"{carrier}"'
        lines.append(f'    {{"{pid}",{" " * (pw - len(pid))} {addr}u, "{owner}", {c}}},')
    lines += [
        "};",
        f"inline constexpr int registered_patch_count = {len(patches)};",
        "",
        "} // namespace mh::addr",
        "",
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the header is stale")
    args = ap.parse_args()

    text = render()
    if text is None:
        return 1
    if args.check:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur.replace("\r\n", "\n") != text:
            print("gen_dll_patches: mh_patches.gen.h is STALE -- rerun tools/gen_dll_patches.py")
            return 1
        print(f"gen_dll_patches: up to date ({text.count(chr(10) + '    {')} promotable functions)")
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
