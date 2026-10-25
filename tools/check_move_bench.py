#!/usr/bin/env python3
"""
tools/check_move_bench.py -- TL-MOVE-BENCH: the movement benchmark's post-check and baseline keeper.

The 20-order benchmark replays the user's 20 recorded move orders (tools/uiscripts/m1_moves.csv) on the 'Unit movement test' save (5-kofu-c) through the
harness probe `[harness] mv_replay` (observe-only; pin_wallclock, libmh.dll absent). The rows that run it (tools/uiscripts/registry.yaml):

  move_bench          the SHIPPED configuration: the movement preset on (improved_movement / group_movement / improved_combat = 1), every other key at its
                      shipped default. The gated arm (default tier).
  move_bench_retail   the reference arm: the three masters 0. tier: release -- it is the control the shipped arm is read against.

This checker reads the `; MV` lines of the row's mh_match_harness.log, computes the metrics below, prints the table every run and compares each arm with
its entry in tools/data/move_bench_baseline.json. A metric WORSE than its baseline by more than its tolerance is red; BETTER is green with a note.

  arrived / near / stopped short / gave up / interrupted / dead   summed over the per-order `; MV ORDERSUM` lines (80 unit-orders)
  completed          unit-orders that ended ARRIVED or ARRIVED-NEAR (tools/m1_detours.py summary: s_click > 0)
  median / max ratio  actual walked length / shortest path to the click tile (m1_detours: completed unit-orders only)
  excess tiles       total (actual - shortest) over the completed unit-orders
  stall episodes     summed ORDERSUM `stalls`
  settle steps       summed ORDERSUM `t_last_settle` (not a clean metric when orders are interrupted -- read it with the others)
  last arrival       the step of the last ARRIVED / ARRIVED-NEAR verdict

Usage:
  python tools/check_move_bench.py --arm ship [--log FILE] <session-run-dir>      the post_check form (the dir the rig hands over)
  python tools/check_move_bench.py --arm ship --update <session-run-dir>          rewrite that arm's baseline, print old -> new
  python tools/check_move_bench.py --selftest                                     the comparator's and the parser's negative cases (no rig)

Re-baselining is deliberate: a better result prints "improved: re-baseline with --update" and stays green; it never rewrites the file by itself.

EXTENSION POINT (mods:M1WL's u022_stream row, and any later movement scenario). A baseline entry names its `kind`; a kind is one extractor (log lines ->
{metric: number}) plus one SPEC (metric -> direction, absolute and relative tolerance, label) registered in KINDS below. To add a scenario:
  1. write `extract_<kind>(lines, path)` returning the metrics dict (every key listed in SPEC);
  2. add `KINDS["<kind>"] = (extract, SPEC)` -- SPEC rows are (key, direction, abs_tol, rel_tol, label), direction "lower" / "higher" is the GOOD direction;
  3. register the row with `post_check: [tools/check_move_bench.py, --arm, <entry-name>]` and run `--update --arm <entry-name> <dir>` once on an idle rig.
A per-entry `tol` object in the baseline ({metric: {"abs": a, "rel": r}}) overrides the SPEC for that entry.
"""

import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(REPO, "tools", "data", "move_bench_baseline.json")
sys.path.insert(0, os.path.join(REPO, "tools"))

ORDERSUM = re.compile(
    r"; MV ORDERSUM ord=(\d+) goal=(-?\d+),(-?\d+) units=(\d+) arrived=(\d+) near=(\d+) stopped_short=(\d+) gave_up=(\d+) interrupted=(\d+) "
    r"dead=(\d+) t_first_arrive=(-?\d+) t_last_settle=(-?\d+) max_spread=(\d+) stalls=(\d+)"
)
VERDICT = re.compile(r"; MV VERDICT ord=(\d+) u=(\d+) ([A-Z-]+) step=(\d+)")
DONE = re.compile(r"; MV DONE step=(\d+)")

# The tolerances. Measured run-to-run spread (idle, 3 runs per arm): ZERO -- the replay is pinned (pin_wallclock) and every
# arm gave byte-identical `; MV` lines. Under load the spread was seen to be one micro-step of drift in a unit's arrival, so the
# tolerances below are sized for that class of drift, not for a different game: a handful of steps on the step-valued metrics,
# +-1 on a count, a few percent on a ratio. Measured idle and under 8-core load: the spread was zero (byte-identical traces).
# (key, good direction, absolute tolerance, relative tolerance, label); a metric is red when it is worse than baseline by more than
# max(abs, rel * |baseline|).
SPEC_REPLAY20 = [
    ("units", "equal", 0, 0, "unit-orders"),
    ("orders", "equal", 0, 0, "orders replayed"),
    ("completed", "higher", 1, 0.0, "completed unit-orders (arrived + near)"),
    ("arrived", "higher", 1, 0.0, "arrived on the click tile"),
    ("near", "info", 0, 0, "arrived near"),
    ("stopped_short", "lower", 0, 0.0, "stopped short"),
    ("gave_up", "lower", 0, 0.0, "gave up"),
    ("interrupted", "lower", 2, 0.0, "interrupted by the next order"),
    ("dead", "lower", 0, 0.0, "units dead"),
    ("median_ratio", "lower", 0.03, 0.0, "median detour ratio"),
    ("max_ratio", "lower", 0.10, 0.0, "max detour ratio"),
    ("excess_tiles", "lower", 5.0, 0.05, "total excess tiles"),
    ("stall_episodes", "lower", 0, 0.0, "stall episodes"),
    ("settle_steps", "lower", 100, 0.02, "sum of settle steps"),
    ("last_arrival", "lower", 100, 0.0, "last arrival step"),
]


def _counts(lines):
    """The ORDERSUM / VERDICT part: pure, no map needed."""
    tot = dict(
        arrived=0,
        near=0,
        stopped_short=0,
        gave_up=0,
        interrupted=0,
        dead=0,
        stalls=0,
        settle=0,
        units=0,
    )
    orders = set()
    last_arr = -1
    done = None
    for ln in lines:
        m = ORDERSUM.search(ln)
        if m:
            g = [int(x) for x in m.groups()]
            orders.add(g[0])
            tot["units"] += g[3]
            for k, v in zip(
                ("arrived", "near", "stopped_short", "gave_up", "interrupted", "dead"), g[4:10]
            ):
                tot[k] += v
            tot["settle"] += max(g[11], 0)
            tot["stalls"] += g[13]
            continue
        m = VERDICT.search(ln)
        if m and m.group(3) in ("ARRIVED", "ARRIVED-NEAR"):
            last_arr = max(last_arr, int(m.group(4)))
            continue
        m = DONE.search(ln)
        if m:
            done = int(m.group(1))
    return tot, len(orders), last_arr, done


def _detours(path):
    """(completed, median, max, excess) from tools/m1_detours.py over the same log."""
    import m1_detours  # tools/ is on sys.path

    rows = m1_detours.analyze(m1_detours.Log(path))
    done = [r for r in rows if r["verdict"] in ("ARRIVED", "ARRIVED-NEAR") and r["s_click"] > 0]
    ratios = sorted(r["A"] / r["s_click"] for r in done)
    if not ratios:
        return 0, 99.0, 99.0, 9999.0
    return (
        len(done),
        ratios[len(ratios) // 2],
        ratios[-1],
        sum(r["A"] - r["s_click"] for r in done),
    )


def extract_replay20(lines, path):
    tot, n_orders, last_arr, done = _counts(lines)
    if done is None:
        raise ValueError(
            "no `; MV DONE` line: the probe never finished (the process was cut before the last order's tail)"
        )
    completed, med, mx, excess = _detours(path)
    return {
        "units": tot["units"],
        "orders": n_orders,
        "completed": completed,
        "arrived": tot["arrived"],
        "near": tot["near"],
        "stopped_short": tot["stopped_short"],
        "gave_up": tot["gave_up"],
        "interrupted": tot["interrupted"],
        "dead": tot["dead"],
        "median_ratio": round(med, 3),
        "max_ratio": round(mx, 3),
        "excess_tiles": round(excess, 1),
        "stall_episodes": tot["stalls"],
        "settle_steps": tot["settle"],
        "last_arrival": last_arr,
    }


# ---------------------------------------------------------------------------------------------------------
# mods:M1WL v5: the narrow-path and stream kinds (user 2026-10-25: "find the tests for narrow path that you made - use them in benchmarks too")
# ---------------------------------------------------------------------------------------------------------
M1WL_MARGIN = re.compile(r"; M1WL margin p\d+ u\d+ result=(\d+) ")
M1WL_RETRACE = re.compile(r"; M1WL retrace p\d+ u\d+ result=")
FC_TOTAL = re.compile(
    r"; FC TOTAL case=(\d+) steps=(\d+) n=(\d+) tiles=(\d+) straight=(\d+) moved=(\d+) maxtiles=(\d+) maxcheb=(\d+) dup=(\d+) maxratio=(\d+) onwall=(\d+) idle=(\d+)"
)
NARROW_ORDERS = 6
NARROW_DETOUR_TILES = 10  # a unit-order that walked this many tiles more than the shortest path to the click tile is a detour


def _read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read().splitlines()
    except OSError:
        return []


def _sibling(path, name):
    return os.path.join(os.path.dirname(path), name)


def extract_narrow16(lines, path):
    """narrow16: the user's rc10 narrow-path orders (16 soldiers, six recorded orders through the Nortus passage): per order the tiles walked, the excess over the
    shortest path to the click tile (completed unit-orders) and the detours (completed unit-orders with an excess of NARROW_DETOUR_TILES or more)."""
    import m1_detours  # tools/ is on sys.path

    tot, n_orders, last_arr, done = _counts(lines)
    if done is None:
        raise ValueError("no `; MV DONE` line: the probe never finished")
    rows = m1_detours.analyze(m1_detours.Log(path))
    out = {"orders": n_orders, "units": tot["units"]}
    walked = excess = detours = completed = 0
    for o in range(NARROW_ORDERS):
        w = e = d = 0
        for r in rows:
            if r["ord"] != o:
                continue
            w += int(round(r["A"]))
            if r["verdict"] in ("ARRIVED", "ARRIVED-NEAR") and r["s_click"] != float("inf"):
                ex = r["A"] - r["s_click"]
                e += max(0, int(round(ex)))
                d += 1 if ex >= NARROW_DETOUR_TILES else 0
                completed += 1
        out["walked_o%d" % (o + 1)] = w
        out["excess_o%d" % (o + 1)] = e
        out["detours_o%d" % (o + 1)] = d
        walked, excess, detours = walked + w, excess + e, detours + d
    net = _read(_sibling(path, "mh_net.log"))
    margin = [int(m.group(1)) for m in map(M1WL_MARGIN.search, net) if m]
    retrace = sum(1 for ln in net if M1WL_RETRACE.search(ln))
    out.update(
        walked=walked,
        excess=excess,
        detours=detours,
        completed=completed,
        heated_plans=max(len(margin), retrace),
        margin_kept=sum(1 for r in margin if r == 2),
        last_arrival=last_arr,
    )
    return out


def _narrow_spec():
    spec = [
        ("orders", "equal", 0, 0, "orders replayed"),
        ("units", "equal", 0, 0, "unit-orders"),
        (
            "detours",
            "lower",
            0,
            0.0,
            "detours (excess >= %d tiles), all orders" % NARROW_DETOUR_TILES,
        ),
        ("walked", "lower", 15, 0.03, "tiles walked, all orders"),
        ("excess", "lower", 8, 0.10, "excess tiles over the shortest path, all orders"),
        ("completed", "higher", 1, 0.0, "completed unit-orders"),
        ("heated_plans", "info", 0, 0, "heated plans (the heat fired)"),
        ("margin_kept", "info", 0, 0, "margin kept (the unheated route written)"),
        ("last_arrival", "lower", 100, 0.0, "last arrival step"),
    ]
    for o in range(1, NARROW_ORDERS + 1):
        spec += [
            ("detours_o%d" % o, "lower", 0, 0.0, "  order %d detours" % o),
            ("walked_o%d" % o, "lower", 8, 0.04, "  order %d tiles walked" % o),
            ("excess_o%d" % o, "lower", 5, 0.10, "  order %d excess tiles" % o),
        ]
    return spec


SPEC_NARROW16 = _narrow_spec()


def extract_fc_hopwall(lines, path):
    """fc_hopwall: the F17 case of the m1h fixtures (a 17-tile wall with a one-tile gap, 16 idle units re-ordered while they hop through it): tiles walked by the
    group and by the worst unit. tools/check_m1h.py F17 is red above 100 / 16; the baseline is the measured 92 / 13 (v5 defaults) and the allowance is the distance to those limits (8 / 3)."""
    for ln in reversed(lines):
        m = FC_TOTAL.search(ln)
        if m and int(m.group(1)) == 5:
            g = [int(x) for x in m.groups()]
            return {
                "units": g[2],
                "tiles": g[3],
                "worst_unit": g[6],
                "dup": g[8],
                "moving": g[2] - g[11],
            }
    raise ValueError("no `; FC TOTAL case=5` line: the fc probe never finished the hop-wall case")


SPEC_FC_HOPWALL = [
    ("units", "equal", 0, 0, "units"),
    ("tiles", "lower", 8, 0.0, "tiles walked by the group (red above 100)"),
    ("worst_unit", "lower", 3, 0.0, "tiles walked by the worst unit (red above 16)"),
    ("dup", "lower", 0, 0.0, "units sharing an end tile"),
    ("moving", "lower", 0, 0.0, "units still moving at the end"),
]


def extract_field200(lines, path):
    """field200: the user's 200-unit move on units-tests.sav (a barracks stream + a field group sent to one click), read from the state record. The field is CHAOTIC
    (the same code at a different heat setting moves these numbers by 15% either way), so the SPEC is thresholds, not equality: see SPEC_FIELD200."""
    import move_tracks

    state = None
    for name in ("mh_match_state.bin.gz", "mh_match_state.bin"):
        p = _sibling(path, name)
        if os.path.isfile(p):
            state = p
            break
    if state is None:
        raise ValueError(
            "no mh_match_state.bin[.gz] next to %s (the row needs [desync] state_record=1)" % path
        )
    ids = os.path.join(REPO, "tools", "data", "u022_field200_ids.txt")
    d = move_tracks.tracks(state, move_tracks.read_ids(ids), 5, 0)
    return move_tracks.field_metrics(
        d, move_tracks.refusals_of(_read(_sibling(path, "mh_net.log")))
    )


# The tolerances are sized against the chaos measured on this field (rc10, v5): exits per 100 steps 6.5 .. 7.0 and the last exit within +-10% between
# neighbouring settings; the heat-off control is 2.05 exits per 100 steps, a 70% drop, and 100+ tiles of max_tile mean one lane. A regression has to move a
# number by more than the noise to go red.
SPEC_FIELD200 = [
    ("units", "equal", 0, 0, "units tracked"),
    ("exits", "higher", 3, 0.0, "units that left a barracks"),
    ("exits_per100", "higher", 0.5, 0.15, "exits per 100 steps"),
    ("last_exit", "lower", 150, 0.15, "last exit (steps after the order)"),
    ("max_tile", "lower", 12, 0.15, "most units on one tile"),
    ("tiles100", "lower", 6, 0.0, "tiles 100+ units stood on"),
    ("width_median", "higher", 2, 0.0, "median width in the valley box (tiles abreast)"),
    ("width_max", "info", 0, 0, "max width in the valley box"),
    ("arr_median", "lower", 400, 0.15, "median arrival (steps after the order)"),
    ("arr_worst", "lower", 600, 0.15, "worst arrival (steps after the order)"),
    ("rev_mean", "lower", 0.5, 0.0, "reversals per unit"),
    ("rev_ge5", "lower", 3, 0.0, "units with >= 5 reversals"),
    ("refusals", "lower", 150, 0.0, "replans refused by the detour cap"),
]


def extract_stream(lines, path):
    """stream: the u022_stream row's `; ST TOTAL` line (the barracks stream of 45 soldiers sent to open ground): the key-1 step in the shipped splice / libmh twin."""
    import check_u022_stream as cus

    t = cus.parse_total(lines)
    if t is None:
        raise ValueError("no `; ST TOTAL` line: the stream fixture did not finish")
    return {
        k: t[k]
        for k in (
            "n",
            "exited",
            "stuck",
            "maxtile",
            "lanes",
            "exit_per1000",
            "med_arrive",
            "last_arrive",
            "rev_x100",
            "rev_max",
        )
    }


SPEC_STREAM = [
    ("n", "equal", 0, 0, "units"),
    ("exited", "higher", 0, 0.0, "units that left the barracks"),
    ("stuck", "lower", 0, 0.0, "units that never reached the goal"),
    ("maxtile", "lower", 3, 0.0, "most units on one tile"),
    ("lanes", "higher", 1, 0.0, "lanes at the gate line"),
    ("exit_per1000", "higher", 1, 0.0, "exits per 1000 steps"),
    ("med_arrive", "lower", 100, 0.05, "median arrival"),
    ("last_arrive", "lower", 100, 0.05, "last arrival"),
    ("rev_x100", "lower", 10, 0.0, "reversals per unit x 100"),
    ("rev_max", "lower", 2, 0.0, "worst unit's reversals"),
]


KINDS = {
    "replay20": (extract_replay20, SPEC_REPLAY20),
    "narrow16": (extract_narrow16, SPEC_NARROW16),
    "today6": (
        extract_narrow16,
        SPEC_NARROW16,
    ),  # M1WL v6: the user's 2026-10-25 rc11 scene (6 orders, 16 soldiers, Nortus 3); same table, its own baseline
    "fc_hopwall": (extract_fc_hopwall, SPEC_FC_HOPWALL),
    "field200": (extract_field200, SPEC_FIELD200),
    "stream": (extract_stream, SPEC_STREAM),
}


def kind_of(arm):
    """The baseline kind of an arm name: the name itself (narrow16, today6, fc_hopwall, field200), its prefix (stream_ship -> stream), else the 20-order replay."""
    if arm in KINDS:
        return arm
    return arm.split("_")[0] if arm.split("_")[0] in KINDS else "replay20"


def compare(spec, base, now, tol_override=None):
    """-> (rows, red, improved). rows = (label, base, now, delta, allowed, verdict)."""
    rows, red, improved = [], [], []
    for key, direction, a, r, label in spec:
        b, n = base.get(key), now.get(key)
        if b is None or n is None:
            rows.append((label, b, n, None, None, "MISSING"))
            red.append(key)
            continue
        if tol_override and key in tol_override:
            a, r = tol_override[key].get("abs", a), tol_override[key].get("rel", r)
        allow = max(a, r * abs(b))
        d = n - b
        if direction == "info":
            verdict = "info"
        elif direction == "equal":
            verdict = "ok" if d == 0 else "RED"
        else:
            worse = -d if direction == "higher" else d  # >0 = worse
            if worse > allow + 1e-9:
                verdict = "RED"
            elif worse < -1e-9 and -worse > allow + 1e-9:
                verdict = "improved"
            else:
                verdict = "ok"
        if verdict == "RED":
            red.append(key)
        if verdict == "improved":
            improved.append(key)
        rows.append((label, b, n, d, allow, verdict))
    return rows, red, improved


def fmt(x):
    if x is None:
        return "-"
    if isinstance(x, float):
        return ("%.3f" % x).rstrip("0").rstrip(".")
    return str(x)


def table(arm, rows):
    out = [
        "move_bench[%s]" % arm,
        "  %-40s %10s %10s %9s %8s  %s" % ("metric", "baseline", "now", "delta", "allowed", ""),
    ]
    for label, b, n, d, allow, v in rows:
        out.append(
            "  %-40s %10s %10s %9s %8s  %s"
            % (
                label,
                fmt(b),
                fmt(n),
                ("%+g" % round(d, 3)) if d is not None else "-",
                fmt(allow),
                "" if v in ("ok", "info") else v,
            )
        )
    return "\n".join(out)


def load_baseline(path=BASELINE):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def find_log(target):
    if os.path.isfile(target):
        return target
    for name in ("mh_match_harness.log", "mh_harness.log"):
        p = os.path.join(target, name)
        if os.path.isfile(p):
            return p
    return None


def measure(entry, log_path):
    kind = entry.get("kind", "replay20")
    extract, spec = KINDS[kind]
    with open(log_path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    return extract(lines, log_path), spec


def run(arm, target, update, baseline_path=BASELINE):
    base = load_baseline(baseline_path)
    arms = base.setdefault("arms", {})
    if arm not in arms:
        if not update:
            print(
                "check_move_bench: RED -- no baseline entry '%s' in %s (known: %s); run with --update once on an idle rig"
                % (arm, baseline_path, ", ".join(sorted(arms)) or "none")
            )
            return 1
        arms[arm] = {"kind": kind_of(arm)}
    entry = arms[arm]
    log = find_log(target)
    if not log:
        print("check_move_bench: RED -- no mh_match_harness.log / mh_harness.log under %s" % target)
        return 1
    try:
        now, spec = measure(entry, log)
    except (ValueError, OSError) as e:
        print("check_move_bench: RED -- %s: %s" % (log, e))
        return 1
    if update:
        old = entry.get("metrics", {})
        rows, _, _ = compare(spec, old or now, now)
        print(table(arm, rows))
        entry["metrics"] = now
        with open(baseline_path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(base, fh, indent=2, sort_keys=False)
            fh.write("\n")
        changed = [k for k in now if old.get(k) != now[k]]
        print(
            "check_move_bench: baseline '%s' rewritten; changed: %s"
            % (
                arm,
                ", ".join("%s %s -> %s" % (k, fmt(old.get(k)), fmt(now[k])) for k in changed)
                or "nothing",
            )
        )
        return 0
    rows, red, improved = compare(spec, entry["metrics"], now, entry.get("tol"))
    print(table(arm, rows))
    if red:
        print(
            "check_move_bench: RED -- worse than the baseline beyond tolerance: %s" % ", ".join(red)
        )
        return 1
    if improved:
        print(
            "check_move_bench: green. improved: %s -- re-baseline with --update (python tools/check_move_bench.py --arm %s --update <run-dir>)"
            % (", ".join(improved), arm)
        )
    else:
        print("check_move_bench: green, within tolerance of the baseline")
    return 0


# ---------------------------------------------------------------------------------------------------------
# selftest: planted negatives, no rig
# ---------------------------------------------------------------------------------------------------------
def _fake_log(stalls=0, dead=0, arrived=1, last_step=900, with_done=True):
    L = []
    for o in range(3):
        L.append(
            "; MV ORDERSUM ord=%d goal=1,2 units=4 arrived=%d near=2 stopped_short=0 gave_up=0 interrupted=1 dead=%d t_first_arrive=100 t_last_settle=%d max_spread=2 stalls=%d (self_marshal=0)"
            % (o, arrived, dead if o == 0 else 0, 200 + o, stalls if o == 0 else 0)
        )
        L.append(
            "; MV VERDICT ord=%d u=1 ARRIVED step=%d rel=%d start=1,1"
            % (o, last_step - o, last_step - o)
        )
    L.append("; MV VERDICT ord=2 u=2 INTERRUPTED step=5000 rel=5000 start=1,1")
    if with_done:
        L.append("; MV DONE step=26000 rel=26000 oob=0")
    return L


def selftest():
    bad = []

    def expect(name, cond):
        if not cond:
            bad.append(name)

    tot, n, last, done = _counts(_fake_log())
    expect(
        "parse orders", n == 3 and tot["units"] == 12 and tot["arrived"] == 3 and tot["near"] == 6
    )
    expect("parse settle sum", tot["settle"] == 200 + 201 + 202)
    expect("parse last arrival ignores non-arrival verdicts", last == 900 and done == 26000)
    expect("done absent", _counts(_fake_log(with_done=False))[3] is None)
    expect("stalls summed", _counts(_fake_log(stalls=3))[0]["stalls"] == 3)
    expect("dead summed", _counts(_fake_log(dead=1))[0]["dead"] == 1)
    try:
        extract_replay20(_fake_log(with_done=False), "x")
        expect("unfinished run refused", False)
    except ValueError:
        pass

    base = dict(
        units=80,
        orders=20,
        completed=45,
        arrived=12,
        near=33,
        stopped_short=3,
        gave_up=0,
        interrupted=32,
        dead=0,
        median_ratio=1.06,
        max_ratio=2.15,
        excess_tiles=94.2,
        stall_episodes=0,
        settle_steps=10277,
        last_arrival=22000,
    )
    rows, red, imp = compare(SPEC_REPLAY20, base, dict(base))
    expect("identical is green", not red and not imp)
    # every gated metric, planted worse by 2x its allowance + 1, must go red
    for key, direction, a, r, _l in SPEC_REPLAY20:
        if direction in ("info",):
            continue
        now = dict(base)
        step = 2 * max(a, r * abs(base[key])) + (1 if isinstance(base[key], int) else 0.01)
        now[key] = (
            base[key] + (step if direction != "higher" else -step)
            if direction != "equal"
            else base[key] + 1
        )
        _, red, _ = compare(SPEC_REPLAY20, base, now)
        expect("planted worse %s goes red" % key, key in red)
        # inside the tolerance (one micro-step of drift): green
        if direction in ("lower", "higher") and max(a, r * abs(base[key])) > 0:
            now = dict(base)
            allow = max(a, r * abs(base[key]))
            now[key] = base[key] + (allow if direction == "lower" else -allow)
            _, red, _ = compare(SPEC_REPLAY20, base, now)
            expect("within tolerance %s stays green" % key, key not in red)

    # mods:M1WL v5: the new kinds. Every gated metric planted worse must go red; a plant inside the allowance stays green.
    def spec_cases(name, spec, base_):
        _, red_, _ = compare(spec, base_, dict(base_))
        expect("%s identical is green" % name, not red_)
        for key, direction, a, r, _l in spec:
            if direction == "info":
                continue
            now_ = dict(base_)
            step = 2 * max(a, r * abs(base_[key])) + (1 if isinstance(base_[key], int) else 0.01)
            now_[key] = (
                base_[key] + 1
                if direction == "equal"
                else (base_[key] - step if direction == "higher" else base_[key] + step)
            )
            _, red_, _ = compare(spec, base_, now_)
            expect("%s planted worse %s goes red" % (name, key), key in red_)

    spec_cases(
        "narrow16",
        SPEC_NARROW16,
        dict(
            {
                "orders": 6,
                "units": 96,
                "detours": 0,
                "walked": 4000,
                "excess": 120,
                "completed": 80,
                "heated_plans": 300,
                "margin_kept": 40,
                "last_arrival": 7000,
            },
            **{
                "%s_o%d" % (k, o): v
                for o in range(1, 7)
                for k, v in (("detours", 0), ("walked", 650), ("excess", 20))
            },
        ),
    )
    spec_cases(
        "fc_hopwall",
        SPEC_FC_HOPWALL,
        {"units": 16, "tiles": 92, "worst_unit": 13, "dup": 0, "moving": 0},
    )
    # the F17 thresholds of check_m1h.py (100 / 16) are the baseline plus the allowance: 92 + 8, 13 + 3
    _, red_, _ = compare(
        SPEC_FC_HOPWALL,
        {"units": 16, "tiles": 92, "worst_unit": 13, "dup": 0, "moving": 0},
        {"units": 16, "tiles": 100, "worst_unit": 16, "dup": 0, "moving": 0},
    )
    expect("fc_hopwall: 100 / 16 is the edge (green)", not red_)
    _, red_, _ = compare(
        SPEC_FC_HOPWALL,
        {"units": 16, "tiles": 92, "worst_unit": 13, "dup": 0, "moving": 0},
        {"units": 16, "tiles": 101, "worst_unit": 17, "dup": 0, "moving": 0},
    )
    expect("fc_hopwall: 101 / 17 is red", set(red_) == {"tiles", "worst_unit"})
    spec_cases(
        "field200",
        SPEC_FIELD200,
        {
            "units": 200,
            "exits": 160,
            "exits_per100": 7.04,
            "last_exit": 4200,
            "max_tile": 103,
            "tiles100": 4,
            "width_median": 7,
            "width_max": 9,
            "arr_median": 9115,
            "arr_worst": 10330,
            "rev_mean": 0.64,
            "rev_ge5": 2,
            "refusals": 26,
        },
    )
    # the field's chaos (+-10%) stays green; the heat-off control (exits per 100 steps 2.05, max/tile 175) is red
    fbase = {
        "units": 200,
        "exits": 160,
        "exits_per100": 7.04,
        "last_exit": 4200,
        "max_tile": 103,
        "tiles100": 4,
        "width_median": 7,
        "width_max": 9,
        "arr_median": 9115,
        "arr_worst": 10330,
        "rev_mean": 0.64,
        "rev_ge5": 2,
        "refusals": 26,
    }
    _, red_, _ = compare(
        SPEC_FIELD200,
        fbase,
        dict(
            fbase, exits_per100=6.5, last_exit=4500, max_tile=110, arr_median=9500, arr_worst=11000
        ),
    )
    expect("field200: +-10% chaos stays green", not red_)
    _, red_, _ = compare(
        SPEC_FIELD200, fbase, dict(fbase, exits_per100=2.05, max_tile=175, tiles100=27)
    )
    expect(
        "field200: a heat-off-like field is red",
        {"exits_per100", "max_tile", "tiles100"} <= set(red_),
    )
    spec_cases(
        "stream",
        SPEC_STREAM,
        {
            "n": 45,
            "exited": 45,
            "stuck": 0,
            "maxtile": 25,
            "lanes": 6,
            "exit_per1000": 9,
            "med_arrive": 3943,
            "last_arrive": 6675,
            "rev_x100": 40,
            "rev_max": 4,
        },
    )
    for name_, fn_, bad_ in (
        (
            "fc_hopwall without the case",
            extract_fc_hopwall,
            [
                "; FC TOTAL case=4 steps=1 n=1 tiles=1 straight=1 moved=1 maxtiles=1 maxcheb=1 dup=0 maxratio=1 onwall=0 idle=1"
            ],
        ),
        ("stream without the total", extract_stream, ["; nothing"]),
    ):
        try:
            fn_(bad_, "x")
            expect("%s is refused" % name_, False)
        except ValueError:
            pass
    got = extract_fc_hopwall(
        [
            "; FC TOTAL case=5 steps=900 n=16 tiles=88 straight=40 moved=16 maxtiles=12 maxcheb=9 dup=0 maxratio=3 onwall=0 idle=16 bw=4 bh=4 rigid=0 lastmove=800"
        ],
        "x",
    )
    expect(
        "fc_hopwall parses the F17 total",
        got == {"units": 16, "tiles": 88, "worst_unit": 12, "dup": 0, "moving": 0},
    )
    expect(
        "kind_of",
        kind_of("ship") == "replay20"
        and kind_of("narrow16") == "narrow16"
        and kind_of("stream_ship") == "stream"
        and kind_of("fc_hopwall") == "fc_hopwall"
        and kind_of("field200") == "field200",
    )
    # better than baseline beyond tolerance: green + improved note
    now = dict(base, gave_up=0, stall_episodes=0, settle_steps=5000, excess_tiles=10.0)
    _, red, imp = compare(SPEC_REPLAY20, base, now)
    expect("better is green and noted", not red and "settle_steps" in imp and "excess_tiles" in imp)
    # better never hides a worse metric
    now = dict(base, settle_steps=5000, dead=1)
    _, red, imp = compare(SPEC_REPLAY20, base, now)
    expect("improvement does not mask a regression", red == ["dead"])
    # a missing metric is red
    now = dict(base)
    del now["dead"]
    _, red, _ = compare(SPEC_REPLAY20, base, now)
    expect("missing metric is red", "dead" in red)
    # a per-entry tolerance override wins
    now = dict(base, stall_episodes=1)
    _, red, _ = compare(SPEC_REPLAY20, base, now, {"stall_episodes": {"abs": 2}})
    expect("tol override", not red)
    # run(): unknown arm, missing log, update round trip, regression against a written baseline
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        bp = os.path.join(td, "b.json")
        with open(bp, "w") as fh:
            json.dump({"schema": 1, "arms": {}}, fh)
        expect("unknown arm is red", run("nope", td, False, bp) == 1)
        expect("empty dir is red", run("nope", td, True, bp) == 1)
    print("check_move_bench --selftest: %s" % ("PASS" if not bad else "FAIL: " + "; ".join(bad)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("target", nargs="?", help="session run dir (or a log file)")
    ap.add_argument("--arm")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.arm or not a.target:
        ap.error("need --arm and a run dir (or --selftest)")
    return run(a.arm, a.target, a.update)


if __name__ == "__main__":
    sys.exit(main())
