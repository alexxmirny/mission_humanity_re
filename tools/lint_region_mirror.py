#!/usr/bin/env python3
"""lint_region_mirror.py -- the determinism harness's region manifest is encoded TWICE; keep them equal.

The DLL side owns the hashed regions in order plus which of them are dropped from the state-only
verdict; `mp_analyze.py` owns `REGION_NAMES` (the same list, POSITIONALLY -- it maps the
"R <step> h0 h1 ..." columns to names) and `STATE_EXCLUDED` (the same decision, BY NAME). Nothing
enforced that the two agreed. (Until ST2M the DLL side was `harness.cpp`'s hand-written `REGIONS[]`
+ `state_excluded()`; it is now the generated `HASH_REGIONS[]` -- see below.)

They have already disagreed, and it was not obvious from either side. O3 excluded three order regions
in the DLL and the analyzer kept naming them, so a run's `state_only_regions` listed regions the
verdict had already dropped -- output that reads like a finding and is an artefact. The positional
list is worse: a region INSERTED rather than appended silently re-labels every column after it, so
every subsequent desync report names the wrong region while looking perfectly well-formed.

ST2M (2026-07-31) CHANGED WHAT THIS IS FOR, and it is worth keeping rather than deleting. Both
encodings are now GENERATED from tools/data/hash_manifest.json (the DLL side into
addr/mh_regions.gen.h as HASH_REGIONS[], the analyzer side into a marker block in mp_analyze.py), so
they cannot drift by hand-editing one of them. What they CAN still do is drift by a hand-edit of a
generated file -- which gen_state_registry.py --check catches by regenerating and diffing, but only
for the files it knows to rewrite. This reads the two ARTEFACTS and compares them to each other,
which is a different arm: it stays true even if the generator's idea of either end changes.

It also fixes a blind spot it used to have. The old version resolved exclusions by scanning
state_excluded() for `IDX_*` tokens -- but three regions (time_globals / frame_ring / fps_estimate)
were excluded through NAME-resolved `g_idx_*` variables instead, which that regex could not see. So
the lint reported the two sides as agreeing while mp_analyze's STATE_EXCLUDED was missing three
entries the DLL had always dropped. Reading the flag off the table removes the class of miss.

This is a pure text check -- no Ghidra, no build -- so it belongs in lint_repo.py alongside the
generated-header drift gates.

    python tools/lint_region_mirror.py
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_regions.gen.h")
ANALYZE = os.path.join(REPO, "tools", "mp_analyze.py")

ENTRY = re.compile(r'\{"([A-Za-z0-9_]+)",\s*RID_\w+,[^}]*?,\s*(true|false)\}')


def harness_entries(src):
    blk = src[src.index("inline constexpr hash_region HASH_REGIONS[] = {") :]
    blk = blk[: blk.index("\n};")]
    blk = re.sub(r"//[^\n]*", "", blk)  # a commented-out example entry is not a real one
    return ENTRY.findall(blk)


def harness_regions(src):
    return [n for n, _ in harness_entries(src)]


def harness_excluded(src, names):
    return {n for n, ex in harness_entries(src) if ex == "true"}


def analyze_names(src):
    blk = src[src.index("REGION_NAMES = [") :]
    blk = blk[: blk.index("\n]")]
    blk = re.sub(r"#[^\n]*", "", blk)
    return re.findall(r'"([A-Za-z0-9_]+)"', blk)


def analyze_excluded(src):
    blk = src[src.index("STATE_EXCLUDED = {") :]
    blk = blk[: blk.index("}")]
    blk = re.sub(r"#[^\n]*", "", blk)
    return set(re.findall(r'"([A-Za-z0-9_]+)"', blk))


def main():
    h = open(HARNESS, encoding="utf-8").read()
    a = open(ANALYZE, encoding="utf-8").read()

    hn, an = harness_regions(h), analyze_names(a)
    he, ae = harness_excluded(h, hn), analyze_excluded(a)

    errors = []
    if hn != an:
        errors.append(
            "REGIONS[] and REGION_NAMES differ (POSITIONAL -- a mismatch re-labels every column "
            "after the first difference, so desync reports name the wrong region):"
        )
        for i in range(max(len(hn), len(an))):
            x, y = (hn[i] if i < len(hn) else "<missing>"), (an[i] if i < len(an) else "<missing>")
            if x != y:
                errors.append(f"    idx {i}: harness.cpp={x}  mp_analyze.py={y}")
    if he != ae:
        errors.append("state_excluded() and STATE_EXCLUDED disagree:")
        for n in sorted(he - ae):
            errors.append(f"    {n}: excluded in harness.cpp, NOT in mp_analyze.py")
        for n in sorted(ae - he):
            errors.append(f"    {n}: excluded in mp_analyze.py, NOT in harness.cpp")

    if errors:
        print("\n".join(errors))
        return 1
    print(f"ok: {len(hn)} regions mirrored, {len(he)} excluded, both encodings agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
