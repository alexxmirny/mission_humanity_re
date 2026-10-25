#!/usr/bin/env python3
"""
tools/check_cap_orders_ui.py -- patching:PT-CAP9-U5: the UI row (cap_ctrlgrp_ui / cap_ctrlgrp_ui_off): the order layers carry a ~490-unit order that a PLAYER
gives through control groups.

The stimulus is tools/uiscripts/cap_ctrlgrp_solo.txt. The harness unit-fill fixture (`capu_*`) gives player 0 490 units round the mother; then the script does
what a player does, through the game's own input path (no harness-issued order):
  1. three box selects (each at most 200 ids, the U6 selection cap), each followed by `key <digit> ctrl` -- Ctrl+1 / Ctrl+2 / Ctrl+3: the selection becomes control
     group N and every member gets a status-bit order (0x34, bit N of its ai_group_index) + its 0xfb echo, ~160 replicated orders in ONE keystroke;
  2. per group: its digit recalls it (the group is the selection again: more status-bit orders), then ONE right-click on the ground gives the selection a move
     order (a move order per unit + echo; the goal fields +0x86/+0x87 take the target).
The harness watch (`capg_*`) is passive: every capg_every steps it logs `; CAPG step=.. groups=n0..n9 bits=B goal_changed=G tracked=T q=.. p=.. s=..` -- per
group the member count, B = the members of groups 1..3 whose ai_group_index carries their group's bit (a status-bit order APPLIED), G = the members whose goal
fields moved since the snapshot taken before any click (a move order APPLIED), T = the distinct members of groups 1..3. The order layer's own census
(`[caps] orders step=.. hw q=.. ovf ..`, mh_net.log) says how deep the arrays went and whether anything was refused.

Clauses, ON arm (the order layer ON, libmh.dll absent):
  1  mh_net.log: `orders layer STAGED` + `orders layer ON`, no `[caps] ... REFUSED`; mh_harness.log: `; CAPU ... TARGET MET` with >= --units units, no `; CAPU FAIL` / `; CAPG FAIL`.
  2  the last `; CAPG` line: groups 1..3 hold T units in total, T >= --units (default 450), each group <= 200, and no other group (4..9) is used.
  3  APPLIED, not displacement: bits == the sum of the three group counts (every member carries its group's status bit) and goal_changed == T (every member's goal fields hold
     the move order's target, not just displaced).
  4  the last order census: hw q > 300 (past the vanilla queue [300]), ovf q=p=s=quota=0, guard=0x0.
  5  no `*** DESYNC`, no `ORDERS GUARD HIT`, the game kept stepping after the last click (>= --after steps).
Clauses, OFF arm (--negative, `[caps] orders_layer=0`, the retail queue [300] on the raised roster):
  1' the layer is OFF (`orders layer OFF`), no `orders layer ON`.
  2' as 2 (the selection / group records are a different layer: they hold T units either way).
  3' the VANILLA DROP: the queue [300] is wiped by the burst of one chord (~320 records), so applied < T on at least one of the two counts (bits < sum or goal_changed < T).

  python tools/check_cap_orders_ui.py [--negative] [--units N] [--after N] <run-dir> [...]
  python tools/check_cap_orders_ui.py --selftest
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402
import check_storage_purge  # noqa: E402

STAMP = re.compile(r"^\[[\d:.]+\]\s*")
CAPG_RE = re.compile(
    r"; CAPG step=(?P<step>\d+) groups=(?P<g>[\d,]+) bits=(?P<bits>\d+) goal_changed=(?P<gc>\d+) tracked=(?P<t>\d+) "
    r"q=(?P<q>-?\d+) p=(?P<p>-?\d+) s=(?P<s>-?\d+)"
)
CENSUS_RE = re.compile(
    r"\[caps\] orders step=(?P<step>\d+) hw q=(?P<hq>\d+) p=(?P<hp>\d+) s=(?P<hs>\d+) \(of (?P<cq>\d+)/(?P<cp>\d+)/(?P<cs>\d+)\) "
    r"ovf q=(?P<oq>\d+) p=(?P<op>\d+) s=(?P<os>\d+) quota=(?P<oquota>\d+) cnt q=(?P<nq>-?\d+) p=(?P<np>-?\d+) s=(?P<ns>-?\d+) guard=0x(?P<g>[0-9A-Fa-f]+)"
)
CAPU_RE = re.compile(r"; CAPU step=(?P<step>\d+) TARGET MET units=(?P<u>\d+)")


def strip(lines):
    return [STAMP.sub("", ln.strip()) for ln in lines]


def parse(net, harness):
    net, harness = strip(net), strip(harness)
    capg = [m for m in map(CAPG_RE.match, harness) if m]
    census = [m for m in map(CENSUS_RE.search, net) if m]
    return net, harness, capg, census


def problems(net, harness, negative=False, units_min=450, after_min=100):
    net, harness, capg, census = parse(net, harness)
    bad = []
    if negative:
        if not any("orders layer OFF" in ln for ln in net):
            bad.append("clause 1': no `orders layer OFF` in mh_net.log")
        if any("orders layer ON" in ln for ln in net):
            bad.append("clause 1': the order layer is ON in the negative arm")
    else:
        if not any("orders layer STAGED" in ln for ln in net):
            bad.append("clause 1: no `orders layer STAGED` in mh_net.log")
        if not any("orders layer ON" in ln for ln in net):
            bad.append("clause 1: no `orders layer ON` in mh_net.log")
        bad += ["clause 1: " + ln for ln in net if "[caps]" in ln and "REFUSED" in ln]
    met = [m for m in map(CAPU_RE.match, harness) if m]
    if not met:
        bad.append("clause 1: no `; CAPU ... TARGET MET` (the roster was never filled)")
    elif int(met[-1].group("u")) < units_min:
        bad.append(
            "clause 1: the roster stopped at %s units (< %d)" % (met[-1].group("u"), units_min)
        )
    bad += ["clause 1: " + ln for ln in harness if "; CAPU FAIL" in ln or "; CAPG FAIL" in ln]
    bad += [
        "clause 5: " + ln for ln in net + harness if "*** DESYNC" in ln or "ORDERS GUARD HIT" in ln
    ]
    if not capg:
        bad.append("clause 2: no `; CAPG` census (capg_at never reached)")
        return bad
    last = capg[-1]
    groups = [int(x) for x in last.group("g").split(",")]
    t, bits, gc = int(last.group("t")), int(last.group("bits")), int(last.group("gc"))
    members = sum(groups[1:4])
    if t < units_min:
        bad.append("clause 2: only %d units sit in control groups 1..3 (< %d)" % (t, units_min))
    if any(g > 200 for g in groups):
        bad.append("clause 2: a group holds more than 200 ids: %s" % groups)
    if any(groups[4:]):
        bad.append("clause 2: groups 4..9 are not empty: %s" % groups)
    if t != members:
        bad.append(
            "clause 2: %d distinct units in groups holding %d members (a unit in two groups?)"
            % (t, members)
        )
    if negative:
        if bits >= members and gc >= t:
            bad.append(
                "clause 3': nothing was dropped (bits %d of %d, goal_changed %d of %d): the vanilla queue [300] was not exceeded"
                % (bits, members, gc, t)
            )
        return bad
    if bits != members:
        bad.append(
            "clause 3: only %d of %d members carry their group's status bit (orders applied)"
            % (bits, members)
        )
    if gc != t:
        bad.append(
            "clause 3: only %d of %d members hold the move order's goal (orders applied)" % (gc, t)
        )
    if not census:
        bad.append("clause 4: no `[caps] orders step=` census in mh_net.log")
    else:
        c = census[-1]
        if int(c.group("hq")) <= 300:
            bad.append("clause 4: queue high-water %s did not pass the vanilla 300" % c.group("hq"))
        for k in ("oq", "op", "os", "oquota"):
            if int(c.group(k)):
                bad.append(
                    "clause 4: the census counts a refusal/overflow (%s=%s)" % (k, c.group(k))
                )
        if int(c.group("g"), 16):
            bad.append("clause 4: guard=0x%s" % c.group("g"))
    if int(capg[-1].group("step")) - int(capg[0].group("step")) < after_min:
        bad.append("clause 5: the watch ran for fewer than %d steps" % after_min)
    return bad


def run(run_dir, negative, units_min):
    net = check_cancel_task.net_log_lines(run_dir)
    harness = check_storage_purge.harness_log_lines(run_dir)
    bad = problems(net, harness, negative, units_min)
    capg = [m for m in map(CAPG_RE.match, strip(harness)) if m]
    if capg:
        print("  " + capg[-1].group(0)[:240])
    for ln in strip(net):
        if CENSUS_RE.search(ln):
            last = ln
    try:
        print("  " + last[:300])
    except NameError:
        pass
    for b in bad:
        print("  check_cap_orders_ui: " + b)
    print(
        "check_cap_orders_ui: %s%s" % ("FAIL" if bad else "PASS", " (OFF arm)" if negative else "")
    )
    return 1 if bad else 0


GOOD_NET = [
    "; [caps] orders layer STAGED (queue 4096 pending 32768 staging 2048)",
    "; [caps] orders layer ON (queue 4096 pending 32768 staging 2048; sender quota 2048)",
    "; [caps] orders step=900 hw q=603 p=0 s=0 (of 4096/32768/2048) ovf q=0 p=0 s=0 quota=0 cnt q=0 p=0 s=0 guard=0x0",
]
GOOD_HARNESS = [
    "; CAPU step=480 TARGET MET units=490 soldiers=0 after 489 orders",
    "; CAPG step=300 groups=1,0,0,0,0,0,0,0,0,0 bits=0 goal_changed=0 tracked=0 q=0 p=0 s=0",
    "; CAPG step=900 groups=0,163,163,163,0,0,0,0,0,0 bits=489 goal_changed=489 tracked=489 q=0 p=0 s=0",
    "; CAPG step=1100 groups=0,163,163,163,0,0,0,0,0,0 bits=489 goal_changed=489 tracked=489 q=0 p=0 s=0",
]
GOOD_OFF_NET = [
    "; [caps] orders layer OFF ([caps] orders_layer=0)",
]
GOOD_OFF_HARNESS = [
    "; CAPU step=480 TARGET MET units=490 soldiers=0 after 489 orders",
    "; CAPG step=300 groups=1,0,0,0,0,0,0,0,0,0 bits=0 goal_changed=0 tracked=0 q=0 p=0 s=0",
    "; CAPG step=1100 groups=0,163,163,163,0,0,0,0,0,0 bits=112 goal_changed=130 tracked=489 q=0 p=0 s=0",
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

    def rep(h, a, b):
        return [x.replace(a, b) for x in h]

    expect(
        "a good ON run is green",
        problems(GOOD_NET, GOOD_HARNESS) == [],
        problems(GOOD_NET, GOOD_HARNESS),
    )
    expect(
        "a good OFF run is green",
        problems(GOOD_OFF_NET, GOOD_OFF_HARNESS, True) == [],
        problems(GOOD_OFF_NET, GOOD_OFF_HARNESS, True),
    )
    expect("no STAGED line is RED", has(problems(GOOD_NET[1:], GOOD_HARNESS), "STAGED"))
    expect(
        "a refusal is RED",
        has(problems(GOOD_NET + ["; [caps] orders REFUSED (x)"], GOOD_HARNESS), "REFUSED"),
    )
    expect("no roster line is RED", has(problems(GOOD_NET, GOOD_HARNESS[1:]), "TARGET MET"))
    expect("no census at all is RED", has(problems(GOOD_NET, GOOD_HARNESS[:1]), "no `; CAPG`"))
    expect(
        "few grouped units are RED",
        has(problems(GOOD_NET, rep(GOOD_HARNESS, "tracked=489", "tracked=300")), "only 300"),
    )
    expect(
        "a group past 200 is RED",
        has(
            problems(GOOD_NET, rep(GOOD_HARNESS, "groups=0,163,163,163", "groups=0,250,120,119")),
            "more than 200",
        ),
    )
    expect(
        "a used group 4 is RED",
        has(
            problems(
                GOOD_NET, rep(GOOD_HARNESS, "groups=0,163,163,163,0", "groups=0,163,163,162,1")
            ),
            "4..9",
        ),
    )
    expect(
        "dropped status bits are RED",
        has(problems(GOOD_NET, rep(GOOD_HARNESS, "bits=489", "bits=400")), "status bit"),
    )
    expect(
        "unapplied move orders are RED",
        has(
            problems(GOOD_NET, rep(GOOD_HARNESS, "goal_changed=489", "goal_changed=300")),
            "move order's goal",
        ),
    )
    expect(
        "a queue that stayed in the vanilla bound is RED",
        has(problems(rep(GOOD_NET, "hw q=603", "hw q=300"), GOOD_HARNESS), "vanilla 300"),
    )
    expect(
        "an overflow is RED",
        has(problems(rep(GOOD_NET, "ovf q=0", "ovf q=5"), GOOD_HARNESS), "overflow"),
    )
    expect(
        "a desync is RED",
        has(problems(GOOD_NET + ["; [desync] *** DESYNC step=9"], GOOD_HARNESS), "DESYNC"),
    )
    expect(
        "OFF arm with the layer ON is RED",
        has(problems(GOOD_NET, GOOD_OFF_HARNESS, True), "ON in the negative"),
    )
    expect(
        "OFF arm that dropped nothing is RED",
        has(
            problems(
                GOOD_OFF_NET,
                rep(GOOD_OFF_HARNESS, "bits=112 goal_changed=130", "bits=489 goal_changed=489"),
                True,
            ),
            "nothing was dropped",
        ),
    )
    print("check_cap_orders_ui --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    negative = "--negative" in argv
    units = 450
    if "--units" in argv:
        i = argv.index("--units")
        units = int(argv[i + 1])
        argv = argv[:i] + argv[i + 2 :]
    args = [a for a in argv if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    return max(run(d, negative, units) for d in args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
