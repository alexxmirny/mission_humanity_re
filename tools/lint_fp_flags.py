#!/usr/bin/env python3
"""lint_fp_flags.py -- every mh/sim and mh/ai TU is compiled x87, in BOTH vcxproj files.

FP-FLAGS (the endgame plan §1). The strategic sim and AI are x87 code:
the original binary uses the x87 stack (80-bit intermediates), and MSVC x86 emits SSE2 by default
(64-bit). A sim/AI body compiled SSE2 diverges from the original exactly where a long trajectory
compounds the rounding -- the reimplementation plan §6's central FP landmine. So every TU under mh/sim and
mh/ai must carry `/arch:IA32 /fp:precise` (EnableEnhancedInstructionSet=NoExtensions +
FloatingPointModel=Precise), and it must carry them in BOTH mh/mh.vcxproj (the DLL) AND
mh_nettest/mh_nettest.vcxproj (the offline oracle) -- a TU x87 in the game but SSE2 in the oracle
makes simtest/aitest agree with a body the game disagrees with.

The gap this closes was MEASURED 2026-09-01: 0 of 99 mh/ai TUs carried the flags, though
ai_construction_plan.cpp and ai_army_milestone.cpp reason explicitly about x87 extended precision.
(The planning census guessed 32 mh/sim TUs were also unflagged; direct measurement here found sim
already 356/356 -- the gap was exactly the 99 AI TUs, in both vcxproj.)

TWO PROJECTS ARE AUDITED PER-TU AND TWO ARE NOT, and the difference is how they set the flags.
mh.vcxproj and mh_nettest.vcxproj flag each ClCompile row individually, which is a hand-list and is
what the per-TU audit below exists for. libmh.vcxproj and (since fork F5I S2) libmh_test.vcxproj set
`EnableEnhancedInstructionSet=NoExtensions` + `FloatingPointModel=Precise` ONCE, in an
ItemDefinitionGroup covering every TU -- strictly stronger, and it cannot rot the way a hand-list
can. A per-TU audit of a blanket project would report every one of its sim/AI rows as MISSING, so
those two get the check that actually applies to them: the blanket settings are THERE. That check is
cheap and it is not decorative -- deleting the ItemDefinitionGroup line is exactly how a blanket
project silently becomes an SSE2 one, and nothing else in the tree reads it.

A TU may be EXEMPT only via tools/data/fp_integer_only.json -- a path -> reason map for bodies with
no floating-point operation at all, where the arch flag is genuinely a no-op. The exemption is a
recorded claim, not a default: a new sim/AI TU in neither the flagged set nor the exemption list
FAILS the check, so the landmine cannot re-enter silently.

Usage:
  python tools/lint_fp_flags.py            # check both vcxproj; nonzero on any unflagged TU
  python tools/lint_fp_flags.py --fix      # add the two settings to every sim/AI TU missing them
"""

import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL_VCX = os.path.join(REPO, "src", "mh_dll", "mh", "mh.vcxproj")
NET_VCX = os.path.join(REPO, "src", "mh_dll", "mh_nettest", "mh_nettest.vcxproj")
EXEMPT = os.path.join(REPO, "tools", "data", "fp_integer_only.json")

# The projects that set the two flags for the WHOLE project instead of per TU (see the header).
BLANKET_VCX = (
    os.path.join(REPO, "src", "mh_dll", "libmh", "libmh.vcxproj"),
    os.path.join(REPO, "src", "mh_dll", "libmh_test", "libmh_test.vcxproj"),
)
BLANKET_SETTINGS = (
    "<EnableEnhancedInstructionSet>NoExtensions</EnableEnhancedInstructionSet>",
    "<FloatingPointModel>Precise</FloatingPointModel>",
)

SETTINGS = (
    "      <EnableEnhancedInstructionSet>NoExtensions</EnableEnhancedInstructionSet>\n"
    "      <FloatingPointModel>Precise</FloatingPointModel>\n"
)


def load_exempt():
    if not os.path.exists(EXEMPT):
        return {}
    doc = json.load(open(EXEMPT, encoding="utf-8"))
    # {"paths": {"sim/foo.cpp": "reason", ...}}; keys are module-relative (sim/... or ai/...).
    return doc.get("paths", {})


def module_rel(include):
    """Normalise a ClCompile Include to a module-relative sim/... or ai/... path, or None.

    BOTH PREFIXES, and the second one is why this gate still measures anything. Fork F5O moved
    the roster from mh/ to libmh/, so mh_nettest spells its rows `..\\libmh\\sim\\x.cpp`; a
    normaliser that only stripped `../mh/` would return None for every one of them and the
    check would report `0 flagged, 0 exempt, 0 MISSING` and exit 0 -- a silently emptied gate,
    which is the exact failure mode the FP landmine makes expensive.
    """
    p = include.replace("\\", "/")
    p = p.replace("../libmh/", "").replace("../mh/", "").lstrip("./")
    if p.startswith("sim/") or p.startswith("ai/"):
        return p
    return None


# One ClCompile element, self-closing OR a block, captured whole so --fix can rewrite it in place.
CLC_RE = re.compile(
    r'<ClCompile Include="(?P<inc>[^"]+)"\s*(?P<body>/>|>(?P<inner>.*?)</ClCompile>)',
    re.S,
)


def audit(path, exempt):
    """Return (missing, exempted, flagged) module-rel paths for the sim/AI TUs in `path`."""
    src = open(path, encoding="utf-8").read()
    missing, exempted, flagged = [], [], []
    for m in CLC_RE.finditer(src):
        rel = module_rel(m.group("inc"))
        if rel is None:
            continue
        has = m.group("body") != "/>" and "FloatingPointModel" in (m.group("inner") or "")
        if has:
            flagged.append(rel)
        elif rel in exempt:
            exempted.append(rel)
        else:
            missing.append(rel)
    return missing, exempted, flagged


def fix(path, exempt):
    src = open(path, encoding="utf-8").read()
    added = []

    def repl(m):
        rel = module_rel(m.group("inc"))
        if rel is None or rel in exempt:
            return m.group(0)
        inner = m.group("inner") or ""
        if m.group("body") != "/>" and "FloatingPointModel" in inner:
            return m.group(0)  # already flagged
        added.append(rel)
        inc = m.group("inc")
        if m.group("body") == "/>":
            return f'<ClCompile Include="{inc}">\n{SETTINGS}    </ClCompile>'
        # A block WITHOUT the settings: insert them right after the open tag.
        return f'<ClCompile Include="{inc}">\n{SETTINGS}{inner}</ClCompile>'

    out = CLC_RE.sub(repl, src)
    if out != src:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(out)
    return added


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fix", action="store_true", help="add the flags to every unflagged sim/AI TU")
    args = ap.parse_args()
    exempt = load_exempt()

    if args.fix:
        total = 0
        for path in (DLL_VCX, NET_VCX):
            added = fix(path, exempt)
            total += len(added)
            print(f"lint_fp_flags: {os.path.basename(path)} -- flagged {len(added)} TU(s)")
        print(f"lint_fp_flags: added flags to {total} entr(y/ies)")
        return 0

    bad = 0
    checked = 0
    for path in (DLL_VCX, NET_VCX):
        missing, exempted, flagged = audit(path, exempt)
        checked += len(flagged) + len(exempted) + len(missing)
        base = os.path.basename(path)
        print(
            f"lint_fp_flags: {base} -- {len(flagged)} flagged, {len(exempted)} exempt, "
            f"{len(missing)} MISSING"
        )
        for rel in missing:
            print(
                f"  MISSING /arch:IA32 /fp:precise: {rel} ({base}) -- a sim/AI TU must be x87 "
                f"(reimpl-plan §6) or listed in tools/data/fp_integer_only.json with a reason"
            )
            bad += 1
    if bad:
        print(f"lint_fp_flags: {bad} unflagged sim/AI TU(s) -- run tools/lint_fp_flags.py --fix")
        return 1
    # A NON-VACUITY FLOOR, added at fork F5O. mh.vcxproj's arm has measured 0 TUs since F4D
    # (it compiles no roster TU any more) and the whole population is mh_nettest's, so a
    # normaliser that stopped matching would leave this printing success over an empty scan.
    if checked < 400:
        print(
            "lint_fp_flags: FAIL -- only %d sim/AI TU(s) were classified at all. The scan is\n"
            "  broken (a ClCompile Include spelling module_rel() does not normalise), not the\n"
            "  tree clean." % checked
        )
        return 1
    # The blanket projects, checked as wholes (see the header).
    for path in BLANKET_VCX:
        base = os.path.basename(path)
        if not os.path.exists(path):
            print(
                "lint_fp_flags: FAIL -- %s is missing; the blanket arm has nothing to read" % base
            )
            return 1
        src = open(path, encoding="utf-8-sig").read()
        gone = [w for w in BLANKET_SETTINGS if w not in src]
        if gone:
            print(
                "lint_fp_flags: FAIL -- %s no longer sets %s for the whole project. It compiles the\n"
                "  roster, so dropping this is the SSE2 landmine for every sim/AI TU in it at once."
                % (base, " and ".join(gone))
            )
            return 1
        print("lint_fp_flags: %s -- blanket /arch:IA32 /fp:precise (whole project)" % base)
    print(
        "lint_fp_flags: every sim and ai TU is x87 in both per-TU vcxproj (%d classified), and "
        "%d blanket project(s) set it whole" % (checked, len(BLANKET_VCX))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
