#!/usr/bin/env python3
# tools/mp_desync_snap_diff.py -- byte-diff the desync watch's full-state snapshots (D25).
#
# The in-game desync watch (mh/desync/desync_watch.cpp, [desync] snapshot=1) reacts to a detected
# mismatch by having EVERY peer dump the full region set at one lockstep-aligned step, so the files
# are byte-comparable. This tool names what the hashes never can: the exact diverging OFFSETS.
#
# File format (little-endian, written by do_snapshot()):
#   header  { u32 magic 'MHSN'=0x4e53484d, u32 ver=1, u32 step, u32 region_count, u64 manifest_fp }
#   then per region, in manifest order: { u32 stream_len, stream bytes }
# The stream is the region's VERDICT-mode emission -- the exact preimage of the compared hash, with
# known peer-local fields zero-substituted (positionally aligned), so a byte that differs here is a
# byte the verdict judges. For plain (non-emitted) regions the stream offset IS the region offset.
#
# Usage:
#   python tools/mp_desync_snap_diff.py <snapA.bin> <snapB.bin> [<snapC.bin> ...]
#
# Region names come from tools/data/hash_manifest.json (guarded by the manifest fingerprint check:
# a build whose manifest differs refuses the comparison rather than mislabeling regions). For the
# game_player_data sub-slice regions (pK_ai_gates / pK_local / pK_ai_econ) diffs are additionally
# mapped to record-relative offsets and, where docs/structs.md knows the field, to field names.

import argparse
import json
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAGIC = 0x4E53484D

# region name -> offset of the slice inside the game_player_data record
PLAYER_SLICE_BASE = {"ai_gates": 0x00, "local": 0x38, "ai_econ": 0x1003C}
PLAYER_SLICE_RE = re.compile(r"^p(\d)_(ai_gates|local|ai_econ)$")


def read_snap(path):
    with open(path, "rb") as f:
        hdr = f.read(24)
        if len(hdr) != 24:
            sys.exit(f"{path}: truncated header")
        magic, ver, step, count, fp = struct.unpack("<IIIIQ", hdr)
        if magic != MAGIC:
            sys.exit(f"{path}: bad magic {magic:#x} (not a desync snapshot)")
        if ver != 1:
            sys.exit(f"{path}: unknown version {ver}")
        regions = []
        for _ in range(count):
            raw = f.read(4)
            if len(raw) != 4:
                sys.exit(f"{path}: truncated region table")
            (n,) = struct.unpack("<I", raw)
            data = f.read(n)
            if len(data) != n:
                sys.exit(f"{path}: truncated region stream")
            regions.append(data)
    return {"path": path, "step": step, "fp": fp, "regions": regions}


def load_manifest():
    p = os.path.join(REPO, "tools", "data", "hash_manifest.json")
    with open(p, encoding="utf-8") as f:
        m = json.load(f)
    return m["regions"] if isinstance(m, dict) else m


def load_player_fields():
    """docs/structs.md game_player_data rows -> sorted [(offset, name)] for offset->field lookup."""
    p = os.path.join(REPO, "docs", "structs.md")
    try:
        t = open(p, encoding="utf-8").read()
    except OSError:
        return []
    i = t.find("#### `game_player_data`")
    if i < 0:
        return []
    j = t.find("####", i + 10)
    rows = re.findall(r"^\| `\+0x([0-9a-f]+)` \| `([^`]+)` \|", t[i : j if j > 0 else len(t)], re.M)
    return sorted((int(o, 16), n) for o, n in rows)


def field_at(fields, off):
    """The last field at or before `off` (fields are sorted; sizes are not modeled -- the NEXT
    field's offset bounds it, which is enough to say 'inside X' vs 'between X and Y')."""
    prev = None
    for fo, fn in fields:
        if fo > off:
            break
        prev = (fo, fn)
    return prev


def diff_ranges(a, b):
    """[(start, end_exclusive)] of differing byte runs, merging gaps < 4 bytes."""
    n = min(len(a), len(b))
    out = []
    i = 0
    while i < n:
        if a[i] == b[i]:
            i += 1
            continue
        j = i
        while j < n and a[j] != b[j]:
            j += 1
        if out and i - out[-1][1] < 4:
            out[-1] = (out[-1][0], j)
        else:
            out.append((i, j))
        i = j
    if len(a) != len(b):
        out.append((n, max(len(a), len(b))))
    return out


def main():
    ap = argparse.ArgumentParser(description="byte-diff desync-watch full-state snapshots")
    ap.add_argument("snaps", nargs="+", help="mh_desync_snap_<step>.bin files, one per peer")
    ap.add_argument("--max-ranges", type=int, default=12, help="diff ranges printed per region")
    args = ap.parse_args()
    if len(args.snaps) < 2:
        sys.exit("need at least two snapshots to diff")

    snaps = [read_snap(p) for p in args.snaps]
    base = snaps[0]
    for s in snaps[1:]:
        if s["step"] != base["step"]:
            sys.exit(
                f"step mismatch: {base['path']} is step {base['step']}, {s['path']} is step "
                f"{s['step']} -- these are different instants and their diff names nothing"
            )
        if s["fp"] != base["fp"]:
            sys.exit(f"manifest fingerprint mismatch between {base['path']} and {s['path']}")

    manifest = load_manifest()
    if len(manifest) != len(base["regions"]):
        print(
            f"WARNING: manifest has {len(manifest)} regions, snapshot has {len(base['regions'])} "
            f"-- names may be stale (regions are compared positionally either way)"
        )
    fields = load_player_fields()

    print(f"snapshot step {base['step']}, {len(base['regions'])} regions, {len(snaps)} peers")
    any_diff = False
    for i, ref in enumerate(base["regions"]):
        name = manifest[i]["name"] if i < len(manifest) else f"region_{i}"
        addr = int(manifest[i]["addr"], 16) if i < len(manifest) else 0
        peers_diff = [s for s in snaps[1:] if s["regions"][i] != ref]
        if not peers_diff:
            continue
        any_diff = True
        who = ", ".join(
            os.path.basename(os.path.dirname(s["path"]) or s["path"]) for s in peers_diff
        )
        print(f"\n== {name} (idx {i}, base {addr:#x}, {len(ref)} bytes) differs vs: {who}")
        m = PLAYER_SLICE_RE.match(name)
        slice_base = PLAYER_SLICE_BASE[m.group(2)] if m else None
        for s in peers_diff:
            ranges = diff_ranges(ref, s["regions"][i])
            shown = ranges[: args.max_ranges]
            print(f"   vs {s['path']}: {len(ranges)} differing run(s)")
            for lo, hi in shown:
                line = f"      +{lo:#08x}..+{hi:#08x} ({hi - lo} B)"
                if addr:
                    line += f"  VA {addr + lo:#010x}"
                if slice_base is not None:
                    rec_off = slice_base + lo
                    line += f"  record +{rec_off:#x}"
                    hit = field_at(fields, rec_off)
                    if hit:
                        line += f"  ~ {hit[1]} (+{rec_off - hit[0]:#x} into it)"
                print(line)
                a, b = ref[lo : min(hi, lo + 16)], s["regions"][i][lo : min(hi, lo + 16)]
                print(f"         A: {a.hex()}  B: {b.hex()}")
            if len(ranges) > len(shown):
                print(f"      ... {len(ranges) - len(shown)} more (raise --max-ranges)")
    if not any_diff:
        print("ALL REGIONS IDENTICAL across the given snapshots")


if __name__ == "__main__":
    main()
