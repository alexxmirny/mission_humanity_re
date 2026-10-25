#!/usr/bin/env python3
"""
tools/check_cap_turret.py -- patching:PT-CAP10: the POWERED-TURRET projectile test of the v0.2.2 cap raise.

The stimulus is the harness cap fixture (`cap_*` / `cap2_*` keys, capf_tick + capf_army_tick in mh_harness/harness.cpp), run with libmh.dll absent
and `[caps] raise=1` + `mirror_check=1` (ini/caps_raise.ini) on a 1-human + 1-AI skirmish where BOTH sides are Alien:

  army A  player 0: `cap_plant` power plants spread through the grid (every `cap_plant_every`-th cell) and 126 alien laser bunkers
          (R_LASER2, speed 2.0 / length 400: the slowest projectile of the three turret weapons, a clip of 3), on a lattice with 6-tile
          spacing, all on player 0's own network, population granted through the game's own debug order 0xec;
  army B  player 1: 200 unbuilt target buildings (storage/mine/lab: tile-mapped, never connected) on the half-cell-shifted lattice, so
          every bunker of A has an enemy building exactly 6 tiles away (the bunker's reach);
  army C  player 1's own powered grid at ITS mother (the same plants + bunkers), army D its targets (player 0's buildings around it).

Clauses (all must hold):
  1  mh_net.log: `[caps] relocation APPLIED`, `[caps] limits ON (`, no `[caps] ... REFUSED`; no desync banner; no `; CAP FAIL` / `; CAP2 FAIL`.
  2  both powered armies stood: the last `; CAP PRJ` sample shows >= --turrets bunkers connected+staffed and >= --attackers of them in
     the attack state; the last `army=C` sample the same (so the count is real fire from 2 x 126 bunkers, not a leftover).
  3  PROJECTILES: the live count (`; CAP PRJ ... live=N`, active records of the pool) crossed --min (default 1500, the vanilla pool) and
     STAYED above it: from the first sample over --min to the last sample, no sample is under --min and the span is >= --hold steps; the
     high-water mark is reported (the pool is 4000 wide, so a mark past 1500 is also a record in the raised part: `hw_index`).
  4  10 GAME MINUTES: the game clock in the last per-step hash line of mh_harness.log is >= --seconds (default 600).
  5  mirror_check: `[caps] mirror_check ARMED` is in the net log and no `[caps] mirror_check: ... changed` writer report.
  6  the SAVE ROUND TRIP while the pool is over --min: check_cap_save's save clauses (equal digest/counts, planet-file footer, container stamp,
     trailer N == ovf, raised save accepted, overflow applied) AND the live census either side of the load (`; CAP SAVEPRJ before/after`):
     live > --min before, and live / highest slot / count past slot 1500 EQUAL after.

  python tools/check_cap_turret.py [--min N] [--hold STEPS] [--seconds S] [--turrets N] [--attackers N] <run-dir> [<run-dir> ...]
  python tools/check_cap_turret.py --selftest
"""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402  (net_log_lines: the session dir + its process dir)
import check_cap_save  # noqa: E402  (save_problems)
import check_storage_purge  # noqa: E402  (harness_log_lines)

STAMP = re.compile(r"^\[[\d:.]+\]\s*")
PRJ_RE = re.compile(
    r"; CAP PRJ step=(?P<step>\d+) live=(?P<live>\d+) hw_index=(?P<hw_index>\d+) hw_live=(?P<hw_live>\d+) plants=(?P<plants>\d+) turrets=(?P<turrets>\d+)"
    r" tur_conn_staffed=(?P<ok>\d+) scan=(?P<scan>\d+) attack=(?P<attack>\d+)"
)
ARMY_RE = re.compile(
    r"; CAP2 step=(?P<step>\d+) army=(?P<army>\w+) owner=(?P<owner>\d+) .* turrets=(?P<turrets>\d+) .* conn_staffed=(?P<ok>\d+) scan=(?P<scan>\d+) attack=(?P<attack>\d+)"
)
CENSUS_RE = re.compile(
    r"; CAP SAVEPRJ (?P<when>before|after) step=(?P<step>\d+) live=(?P<live>\d+) highest_slot=(?P<hi>\d+) past_1500=(?P<past>\d+) pool_header=(?P<hdr>\d+)"
)
STEP_RE = re.compile(r"^(?P<step>\d+) (?P<clock>[0-9A-Fa-f]{16}) ")


def strip(lines):
    return [STAMP.sub("", ln.strip()) for ln in lines]


def clock_seconds(hexword):
    return struct.unpack(">d", bytes.fromhex(hexword))[0]


def problems(net, harness, min_live=1500, hold=20000, seconds=600.0, turrets=120, attackers=100):
    net, harness = strip(net), strip(harness)
    bad = []
    # 1 banners
    if not any("[caps] relocation APPLIED" in ln for ln in net):
        bad.append("clause 1: no `[caps] relocation APPLIED` in mh_net.log")
    if not any("[caps] limits ON (" in ln for ln in net):
        bad.append("clause 1: no `[caps] limits ON (` in mh_net.log (the raise never turned on)")
    for ln in net:
        if "[caps]" in ln and "REFUSED" in ln:
            bad.append("clause 1: " + ln)
    for ln in net + harness:
        if "*** DESYNC" in ln:
            bad.append("clause 1: " + ln)
    for ln in harness:
        if ln.startswith("; CAP FAIL") or ln.startswith("; CAP2 FAIL"):
            bad.append("clause 1: " + ln)
    # 2 the armies
    prj = [m for m in (PRJ_RE.match(ln) for ln in harness) if m]
    if not prj:
        bad.append(
            "clause 2: no `; CAP PRJ` sample (the fixture never ran, or the process died first)"
        )
        return bad
    last = prj[-1]
    if int(last.group("ok")) < turrets:
        bad.append(
            "clause 2: army A has %s bunkers connected+staffed in the last sample, want >= %d"
            % (last.group("ok"), turrets)
        )
    if int(last.group("attack")) < attackers:
        bad.append(
            "clause 2: army A has %s bunkers in the attack state in the last sample, want >= %d"
            % (last.group("attack"), attackers)
        )
    armies = [m for m in (ARMY_RE.match(ln) for ln in harness) if m and m.group("army") == "C"]
    if not armies:
        bad.append("clause 2: no `army=C` sample (the second powered army never started)")
    else:
        c = armies[-1]
        if int(c.group("ok")) < turrets:
            bad.append(
                "clause 2: army C has %s bunkers connected+staffed in its last sample, want >= %d"
                % (c.group("ok"), turrets)
            )
        if int(c.group("attack")) < attackers:
            bad.append(
                "clause 2: army C has %s bunkers in the attack state in its last sample, want >= %d"
                % (c.group("attack"), attackers)
            )
    # 3 projectiles
    over = [m for m in prj if int(m.group("live")) > min_live]
    hw = max(int(m.group("hw_live")) for m in prj)
    if not over:
        bad.append(
            "clause 3: the live projectile count never passed %d (high-water %d)" % (min_live, hw)
        )
    else:
        first = over[0]
        after = [m for m in prj if int(m.group("step")) >= int(first.group("step"))]
        under = [m for m in after if int(m.group("live")) <= min_live]
        span = int(after[-1].group("step")) - int(first.group("step"))
        if under:
            bad.append(
                "clause 3: after crossing %d at step %s the count fell to %s at step %s (%d of %d samples at or under)"
                % (
                    min_live,
                    first.group("step"),
                    under[0].group("live"),
                    under[0].group("step"),
                    len(under),
                    len(after),
                )
            )
        if span < hold:
            bad.append(
                "clause 3: the count held over %d for only %d steps (want >= %d)"
                % (min_live, span, hold)
            )
    # 4 game minutes
    steps = [m for m in (STEP_RE.match(ln) for ln in harness) if m]
    if not steps:
        bad.append("clause 4: no per-step hash line in mh_harness.log (cannot read the game clock)")
    else:
        t = clock_seconds(steps[-1].group("clock"))
        if t < seconds:
            bad.append("clause 4: the game clock reached %.1f s, want >= %.0f s" % (t, seconds))
    # 5 mirror_check
    if not any("[caps] mirror_check ARMED" in ln for ln in net):
        bad.append(
            "clause 5: no `[caps] mirror_check ARMED` banner (a silent watch proves nothing)"
        )
    for ln in net:
        if "[caps] mirror_check:" in ln and "changed" in ln:
            bad.append("clause 5: " + ln)
    # 6 the save round trip, with the pool over the vanilla window
    bad += check_cap_save.save_problems(net, harness, prj_min=min_live // 2)
    cen = {m.group("when"): m for m in (CENSUS_RE.match(ln) for ln in harness) if m}
    if "before" not in cen or "after" not in cen:
        bad.append(
            "clause 6: no `; CAP SAVEPRJ before/after` pair (the live census around the round trip)"
        )
    else:
        b, a = cen["before"], cen["after"]
        if int(b.group("live")) <= min_live:
            bad.append(
                "clause 6: only %s projectiles were live when the save was taken, want > %d"
                % (b.group("live"), min_live)
            )
        for k, what in (
            ("live", "live count"),
            ("hi", "highest active slot"),
            ("past", "records past slot 1500"),
            ("hdr", "pool header count"),
        ):
            if b.group(k) != a.group(k):
                bad.append(
                    "clause 6: %s %s before the save, %s after the load"
                    % (what, b.group(k), a.group(k))
                )
    return bad


def run(run_dir, **kw):
    net = check_cancel_task.net_log_lines(run_dir)
    harness = check_storage_purge.harness_log_lines(run_dir)
    bad = problems(net, harness, **kw)
    h = strip(harness)
    prj = [m for m in (PRJ_RE.match(ln) for ln in h) if m]
    if prj:
        live = [int(m.group("live")) for m in prj]
        steps = STEP_RE
        last_clock = [m for m in (steps.match(ln) for ln in h) if m]
        print(
            "  samples=%d  live: peak=%d (high-water %s, highest slot %s)  last=%d  steady(median of samples over 1500)=%s  game clock=%s s"
            % (
                len(live),
                max(live),
                prj[-1].group("hw_live"),
                max(int(m.group("hw_index")) for m in prj),
                live[-1],
                sorted([x for x in live if x > 1500] or [0])[
                    len([x for x in live if x > 1500]) // 2
                ],
                "%.0f" % clock_seconds(last_clock[-1].group("clock")) if last_clock else "?",
            )
        )
    for ln in h:
        if (
            ln.startswith("; CAP SAVE ")
            or ln.startswith("; CAP LOAD ")
            or ln.startswith("; CAP SAVEPRJ")
        ):
            print("  " + ln[:200])
    for b in bad:
        print("  check_cap_turret: " + b)
    print("check_cap_turret: %s" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


# ---------------------------------------------------------------------------------------------------------------------------------------
# selftest: planted logs (hermetic).
GOOD_NET = [
    "; [caps] mirror_check ARMED: a writer that touches a stock (canonical-mirror) address is reported once per array",
    "; [caps] relocation APPLIED (3945 sites, 2 sections, base .mhcapd=0x040C0000) in 10.1 ms; limits OFF",
    "; [caps] limits ON (gameplay settings): buildings 500, storage 127, turrets 127, mines 127, labs 127, productions 127, projectiles 4000",
    "; [caps] save splices: trailer armed, container stamp armed, version check armed, refusal message armed",
    "; [caps] save: planet 31 trailer 2612 records (401336 bytes)",
    "; [caps] save: container stamped (a member planet carries an overflow trailer)",
    "; [caps] load: raised save accepted",
    "; [caps] load: planet file overflow 2612 records applied",
]


def _good_harness():
    h = []
    for i, step in enumerate(range(436, 30001, 250)):
        live = 0 if step < 900 else (1700 if step < 1400 else 2700)
        h.append(
            "; CAP PRJ step=%d live=%d hw_index=%d hw_live=%d plants=60 turrets=126 tur_conn_staffed=126 scan=2 attack=124 other=0 eff_x1000=1000"
            % (step, live, live + 100, live)
        )
        h.append(
            "; CAP2 step=%d army=C owner=1 bldg=490 plants=60 turrets=126 relays_near=2 conn_staffed=126 scan=2 attack=124 other=0 eff_x1000=1000 placed=224 refused=31"
            % step
        )
    h += [
        "; CAP SAVEPRJ before step=2900 live=2736 highest_slot=2868 past_1500=1300 pool_header=2736",
        "; CAP SAVE step=2900 name=capturret rc=1 digest=AA638E7743D707B3 ovf=2612 bldg_ovf=713 prj_ovf=1369",
        "; CAP SAVE FILE name=capturret size=926833 fnv=E696F9ED stamped=1",
        "; CAP SAVE PLANET file=save\\temp\\save31.dat size=897457 footer=1 footer_len=401336",
        "; CAP LOAD step=2900 name=capturret rc=1 digest=AA638E7743D707B3 ovf=2612 bldg_ovf=713 prj_ovf=1369",
        "; CAP SAVEPRJ after step=2900 live=2736 highest_slot=2868 past_1500=1300 pool_header=2736",
        "30000 %016X 0000000000000000 0000000000000000"
        % struct.unpack(">Q", struct.pack(">d", 611.0)),
    ]
    return h


def selftest():
    fails = []

    def expect(what, ok):
        if not ok:
            fails.append(what)
        print("  %-4s %s" % ("ok" if ok else "FAIL", what))

    def has(bad, frag):
        return any(frag in b for b in bad)

    good = problems(GOOD_NET, _good_harness())
    expect("planted good logs pass", not good)
    if good:
        print("\n".join("      " + b for b in good))

    def mut(lines, frag, repl):
        out, hit = [], False
        for ln in lines:
            if frag in ln and not hit:
                out.append(ln.replace(frag, repl))
                hit = True
            else:
                out.append(ln)
        assert hit, frag
        return out

    h = _good_harness()
    # 3: never over the vanilla pool
    low = [ln.replace("live=2700", "live=900").replace("live=1700", "live=900") for ln in h]
    expect(
        "a pool that never passes 1500 goes red",
        has(problems(GOOD_NET, low), "clause 3: the live projectile count never passed"),
    )
    # 3: a dip after the crossing
    dip = mut(h, "live=2700", "live=1200")
    expect(
        "a dip under 1500 after the crossing goes red",
        has(problems(GOOD_NET, dip), "the count fell to"),
    )
    # 3: held too briefly
    short = [
        ln
        for ln in h
        if not (ln.startswith("; CAP PRJ") and int(PRJ_RE.match(ln).group("step")) > 5000)
    ]
    expect(
        "a count that is not held goes red",
        has(problems(GOOD_NET, short), "held over 1500 for only"),
    )
    # 2: second army idle
    idle = [ln.replace("attack=124", "attack=0") if ln.startswith("; CAP2") else ln for ln in h]
    expect("an idle second army goes red", has(problems(GOOD_NET, idle), "army C has"))
    # 2: unpowered army A
    unp = [
        ln.replace("tur_conn_staffed=126", "tur_conn_staffed=3")
        if ln.startswith("; CAP PRJ")
        else ln
        for ln in h
    ]
    expect(
        "unpowered bunkers go red",
        has(problems(GOOD_NET, unp), "army A has 3 bunkers connected+staffed"),
    )
    # 4: short game
    short_clock = [ln for ln in h]
    short_clock[-1] = "30000 %016X 0 0" % struct.unpack(">Q", struct.pack(">d", 300.0))
    expect(
        "a game that stopped at 300 s goes red",
        has(problems(GOOD_NET, short_clock), "clause 4: the game clock reached 300.0 s"),
    )
    # 5: mirror writer / no banner
    expect(
        "a mirror_check writer report goes red",
        has(
            problems(
                GOOD_NET + ["; [caps] mirror_check: array buildings changed at 0x00b3a1c0"], h
            ),
            "clause 5",
        ),
    )
    expect(
        "no mirror_check banner goes red",
        has(
            problems([ln for ln in GOOD_NET if "mirror_check" not in ln], h),
            "clause 5: no `[caps] mirror_check ARMED`",
        ),
    )
    # 6: the load changed the count
    bad_after = mut(
        h, "; CAP SAVEPRJ after step=2900 live=2736", "; CAP SAVEPRJ after step=2900 live=2000"
    )
    expect(
        "a different live count after the load goes red",
        has(problems(GOOD_NET, bad_after), "live count 2736 before the save, 2000 after the load"),
    )
    # 6: save under the pool
    few = mut(
        h, "; CAP SAVEPRJ before step=2900 live=2736", "; CAP SAVEPRJ before step=2900 live=900"
    )
    expect(
        "a save taken with the pool under 1500 goes red",
        has(problems(GOOD_NET, few), "projectiles were live when the save was taken"),
    )
    # 6: digest differs
    dg = (
        mut(
            h,
            "digest=AA638E7743D707B3 ovf=2612 bldg_ovf=713 prj_ovf=1369\n",
            "digest=AA638E7743D707B3 ovf=2612 bldg_ovf=713 prj_ovf=1369",
        )
        if False
        else [
            ln.replace(
                "; CAP LOAD step=2900 name=capturret rc=1 digest=AA638E7743D707B3",
                "; CAP LOAD step=2900 name=capturret rc=1 digest=BB638E7743D707B3",
            )
            for ln in h
        ]
    )
    expect(
        "a digest that differs after the load goes red", has(problems(GOOD_NET, dg), "NOT exact")
    )
    # 1: refusal / desync / fixture failure
    expect(
        "a REFUSED line goes red",
        has(problems(GOOD_NET + ["; [caps] load REFUSED: x"], h), "clause 1"),
    )
    expect(
        "a desync banner goes red",
        has(problems(GOOD_NET, h + ["; [desync] *** DESYNC step=5"]), "clause 1"),
    )
    expect(
        "a CAP2 FAIL line goes red",
        has(problems(GOOD_NET, h + ["; CAP2 FAIL army C: ran out of candidate tiles"]), "clause 1"),
    )
    print("check_cap_turret --selftest: %s" % ("FAIL (%d)" % len(fails) if fails else "PASS"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    opts = {"min_live": 1500, "hold": 20000, "seconds": 600.0, "turrets": 120, "attackers": 100}
    flags = {
        "--min": ("min_live", int),
        "--hold": ("hold", int),
        "--seconds": ("seconds", float),
        "--turrets": ("turrets", int),
        "--attackers": ("attackers", int),
    }
    rest = []
    i = 0
    while i < len(argv):
        if argv[i] in flags:
            key, cast = flags[argv[i]]
            opts[key] = cast(argv[i + 1])
            i += 2
        else:
            rest.append(argv[i])
            i += 1
    args = [a for a in rest if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    return max(run(d, **opts) for d in args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
