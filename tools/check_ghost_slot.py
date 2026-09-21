#!/usr/bin/env python3
"""check_ghost_slot.py -- mp:GS1(b): A HOST WHOSE LOBBY LOSES A PEER TO A PROCESS EXIT AND THEN
SEATS A FRESH ONE MUST NOT CARRY A GHOST SLOT INTO THE MATCH, proven from the host's (and the
seated client's) `mh_lockstep.log` rather than from the pixels.

THE BUG THIS GATES (2026-09-20 player report, session report Sec.11; tracker mp:GS1 scope clause
(b)). The joiner's own LEAVE at clock 0 already frees the slot in the SAME-PROCESS case
(`ghost_leave_rejoin` is GREEN); Rizzen's field incident wraps a REAL PROCESS EXIT around the same
sequence -- Cancel/LEAVE at 09:52:01.987, `EXIT llm_wnd_on_destroy` at 09:52:26 (quit to desktop),
a fresh process launched at 09:52:52, re-joining at 09:53:07 as slot 2 -- and the re-join landed on
a ghost: the host's committed horizon pinned at the 10000 ms initial advertisement forever (peer0
climbing 100->1780->10460 while peer1 stayed == 10000 throughout), so both peers froze at clock
9999. `ghost_exit_rejoin` reproduces the same shape with a real process exit in the middle (a NEW
game process launches only once the leaving one has actually exited -- ui_test.py's
`--client-after-exit`), where `ghost_leave_rejoin`'s same-process re-join cannot.

WHAT IT ASSERTS, off the per-frame `mh_lockstep.log` (`[net] lockstep_log=1`, on by default at
SHIP_LOG_LEVEL, so a real player's log already carries it) that net_lockstep.cpp writes into each
match's own session directory:

  1. the run holds EXACTLY ONE session directory for the launching process -- `ghost_exit_rejoin`
     never rematches (client1's lobby visit ends in a process EXIT before any Start, so the only
     session that ever opens is the one client2 seats into). Zero is a refusal (the match never
     launched); more than one is a topology this tool was not written for.
  2. that session's `mh_lockstep.log` exists and holds at least MIN_ROWS rows -- fewer is a refusal
     (the log was never armed, or the match ended before it wrote anything: a vacuous run is
     refused, never passed -- check_module_bind.py's rule).
  3. THE COMMITTED HORIZON REACHES PAST THE INITIAL 10000 MS ADVERTISEMENT: the LAST row's
     `clock_ms` and `committed_ms` are both >= PASS_MS (11000 -- comfortably past 10000, with the
     script's own `gameclock 12000` wait as headroom). Since committed = min(local horizon, every
     ACTIVE peer's horizon -- including any slot this log's two PRINTED columns cannot see, the
     retail PEER_HORIZON array is 8 wide and the log only ever prints slots 0/1) this clause cannot
     be fooled by a ghost hiding outside peer0_ms/peer1_ms: a slot pinned ANYWHERE holds
     `committed_ms` (and, by construction, `clock_ms <= committed_ms`) at its own value forever.
  4. THE REMOTE PEER'S printed slot -- NOT both -- is either well past PASS_MS - INITIAL_HORIZON_MS,
     or frozen exactly at the initial 10000 ms advertisement. `commit_horizon`'s own comment
     (turn_engine.cpp ~197) is explicit that a side's OWN slot in `_G_LLM_NET_PEER_HORIZON` is
     "never written" by that side -- its own advertised horizon lives in a different variable
     (`local_h_ms`, printed separately) -- so a HOST's `peer0_ms` (its own slot; host is always
     player index 0 in a 2-peer match) and a CLIENT's `peer1_ms` (its own slot) sit at the 10000 ms
     boot value FOREVER in every healthy match too, by construction, and asserting on them would be
     a permanent false positive. Only the column that names the OTHER peer -- host's `peer1_ms`
     (what host received FROM the client), client's `peer0_ms` (what the client received FROM the
     host) -- is a real "did the other side's horizon ever move" signal, and that is the one this
     clause checks. This is the DIAGNOSTIC clause: it names WHICH remote slot is stuck; clause 3 is
     the one that cannot miss a ghost outside the two printed columns altogether.

ABSENCE IS A FAILURE (check_module_bind.py's rule): no session directory, no log, too few rows --
each is a refusal, never a pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_ghost_slot.py <host-run-dir> <client-run-dir>
  python tools/check_ghost_slot.py --selftest        planted logs; every negative RED

`<run-dir>` is what test_ui.py's `post_check` (with `post_check_peers`) hands a checker for every
peer: that lane's newest run directory. Resolved to its lane's `logs/` and the SESSION directory
of THAT PROCESS (session.json `process_dir` naming it) -- the same resolution
check_rematch_residue.py and check_cheat_gate.py use, duplicated here rather than imported: this
tree's checkers have no import chain between them, so a bug fixed in one does not silently reach
the others (the same reason `SESSION_DIR_RE`'s literal is repeated rather than shared).
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


# mh_session_dir.h's directory shape ("<UTC>Z_<mid8>_<slot>_<role>"); textually the same literal
# check_rematch_residue.py / check_cheat_gate.py carry, for the same no-import-chain reason.
SESSION_DIR_RE = re.compile(r"^\d{8}T\d{6}Z_[0-9a-f]{8}_\d+_[A-Za-z0-9]+$")

# net_lockstep.cpp's own header line, positional: LOCKSTEP_COLS[i] is column i of every data row.
# Duplicated from mp_analyze.py's LOCKSTEP_COLS (no import chain between this tree's checkers) --
# the header sanity check in parse_lockstep() below catches drift either way.
LOCKSTEP_COLS = [
    "wall_ms",
    "clock_ms",
    "total_ms",
    "local_h_ms",
    "committed_ms",
    "peer0_ms",
    "peer1_ms",
    "step_ms",
    "stall",
    "pcount",
    "tx_pkts",
    "rx_pkts",
    "since_rx_ms",
]

INITIAL_HORIZON_MS = 10000  # the retail lookahead every PEER_HORIZON slot starts at
PASS_MS = 11000  # short of the script's own `gameclock 12000` wait, comfortably past 10000
MIN_ROWS = 20  # a genuinely-played tail this short is not a vacuous log


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


def lane_logs_dir(run_dir):
    if not os.path.isdir(run_dir):
        raise Refusal("no such run directory: %s" % run_dir)
    logs = os.path.dirname(run_dir.rstrip("\\/"))
    if os.path.basename(logs) != "logs":
        raise Refusal("%s is not a run directory under a lane's logs/" % run_dir)
    return logs


def parse_lockstep(fp):
    """mh_lockstep.log -> list of row dicts (LOCKSTEP_COLS keys, int values)."""
    rows = []
    header_seen = False
    with open(fp, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                cols = line.lstrip("#").split()
                if cols[: len(LOCKSTEP_COLS)] != LOCKSTEP_COLS:
                    raise Refusal(
                        "%s: header does not start with the expected %d columns (got %s) -- "
                        "net_lockstep.cpp's format drifted; update LOCKSTEP_COLS"
                        % (fp, len(LOCKSTEP_COLS), cols[: len(LOCKSTEP_COLS)])
                    )
                header_seen = True
                continue
            parts = line.split()
            if len(parts) < len(LOCKSTEP_COLS):
                continue
            try:
                vals = [int(x) for x in parts[: len(LOCKSTEP_COLS)]]
            except ValueError:
                continue
            rows.append(dict(zip(LOCKSTEP_COLS, vals)))
    if not header_seen:
        raise Refusal("%s: no header line (`# wall_ms ...`) -- not a real lockstep log" % fp)
    return rows


def lockstep_rows(run_dir):
    """(session_dir, rows) -- the single match session's mh_lockstep.log for one peer's run dir."""
    run_dir = os.path.abspath(run_dir)
    logs = lane_logs_dir(run_dir)
    leaf = os.path.basename(run_dir.rstrip("\\/"))
    sess = session_runs(logs, leaf)
    if len(sess) != 1:
        raise Refusal(
            "process %s holds %d session directory(ies), want EXACTLY 1 (ghost_exit_rejoin never "
            "rematches) -- %s"
            % (leaf, len(sess), ", ".join(os.path.basename(d) for d in sess) or "(none)")
        )
    fp = os.path.join(sess[0], "mh_lockstep.log")
    if not os.path.isfile(fp):
        raise Refusal("%s has no mh_lockstep.log ([net] lockstep_log was never armed)" % sess[0])
    rows = parse_lockstep(fp)
    if len(rows) < MIN_ROWS:
        raise Refusal(
            "%s: only %d lockstep row(s), want >= %d (vacuous run)" % (fp, len(rows), MIN_ROWS)
        )
    return sess[0], rows


def check(run_dirs):
    if len(run_dirs) != 2:
        raise Refusal(
            "want exactly two peer run directories (host, client), got %d -- the ghost-slot claim "
            "is cross-peer (register the row with post_check_peers)" % len(run_dirs)
        )
    fails = []
    for role, rd in zip(("host", "client"), run_dirs):
        sess_dir, rows = lockstep_rows(rd)
        first, last = rows[0], rows[-1]
        print(
            "  %-6s %s (%d rows): committed %d->%d clock %d->%d peer0 %d->%d peer1 %d->%d"
            % (
                role,
                os.path.basename(sess_dir),
                len(rows),
                first["committed_ms"],
                last["committed_ms"],
                first["clock_ms"],
                last["clock_ms"],
                first["peer0_ms"],
                last["peer0_ms"],
                first["peer1_ms"],
                last["peer1_ms"],
            )
        )
        if last["clock_ms"] < PASS_MS or last["committed_ms"] < PASS_MS:
            fails.append(
                "%s %s: clock_ms=%d committed_ms=%d, want both >= %d -- the committed horizon "
                "never passed the initial %d ms advertisement (a slot is pinned SOMEWHERE, seen or "
                "not by peer0_ms/peer1_ms)"
                % (
                    role,
                    os.path.basename(sess_dir),
                    last["clock_ms"],
                    last["committed_ms"],
                    PASS_MS,
                    INITIAL_HORIZON_MS,
                )
            )
        # A side's OWN slot (host=peer0_ms, client=peer1_ms) is never written by that side at all
        # (commit_horizon skips self) -- it sits at the boot value forever even in a healthy match,
        # so only the REMOTE column is a real signal. See the module docstring, clause 4.
        remote_col = "peer1_ms" if role == "host" else "peer0_ms"
        if last[remote_col] == INITIAL_HORIZON_MS:
            fails.append(
                "%s %s: %s (the REMOTE peer's advertised horizon, as received) is still EXACTLY "
                "the initial %d ms advertisement at the last row (clock_ms=%d) -- the other side "
                "never advanced its own horizon"
                % (
                    role,
                    os.path.basename(sess_dir),
                    remote_col,
                    INITIAL_HORIZON_MS,
                    last["clock_ms"],
                )
            )
        # Clause 5 (2026-09-21): the match must also be IN SYNC. The first cut of the GS1(b) fix
        # unpinned the horizon but left retail's D18 diagonal write stamping the relation row into
        # the wrong Players[] slot -- a `[desync] *** DESYNC` from step 50 -- and the scenario's
        # captures were re-baselined WITH the red "DESYNC DETECTED" banner in the frame, which the
        # 2 % pixel tolerance then hid (the banner is 1.56 % of a 1024x768 frame). A verdict line
        # in the log cannot hide under a tolerance.
        d = desync_lines(sess_dir)
        if d:
            fails.append(
                "%s %s: %d [desync] verdict line(s) in mh_net.log -- first: %s"
                % (role, os.path.basename(sess_dir), len(d), d[0])
            )
    for f in fails:
        print("  [FAIL] %s" % f)
    if fails:
        print("check_ghost_slot: FAIL (%d)" % len(fails))
        return 1
    print(
        "check_ghost_slot: PASS -- committed horizon past %d ms on both peers, no printed slot "
        "pinned at %d" % (PASS_MS, INITIAL_HORIZON_MS)
    )
    return 0


# ---- selftest ---------------------------------------------------------------------------------

HEADER = "# " + " ".join(LOCKSTEP_COLS)


def _row(wall, clock, committed, peer0, peer1, step=100):
    # total_ms/local_h_ms/stall/pcount/tx/rx/since_rx are not read by this tool; fixed filler.
    return "%d %d %d %d %d %d %d %d 0 2 1 1 0" % (
        wall,
        clock,
        clock,
        committed,
        committed,
        peer0,
        peer1,
        step,
    )


def _rows_green():
    """A genuinely-played tail: clock/committed climb to 12000, both peer horizons lead them."""
    out = [HEADER]
    for t in range(0, 12001, 500):
        out.append(_row(t, t, min(t + 200, 12000), t + 10000, t + 10000))
    return out


def _rows_pinned():
    """The field shape: peer1 (a ghost) never leaves the initial 10000 ms advertisement, so
    committed = min(...) cannot pass it and clock freezes at 9999."""
    out = [HEADER]
    for t in range(0, 3001, 500):
        out.append(_row(t, t, min(t + 200, 9999), t + 10000, INITIAL_HORIZON_MS))
    for t in range(3500, 12001, 500):
        out.append(_row(t, 9999, 9999, t + 10000, INITIAL_HORIZON_MS))
    return out


def _rows_slot_stuck_but_committed_passed():
    """Edge case for clause 4: committed/clock legitimately pass PASS_MS (a slot that left is no
    longer counted toward the min), but its printed horizon column still literally reads the boot
    value it never got to update past -- clause 3 alone would pass this; clause 4 must not."""
    out = [HEADER]
    for t in range(0, 12001, 500):
        out.append(_row(t, t, min(t + 200, 12000), t + 10000, INITIAL_HORIZON_MS))
    return out


DESYNC_MARK = "*** DESYNC"


def desync_lines(sess_dir):
    """Every `[desync] *** DESYNC ...` verdict line in the session's mh_net.log (absent log = none:
    the lockstep rows are the vacuity guard, not this file)."""
    fp = os.path.join(sess_dir, "mh_net.log")
    if not os.path.isfile(fp):
        return []
    out = []
    with open(fp, "r", encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if "[desync]" in ln and DESYNC_MARK in ln:
                out.append(ln.strip()[:160])
    return out


def plant(root, role, rows, with_session=True, desync=False):
    lane = os.path.join(root, role)
    logs = os.path.join(lane, "logs")
    proc_leaf = "20260921T060000Z_menu_" + role
    proc = os.path.join(logs, proc_leaf)
    os.makedirs(proc)
    if with_session:
        sess = os.path.join(
            logs, "20260921T060010Z_a952c570_%d_%s" % (0 if role == "host" else 1, role)
        )
        os.makedirs(sess)
        with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
            json.dump({"match_id": "a952c570", "process_dir": proc_leaf}, fh)
        if rows is not None:
            with open(os.path.join(sess, "mh_lockstep.log"), "w", encoding="utf-8") as fh:
                fh.write("\n".join(rows) + "\n")
        with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
            fh.write("[00:00:00.000] ; [desync] first sample sent: step=50 state=4A39\n")
            if desync:
                fh.write(
                    "[00:00:01.000] ; [desync] *** DESYNC at step 50: first_region=55 players\n"
                )
    return proc


def selftest():
    cases = [
        # title, host rows (None = no log file, and with h_has_session=False no session at all),
        # client rows, host has a session dir, client has a session dir, want exit code.
        (
            "green: committed/clock pass 11000, both printed slots lead them",
            _rows_green(),
            _rows_green(),
            True,
            True,
            0,
        ),
        (
            "RED: peer1 pinned at 10000, committed/clock frozen at 9999",
            _rows_pinned(),
            _rows_pinned(),
            True,
            True,
            1,
        ),
        ("RED: host clean, client pinned", _rows_green(), _rows_pinned(), True, True, 1),
        (
            "RED: committed passed but a printed slot never left the boot value",
            _rows_slot_stuck_but_committed_passed(),
            _rows_slot_stuck_but_committed_passed(),
            True,
            True,
            1,
        ),
        (
            "RED: horizons clean but the match DESYNCED (the fix-v1 shape, banner hidden by tol)",
            _rows_green(),
            _rows_green(),
            True,
            True,
            1,
            True,
        ),
        (
            "REFUSED: no session directory (match never launched)",
            _rows_green(),
            _rows_green(),
            False,
            True,
            2,
        ),
        (
            "REFUSED: no mh_lockstep.log ([net] lockstep_log never armed)",
            None,
            _rows_green(),
            True,
            True,
            2,
        ),
        (
            "REFUSED: too few rows (vacuous run)",
            [HEADER, _row(0, 0, 200, 10000, 10000)],
            _rows_green(),
            True,
            True,
            2,
        ),
    ]
    bad = 0
    for case in cases:
        title, hrows, crows, h_has_session, c_has_session, want = case[:6]
        desync = case[6] if len(case) > 6 else False
        root = tempfile.mkdtemp(prefix="ghost_slot_selftest_")
        try:
            h = plant(root, "host", hrows, with_session=h_has_session, desync=desync)
            c = plant(root, "client", crows, with_session=c_has_session, desync=desync)
            try:
                got = check([h, c])
            except Refusal as e:
                print("  [REFUSED] %s" % e)
                got = 2
        finally:
            shutil.rmtree(root, ignore_errors=True)
        ok = got == want
        bad += 0 if ok else 1
        print("  [%s] %-66s want=%s got=%s" % ("ok" if ok else "BAD", title, want, got))
    print("selftest: %s" % ("PASS" if bad == 0 else "FAIL (%d)" % bad))
    return 0 if bad == 0 else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    dirs = [a for a in argv if not a.startswith("--")]
    try:
        return check(dirs)
    except Refusal as e:
        print("[REFUSED] %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
