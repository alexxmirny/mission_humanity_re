#!/usr/bin/env python3
"""mp:P6 post_check -- the two clauses the user's 2026-09-25 decision needs to close P6 as "freeze
not reproduced" (the delayed-loser stall/blackhole arm is explicitly DROPPED from scope, not just
deferred):

  (a) ALL PAIRS IDENTICAL up to the match end -- the D21 in-band desync watch (always-on per-match
      region-hash comparator, no verbose ini needed) shows 0 mismatching samples on a NON-VACUOUS
      (>0) compared-sample count, read from the HOST's own mh_net.log.
  (b) the WINNER is no worse -- the HOST's own longest present-to-present frame gap in the same
      window is also < the bound (the original mp:P6 clause below only ever checked the loser).

usage: check_p6_gap.py [--expect fixed] [--max-ms N] <host session dir> <client session dir>

THE ORIGINAL CLAUSE (still checked): the LOSING peer's longest frame gap between the deciding step
and its own lose screen.

The conquest workload the row arms (`[harness] conq=1`, host-only) makes the HOST win and the
CLIENT lose -- host's side issues the eliminating orders against the enemy=client slot (measured
wave 2, 2026-09-23: `on_gameover ENTER sess=2 outcome=4 gclk=N` on the client, `outcome=5` on the
host, same gclk).

THE WINDOW. "The deciding step" is when the HOST leaves lockstep on its own outcome (net_lockstep.
cpp's HB_ENDGAME_GRACE_MS grace-timer starts counting from here) -- so the window this checks is
[host's own on_gameover wall-time, client's own on_gameover wall-time], i.e. real elapsed time from
the host resolving to the trailing/losing peer finally resolving. Both scripts end on
`screen 0x00653dc3` (the outcome dialog's own widget list, `_G_LLM_UI_OUTCOME_DLG_WIDGET_LIST`),
so each peer's own `mh_net.log` on_gameover line is that same moment.

WHY A WALL-CLOCK ANCHOR, NOT A GCLK ONE, TO PLACE THE WINDOW ON EACH PEER'S OWN mh_frametime.log.
mh_frametime.log's qpc_us is QueryPerformanceCounter on an arbitrary non-wall-clock epoch (see
tools/mp_analyze.py's parse_frametime docstring); mh_net.log's "[HH:MM:SS.mmm]" line stamps ARE
wall-clock (mh_log_stamp/GetLocalTime). tools/mp_analyze.py's find_lobby_clip_qpc already solved
exactly this join for the lobby-clip case (anchor via the "present hook armed" line's wall time
against mh_frametime.log row 0's qpc_us, then qpc_us advances at 1e6 per wall-clock second) -- this
reuses the SAME anchor technique for BOTH peers, each against its OWN "present hook armed" + row 0:
a peer's own on_gameover wall-time converts exactly against its own log (self-consistent); the
OTHER peer's on_gameover wall-time converts on the ASSUMPTION the two peers' local wall clocks
agree closely enough for a 500 ms bound to be meaningful (true for the rig's Hyper-V peers, which
sync to the host clock; see find_lobby_clip_qpc's own note that this anchor technique lands within
~1s over a 21.6-minute run, i.e. ~0.08% error -- two orders of magnitude under the bound this
checks).

mh_frametime.log has been a DIET stream since mp:SES5 (2026-09-21): a row is written only when its
interval exceeds 2x the previous 1Hz window's median, carrying that exact interval in an optional
3rd `interval_us` column (tools/data/log_formats.json's frametime.row entry) -- so "the longest
gap" is the max of that column over the window, no delta-reconstruction from neighbouring rows
needed.

THE DESYNC CLAUSE reads the HOST's own mh_net.log for the D21 watch's always-on (non-verbose)
summary line, `; [desync] match end: N mismatching / M compared sample(s), ... S steps seen`
(tools/check_resync_storm.py's DESYNC_BAD_NEEDLE / verbose DESYNC_MATCH_RE are the sibling tool's
own per-step forms, gated behind a verbose ini fragment this row does not carry -- the always-on
summary line needs no extra ini). N must be 0 and M must be > 0 (a 0/0 read is a vacuous pass, not
proof of identity -- a match that ends too fast for the watch's every-50-step cadence to pair up
even one comparison; mp:P6's own harness_extra_host comment in tools/test_ui.py records the run
that this bit, and the conq_at fix that gives the watch a real window before the conquest workload
does anything).
"""

import argparse
import glob
import os
import re
import sys
import tempfile

_WALL_TS_RE = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]")
_GAMEOVER_RE = re.compile(r"on_gameover ENTER sess=(\d+) outcome=(\d+) gclk=(\d+)")
_DESYNC_BAD_NEEDLE = "*** DESYNC"
_DESYNC_END_RE = re.compile(
    r"match end: (\d+) mismatching / (\d+) compared sample\(s\).*?(\d+) steps seen"
)


def wall_seconds(line):
    m = _WALL_TS_RE.match(line)
    if not m:
        return None
    h, mi, s, ms = (int(x) for x in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000.0


def first_wall(lines, marker):
    for ln in lines:
        if marker in ln:
            return wall_seconds(ln)
    return None


def first_gameover(lines):
    """(wall_seconds, outcome, gclk) of the FIRST on_gameover ENTER line, or None."""
    for ln in lines:
        m = _GAMEOVER_RE.search(ln)
        if m:
            w = wall_seconds(ln)
            if w is not None:
                return w, int(m.group(2)), int(m.group(3))
    return None


def desync_end(lines):
    """(mismatching, compared, steps_seen) from the D21 watch's always-on 'match end' summary line
    in a peer's mh_net.log, or None if the line never appeared (the match ended before the watch's
    session-end hook ran, or the watch itself is off)."""
    for ln in lines:
        m = _DESYNC_END_RE.search(ln)
        if m:
            return int(m.group(1)), int(m.group(2)), int(m.group(3))
    return None


def boot_net_lines(session_dir):
    """The BOOT ("_menu_") sibling session's own mh_net.log lines, or [] if none is found.

    `session_dir` is the MATCH session dir a `post_check_session: True` row hands the checker
    (test_ui.py's sp_newest_session_run) -- but "; present hook armed" is a DLL-init-time banner
    printed once per PROCESS, into the process/"menu" directory (SES1), well before any lobby or
    match session exists (see tools/test_ui.py's sp_newest_run docstring). mh_frametime.log's qpc_us
    is QueryPerformanceCounter, a continuous system-wide clock that does not reset when a new
    session's log file opens, so a (wall, qpc) anchor pair taken from the boot dir is still valid
    against rows in the match session's own mh_frametime.log -- only the FILE rotates, not the
    clock. `session_dir`'s parent is the lane's "logs" folder (the same layout sp_newest_run reads),
    so the boot dir is found the same way: the newest `*_menu_*` sibling.
    """
    logs_dir = os.path.dirname(os.path.abspath(session_dir.rstrip("\\/")))
    cands = sorted(d for d in glob.glob(os.path.join(logs_dir, "*_menu_*")) if os.path.isdir(d))
    if not cands:
        return []
    p = os.path.join(cands[-1], "mh_net.log")
    if not os.path.isfile(p):
        return []
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def parse_frametime_rows(path):
    """[(qpc_us, game_mode, interval_us|None), ...], skipping '#' header/summary lines."""
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if len(p) < 2:
                continue
            try:
                qpc = int(p[0])
                mode = int(p[1])
                interval = int(p[2]) if len(p) >= 3 else None
            except ValueError:
                continue
            rows.append((qpc, mode, interval))
    return rows


def frame_gap(session_dir, net_lines, host_wall, client_wall, pad_s, label):
    """The longest present-to-present gap (ms) in [host_wall, client_wall] +/- pad_s, read from
    `session_dir`'s OWN mh_frametime.log, anchored via `session_dir`'s OWN 'present hook armed'
    line (own mh_net.log, falling back to its boot-session sibling). Returns
    (result_dict_or_None, error_or_None); result_dict has max_gap_ms/hits/total/span_ms."""
    armed_wall = first_wall(net_lines, "present hook armed")
    if armed_wall is None:
        armed_wall = first_wall(boot_net_lines(session_dir), "present hook armed")
    if armed_wall is None:
        return None, (
            "no 'present hook armed' line in %s's mh_net.log or its boot-session sibling -- "
            "cannot anchor wall-clock to mh_frametime.log's qpc_us" % label
        )
    ft_path = os.path.join(session_dir, "mh_frametime.log")
    rows = parse_frametime_rows(ft_path)
    if not rows:
        return None, "%s has no rows" % ft_path

    def to_qpc(wall):
        return rows[0][0] + (wall - armed_wall) * 1e6

    start_qpc = to_qpc(host_wall) - pad_s * 1e6
    end_qpc = to_qpc(client_wall) + pad_s * 1e6
    span_ms = (end_qpc - start_qpc) / 1000.0
    if span_ms < 0:
        return None, (
            "computed window is negative (%.0f ms) for %s -- host_wall=%.3f client_wall=%.3f "
            "armed_wall=%.3f; the wall-clock anchor did not resolve sanely"
            % (span_ms, label, host_wall, client_wall, armed_wall)
        )
    hits = [(qpc, mode, iv) for qpc, mode, iv in rows if start_qpc <= qpc <= end_qpc]
    gaps = [iv for _, _, iv in hits if iv is not None]
    max_gap_us = max(gaps) if gaps else 0
    return {
        "max_gap_ms": max_gap_us / 1000.0,
        "hits": len(hits),
        "total": len(rows),
        "span_ms": span_ms,
    }, None


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def _make_session(
    base,
    match_dir_name,
    host_wall,
    host_outcome,
    client_wall,
    client_outcome,
    gap_ms,
    desync_line=None,
    host_gap_ms=0.0,
):
    """Build a synthetic <base>/{host,client}/logs/{<match>,<boot>}/... pair the checker can be
    pointed at. The client's frametime.log always carries one outlier row of exactly `gap_ms`
    inside the window; the host's carries one of `host_gap_ms` (0 by default -- a clean winner).
    `desync_line` (if given) is appended verbatim to the host's mh_net.log, in place of the default
    clean "match end: 0 mismatching / N compared" line."""
    host_net = os.path.join(base, "host", "logs", match_dir_name)
    client_net = os.path.join(base, "client", "logs", match_dir_name)
    host_boot = os.path.join(base, "host", "logs", "00000000T000000Z_menu_solo")
    client_boot = os.path.join(base, "client", "logs", "00000000T000000Z_menu_solo")

    def go_line(wall, outcome):
        h = int(wall // 3600)
        mi = int((wall % 3600) // 60)
        s = int(wall % 60)
        ms = int(round((wall - int(wall)) * 1000))
        return (
            "[%02d:%02d:%02d.%03d] ; on_gameover ENTER sess=2 outcome=%d gclk=45799 (downgrade=0)\n"
            % (
                h,
                mi,
                s,
                ms,
                outcome,
            )
        )

    host_text = go_line(host_wall, host_outcome) if host_outcome is not None else ""
    if host_outcome is not None:
        host_text += (
            desync_line
            if desync_line is not None
            else "; [desync] match end: 0 mismatching / 5 compared sample(s), 0 dropped as too "
            "old, 0 bad frame(s), 0 order-digest mismatching, 324 steps seen\n"
        )
    _write(os.path.join(host_net, "mh_net.log"), host_text)
    if client_outcome is not None:
        _write(os.path.join(client_net, "mh_net.log"), go_line(client_wall, client_outcome))
    else:
        _write(os.path.join(client_net, "mh_net.log"), "")
    for boot_dir in (host_boot, client_boot):
        _write(
            os.path.join(boot_dir, "mh_net.log"),
            "[00:00:00.000] ; present hook armed (own detour): frametime_log=1 "
            "eager_advertise=1 temporal=1\n",
        )
    # row 0's qpc anchors to the boot line's wall time (0.0s here); the outlier row sits at the
    # MIDPOINT of the window with an interval_us equal to the requested gap, in microseconds.
    mid_wall = (host_wall + client_wall) / 2.0
    mid_qpc = int(mid_wall * 1e6)
    _write(
        os.path.join(client_net, "mh_frametime.log"),
        "# qpc_us game_mode\n0 3\n%d 3 %d\n" % (mid_qpc, int(gap_ms * 1000)),
    )
    _write(
        os.path.join(host_net, "mh_frametime.log"),
        "# qpc_us game_mode\n0 3\n%d 3 %d\n" % (mid_qpc, int(host_gap_ms * 1000)),
    )
    return host_net, client_net


def selftest():
    """Synthetic arms: a real change to the checker's window/anchor/outcome logic must flip one of
    these, the same discipline every other post_check tool's --selftest carries."""
    import subprocess

    failures = []

    def run(args_list):
        return subprocess.run(
            [sys.executable, os.path.abspath(__file__)] + args_list, capture_output=True, text=True
        )

    def check(name, ok, detail=""):
        print(
            ("  ok   " if ok else "  FAIL ")
            + name
            + (("  -- " + detail) if detail and not ok else "")
        )
        if not ok:
            failures.append(name)

    with tempfile.TemporaryDirectory() as td:
        # (a) a clean, fast resolve (0 ms gap) PASSES --expect fixed.
        h, c = _make_session(os.path.join(td, "a"), "m", 10.0, 5, 10.05, 4, 0.0)
        r = run(["--expect", "fixed", h, c])
        check("(a) 0ms gap passes --expect fixed", r.returncode == 0, r.stdout + r.stderr)

        # (b) a real freeze (600 ms, over the 500 ms default bound) FAILS --expect fixed -- the
        # MUTATION-RED arm: if the bound check or the window math ever silently stopped comparing
        # against max_ms, this would go green for the wrong reason.
        h, c = _make_session(os.path.join(td, "b"), "m", 10.0, 5, 13.6, 4, 600.0)
        r = run(["--expect", "fixed", h, c])
        check("(b) 600ms gap FAILS --expect fixed", r.returncode == 1, r.stdout + r.stderr)

        # (c) same outcome on both peers (workload never split winner/loser) is refused outright.
        h, c = _make_session(os.path.join(td, "c"), "m", 10.0, 5, 10.05, 5, 0.0)
        r = run(["--expect", "fixed", h, c])
        check(
            "(c) same outcome on both peers is refused",
            r.returncode == 1 and "SAME on_gameover outcome" in r.stdout,
        )

        # (d) the host never reached game-over -- refused, not a false 0 ms pass.
        h, c = _make_session(os.path.join(td, "d"), "m", 10.0, None, 10.05, 4, 0.0)
        r = run(["--expect", "fixed", h, c])
        check(
            "(d) missing host on_gameover is refused",
            r.returncode == 1 and "HOST's mh_net.log" in r.stdout,
        )

        # (e) baseline mode never asserts, even over the bound (a report, not a gate).
        h, c = _make_session(os.path.join(td, "e"), "m", 10.0, 5, 13.6, 4, 600.0)
        r = run(["--expect", "baseline", h, c])
        check("(e) baseline mode does not fail on a 600ms gap", r.returncode == 0)

        # (f) clean desync watch (0 mismatching, 5 compared) + a clean winner PASSES -- the default
        # _make_session desync_line, exercised via a plain 0/0-ms-gap arm.
        h, c = _make_session(os.path.join(td, "f"), "m", 10.0, 5, 10.05, 4, 0.0)
        r = run(["--expect", "fixed", h, c])
        check(
            "(f) clean desync watch + clean winner passes --expect fixed",
            r.returncode == 0 and "ALL PAIRS IDENTICAL" in r.stdout,
            r.stdout + r.stderr,
        )

        # (g) MUTATION RED: real desync (mismatching > 0) FAILS even though both peers' own frame
        # gaps are 0 ms -- catches a checker that only ever looked at frame timing.
        h, c = _make_session(
            os.path.join(td, "g"),
            "m",
            10.0,
            5,
            10.05,
            4,
            0.0,
            desync_line="; [desync] *** DESYNC step=100 peer=1 mine=AA theirs=BB first_region=41 "
            "order_queue (mismatch #1)\n; [desync] match end: 3 mismatching / 5 compared "
            "sample(s), 0 dropped as too old, 0 bad frame(s), 3 order-digest mismatching, "
            "324 steps seen\n",
        )
        r = run(["--expect", "fixed", h, c])
        check(
            "(g) nonzero desync mismatching FAILS regardless of frame gaps",
            r.returncode == 1 and "DESYNC" in r.stdout.upper(),
            r.stdout + r.stderr,
        )

        # (h) MUTATION RED: a vacuous 0/0 desync read (match ended before the watch paired up even
        # one comparison) is refused, not waved through as "0 mismatching, so IDENTICAL".
        h, c = _make_session(
            os.path.join(td, "h"),
            "m",
            10.0,
            5,
            10.05,
            4,
            0.0,
            desync_line="; [desync] match end: 0 mismatching / 0 compared sample(s), 0 dropped as "
            "too old, 0 bad frame(s), 0 order-digest mismatching, 84 steps seen\n",
        )
        r = run(["--expect", "fixed", h, c])
        check(
            "(h) vacuous 0/0 desync read is refused, not a false pass",
            r.returncode == 1 and "vacuous" in r.stdout.lower(),
            r.stdout + r.stderr,
        )

        # (i) no desync 'match end' line at all (watch never ran / line lost) is refused.
        h, c = _make_session(os.path.join(td, "i"), "m", 10.0, 5, 10.05, 4, 0.0, desync_line="")
        r = run(["--expect", "fixed", h, c])
        check(
            "(i) missing desync 'match end' line is refused",
            r.returncode == 1 and "desync" in r.stdout.lower(),
            r.stdout + r.stderr,
        )

        # (j) MUTATION RED -- clause (b): the WINNER's own 600 ms gap FAILS even though the loser's
        # own gap is 0 ms -- catches a checker that only ever measured the loser (the original P6
        # clause, kept, but not sufficient alone per the user's 2026-09-25 decision).
        h, c = _make_session(os.path.join(td, "j"), "m", 10.0, 5, 13.6, 4, 0.0, host_gap_ms=600.0)
        r = run(["--expect", "fixed", h, c])
        check(
            "(j) winner's own 600ms gap FAILS even with a 0ms loser gap",
            r.returncode == 1 and "host (winner)" in r.stdout.lower(),
            r.stdout + r.stderr,
        )

    if failures:
        print("check_p6_gap --selftest: FAIL -- %s" % ", ".join(failures))
        return 1
    print("check_p6_gap --selftest: PASS (%d arms)" % 10)
    return 0


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect", choices=["baseline", "fixed"], default="baseline")
    ap.add_argument("--max-ms", type=float, default=500.0)
    ap.add_argument(
        "--pad-s",
        type=float,
        default=0.5,
        help="slack added on both ends of the window for anchor noise",
    )
    ap.add_argument("host_dir")
    ap.add_argument("client_dir")
    args = ap.parse_args()

    def read(d, name):
        with open(os.path.join(d, name), encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()

    host_net = read(args.host_dir, "mh_net.log")
    client_net = read(args.client_dir, "mh_net.log")

    host_go = first_gameover(host_net)
    client_go = first_gameover(client_net)
    if not host_go:
        print(
            "FAIL: no 'on_gameover ENTER' line in the HOST's mh_net.log -- match never reached "
            "game-over (%s)" % args.host_dir
        )
        return 1
    if not client_go:
        print(
            "FAIL: no 'on_gameover ENTER' line in the CLIENT's mh_net.log -- match never reached "
            "game-over (%s)" % args.client_dir
        )
        return 1
    host_wall, host_outcome, host_gclk = host_go
    client_wall, client_outcome, client_gclk = client_go
    if host_outcome == client_outcome:
        print(
            "FAIL: host and client logged the SAME on_gameover outcome (%d) -- the conquest "
            "workload did not produce a winner/loser split (armed? see [harness] conq=1)"
            % host_outcome
        )
        return 1
    print(
        "host (winner): on_gameover outcome=%d gclk=%d  |  client (loser): outcome=%d gclk=%d"
        % (host_outcome, host_gclk, client_outcome, client_gclk)
    )

    # ---- clause (a): the D21 desync watch, read from the host's own mh_net.log -------------------
    de = desync_end(host_net)
    if de is None:
        print(
            "FAIL: no D21 desync-watch 'match end' line in the HOST's mh_net.log -- the watch "
            "never reported (off, or the match ended before its session-end hook ran)"
        )
        return 1
    mismatching, compared, steps_seen = de
    print(
        "desync watch (host's own mh_net.log): %d mismatching / %d compared sample(s), %d steps "
        "seen" % (mismatching, compared, steps_seen)
    )
    if any(_DESYNC_BAD_NEEDLE in ln for ln in host_net):
        print("FAIL: '%s' present in the host's mh_net.log" % _DESYNC_BAD_NEEDLE)
        return 1
    if compared <= 0:
        print(
            "FAIL: vacuous desync-watch read (0 compared samples over %d steps) -- the match ended "
            "before the watch's every-N-step cadence could pair up even one comparison, so "
            "'0 mismatching' is not evidence of anything; extend the run (see mp:P6's own "
            "harness_extra_host comment in tools/test_ui.py)" % steps_seen
        )
        return 1
    if mismatching > 0:
        print(
            "FAIL: %d/%d desync samples mismatching -- NOT all pairs identical"
            % (mismatching, compared)
        )
        return 1
    print(
        "ALL PAIRS IDENTICAL: 0/%d desync samples mismatching over %d steps"
        % (compared, steps_seen)
    )

    # ---- clause (b) + the original loser clause: each peer's own longest frame gap in the window --
    client_gap, err = frame_gap(
        args.client_dir, client_net, host_wall, client_wall, args.pad_s, "the client"
    )
    if err:
        print("FAIL: " + err)
        return 1
    host_gap, err = frame_gap(
        args.host_dir, host_net, host_wall, client_wall, args.pad_s, "the host"
    )
    if err:
        print("FAIL: " + err)
        return 1

    print(
        "window (host's own outcome -> client's own outcome, +/-%.1fs pad): %.0f ms total span"
        % (args.pad_s, client_gap["span_ms"])
    )
    print(
        "client (loser) longest single present-to-present gap in that window: %.0f ms "
        "(outlier rows in window: %d, total frametime rows: %d)"
        % (client_gap["max_gap_ms"], client_gap["hits"], client_gap["total"])
    )
    print(
        "host (winner) longest single present-to-present gap in that window: %.0f ms "
        "(outlier rows in window: %d, total frametime rows: %d)"
        % (host_gap["max_gap_ms"], host_gap["hits"], host_gap["total"])
    )

    if args.expect == "fixed":
        bad = []
        if client_gap["max_gap_ms"] >= args.max_ms:
            bad.append("client (loser) %.0f ms" % client_gap["max_gap_ms"])
        if host_gap["max_gap_ms"] >= args.max_ms:
            bad.append("host (winner) %.0f ms" % host_gap["max_gap_ms"])
        if bad:
            print("FAIL: %s >= the %.0f ms bound" % (", ".join(bad), args.max_ms))
            return 1
        print(
            "PASS: loser %.0f ms and winner %.0f ms both < the %.0f ms bound"
            % (client_gap["max_gap_ms"], host_gap["max_gap_ms"], args.max_ms)
        )
        return 0

    print("(baseline mode -- no assertion; pass --expect fixed once the grace fix is in)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
