"""check_roster_caps.py -- no PRODUCTION index expression uses a compile-time roster cap (SB-BIND T2).

THE DEFECT THIS KEEPS CLOSED. The eight per-player roster capacities used to be `inline constexpr`
values duplicated across five module headers. T2 replaced every production use with the DERIVED
capacity carried on each module's view/store (`v.caps.units`, `caps_.buildings`, ...), so a host
that binds a 500-cap roster is followed rather than mis-indexed. The constants survive only as the
offline fixtures' default and as documentation of the stock strides.

WHY A RATCHET AT ZERO RATHER THAN A FLAT BAN. `src/mh_dll/libmh_test/` (and `mh_nettest/`)
legitimately use the
constants -- its fixtures allocate 8*100 vectors and index them, and several declare real C arrays
(`uint8_t flags[BUILDINGS_PER_PLAYER]`) that could not be runtime-sized without changing the test.
So the rule is scoped to the MODULE tree, where the count is 0 and must stay 0.

WHY THIS MATTERS MORE THAN IT LOOKS. A single re-introduced site is worse than the pre-T2 state:
before, every site said 100 and the whole module was uniformly wrong on a cap-raised host; after,
one stale site disagrees with its neighbours about where player p's row starts, and a read and a
write of the same (player, slot) land in different records. Uniform staleness is at least
diagnosable; a mixed roster is not.

Usage:
  python tools/check_roster_caps.py            # the gate
  python tools/check_roster_caps.py --selftest # prove the scanner still fires
"""

import argparse
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _dllsrc  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULE_ROOT = os.path.join(REPO, "src", "mh_dll", "mh")

# The eight caps, plus the one TU-local alias. Matched with or without a namespace qualifier.
CAPS = (
    "UNITS_PER_PLAYER",
    "BUILDINGS_PER_PLAYER",
    "SOLDIERS_PER_PLAYER",
    "STORAGE_PER_PLAYER",
    "UNIT_STORAGE_PER_PLAYER",
    "TURRETS_PER_PLAYER",
    "PRODUCTIONS_PER_PLAYER",
    "LABS_PER_PLAYER",
    "MINES_PER_PLAYER",
)
RE_CAP = re.compile(r"\b(?:mh::(?:sim|ai|orders)::(?:issue::)?)?(" + "|".join(CAPS) + r")\b")

# The definition itself and the stock-stride asserts are the sanctioned mentions.
RE_EXEMPT_LINE = re.compile(r"inline constexpr int32_t|static_assert|// CAPS-OK\b")


def strip_comments(text):
    """Blank comment content, keeping line structure -- the same treatment check_sim_addresses uses,
    and for the same reason: these headers quote the caps in prose constantly."""
    out = []
    in_block = False
    for line in text.splitlines():
        buf = []
        i = 0
        while i < len(line):
            if in_block:
                j = line.find("*/", i)
                if j < 0:
                    i = len(line)
                    break
                in_block = False
                i = j + 2
                continue
            if line.startswith("//", i):
                break
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            buf.append(line[i])
            i += 1
        out.append("".join(buf))
    return out


def scan_tree(root=None):
    """[(repo_rel, line_no, cap, text)] for every production use of a compile-time cap."""
    roots = _dllsrc.ROOTS if root is None else (root,)
    found = []
    for dirpath, _dirs, files in _dllsrc.walk_flat(roots):
        for name in sorted(files):
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
            raw = text.splitlines()
            for i, code in enumerate(strip_comments(text), 1):
                if RE_EXEMPT_LINE.search(raw[i - 1]):
                    continue
                m = RE_CAP.search(code)
                if m:
                    # relpath, but tolerant: --selftest's temp dir can land on a different drive
                    # from the repo, which makes os.path.relpath raise rather than return a path.
                    try:
                        rel = os.path.relpath(path, REPO).replace(os.sep, "/")
                    except ValueError:
                        rel = path.replace(os.sep, "/")
                    found.append((rel, i, m.group(1), raw[i - 1].strip()))
    return found


def selftest():
    fails = 0
    with tempfile.TemporaryDirectory(prefix="mh_rostercaps_") as wd:
        # A clean file -- the derived form -- must pass, or a scanner that flagged everything could
        # report green too.
        open(os.path.join(wd, "ok.cpp"), "w", encoding="utf-8").write(
            "namespace mh::sim { int f(const sim_view &v) { return v.units[3 * v.caps.units]; } }\n"
        )
        if scan_tree(wd):
            print("  FAIL: the derived form (v.caps.units) was reported as a violation")
            fails += 1
        else:
            print("  ok: the derived form passes")

        # The definition line stays legal.
        os.remove(os.path.join(wd, "ok.cpp"))
        open(os.path.join(wd, "def.h"), "w", encoding="utf-8").write(
            "inline constexpr int32_t UNITS_PER_PLAYER = mh::state::STOCK_ROSTER_CAPS.units;\n"
        )
        if scan_tree(wd):
            print("  FAIL: the definition itself was reported as a violation")
            fails += 1
        else:
            print("  ok: the surviving definition is exempt")

        # ... and so does a stock-stride assert.
        os.remove(os.path.join(wd, "def.h"))
        open(os.path.join(wd, "assert.cpp"), "w", encoding="utf-8").write(
            "static_assert(UNITS_PER_PLAYER * sizeof(unit) == 0x5b04u, \"stock stride\");\n"
        )
        if scan_tree(wd):
            print("  FAIL: a stock-stride static_assert was reported as a violation")
            fails += 1
        else:
            print("  ok: a stock-stride assert is exempt")

        # A comment mentioning the cap is documentation, not a binding.
        os.remove(os.path.join(wd, "assert.cpp"))
        open(os.path.join(wd, "comment.cpp"), "w", encoding="utf-8").write(
            "/*\n  units is [MAX_PLAYERS][UNITS_PER_PLAYER], row-major\n*/\nint f() { return 0; }\n"
        )
        if scan_tree(wd):
            print("  FAIL: a cap named inside a block comment was reported as a violation")
            fails += 1
        else:
            print("  ok: a cap named in a comment is skipped")

        # THE ARM WITH TEETH: a re-introduced production use is caught, by name.
        os.remove(os.path.join(wd, "comment.cpp"))
        open(os.path.join(wd, "bad.cpp"), "w", encoding="utf-8").write(
            "namespace mh::sim { int f(const sim_view &v) { return v.units[3 * UNITS_PER_PLAYER]; } }\n"
        )
        hits = scan_tree(wd)
        if not any(cap == "UNITS_PER_PLAYER" for _, _, cap, _ in hits):
            print("  FAIL: a re-introduced compile-time cap was NOT caught")
            fails += 1
        else:
            print("  ok: a re-introduced compile-time cap is caught by name")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        print("=== check_roster_caps --selftest ===")
        fails = selftest()
        print("%d failure(s)" % fails)
        return 1 if fails else 0

    found = scan_tree()
    if found:
        print(
            "=== check_roster_caps: %d compile-time roster cap(s) in module code ===" % len(found)
        )
        for rel, ln, cap, text in found:
            print("  %s:%d  %s" % (rel, ln, cap))
            print("      %s" % text[:140])
        print(
            "  Index with the DERIVED capacity instead -- `v.caps.<roster>` on a view, `caps_.<roster>`\n"
            "  inside a store. The constants are the STOCK values and are for fixtures only; a single\n"
            "  stale site disagrees with its neighbours about where a player's row starts."
        )
        return 1
    print(
        "check_roster_caps: ok -- no compile-time roster cap is used in src/mh_dll/mh "
        "(every production index reads the host-bound capacity)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
