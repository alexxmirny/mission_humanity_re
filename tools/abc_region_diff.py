#!/usr/bin/env python3
"""abc_region_diff.py -- name the BYTES two UI-REC arms disagree about (tooling TL-GATE-D25FX).

WHAT IT ANSWERS. The A/B/C verdict says `state` differed from step N; the `R` columns say which
REGION; neither says which bytes. This reads two arm run directories -- the ship and original arms
of `--ui-abc`, run with a shared rdump window (`--ui-abc-ini` carrying
`[harness] rdump_rid=<hash index>;rdump_lo=<step>;rdump_hi=<step>`) -- and produces:

  (1) the per-region column diff over EVERY common step, from the `R` lines (which regions ever
      disagree, on how many steps, first at which step);
  (2) the byte-level diff of the dumped region at every dumped step, from the `RX` rows, as
      contiguous [offset, len) ranges with both arms' values;
  (3) when an excusal file is given, the verdict: every differing byte of every differing region
      lies inside a DECLARED excused range (PROVED), or the strays are listed and the exit is 1.

It is the evidence half of tools/data/abc_excusals.json: an excusal names bytes, and this is what
shows the arms disagree about those bytes and nothing else. Its output is what gets committed as
tools/data/abc_excusal_evidence.json (`--write-evidence`), so the claim "only resource_spent
differs" is a file with the measurement in it rather than a sentence in a tracker row.

Usage:
    python tools/abc_region_diff.py <arm A run dir> <arm B run dir> [--label-a ship --label-b original]
        [--excusals tools/data/abc_excusals.json] [--write-evidence tools/data/abc_excusal_evidence.json]
        [--note "..."]

Exit 0 = every differing byte is declared (or no excusal file was given); 1 = a stray byte or a
region the excusals do not name differs; 2 = the inputs cannot be read.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mp_analyze as _m  # noqa: E402

EXCUSALS = os.path.join(REPO, "tools", "data", "abc_excusals.json")


def region_index():
    """name -> hash-manifest column, from the analyzer's generated list (positional, by contract)."""
    return {nm: i for i, nm in enumerate(_m.REGION_NAMES)}


def column_diff(a, b):
    """Per-region column diff over the common steps of two parse_harness() segments."""
    common = sorted(set(a["regions"]) & set(b["regions"]))
    out = {}
    for s in common:
        ra, rb = a["regions"][s], b["regions"][s]
        for i, (x, y) in enumerate(zip(ra, rb)):
            if x != y:
                d = out.setdefault(i, {"mismatches": 0, "first": s})
                d["mismatches"] += 1
    rows = []
    for i in sorted(out):
        rows.append(
            {
                "column": i,
                "region": _m.REGION_NAMES[i] if i < len(_m.REGION_NAMES) else "col%d" % i,
                "compared": len(common),
                "mismatches": out[i]["mismatches"],
                "first": out[i]["first"],
            }
        )
    return len(common), rows


def read_rx(harness_log):
    """{(step, rid): bytearray} from the `RX <step> <rid> <off> <hex>` rows (harness.cpp rdump_emit)."""
    out = {}
    with open(harness_log, encoding="utf-8", errors="replace") as f:
        for ln in f:
            if not ln.startswith("RX "):
                continue
            p = ln.split()
            if len(p) < 5:
                continue
            step, rid, off = int(p[1]), int(p[2]), int(p[3])
            buf = out.setdefault((step, rid), bytearray())
            chunk = bytes.fromhex(p[4])
            if len(buf) < off + len(chunk):
                buf.extend(b"\0" * (off + len(chunk) - len(buf)))
            buf[off : off + len(chunk)] = chunk
    return out


def byte_ranges(x, y):
    """Contiguous [offset, len) ranges where two equal-length buffers differ."""
    ranges, start = [], None
    n = min(len(x), len(y))
    for i in range(n):
        if x[i] != y[i]:
            if start is None:
                start = i
        elif start is not None:
            ranges.append((start, i - start))
            start = None
    if start is not None:
        ranges.append((start, n - start))
    return ranges


def as_ints(buf, off, ln):
    """The dwords a range TOUCHES (aligned outward), as signed int32 -- the readable form of a
    counter whose low half moved. Returns (aligned offset, [ints])."""
    lo = off - (off % 4)
    hi = off + ln
    hi += (-hi) % 4
    hi = min(hi, len(buf) - (len(buf) % 4))
    return lo, [int.from_bytes(buf[o : o + 4], "little", signed=True) for o in range(lo, hi, 4)]


def load_excusals(path):
    d = json.load(open(path, encoding="utf-8"))
    return d.get("excusals", [])


def excused_ranges(excusals, region, leg="A-vs-B"):
    out = []
    for e in excusals:
        if leg not in (e.get("legs") or []) or e.get("region") != region:
            continue
        for b in e.get("bytes", []):
            out.append((int(b["offset"]), int(b["len"]), e.get("id", "?"), b.get("field", "")))
    return out


def inside(off, ln, ranges):
    return any(off >= o and off + ln <= o + n for o, n, _i, _f in ranges)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("arm_a")
    ap.add_argument("arm_b")
    ap.add_argument("--label-a", default="ship")
    ap.add_argument("--label-b", default="original")
    ap.add_argument(
        "--excusals", default=None, help="tools/data/abc_excusals.json -- the verdict arm"
    )
    ap.add_argument("--write-evidence", default=None, help="write the measurement as JSON here")
    ap.add_argument("--note", default="", help="free text carried into the evidence file")
    args = ap.parse_args()

    logs = {}
    for lab, d in ((args.label_a, args.arm_a), (args.label_b, args.arm_b)):
        hl = d if os.path.isfile(d) else os.path.join(d, "mh_harness.log")
        if not os.path.isfile(hl):
            print("FAIL: no harness log at %s" % hl)
            return 2
        logs[lab] = hl
    seg = {lab: _m.parse_harness(hl) for lab, hl in logs.items()}
    rx = {lab: read_rx(hl) for lab, hl in logs.items()}
    A, B = args.label_a, args.label_b

    n_common, cols = column_diff(seg[A], seg[B])
    print("per-region column diff (%s vs %s): %d common step(s)" % (A, B, n_common))
    if not n_common:
        print("FAIL: no common steps -- the arms compared nothing")
        return 2
    for r in cols:
        print(
            "  col %-3d %-20s mismatches %-6d first %d"
            % (r["column"], r["region"], r["mismatches"], r["first"])
        )
    if not cols:
        print("  (no region column ever differs)")

    idx = region_index()
    steps_dumped = sorted(set(rx[A]) & set(rx[B]))
    print(
        "rdump rows: %s %d step/region pair(s), %s %d, common %d"
        % (A, len(rx[A]), B, len(rx[B]), len(steps_dumped))
    )
    excusals = load_excusals(args.excusals) if args.excusals else None
    byte_rows, strays, ok = [], [], True
    for step, rid in steps_dumped:
        name = _m.REGION_NAMES[rid] if 0 <= rid < len(_m.REGION_NAMES) else "col%d" % rid
        x, y = rx[A][(step, rid)], rx[B][(step, rid)]
        if len(x) != len(y):
            print("  step %d %s: dump lengths differ (%d vs %d)" % (step, name, len(x), len(y)))
            ok = False
        ranges = byte_ranges(x, y)
        exc = excused_ranges(excusals, name) if excusals is not None else []
        row = {"step": step, "region": name, "column": rid, "bytes": len(x), "differs": []}
        for off, ln in ranges:
            ent = {
                "offset": off,
                "len": ln,
                A: x[off : off + ln].hex().upper(),
                B: y[off : off + ln].hex().upper(),
            }
            (lo, ia), (_lo, ib) = as_ints(x, off, ln), as_ints(y, off, ln)
            if ia:
                ent["dword_at"], ent[A + "_int32"], ent[B + "_int32"] = lo, ia, ib
            if excusals is not None:
                ent["excused"] = inside(off, ln, exc)
                if not ent["excused"]:
                    strays.append((step, name, off, ln))
            row["differs"].append(ent)
        byte_rows.append(row)
        desc = ", ".join("+0x%x..+0x%x" % (o, o + n) for o, n in ranges) or "identical"
        print("  step %-6d %-14s %6d B dumped: differing %s" % (step, name, len(x), desc))
    # The column diff must be explained too: a region that differs in the R columns but was never
    # dumped is a region this run PROVES NOTHING about -- and that is reported, not assumed.
    verdict = None
    if excusals is not None:
        named = {e.get("region") for e in excusals if "A-vs-B" in (e.get("legs") or [])}
        dumped = {r["region"] for r in byte_rows}
        unexplained = [r["region"] for r in cols if r["region"] not in named]
        undumped = [r["region"] for r in cols if r["region"] not in dumped]
        if unexplained:
            ok = False
            print("FAIL: region(s) differ that NO excusal names: %s" % ", ".join(unexplained))
        if undumped:
            ok = False
            print(
                "FAIL: region(s) differ in the R columns but carry no rdump in this run, so their "
                "bytes are unmeasured: %s" % ", ".join(undumped)
            )
        if strays:
            ok = False
            print("FAIL: %d differing byte range(s) lie OUTSIDE every excused range:" % len(strays))
            for step, name, off, ln in strays[:20]:
                print("      step %d %s +0x%x len %d" % (step, name, off, ln))
        if not steps_dumped:
            ok = False
            print("FAIL: no common rdump step -- nothing byte-level was measured")
        verdict = "PROVED" if ok else "REFUTED"
        print(
            "verdict: %s -- %s"
            % (
                verdict,
                "every differing byte at every dumped step is inside a declared excused range, and "
                "every differing region is named by an excusal"
                if ok
                else "see the FAIL lines above",
            )
        )

    if args.write_evidence:
        ev = {
            "_what": "MEASURED evidence for tools/data/abc_excusals.json (tools/abc_region_diff.py, "
            "tooling TL-GATE-D25FX): the per-region column diff of two --ui-abc arms over every "
            "common step, and the byte-level diff of the rdumped region at each dumped step. "
            "Regenerate with the command in `command`; do not hand-edit.",
            "generated": datetime.date.today().isoformat(),
            "command": "python tools/abc_region_diff.py <%s run> <%s run> --excusals %s --write-evidence %s"
            % (
                A,
                B,
                os.path.relpath(args.excusals or EXCUSALS, REPO).replace("\\", "/"),
                os.path.relpath(args.write_evidence, REPO).replace("\\", "/"),
            ),
            "arms": {
                A: os.path.basename(os.path.dirname(logs[A])),
                B: os.path.basename(os.path.dirname(logs[B])),
            },
            "note": args.note,
            "common_steps": n_common,
            "region_columns_differing": cols,
            "rdump": byte_rows,
            "verdict": verdict,
        }
        # APPEND rather than replace when the file already holds runs of the same arms pair: two
        # windows (an early one at the first mismatch, a late one at the journal's end) are more
        # evidence than one, and each keeps its own command line.
        runs = []
        if os.path.isfile(args.write_evidence):
            try:
                prev = json.load(open(args.write_evidence, encoding="utf-8"))
                runs = prev.get("runs", [])
            except (OSError, ValueError):
                runs = []
        runs.append(ev)
        with open(args.write_evidence, "w", encoding="utf-8", newline="\n") as f:
            json.dump(
                {
                    "_what": ev["_what"],
                    "_read": "one entry per measured arms pair under `runs`; the newest is last. "
                    "`region_columns_differing` is the whole-run R-column diff, `rdump` the "
                    "byte ranges at the dumped steps, `verdict` PROVED when every one of them is "
                    "inside a declared excused range.",
                    "runs": runs,
                },
                f,
                indent=1,
            )
            f.write("\n")
        print("wrote %s (%d run(s))" % (args.write_evidence, len(runs)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
