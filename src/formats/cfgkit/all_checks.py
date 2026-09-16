"""Run every cfgkit checkpoint in order and fail if any regresses.

One command to verify the whole toolchain against the verified-pristine retail
config (parse -> validate -> round-trip fidelity -> the English alias layer):

  python -m src.formats.cfgkit.all_checks

Each stage is the module's own ``main`` (same defaults, same gates); this just
sequences them and returns non-zero if any fails, so it doubles as the repo's
regression test for the cfg pipeline.
"""
from __future__ import annotations

import sys

from . import alias_check, check, roundtrip, validate

STAGES = [
    ("Phase 1  parse + census", check.main),
    ("Phase 3  static validation", validate.main),
    ("Phase 2  round-trip fidelity", roundtrip.main),
    ("Phase 4b English alias layer", alias_check.main),
]


def main(argv=None):
    results = []
    for title, fn in STAGES:
        print("\n" + "=" * 70)
        print(f"### {title}")
        print("=" * 70)
        rc = fn([])
        results.append((title, rc == 0))

    print("\n" + "=" * 70)
    print("### cfgkit checkpoint summary")
    print("=" * 70)
    for title, ok in results:
        print(f"  {'PASS' if ok else 'FAIL':4}  {title}")
    all_ok = all(ok for _, ok in results)
    print("\nALL CFGKIT CHECKS:", "PASS" if all_ok else "FAIL")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
