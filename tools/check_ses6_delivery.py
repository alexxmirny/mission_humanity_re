#!/usr/bin/env python3
"""check_ses6_delivery.py -- mp:SES6: PROVE OUTBOUND DELIVERY FROM ONE PEER'S LOG ALONE.

THE QUESTION THIS ANSWERS (2026-09-20 field report §8/§9.3): when a match freezes with one side
"keepalive-alive but data-silent" (GS1/GS2's class), the pre-SES6 evidence for "did I keep sending, or
did the far side stop" needed BOTH peers' logs cross-matched by hand (§9.3's table). SES6 put the two
halves of that answer on one line -- `src/mh_dll/mh_net_udp/udp_endpoint.cpp`'s 10 s
"net: udp counters" line, per-peer since -- so this tool reads ONE run's mh_net.log and says whether
the shape is there: this peer's inbound game DATA aged out while its outbound stayed fresh and the
peer's own advertised lockstep horizon sat still.

WHAT IT PARSES (tools/data/log_formats.json's `net.udp_counters`): the repeating
` | peer<id> data_rx_age <ms|-1> data_tx_age <ms|-1> data_rx_bytes <n> data_tx_bytes <n>
horizon_ms <ms|-1>` segment(s) SES6 appended to the aggregate counters line. -1 on an age column means
"no FLAG_DATA seen yet in that direction", not a real elapsed time, and -1 on horizon_ms means mh.dll
never pushed one for that peer (no lockstep seam bound).

WHAT IT ASSERTS (`check`), over one peer's samples for ONE conn -- on the TRAILING FREEZE WINDOW
(everything after the last sample whose data_rx_age is still fresh, < FRESH_MS), because a real run
has a healthy phase first:
  1. At least MIN_SAMPLES counters lines carry that conn (a baseline and a tick after it; a match
     shorter than that, or a peer that never entered lockstep, REFUSES rather than answering).
  2. The window is non-empty (the LAST sample is not fresh), data_rx_age is NON-DECREASING inside it
     and its last value clears RISE_FLOOR_MS -- "the gap since our last inbound DATA kept growing".
  3. data_tx_age never exceeds TX_AGE_CEIL_MS on any window sample -- "we kept sending" is a
     per-sample bound, not an average, so one silent tick cannot hide inside a good mean.
  4. horizon_ms was pushed (>= 0) on the window and, when the window has >= 2 samples, is IDENTICAL
     across it -- "the peer's advertised horizon never moved", the game-level confirmation that this
     is a stalled SIM and not merely a slow link. A one-sample window (a whole-process freeze: the
     link watchdog drops the keepalive-silent conn within one tick) reports the horizon instead.
Each clause is independently named on failure; the caller decides whether a partial match (e.g. rx_age
climbing but horizon still moving, a plain-slow-peer shape rather than a stalled one) is still useful.

ABSENCE IS A FAILURE (check_module_bind.py's rule, check_data_timeout.py's precedent): no run
directory, no mh_net.log, no `net: udp counters` line at all, or no per-peer segment for any conn --
each REFUSES; it is never silently a pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_ses6_delivery.py [--peer N] [--min-samples K] <run-dir>
  python tools/check_ses6_delivery.py --selftest                    planted logs; every negative RED

`<run-dir>` is a peer's own log directory the way test_ui.py's post_check hands one to a checker (the
process dir, or a session dir -- either is read directly; this tool does not need the
process-dir-plus-sessions walk check_data_timeout.py's peer_lines does, because the counters line is
written by the SAME endpoint object for the whole process and every session's mh_net.log the endpoint
opens carries its own copy). `--peer N` pins which conn's segments to grade when a log carries more
than one peer_id (a >2-player run); omitted, the tool grades whichever peer_id has the most samples,
which is the reliable single conn on a 2-player match.
"""

import os
import re
import shutil
import sys
import tempfile


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


COUNTERS_RE = re.compile(r"net: udp counters dgram tx ")
PEER_SEG_RE = re.compile(
    r"peer(-?\d+) data_rx_age (-?\d+) data_tx_age (-?\d+) "
    r"data_rx_bytes (-?\d+) data_tx_bytes (-?\d+) horizon_ms (-?\d+)"
)

MIN_SAMPLES = 2  # two 10 s ticks is the least that can show a TREND rather than a single reading
RISE_FLOOR_MS = 2000  # the window's total data_rx_age rise must clear this to count as "climbing"
TX_AGE_CEIL_MS = (
    3000  # generous vs the 10 s cadence: "we kept sending" allows one slow tick, not one dead one
)


def read_lines(run_dir):
    """mh_net.log of the run dir AND of the PROCESS dir its session.json names: the `net: udp
    counters` line is written by the transport module into the process directory's log, while
    test_ui.py's post_check hands a checker the newest SESSION directory. Only that one process dir
    is read (a lane's older process dirs hold other runs' samples)."""
    cands = [os.path.join(run_dir, "mh_net.log")]
    sj = os.path.join(run_dir, "session.json")
    if os.path.isfile(sj):
        try:
            import json

            pd = json.load(open(sj, encoding="utf-8")).get("process_dir")
        except Exception:
            pd = None
        if pd:
            parent = os.path.dirname(os.path.abspath(run_dir).rstrip("\\/"))
            cands.append(os.path.join(parent, pd, "mh_net.log"))
    out = []
    for fp in cands:
        if os.path.isfile(fp):
            with open(fp, encoding="utf-8", errors="replace") as fh:
                out.extend(fh.read().splitlines())
    if not out:
        raise Refusal("%s: no mh_net.log in the run dir or its process dir" % run_dir)
    return out


def counters_samples(lines):
    """[(peer_id, rx_age, tx_age, rx_bytes, tx_bytes, horizon_ms), ...] per segment, in log order,
    across every `net: udp counters` line found -- one sample per (line, peer) pair."""
    out = []
    for ln in lines:
        if COUNTERS_RE.search(ln) is None:
            continue
        for m in PEER_SEG_RE.finditer(ln):
            out.append(tuple(int(x) for x in m.groups()))
    return out


def by_peer(samples):
    peers = {}
    for pid, rx_age, tx_age, rx_b, tx_b, hz in samples:
        peers.setdefault(pid, []).append((rx_age, tx_age, rx_b, tx_b, hz))
    return peers


FRESH_MS = 1000  # a data_rx_age under one 10 s tick's worth of jitter means DATA is still arriving


def check(run_dir, peer=None, min_samples=MIN_SAMPLES):
    """A real run has a HEALTHY phase (rx_age ~0 on every tick, horizon advancing) and then, if a
    freeze happens, a trailing window where rx_age climbs tick over tick. The verdict is over that
    trailing window: its BASELINE is the last sample whose data_rx_age is still fresh (< FRESH_MS),
    and the window is every sample after it. Measured 2026-09-21 (rig, client PROCESS suspended
    mid-match): healthy ticks read rx_age 16 / horizon 9380 -> 19370 -> 29370, then ONE tick reads
    rx_age 7485 / tx_age 0 / horizon 31910 before the link watchdog (10 s keepalive silence) drops
    the conn and the segment disappears -- so a whole-process freeze can only ever show ONE rising
    sample, and horizon-frozen is provable only when the far transport keeps keepalives flowing (the
    field's 2026-09-20 shape, `since_rx` 22-35 s with the link up). The clauses adapt: with one rising
    sample the rx/tx clauses decide and the horizon is reported, not asserted."""
    lines = read_lines(run_dir)
    samples = counters_samples(lines)
    if not samples:
        raise Refusal(
            "%s: mh_net.log has no `net: udp counters` line carrying a peer segment -- either the "
            "match never reached the 10 s cadence, or this build predates mp:SES6" % run_dir
        )
    peers = by_peer(samples)
    if peer is not None:
        if peer not in peers:
            raise Refusal("%s: no samples for --peer %d (have %s)" % (run_dir, peer, sorted(peers)))
        pid = peer
    else:
        pid = max(peers, key=lambda k: len(peers[k]))
    rows = peers[pid]

    fails = []
    print("  peer %d: %d sample(s)" % (pid, len(rows)))
    if len(rows) < min_samples:
        raise Refusal(
            "%s: peer %d has only %d sample(s), need >= %d (a baseline and at least one tick after "
            "it; match too short, or lockstep never ran long enough to hit a second 10 s tick)"
            % (run_dir, pid, len(rows), min_samples)
        )

    rx_ages = [r[0] for r in rows]
    tx_ages = [r[1] for r in rows]
    horizons = [r[4] for r in rows]
    print("  data_rx_age: %s" % rx_ages)
    print("  data_tx_age: %s" % tx_ages)
    print("  horizon_ms:  %s" % horizons)

    # the trailing window: baseline = the last FRESH sample; window = everything after it.
    base = None
    for i in range(len(rx_ages) - 1, -1, -1):
        if 0 <= rx_ages[i] < FRESH_MS:
            base = i
            break
    if base is None:
        base = 0  # never fresh: the whole run is the window (a peer that was silent from the start)
    win = list(range(base + 1, len(rows)))
    print("  freeze window: baseline sample %d, %d rising sample(s)" % (base, len(win)))
    if not win:
        fails.append(
            "data_rx_age is fresh (< %d ms) on the LAST sample -- DATA kept arriving to the end, this "
            "peer never went data-silent" % FRESH_MS
        )
    else:
        # clause 2: rx_age non-decreasing across the window and its last value clears the floor.
        for i in win[1:]:
            if rx_ages[i] < rx_ages[i - 1]:
                fails.append(
                    "data_rx_age DROPPED inside the window at sample %d (%d -> %d) -- new DATA "
                    "arrived, this peer is not data-silent" % (i, rx_ages[i - 1], rx_ages[i])
                )
        if rx_ages[win[-1]] < RISE_FLOOR_MS:
            fails.append(
                "data_rx_age reached only %d ms (need >= %d ms) -- not climbing enough to call this "
                "peer data-silent" % (rx_ages[win[-1]], RISE_FLOOR_MS)
            )
        # clause 3: tx_age bounded on every window sample -- "we kept sending".
        for i in win:
            v = tx_ages[i]
            if v < 0:
                fails.append(
                    "sample %d: data_tx_age is -1 (never sent) -- outbound delivery is unproven, "
                    "not silent" % i
                )
            elif v > TX_AGE_CEIL_MS:
                fails.append(
                    "sample %d: data_tx_age %d ms exceeds the %d ms ceiling -- WE stopped sending "
                    "too, this is not a one-sided freeze" % (i, v, TX_AGE_CEIL_MS)
                )
        # clause 4: the horizon. Pushed at all (>= 0 on the window), and FROZEN across the window when
        # the window is long enough to say so.
        if any(horizons[i] < 0 for i in win):
            fails.append(
                "horizon_ms is -1 inside the window -- nothing was pushed for this peer (no lockstep "
                "seam bound)"
            )
        elif len(win) >= 2 and len(set(horizons[i] for i in win)) != 1:
            fails.append(
                "horizon_ms MOVED across the window (%s) -- the peer's sim is still advancing, this "
                "is a slow link, not a stalled one" % sorted(set(horizons[i] for i in win))
            )
        elif len(win) == 1:
            print(
                "  note: one rising sample (the link watchdog drops a keepalive-silent conn within "
                "one tick) -- horizon %d reported, frozen-ness not provable from one sample"
                % horizons[win[0]]
            )

    for f in fails:
        print("  [FAIL] %s" % f)
    if fails:
        print("check_ses6_delivery: FAIL (%d)" % len(fails))
        return 1
    print(
        "check_ses6_delivery: PASS -- peer %d data_rx_age climbed to %d ms while data_tx_age stayed "
        "<= %d ms and horizon_ms sat at %d"
        % (pid, rx_ages[win[-1]], max(tx_ages[i] for i in win), horizons[win[-1]])
    )
    return 0


_BANNER = "[00:00:00.000] net: udp accepted 192.0.2.1:5 -> conn 0 (player 1)\n"


def _counters_line(t, peer_id, rx_age, tx_age, rx_b, tx_b, horizon_ms):
    return (
        "[00:00:%02d.000] net: udp counters dgram tx 1 rx 1 | seg tx 1 new 1 dup 0 | repaired K 0 "
        "rto 0 (rto sent 0) | gap stalls 0 (worst 0 ms) | mac-fail 0 replay 0 malformed 0 "
        "wrong-conn 0 | peer%d data_rx_age %d data_tx_age %d data_rx_bytes %d data_tx_bytes %d "
        "horizon_ms %d\n" % (t, peer_id, rx_age, tx_age, rx_b, tx_b, horizon_ms)
    )


def _plant(root, name, lines_text):
    run_dir = os.path.join(root, name)
    os.makedirs(run_dir)
    with open(os.path.join(run_dir, "mh_net.log"), "w", encoding="utf-8") as fh:
        fh.write(_BANNER + lines_text)
    return run_dir


def selftest():
    # the exact GS2 shape: rx_age climbs, tx_age stays ~0, horizon frozen.
    stalled = "".join(
        [
            _counters_line(10, 1, 0, 0, 1000, 1000, 5990),
            _counters_line(20, 1, 10000, 20, 1000, 20000, 5990),
            _counters_line(30, 1, 20000, 10, 1000, 40000, 5990),
        ]
    )
    # a slow-but-alive peer: rx_age climbs a little each sample but never actually falls behind, and
    # the horizon keeps moving -- should FAIL clause 4.
    slow_alive = "".join(
        [
            _counters_line(10, 1, 100, 50, 1000, 1000, 5990),
            _counters_line(20, 1, 150, 60, 2000, 2000, 6090),
            _counters_line(30, 1, 120, 40, 3000, 3000, 6190),
        ]
    )
    # both directions dead (a real two-way network loss, not a one-sided sim stall) -- FAIL clause 3.
    both_dead = "".join(
        [
            _counters_line(10, 1, 0, 0, 1000, 1000, 5990),
            _counters_line(20, 1, 10000, 10000, 1000, 1000, 5990),
            _counters_line(30, 1, 20000, 20000, 1000, 1000, 5990),
        ]
    )
    # rx_age goes backwards (new data arrived) -- FAIL clause 2's drop check.
    recovered = "".join(
        [
            _counters_line(10, 1, 8000, 0, 1000, 1000, 5990),
            _counters_line(20, 1, 500, 10, 2000, 2000, 5990),
        ]
    )
    # too short a window to say anything -- REFUSE.
    one_sample = _counters_line(10, 1, 5000, 0, 1000, 1000, 5990)
    # never pushed a horizon at all (no lockstep seam bound) -- FAIL clause 4's -1 case.
    no_horizon = "".join(
        [
            _counters_line(10, 1, 0, 0, 1000, 1000, -1),
            _counters_line(20, 1, 10000, 20, 1000, 2000, -1),
        ]
    )
    # no counters line at all -- REFUSE.
    empty = ""
    # MEASURED 2026-09-21 (rig, client process suspended 40 s into the match): a healthy phase, then
    # ONE rising tick before the link watchdog drops the conn. PASS on the window; horizon reported.
    measured_suspend = "".join(
        [
            _counters_line(10, 1, 1000, 16, 34, 4980, -1),
            _counters_line(20, 1, 16, 16, 15923, 22112, 9380),
            _counters_line(30, 1, 16, 16, 32702, 38891, 19370),
            _counters_line(40, 1, 16, 16, 49439, 55628, 29370),
            _counters_line(50, 1, 7485, 0, 53719, 72607, 31910),
        ]
    )
    # MEASURED 2026-09-21 (rig, client SIM fenced from the start): DATA frames keep flowing from the
    # fenced peer's transport, so rx_age stays fresh while the horizon sits still -- NOT the
    # data-silent shape; FAIL (this is what the GS2 fence looks like at the transport level).
    measured_fence = "".join(
        [
            _counters_line(10, 1, 984, 0, 34, 4980, -1),
            _counters_line(20, 1, 0, 15, 15125, 22220, 1030),
            _counters_line(30, 1, 16, 16, 31883, 39169, 1030),
            _counters_line(40, 1, 0, 16, 48662, 56097, 1030),
        ]
    )

    cases = [
        ("the GS2 shape: rx climbs, tx flat, horizon frozen", stalled, 0),
        ("slow but alive: horizon still moving", slow_alive, 1),
        ("both directions dead: not one-sided", both_dead, 1),
        ("data recovered: rx_age dropped", recovered, 1),
        ("only one sample: cannot show a trend", one_sample, "refuse"),
        ("horizon never pushed (-1)", no_horizon, 1),
        ("no counters line at all", empty, "refuse"),
        ("MEASURED: process suspended, one rising tick", measured_suspend, 0),
        ("MEASURED: sim fenced, DATA still flowing", measured_fence, 1),
    ]
    bad = 0
    for title, text, want in cases:
        root = tempfile.mkdtemp(prefix="ses6_selftest_")
        try:
            d = _plant(root, "peer", text)
            import contextlib
            import io

            buf = io.StringIO()
            try:
                with contextlib.redirect_stdout(buf):
                    got = check(d)
            except Refusal:
                got = "refuse"
        finally:
            shutil.rmtree(root, ignore_errors=True)
        ok = got == want
        bad += 0 if ok else 1
        print("  [%s] %-52s want=%s got=%s" % ("ok" if ok else "BAD", title, want, got))
    print("selftest: %s" % ("PASS" if bad == 0 else "FAIL (%d)" % bad))
    return 0 if bad == 0 else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    peer = None
    min_samples = MIN_SAMPLES
    dirs = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--peer":
            peer = int(argv[i + 1])
            i += 2
            continue
        if a == "--min-samples":
            min_samples = int(argv[i + 1])
            i += 2
            continue
        if a.startswith("--"):
            i += 1
            continue
        dirs.append(a)
        i += 1
    if len(dirs) != 1:
        print("[REFUSED] expected exactly one run directory; got %d" % len(dirs))
        return 2
    try:
        return check(dirs[0], peer=peer, min_samples=min_samples)
    except Refusal as e:
        print("[REFUSED] %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
