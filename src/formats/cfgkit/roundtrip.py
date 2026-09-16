"""Phase 2 checkpoint: round-trip fidelity + boot-test file generation.

Gates (all must hold on retail):
  A. emitter:    parse -> emit -> re-parse            canon== retail, 0 issues
  B. dict layer: parse -> decompile -> json -> compile canon== retail
  C. full chain: parse -> decompile -> compile -> emit -> re-parse
                                                       canon== retail, 0 issues

With ``--write <overlay_dir>`` it also emits the full-chain result as
``<dir>/mh/init/INIT.CFG`` + ``<dir>/mh_ex/init/initlang.cfg`` — exactly
build_mod.py's ``--overlay`` shape — so a *recompiled retail* config can be
installed and booted. That boot is the empirical test of the flattened
section-order emit (see emit.py): if the game runs strategic + a tactical
mission on it, cross-class ordering is confirmed irrelevant.

Usage:
  python -m src.formats.cfgkit.roundtrip [--init ...] [--lang ...] [--write DIR]
"""
from __future__ import annotations

import argparse
import json
import os
import sys

from . import canon, decompile, emit
from ._paths import INIT_DEFAULT, LANG_DEFAULT
from .parse import parse_files, parse_text, _feed


def _reparse(init_text, lang_text):
    m = parse_text(init_text, "emit:init")
    _feed(m, lang_text, "emit:lang")
    return m


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--init", default=INIT_DEFAULT)
    ap.add_argument("--lang", default=LANG_DEFAULT)
    ap.add_argument("--write", metavar="OVERLAY_DIR",
                    help="also emit the full-chain cfg into this overlay tree")
    args = ap.parse_args(argv)

    retail = parse_files(args.init, args.lang)

    # A — emitter
    it, lt = emit.emit(retail)
    mA = _reparse(it, lt)
    dA = canon.canon_diff(retail, mA)
    okA = not dA and not mA.issues

    # B — dict layer through JSON
    mB = decompile.compile_(json.loads(json.dumps(decompile.decompile(retail))))
    dB = canon.canon_diff(retail, mB)
    okB = not dB

    # C — full chain
    it2, lt2 = emit.emit(mB)
    mC = _reparse(it2, lt2)
    dC = canon.canon_diff(retail, mC)
    okC = not dC and not mC.issues

    def report(tag, ok, diff, issues=None):
        print(f"  {tag}: {'PASS' if ok else 'FAIL'}"
              + (f"  (canon diffs: {len(diff)})" if diff else "")
              + (f"  (issues: {len(issues)})" if issues else ""))
        for x in (diff or [])[:10]:
            print("     " + x)

    print("=== Phase 2 round-trip gates ===")
    report("A emitter          ", okA, dA, mA.issues)
    report("B decompile/compile", okB, dB)
    report("C full chain       ", okC, dC, mC.issues)

    if args.write:
        ip = os.path.join(args.write, "mh", "init", "INIT.CFG")
        lp = os.path.join(args.write, "mh_ex", "init", "initlang.cfg")
        emit.write_files(mB, ip, lp)
        print(f"\nwrote recompiled cfg:\n  {ip}\n  {lp}")

    ok = okA and okB and okC
    print("\nPHASE 2 CHECKPOINT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
