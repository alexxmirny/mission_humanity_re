#!/usr/bin/env python3
"""check_data_timeout.py -- mp:GS2: A PEER WHOSE SIM IS FENCED (TRANSPORT ALIVE, NO NEW LOCKSTEP DATA)
MUST BE DROPPED AT data_timeout_ms, not left to hang until someone quits by hand.

THE BUG THIS GATES (the 2026-09-20 field freezes; the known silent-peer class):
MH_NetStats.last_rx_tick counts DATA frames and the link watchdog counts keepalives (10 s) -- a peer
whose SIM stalls but whose TRANSPORT stays up keeps the link "alive" forever, so `since_rx` climbed
22-35 s with no exit in every 2026-09-20 freeze. mh.dll's GS2 watchdog (net_lockstep.cpp
`data_timeout_tick`, called from `on_time_tick` right after `lateness_tick`) reads
`g_late_last_move[]` -- the per-peer horizon-movement clock the lookahead controller already
maintains straight off `ADDR_PEER_HORIZON`, so it needs no lockstep promotion and runs in
CONFIGURATION (1) too -- and once a peer's horizon has not moved for `[net] data_timeout_ms`, runs
the same direct `llm_net_player_remove` call U17(b)'s transport-death fast-drop uses.

WHAT IT ASSERTS, off the GS2 log line (`; GS2: peer <N> data-silent for <since> ms > <T> -> dropped`)
in mh_net.log:

  1. EXACTLY ONE GS2 drop line exists, across BOTH peers combined. Zero on both is a REFUSAL (this
     tool has no evidence the fix even ran); one on each, or two on the same peer, is a concrete
     defect and FAILS by name rather than being waved off -- the "frozen peer also drops the healthy
     survivor" shape (clause 5) is exactly this case, so it is not a separate code path.
  2. The line's own `T` (the configured `data_timeout_ms`) matches `--timeout-ms` when given.
  3. The line's own `since` is within `[T, T + --max-frame-ms]` (default 2000 ms) -- "T (+1 step)":
     a `since` below T would mean the watchdog fired early, and a value far above T would mean it sat
     on an already-expired slot for multiple frames before acting, which is not "at T" either.
  4. Implied by clause 1: the FROZEN peer's own log carries NO GS2 drop line at all -- the peer whose
     horizon legitimately stalled must not itself declare the (perfectly healthy) survivor data-silent.

ABSENCE IS A FAILURE (check_module_bind.py's rule): no lane, no mh_net.log, no GS2 line at all -- each
is a refusal; more than one line is a fail, never a pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_data_timeout.py [--timeout-ms T] [--max-frame-ms M] <host-run-dir> <client-run-dir>
  python tools/check_data_timeout.py --selftest                          planted lanes; every negative RED

`<run-dir>` is what test_ui.py's `post_check` machinery (with `post_check_peers`) hands a checker for
every peer: that lane's newest run directory. gs2_data_timeout is a lane SHARER (TL-LANEPOOL, borrows
d28_canceltask's lanes since TL-SUITE-FOLD-ML folded match_launch into shim_udp), so each peer's
`logs/` may hold more than this process's own session -- each
run directory is resolved to its lane's `logs/` and the SESSION directories whose `session.json`
`process_dir` names this run (check_rematch_residue.py's pattern), plus the process directory's own
mh_net.log (the GS2 line is written by the main-thread `on_time_tick` seam, which runs inside the
open SESSION directory once the match has entered lockstep -- but the process directory is read too,
the same defensive belt check_cheat_gate.py's `peer_lines` uses, since a boundary arm banner can land
there before any session opens).
"""

import glob
import json
import os
import re
import shutil
import sys
import tempfile


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


SESSION_DIR_RE = re.compile(r"^\d{8}T\d{6}Z_[0-9a-f]{8}_\d+_[A-Za-z0-9]+$")

GS2_RE = re.compile(r"; GS2: peer (\d+) data-silent for (\d+) ms > (\d+) -> dropped")

DEFAULT_MAX_FRAME_MS = (
    2000  # "T (+1 step)" -- generous for rig jitter, still tight vs a multi-second hang
)


def session_process_dir(folder):
    fp = os.path.join(folder, "session.json")
    try:
        with open(fp, encoding="utf-8", errors="replace") as fh:
            d = json.load(fh)
    except (OSError, ValueError):
        return None
    return d.get("process_dir") if isinstance(d, dict) else None


def session_runs(logs_dir, process_leaf):
    c = [
        d
        for d in glob.glob(os.path.join(logs_dir, "*"))
        if os.path.isdir(d)
        and SESSION_DIR_RE.match(os.path.basename(d))
        and session_process_dir(d) == process_leaf
    ]
    return sorted(c, key=lambda d: os.path.basename(d))


def read_lines(folder):
    fp = os.path.join(folder, "mh_net.log")
    if not os.path.isfile(fp):
        return None
    with open(fp, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def peer_lines(run_dir):
    """All mh_net.log lines of one peer: the process directory's, then each of its sessions', in
    order (check_cheat_gate.py's pattern). Refuses when nothing is readable."""
    run_dir = os.path.abspath(run_dir)
    if not os.path.isdir(run_dir):
        raise Refusal("%s is not a directory" % run_dir)
    logs_dir = os.path.dirname(run_dir)
    leaf = os.path.basename(run_dir)
    out = []
    proc = read_lines(run_dir)
    if proc is not None:
        out.extend(proc)
    sessions = session_runs(logs_dir, leaf)
    for s in sessions:
        ln = read_lines(s)
        if ln is not None:
            out.extend(ln)
    if not out:
        raise Refusal("%s: no mh_net.log in the process directory or its sessions" % run_dir)
    if not sessions:
        raise Refusal(
            "%s: no session directory names it as process_dir (the walk never opened a lobby)"
            % run_dir
        )
    return out


def gs2_drops(lines):
    out = []
    for ln in lines:
        m = GS2_RE.search(ln)
        if m:
            out.append(tuple(int(x) for x in m.groups()))
    return out


def check(dirs, timeout_ms=None, max_frame_ms=DEFAULT_MAX_FRAME_MS):
    if len(dirs) != 2:
        raise Refusal("expected exactly two run directories (host, client); got %d" % len(dirs))
    peers = [peer_lines(d) for d in dirs]
    drops = [gs2_drops(p) for p in peers]

    survivors = [i for i, d in enumerate(drops) if d]
    total_lines = sum(len(d) for d in drops)
    if not survivors:
        raise Refusal("neither peer's mh_net.log carries a `; GS2: ... -> dropped` line")
    fails = []
    if total_lines != 1:
        # NOT a refusal: two-or-more lines is concrete evidence of a real defect (both peers
        # dropping each other, or the same peer firing twice), so it is named and FAILED rather
        # than waved off as "this tool cannot say" -- unlike the zero-lines case above, which
        # really is silence with no evidence to report.
        fails.append(
            "expected exactly ONE GS2 drop line across both peers, found %d: %s"
            % (total_lines, {dirs[i]: drops[i] for i in range(2)})
        )
        for f in fails:
            print("  [FAIL] %s" % f)
        print("check_data_timeout: FAIL (%d)" % len(fails))
        return 1
    surv = survivors[0]
    frozen = 1 - surv
    dropped_side, since_ms, t_ms = drops[surv][0]
    print(
        "  survivor: %s  (dropped side=%d, since=%d ms, T=%d ms)"
        % (dirs[surv], dropped_side, since_ms, t_ms)
    )
    print("  frozen peer: %s" % dirs[frozen])

    if timeout_ms is not None and t_ms != timeout_ms:
        fails.append(
            "the drop line's own T=%d ms does not match --timeout-ms=%d" % (t_ms, timeout_ms)
        )
    if since_ms < t_ms:
        fails.append(
            "since=%d ms is BELOW its own T=%d ms -- the watchdog fired early" % (since_ms, t_ms)
        )
    if since_ms > t_ms + max_frame_ms:
        fails.append(
            "since=%d ms is %d ms past T=%d ms (budget %d ms) -- the watchdog sat on an already-"
            "expired slot instead of acting at T" % (since_ms, since_ms - t_ms, t_ms, max_frame_ms)
        )

    for f in fails:
        print("  [FAIL] %s" % f)
    if fails:
        print("check_data_timeout: FAIL (%d)" % len(fails))
        return 1
    print(
        "check_data_timeout: PASS -- side %d dropped at %d ms (T=%d ms, +%d ms)"
        % (dropped_side, since_ms, t_ms, since_ms - t_ms)
    )
    return 0


# ---- selftest: planted lanes ----------------------------------------------------------------------

_BANNER = "[00:00:00.000] ; net arm ok\n"
_DROP = "[00:00:03.010] ; GS2: peer 1 data-silent for 3010 ms > 3000 -> dropped\n"
_DROP_EARLY = "[00:00:02.500] ; GS2: peer 1 data-silent for 2500 ms > 3000 -> dropped\n"
_DROP_LATE = "[00:00:09.000] ; GS2: peer 1 data-silent for 9000 ms > 3000 -> dropped\n"
_DROP_WRONG_T = "[00:00:03.010] ; GS2: peer 1 data-silent for 3010 ms > 5000 -> dropped\n"
_DROP_SELF = "[00:00:03.010] ; GS2: peer 0 data-silent for 3010 ms > 3000 -> dropped\n"


def _plant(root, name, text):
    lane = os.path.join(root, name)
    logs = os.path.join(lane, "logs")
    menu_leaf = "20260921T000000Z_menu_solo"
    menu = os.path.join(logs, menu_leaf)
    os.makedirs(menu)
    sess = os.path.join(logs, "20260921T000001Z_00000001_0_solo")
    os.makedirs(sess)
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        fh.write(_BANNER + text)
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "00000001", "process_dir": menu_leaf}, fh)
    return menu


def selftest():
    # (title, host_text, client_text, timeout_ms, want)
    cases = [
        ("clean drop, host is the survivor", _DROP, _BANNER, 3000, 0),
        ("clean drop, client is the survivor (host is the frozen one)", _BANNER, _DROP, 3000, 0),
        ("fired early (since < T)", _DROP_EARLY, _BANNER, 3000, 1),
        ("fired late (since >> T + budget)", _DROP_LATE, _BANNER, 3000, 1),
        ("--timeout-ms mismatch", _DROP, _BANNER, 5000, 1),
        ("the frozen peer also drops (self-drop shape)", _DROP, _DROP_SELF, 3000, 1),
        ("both peers drop each other", _DROP, _DROP, 3000, 1),
        ("neither peer drops anything (the pre-fix hang)", _BANNER, _BANNER, 3000, "refuse"),
    ]
    bad = 0
    for title, host_text, client_text, timeout_ms, want in cases:
        root = tempfile.mkdtemp(prefix="gs2_selftest_")
        try:
            h = _plant(root, "ui_x_host", host_text)
            c = _plant(root, "ui_x_c1", client_text)
            import contextlib
            import io

            buf = io.StringIO()
            try:
                with contextlib.redirect_stdout(buf):
                    got = check([h, c], timeout_ms=timeout_ms)
            except Refusal:
                got = "refuse"
        finally:
            shutil.rmtree(root, ignore_errors=True)
        ok = got == want
        bad += 0 if ok else 1
        print("  [%s] %-58s want=%s got=%s" % ("ok" if ok else "BAD", title, want, got))
    print("selftest: %s" % ("PASS" if bad == 0 else "FAIL (%d)" % bad))
    return 0 if bad == 0 else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    timeout_ms = None
    max_frame_ms = DEFAULT_MAX_FRAME_MS
    dirs = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--timeout-ms":
            timeout_ms = int(argv[i + 1])
            i += 2
            continue
        if a == "--max-frame-ms":
            max_frame_ms = int(argv[i + 1])
            i += 2
            continue
        if a.startswith("--"):
            i += 1
            continue
        dirs.append(a)
        i += 1
    try:
        return check(dirs, timeout_ms=timeout_ms, max_frame_ms=max_frame_ms)
    except Refusal as e:
        print("[REFUSED] %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
