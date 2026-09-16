#!/usr/bin/env python3
"""lint_libmh_layering.py -- no libmh module TU names the HOST-ONLY original-address table.

NARROWED AT FORK F4D, and the narrowing IS the ruling F4D-PRE deferred here.

WHAT THIS FILE USED TO BE. LIB0's layering check (the endgame plan, tracker LIB0): a direct
include of a harness header (hook/, seams/, effects/, shadow/) from libmh module code had to sit
inside a conditional excluding MH_LIBMH_BUILD, and an unguarded one went red naming file:line.

WHY THAT HALF IS GONE. F4D-PRE replaced the rule it enforced. `tools/check_libmh_outbound.py`'s SRC
arm walks the include CLOSURE from all 627 roster TUs and fails a harness include GUARDED OR NOT --
because a guard hides the edge from a link-level reader while leaving it in the HOSTED build, which
is the configuration libmh.dll actually ships. Eight TUs used to comply with the old rule by each
writing its own `#else` stub; F4D-PRE deleted all eight and closed the edge onto one table.

So keeping this arm would have meant shipping a gate whose PASS MESSAGE STATES A RULE THAT IS NO
LONGER TRUE -- "a guarded harness include is fine" -- next to another gate that reds on exactly
that. A weaker check that never contradicts a stronger one is redundant; one that teaches the
opposite rule is worse than redundant. F4D-PRE kept it alive deliberately ("finer message,
independent selftest") only until the DLL existed to re-rule against, and it does now.

WHAT SURVIVES, AND WHY THE FILE IS NOT DELETED. The second claim was never about harness layering
at all and nothing else makes it:

  LIB-REF-LIVE -- `libref_host/host_stock_bases.gen.h` carries mh.exe's per-region ORIGINAL bases,
  so an importing HOST can re-stamp carried pointers into bound regions. It is a relocation
  reader's input, and the libmh ARTIFACT must never contain one (LIB-REF-SPLIT S5, enforced over
  the BUILT lib by tools/scan_libmh_vas.py's byte ratchet). No module TU may include it, mention it
  in a comment a later edit could uncomment, or reach it by any path -- and unlike a harness
  include there is no configuration in which one legitimately may, so a guard does NOT excuse it.

  The header also `#error`s without MH_LIBREF_HOST_TU, which only libref_host's TUs define. Belt
  AND braces: that is a BUILD-time refusal, this is a COMMIT-time one, and scan_libmh_vas is an
  ARTIFACT-time one. Three mechanisms, one claim, none of them able to see what the others do.

Matched anywhere on the line, not only in an #include.

`--selftest` proves both arms on inline fixtures.
"""

import argparse
import io
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MH_SRC = os.path.join(REPO, "src", "mh_dll", "libmh")
MODULES = ("sim", "ai", "orders", "tact", "save", "state", "lockstep")

HOST_ONLY_TABLE_RE = re.compile(r"host_stock_bases\.gen\.h")


def scan_text(relname, text):
    """-> [(relname, lineno, line)] for every mention of the host-only stock-base table."""
    bad = []
    for i, line in enumerate(text.splitlines(), 1):
        if HOST_ONLY_TABLE_RE.search(line):
            bad.append((relname, i, line.strip() + "   <- HOST-ONLY table, never in module code"))
    return bad


def scan_tree():
    bad = []
    for mod in MODULES:
        base = os.path.join(MH_SRC, mod)
        for root, _dirs, files in os.walk(base):
            for fn in sorted(files):
                if not fn.endswith((".cpp", ".h")):
                    continue
                path = os.path.join(root, fn)
                rel = os.path.relpath(path, REPO)
                bad.extend(scan_text(rel, io.open(path, encoding="utf-8", errors="replace").read()))
    return bad


def selftest():
    hostonly = '#include "host_stock_bases.gen.h"\n'
    hostguarded = '#ifndef MH_LIBMH_BUILD\n#include "host_stock_bases.gen.h"\n#endif\n'
    commented = "// host_stock_bases.gen.h is the host's, never ours\n"
    unrelated = '#include "sim/foo.h"\n#include "hook/promoted.h"\n'
    checks = [
        (
            "the HOST-ONLY stock-base table goes red in module code",
            len(scan_text("f", hostonly)) == 1,
        ),
        (
            "...and a guard does NOT excuse it (no configuration may reach it)",
            len(scan_text("f", hostguarded)) == 1,
        ),
        (
            "...and a COMMENT naming it goes red too (a later edit only has to uncomment it)",
            len(scan_text("f", commented)) == 1,
        ),
        (
            "a harness include is NOT this lint's business any more (check_libmh_outbound owns it,"
            " and reds it guarded or not)",
            len(scan_text("f", unrelated)) == 0,
        ),
    ]
    ok = True
    for name, passed in checks:
        print("  [%s] %s" % ("ok" if passed else "FAIL", name))
        ok = ok and passed
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true", help="prove both arms on inline fixtures")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    bad = scan_tree()
    if bad:
        print(
            "[lint_libmh_layering] FAIL: %d libmh module file(s) reference the HOST-ONLY "
            "stock-base table (libref_host/host_stock_bases.gen.h). It carries mh.exe's original "
            "per-region bases for an importing host to re-stamp with; the libmh artifact must "
            "never contain one, in any configuration:" % len(bad)
        )
        for rel, line_no, line in bad:
            print("  %s:%d: %s" % (rel, line_no, line))
        return 1
    print(
        "[lint_libmh_layering] OK: no libmh module names the host-only stock-base table "
        "(harness includes are check_libmh_outbound's, guarded or not)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
