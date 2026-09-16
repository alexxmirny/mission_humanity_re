#!/usr/bin/env python3
"""scan_interior_ptrs.py -- the INTERIOR-POINTER screen over a committed world blob.

WHAT IT ANSWERS, and why the blob's own header cannot.

A world blob (`tools/data/fixtures/<fixture>/world.bin.zz`) carries every bound region byte for
byte, pointers included -- and a pointer is the one kind of content that is wrong in another
process however faithfully it is copied. The blob header counts the ones it can see:
`region_head_ptrs` is "dwords whose value EQUALS some carried region's live base", which finds the
measured `tile_objects_ptr` / `fow_ptr` class exactly. It is BLIND BY CONSTRUCTION to a dword
pointing INTO a region at an interior offset -- and that is the class the LIB-REF-LIVE `G_TEXT_PTRS`
fault turned out to be (806 carried text pointers, 720 distinct interior offsets into
`G_TEXT_BLOCK`, faulting the first time the live loop formatted a message; see
`src/mh_dll/libref_host/main.cpp` "THE CARRIED-POINTER RE-STAMP" and
`src/mh_dll/libmh/state/spine.cpp`).

This tool is the whole-blob generalisation of that census, so the population is RE-DERIVED rather
than quoted. It reads only committed artifacts:

  * the fixture's `world.bin.zz` (block stream: rid, len, verbatim payload), and
  * `src/mh_dll/mh/addr/mh_regions.gen.h` (the registry: stock base + reach per rid),

so it runs on a fresh clone with no game binary, no Ghidra and no rig.

THE SCREEN RULE, stated exactly. For every carried block, at every offset o = 0, 4, 8, ... (the
same walk `world_snapshot.cpp`'s `emit_block` uses -- from the region base, NOT from absolute
4-alignment, because a region is not guaranteed 4-aligned), read the little-endian dword v. v is
INTERIOR-POINTER-SHAPED if it lands inside [stock_base, stock_base+reach) of some registry region
with a non-zero base and reach. That is the same predicate `libref_host`'s
`stock_region_containing()` applies, evaluated over the whole blob instead of over one table.

A DWORD THAT EQUALS AN ADDRESS IS NOT A POINTER, and this screen cannot tell the difference on its
own -- 300 KB of UTF-16 text aliases a 4-byte window into an 11 MB registry span constantly. So the
tool reports four independent evidence classes beside the raw count, and the adjudication is made on
those, never on the count:

  DENSITY      hits / dwords scanned. A pointer table is mostly pointers.
  DOMINANT     the largest single target region's share of the hits. A table points somewhere
               specific; aliasing scatters across whatever spans the byte pattern happens to reach.
  PHASE LIFT   the same screen re-run at offsets o ≡ 1, 2, 3 (mod 4), reported as hits divided by
               the STRONGEST of those three controls. A real table's pointers are all at
               4-multiples, so every misaligned phase reads zero and the lift is infinite; data that
               merely aliases hits at some other phase too. The strongest control, not the mean, is
               the honest denominator: UTF-16 text is 2-byte aligned, so it aliases at phases 0 and
               2 and reads ~0 at 1 and 3 -- against the MEAN that looks like a 3x "lift" and against
               the max it correctly reads 1.0. This is the load-bearing control, and it is the one
               class a region too small to have a meaningful density cannot fake.
  RUN SHARE    the share of hits whose offset is exactly 4 past the previous hit -- a table is
               contiguous slots, scattered aliasing is not.

plus WIDE-TEXT SHARE, the share of hit dwords that decode as a printable ASCII-in-UTF-16 character
pair (b0 printable, b1 == 0, b2 printable, b3 == 0) -- the specific aliasing mechanism behind the
largest holder, named rather than waved at.

SELF-CHECKS (`--check` exits non-zero if any fails). These are what make the numbers evidence:

  1. the block stream parses to exactly `payload_len` and `block_count` blocks;
  2. the base-equality subtotal this tool computes equals the blob header's own
     `region_head_ptrs` -- an independent instrument reproducing the recording process's live
     census proves the target index is the right one;
  3. the `G_TEXT_PTRS` adjudication `libref_host` actually performs is reproduced entry for entry:
     806 slots, 721 landing in a carried region (720 into `G_TEXT_BLOCK` + 1 into
     `STRAT_SCENARIO_PLANET_NAME_W`), 85 holding the .rdata constant 0x0050108F, 0 zero, 0 left
     over. A screen that cannot reproduce the one adjudicated holder is not measuring the class.

Usage:
    python tools/scan_interior_ptrs.py                    # table over the default fixture
    python tools/scan_interior_ptrs.py --fixture NAME     # another committed fixture
    python tools/scan_interior_ptrs.py --all-fixtures     # totals for every fixture present
    python tools/scan_interior_ptrs.py --holder NAME      # one holder in detail
    python tools/scan_interior_ptrs.py --json PATH        # machine-readable dump
    python tools/scan_interior_ptrs.py --check            # self-checks only; exit 1 on failure
"""

from __future__ import annotations

import argparse
import array
import io
import json
import os
import re
import struct
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGIONS_H = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_regions.gen.h")
FIXTURES = os.path.join(REPO, "tools", "data", "fixtures")

# The fixture whose numbers the tracked item quotes: the LIB-REF-LIVE live-loop fixture,
# the run whose step-2001 fault exposed the class in the first place.
DEFAULT_FIXTURE = "libref-live-aisoak-v1"

MAGIC = b"MHWRLD\x00\x01"
HDR_LEN = 96  # common_header(32) + the world header's own fields(64); see state/world_snapshot.h

# The one .rdata constant the G_TEXT_PTRS adjudication recognises by content (a carried pointer may
# name a .rdata address, which is not a bound region and which no arithmetic can retarget). Kept
# here only so self-check 3 can account for every one of the 806 slots -- the host's copy of the
# BYTES lives in tools/gen_libref_stock_bases.py, which is where that fact belongs.
G_TEXT_PTRS_RDATA_VA = 0x0050108F


# ---- inputs ------------------------------------------------------------------------------------


def parse_regions():
    """-> [(rid, name, stock_base, size, reach)] from the generated registry header.

    KEYED ON POSITION, NOT ON THE DISPLAY NAME -- `REGIONS[]` is declared `region REGIONS[RID_COUNT]`
    and dense, so row i IS rid i, and the row count is cross-checked against RID_COUNT below. The
    display string keeps the `_G_LLM_` prefix that the `RID_` enumerator drops for some rows and not
    others, so matching on it is a trap (gen_libref_stock_bases.py records the same lesson).
    """
    txt = io.open(REGIONS_H, encoding="utf-8", errors="replace").read()
    name_by_rid = {int(m.group(2)): m.group(1) for m in re.finditer(r"RID_(\w+)\s*=\s*(\d+)", txt)}
    body = txt.split("REGIONS[RID_COUNT] = {", 1)
    if len(body) != 2:
        sys.exit(
            "scan_interior_ptrs: could not find the REGIONS[RID_COUNT] table in %s" % REGIONS_H
        )
    rows = re.findall(
        r'\{"([^"]+)",\s*MH_STOCK_BASE\(0x([0-9a-fA-F]+)u\),\s*(\d+)u,\s*(\d+)u', body[1]
    )
    declared = re.search(r"RID_COUNT\s*=\s*(\d+)", txt)
    if declared and int(declared.group(1)) != len(rows):
        sys.exit(
            "scan_interior_ptrs: parsed %d REGIONS rows but RID_COUNT is %s -- the position keying "
            "this reader depends on is broken, which is a finding, not a row to pad"
            % (len(rows), declared.group(1))
        )
    return [
        (i, name_by_rid.get(i, n), int(b, 16), int(sz), int(re_))
        for i, (n, b, sz, re_) in enumerate(rows)
    ]


def read_blob(fixture):
    """-> (header dict, raw bytes, [(rid, len, payload_offset)])."""
    path = os.path.join(FIXTURES, fixture, "world.bin.zz")
    if not os.path.isfile(path):
        sys.exit("scan_interior_ptrs: no world blob at %s" % os.path.relpath(path, REPO))
    raw = zlib.decompress(open(path, "rb").read())
    if raw[:8] != MAGIC:
        sys.exit("scan_interior_ptrs: %s is not a world blob (magic %r)" % (fixture, raw[:8]))
    fmt, schema, block_count, payload_len = struct.unpack_from("<IIII", raw, 8)
    (content_hash,) = struct.unpack_from("<Q", raw, 24)
    lockstep_combined, lockstep_state, game_clock = struct.unpack_from("<QQQ", raw, 32)
    (
        step,
        mask_flags,
        hash_sink_fp,
        hash_manifest_fp,
        hash_slice_count,
        region_head_ptrs,
        registry_span_ptrs,
        nav_offset,
        nav_len,
    ) = struct.unpack_from("<IIIIIIIII", raw, 56)
    hdr = {
        "fixture": fixture,
        "format": fmt,
        "schema": schema,
        "block_count": block_count,
        "payload_len": payload_len,
        "content_hash": "%016X" % content_hash,
        "lockstep_state": "%016X" % lockstep_state,
        "step": step,
        "region_head_ptrs": region_head_ptrs,
        "registry_span_ptrs": registry_span_ptrs,
        "nav_offset": nav_offset,
        "nav_len": nav_len,
        "raw_bytes": len(raw),
    }
    blocks, off = [], 0
    while off < payload_len:
        if off + 8 > payload_len:
            sys.exit("scan_interior_ptrs: block table runs past payload_len -- truncated blob")
        rid, ln = struct.unpack_from("<II", raw, HDR_LEN + off)
        blocks.append((rid, ln, HDR_LEN + off + 8))
        off += 8 + ln
    hdr["parsed_payload"] = off
    hdr["parsed_blocks"] = len(blocks)
    return hdr, raw, blocks


# ---- the target index --------------------------------------------------------------------------


class TargetIndex:
    """`stock_region_containing()` as an O(1) lookup over the registry's stock spans.

    A byte-addressed owner map rather than a per-value scan of 845 regions: the blob is ~1.9M
    aligned dwords and the phase control multiplies that by four. `owner` holds the LOWEST rid
    covering each byte (the order `libref_host`'s loop resolves in) and `ambiguous` marks the bytes
    more than one span covers -- overlap is real (gen_world_snapshot --check reports overlapping
    block pairs) and must be REPORTED, never silently resolved to the first match.
    """

    def __init__(self, regions):
        spans = [(b, b + r, rid) for rid, _n, b, _s, r in regions if b and r]
        if not spans:
            sys.exit("scan_interior_ptrs: the registry has no region with a base and a reach")
        self.lo = min(s[0] for s in spans)
        self.hi = max(s[1] for s in spans)
        n = self.hi - self.lo
        self.owner = array.array("i", b"\xff\xff\xff\xff" * n)  # -1 everywhere
        self.ambiguous = bytearray(n)
        self.union_bytes = 0
        for base, end, rid in sorted(spans, key=lambda s: s[2]):
            a, b = base - self.lo, end - self.lo
            sl = self.owner[a:b]
            if sl.count(-1) == len(sl):
                self.owner[a:b] = array.array("i", [rid]) * (b - a)
                self.union_bytes += b - a
            else:  # the rare overlapping span: resolve byte by byte and flag what collides
                for i in range(a, b):
                    if self.owner[i] < 0:
                        self.owner[i] = rid
                        self.union_bytes += 1
                    else:
                        self.ambiguous[i] = 1

    def rid_of(self, v):
        """-> rid, or -1 if v is inside no region's stock span."""
        if v < self.lo or v >= self.hi:
            return -1
        return self.owner[v - self.lo]

    def is_ambiguous(self, v):
        return self.lo <= v < self.hi and self.ambiguous[v - self.lo] != 0


# ---- the screen --------------------------------------------------------------------------------

PRINTABLE = range(0x20, 0x7F)


def scan_block(raw, off, ln, idx, phase):
    """One block at one phase -> (hits, [(offset, value, rid)]). Detail only for phase 0."""
    if ln <= phase + 3:
        return 0, []
    buf = array.array("I")
    n = (ln - phase) // 4
    buf.frombytes(raw[off + phase : off + phase + n * 4])
    if sys.byteorder != "little":  # the blob is a little-endian memory image
        buf.byteswap()
    hits, detail = 0, []
    rid_of = idx.rid_of
    for i in range(n):
        v = buf[i]
        if v == 0:
            continue
        rid = rid_of(v)
        if rid < 0:
            continue
        hits += 1
        if phase == 0:
            detail.append((i * 4, v, rid))
    return hits, detail


def screen(hdr, raw, blocks, idx, regions, phases=True):
    name_by_rid = {rid: n for rid, n, _b, _s, _r in regions}
    base_by_rid = {rid: b for rid, _n, b, _s, _r in regions}
    holders, head_total, hits_total = [], 0, 0
    for rid, ln, off in blocks:
        hits, detail = scan_block(raw, off, ln, idx, 0)
        if hits == 0:
            continue
        by_target, heads, amb, wide, runs = {}, 0, 0, 0, 0
        prev = None
        for o, v, trid in detail:
            by_target[trid] = by_target.get(trid, 0) + 1
            if v == base_by_rid.get(trid):
                heads += 1
            if idx.is_ambiguous(v):
                amb += 1
            if (
                (v & 0xFF) in PRINTABLE
                and ((v >> 8) & 0xFF) == 0
                and ((v >> 16) & 0xFF) in PRINTABLE
                and (v >> 24) == 0
            ):
                wide += 1
            if prev is not None and o - prev == 4:
                runs += 1
            prev = o
        ctrl = [scan_block(raw, off, ln, idx, p)[0] for p in (1, 2, 3)] if phases else []
        dwords = ln // 4
        dom_rid, dom_n = max(by_target.items(), key=lambda kv: kv[1])
        holders.append(
            {
                "rid": rid,
                "name": name_by_rid.get(rid, "<rid %d>" % rid),
                "bytes": ln,
                "dwords": dwords,
                "hits": hits,
                "density": hits / dwords if dwords else 0.0,
                "targets": len(by_target),
                "dominant": name_by_rid.get(dom_rid, "<rid %d>" % dom_rid),
                "dominant_hits": dom_n,
                "dominant_share": dom_n / hits,
                # The already-counted class, split out rather than folded in: a hit whose value IS a
                # region's base is what `region_head_ptrs` counts and what spine.cpp's "eleven"
                # names. Only `interior` is the population this pass is about.
                "base_equal": heads,
                "interior": hits - heads,
                "ambiguous": amb,
                "wide_text_share": wide / hits,
                "run_share": runs / hits,
                "phase_control": ctrl,
                # Against the STRONGEST misaligned phase, not the mean -- see the banner. None means
                # "no control was run"; inf means every misaligned phase read zero.
                "phase_lift": (
                    None
                    if not ctrl
                    else (float("inf") if max(ctrl) == 0 else hits / float(max(ctrl)))
                ),
                "by_target": sorted(
                    ((name_by_rid.get(t, "<rid %d>" % t), c) for t, c in by_target.items()),
                    key=lambda kv: -kv[1],
                ),
                "_detail": detail,
            }
        )
        head_total += heads
        hits_total += hits
    holders.sort(key=lambda h: -h["hits"])
    return holders, head_total, hits_total


# ---- self-checks -------------------------------------------------------------------------------


def self_checks(hdr, holders, head_total, raw, blocks, idx, regions):
    """-> [(ok, label, detail)]. These are what turn the table into evidence."""
    out = []
    out.append(
        (
            hdr["parsed_payload"] == hdr["payload_len"]
            and hdr["parsed_blocks"] == hdr["block_count"],
            "blob parse is complete",
            "%d blocks / %d payload bytes (header says %d / %d)"
            % (hdr["parsed_blocks"], hdr["parsed_payload"], hdr["block_count"], hdr["payload_len"]),
        )
    )
    out.append(
        (
            head_total == hdr["region_head_ptrs"],
            "base-equality subtotal reproduces the blob's own census",
            "this screen %d vs header region_head_ptrs %d" % (head_total, hdr["region_head_ptrs"]),
        )
    )

    # TLO_REGISTRY reader gate (F1F rider, user-ratified 2026-09-12). The world blob carries the
    # region's raw image pointers VERBATIM (the boot blob derives them instead); the only thing
    # keeping that safe is that materialise_tlo() in state/boot_snapshot.cpp is the sole reader
    # and it refuses a rebased bind. A future generated state view or hand reader would
    # dereference the recording's .rdata addresses -- so the sole-reader claim is a GATE here,
    # not prose. Positive control: the scan must FIND boot_snapshot.cpp, or it is broken.
    src_root = os.path.join(REPO, "src", "mh_dll")
    tlo_files = set()
    for dirpath, _dirs, files in os.walk(src_root):
        for f in files:
            if not f.endswith((".cpp", ".h")):
                continue
            p = os.path.join(dirpath, f)
            rel = os.path.relpath(p, src_root).replace("\\", "/")
            if rel.startswith("mh/addr/"):
                continue  # the generated registry defines the RID; it is the subject
            try:
                with open(p, encoding="utf-8", errors="replace") as fh:
                    if "RID_TLO_REGISTRY" in fh.read():
                        tlo_files.add(rel)
            except OSError:
                pass
    # boot_snapshot_selftest exercises the DERIVED block through the boot-snapshot API (and
    # excuses the raw one from its byte compare) -- it never touches the live region.
    tlo_allowed = {"libmh/state/boot_snapshot.cpp", "mh_nettest/boot_snapshot_selftest.cpp"}
    out.append(
        (
            tlo_files == tlo_allowed,
            "TLO_REGISTRY readers are exactly the adjudicated set",
            "RID_TLO_REGISTRY referenced by %s (allowed: %s)"
            % (sorted(tlo_files) or "NOBODY -- the scan is broken", sorted(tlo_allowed)),
        )
    )

    # The G_TEXT_PTRS adjudication, entry for entry -- the one holder libref_host actually re-stamps.
    name_by_rid = {rid: n for rid, n, _b, _s, _r in regions}
    tp = next((h for h in holders if h["name"] == "G_TEXT_PTRS"), None)
    if tp is None:
        out.append((False, "G_TEXT_PTRS is a holder", "not present in this blob"))
        return out
    blk = next(((r, ln, off) for r, ln, off in blocks if r == tp["rid"]), None)
    slots = blk[1] // 4
    zero = rdata = leftover = 0
    for i in range(slots):
        (v,) = struct.unpack_from("<I", raw, blk[2] + i * 4)
        if v == 0:
            zero += 1
        elif idx.rid_of(v) >= 0:
            pass  # counted as a hit above
        elif v == G_TEXT_PTRS_RDATA_VA:
            rdata += 1
        else:
            leftover += 1
    into = dict(tp["by_target"])
    ok = (
        slots == 806
        and tp["hits"] == 721
        and into.get("G_TEXT_BLOCK") == 720
        and into.get("STRAT_SCENARIO_PLANET_NAME_W") == 1
        and rdata == 85
        and zero == 0
        and leftover == 0
    )
    out.append(
        (
            ok,
            "G_TEXT_PTRS reproduces libref_host's adjudication",
            "%d slots -- %d into carried regions (%s), %d .rdata %08X, %d zero, %d UNADJUDICATED"
            % (
                slots,
                tp["hits"],
                " + ".join("%d %s" % (c, n) for n, c in tp["by_target"]),
                rdata,
                G_TEXT_PTRS_RDATA_VA,
                zero,
                leftover,
            ),
        )
    )
    return out


# ---- reporting ---------------------------------------------------------------------------------


def fmt_table(holders):
    L = []
    L.append(
        "%4s  %-44s %9s %8s %7s %5s %7s %5s  %-30s %7s %6s %6s %6s"
        % (
            "rid",
            "holder region",
            "bytes",
            "dwords",
            "hits",
            "base=",
            "dens%",
            "tgts",
            "dominant target",
            "dom%",
            "lift",
            "run%",
            "wtxt%",
        )
        + "  %-14s" % "ctrl@1/2/3"
    )
    L.append("-" * 178)
    for h in holders:
        pl = h["phase_lift"]
        lift = "   n/a" if pl is None else ("   inf" if pl == float("inf") else "%6.1f" % pl)
        L.append(
            "%4d  %-44s %9d %8d %7d %5d %7.3f %5d  %-30s %6.1f%% %s %5.1f%% %5.1f%%"
            % (
                h["rid"],
                h["name"][:44],
                h["bytes"],
                h["dwords"],
                h["hits"],
                h["base_equal"],
                100.0 * h["density"],
                h["targets"],
                h["dominant"][:30],
                100.0 * h["dominant_share"],
                lift,
                100.0 * h["run_share"],
                100.0 * h["wide_text_share"],
            )
            + "  %-14s" % ("/".join(str(c) for c in h["phase_control"]) or "-")
        )
    return "\n".join(L)


def run(fixture, phases=True):
    regions = parse_regions()
    idx = TargetIndex(regions)
    hdr, raw, blocks = read_blob(fixture)
    holders, head_total, hits_total = screen(hdr, raw, blocks, idx, regions, phases)
    checks = self_checks(hdr, holders, head_total, raw, blocks, idx, regions)
    return regions, idx, hdr, raw, blocks, holders, head_total, hits_total, checks


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE, help="fixture under tools/data/fixtures")
    ap.add_argument("--all-fixtures", action="store_true", help="totals for every fixture present")
    ap.add_argument("--holder", default=None, help="print one holder's hits in detail")
    ap.add_argument("--json", default=None, help="write the full screen to this path")
    ap.add_argument("--check", action="store_true", help="self-checks only; exit 1 on failure")
    ap.add_argument("--no-phase", action="store_true", help="skip the misaligned phase control")
    # The SCOPING partition, so a density-threshold decision is re-derivable by running the tool
    # rather than by reading a number out of prose. Both halves are needed and neither alone works:
    # density alone puts a 6-byte format string holding one aliased dword at 100%, and a hit count
    # alone puts 300 KB of text above any table. The defaults are the ones the F1(f) analysis
    # proposes; the separation is not finely tuned (see --partition's own output for the margin).
    ap.add_argument("--min-density", type=float, default=0.25, help="scoping: hits/dwords floor")
    ap.add_argument("--min-hits", type=int, default=32, help="scoping: interior-hit count floor")
    ap.add_argument(
        "--partition", action="store_true", help="split the holders by the scoping threshold"
    )
    args = ap.parse_args()

    fixtures = (
        sorted(
            d
            for d in os.listdir(FIXTURES)
            if os.path.isfile(os.path.join(FIXTURES, d, "world.bin.zz"))
        )
        if args.all_fixtures
        else [args.fixture]
    )

    rc = 0
    payload = []
    for fx in fixtures:
        regions, idx, hdr, raw, blocks, holders, head_total, hits_total, checks = run(
            fx, phases=not args.no_phase
        )
        print(
            "[scan_interior_ptrs] %s -- %d blocks, %d payload bytes, step %d, lockstep_state %s"
            % (fx, hdr["block_count"], hdr["payload_len"], hdr["step"], hdr["lockstep_state"])
        )
        print(
            "  registry: %d regions, %d with base+reach, union %d bytes over [%08X,%08X)"
            % (
                len(regions),
                sum(1 for _r, _n, b, _s, re_ in regions if b and re_),
                idx.union_bytes,
                idx.lo,
                idx.hi,
            )
        )
        base_only = sum(1 for h in holders if h["interior"] == 0)
        print(
            "  SCREEN: %d holder region(s), %d dword(s) landing in a carried stock span -- %d of "
            "them BASE-EQUALITY (the header's own census, spine.cpp's 'eleven'), so the INTERIOR "
            "population this pass is about is %d dword(s) over %d holder(s) (%d holder(s) are "
            "base-equality only and carry no interior residue at all)"
            % (
                len(holders),
                hits_total,
                head_total,
                hits_total - head_total,
                len(holders) - base_only,
                base_only,
            )
        )
        for ok, label, detail in checks:
            print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", label, detail))
            if not ok:
                rc = 1
        if not args.check and not args.all_fixtures:
            print()
            print(fmt_table(holders))
            if args.holder:
                h = next((x for x in holders if x["name"] == args.holder), None)
                if h is None:
                    print("\nno holder named %s in this blob" % args.holder)
                    rc = 1
                else:
                    print("\n%s -- %d hit(s):" % (h["name"], h["hits"]))
                    nm = {r: n for r, n, _b, _s, _re in regions}
                    for o, v, trid in h["_detail"][:400]:
                        print("  +%-8d %08X -> %s" % (o, v, nm.get(trid)))
                    if len(h["_detail"]) > 400:
                        print("  ... %d more" % (len(h["_detail"]) - 400))

        above = [
            h
            for h in holders
            if h["density"] >= args.min_density and h["interior"] >= args.min_hits
        ]
        below = [h for h in holders if h not in above]
        if args.partition and not args.check:
            qualified = [h for h in holders if h["interior"] >= args.min_hits]
            runner = max((h["density"] for h in qualified if h not in above), default=0.0)
            print(
                "\nSCOPING PARTITION at density >= %.3f and interior hits >= %d:"
                % (args.min_density, args.min_hits)
            )
            print(
                "  ABOVE (%d): %s"
                % (
                    len(above),
                    ", ".join(
                        "%s (%.1f%% of %d dwords, %d interior)"
                        % (h["name"], 100.0 * h["density"], h["dwords"], h["interior"])
                        for h in above
                    )
                    or "-",
                )
            )
            print(
                "  BELOW (%d holders, %d interior dwords). The densest holder that clears the hit "
                "floor but not the density floor is at %.3f%%, so any density cut between that and "
                "%.3f%% selects the SAME set -- the separation is a gap, not a tuned constant."
                % (
                    len(below),
                    sum(h["interior"] for h in below),
                    100.0 * runner,
                    100.0 * min((h["density"] for h in above), default=0.0),
                )
            )
        payload.append(
            {
                "header": hdr,
                "scoping": {
                    "min_density": args.min_density,
                    "min_hits": args.min_hits,
                    "above": [h["name"] for h in above],
                },
                "holders": [{k: v for k, v in h.items() if k != "_detail"} for h in holders],
                "totals": {
                    "holders": len(holders),
                    "hits": hits_total,
                    "base_equality": head_total,
                    "interior": hits_total - head_total,
                },
                "checks": [{"ok": o, "label": lab, "detail": d} for o, lab, d in checks],
            }
        )

    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)) or ".", exist_ok=True)
        io.open(args.json, "w", encoding="utf-8", newline="\n").write(
            json.dumps(payload if len(payload) > 1 else payload[0], indent=1) + "\n"
        )
        print("\nwrote %s" % args.json)
    return rc


if __name__ == "__main__":
    sys.exit(main())
