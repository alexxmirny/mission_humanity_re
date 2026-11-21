#!/usr/bin/env python3
"""
tools/check_imgui_heat.py -- mods:M1WL v3: the post_check of the imgui_heat UI row (the ImGui "Stream heat" section + heat map).

The row's script clicks the section's `scale by mates` checkbox through the real DI-filter path (rawmouse abs / down / up on the open panel), so the live
setter runs end to end: panel -> mh::gameplay::heat_set_live -> the move_stream_heat_scale hook -> the splice's parameters. The proofs are in the logs:

  mh_net.log    `; [heat] live set cost=2 window=120 mates=3 scale=0 retrace=1 turn=4 cap=16 margin=50 at step S on=1`   (exactly one: one click, one change; the log line the brief asks for)
                `; M1WL heat p.. u.. mates=.. tiles=..`                                 (the rule fired: a plan was priced, so the snapshot exists)

The map itself is a presented image: the row's bcapture baselines are its oracle.

  python tools/check_imgui_heat.py <session-run-dir>
"""

import argparse
import os
import re
import sys

LIVE = re.compile(
    r"; \[heat\] live set cost=(\d+) window=(\d+) mates=(\d+) scale=(\d+) retrace=(\d+) turn=(\d+) cap=(\d+) margin=(\d+) at step (\d+) on=(\d)"
)
FIRED = re.compile(r"; M1WL heat p\d+ u\d+ mates=(\d+) tiles=(\d+)")


def read(path):
    try:
        return open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []


def find(run_dir, name):
    """The log of the run: the session directory itself or one level below it (the lane's per-run dirs)."""
    p = os.path.join(run_dir, name)
    if os.path.exists(p):
        return read(p)
    out = []
    for d in sorted(os.listdir(run_dir)) if os.path.isdir(run_dir) else []:
        q = os.path.join(run_dir, d, name)
        if os.path.exists(q):
            out += read(q)
    return out


def judge(net):
    f = []
    live = [m for m in map(LIVE.search, net) if m]
    if len(live) != 1:
        f.append("expected exactly one `; [heat] live set` line (one click), found %d" % len(live))
    else:
        scale, on = int(live[0].group(4)), int(live[0].group(10))
        # The fixture explicitly starts enabled/scaled; the real click must
        # disable scaling without disabling the heat map.
        if scale != 0 or on != 1:
            f.append("the live click must leave scale=0 and on=1, got %d / %d" % (scale, on))
    if not any(FIRED.search(x) for x in net):
        f.append(
            "no `; M1WL heat` line: the stream heat never priced a plan, so the map had nothing to show"
        )
    return f


def selftest():
    good_net = [
        "; [heat] live set cost=2 window=120 mates=3 scale=0 retrace=1 turn=4 cap=16 margin=50 at step 120 on=1",
        "; M1WL heat p0 u20 mates=10 tiles=94",
    ]
    bad = 0

    def case(name, net, want_red):
        nonlocal bad
        red = bool(judge(net))
        bad += red != want_red
        print("  %-52s %s" % (name, "ok" if red == want_red else "WRONG"))

    case("good run is green", good_net, False)
    case("no live line is red", good_net[1:], True)
    case("two live lines is red", good_net[:1] * 2 + good_net[1:], True)
    case(
        "scale did not switch off", [good_net[0].replace("scale=0", "scale=1")] + good_net[1:], True
    )
    case("heat off is red", [good_net[0].replace("on=1", "on=0")] + good_net[1:], True)
    case("no plan priced is red", good_net[:1], True)
    print("check_imgui_heat --selftest: %s" % ("PASS" if not bad else "FAIL (%d)" % bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("run_dir", nargs="?")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.run_dir:
        ap.error("usage: check_imgui_heat.py <session-run-dir>")
    fails = judge(find(a.run_dir, "mh_net.log"))
    for x in fails:
        print("FAIL " + x)
    print("check_imgui_heat: %s" % ("FAIL (%d clause(s))" % len(fails) if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
