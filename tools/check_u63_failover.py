#!/usr/bin/env python3
"""check_u63_failover.py -- mp:U63 (HM-M5): did the CRASH FAILOVER of the hub work on a 3-peer run?

Reads the three pulled peer dirs of a `det_arms.py --u63-*` run (<det_dir>/{host,client1,client2}); the
host is the CRASHED hub (the harness ends its PROCESS with TerminateProcess at a pinned sim step: no
LEAVING, no DISCONNECT, nothing on the wire), the two clients are the SURVIVORS. Pass conditions (the
done_when of mp:U63):

  * BOTH survivors log `failover SUSPECT` (the hub went silent) and `failover CORROBORATED`;
  * BOTH survivors log the SAME `hub elected <id> epoch <e>` line (id and epoch), the id is a survivor, and the
    elected hub's own log says it became the hub while the other survivor's says it re-dialled it;
  * both survivors carry the in-game notice `U62 notice: hub 0 -> <id>` marked as a crash failover;
  * both survivors kept stepping >= MIN_POST steps past the crashed hub's last step, and are ALL PAIRS
    IDENTICAL over them (mp_analyze);
  * no game-clock stall > STALL_MAX_MS around the crash (mh_lockstep.log wall gap between clock advances in
    [crash clock - 1 s, crash clock + 20 s]);
  * no survivor was dropped by the game's own silence timers: no `GS2: peer <survivor>` and no
    `U17 fast-drop ... side=<survivor>` line (the failover suspends them);
  * the game said the failover was active and then over (`U63 failover ACTIVE` / `U63 failover over`).

`--negative` is the `[net] hub_migration=0` arm: NO failover line may appear on a survivor, and a survivor
must reach `on_gameover ... outcome=8` between 30 and 90 s after its first stall (U55 measured ~58 s).

  python tools/check_u63_failover.py <det_dir> [--negative] [--min-post N]
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_u53_elim as u53  # noqa: E402
from check_u62_handover import _wall, stall_window  # noqa: E402

MIN_POST = 1000
STALL_MAX_MS = 6000
SURVIVORS = ("client1", "client2")
ELECTED_RE = re.compile(r"hub elected (\d+) epoch (\d+)")


def _first_wall(net, needle):
    for ln in net.splitlines():
        if needle in ln:
            w = _wall(ln)
            if w is not None:
                return w
    return None


def check(det_dir, negative=False, min_post=MIN_POST):
    out, bad = [], []
    logs = {}
    for p in u53.PEERS:
        d = os.path.join(det_dir, p)
        logs[p] = (
            u53._read(os.path.join(d, "mh_net.log")),
            u53._read(os.path.join(d, "mh_harness.log")),
        )
    host_net, host_har = logs["host"]
    hs = u53.harness_steps(host_har)
    if not hs:
        raise u53.Refusal("the host's harness log has no hashed steps")
    crash_step, crash_clock = hs[-1]
    m = re.search(r"; EXIT-PROCESS step=(\d+) mode=(\w+) pid=(\d+)", host_har)
    out.append(
        "crashed hub (host): last hashed step=%d (gclk %.1fs); exit line: %s"
        % (crash_step, crash_clock, m.group(0) if m else "MISSING")
    )
    if not m:
        bad.append("the host never logged its `; EXIT-PROCESS` line (the exit knob did not fire)")
    elif m.group(2).lower() not in ("terminate", "terminateprocess", "abrupt", "0"):
        out.append("NOTE: exit mode is %r (expected the abrupt TerminateProcess)" % m.group(2))
    last = {}
    for sv in SURVIVORS:
        st = u53.harness_steps(logs[sv][1])
        last[sv] = st[-1][0] if st else 0
        out.append(
            "%s last hashed step=%d (%d past the crash)" % (sv, last[sv], last[sv] - crash_step)
        )

    if negative:
        for sv in SURVIVORS:
            net = logs[sv][0]
            fl = [ln for ln in net.splitlines() if "failover " in ln and "(mp:U63)" in ln]
            if fl:
                bad.append(
                    "%s logged a failover line with migration OFF: %s" % (sv, fl[0].strip()[-120:])
                )
        found = False
        for sv in SURVIVORS:
            tl = u53.timeline(logs[sv][0], sv)
            out += tl
            for t in tl:
                mm = re.search(r"timeline \+\s*([\d.]+)s\s+on_gameover outcome=(\d+)", t)
                if mm and mm.group(2) == "8":
                    found = True
                    secs = float(mm.group(1))
                    if not 30.0 <= secs <= 90.0:
                        bad.append(
                            "%s reached outcome 8 at +%.1fs (expected ~58 s, window 30..90)"
                            % (sv, secs)
                        )
        if not found:
            bad.append("no survivor reached outcome 8 (U55's timeline did not reproduce)")
        return (not bad), out + ["FAIL: " + b for b in bad]

    elected = {}
    notice = {}
    for sv in SURVIVORS:
        net = logs[sv][0]
        for key in ("failover SUSPECT", "failover CORROBORATED", "failover DONE"):
            ln = next((x for x in net.splitlines() if key in x), None)
            if ln is None:
                bad.append("%s never logged `%s`" % (sv, key))
            else:
                out.append("    %s: %s" % (sv, ln.strip()[:190]))
        el = ELECTED_RE.findall(net)
        elected[sv] = el[-1] if el else None
        ln = next((x for x in net.splitlines() if "hub elected" in x), None)
        if ln:
            out.append("    %s: %s" % (sv, ln.strip()[:190]))
        nt = re.findall(r"U62 notice: hub (\d+) -> (\d+) .*", net)
        notice[sv] = nt[-1] if nt else None
        if not any("(crash failover" in x for x in net.splitlines() if "U62 notice" in x):
            bad.append("%s: the in-game notice is missing or not marked as a crash failover" % sv)
        drops = [
            x
            for x in net.splitlines()
            if re.search(r"GS2: peer [12] ", x) or re.search(r"U17 fast-drop: .*side=[12]\b", x)
        ]
        for x in drops[:2]:
            bad.append("%s: a survivor was dropped by a silence timer: %s" % (sv, x.strip()[-120:]))
        if "U63 failover ACTIVE" not in net or "U63 failover over" not in net:
            bad.append("%s: the game never saw the failover start and end (U63 ACTIVE / over)" % sv)
        # the timeline (wall clock): first stall -> suspect -> elected
        t_stall = _first_wall(net, "[netind] stall waiting_for=host")
        t_susp = _first_wall(net, "failover SUSPECT")
        t_elec = _first_wall(net, "hub elected")
        t_done = _first_wall(net, "failover DONE")
        if None not in (t_susp, t_elec, t_done):
            out.append(
                "    %s timeline: suspect %s, elected +%.2fs, DONE +%.2fs after the suspicion"
                % (
                    sv,
                    ("%.2fs after its first stall" % (t_susp - t_stall))
                    if t_stall
                    else "(no stall line)",
                    t_elec - t_susp,
                    t_done - t_susp,
                )
            )
    if None in elected.values():
        bad.append("a survivor never logged `hub elected`: %r" % (elected,))
    elif len(set(elected.values())) != 1:
        bad.append("the survivors elected DIFFERENT hubs: %r" % (elected,))
    else:
        hid, ep = next(iter(elected.values()))
        out.append("both survivors logged `hub elected %s epoch %s`" % (hid, ep))
        if int(hid) not in (1, 2):
            bad.append("the elected hub %s is not a survivor" % hid)
        else:
            elected_peer = SURVIVORS[int(hid) - 1] if int(hid) in (1, 2) else None
            other = SURVIVORS[0] if elected_peer == SURVIVORS[1] else SURVIVORS[1]
            if elected_peer and "this player is the hub now" not in logs[elected_peer][0]:
                bad.append("%s was elected but never logged that it became the hub" % elected_peer)
            if "re-dialled player" not in logs[other][0]:
                bad.append("%s never logged re-dialling the elected hub" % other)
    if len({v for v in notice.values()}) != 1 or None in notice.values():
        bad.append("survivors do not agree on the hub change notice: %r" % (notice,))
    for sv in SURVIVORS:
        if last[sv] - crash_step < min_post:
            bad.append(
                "%s stepped only %d past the crash (< %d)" % (sv, last[sv] - crash_step, min_post)
            )
    a, b = SURVIVORS
    txt = u53.analyze(os.path.join(det_dir, a), os.path.join(det_dir, b), min_post)
    mm = re.search(r"combined-hash=(\d+)", txt)
    common = int(mm.group(1)) if mm else 0
    ident = "ALL PAIRS IDENTICAL" in txt
    out.append(
        "survivors %s vs %s: %s, %d common hashed steps (the hub crashed at step %d)"
        % (a, b, "ALL PAIRS IDENTICAL" if ident else "NOT IDENTICAL", common, crash_step)
    )
    if not ident:
        bad.append("survivors are not ALL PAIRS IDENTICAL")
        out += ["    " + ln for ln in txt.strip().splitlines()[-8:]]
    elif common < crash_step + min_post:
        bad.append(
            "only %d common steps (< %d = crash step + %d)"
            % (common, crash_step + min_post, min_post)
        )
    lo = int(max(0.0, crash_clock - 1.0) * 1000)
    hi = int((crash_clock + 20.0) * 1000)
    for sv in SURVIVORS:
        gap, n = stall_window(os.path.join(det_dir, sv, "mh_lockstep.log"), lo, hi)
        out.append(
            "%s longest game-clock stall in [%.1fs, %.1fs]: %d ms over %d rows"
            % (sv, lo / 1000.0, hi / 1000.0, gap, n)
        )
        if n == 0:
            bad.append("%s: no lockstep rows in the crash window (cannot measure the stall)" % sv)
        elif gap > STALL_MAX_MS:
            bad.append("%s stalled %d ms around the crash (> %d)" % (sv, gap, STALL_MAX_MS))
    return (not bad), out + ["FAIL: " + b for b in bad]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("det_dir")
    ap.add_argument("--negative", action="store_true")
    ap.add_argument("--min-post", type=int, default=MIN_POST)
    a = ap.parse_args()
    try:
        ok, lines = check(a.det_dir, a.negative, a.min_post)
    except u53.Refusal as e:
        print("REFUSED: %s" % e)
        return 2
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
