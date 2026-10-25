#!/usr/bin/env python3
"""check_res4_orders.py -- mods:RES4 stage 4: an order given through the scaled UI lands on the tile it does at 100%.

Rows res4_orders_100 and res4_orders_150 run tools/uiscripts/res4_orders_150.txt on a pinned 1280x800 screen with the camera pinned: a left
drag (owned-DirectInput absolute packets + button) selects unit 5, a right click on open ground at (400,300) orders it. The script logs
`; [sel]` and `; [goal]` lines (logsel / loggoal) after the selection and after the order. Both rows are judged against the SAME committed
record, tools/data/res4_orders_goal.json (derived from the 100% run: the click lands on tile (cam_x + 400) / 32, (cam_y + 300) / 32), so
equality of the two rows' goals follows from both equalling it.

  check_res4_orders.py <run-dir>
  check_res4_orders.py --selftest
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RECORD = os.path.join(HERE, "data", "res4_orders_goal.json")
GOAL = re.compile(r"; \[goal\] (\w+) n=(\d+) goals=([^ ]*)")
SEL = re.compile(
    r"; \[sel\] (\w+) cam=(-?\d+),(-?\d+) gest=\d+ btn=\d+ n=(\d+) bldg=(\d+) ids=([\d,]*)"
)


def problems(run_dir, record=None):
    record = record or json.load(open(RECORD))
    log = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(log):
        return ["no mh_video.log in %s" % run_dir]
    text = open(log, encoding="utf-8", errors="replace").read()
    goals = {m[0]: (int(m[1]), m[2]) for m in GOAL.findall(text)}
    sels = {m[0]: m[1:] for m in SEL.findall(text)}
    out = []
    for k in ("start", "selected", "ordered"):
        if k not in goals or k not in sels:
            out.append("no `[sel]` / `[goal]` line for %s" % k)
    if out:
        return out
    cam = (int(sels["ordered"][0]), int(sels["ordered"][1]))
    if cam != tuple(record["cam"]):
        out.append(
            "the camera is %s, the record was made at %s (the pin failed)"
            % (cam, tuple(record["cam"]))
        )
    if (
        sels["selected"][3] != "0"
        or sels["selected"][2] != "1"
        or sels["selected"][4] != str(record["unit"])
    ):
        out.append(
            "the drag selected n=%s ids=%s, wanted exactly unit %d"
            % (sels["selected"][2], sels["selected"][4], record["unit"])
        )
    if goals["selected"][1] != "%d:%d:%d" % (record["unit"], *record["selected_goal"]):
        out.append(
            "goal before the order %s, record %s" % (goals["selected"][1], record["selected_goal"])
        )
    want = "%d:%d:%d" % (record["unit"], *record["ordered_goal"])
    if goals["ordered"][1] != want:
        out.append(
            "goal after the order %s, wanted %s (the tile the order has at 100%%)"
            % (goals["ordered"][1], want)
        )
    return out


def selftest():
    import tempfile

    rec = {"cam": [1312, 1664], "unit": 5, "selected_goal": [61, 1], "ordered_goal": [53, 61]}
    good = (
        "; [sel] start cam=1312,1664 gest=0 btn=0 n=0 bldg=1 ids=\n; [goal] start n=0 goals= side=0\n"
        "; [sel] selected cam=1312,1664 gest=0 btn=0 n=1 bldg=0 ids=5\n; [goal] selected n=1 goals=5:61:1 side=0\n"
        "; [sel] ordered cam=1312,1664 gest=0 btn=0 n=1 bldg=0 ids=5\n; [goal] ordered n=1 goals=5:53:61 side=0\n"
    )
    d = tempfile.mkdtemp()
    f = os.path.join(d, "mh_video.log")

    def run(text):
        open(f, "w").write(text)
        return problems(d, rec)

    assert not run(good)
    assert run(good.replace("5:53:61", "5:54:61"))
    assert run(
        good.replace("n=1 bldg=0 ids=5\n; [goal] selected", "n=2 bldg=0 ids=5,6\n; [goal] selected")
    )
    assert run(good.replace("cam=1312,1664", "cam=1344,1664"))
    assert run(good.replace("; [goal] ordered n=1 goals=5:53:61 side=0\n", ""))
    print("check_res4_orders selftest: PASS")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if not argv:
        print(__doc__)
        return 2
    probs = problems(argv[0])
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print("PASS: the order through the pointer reached the recorded goal tile")
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
