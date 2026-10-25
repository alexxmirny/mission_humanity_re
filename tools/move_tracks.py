#!/usr/bin/env python3
"""
tools/move_tracks.py -- mods:M1WL v5: per-unit tile tracks out of a state record, and the metrics of the 200-unit field benchmark (check_move_bench.py kind
field200). Promoted from the one-off scripts of the U022 rounds (tmp/u022/ln/scripts/track.py, tmp/u022/heat/field_metrics.py).

  tracks(path, ids, every, player)   {"every": N, "tracks": {unit: [(step, x, y), ...]}}: where player P's units stood every Nth step, read from the
                                      `tile_objects` region of a `[desync] state_record=1` file (raw or gzip). tile_objects is 8 bytes per tile, indexed
                                      (x << 8) | y; the building u16 at +2 is the unit index when class_owner (+6) has 0x80 and its low nibble is the player.
  field_metrics(tracks, refusals)    the numbers of the 200-unit field run (the barracks stream + a field group sent to one click):
        units / exits         units seen / units that first appear after the start (the ones that left a barracks)
        exits_per100          exits per 100 steps between the first and the last exit
        last_exit             the step of the last exit, counted from the order (the first step a unit moved)
        max_tile              the most distinct units that ever stood on one tile of the map
        tiles100              tiles that 100 or more distinct units stood on
        width_median/max      units abreast in the diagonal valley box (125..136, 23..36): distinct perpendicular offsets of the units there, samples of >= 8
        arr_median/arr_worst  steps from the order to a unit's last tile change
        rev_mean / rev_ge5    tile steps exactly opposite to the previous one, per unit / units with >= 5 of them
        refusals              the last `replan refused ... (N)` count of the net log

  python tools/move_tracks.py <mh_match_state.bin[.gz]> [ids.txt|-] [every=5] [player=0]     print the metrics
  python tools/move_tracks.py --selftest
"""

import collections
import json
import math
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

W = 256
REFUSED = re.compile(r"replan refused.*\((\d+)\)\s*$")


def wd(a, b):
    x = (a - b) % W
    return x - W if x > W // 2 else x


def read_ids(path):
    if not path or path == "-":
        return None
    return {int(x) for x in open(path).read().replace(",", " ").split()}


def tracks(path, ids=None, every=5, player=0):
    import numpy as np
    import state_record as sr

    rec = sr.load(path)
    names = [r.name for r in rec.regions]
    to = rec.region_offset[names.index("tile_objects")]
    out = {}
    buf = None
    step = 0
    for c in rec.chunks:
        if isinstance(c, sr.KeyframeChunk):
            buf = bytearray(c.state)
            step = c.step
        elif isinstance(c, sr.StepChunk):
            if buf is None:
                continue
            for ridx, off, rlen, data in c.runs:
                base = rec.region_offset[ridx]
                buf[base + off : base + off + rlen] = data
            step = c.step
        else:
            continue
        if step % every:
            continue
        a = np.frombuffer(bytes(buf[to : to + 524288]), dtype=np.uint8).reshape(65536, 8)
        co = a[:, 6]
        bid = a[:, 2].astype(np.int32) | (a[:, 3].astype(np.int32) << 8)
        sel = np.nonzero(((co & 0x80) != 0) & ((co & 0x0F) == player) & (bid > 0))[0]
        for t in sel:
            u = int(bid[t])
            if ids is not None and u not in ids:
                continue
            out.setdefault(u, []).append((step, int(t) >> 8, int(t) & 255))
    return {"every": every, "tracks": out}


def refusals_of(lines):
    n = 0
    for ln in lines:
        m = REFUSED.search(ln)
        if m:
            n = max(n, int(m.group(1)))
    return n


def field_metrics(d, refusals=0):
    every = d["every"]
    t = {int(k): v for k, v in d["tracks"].items()}
    if not t:
        raise ValueError("no unit tracks (was the run made with [desync] state_record=1?)")
    first = min(v[0][0] for v in t.values())
    order = min(
        (v[i][0] for v in t.values() for i in range(1, len(v)) if v[i][1:] != v[i - 1][1:]),
        default=first,
    )
    exited = {u: t[u][0] for u in t if t[u][0][0] != first}
    arr = []
    for u, v in t.items():
        lastchg = v[0][0]
        for i in range(1, len(v)):
            if v[i][1:] != v[i - 1][1:]:
                lastchg = v[i][0]
        arr.append(lastchg - order)
    arr.sort()
    app = sorted(p[0] for p in exited.values())
    cnt = collections.Counter()
    box = collections.Counter()
    for v in t.values():
        for tile in {(p[1], p[2]) for p in v}:
            cnt[tile] += 1
            if 125 <= tile[0] <= 136 and 23 <= tile[1] <= 36:
                box[tile] += 1
    width = []
    if box:
        ln = math.hypot(11, 13)
        byt = collections.defaultdict(set)
        nby = collections.Counter()
        for v in t.values():
            for s, x, y in v:
                if 125 <= x <= 136 and 23 <= y <= 36:
                    byt[s].add(round((13 * (x - 125) - 11 * (y - 23)) / ln))
                    nby[s] += 1
        width = sorted(len(b) for s, b in byt.items() if nby[s] >= 8)
    rev = []
    for v in t.values():
        pm, r = None, 0
        for i in range(1, len(v)):
            if v[i][1:] == v[i - 1][1:]:
                continue
            m = (wd(v[i][1], v[i - 1][1]), wd(v[i][2], v[i - 1][2]))
            if pm and m[0] == -pm[0] and m[1] == -pm[1]:
                r += 1
            pm = m
        rev.append(r)
    return {
        "units": len(t),
        "exits": len(app),
        "exits_per100": round(100.0 * len(app) / max(1, app[-1] - app[0]), 2) if app else 0.0,
        "last_exit": (app[-1] - order) if app else -1,
        "max_tile": max(cnt.values()),
        "tiles100": sum(1 for c in cnt.values() if c >= 100),
        "width_median": width[len(width) // 2] if width else 0,
        "width_max": width[-1] if width else 0,
        "arr_median": arr[len(arr) // 2],
        "arr_worst": arr[-1],
        "rev_mean": round(sum(rev) / len(rev), 2),
        "rev_ge5": sum(1 for r in rev if r >= 5),
        "refusals": refusals,
    }


def selftest():
    bad = []

    def expect(name, cond):
        if not cond:
            bad.append(name)

    # three units: 1 and 2 stand at the start, 3 leaves a door at step 20; 2 walks right then back (a reversal)
    tr = {
        "every": 5,
        "tracks": {
            1: [(0, 10, 10), (5, 10, 10), (10, 11, 10), (15, 12, 10), (20, 13, 10)],
            2: [(0, 20, 20), (5, 21, 20), (10, 20, 20), (15, 21, 20), (20, 21, 20)],
            3: [(20, 30, 30)],
        },
    }
    m = field_metrics(tr, 7)
    expect("units", m["units"] == 3)
    expect("exits", m["exits"] == 1)
    expect("reversals", m["rev_mean"] == round(2 / 3, 2) and m["rev_ge5"] == 0)
    expect(
        "arrival is counted from the first move (step 5)",
        m["arr_worst"] == 15 and m["arr_median"] == 15,
    )
    expect("max_tile is distinct units", m["max_tile"] == 1)
    expect("refusals pass through", m["refusals"] == 7)
    expect(
        "refusal parse",
        refusals_of(
            [
                "[1] ; M1DT replan refused p0 unit 3 at 1,2 goal 3,4 (refusal 0) (12)",
                "x",
                "; M1DT replan refused p0 unit 4 at 1,2 goal 3,4 (refusal 1) (13)",
            ]
        )
        == 13,
    )
    try:
        field_metrics({"every": 5, "tracks": {}})
        expect("empty tracks refused", False)
    except ValueError:
        pass
    print("move_tracks --selftest: %s" % ("PASS" if not bad else "FAIL: " + "; ".join(bad)))
    return 1 if bad else 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    ids = read_ids(sys.argv[2]) if len(sys.argv) > 2 else None
    every = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    player = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    print(json.dumps(field_metrics(tracks(path, ids, every, player)), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
