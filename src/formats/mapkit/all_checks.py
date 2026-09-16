"""Run all mapkit checkpoints; non-zero exit on any regression.

  python -m src.formats.mapkit.all_checks

Sequences check (binary-codec byte-identity) then roundtrip (Tiled fidelity +
mutation). The one-command regression gate — run green before any commit."""
from __future__ import annotations

import sys

from . import check, roundtrip


def main(argv=None):
    rc = 0
    for name, mod in (("check", check), ("roundtrip", roundtrip)):
        print(f"=== {name} ===")
        r = mod.main([])
        rc = rc or r
    print("=== all_checks:", "PASS" if rc == 0 else "FAIL", "===")
    return rc


if __name__ == "__main__":
    sys.exit(main())
