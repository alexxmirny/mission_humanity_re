"""PT-CAP9-U2: no hard-coded 100-unit player stride in the harness or in mh.dll.

Since v0.2.3 the unit roster is [8][500] x 233 once relocated (mh::caps), so a literal `p * 100u` over
`mh_map_object_unit` records addresses the wrong record for every player but 0. Two shapes are rejected
in src/mh_dll/mh_harness and src/mh_dll/mh (the generated headers are exempt):

  1. `hash_base(g_idx_units)` -- the canonical MIRROR of the unit region. Reading it is correct for
     records below the vanilla window, but a write through it is lost and every walk past 100 misses
     the overflow, so units go through `live_unit_row(p)` / `MH_Caps_Rec(MH_CAPS_IDX_UNITS, p, i)`.
  2. a `mh_map_object_unit` pointer expression followed by `<name> * 100` (the vanilla row stride).

Buildings keep their `hash_base(g_idx_bldgs) + p * 100u` reads on purpose (PT-CAP8: the mirror is
vanilla-shaped); this lint is about units only.

  python tools/lint_unit_stride.py            # exit 0 = clean
  python tools/lint_unit_stride.py --selftest # planted offenders must be caught, clean lines must not
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOTS = [
    os.path.join(REPO, "src", "mh_dll", "mh_harness"),
    os.path.join(REPO, "src", "mh_dll", "mh"),
]
EXTS = (".cpp", ".h")

RULES = [
    (
        "hash_base(g_idx_units): the unit mirror is read-only and 100-wide",
        re.compile(r"hash_base\s*\(\s*g_idx_units\s*\)"),
    ),
    (
        "a mh_map_object_unit pointer plus `<index> * 100`: the vanilla row stride",
        re.compile(r"mh_map_object_unit\b[^;]{0,160}?\+\s*[A-Za-z_(][\w()\[\]>.\- ]*\*\s*100u?\b"),
    ),
]


def scan_text(text):
    """-> [(line, rule)] for a whole file, joined so a split-over-two-lines expression is still seen."""
    hits = []
    for rule, rx in RULES:
        for m in rx.finditer(text):
            hits.append((text.count("\n", 0, m.start()) + 1, rule))
    return sorted(hits)


def files():
    for root in ROOTS:
        for d, _, fs in os.walk(root):
            for f in fs:
                if f.endswith(EXTS) and ".gen." not in f:
                    yield os.path.join(d, f)


def main():
    bad = []
    for p in files():
        try:
            text = open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for line, rule in scan_text(text):
            bad.append("%s:%d: %s" % (os.path.relpath(p, REPO), line, rule))
    for b in bad:
        print(b)
    print("lint_unit_stride: %s (%d finding(s))" % ("FAIL" if bad else "PASS", len(bad)))
    return 1 if bad else 0


def selftest():
    fails = []

    def expect(name, text, n):
        got = len(scan_text(text))
        if got != n:
            fails.append("%s: wanted %d finding(s), got %d" % (name, n, got))

    expect("mirror base", "auto *u = (mh_map_object_unit *)hash_base(g_idx_units);", 1)
    expect(
        "stride",
        "auto *un = reinterpret_cast<mh::game::mh_map_object_unit *>(base) + p * 100u;",
        1,
    )
    expect(
        "stride, split over two lines",
        "reinterpret_cast<const mh::game::mh_map_object_unit *>(x) +\n    (uintptr_t)i * 100u;",
        1,
    )
    expect(
        "the building mirror is allowed",
        "reinterpret_cast<mh_map_object_building *>(hash_base(g_idx_bldgs)) + p * 100u;",
        0,
    )
    expect("the live row is allowed", "auto *un = live_unit_row(p); un[i].energy;", 0)
    expect(
        "an unrelated unit line", "mh_map_object_unit *u = live_unit_row(p); int x = n * 100;", 0
    )
    for f in fails:
        print("FAIL:", f)
    print("lint_unit_stride --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(selftest() if "--selftest" in sys.argv[1:] else main())
