"""Phase 1 checkpoint: parse retail INIT.CFG + INITLANG.CFG and self-verify.

Gate (must all hold):
  1. zero unrecognized/ill-formed tokens (the safety net the game lacks);
  2. ``canon_diff(retail, retail) == []`` across two independent parses
     (canonicalization is a deterministic pure function);
  3. census matches the RE'd ground truth in the cfg grammar notes.

Usage:
  python -m src.formats.cfgkit.check [--init <INIT.CFG>] [--lang <initlang.cfg>]

Defaults point at the verified-pristine clean unpack.
"""
from __future__ import annotations

import argparse
import sys

from . import canon
from ._paths import CLEAN
from .parse import parse_files
DEF_INIT = CLEAN + "/mh/init/INIT.CFG"
DEF_LANG = CLEAN + "/mh_ex/init/initlang.cfg"

# Ground-truth section/entry counts, verified two independent ways 2026-07-05
# (this parser's nesting-aware pass + a standalone recount). These are the counts
# the game's pass-1 CountObjects produces (headers matched only at top level).
# NOTE: they correct the planning-time census in the cfgkit plan/memory, which
# naively counted `KEYWORD "name"` lines and so conflated section headers with
# body references: WEAPON "33" = 27 sections + 6 BUILDING weapon refs (so the R4
# "33 vs Weapon[32] overflow" concern is void — there are 27); UPGRADE "56" = 50
# sections + BUILDING upgrade refs; ANIM is 368, not 296.
EXPECTED = {
    "ANIM": 368, "WEAPON": 27, "UNIT": 54, "BUILDING": 68, "PROGRESS": 286,
    "PROJECT": 86, "TREE": 48, "SYSTEM": 3, "PLANET": 25, "UPGRADE": 50,
    "STONE": 0, "DEF_BANK": 57, "BANK_section": 43, "DEFINE": 989,
    "ANIM_length_total": 1653,
}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--init", default=DEF_INIT)
    ap.add_argument("--lang", default=DEF_LANG)
    args = ap.parse_args(argv)

    model = parse_files(args.init, args.lang)
    cen = model.census()

    print("=== census ===")
    for k in sorted(cen):
        exp = EXPECTED.get(k)
        flag = ""
        if exp is not None:
            flag = "  OK" if exp == cen[k] else f"  <-- expected {exp}  DIFF"
        print(f"  {k:20} {cen[k]:>7}{flag}")

    census_ok = all(EXPECTED[k] == cen.get(k) for k in EXPECTED)

    print("\n=== token issues ===")
    if model.issues:
        for iss in model.issues[:40]:
            print(f"  {iss.source}:{iss.line}: {iss.message}")
        if len(model.issues) > 40:
            print(f"  ... and {len(model.issues) - 40} more")
    else:
        print("  none (zero unrecognized tokens)")

    print("\n=== canon_diff(retail, retail) ===")
    model2 = parse_files(args.init, args.lang)
    diff = canon.canon_diff(model, model2)
    if diff:
        for d in diff[:40]:
            print("  " + d)
    else:
        print("  [] (canon is deterministic)")

    # Prove the comparator is discriminating (guards against a vacuously-empty
    # diff — e.g. a canonicalize() that drops the mutated field).
    import copy
    mutant = copy.deepcopy(model)
    mutant.classes["UNIT"][0]["energy"] += 1
    discriminates = bool(canon.canon_diff(model, mutant))
    print(f"\n=== canon discrimination === {'OK (detects a 1-field change)' if discriminates else 'FAIL (blind!)'}")

    ok = (not model.issues) and (not diff) and census_ok and discriminates
    print("\nPHASE 1 CHECKPOINT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
