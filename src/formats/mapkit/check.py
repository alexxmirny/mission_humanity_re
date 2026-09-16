"""Checkpoint: parse every shipped .MP/.TLO, assert emit(parse(f)) == f byte-identical.

  python -m src.formats.mapkit.check [--clean DIR]

The binary-codec fidelity gate (cfgkit's check.py analog). Prints a census with a
hardcoded EXPECTED tripwire so a miscount regresses loudly."""
from __future__ import annotations

import argparse
import glob
import os
import sys

from . import strategic, tactical, tlo
from .common import CLEAN

EXPECTED = {"mp": 31, "tlo": 9, "map": 3}


def _roundtrip(path, parse):
    with open(path, "rb") as fh:
        original = fh.read()
    got = parse.emit(parse.parse(path))
    return got == original, len(original), len(got)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clean", default=CLEAN)
    args = ap.parse_args(argv)
    mh = os.path.join(args.clean, "mh")

    fails = 0
    counts = {"mp": 0, "tlo": 0, "map": 0}
    for path in sorted(glob.glob(os.path.join(mh, "*.MP"))):
        counts["mp"] += 1
        ok, a, b = _roundtrip(path, strategic)
        if not ok:
            fails += 1
            print(f"  FAIL .MP  {os.path.basename(path)}  ({a} -> {b} bytes)")
    for path in sorted(glob.glob(os.path.join(mh, "*.TLO"))):
        counts["tlo"] += 1
        ok, a, b = _roundtrip(path, tlo)
        if not ok:
            fails += 1
            print(f"  FAIL .TLO {os.path.basename(path)}  ({a} -> {b} bytes)")
    for path in sorted(glob.glob(os.path.join(mh, "ALIEN_*.MAP"))):
        counts["map"] += 1
        ok, a, b = _roundtrip(path, tactical)
        if not ok:
            fails += 1
            print(f"  FAIL .MAP {os.path.basename(path)}  ({a} -> {b} bytes)")

    print(f"census: {counts['mp']} .MP, {counts['tlo']} .TLO, {counts['map']} .MAP")
    census_bad = counts != EXPECTED
    if census_bad:
        print(f"  WARNING census != EXPECTED {EXPECTED}")
    if fails:
        print(f"RESULT: {fails} byte-identity FAILURES")
        return 1
    print("RESULT: all files round-trip byte-identical" +
          ("  (census mismatch)" if census_bad else ""))
    return 1 if census_bad else 0


if __name__ == "__main__":
    sys.exit(main())
