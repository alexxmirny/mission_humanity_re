#!/usr/bin/env python3
"""check_save_block_slices.py -- every save block must be servable under a RELOCATED bind.

THE PROPERTY. A save block is a contiguous (address, size) window and the driver serves it with one
`translate()`. That is correct exactly when one region OWNS every byte of the window. Ten blocks run
past the symbol they start at (docs/save-format.md "Blocks do not align to symbols"), and for four of
them the overrun lands in OTHER LIVE REGIONS rather than in gap bytes.

WHY THAT IS SILENT RATHER THAN LOUD, which is the whole reason this gate exists. `covering()` resolves
a block against `reach`, and `reach` was widened so precisely these blocks WOULD resolve (D6.5). So a
block spanning four regions still gets a single valid pointer -- to the region it STARTED in -- and
under a relocating host it reads the other three out of the copies they abandoned. Nothing returns
null, nothing fails, and the determinism hash cannot see it, because the damage is in bytes only the
save format reads.

THE RULE. Decompose every block against the MEASURED region sizes -- never against `reach`, which is
the number that caused this. Then each block must be either:

  * ONE region run (with or without a trailing gap): the existing single-`translate()` path is
    correct, because the gap lies inside the host's `reach` and the host bind copies `reach` bytes,
    so the gap travels with the region; or
  * TWO OR MORE region runs: it must appear in `SLICED_BLOCKS` in the generated save table, and the
    driver gathers/scatters it run by run.

Anything else is an UNHANDLED OVERRUN, and the count of those is reported and must be zero.

    python tools/check_save_block_slices.py            # the gate (lint_repo)
    python tools/check_save_block_slices.py --report    # every block, its runs, and its verdict
    python tools/check_save_block_slices.py --selftest  # prove it can go red
"""

import argparse
import bisect
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(REPO, "tools", "data", "save_block_table.json")
REGISTRY = os.path.join(REPO, "tools", "data", "state_regions.json")
HEADER = os.path.join(REPO, "src", "mh_dll", "libmh", "save", "save_table.gen.h")


def regions():
    """(base, size, id) for every SIZED registry region, sorted by base."""
    regs = json.loads(open(REGISTRY, encoding="utf-8-sig").read())["regions"]
    return sorted((r["base"], r["size"], r["id"]) for r in regs if r["size"])


def blocks():
    """Every distinct (addr, size) block in the extracted table, with the programs that use it."""
    tbl = json.loads(open(TABLE, encoding="utf-8-sig").read())["tables"]
    out = {}
    for fn, rows in tbl.items():
        for b in rows:
            if not b.get("addr") or not b.get("size"):
                continue
            out.setdefault((int(b["addr"], 16), b["size"]), set()).add(fn)
    return out


def decompose(addr, size, regs):
    """[(id or None, off, stock, len)] tiling [addr, addr+size) exactly."""
    starts = [b for b, _s, _i in regs]
    out, cur, end = [], addr, addr + size
    while cur < end:
        i = bisect.bisect_right(starts, cur) - 1
        if i >= 0 and cur < regs[i][0] + regs[i][1]:
            base, sz, rid = regs[i]
            stop = min(end, base + sz)
            out.append((rid, cur - base, cur, stop - cur))
        else:
            j = bisect.bisect_right(starts, cur)
            stop = min(end, starts[j]) if j < len(starts) else end
            out.append((None, 0, cur, stop - cur))
        cur = stop
    return out


def sliced_in_header():
    """{(addr, size): run_count} out of the COMMITTED generated header.

    Read from the artefact the DLL compiles against rather than re-derived, so this gate answers
    "is the thing that ships correct", not "would a fresh generation be correct".
    """
    src = open(HEADER, encoding="utf-8").read()
    m = re.search(r"inline constexpr sliced_block SLICED_BLOCKS\[\] = \{(.*?)\n\};", src, re.S)
    if not m:
        return {}
    out = {}
    for row in re.finditer(r"\{\s*(0x[0-9a-f]+)u,\s*(\d+),\s*(\d+),\s*(\d+),", m.group(1)):
        out[(int(row.group(1), 16), int(row.group(2)))] = int(row.group(4))
    return out


def audit(regs=None, blks=None, sliced=None):
    """[(addr, size, n_region_runs, n_gaps, verdict)] -- verdict is 'single'/'sliced'/'UNHANDLED'."""
    regs = regions() if regs is None else regs
    blks = blocks() if blks is None else blks
    sliced = sliced_in_header() if sliced is None else sliced
    rows = []
    for addr, size in sorted(blks):
        parts = decompose(addr, size, regs)
        nreg = sum(1 for p in parts if p[0])
        ngap = len(parts) - nreg
        if nreg >= 2:
            want = len(parts)
            got = sliced.get((addr, size))
            verdict = "sliced" if got == want else "UNHANDLED"
        else:
            verdict = "single"
        rows.append((addr, size, nreg, ngap, verdict))
    return rows


def report():
    rows = audit()
    print("save blocks, decomposed against MEASURED region sizes (SB-HOSTFREE):")
    for addr, size, nreg, ngap, verdict in rows:
        if verdict == "single" and nreg == 1 and ngap == 0:
            continue  # the ordinary case: one region, exactly
        print(
            "  0x%08x +%-9d %2d region run(s) + %d gap(s)   %s" % (addr, size, nreg, ngap, verdict)
        )
    n_s = sum(1 for r in rows if r[4] == "sliced")
    n_u = sum(1 for r in rows if r[4] == "UNHANDLED")
    print(
        "  %d block(s); %d need slicing (%d served, %d UNHANDLED)"
        % (len(rows), n_s + n_u, n_s, n_u)
    )
    return 0


def selftest():
    fails = 0
    if [r for r in audit() if r[4] == "UNHANDLED"]:
        print("SELFTEST FAIL: the committed tree already has an unhandled overrun")
        fails += 1
    else:
        print("ok: the committed tree has no unhandled overrun")

    regs, blks = regions(), blocks()
    sliced = sliced_in_header()
    if not sliced:
        print("SELFTEST FAIL: no SLICED_BLOCKS parsed out of the header -- the gate would be inert")
        fails += 1
    else:
        print("ok: %d sliced block(s) parsed out of the committed header" % len(sliced))

    # THE ARM WITH TEETH: drop one block's decomposition and the gate must refuse. Driven on the
    # parsed header rather than by editing it, so the selftest leaves nothing behind.
    if sliced:
        k = sorted(sliced)[0]
        short = {a: b for a, b in sliced.items() if a != k}
        if not [r for r in audit(regs, blks, short) if r[4] == "UNHANDLED"]:
            print("SELFTEST FAIL: removing 0x%08x's decomposition did NOT trip the gate" % k[0])
            fails += 1
        else:
            print("ok: a block that needs slicing and is not in the header is refused")
        # ...and a WRONG run count is refused too, not just an absent entry: a decomposition with
        # the wrong number of runs would tile the block incorrectly and is the quieter error.
        wrong = dict(sliced)
        wrong[k] = wrong[k] + 1
        if not [r for r in audit(regs, blks, wrong) if r[4] == "UNHANDLED"]:
            print("SELFTEST FAIL: a wrong run count did NOT trip the gate")
            fails += 1
        else:
            print("ok: a decomposition with the wrong run count is refused")

    # A NEW overrunning block must be refused. Synthesised from a real region pair so the fixture
    # cannot become unreachable if the registry shifts.
    b0, s0, _i0 = regs[0]
    fake = dict(blks)
    fake[(b0, (regs[1][0] - b0) + regs[1][1])] = {"fixture"}
    rows = audit(regs, fake, sliced)
    if not [r for r in rows if r[4] == "UNHANDLED"]:
        print("SELFTEST FAIL: a NEW block spanning two regions was not refused")
        fails += 1
    else:
        print("ok: a new block spanning two regions is refused")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        print("=== check_save_block_slices --selftest ===")
        n = selftest()
        print("%d failure(s)" % n)
        return 1 if n else 0
    if a.report:
        return report()

    rows = audit()
    bad = [r for r in rows if r[4] == "UNHANDLED"]
    if bad:
        print("=== check_save_block_slices: %d UNHANDLED overrun(s) ===" % len(bad))
        for addr, size, nreg, ngap, _v in bad:
            print(
                "  0x%08x +%-9d spans %d regions (+%d gap runs) and is not in SLICED_BLOCKS"
                % (addr, size, nreg, ngap)
            )
        print(
            "  Under a relocating host this block reads its trailing regions from the copies they\n"
            "  abandoned -- silently, because covering() resolves it against `reach`. Re-run\n"
            "  tools/gen_save_table_header.py so the decomposition is emitted, and commit."
        )
        return 1
    n_s = sum(1 for r in rows if r[4] == "sliced")
    print(
        "check_save_block_slices: ok -- %d block(s), %d span more than one region and all %d are "
        "served run by run; 0 unhandled overruns" % (len(rows), n_s, n_s)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
