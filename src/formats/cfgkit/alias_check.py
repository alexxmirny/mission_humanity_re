"""Phase 4b checkpoint: the English alias layer is invisible on the wire.

Gates (all on retail):
  A. model round-trip, ALL names aliased (stress bijection):
        emit(to_wire(to_english(retail)))  byte-identical to  emit(retail)
  B. same, but through the YAML source tree (write_tree -> read_tree with the
     alias table): the full editor loop is lossless.
  C. bootstrap table (initlang glosses + ore resources) through the tree:
     byte-identical (a realistic partial table).
  D. discrimination: the aliased intermediate really renamed things (guards a
     no-op transform) and the bootstrap table has zero dangling entries.

Usage:
  python -m src.formats.cfgkit.alias_check [--init ...] [--lang ...]
"""
from __future__ import annotations

import argparse
import shutil
import sys
import tempfile

from . import aliases as A
from . import canon, emit, yamlio
from ._paths import INIT_DEFAULT, LANG_DEFAULT
from .parse import parse_files


def _stress_table(model):
    """Alias every name in every namespace to a unique reversible English form,
    so the round-trip exercises every name-bearing position (defs + all refs)."""
    uni = A.model_names(model)
    sections = {ns: {w: "en__" + w for w in names} for ns, names in uni.items()}
    return A.AliasTable(sections)


def _emit_bytes(model):
    it, lt = emit.emit(model)
    return it, lt


def _identical(m0, m1):
    """(ok, detail) — byte-identical emit of two models."""
    a_i, a_l = _emit_bytes(m0)
    b_i, b_l = _emit_bytes(m1)
    if a_i == b_i and a_l == b_l:
        return True, ""
    which = []
    if a_i != b_i:
        which.append("INIT.CFG")
    if a_l != b_l:
        which.append("initlang.cfg")
    return False, ", ".join(which) + " differ"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--init", default=INIT_DEFAULT)
    ap.add_argument("--lang", default=LANG_DEFAULT)
    args = ap.parse_args(argv)

    retail = parse_files(args.init, args.lang)
    stress = _stress_table(retail)
    boot = A.bootstrap(retail, args.lang)

    print("=== Phase 4b alias gates ===")

    # A — model-level, all names
    okA, detA = _identical(retail, stress.to_wire(stress.to_english(retail)))
    print(f"  A model round-trip (all names)   : {'PASS' if okA else 'FAIL — ' + detA}"
          f"   [{len(stress)} names aliased]")

    # B — through the YAML tree, all names
    tmp = tempfile.mkdtemp(prefix="cfgkit_alias_")
    try:
        yamlio.write_tree(retail, tmp, aliases=stress)
        mB = yamlio.read_tree(tmp, aliases=stress)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    okB, detB = _identical(retail, mB)
    dB = canon.canon_diff(retail, mB)
    okB = okB and not dB
    print(f"  B tree round-trip  (all names)   : {'PASS' if okB else 'FAIL — ' + (detB or f'{len(dB)} canon diffs')}")
    for x in dB[:8]:
        print("       " + x)

    # C — bootstrap table through the tree
    tmp = tempfile.mkdtemp(prefix="cfgkit_alias_")
    try:
        yamlio.write_tree(retail, tmp, aliases=boot)
        mC = yamlio.read_tree(tmp, aliases=boot)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    okC, detC = _identical(retail, mC)
    print(f"  C bootstrap table through tree   : {'PASS' if okC else 'FAIL — ' + detC}"
          f"   [{len(boot)} names: text ids + ore]")

    # E — per-object layout through the tree. Folding splits the flat ICON section
    # across object files, which the game resolves by name (order-insensitive, see
    # Phase 4a canon + the Phase 2 boot test), so the bar here is canon-equality,
    # not byte-identity: only the cosmetic ICON emit order may move.
    tmp = tempfile.mkdtemp(prefix="cfgkit_alias_")
    try:
        yamlio.write_tree_per_object(retail, tmp, aliases=stress)
        mE = yamlio.read_tree(tmp, aliases=stress)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    dE = canon.canon_diff(retail, mE)
    okE = not dE and not mE.issues
    bit, blt = _emit_bytes(retail)
    eit, elt = _emit_bytes(mE)
    byte_moved = sorted({l.split()[0] for a, b in zip(bit.splitlines(), eit.splitlines())
                         if a != b for l in (a,) if l.split()})
    okE = okE and byte_moved in ([], ["ICON"])   # nothing but ICON order may move
    print(f"  E per-object layout (canon-equal): {'PASS' if okE else 'FAIL — ' + (f'{len(dE)} canon diffs' if dE else f'reordered {byte_moved}')}"
          f"   [byte reorder: {byte_moved or 'none'}]")
    for x in dE[:8]:
        print("       " + x)

    # D — discrimination + no dangling
    english = stress.to_english(retail)
    renamed = bool(canon.canon_diff(retail, english))     # names actually changed
    boot_errs, boot_info = boot.check(retail)
    okD = renamed and not boot_errs
    print(f"  D discrimination + clean table   : {'PASS' if okD else 'FAIL'}"
          f"   (renamed={renamed}, dangling/collision={len(boot_errs)})")
    for e in boot_errs[:8]:
        print("       " + e)

    ok = okA and okB and okC and okD and okE
    print("\ncoverage from bootstrap table:")
    for line in boot_info:
        print("   " + line)
    print("\nPHASE 4b CHECKPOINT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
