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
  srtt_ms / rttvar_ms      mp:T3. The TRANSPORT's own RFC 6298 view of peer 0, median over the
                           in-game rows. `n/a` -- never nan, never 0 -- when the transport cannot
                           measure a round trip at all, which over the TCP module is always: it has
                           no channel B, and TCP's invisible retransmits make loss meaningless there
                           anyway. A 0 ms SRTT would read as a perfect link, and that is the one
                           answer worse than no answer -- the same shape as the G198 failure ledger
                           entry: a reader degrading to a plausible number, silently.
  loss_pm                  RFC 7680 loss over a 256-packet sequence window, PER MILLE. Per mille and
                           not percent because the interesting values are fractions of a percent.
  late_p50 / late_t95      mp:T3's arrival lateness, in L0's sign: POSITIVE = ms of margin before the
                           deadline. `late_t95` is the 95th-percentile-WORST margin, which under that
                           sign is the LOW tail -- it is what the adaptive controller steers on, so a
                           run whose lookahead moved is read here first. These two are measured from
                           the lockstep's own peer-horizon array, so unlike srtt/loss they are
                           present over BOTH transports.
  la_moves                 how many times the adaptive controller changed the lookahead (the
                           `; [adaptive]` lines in mh_net.log). Pair it with step_lo/step_hi: those
                           say where it ended up, this says how hard it worked to get there.
  t_min_s / t70_s          mp:T3c / mp:T3f. Seconds from the peer's first advancing sim frame to the
                           first row whose `step_ms` (the controller's own pinned lookahead width, NOT
                           `local_h_ms` -- see the comment above `adaptive_floor_ms`) is at or below
                           this run's EFFECTIVE floor (t_min_s) or the fixed 70 ms line T3c's amended
                           done_when names (t70_s). `inf` prints as `never`: the run's controller
                           simply did not arrive, which is a RESULT and must not read as a real number.

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
import re
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


class FrametimeFormatError(RuntimeError):
    """read_frametimes could not confirm the qpc column's unit from the log's own header -- see
    dead-ends G198.

    G198: a reader whose ms conversion depended on an OPTIONAL header token (`qpc_freq=`) degraded
    to silence, not to an error, when D22 removed that token -- `if prev is not None and freq:`
    short-circuited and every frame-time column (ft_p95/ft_max/hitch_*) printed nan/0 with exit 0 on
    every current log. The fix is to REFUSE when the unit cannot be confirmed, not to guess."""


def read_frametimes(path):
    """Frame deltas in ms for IN-GAME frames only.

    THE ACTUAL BUG, traced past D22: the `qpc_us` column has ALWAYS been QueryPerformanceCounter
    already converted to microseconds by the DLL (net_lockstep.cpp `on_present`; true both before
    and after D22 -- `git show 5be708e4:...net_lockstep.cpp` shows the identical
    `(t.QuadPart * 1e6) / freq` conversion under the OLD header too). This function's `/ freq` was
    therefore ALREADY wrong before D22 -- freq was never a valid second divisor for an already-us
    column, so treating it as one (when the header still printed a freq value) silently produced a
    timeline ~10x too short. D22 removed only the header's decorative `(qpc_freq=...)` suffix
    (config it never should have needed), which stopped populating `freq` and flipped the bug from
    "silently ~10x short" to "silently nan/0, exit 0" -- both wrong, both silent, which is the
    G198 shape.

    Fixed: ms = us / 1000, unconditionally (matches mp_analyze.parse_frametime, which has always
    ignored header lines and divided by 1e3). The header is used only to CONFIRM the file is in the
    `qpc_us`-column shape (present under both the old and the current header text) before trusting
    it -- a header that names something else, or no header line at all, means the format cannot be
    confirmed, and this REFUSES (raises FrametimeFormatError) rather than silently emitting
    nan/0 (G198)."""
    if not os.path.isfile(path):
        return []
    unit_known = False
    header_seen = False
    prev, out = None, []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                header_seen = True
                cols = line.lstrip("# ").split()
                if cols and cols[0] == "qpc_us":
                    unit_known = True
                continue
            if not unit_known:
                # No header confirming the qpc_us shape yet -- keep scanning (a session boundary
                # can rewrite the header mid-file); if EOF arrives with none, refuse below.
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
            if prev is not None:
                d = (t - prev) / 1000.0
                if 0 <= d < 60000:  # guard a clock reset / QPC glitch
                    out.append(d)
            prev = t
    if not unit_known:
        raise FrametimeFormatError(
            "%s: %s -- refusing to guess ms from raw qpc stamps rather than silently print nan/0 "
            "(dead-ends G198, mp:SES2b)"
            % (
                path,
                "header present but its first column is not named qpc_us"
                if header_seen
                else "no header line at all",
            )
        )
    return out


def pct(xs, q):
    if not xs:
        return float("nan")
    s = sorted(xs)
    i = min(len(s) - 1, max(0, int(round(q / 100.0 * (len(s) - 1)))))
    return s[i]


# mp:T3. The controller's own account of WHY the lookahead moved. step_lo/step_hi/step_n below say
# THAT it moved; only this line carries the error term behind each move, which is what a run that
# misbehaved has to be read from without re-running it. Registered in tools/data/log_formats.json as
# `net.adaptive_lookahead`.
ADAPTIVE_NEEDLE = "; [adaptive] "
ADAPTIVE_RE = re.compile(r"lookahead (\d+) -> (\d+) ms (\w+)")


def adaptive_moves(run_dir):
    """Every adaptive-lookahead change in this run's mh_net.log, oldest first.

    Returns [{"t_ms", "from_ms", "to_ms", "verdict", "line"}]. An empty list is ambiguous on purpose
    and the caller must not read it as "the controller was off": the line is written only when the
    value actually CHANGES, so a controller correctly sitting on the floor for a whole run is silent.
    `armed_config` is what says whether adaptive was on."""
    out = []
    path = os.path.join(run_dir, "mh_net.log")
    if not os.path.isfile(path):
        return out
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if ADAPTIVE_NEEDLE not in line:
                continue
            m = ADAPTIVE_RE.search(line)
            if not m:
                continue
            t = 0
            for token in line.split():
                if token.startswith("t="):
                    try:
                        t = int(token[2:])
                    except ValueError:
                        t = 0
            out.append(
                {
                    "t_ms": t,
                    "from_ms": int(m.group(1)),
                    "to_ms": int(m.group(2)),
                    "verdict": m.group(3),
                    "line": line.strip(),
                }
            )
    return out


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


# mp:T3c -- the controller's EFFECTIVE floor, read off the same armed line.
#
# The band the ini asked for is on that line as `adaptive=1 [30..400 ms]`, but the value the
# controller can actually reach is max(that minimum, AD_SIM_FLOOR_MULT * sim_step) -- 60 ms at the
# shipping 20 ms sub-step, not 30 -- because a peer needs a few sub-steps of horizon to run
# concurrently at all. A `t_min_s` computed against 30 would read "never" on every healthy run.
ADAPTIVE_BAND_RE = re.compile(
    r"adaptive=\d+\s*\[\s*(\d+(?:\.\d+)?)\s*\.\.\s*(\d+(?:\.\d+)?)\s*ms\s*\]"
)
SIM_STEP_RE = re.compile(r"sim_step=(\d+(?:\.\d+)?)")
AD_SIM_FLOOR_MULT = 3.0  # src/mh_dll/mh/seams/net_lockstep.cpp


def adaptive_floor_ms(run_dir):
    """The smallest lookahead this run's controller could have reached, or None if unreadable."""
    net = os.path.join(run_dir, "mh_net.log")
    if not os.path.isfile(net):
        return None
    with open(net, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "time_tick hook armed" not in line:
                continue
            band = ADAPTIVE_BAND_RE.search(line)
            sim = SIM_STEP_RE.search(line)
            if not band or not sim:
                return None
            return max(float(band.group(1)), AD_SIM_FLOOR_MULT * float(sim.group(1)))
    return None


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


def session_dir_for(run_dir):
    """SES1: if `run_dir` is a PROCESS ("menu") directory, the newest SESSION directory beside it.

    mh_lockstep.log and mh_frametime.log are per-SESSION since SES1, so a path that used to hold both
    may now hold neither -- a pacing report pointed at `logs/<UTC>_menu_host` would simply print
    "skipped, no usable in-game mh_lockstep.log" and say nothing about why. Sessions are named
    `<UTC>_<mid8>_<slot>_<role>` and sort after the process directory they hang off; the newest is
    the last match that peer played, which is what "point the report at the run" has always meant.
    Returns None when `run_dir` already holds the logs (a session directory, or a pre-SES1 folder)."""
    base = os.path.basename(os.path.abspath(run_dir).rstrip("\\/"))
    if "_menu_" not in base:
        return None
    role = base.rsplit("_", 1)[-1]
    parent = os.path.dirname(os.path.abspath(run_dir))
    try:
        names = sorted(
            n
            for n in os.listdir(parent)
            if "_menu_" not in n
            and n.endswith("_" + role)
            and n > base
            and os.path.isdir(os.path.join(parent, n))
        )
    except OSError:
        return None
    return os.path.join(parent, names[-1]) if names else None


def analyse(run_dir):
    ls = os.path.join(run_dir, "mh_lockstep.log")
    if not os.path.isfile(ls):
        sd = session_dir_for(run_dir)
        if sd and os.path.isfile(os.path.join(sd, "mh_lockstep.log")):
            print("  (%s -> newest session %s)" % (run_dir, os.path.basename(sd)), file=sys.stderr)
            run_dir, ls = sd, os.path.join(sd, "mh_lockstep.log")
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

    # mp:T3c -- HOW LONG THE PEER TOOK TO GIVE THE LOOKAHEAD BACK, in seconds from the moment its sim
    # clock first advanced (not from the first in-game row: a peer sits in lockstep for a moment
    # before the match actually runs, and charging that to the controller would flatter it).
    # `n/a` means the run carries no readable band; a run that simply never arrived prints `never`,
    # which is a RESULT and must not be confused with one -- T3's clean-LAN client never did.
    #
    # mp:T3f -- shares the exact same clock (first advancing sim frame) and the exact same column
    # (`step_ms`, the controller's own pinned lookahead WIDTH -- see the module header note above
    # `adaptive_floor_ms`). It does NOT read `local_h_ms`: that column is the ABSOLUTE horizon point
    # (clock_ms + step_ms, ms_of(ADDR_LOCAL_HORIZON) in net_lockstep.cpp's ls_log_tick) and crosses
    # any fixed small threshold within the first frame or two by construction, which would make a
    # "<=70" column read as an instant, uninformative "0.0 s" on every run -- verified against a real
    # T3c-era client log (tmp/ui_test/determinism/client1/mh_lockstep.log) before writing this: its
    # local_h_ms tracks clock_ms + step_ms row for row, while step_ms is the bounded quantity that
    # actually descends from ~100 toward the floor over tens of seconds. T3f's own done_when calls
    # this a "t_min_s-style column at 70ms", which is what this is.
    def _time_to_threshold(threshold_ms):
        """First in-game wall_ms (relative to the peer's first advancing sim frame) at which
        step_ms <= threshold_ms + 0.5 (see the anti-alias note below), or +inf if it never arrives."""
        sim_t0 = None
        out = float("inf")
        for r in ing:
            if sim_t0 is None:
                if col(r, "clock_ms") > 0:
                    sim_t0 = col(r, "wall_ms")
                continue
            # Half a millisecond of slack, which is off_grid_ms()'s anti-alias nudge (0.01 ms) and
            # nothing else: a logged 61 is a controller still one step out, not one that arrived.
            if col(r, "step_ms") > 0 and col(r, "step_ms") <= threshold_ms + 0.5:
                out = (col(r, "wall_ms") - sim_t0) / 1000.0
                break
        return out

    floor_ms = adaptive_floor_ms(run_dir)
    t_min_s = _time_to_threshold(floor_ms) if floor_ms is not None else float("nan")
    # mp:T3f -- fixed at 70 ms regardless of the run's own adaptive_floor_ms: T3c's amended done_when
    # is "within one sim sub-step (<=70 ms) of the effective floor", a literal number, not a formula
    # re-derived per run. Always computed (no `is not None` gate) since it needs no armed-line read.
    t70_s = _time_to_threshold(70.0)

    # mp:T3. `measured` drops the rows where the column is the literal `n/a` (or missing entirely,
    # which is what an OLDER log looks like): the median is then over what was actually measured, and
    # an empty list becomes nan -> printed as `n/a`. Substituting 0 for an unmeasured row would pull
    # every median toward a link that was never observed.
    def measured(key):
        out = []
        for r in ing:
            raw = r.get(key)
            if raw is None or raw == "n/a":
                continue
            try:
                out.append(float(raw))
            except ValueError:
                continue
        return out

    srtt = measured("srtt0_ms")
    rttvar = measured("rttvar0_ms")
    ipdv = measured("ipdv0_ms")
    loss = measured("loss0_pm")
    late50 = measured("late_p50_ms")
    late95 = measured("late_tail95_ms")

    return {
        "armed": armed_config(run_dir),
        "peer": peer_label(run_dir),
        # The directory analyse() SETTLED ON, which is not necessarily the one it was given (SES1's
        # menu-directory redirect above). The adaptive-move dump reads mh_net.log from here.
        "dir": run_dir,
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
        "t_min_s": t_min_s,
        "t70_s": t70_s,
        "srtt_ms": pct(srtt, 50) if srtt else float("nan"),
        "rttvar_ms": pct(rttvar, 50) if rttvar else float("nan"),
        "ipdv_ms": pct(ipdv, 50) if ipdv else float("nan"),
        "loss_pm": pct(loss, 50) if loss else float("nan"),
        # The TAIL of the tail: 95% of the run's windows had a pessimistic margin at least this
        # large. Taking the median of late_tail95_ms instead would describe a typical window, and the
        # question this column answers is about the bad ones.
        "late_p50": pct(late50, 50) if late50 else float("nan"),
        "late_t95": pct(late95, 5) if late95 else float("nan"),
        "la_moves": len(adaptive_moves(run_dir)),
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
    ("t_min_s", "%8s", "%8.1f"),
    ("t70_s", "%7s", "%7.1f"),
    ("srtt_ms", "%8s", "%8.1f"),
    ("rttvar_ms", "%10s", "%10.1f"),
    ("ipdv_ms", "%8s", "%8.1f"),
    ("loss_pm", "%8s", "%8.1f"),
    ("late_p50", "%9s", "%9.0f"),
    ("late_t95", "%9s", "%9.0f"),
    ("la_moves", "%9s", "%9d"),
]


def cell(head_fmt, val_fmt, v):
    """One table cell -- `n/a` for a figure this run does not carry, never `nan`.

    `nan` is Python telling you about a float; `n/a` is the report telling you the run did not
    measure that. The distinction is the whole of mp:T3's reporting contract (the TCP module cannot
    measure a round trip), and it applies equally to the pre-existing frame-time columns, which have
    printed a bare `nan` for a run with no mh_frametime.log since SES2b."""
    if isinstance(v, float) and v != v:
        return head_fmt % "n/a"
    # mp:T3c's t_min_s: "the controller never got there" is a finding, and a huge float would read as
    # one that did. `inf` is the only value that reaches here, and it is deliberate.
    if isinstance(v, float) and v == float("inf"):
        return head_fmt % "never"
    return val_fmt % v


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dirs", nargs="*", help="run directories (each holding mh_lockstep.log)")
    ap.add_argument(
        "--label", default="", help="prefix printed before each row (the config under test)"
    )
    ap.add_argument("--csv", help="also append the rows to this CSV (created with a header if new)")
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="SES2b: exercise read_frametimes over synthetic logs (the current header, the old "
        "pre-D22 header text, and a header-less/unrecognised one) and exit non-zero on the wrong "
        "answer -- no rig, no game",
    )
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.dirs:
        ap.error("give at least one run directory (or --selftest)")

    results = []
    for d in args.dirs:
        try:
            r = analyse(d)
        except FrametimeFormatError as exc:
            print("REFUSED: %s" % exc, file=sys.stderr)
            return 2
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
        print(("%-22s " % r["label"][:22]) + " ".join(cell(h, f, r[n]) for n, h, f in COLS))
    print("\narmed (from each peer's own seam log -- check this matches what you asked for):")
    for r in results:
        print("    %-22s %-9s %s" % (r["label"][:22], r["peer"], r["armed"]))
    # mp:T3. The controller's moves, verbatim and with their own t= stamps, under the table rather
    # than folded into it: a lookahead that climbed from 100 to 400 and came back is four numbers,
    # not one, and the acceptance evidence for T3 is the SEQUENCE.
    for r in results:
        moves = adaptive_moves(r["dir"])
        if not moves:
            continue
        print("\nadaptive-lookahead moves (%s, %d):" % (r["peer"], len(moves)))
        for m in moves:
            print(
                "    t=%-10d %4d -> %-4d ms  %s"
                % (m["t_ms"], m["from_ms"], m["to_ms"], m["verdict"])
            )

    if args.csv:
        new = not os.path.exists(args.csv)
        with open(args.csv, "a", encoding="utf-8", newline="") as fh:
            if new:
                fh.write("label," + ",".join(n for n, _, _ in COLS) + "\n")
            for r in results:
                fh.write(r["label"] + "," + ",".join(str(r[n]) for n, _, _ in COLS) + "\n")
        print("\nwrote %s" % args.csv)
    return 0


# ---- the selftest: SES2b (dead-ends G198) -----------------------------------------------------
#
# G198's shape was a reader that silently degraded (nan/0, exit 0) instead of refusing when its
# optional header token went stale. This selftest asserts BOTH halves stay true: the current
# format is read for real numbers (not nan), and a format read_frametimes cannot identify is a
# hard refusal, not another silent pass.

FIXTURE_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "data", "fixtures", "pacing-frametime-v1", "host"
)

# The PRE-T3 header, kept verbatim because the back-compat arm below is the whole reason to keep a
# copy of it: a log written by a DLL from before 2026-09-18 must still produce every column it CAN
# produce, and must not be made to look as though it measured a 0 ms link.
_LOCKSTEP_HDR_24 = (
    "# wall_ms clock_ms total_ms local_h_ms committed_ms peer0_ms peer1_ms "
    "step_ms stall pcount tx_pkts rx_pkts since_rx_ms "
    "sess game flags grace_ms syncwait countdn p54bc sync_ms sim_burst "
    "icon_calls icon_shown\n"
)

# The current one: mp:T3 appended 13 columns (net_lockstep.cpp ls_log_tick).
_LOCKSTEP_HDR = _LOCKSTEP_HDR_24.rstrip("\n") + (
    " srtt0_ms srtt1_ms rttvar0_ms rttvar1_ms ipdv0_ms ipdv1_ms loss0_pm loss1_pm "
    "late_p50_ms late_tail95_ms late_tail99_ms late_n late_peer\n"
)


def _fx_lockstep(path, steps=60, interval_ms=100, latency=True, tcp=False):
    """A minimal but REAL-shaped mh_lockstep.log, all in-game (sess=3).

    `latency=False` writes the PRE-T3 24-column shape (an older DLL). `tcp=True` writes the current
    37-column shape as the TCP module fills it: the eight transport columns are the literal `n/a`
    and the five lateness columns are real, because lateness is measured from the lockstep's own
    peer-horizon array and does not need a channel B."""
    hdr = _LOCKSTEP_HDR if latency else _LOCKSTEP_HDR_24
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(hdr)
        for s in range(1, steps + 1):
            ms = s * interval_ms
            row = "%d %d %d %d %d %d %d %d 0 2 %d %d 40 3 3 0x00 0 0 0 2 0 1 0 0" % (
                ms,
                ms,
                ms,
                ms + interval_ms,
                ms,
                ms,
                ms,
                interval_ms,
                s * 2,
                s * 2,
            )
            if latency:
                row += (
                    " n/a n/a n/a n/a n/a n/a n/a n/a 55 20 8 128 1"
                    if tcp
                    else " 80 n/a 9 n/a 6 n/a 0 n/a 55 20 8 128 1"
                )
            fh.write(row + "\n")


def _fx_frametime(
    path, header, frames=120, frame_us=16667, hitch_at=None, hitch_us=200000, t0=1000000
):
    """`header` is the raw header LINE (or None for no header at all)."""
    with open(path, "w", encoding="utf-8") as fh:
        if header is not None:
            fh.write(header)
        t = t0
        for i in range(1, frames + 1):
            t += hitch_us if i == hitch_at else frame_us
            fh.write("%d %d\n" % (t, 3))


def selftest():
    import shutil
    import tempfile

    fails = 0

    def arm(name, ok):
        nonlocal fails
        print("   %-64s %s" % (name, "ok" if ok else "FAIL"))
        if not ok:
            fails += 1

    tmp = tempfile.mkdtemp(prefix="mp_pacing_report_selftest_")
    try:
        # ---- arm 1: the committed current-format fixture -> real numbers, no nan, hitch caught ---
        r = analyse(FIXTURE_DIR)
        ok = (
            r is not None
            and r["ft_frames"] > 0
            and r["ft_p95"] == r["ft_p95"]  # not nan
            and r["ft_max"] == r["ft_max"]
            and r["hitch_100ms"] >= 1
        )
        arm(
            "committed fixture (current D22+ format, 200ms hitch) -> real ft_p95/ft_max, "
            "hitch_100ms>=1",
            ok,
        )

        # ---- arm 2: a synthetic current-format log with NO hitch -> hitch counters are 0, not nan -
        d = os.path.join(tmp, "clean")
        os.makedirs(d)
        _fx_lockstep(os.path.join(d, "mh_lockstep.log"))
        _fx_frametime(os.path.join(d, "mh_frametime.log"), "# qpc_us game_mode\n", frames=120)
        r = analyse(d)
        ok = r is not None and r["hitch_100ms"] == 0 and r["ft_p95"] == r["ft_p95"]
        arm("synthetic current-format log with no hitch -> hitch_100ms==0 (not nan)", ok)

        # ---- arm 3: a PRE-D22 header (still carries the decorative qpc_freq= suffix this fix does
        # NOT use as a divisor) over the SAME already-microsecond values -- must read identically to
        # arm 2, not ~10x short. `git show 5be708e4:...net_lockstep.cpp` confirms the pre-D22 column
        # was ALREADY `(QuadPart * 1e6) / freq` -- i.e. already-us, same as today; the only thing D22
        # removed was that header suffix. There never was a raw-ticks format to convert from.
        d = os.path.join(tmp, "old_header")
        os.makedirs(d)
        _fx_lockstep(os.path.join(d, "mh_lockstep.log"))
        _fx_frametime(
            os.path.join(d, "mh_frametime.log"),
            "# qpc_us game_mode  (qpc_freq=10000000)\n",
            frames=120,
        )
        r = analyse(d)
        ok = r is not None and r["hitch_100ms"] == 0 and 15 < r["ft_p95"] < 18
        arm(
            "old (pre-D22) header text, same already-us values -> same real ft_p95 (not ~10x off, "
            "not fooled by the decorative qpc_freq= suffix)",
            ok,
        )

        # ---- arm 4: a header that does not name qpc_us as its first column -> REFUSED, not nan/0 --
        d = os.path.join(tmp, "unrecognised")
        os.makedirs(d)
        _fx_lockstep(os.path.join(d, "mh_lockstep.log"))
        _fx_frametime(os.path.join(d, "mh_frametime.log"), "# some_other_column game_mode\n")
        refused = False
        try:
            analyse(d)
        except FrametimeFormatError:
            refused = True
        arm(
            "a header not naming qpc_us as its first column -> FrametimeFormatError (refused)",
            refused,
        )

        # ---- arm 5: NO header line at all -> also refused -----------------------------------------
        d = os.path.join(tmp, "noheader")
        os.makedirs(d)
        _fx_lockstep(os.path.join(d, "mh_lockstep.log"))
        _fx_frametime(os.path.join(d, "mh_frametime.log"), None)
        refused = False
        try:
            analyse(d)
        except FrametimeFormatError:
            refused = True
        arm("no header line at all -> FrametimeFormatError (refused)", refused)

        # ---- arm 7 (mp:T3): the committed fixture carries the CURRENT 37-column shape, and the
        # per-peer figures come back real -- while transport peer SLOT 1, which a 2-player run does
        # not have, is `n/a` and must not become a 0 that pulls a median.
        r = analyse(FIXTURE_DIR)
        ok = (
            r is not None
            and r["srtt_ms"] == r["srtt_ms"]
            and 70 <= r["srtt_ms"] <= 90
            and r["late_p50"] == r["late_p50"]
            and r["late_t95"] == r["late_t95"]
            and r["loss_pm"] == 0
        )
        arm("committed fixture -> real srtt_ms/late_p50/late_t95/loss_pm for peer 0", ok)

        # ---- arm 8 (mp:T3): the TCP shape. The eight transport columns are `n/a` -> nan (printed
        # `n/a`), and the FIVE LATENESS COLUMNS ARE STILL READ. This is the arm with teeth: a parser
        # that stopped at the first unparseable optional column would drop the lateness figures on
        # every TCP run, silently, because they come after the `n/a` block.
        d = os.path.join(tmp, "tcp_shape")
        os.makedirs(d)
        _fx_lockstep(os.path.join(d, "mh_lockstep.log"), tcp=True)
        r = analyse(d)
        ok = (
            r is not None
            and r["srtt_ms"] != r["srtt_ms"]  # nan: not measurable over TCP
            and r["loss_pm"] != r["loss_pm"]
            and r["late_p50"] == 55  # ...but lateness IS
            and r["late_t95"] == 20
        )
        arm("TCP shape -> srtt/loss are n/a while late_p50/late_t95 are still read", ok)

        # ---- arm 9 (mp:T3): a PRE-T3 log (24 columns) still analyses, and every T3 figure is
        # absent rather than zero. An old log must not be able to claim a perfect link.
        d = os.path.join(tmp, "pre_t3")
        os.makedirs(d)
        _fx_lockstep(os.path.join(d, "mh_lockstep.log"), latency=False)
        r = analyse(d)
        ok = (
            r is not None
            and r["frames"] > 0
            and r["srtt_ms"] != r["srtt_ms"]
            and r["late_p50"] != r["late_p50"]
            and r["deficit_%"] == r["deficit_%"]  # the pre-T3 columns still work
        )
        arm("a pre-T3 24-column log still analyses; every T3 figure is n/a, not 0", ok)

        # ---- arm 10 (mp:T3): the controller's move lines, and the silence that is NOT a verdict.
        d = os.path.join(tmp, "moves")
        os.makedirs(d)
        _fx_lockstep(os.path.join(d, "mh_lockstep.log"))
        with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
            fh.write("; something else entirely\n")
            fh.write(
                "; [adaptive] t=12345 lookahead 100 -> 200 ms grow (late p50 -40 tail95 -210 "
                "tail99 -260 ms, n=64, peer=1; starved 900/2000 ms diag)\n"
            )
            fh.write(
                "; [adaptive] t=14500 lookahead 200 -> 400 ms grow (late p50 -10 tail95 -120 "
                "tail99 -180 ms, n=64, peer=1; starved 400/2000 ms diag)\n"
            )
            fh.write(
                "; [adaptive] t=99000 lookahead 400 -> 384 ms shrink (late p50 300 tail95 240 "
                "tail99 190 ms, n=64, peer=1; starved 0/2000 ms diag)\n"
            )
        mv = adaptive_moves(d)
        ok = (
            len(mv) == 3
            and mv[0]["t_ms"] == 12345
            and mv[0]["from_ms"] == 100
            and mv[0]["to_ms"] == 200
            and mv[0]["verdict"] == "grow"
            and mv[2]["verdict"] == "shrink"
            and analyse(d)["la_moves"] == 3
        )
        arm("adaptive_moves reads t=/from/to/verdict off the ; [adaptive] lines", ok)
        d = os.path.join(tmp, "nomoves")
        os.makedirs(d)
        _fx_lockstep(os.path.join(d, "mh_lockstep.log"))
        arm(
            "a run with no mh_net.log yields no moves (silence is not 'the controller was off')",
            adaptive_moves(d) == [],
        )

        # ---- arm 6: a run with NO mh_frametime.log at all is NOT a format error -- just no ft data
        d = os.path.join(tmp, "nofile")
        os.makedirs(d)
        _fx_lockstep(os.path.join(d, "mh_lockstep.log"))
        r = analyse(d)
        ok = r is not None and r["ft_frames"] == 0 and r["ft_p95"] != r["ft_p95"]  # nan is expected
        arm("a run dir with no mh_frametime.log at all -> ft_frames=0 (not a refusal)", ok)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("mp_pacing_report --selftest: %s" % ("PASS" if not fails else "%d ARM(S) FAILED" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
