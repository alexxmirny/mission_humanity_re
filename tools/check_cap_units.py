#!/usr/bin/env python3
"""
tools/check_cap_units.py -- patching:PT-CAP9-U1/U2: the ON smoke of the v0.2.3 units + soldiers raise.

The stimulus is the harness unit-fill fixture (`capu_*` keys, capu_tick in mh_harness/harness.cpp): with libmh.dll ABSENT and
`[caps] raise=1` (tools/uiscripts/ini/caps_raise.ini) one player creates units through the game's own debug order 0xeb until it
owns capu_units live units (every unit carries crew soldiers, so the soldier roster fills too), the match runs on, and the harness
saves and reloads the world (`cap_save_at`) through the game's own game_SaveGame / llm_game_load.

Clauses (all must hold):
  1  mh_net.log: `[caps] units relocation APPLIED`, `[caps] limits ON (` naming units 500 and soldiers 500; no `[caps] ... REFUSED`.
  2  mh_harness.log: `; CAPU VERDICT ok=1`, no `; CAPU FAIL`, the START line shows the wide rows (units 500, soldiers 500).
  3  the verdict reached --units live units (default 480), the highest unit index is past the vanilla window (> 100), and the
     soldier census is past the vanilla 100 (the crew records live in the raised part).
  4  the game kept stepping after the verdict (a later `; CAPU ... SAMPLE`) and no `*** DESYNC` / crash banner is in either log.
  5  `[caps] mirror_check ARMED` and no writer report (`mirror_check: array ... changed`).
  6  the save/load round trip: `; CAP SAVE ... rc=1` and `; CAP LOAD ... rc=1` (1 = the game call succeeded) carry the same overflow digest, and the `; CAPU SAVEUNITS
     before` / `after` census lines are equal.

  python tools/check_cap_units.py [--units N] <run-dir> [<run-dir> ...]
  python tools/check_cap_units.py --selftest
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402
import check_storage_purge  # noqa: E402

STAMP = re.compile(r"^\[[\d:.]+\]\s*")
VERDICT_RE = re.compile(
    r"; CAPU VERDICT ok=1 step=(?P<step>\d+) units=(?P<units>\d+)\(hi (?P<uhi>\d+)\) soldiers=(?P<sol>\d+)\(hi (?P<shi>\d+)\)"
)
START_RE = re.compile(r"; CAPU step=\d+ START .* row units=(?P<u>\d+) soldiers=(?P<s>\d+)")
SAMPLE_RE = re.compile(r"; CAPU step=(?P<step>\d+) SAMPLE ")
CENSUS_RE = re.compile(
    r"; CAPU SAVEUNITS (?P<when>before|after) step=\d+ units=(?P<u>\d+)\(hi (?P<uh>\d+)\) soldiers=(?P<s>\d+)\(hi (?P<sh>\d+)\)"
)
SAVE_RE = re.compile(
    r"; CAP (?P<what>SAVE|LOAD) step=\d+ .*?rc=(?P<rc>-?\d+).*?digest=(?P<d>[0-9A-Fa-f]+)"
)
LIMITS_RE = re.compile(r"\[caps\] limits ON \(.*units (?P<u>\d+), soldiers (?P<s>\d+)")


def strip(lines):
    return [STAMP.sub("", ln.strip()) for ln in lines]


def problems(net, harness, units_min=480):
    net, harness = strip(net), strip(harness)
    bad = []
    if not any("[caps] units relocation APPLIED" in ln for ln in net):
        bad.append("clause 1: no `[caps] units relocation APPLIED` in mh_net.log")
    lim = [m for m in (LIMITS_RE.search(ln) for ln in net) if m]
    if not lim:
        bad.append("clause 1: no `[caps] limits ON (... units N, soldiers M` line")
    elif (int(lim[-1].group("u")), int(lim[-1].group("s"))) != (500, 500):
        bad.append(
            "clause 1: limits ON names units %s / soldiers %s, expected 500 / 500"
            % lim[-1].groups()
        )
    bad += ["clause 1: " + ln for ln in net if "[caps]" in ln and "REFUSED" in ln]
    bad += ["clause 2: " + ln for ln in harness if ln.startswith("; CAPU FAIL")]
    start = [m for m in (START_RE.match(ln) for ln in harness) if m]
    if not start:
        bad.append("clause 2: no `; CAPU ... START` line")
    elif (int(start[0].group("u")), int(start[0].group("s"))) != (500, 500):
        bad.append(
            "clause 2: START rows units=%s soldiers=%s, expected the wide 500 / 500"
            % start[0].groups()
        )
    v = None
    for ln in harness:
        m = VERDICT_RE.match(ln)
        if m:
            v = {k: int(x) for k, x in m.groupdict().items()}
    if v is None:
        bad.append(
            "clause 2: no `; CAPU VERDICT ok=1` (the fixture never reached its target, or the process died)"
        )
        return bad
    if v["units"] < units_min:
        bad.append("clause 3: %d live units < the asked %d" % (v["units"], units_min))
    if v["uhi"] <= 100:
        bad.append("clause 3: highest unit index %d is inside the vanilla window" % v["uhi"])
    if v["sol"] <= 100 or v["shi"] <= 100:
        bad.append(
            "clause 3: soldiers %d (hi %d) never passed the vanilla 100" % (v["sol"], v["shi"])
        )
    later = [int(m.group("step")) for m in (SAMPLE_RE.match(ln) for ln in harness) if m]
    if not later or max(later) <= v["step"]:
        bad.append(
            "clause 4: no SAMPLE after the verdict step %d (the game stopped stepping)" % v["step"]
        )
    bad += ["clause 4: " + ln for ln in net + harness if "*** DESYNC" in ln]
    if not any("[caps] mirror_check ARMED" in ln for ln in net):
        bad.append("clause 5: no `[caps] mirror_check ARMED` banner")
    bad += ["clause 5: " + ln for ln in net if "[caps] mirror_check:" in ln and "changed" in ln]
    sv = {m.group("what"): m for m in (SAVE_RE.search(ln) for ln in harness) if m}
    cen = {m.group("when"): m for m in (CENSUS_RE.match(ln) for ln in harness) if m}
    if "SAVE" not in sv or "LOAD" not in sv:
        bad.append("clause 6: no `; CAP SAVE` / `; CAP LOAD` pair (cap_save_at never fired)")
    else:
        if sv["SAVE"].group("rc") != "1" or sv["LOAD"].group("rc") != "1":
            bad.append(
                "clause 6: save rc=%s load rc=%s" % (sv["SAVE"].group("rc"), sv["LOAD"].group("rc"))
            )
        if sv["SAVE"].group("d").upper() != sv["LOAD"].group("d").upper():
            bad.append(
                "clause 6: overflow digest %s -> %s across the round trip"
                % (sv["SAVE"].group("d"), sv["LOAD"].group("d"))
            )
    if "before" not in cen or "after" not in cen:
        bad.append("clause 6: no `; CAPU SAVEUNITS before/after` pair")
    else:
        b, a = cen["before"].groupdict(), cen["after"].groupdict()
        b.pop("when")
        a.pop("when")
        if a != b:
            bad.append("clause 6: census %s -> %s across the round trip" % (b, a))
        if b["u"] == "0":
            bad.append("clause 6: the census saw no units at all")
    return bad


def run(run_dir, units_min):
    net = check_cancel_task.net_log_lines(run_dir)
    harness = check_storage_purge.harness_log_lines(run_dir)
    bad = problems(net, harness, units_min)
    for ln in strip(harness):
        if (
            ln.startswith("; CAPU VERDICT")
            or ln.startswith("; CAPU SAVEUNITS")
            or ln.startswith("; CAP SAVE step")
            or ln.startswith("; CAP LOAD step")
        ):
            print("  " + ln[:200])
    for b in bad:
        print("  check_cap_units: " + b)
    print("check_cap_units: %s" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


GOOD_NET = [
    "; [caps] relocation APPLIED (3945 sites) in 10 ms; limits OFF",
    "; [caps] units relocation APPLIED (3826 sites, base .mhcapu=0x01400000); units limits staged",
    "; [caps] limits ON (gameplay settings): buildings 500, storage 127, turrets 127, mines 127, labs 127, productions 127, projectiles 4000, units 500, soldiers 500",
    "; [caps] mirror_check ARMED",
]
GOOD_HARNESS = [
    "; CAPU step=300 START player 0 proto 34 target=490 batch=20 row units=500 soldiers=500",
    "; CAPU step=900 SAMPLE units=300(hi 330) soldiers=700(hi 701) issued=320 met=0",
    "; CAPU step=2000 TARGET MET units=490 soldiers=1000 after 520 orders",
    "; CAPU VERDICT ok=1 step=5000 units=490(hi 490) soldiers=490(hi 490) issued=520 hw_units=490 hw_soldiers=490",
    "; CAPU step=5500 SAMPLE units=490(hi 490) soldiers=490(hi 490) issued=520 met=1",
    "; CAPU SAVEUNITS before step=4000 units=490(hi 490) soldiers=490(hi 490)",
    "; CAP SAVE step=4000 name=capu rc=1 digest=00112233AABBCCDD ovf=1 bldg_ovf=0 prj_ovf=0",
    "; CAP LOAD step=4000 name=capu rc=1 digest=00112233AABBCCDD ovf=1 bldg_ovf=0 prj_ovf=0",
    "; CAPU SAVEUNITS after step=4000 units=490(hi 490) soldiers=490(hi 490)",
]


def selftest():
    fails = []

    def expect(name, ok, detail=""):
        print("  [%s] %s" % ("ok" if ok else "FAIL", name))
        if not ok:
            fails.append(name)
            if detail:
                print("      " + str(detail))

    def has(lst, needle):
        return any(needle in x for x in lst)

    expect(
        "a good run is green",
        problems(GOOD_NET, GOOD_HARNESS) == [],
        problems(GOOD_NET, GOOD_HARNESS),
    )
    expect(
        "no units relocation is RED",
        has(problems(GOOD_NET[:1] + GOOD_NET[2:], GOOD_HARNESS), "units relocation APPLIED"),
    )
    expect(
        "units limit 100 is RED",
        has(
            problems(
                [
                    GOOD_NET[0],
                    GOOD_NET[1],
                    GOOD_NET[2].replace("units 500", "units 100"),
                    GOOD_NET[3],
                ],
                GOOD_HARNESS,
            ),
            "expected 500",
        ),
    )
    expect(
        "a refusal is RED",
        has(problems(GOOD_NET + ["; [caps] units relocation REFUSED: x"], GOOD_HARNESS), "REFUSED"),
    )
    expect(
        "a stock START row is RED",
        has(
            problems(
                GOOD_NET, [GOOD_HARNESS[0].replace("units=500", "units=100")] + GOOD_HARNESS[1:]
            ),
            "START rows",
        ),
    )
    expect("too few units is RED", has(problems(GOOD_NET, GOOD_HARNESS, 600), "live units"))
    expect(
        "units inside the vanilla window is RED",
        has(
            problems(
                GOOD_NET,
                [
                    x.replace("(hi 490) soldiers=490", "(hi 99) soldiers=490")
                    if "VERDICT" in x
                    else x
                    for x in GOOD_HARNESS
                ],
            ),
            "vanilla window",
        ),
    )
    expect("no verdict is RED", has(problems(GOOD_NET, GOOD_HARNESS[:2]), "no `; CAPU VERDICT"))
    expect(
        "a CAPU FAIL is RED",
        has(problems(GOOD_NET, ["; CAPU FAIL step=5: x"] + GOOD_HARNESS), "CAPU FAIL"),
    )
    expect(
        "a stopped game is RED",
        has(problems(GOOD_NET, GOOD_HARNESS[:4] + GOOD_HARNESS[5:]), "stopped stepping"),
    )
    expect(
        "a desync is RED",
        has(problems(GOOD_NET + ["; [desync] *** DESYNC step=9"], GOOD_HARNESS), "DESYNC"),
    )
    expect(
        "a mirror writer report is RED",
        has(
            problems(GOOD_NET + ["; [caps] mirror_check: array units changed"], GOOD_HARNESS),
            "clause 5",
        ),
    )
    expect(
        "a digest change across the round trip is RED",
        has(
            problems(
                GOOD_NET,
                [
                    x.replace("digest=00112233AABBCCDD", "digest=1") if "CAP LOAD" in x else x
                    for x in GOOD_HARNESS
                ],
            ),
            "digest",
        ),
    )
    expect(
        "a census change across the round trip is RED",
        has(
            problems(
                GOOD_NET,
                [
                    x.replace(
                        "units=490(hi 490) soldiers=490(hi 490)",
                        "units=300(hi 490) soldiers=490(hi 490)",
                    )
                    if "after" in x and "SAVEUNITS" in x
                    else x
                    for x in GOOD_HARNESS
                ],
            ),
            "census",
        ),
    )
    expect("no save pair is RED", has(problems(GOOD_NET, GOOD_HARNESS[:5]), "clause 6"))
    print("check_cap_units --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    units = 480
    if "--units" in argv:
        i = argv.index("--units")
        units = int(argv[i + 1])
        argv = argv[:i] + argv[i + 2 :]
    args = [a for a in argv if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    return max(run(d, units) for d in args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
