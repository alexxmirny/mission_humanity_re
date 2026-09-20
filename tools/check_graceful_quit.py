#!/usr/bin/env python3
"""check_graceful_quit.py -- mp:U19: the CLEAN in-game leave, judged from the two peers' logs.

WHAT THE PIXELS CANNOT SAY. The `graceful_quit` scenario (tools/test_ui.py) drives a client through
ESC -> Quit game -> Yes out of a running lockstep match and captures the survivor's end-of-match
frame. That frame looks the same whether the departure took 200 ms or a minute, and it looks the
same whether the survivor learned of it from a lockstep control frame or from the socket dying --
which are the two things U19 actually had to separate. Both live in the logs, so this is where the
claim is made.

THE FIVE CLAUSES, and each exists because a weaker gate would pass on a broken mechanism:

  1. THE QUITTER BROADCAST. Its log carries `; U17 graceful-leave: broadcast self-removal side=N`.
     Without it the knob was off, or the detour never armed, and everything downstream is vacuous.

  2. THE BROADCAST RODE A LIVE LINK (`--expect-graceful`). No `; U40 relink: re-initialising the
     transport` may appear between the session close and that broadcast. This is the regression
     guard for the defect U19 found: mp_session_close("quit") arms U40's relink latch, and
     llm_net_player_remove reaches the wire through MH_Seam_GameSend -> lazy_start, which CONSUMES
     that latch and re-enters MH_Net_InitEx -- so before the fix the announcement closed its own
     socket and handed the frame to a transport that was still handshaking. The frame never arrived,
     and the survivor dropped the peer by the B2 socket-close catch instead. Every other clause here
     was GREEN through that whole defect, which is why this one is written as an ordering assertion
     and not as "a removal was sent".

  3. THE SURVIVOR LEARNED IT FROM THE WIRE. Its log must NOT carry `; U17 fast-drop: transport-dead
     peer` for this match: that line IS the B2 catch, and a run that ends through it has proven
     graceful_drop works, not graceful_leave. Clause 2 and clause 3 are the same defect seen from
     each end, deliberately: one of them alone would have been satisfiable by the broken build.

  4. IT LANDED IN ORDER, AND FAST. Both peers' `SESSION_END ... final_clock_ms=` must agree to
     within `--max-drop-steps` lockstep steps (the step length is read from the survivor's own
     mh_lockstep.log, not assumed), and the survivor's SESSION_END must follow the quitter's
     broadcast within `--max-drop-wall-ms`. The CLOCK agreement is the real statement -- an
     in-order drop ends the match at the same sim clock on both peers -- and the wall bound is what
     separates the shipping behaviour from the opt-out it replaced: measured 2026-09-18, the same
     walk with `[net] graceful_leave=0` put 56.7 s between those two events, against 0.187 s with
     the knob on.

  5. IT DID NOT END THROUGH THE GARBLED-STREAM ARM (`--expect-no-network-error`, mp:U19e). The
     survivor's log may carry neither `; [rx] garbled:` nor an `on_gameover ENTER ... outcome=7`,
     and U19d's correction line must not fire. Until U19e those three were ALL present on a
     perfectly clean quit: the leader's re-broadcast of the drop (llm_net_send_lockstep_kick)
     builds into the same buffer the datagram being dispatched lives in, so it overwrote the head
     of the next frame from the departed peer, the parse loop read a payload byte as an outer tag,
     and the default arm opened the end-of-match dialog with outcome 7 (NETWORK_ERROR). The match
     ended for the right reason only by accident. This clause is what says it ends for the right
     reason on purpose. It is deliberately about the SURVIVOR's route, not about the dialog's
     words -- U19d's label rewrite stays as the safety net, and its line firing here is a FAILURE,
     because the thing it corrects should no longer happen.

ABSENCE IS A FAILURE (check_module_bind.py's rule, kept here): a lane that cannot be found, a log
that cannot be read, or a peer with no SESSION_END is a REFUSAL, never a vacuous pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_graceful_quit.py <host-run-dir> --expect-graceful \
      --expect-same-end-clock --max-drop-steps 2
  python tools/check_graceful_quit.py <host-run-dir> --arm off      the opt-out arm, for comparison
  python tools/check_graceful_quit.py --selftest                    planted logs; every negative RED

`<host-run-dir>` is what test_ui.py's `post_check` machinery appends -- the HOST lane's newest run
directory. The HOST is the survivor in this scenario and the CLIENT is the quitter, so this tool
derives the host LANE (run-dir -> logs/ -> lane/) and then the client's sibling lane by the
registry's own naming convention ("ui_<test>_host" -> "ui_<test>_c1"), exactly as
check_session_rollover.py does. Both are read locally.

A PEER'S LINES ARE SPREAD OVER TWO RUN DIRECTORIES and that is not incidental here. SES1 gives a
match its own folder and closes it at the quit, so `SESSION_END` is the last line of the SESSION
folder while `; U17 graceful-leave: broadcast self-removal` -- which happens a few statements later
inside the same seam -- lands in the MENU folder the peer has already switched back to. Reading one
folder would silently lose half the evidence, so every needle is searched across all of a lane's run
directories, ordered by name (= by UTC stamp).
"""

import argparse
import glob
import json
import os
import re
import shutil
import sys
import tempfile

# ---- the needles -------------------------------------------------------------------------------
# Registered in tools/data/log_formats.json (net.graceful_leave_broadcast, net.u40_relink,
# net.fast_drop, session.end). Keep the two copies textually identical: lint_log_formats arm A
# compares these literals against that registry.
BROADCAST = "; U17 graceful-leave: broadcast self-removal side="
RELINK = "; U40 relink: re-initialising the transport"
FAST_DROP = "; U17 fast-drop: transport-dead peer"
SESSION_END = "SESSION_END"  # the registry needle (net.session_end); no channel tag, on purpose
DISABLED = "; U17 graceful-leave DISABLED by config"
# U19e. Registered as net.rx_garbled, net.gameover_enter and net.u19d_outcome_correction.
GARBLED = "; [rx] garbled:"
GAMEOVER_ENTER = "on_gameover ENTER"
U19D_CORRECTION = "U19d: outcome-dialog said"
OUTCOME_RE = re.compile(re.escape(GAMEOVER_ENTER) + r"[^\n]*?outcome=(\d+)")
NETWORK_ERROR_OUTCOME = 7
# lockstep.columns: the per-frame CSV's own header token. Read POSITIONALLY below (step_ms is
# column 8, index 7) the way mp_analyze does, but the header is checked first so a column inserted
# ahead of step_ms turns into a refusal to measure rather than a bound computed from the wrong field.
LOCKSTEP_HEADER_FIRST_COL = "wall_ms"
LOCKSTEP_STEP_COL = 7

TS_RE = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]")
FINAL_CLOCK_RE = re.compile(r"final_clock_ms=(-?\d+)")
REASON_RE = re.compile(r"\breason=(\w+)")
SIDE_RE = re.compile(re.escape(BROADCAST) + r"(-?\d+)")

DEFAULT_MAX_DROP_STEPS = 2
DEFAULT_MAX_DROP_WALL_MS = 5000
# The fallback lockstep step used only when the survivor's mh_lockstep.log carries no usable row.
# It is the value ui_test.py's own [net] block writes, so a run that loses the CSV still gets a
# bound rather than an excuse -- but the fallback is REPORTED, never silent.
FALLBACK_STEP_MS = 30


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


def ts_ms(line):
    """Milliseconds-since-midnight from a seam-log timestamp, or None."""
    m = TS_RE.match(line)
    if not m:
        return None
    h, mi, s, ms = (int(x) for x in m.groups())
    return ((h * 60 + mi) * 60 + s) * 1000 + ms


def wall_delta_ms(a, b):
    """b - a, tolerating one midnight wrap (a run does not last a day)."""
    d = b - a
    return d + 24 * 3600 * 1000 if d < -12 * 3600 * 1000 else d


def lane_of(run_dir):
    """run-dir -> logs/ -> lane directory."""
    logs = os.path.dirname(os.path.abspath(run_dir))
    lane = os.path.dirname(logs)
    if os.path.basename(logs).lower() != "logs" or not os.path.isdir(lane):
        raise Refusal(
            "%s does not look like a lane run directory (expected <lane>/logs/<run>)" % run_dir
        )
    return lane


def sibling_client_lane(host_lane):
    """ "ui_<test>_host" -> "ui_<test>_c1", the registry's own local-lane naming (test_ui.lane_names)."""
    base = os.path.basename(host_lane)
    if not base.endswith("_host"):
        raise Refusal("host lane %r does not end in '_host'; cannot name its client sibling" % base)
    cand = os.path.join(os.path.dirname(host_lane), base[: -len("_host")] + "_c1")
    if not os.path.isdir(cand):
        raise Refusal("client lane %s not found beside the host lane" % cand)
    return cand


def lane_net_lines(lane):
    """Every mh_net.log line of a lane, run directories in UTC-stamp order.

    Returns [(run_dir_basename, line)]. Empty is a Refusal at the caller."""
    runs = sorted(d for d in glob.glob(os.path.join(lane, "logs", "*")) if os.path.isdir(d))
    out = []
    for run in runs:
        fp = os.path.join(run, "mh_net.log")
        if not os.path.isfile(fp):
            continue
        with open(fp, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                out.append((os.path.basename(run), line.rstrip("\n")))
    if not out:
        raise Refusal("no mh_net.log content under %s" % os.path.join(lane, "logs"))
    return out


def first_hit(lines, needle):
    """(index, line) of the first line containing `needle`, or (None, None)."""
    for i, (_run, line) in enumerate(lines):
        if needle in line:
            return i, line
    return None, None


def last_session_end(lines):
    """(index, line) of the LAST SESSION_END, or (None, None). The last one is the match's own: a
    lane that played more than one match keeps them all, and the one this scenario is about is the
    one it just finished."""
    hit = (None, None)
    for i, (_run, line) in enumerate(lines):
        if SESSION_END in line:
            hit = (i, line)
    return hit


def lockstep_step_ms(lane):
    """The lockstep step length in ms, from the survivor's own mh_lockstep.log (column 8 of the
    per-frame CSV, `step_ms`). Returns (value, source) -- source says 'measured' or 'fallback', and
    the caller PRINTS it, because a bound computed from an assumed step is a bound nobody can check."""
    for run in sorted(glob.glob(os.path.join(lane, "logs", "*")), reverse=True):
        fp = os.path.join(run, "mh_lockstep.log")
        if not os.path.isfile(fp):
            continue
        val, header_ok = None, False
        with open(fp, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("#"):
                    names = line.lstrip("#").split()
                    header_ok = (
                        bool(names)
                        and names[0] == LOCKSTEP_HEADER_FIRST_COL
                        and (
                            len(names) > LOCKSTEP_STEP_COL and names[LOCKSTEP_STEP_COL] == "step_ms"
                        )
                    )
                    continue
                if not header_ok:
                    continue
                parts = line.split()
                if len(parts) <= LOCKSTEP_STEP_COL:
                    continue
                try:
                    step = int(parts[LOCKSTEP_STEP_COL])
                except ValueError:
                    continue
                if step > 0:
                    val = step
        if val:
            return val, "measured (%s/mh_lockstep.log)" % os.path.basename(run)
    return FALLBACK_STEP_MS, "FALLBACK -- no usable mh_lockstep.log row"


def check(
    run_dir,
    arm,
    max_drop_steps,
    max_drop_wall_ms,
    expect_same_end_clock,
    expect_no_network_error=False,
):
    """(ok, [report lines]). Raises Refusal when the run cannot be judged at all."""
    host_lane = lane_of(run_dir)
    client_lane = sibling_client_lane(host_lane)
    survivor = lane_net_lines(host_lane)
    quitter = lane_net_lines(client_lane)
    step_ms, step_src = lockstep_step_ms(host_lane)

    out = []
    bad = []
    out.append("survivor lane: %s" % os.path.basename(host_lane))
    out.append("quitter  lane: %s" % os.path.basename(client_lane))
    out.append("lockstep step: %d ms -- %s" % (step_ms, step_src))

    # ---- clause 1: the quitter broadcast (or, in the off arm, did not) --------------------------
    b_i, b_line = first_hit(quitter, BROADCAST)
    if arm == "on":
        if b_i is None:
            bad.append(
                "the quitter never logged %r -- the knob was off, or the quit-to-menu detour did not arm"
                % BROADCAST
            )
        else:
            m = SIDE_RE.search(b_line)
            out.append("quitter broadcast: side=%s" % (m.group(1) if m else "?"))
    else:
        if b_i is not None:
            bad.append(
                "--arm off, but the quitter broadcast a self-removal anyway: %s" % b_line.strip()
            )
        d_i, _ = first_hit(quitter, DISABLED)
        out.append(
            "opt-out arm: graceful-leave %s"
            % ("declared disabled at arm time" if d_i is not None else "silent")
        )

    # ---- clause 2: the broadcast rode a LIVE link ----------------------------------------------
    if arm == "on" and b_i is not None:
        r_i, r_line = first_hit(quitter, RELINK)
        if r_i is not None and r_i < b_i:
            bad.append(
                "U40 relink ran BEFORE the self-removal broadcast (log lines %d < %d) -- the "
                "announcement tore its own link down and the frame went to a handshaking socket. "
                "This is the U19 defect; the fix is the relink-latch hold in on_quit_to_menu."
                % (r_i, b_i)
            )
        else:
            out.append(
                "broadcast ordering: no relink between the session close and the broadcast -- live link"
            )

    # ---- clause 3: the survivor learned it from the wire ---------------------------------------
    f_i, f_line = first_hit(survivor, FAST_DROP)
    if arm == "on" and f_i is not None:
        bad.append(
            "the survivor dropped the peer through the B2 socket-close catch (%s) -- that is "
            "graceful_drop, not graceful_leave, and it is what the broken build did"
            % f_line.strip()
        )
    elif arm == "on":
        out.append("survivor drop route: lockstep dispatch (no fast-drop line)")

    # ---- clause 4: same clock, and fast --------------------------------------------------------
    s_i, s_line = last_session_end(survivor)
    q_i, q_line = last_session_end(quitter)
    if s_i is None:
        raise Refusal("the survivor never wrote a SESSION_END -- its match did not end at all")
    if q_i is None:
        raise Refusal("the quitter never wrote a SESSION_END -- it never left the match")
    sc = FINAL_CLOCK_RE.search(s_line)
    qc = FINAL_CLOCK_RE.search(q_line)
    if not sc or not qc:
        raise Refusal(
            "a SESSION_END carries no final_clock_ms; cannot compare the peers' end clocks"
        )
    s_clock, q_clock = int(sc.group(1)), int(qc.group(1))
    s_reason = REASON_RE.search(s_line)
    q_reason = REASON_RE.search(q_line)
    out.append(
        "end clocks: survivor %d ms (reason=%s), quitter %d ms (reason=%s), delta %d ms"
        % (
            s_clock,
            s_reason.group(1) if s_reason else "?",
            q_clock,
            q_reason.group(1) if q_reason else "?",
            s_clock - q_clock,
        )
    )
    if expect_same_end_clock and arm == "on":
        budget = max_drop_steps * step_ms
        if abs(s_clock - q_clock) > budget:
            bad.append(
                "the peers ended %d ms of game clock apart, over the %d-step budget (%d ms) -- the "
                "removal was not applied at the same point in the lockstep stream"
                % (abs(s_clock - q_clock), max_drop_steps, budget)
            )

    if arm == "on" and b_i is not None:
        b_ts, s_ts = ts_ms(b_line), ts_ms(s_line)
        if b_ts is None or s_ts is None:
            out.append("wall gap: NOT MEASURABLE (a line carries no timestamp)")
        else:
            gap = wall_delta_ms(b_ts, s_ts)
            out.append("wall gap broadcast -> survivor SESSION_END: %d ms" % gap)
            if gap > max_drop_wall_ms:
                bad.append(
                    "the survivor took %d ms to end its match, over the %d ms budget -- that is the "
                    "shape of the retail silence timeout, not of an in-order drop"
                    % (gap, max_drop_wall_ms)
                )
            elif gap < 0:
                bad.append(
                    "the survivor's SESSION_END precedes the quitter's broadcast by %d ms" % -gap
                )

    # ---- clause 5: the survivor did not end through the garbled-stream arm (U19e) ---------------
    if expect_no_network_error and arm == "on":
        clause5 = []
        g_i, g_line = first_hit(survivor, GARBLED)
        if g_i is not None:
            clause5.append(
                "the survivor's dispatcher hit the unknown-outer-tag arm: %s -- a frame from the "
                "departed peer was overwritten by this peer's own drop re-broadcast before it was "
                "parsed (MP U19e; the fix is [net] gone_peer_frame_guard)" % g_line.strip()
            )
        bad_outcomes = [
            line
            for _run, line in survivor
            if GAMEOVER_ENTER in line
            for m in [OUTCOME_RE.search(line)]
            if m and int(m.group(1)) == NETWORK_ERROR_OUTCOME
        ]
        if bad_outcomes:
            clause5.append(
                "the survivor opened its end-of-match dialog with outcome %d (NETWORK_ERROR) on a "
                "CLEAN quit: %s" % (NETWORK_ERROR_OUTCOME, bad_outcomes[0].strip())
            )
        c_i, c_line = first_hit(survivor, U19D_CORRECTION)
        if c_i is not None:
            clause5.append(
                "U19d's label correction fired (%s) -- it is the safety net for a genuinely garbled "
                "stream, so on a clean quit it firing means the U19e framing defect is back"
                % c_line.strip()
            )
        bad.extend(clause5)
        if not clause5:
            out.append(
                "survivor end route: below-quorum presence loss (no garbled frame, no outcome %d, "
                "no U19d correction)" % NETWORK_ERROR_OUTCOME
            )

    for b in bad:
        out.append("FAIL: " + b)
    return (not bad), out


# ---- selftest ----------------------------------------------------------------------------------
# Each arm plants a pair of lanes and requires the stated verdict. The GREEN arm is first so a
# mistake in the fixture itself shows up as a green that should be red rather than the reverse.
GREEN_QUITTER = [
    "[01:59:45.684] ; U40: match over -> client link released; the next browser refresh re-dials the host",
    "[01:59:45.686] ; on_gameover ENTER sess=2 outcome=0 gclk=3259 (downgrade=0)",
    "[01:59:45.686] " + BROADCAST + "1 before quit-to-menu",
]
GREEN_QUITTER_SESSION = [
    "[01:59:45.684] ; [session] SESSION_END match_id=deadbeef reason=quit final_clock_ms=3259 stall=0",
]
GREEN_SURVIVOR = [
    "[01:59:45.731] ; GameRecv sender=1 len=14 type=0x04",
    "[01:59:45.871] ; [session] SESSION_END match_id=deadbeef reason=gameover final_clock_ms=3259 stall=0",
]
LOCKSTEP_CSV = [
    "# wall_ms clock_ms total_ms local_h_ms committed_ms peer0_ms peer1_ms step_ms stall pcount",
    "1732351218 3259 3260 3290 3290 3290 3290 30 0 0",
]


def _plant(root, test, quitter_menu, quitter_session, survivor_session, csv=LOCKSTEP_CSV):
    host_run = os.path.join(root, "ui_%s_host" % test, "logs", "20260918T000001Z_aaaaaaaa_0_solo")
    cli_menu = os.path.join(root, "ui_%s_c1" % test, "logs", "20260918T000000Z_menu_solo")
    cli_run = os.path.join(root, "ui_%s_c1" % test, "logs", "20260918T000001Z_aaaaaaaa_1_solo")
    for d in (host_run, cli_menu, cli_run):
        os.makedirs(d)
    open(os.path.join(host_run, "mh_net.log"), "w").write("\n".join(survivor_session) + "\n")
    open(os.path.join(host_run, "mh_lockstep.log"), "w").write("\n".join(csv) + "\n")
    open(os.path.join(cli_run, "mh_net.log"), "w").write("\n".join(quitter_session) + "\n")
    open(os.path.join(cli_menu, "mh_net.log"), "w").write("\n".join(quitter_menu) + "\n")
    return host_run


def selftest():
    arms = []

    def arm(name, quitter_menu, quitter_session, survivor_session, want_ok, kw=None):
        arms.append((name, quitter_menu, quitter_session, survivor_session, want_ok, kw or {}))

    arm(
        "green: in-order drop over a live link",
        GREEN_QUITTER,
        GREEN_QUITTER_SESSION,
        GREEN_SURVIVOR,
        True,
    )
    arm(
        "red: no broadcast at all (knob off / detour unarmed)",
        [ln for ln in GREEN_QUITTER if BROADCAST not in ln],
        GREEN_QUITTER_SESSION,
        GREEN_SURVIVOR,
        False,
    )
    arm(
        "red: the U19 defect -- relink ran before the broadcast",
        [GREEN_QUITTER[0], "[01:59:45.685] " + RELINK + " for a fresh dial to the host"]
        + GREEN_QUITTER[1:],
        GREEN_QUITTER_SESSION,
        GREEN_SURVIVOR,
        False,
    )
    arm(
        "red: the survivor dropped it by the B2 socket-close catch",
        GREEN_QUITTER,
        GREEN_QUITTER_SESSION,
        ["[01:59:45.455] " + FAST_DROP + " side=1 -> broadcast removal (PARKED after 3 frames)"]
        + GREEN_SURVIVOR,
        False,
    )
    arm(
        "red: the peers ended on different game clocks",
        GREEN_QUITTER,
        GREEN_QUITTER_SESSION,
        [
            "[01:59:45.871] ; [session] SESSION_END match_id=deadbeef reason=gameover "
            "final_clock_ms=3999 stall=0"
        ],
        False,
    )
    arm(
        "red: the retail silence timeout (56.7 s), which is the pre-U19 default",
        GREEN_QUITTER,
        GREEN_QUITTER_SESSION,
        [
            "[02:00:42.400] ; [session] SESSION_END match_id=deadbeef reason=gameover final_clock_ms=3259 stall=0"
        ],
        False,
    )
    # ---- U19e: the three shapes the garbled-stream arm leaves behind ---------------------------
    # The literals are the pre-fix rig measurement (2026-09-18, graceful_quit): the kick's own six
    # bytes followed by the last three of the 9-byte horizon it overwrote.
    arm(
        "red (U19e): the survivor's dispatcher hit the unknown-outer-tag arm",
        GREEN_QUITTER,
        GREEN_QUITTER_SESSION,
        [
            "[01:59:45.731] " + GARBLED + " sender=1 tag=0x70 at off=6 of len=9 "
            "head=04 0a 01 00 00 00 70 0b 40 "
        ]
        + GREEN_SURVIVOR,
        False,
    )
    arm(
        "red (U19e): the end-of-match dialog opened with outcome 7 on a clean quit",
        GREEN_QUITTER,
        GREEN_QUITTER_SESSION,
        ["[01:59:45.751] ; " + GAMEOVER_ENTER + " sess=3 outcome=7 gclk=3409 (downgrade=1)"]
        + GREEN_SURVIVOR,
        False,
    )
    arm(
        "red (U19e): U19d's safety-net correction fired, so the framing defect is back",
        GREEN_QUITTER,
        GREEN_QUITTER_SESSION,
        [
            "[01:59:45.752] ; " + U19D_CORRECTION + " 'Connection to server lost' with no real "
            "transport failure -- corrected to 'you are the last player'"
        ]
        + GREEN_SURVIVOR,
        False,
    )
    # ...and the shape that must stay GREEN: an ORDINARY gameover outcome is not a network error.
    arm(
        "green (U19e): a below-quorum outcome 8 dialog is the RIGHT end route",
        GREEN_QUITTER,
        GREEN_QUITTER_SESSION,
        ["[01:59:45.751] ; " + GAMEOVER_ENTER + " sess=3 outcome=8 gclk=3409 (downgrade=1)"]
        + GREEN_SURVIVOR,
        True,
    )
    arm(
        "refusal: the survivor never ended its session",
        GREEN_QUITTER,
        GREEN_QUITTER_SESSION,
        ["[01:59:45.731] ; GameRecv sender=1 len=14 type=0x04"],
        False,
    )

    failures = 0
    for name, qm, qs, ss, want_ok, kw in arms:
        root = tempfile.mkdtemp(prefix="gquit_selftest_")
        try:
            host_run = _plant(root, "graceful_quit", qm, qs, ss)
            try:
                ok, _report = check(
                    host_run,
                    kw.get("arm", "on"),
                    kw.get("max_drop_steps", DEFAULT_MAX_DROP_STEPS),
                    kw.get("max_drop_wall_ms", DEFAULT_MAX_DROP_WALL_MS),
                    True,
                    kw.get("expect_no_network_error", True),
                )
            except Refusal as exc:
                ok = False
                _report = ["REFUSAL: %s" % exc]
            verdict = "PASS" if ok == want_ok else "SELFTEST FAILURE"
            if ok != want_ok:
                failures += 1
            print("  [%s] %s (got ok=%s, wanted ok=%s)" % (verdict, name, ok, want_ok))
        finally:
            shutil.rmtree(root, ignore_errors=True)
    print(
        "check_graceful_quit --selftest: %s -- %d arm(s)"
        % ("FAIL" if failures else "PASS", len(arms))
    )
    return 1 if failures else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("run_dir", nargs="?", help="the HOST (survivor) lane's run directory")
    ap.add_argument(
        "--arm", choices=("on", "off"), default="on", help="which graceful_leave arm this run is"
    )
    ap.add_argument(
        "--expect-graceful",
        action="store_true",
        help="assert the quitter broadcast its own removal over a live link (the --arm on default)",
    )
    ap.add_argument(
        "--expect-same-end-clock",
        action="store_true",
        help="assert both peers ended on the same game clock, within --max-drop-steps",
    )
    ap.add_argument(
        "--expect-no-network-error",
        action="store_true",
        help="assert the survivor did not end through the garbled-stream arm (U19e): no "
        "'; [rx] garbled:', no on_gameover outcome=7, no U19d correction line",
    )
    ap.add_argument("--max-drop-steps", type=int, default=DEFAULT_MAX_DROP_STEPS)
    ap.add_argument("--max-drop-wall-ms", type=int, default=DEFAULT_MAX_DROP_WALL_MS)
    ap.add_argument(
        "--selftest", action="store_true", help="planted logs; every negative must go red"
    )
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.run_dir:
        ap.error("give a run directory or --selftest")
    try:
        ok, report = check(
            args.run_dir,
            args.arm,
            args.max_drop_steps,
            args.max_drop_wall_ms,
            args.expect_same_end_clock,
            args.expect_no_network_error,
        )
    except Refusal as exc:
        print("check_graceful_quit: REFUSED -- %s" % exc)
        return 2
    for line in report:
        print("  " + line)
    print("check_graceful_quit: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
