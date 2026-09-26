#!/usr/bin/env python3
"""lint_rig_ini_defaults.py -- the rig's [net] defaults must match the shipped example ini, or say why not.

THE TRAP THIS CLOSES (tooling:TL-SUITE-INIMERGE, shaped like tooling:TL-RIG-DEFANG / dead-ends G274):
tools/ui_test.py's NET_BLOCK hardcodes a [net] default for every peer this rig launches. For two years
it pinned `defang_overlay=1` while the shipped example ini had moved to `0` -- nothing compared the
two, so every multi-peer suite row silently ran the wrong barrier-frame instrument under a green
suite the whole time (caught only by a field repro, mp:P9). Nothing else stops a second such drift
from sitting unnoticed the same way.

This reads ui_test.py's NET_BLOCK (the rig's own live default for every key it hardcodes, with
DEFANG_OVERLAY folded in exactly as make_ini folds it) and `src/mh_dll/mh_net.example.ini`'s [net]
section, through the SAME GetPrivateProfile-first-match model ui_test.ini_effective uses, and reds on
any key BOTH sides actually set to a DIFFERENT value -- unless the mismatch is declared, with a
reason, in tools/data/rig_ini_default_exceptions.json. A key the shipped ini leaves commented out or
absent is OUT OF SCOPE: there is nothing live to diff a hardcoded rig default against without
extracting the DLL's own compiled default, which is a different (and much larger) job.

    python tools/lint_rig_ini_defaults.py             # the gate
    python tools/lint_rig_ini_defaults.py --list      # print every compared key + both values
    python tools/lint_rig_ini_defaults.py --selftest  # a planted undeclared mismatch goes RED
"""

import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

EXAMPLE_INI = os.path.join(REPO, "src", "mh_dll", "mh_net.example.ini")
EXCEPTIONS = os.path.join(REPO, "tools", "data", "rig_ini_default_exceptions.json")


def load_exceptions(path=EXCEPTIONS):
    if not os.path.isfile(path):
        return {}
    with open(path, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    return {k: v for k, v in doc.items() if k != "_meta"}


# [net] keys read as a raw STRING (GetPrivateProfileStringA), where a same-line `;comment` IS the
# value (dead-ends G249) -- so trimming one here would be wrong the same way an untrimmed read is
# wrong for a NUMERIC key. Every other NET_BLOCK key is read as an int/float (atoi-style, via
# GetPrivateProfileIntA), which stops at the first non-digit regardless of a trailing comment. Keep
# this in sync with NET_BLOCK if a new STRING key joins it (`relay`, `transport`, `module` are string
# keys too, but none of them live in NET_BLOCK today).
STRING_KEYS = {"host"}


def _int_value(v):
    """Trim a same-line `;comment` the way GetPrivateProfileIntA (atoi) reads it -- everything from
    the DIGITS on is the value, so the comment every key in the example ini is documented with is
    not part of what the game actually sees for a NUMERIC key. NOT applied to STRING_KEYS."""
    return v.split(";", 1)[0].strip()


def rig_net_defaults(ui_test_mod):
    """{key: value} for every [net] key tools/ui_test.py's NET_BLOCK hardcodes -- DEFANG_OVERLAY
    folded in exactly the way make_ini folds it (it replaces NET_BLOCK's own baked-in default)."""
    out = {}
    for section, lines in ui_test_mod.ini_split_sections(ui_test_mod.NET_BLOCK):
        for ln in lines:
            k, _, v = ln.partition("=")
            k = k.strip()
            if k == "defang_overlay":
                out[k] = str(ui_test_mod.DEFANG_OVERLAY)
            elif k in STRING_KEYS:
                out[k] = v.strip()
            else:
                out[k] = _int_value(v)
    return out


def ship_net_defaults(ini_text, ui_test_mod, keys):
    """{key: value}, read from `ini_text`'s [net] section with the SAME first-match model the game
    itself uses (ui_test.ini_effective) -- restricted to `keys`. A key the ship ini leaves commented
    out or absent is left OUT of the result, not reported as a mismatch against None: see the module
    docstring for why that is out of scope rather than a green result on a real gap."""
    out = {}
    for k in keys:
        v = ui_test_mod.ini_effective(ini_text, "net", k)
        if v is None:
            continue
        out[k] = v.strip() if k in STRING_KEYS else _int_value(v)
    return out


def check(ini_text=None, exceptions=None, ui_test_mod=None):
    """(offenders, compared). offenders: [(key, rig_value, ship_value)] for undeclared mismatches.
    compared: {key: (rig_value, ship_value)} for every key both sides actually set."""
    if ui_test_mod is None:
        import ui_test as ui_test_mod
    if ini_text is None:
        with open(EXAMPLE_INI, "r", encoding="utf-8") as fh:
            ini_text = fh.read()
    if exceptions is None:
        exceptions = load_exceptions()
    rig = rig_net_defaults(ui_test_mod)
    ship = ship_net_defaults(ini_text, ui_test_mod, rig.keys())
    offenders = []
    compared = {}
    for k in sorted(ship):
        rig_v, ship_v = rig[k], ship[k]
        compared[k] = (rig_v, ship_v)
        if rig_v != ship_v and k not in exceptions:
            offenders.append((k, rig_v, ship_v))
    return offenders, compared


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true", help="print every compared key + both values")
    ap.add_argument("--selftest", action="store_true", help="planted undeclared mismatch goes RED")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest()

    offenders, compared = check()
    exceptions = load_exceptions()
    if args.list:
        for k in sorted(compared):
            rig_v, ship_v = compared[k]
            if rig_v == ship_v:
                tag = "same"
            elif k in exceptions:
                tag = "EXCEPT: %s" % exceptions[k][:70]
            else:
                tag = "MISMATCH"
            print("  [net] %-22s rig=%-14s ship=%-14s %s" % (k, rig_v, ship_v, tag))
    if offenders:
        print("RED: undeclared rig/ship [net] default mismatch(es):")
        for k, rig_v, ship_v in offenders:
            print(
                "  [net] %s: rig default %r != shipped example %r -- declare it (with a reason) in "
                "%s, or fix whichever side is stale." % (k, rig_v, ship_v, EXCEPTIONS)
            )
        return 1
    print(
        "lint_rig_ini_defaults: clean (%d key(s) compared, %d declared exception(s))"
        % (len(compared), len(exceptions))
    )
    return 0


class _FakeUiTest:
    """A minimal stand-in for the ui_test module, for the selftest -- so the planted cases do not
    depend on NET_BLOCK's real, moving key set. Reuses the real ini_split_sections/ini_effective
    (the thing under test is THIS tool's comparison, not the parser)."""

    def __init__(self, net_kv, defang_overlay=0):
        import ui_test as _real

        self.NET_BLOCK = "[net]\n" + "".join("%s=%s\n" % kv for kv in net_kv.items())
        self.DEFANG_OVERLAY = defang_overlay
        self.ini_split_sections = _real.ini_split_sections
        self.ini_effective = _real.ini_effective


def _report(name, cond):
    print("   %-72s %s" % (name, "ok" if cond else "FAIL"))
    return cond


def run_selftest():
    ok = True

    offenders, _ = check(
        ini_text="[net]\nport=6501\n",
        exceptions={},
        ui_test_mod=_FakeUiTest({"port": "6501"}),
    )
    ok = _report("clean match -> no offenders", not offenders) and ok

    offenders, _ = check(
        ini_text="[net]\nport=6600\n",
        exceptions={},
        ui_test_mod=_FakeUiTest({"port": "6501"}),
    )
    ok = (
        _report(
            "planted undeclared mismatch -> RED",
            len(offenders) == 1 and offenders[0] == ("port", "6501", "6600"),
        )
        and ok
    )

    offenders, _ = check(
        ini_text="[net]\nport=6600\n",
        exceptions={"port": "rig binds a per-lane port; the example ini's is illustrative"},
        ui_test_mod=_FakeUiTest({"port": "6501"}),
    )
    ok = _report("same mismatch, DECLARED with a reason -> clean", not offenders) and ok

    offenders, _ = check(
        ini_text="[net]\n; lockstep_step_ms=100\n",
        exceptions={},
        ui_test_mod=_FakeUiTest({"lockstep_step_ms": "30"}),
    )
    ok = (
        _report(
            "ship leaves the key commented out -> out of scope, not a mismatch (see docstring)",
            not offenders,
        )
        and ok
    )

    # the real files, sanity-checked against the committed exceptions: must be clean at HEAD.
    real_offenders, real_compared = check()
    ok = (
        _report(
            "the real rig vs the real shipped ini is clean at HEAD (%d compared)"
            % len(real_compared),
            not real_offenders,
        )
        and ok
    )

    print("lint_rig_ini_defaults --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
