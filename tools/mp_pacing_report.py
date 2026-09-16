#!/usr/bin/env python3
"""mp_pacing_report -- turn a peer's run logs into the P5 pacing metrics, one row per peer.

P5 asks a comparative question ("what should a player actually run over an internet link?"), and a
comparative question needs the same numbers computed the same way every time. Doing it by eye per run
is how the 2026-07-20 tail-vs-total mistake happened -- a windowed rate read ~1.0x while the run was
meaningfully behind ideal. So the metrics live here, with the traps baked in:

  deficit_ms / deficit_%   TOTAL wall-clock deficit over the whole in-game span, NOT a windowed rate.
                           (wall elapsed) - (game clock elapsed). This is the honest "is it keeping
                           up" number; see the MP latency notes CORRECTION 2.
  stall_%                  fraction of in-game frames with the engine's own stall flag set.
  since_rx_p95_ms          95th percentile of "ms since the last inbound DATA frame" -- how starved
                           the link looked, as opposed to how starved the sim was.
  icon_per_1k              de-sync icon shows per 1000 frames (P4). NORMALISED, because frame counts
                           differ several-fold across pacing configs and a raw total would mislead.
  ft_p95_ms / ft_max_ms    frame-time percentiles from mh_frametime.log, in-game frames only.
  hitch_100ms / hitch_250  frames that took longer than that -- the "micro-freeze" candidate.

The last two are the point of the whole tool: they are what separates "the SIM stalled" (a pacing
problem, fixable with lookahead/rx_spin/heartbeat) from "the FRAME took 300 ms" (a render problem,
which no amount of pacing tuning will touch). A player feels both as a freeze.

Usage:
    python tools/mp_pacing_report.py <run-dir> [<run-dir> ...] [--label NAME]
    python tools/mp_pacing_report.py tmp/p4b_la60_logs/* --label "lookahead 60"
    python tools/mp_pacing_report.py --csv results.csv tmp/*_logs/*
A run-dir is a folder holding mh_lockstep.log (+ optionally mh_frametime.log) -- i.e. what
ui_test.py --determinism leaves in tmp/ui_test/determinism/<peer>/.
"""

import argparse
import os
import sys

# mh_lockstep.log columns (see ls_log_tick in net_lockstep.cpp). Indexed by NAME, not position:
# the row grew an icon_calls/icon_shown pair on 2026-07-26 and will grow again.
LS_HEADER_HINT = "wall_ms"


def read_lockstep(path):
    """Return (list-of-dicts, header-names). Rows are dicts so a column added later cannot shift us."""
    rows, names = [], None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                if LS_HEADER_HINT in line:
                    names = line.lstrip("# ").split()
                continue
            if names is None:
                continue
            parts = line.split()
            if len(parts) != len(names):
                continue  # a torn final write, or a row from an older DLL -- skip rather than misalign
            rows.append(dict(zip(names, parts)))
    return rows, names


def read_frametimes(path):
    """Frame deltas in ms for IN-GAME frames only, using the qpc_freq the DLL recorded."""
    if not os.path.isfile(path):
        return []
    freq, prev, out = None, None, []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("#"):
                if "qpc_freq=" in line:
                    try:
                        freq = float(line.split("qpc_freq=")[1].split(")")[0].split()[0])
                    except (IndexError, ValueError):
                        freq = None
                continue
            p = line.split()
            if len(p) < 2:
                continue
            try:
                t, mode = int(p[0]), int(p[1])
            except ValueError:
                continue
            if mode != 3:  # in-game only; menu frames are a different workload entirely
                prev = None
                continue
            if prev is not None and freq:
                d = (t - prev) / freq * 1000.0
                if 0 <= d < 60000:  # guard a clock reset / QPC glitch
                    out.append(d)
            prev = t
    return out


def pct(xs, q):
    if not xs:
        return float("nan")
    s = sorted(xs)
    i = min(len(s) - 1, max(0, int(round(q / 100.0 * (len(s) - 1)))))
    return s[i]


def armed_config(run_dir):
    """The pacing the peer ACTUALLY armed, read from its own seam log.

    Not decoration. P5's method notes say "verify what actually ARMED from the seam log", and on
    2026-07-26 skipping that produced a confident wrong answer: a shell `&&` short-circuited on a
    taskkill that had nothing to kill, the run never launched, and the report happily re-measured the
    PREVIOUS run's leftover logs -- numbers identical to the row above it, presented as a new result.
    A row that carries the config it measured cannot be mistaken for a row about the config you asked
    for.
    """
    net = os.path.join(run_dir, "mh_net.log")
    if not os.path.isfile(net):
        return "?"
    want = ("lockstep_step=", "adaptive=", "sim_step=", "rx_spin=")
    with open(net, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "time_tick hook armed" not in line:
                continue
            bits = []
            for token in line.replace(",", " ").split():
                if token.startswith(want):
                    bits.append(token)
            return " ".join(bits) if bits else "?"
    return "?"


# A batch archives every run as <batch>/<run>/host, so the bare basename is "host" for all of them and
# a multi-run table becomes a column of identical labels whose arm you have to infer from path order.
# Inferring an arm from path order is what produced a fake data point on 2026-07-29. Qualify the
# generic names with the parent directory, which is the run.
GENERIC_DIR_NAMES = ("host", "client1", "client2", "client3")


def peer_label(run_dir):
    p = os.path.normpath(run_dir)
    base = os.path.basename(p)
    if base in GENERIC_DIR_NAMES:
        parent = os.path.basename(os.path.dirname(p))
        if parent:
            return "%s/%s" % (parent, base)
    return base


def analyse(run_dir):
    ls = os.path.join(run_dir, "mh_lockstep.log")
    if not os.path.isfile(ls):
        return None
    rows, _ = read_lockstep(ls)
    ing = [r for r in rows if r.get("sess") == "3"]  # in-game only
    if len(ing) < 10:
        return None

    def col(r, k, default=0.0):
        try:
            return float(r.get(k, default))
        except ValueError:
            return default

    wall = col(ing[-1], "wall_ms") - col(ing[0], "wall_ms")
    clock = col(ing[-1], "clock_ms") - col(ing[0], "clock_ms")
    deficit = wall - clock
    stalls = sum(1 for r in ing if col(r, "stall") > 0)
    since_rx = [col(r, "since_rx_ms") for r in ing]
    icons = col(ing[-1], "icon_calls") - col(ing[0], "icon_calls")
    shown = col(ing[-1], "icon_shown") - col(ing[0], "icon_shown")
    ft = read_frametimes(os.path.join(run_dir, "mh_frametime.log"))
    # The LOOKAHEAD AS THE RUN ACTUALLY FLEW IT, per frame -- not the value that armed. `armed` says
    # what the controller STARTED from; these say whether it then moved. Needed to tell a working
    # adaptive controller (step_n > 1, a spread between lo and hi) from one that armed adaptive=1 and
    # then sat still, which the armed line alone reports identically. A pinned run should show
    # step_n == 1, so the same columns also prove a fixed-lookahead sweep point really was fixed.
    steps = [col(r, "step_ms") for r in ing]
    steps = [s for s in steps if s > 0]

    return {
        "armed": armed_config(run_dir),
        "peer": peer_label(run_dir),
        "frames": len(ing),
        "wall_s": wall / 1000.0,
        "sim_s": clock / 1000.0,
        "deficit_s": deficit / 1000.0,
        "deficit_%": (deficit / wall * 100.0) if wall > 0 else float("nan"),
        "stall_%": stalls * 100.0 / len(ing),
        "since_rx_p95": pct(since_rx, 95),
        "icon_per_1k": icons * 1000.0 / len(ing),
        "icon_shown_per_1k": shown * 1000.0 / len(ing),
        "ft_p95": pct(ft, 95) if ft else float("nan"),
        "ft_p99": pct(ft, 99) if ft else float("nan"),
        "ft_max": max(ft) if ft else float("nan"),
        "hitch_100ms": sum(1 for d in ft if d > 100),
        "hitch_250ms": sum(1 for d in ft if d > 250),
        "ft_frames": len(ft),
        "step_lo": min(steps) if steps else float("nan"),
        "step_hi": max(steps) if steps else float("nan"),
        "step_n": len(set(steps)),
    }


COLS = [
    ("peer", "%-18s", "%-18s"),
    ("frames", "%7s", "%7d"),
    ("wall_s", "%7s", "%7.1f"),
    ("deficit_s", "%9s", "%9.1f"),
    ("deficit_%", "%9s", "%9.1f"),
    ("stall_%", "%7s", "%7.1f"),
    ("since_rx_p95", "%12s", "%12.0f"),
    ("icon_per_1k", "%11s", "%11.0f"),
    ("ft_p95", "%7s", "%7.1f"),
    ("ft_p99", "%7s", "%7.1f"),
    ("ft_max", "%8s", "%8.1f"),
    ("hitch_100ms", "%11s", "%11d"),
    ("hitch_250ms", "%11s", "%11d"),
    ("step_lo", "%7s", "%7.1f"),
    ("step_hi", "%7s", "%7.1f"),
    ("step_n", "%6s", "%6d"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dirs", nargs="+", help="run directories (each holding mh_lockstep.log)")
    ap.add_argument(
        "--label", default="", help="prefix printed before each row (the config under test)"
    )
    ap.add_argument("--csv", help="also append the rows to this CSV (created with a header if new)")
    args = ap.parse_args()

    results = []
    for d in args.dirs:
        r = analyse(d)
        if r is None:
            print("  (skipped %s -- no usable in-game mh_lockstep.log)" % d, file=sys.stderr)
            continue
        r["label"] = args.label
        results.append(r)
    if not results:
        return 1

    hdr = ("%-22s " % "label") + " ".join(f % n for n, f, _ in COLS)
    print(hdr)
    print("-" * len(hdr))
    for r in results:
        print(("%-22s " % r["label"][:22]) + " ".join(f % r[n] for n, _, f in COLS))
    print("\narmed (from each peer's own seam log -- check this matches what you asked for):")
    for r in results:
        print("    %-22s %-9s %s" % (r["label"][:22], r["peer"], r["armed"]))

    if args.csv:
        new = not os.path.exists(args.csv)
        with open(args.csv, "a", encoding="utf-8", newline="") as fh:
            if new:
                fh.write("label," + ",".join(n for n, _, _ in COLS) + "\n")
            for r in results:
                fh.write(r["label"] + "," + ",".join(str(r[n]) for n, _, _ in COLS) + "\n")
        print("\nwrote %s" % args.csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
