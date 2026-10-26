#!/usr/bin/env python3
"""
tools/check_soa.py -- mp:DSHEAL: the shot origin does not read the render-only soldier anim_change_count.

The row soa_probe loads u022field.sav (200 soldiers) with the shipped configuration (mh.dll without libmh.dll, so the retail splice at
0x00491166 is what is under test) and the harness probe (`soa_at`) calls the game's llm_strat_unit_soldier_get_sprite_screen_pos for a few
soldier units with the head soldier's anim_change_count (+0x1b, written by the renderers only) set to 0, 4, 8 ... 252:

    ; SOA UNIT p=0 u=3 proto=12 head=7 microstep=0 saved_cnt=13
    ; SOA ORIGIN p=0 u=3 cnt=0 x=1234 y=567
    ...
    ; SOA DONE step=312 units=6

  python tools/check_soa.py --expect fixed  <run-dir>   # [net] soldier_origin_anim_fix=1: every unit's origin is the same for all 64 counters
  python tools/check_soa.py --expect retail <run-dir>   # key 0 (the reproduction arm): >= 1 unit's origin depends on the counter, else the
                                                        # fixed arm proves nothing (a sprite whose mounts never differ cannot show it)
  python tools/check_soa.py --selftest
"""

import argparse
import glob
import os
import re
import sys

MIN_UNITS = 3  # units a run must have probed, or it measured nothing
COUNTERS = 64  # 0, 4 .. 252

ORIGIN = re.compile(r"; SOA ORIGIN p=(\d+) u=(\d+) cnt=(\d+) x=(\d+) y=(\d+)")
DONE = re.compile(r"; SOA DONE step=(\d+) units=(\d+)")


def scan(path):
    """-> ({(p, u): {cnt: (x, y)}}, units reported by DONE or None)"""
    per = {}
    done = None
    with open(path, errors="replace") as fh:
        for ln in fh:
            m = ORIGIN.search(ln)
            if m:
                p, u, c, x, y = (int(g) for g in m.groups())
                per.setdefault((p, u), {})[c] = (x, y)
                continue
            m = DONE.search(ln)
            if m:
                done = int(m.group(2))
    return per, done


def find_log(run_dir):
    for pat in ("mh_match_harness.log", "*/mh_match_harness.log"):
        g = sorted(glob.glob(os.path.join(run_dir, pat)))
        if g:
            return g[-1]
    return None


def verdict(per, done, expect):
    msgs = []
    ok = True
    varying = [k for k, v in per.items() if len(set(v.values())) > 1]
    msgs.append(
        "  %d unit(s) probed (DONE says %s), %d with an origin that depends on anim_change_count"
        % (len(per), done, len(varying))
    )
    for k in sorted(varying)[:5]:
        vals = sorted(set(per[k].values()))
        msgs.append(
            "    p=%d u=%d: %d distinct origins, e.g. %s" % (k[0], k[1], len(vals), vals[:3])
        )
    short = [k for k, v in per.items() if len(v) != COUNTERS]
    if done is None or len(per) < MIN_UNITS or short:
        ok = False
        msgs.append(
            "FAIL: sample too small or truncated (units %d, DONE %s, %d unit(s) without all %d counters) -- the probe did not run"
            % (len(per), done, len(short), COUNTERS)
        )
    elif expect == "fixed":
        if varying:
            ok = False
            msgs.append(
                "FAIL: %d unit(s) still read anim_change_count into the shot origin" % len(varying)
            )
    else:
        if not varying:
            ok = False
            msgs.append(
                "FAIL: the reproduction arm shows no counter dependence: the row guards nothing"
            )
    return ok, msgs


def selftest():
    import tempfile

    def unit(p, u, f):
        out = ["; SOA UNIT p=%d u=%d proto=1 head=1 microstep=0 saved_cnt=0" % (p, u)]
        for c in range(0, 256, 4):
            x, y = f(c)
            out.append("; SOA ORIGIN p=%d u=%d cnt=%d x=%d y=%d" % (p, u, c, x, y))
        return out

    def run(lines, expect):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "mh_match_harness.log")
            with open(p, "w") as f:
                f.write("\n".join(lines) + "\n")
            return verdict(*scan(p), expect)[0]

    flat = lambda c: (100, 200)
    dep = lambda c: (100 + (c // 4) % 8 * 3, 200)
    done = "; SOA DONE step=5 units=3"
    good = unit(0, 1, flat) + unit(0, 2, flat) + unit(0, 3, flat) + [done]
    bad = unit(0, 1, flat) + unit(0, 2, dep) + unit(0, 3, flat) + [done]
    assert run(good, "fixed")
    assert not run(bad, "fixed")  # one unit reads the counter
    assert run(bad, "retail")
    assert not run(good, "retail")  # the negative arm must bite
    assert not run(unit(0, 1, flat) + [done], "fixed")  # too few units
    assert not run(good[:-1], "fixed")  # no DONE line
    assert not run(
        unit(0, 1, flat)[:-5] + unit(0, 2, flat) + unit(0, 3, flat) + [done], "fixed"
    )  # truncated unit
    print("check_soa selftest: PASS")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect", choices=("fixed", "retail"), default="fixed")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("run_dir", nargs="?")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.run_dir:
        ap.error("usage: check_soa.py [--expect fixed|retail] <session-run-dir>")
    log = find_log(a.run_dir)
    if not log:
        print("FAIL: no mh_match_harness.log under %s" % a.run_dir)
        return 1
    ok, msgs = verdict(*scan(log), a.expect)
    print("\n".join(msgs))
    print("check_soa: %s [%s arm]" % ("PASS" if ok else "FAIL", a.expect))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
