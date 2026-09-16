#!/usr/bin/env python3
"""check_movable_addresses.py -- nothing may spell a MOVABLE region's address as a constant.

THE PROPERTY, and why it is not the same one check_sim_addresses.py already enforces. That gate asks
"does a migrated MODULE name a game address outside its binder", and it is scoped to the seven
translated modules. This asks a narrower question of the WHOLE dll: does anything -- module, seam,
harness, instrument -- spell `mh::addr::X` where X is a region a relocating host is allowed to MOVE?

Such a constant is not merely untidy. Under `[harness] relocate_state=1` the bytes are in the host's
arena and the stock range is filled with 0xCD, so the constant reads poison. Measured, on the first
relocated soak (SB-HOSTFREE, 2026-09-06): `harness.cpp` read `_G_LLM_STRAT_AI_ENABLED` from its
stock VA and printed `master_gate=3452816845` -- 0xCDCDCDCD -- in the run's own AI banner, and the
run still reported PASS. An instrument reading dead memory is worse than a module doing it, because
its output looks well-formed.

IT IS AT ZERO, AND IT IS STILL A RATCHET. The 16 sites this opened with -- all in mh/seams, the
instrumentation layer the module gate never covered -- were drained on 2026-09-06, so the baseline is
an empty object. Keep the FILE: an absent baseline means an implicit zero and would then be trivially
satisfied, and the ratchet still has work to do, because `movable` GROWS (see below) and a region that
becomes movable while something spells its address is a new hazard on old code.

IT ALSO TIGHTENS BY ITSELF, WHICH IS THE POINT. `movable` is derived from the generated registry,
and it GROWS as promotion retires the original accessors that pin regions in place. So the day a
region becomes movable while something still spells its address, this goes red -- naming a hazard
that did not exist when the code was written and that nobody would otherwise re-examine.

    python tools/check_movable_addresses.py                  # the gate (lint_repo)
    python tools/check_movable_addresses.py --report         # per-file, per-region breakdown
    python tools/check_movable_addresses.py --update-baseline  # lowers only
    python tools/check_movable_addresses.py --selftest       # prove it can go red
"""

import argparse
import bisect
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _dllsrc  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_regions.gen.h")
ADDRS = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_addrs.gen.h")
ROOT = os.path.join(REPO, "src", "mh_dll", "mh")
BASELINE = os.path.join(REPO, "tools", "data", "movable_va_baseline.json")

# The declared binders -- the one place a region's address is allowed to be resolved -- plus the
# generated headers, whose `mh::addr::` mentions are the static_asserts that PIN the registry to the
# address table. Those are descriptions, not dereferences, and banning them would ban the pinning.
EXEMPT = {
    "mh_regions.gen.h",
    "mh_addrs.gen.h",
    "host_bind.cpp",
    "region_runtime.h",
    "lockstep_state.cpp",
    "overlay_hoist.cpp",
    "save_state.cpp",
    "save_table.gen.h",
    "ai_state.cpp",
    "order_queue.cpp",
    "issue_state.cpp",
    "tact_state.cpp",
}

# The base column is wrapped in MH_STOCK_BASE(...) since LIB-REF-SPLIT (the standalone build emits
# 0 there; addr/mh_regions.gen.h's own banner has the why), so the wrapper is optional here. This
# parser is not the only consumer of the row shape and it is the one that behaved WELL when the shape
# moved: it counted zero rows and said "has the row shape changed?" instead of reporting a clean pass
# over nothing. Keep that property -- the `if not rows: fail` below is what makes this regex safe to
# edit at all.
ROW = re.compile(
    r'\{"([^"]+)",\s*(?:MH_STOCK_BASE\()?0x[0-9a-fA-F]+u\)?,\s*(\d+)u,\s*(\d+)u,\s*OWN_\w+,'
    r"\s*static_cast<uint8_t>\([^)]*\),\s*(true|false),\s*(true|false),\s*(true|false)\}"
)
USE = re.compile(r"mh::addr::([A-Za-z_][A-Za-z0-9_]*)")


def movable_regions():
    """The movable regions as (base, size, name), sorted by base, off the generated registry.

    Read from the HEADER rather than re-derived from tools/data/*.json on purpose: the header is the
    artefact the DLL compiles against, so this gate and the bind cannot disagree about which regions
    move. Mirrors mh_regions.gen.h's own `is_movable()`.
    """
    src = open(HEADER, encoding="utf-8").read()
    rows = [
        (m.group(1), int(m.group(2)), int(m.group(3)), m.group(6) == "true")
        for m in ROW.finditer(src)
    ]
    if not rows:
        raise SystemExit(
            "no REGIONS[] rows parsed out of %s -- has the row shape changed?" % HEADER
        )
    base = {}
    # The SECOND place the row shape is parsed, and the one the `if not rows` guard above does NOT
    # cover: an empty `base` map silently filters every region out of the result below, which reads
    # as "nothing is movable" rather than as a parse failure. That is what happened when
    # MH_STOCK_BASE was introduced -- the row guard passed and the SELFTEST caught it, one layer
    # down, with "no movable regions parsed -- the gate would be inert". Both parsers take the
    # optional wrapper now.
    for m in re.finditer(
        r'\{"([^"]+)",\s*(?:MH_STOCK_BASE\()?(0x[0-9a-fA-F]+)u\)?,\s*(\d+)u,\s*(\d+)u,', src
    ):
        base[m.group(1)] = int(m.group(2), 16)
    # MIRRORS mh_regions.gen.h's is_movable() -- `relocatable`, non-zero, and not itself an
    # overrunning window. It does NOT exclude regions that lie UNDER someone else's window: those
    # move now, because the four blocks that span regions are decomposed (SLICED_BLOCKS) and
    # tools/check_save_block_slices.py refuses a new one that is not. Keeping that exclusion here
    # after the header dropped it is exactly the drift this file warns about elsewhere: two
    # predicates, one question.
    return sorted(
        (base[n], size, n)
        for n, size, reach, reloc in rows
        if reloc and reach and reach == size and n in base
    )


def addr_constants():
    """Every `mh::addr::` constant and its VALUE, out of the generated address table."""
    src = open(ADDRS, encoding="utf-8").read()
    return {
        m.group(1): int(m.group(2), 16)
        for m in re.finditer(r"inline constexpr uintptr_t (\w+)\s*=\s*(0x[0-9a-fA-F]+)u", src)
    }


def region_at(movable, addr):
    """The movable region containing `addr`, or None.

    BY ADDRESS, NOT BY NAME, and that is the whole correction of 2026-09-06. The first version of
    this gate compared the CONSTANT'S NAME against the registry's region names, which is only the
    same question when the two tables happen to spell the bytes the same way. They do not always:
    `squad_blackboard` (mh_addrs.gen.h) and `_G_LLM_SQUAD_STATUS` (the registry) are the same 1024
    bytes at 0x00e15e60 under two names, so a name-keyed scan reported the tree clean while
    launch.cpp wrote the squad to the stock alias of a region the host had relocated -- and the
    tactical mission ended at frame 1 with an empty squad. Five sibling scalars were hidden the same
    way. Comparing addresses cannot be fooled by a rename, an alias, or an interior offset.
    """
    i = bisect.bisect_right([b for b, _s, _n in movable], addr) - 1
    if i < 0:
        return None
    base, size, name = movable[i]
    return name if addr < base + size else None


def scan():
    """{relative path: {"constant -> region": count}} for every non-exempt source under mh/."""
    movable = movable_regions()
    addrs = addr_constants()
    out = {}
    for _tree, root, _dirs, files in _dllsrc.walk():
        if "attic" in root.replace("\\", "/").split("/"):
            continue
        for fn in files:
            if not fn.endswith((".cpp", ".h")) or fn in EXEMPT:
                continue
            path = os.path.join(root, fn)
            text = open(path, encoding="utf-8", errors="ignore").read()
            text = re.sub(r"//[^\n]*", "", text)
            text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
            hits = {}
            for m in USE.finditer(text):
                nm = m.group(1)
                if nm not in addrs:
                    continue
                r = region_at(movable, addrs[nm])
                if r:
                    # The alias is reported WITH the region it lands in, because "squad_blackboard is
                    # a movable address" is not actionable and "squad_blackboard IS _G_LLM_SQUAD_STATUS,
                    # which moves" names the fix.
                    key = nm if nm == r else "%s -> %s" % (nm, r)
                    hits[key] = hits.get(key, 0) + 1
            if hits:
                out[os.path.relpath(path, REPO).replace("\\", "/")] = hits
    return out


def load_baseline():
    if not os.path.exists(BASELINE):
        return {}
    with open(BASELINE, encoding="utf-8") as fh:
        return {k: v for k, v in json.load(fh).items() if not k.startswith("_")}


def counts(found):
    return {f: sum(h.values()) for f, h in found.items()}


def check(found, base=None):
    base = load_baseline() if base is None else base
    now = counts(found)
    problems = []
    for f, n in sorted(now.items()):
        allowed = base.get(f, 0)
        if n > allowed:
            problems.append(
                "%s: %d movable-region address(es), baseline %d -- %s"
                % (f, n, allowed, ", ".join(sorted(found[f])))
            )
    return problems


def update_baseline():
    base, now = load_baseline(), counts(scan())
    merged = {f: min(now.get(f, 0), base[f]) for f in base if now.get(f, 0) < base[f]}
    merged.update({f: n for f, n in now.items() if f not in base})
    kept = {f: (merged.get(f, base.get(f, now.get(f, 0)))) for f in set(base) | set(now)}
    kept = {f: n for f, n in sorted(kept.items()) if n}
    payload = {
        "_comment": [
            "GENERATED by tools/check_movable_addresses.py --update-baseline. A RATCHET: it only",
            "ever lowers. Each entry is a file that still spells the address of a region a",
            "relocating host is allowed to MOVE, which under [harness] relocate_state=1 reads 0xCD.",
            "The drain target is an empty object; do not delete the file to reach it, since an",
            "absent baseline means an implicit zero and would then be trivially satisfied.",
        ]
    }
    payload.update(kept)
    with open(BASELINE, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(payload, fh, indent=1)
        fh.write("\n")
    print("wrote %s (%d file(s), %d use(s))" % (BASELINE, len(kept), sum(kept.values())))


def report():
    found = scan()
    print("movable-region addresses spelled as constants (SB-HOSTFREE):")
    for f, hits in sorted(found.items()):
        print("  %-52s %d" % (f, sum(hits.values())))
        for n, k in sorted(hits.items()):
            print("      %-46s %d" % (n, k))
    print(
        "  %d use(s) in %d file(s); %d region(s) are movable in this build"
        % (sum(sum(h.values()) for h in found.values()), len(found), len(movable_regions()))
    )
    return 0


def selftest():
    """A gate nobody has watched fail is a gate nobody has."""
    fails = 0
    found = scan()
    if check(found):
        print("SELFTEST FAIL: the committed tree is already over its own baseline")
        fails += 1
    else:
        print("ok: the committed tree is at or under baseline")

    mv = movable_regions()
    if not mv:
        print("SELFTEST FAIL: no movable regions parsed -- the gate would be inert")
        fails += 1
    else:
        print("ok: %d movable region(s) parsed out of the generated registry" % len(mv))

    # THE ARM WITH TEETH: a file ALREADY at its baseline may not gain one more use. Driven off a
    # SYNTHETIC pair rather than off whatever the tree happens to contain -- the baseline is empty now
    # that the drain finished, and an arm that silently stops running when the tree is clean is the
    # vacuous shape this whole file exists to refuse.
    synth_base = {"src/mh_dll/mh/seams/fixture.cpp": 2}  # CITATION-OK
    at_baseline = {"src/mh_dll/mh/seams/fixture.cpp": {"_G_LLM_A": 1, "_G_LLM_B": 1}}  # CITATION-OK
    over = {"src/mh_dll/mh/seams/fixture.cpp": {"_G_LLM_A": 2, "_G_LLM_B": 1}}  # CITATION-OK
    if check(at_baseline, synth_base):
        print("SELFTEST FAIL: a file exactly AT its baseline was refused")
        fails += 1
    elif not check(over, synth_base):
        print("SELFTEST FAIL: one use over baseline did NOT trip the ratchet")
        fails += 1
    else:
        print("ok: ratchet allows a file at its baseline and fires on one use over it")

    # And a file that has NONE today must not be allowed to acquire one.
    fresh = dict(found)
    fresh["src/mh_dll/mh/NOT_A_REAL_FILE.cpp"] = {"_G_LLM_FIXTURE": 1}  # CITATION-OK
    if not check(fresh):
        print("SELFTEST FAIL: a NEW file with a movable-region address did not trip the ratchet")
        fails += 1
    else:
        print("ok: a new file with a movable-region address is refused")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        print("=== check_movable_addresses --selftest ===")
        n = selftest()
        print("%d failure(s)" % n)
        return 1 if n else 0
    if args.update_baseline:
        update_baseline()
        return 0
    if args.report:
        return report()

    found = scan()
    problems = check(found)
    if problems:
        print("=== check_movable_addresses: over baseline ===")
        for p in problems:
            print("  " + p)
        print(
            "  These regions MOVE under [harness] relocate_state=1, and the stock range they leave\n"
            "  is filled with 0xCD -- so a constant address here reads poison. Resolve through\n"
            "  mh::state::ptr<T>(RID_...) / live_base(RID_...). If a drain LOWERED a count, run\n"
            "  `python tools/check_movable_addresses.py --update-baseline` in the same commit."
        )
        return 1
    n = sum(sum(h.values()) for h in found.values())
    print(
        "check_movable_addresses: ok -- %d movable-region address(es) in %d file(s), all at or "
        "under baseline (drain target 0; --report for the breakdown)" % (n, len(found))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
