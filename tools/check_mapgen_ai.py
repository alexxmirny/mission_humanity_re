#!/usr/bin/env python3
"""check_mapgen_ai.py -- mods:MAPGEN1 W4: the AI-suitability readout of one `mapgen_ai.txt` match (a generated map, 7-8 AIs).

Reads the session dir the registry's post_check_session hands it. Evidence, all already logged by the game / harness:
  * landing, per player: `; presence_lost player=P mode=0 gate=alive ... caller=0x00487f41` in mh_net.log is the mother's
    deploy teardown (the building exists from then on); a player with `en=1` in the harness `; AIP` lines and NO such line is
    STRANDED (a blocked deploy: the mother never lands -- on a generated map that is a start site the AI cannot use).
  * development over time, per AI: `; AIP pN ... uc= us= bldg= groups=` every ai_probe_step sim steps (steps are 50/s).
  * the sim clock keeps advancing: the last probe's acc_ms (the AI tick accumulator, game ms) must reach --min-clock-s.
  * ore: only `; AISTATE` (one shot, aistate_probe_at) carries `mine=` (ai_mine_yield_by_resource) and `res=` (resource_spent);
    there is NO per-tick ore-collected counter, so "mined ore" is read as resource_spent / mine yield non-zero.
  * `MV STALL` lines belong to the recorded-move probe (mv_* knobs) and never appear in AI play: not judged here.
FAILS (exit 1) when: no AIPROBE (harness/probe absent), or the clock stopped short of --min-clock-s. Strandings are WARNINGS
unless --strict (the row exists to SHOW them; a generator problem is not a crash).
Usage: python tools/check_mapgen_ai.py [--strict] [--min-clock-s N] <session-dir>
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_cancel_task  # noqa: E402
import check_storage_purge  # noqa: E402

AIP = re.compile(r";\s+AIP p(\d+) (.*)")
PROBE = re.compile(r"; AIPROBE step=(\d+)")
LAND = re.compile(r"presence_lost player=(\d+) mode=0 gate=alive .*caller=0x00487f41 .*gclk=(\d+)")
STATE = re.compile(r"; AISTATE p=(\d+) ai=1 .*?bldg=(\d+) btypes=(\d+) .*?res=(\S+) .*?mine=(\S+)")


def analyse(net, harness):
    land = {}
    for ln in net:
        m = LAND.search(ln)
        if m:
            land.setdefault(int(m.group(1)), int(m.group(2)))
    series, step = {}, None
    for ln in harness:
        m = PROBE.search(ln)
        if m:
            step = int(m.group(1))
            continue
        m = AIP.search(ln)
        if m and step is not None:
            kv = dict(x.split("=", 1) for x in m.group(2).split() if "=" in x)
            series.setdefault(int(m.group(1)), []).append(
                (step, int(kv["en"]), int(kv["bldg"]), int(kv["us"]), int(kv["acc_ms"]))
            )
    series = {p: sorted(set(r)) for p, r in series.items()}  # both dirs carry the same log
    state = {}
    for ln in harness:
        m = STATE.search(ln)
        if m:
            state[int(m.group(1))] = m.groups()[1:]
    return land, series, state


def run(d, strict, min_clock_s):
    net = check_cancel_task.net_log_lines(d)
    harness = check_storage_purge.harness_log_lines(d)
    land, series, state = analyse(net, harness)
    bad, warn = [], []
    if not series:
        bad.append(
            "no `; AIP` lines: ai_probe_step missing from the row, or the harness never armed"
        )
    clock = max((r[-1][4] for r in series.values()), default=0) / 1000.0
    if series and clock < min_clock_s:
        bad.append("the sim clock stopped at %.0f s (< %d): hang or crash" % (clock, min_clock_s))
    print("  last probe clock %.0f s, %d player(s) with AIP lines" % (clock, len(series)))
    for p, rows in sorted(series.items()):
        if not rows[-1][1]:
            continue  # a human slot (en=0)
        landed = p in land
        b = [r[2] for r in rows]
        stuck = len(b) >= 4 and len(set(b[-4:])) == 1 and landed
        tl = " ".join(
            "%d:%d/%d" % (r[0] // 50, r[2], r[3]) for r in rows[:: max(1, len(rows) // 8)]
        )
        print(
            "  p%d %s  bldg %d->%d units(last) %d%s  [t_s:bldg/units] %s"
            % (
                p,
                ("landed gclk=%d" % land[p]) if landed else "STRANDED (never landed)",
                b[0],
                b[-1],
                rows[-1][3],
                "  PLATEAU(last 4 probes)" if stuck else "",
                tl,
            )
        )
        if p in state:
            print("      AISTATE bldg=%s btypes=%s res_spent=%s mine_yield=%s" % state[p])
        if not landed:
            warn.append("p%d never landed" % p)
        elif stuck:
            warn.append("p%d bldg count flat over the last 4 probes" % p)
    if strict:
        bad += warn
    else:
        for w in warn:
            print("  WARN: " + w)
    for b in bad:
        print("  check_mapgen_ai: " + b)
    print("check_mapgen_ai: %s" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


def main(argv):
    strict = "--strict" in argv
    mc = 500
    if "--min-clock-s" in argv:
        mc = int(argv[argv.index("--min-clock-s") + 1])
    args = [
        a
        for i, a in enumerate(argv)
        if not a.startswith("--") and not (i and argv[i - 1] == "--min-clock-s")
    ]
    if len(args) != 1:
        print(__doc__)
        return 2
    return run(args[0], strict, mc)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
