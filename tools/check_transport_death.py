#!/usr/bin/env python3
"""check_transport_death.py -- mp:U19f: an IN-GAME transport death, judged from both peers' logs.

WHAT WAS UNTESTED. `link_death` (tools/test_ui.py) kills the link in the LOBBY (`peers <1`, never
past Start); `graceful_quit`/U19d are a DELIBERATE quit. Nothing had ever killed a peer's TRANSPORT
while a match was RUNNING. `txdeath_ingame` does: it cuts the link (real FIN close, `tools/net_shim.py`
`cut`) well after both peers are in-game; this is that scenario's post_check.

G270 CORRECTED THIS CHECKER'S PREMISE (2026-09-22, real rig evidence). The finding is inlined below (the public cut carries no failure ledger). The
scenario was originally written expecting a real transport death to reach `on_gameover` with
outcome=7 (NETWORK_ERROR, "Connection to server lost"), on the theory that U19d's discriminator
(which rewrites outcome 7 to "you are the last player" ONLY when no real fast-drop landed recently)
needed its negative arm exercised. MEASURED: outcome 7 is produced by exactly ONE call site in the
whole closure -- `handle_garbled` (libmh/lockstep/rx_dispatch.cpp), the unknown-outer-tag arm, which
fires only on a genuinely garbled/misframed datagram. A clean below-quorum departure -- whether a
graceful quit (U17a) or a real transport death (U17b/fast-drop) -- NEVER reaches that arm: both are
retired through `llm_strat_player_presence_lost`, which calls the outcome dialog with outcome=8
("you are the last player !") directly, unconditionally, regardless of how the peer left. U19e's
`gone_peer_frame_guard` (default ON) fixed the ONE known way a clean departure used to misroute into
outcome 7 (a stale-buffer race in the timeout-drop re-broadcast) -- so in the guarded build outcome 7
is not reachable from a normal departure at all, real transport death included. So a real transport
death and a graceful quit are, correctly, INDISTINGUISHABLE by wording: both show "you are the last
player !" -- there is no live case left where "Connection to server lost" is the CORRECT thing to
show for a departure. The scenario's real, provable claim is narrower but still real: a transport
death is retired through the SAME fast in-order broadcast (fast-drop) a graceful quit uses, not
through the framing-bug arm and not through the old below-quorum silence-timeout stall.

THE THREE CLAUSES, and each exists because a weaker gate would pass on the wrong route:

  1. THE DROP WAS SEEN AS A TRANSPORT DEATH, RETIRED VIA THE FAST-DROP ROUTE. The FIRST directory
     given (the host -- see the FAST_DROP asymmetry note below) must carry
     `; U17 fast-drop: transport-dead peer` (the B2 socket-dead catch).

  2. EVERY PEER'S on_gameover REACHED outcome=8 (the normal below-quorum "last player" outcome), and
     NEVER outcome=7 (NETWORK_ERROR), and NEVER `; [rx] garbled:`. A run that reached the dialog
     through the unknown-outer-tag arm (or any other route) proves nothing about a genuine transport
     death, and outcome 7 appearing at all means the `gone_peer_frame_guard` fix (G270) regressed.

  3. NO SPURIOUS CORRECTION, on any peer. U19d's correction line (`; U19d: outcome-dialog said
     'Connection to server lost' ...`) must NOT appear -- it is gated on outcome==7, so with clause 2
     holding it cannot fire; asserting it anyway catches the correction itself misfiring on an
     outcome it should not be touching.

FAST_DROP IS HOST-ONLY BY CONSTRUCTION (measured on the rig, G270): `MH_Net_TakeDeadPeer()` excludes
a client's own host-connection death (`dead == -1`, net_lockstep.cpp's own comment: "Host-only by
construction (a client's host-conn latches -1)"), so the fast-drop broadcast -- and its log line --
is emitted ONLY by the peer acting as host. `txdeath_ingame` is a 2-peer match whose shim `cut` closes
BOTH legs of the proxied connection (net_shim.py's `cut()`), so the CLIENT independently reaches the
SAME outcome=8 dialog via its own local below-quorum detection, but never logs FAST_DROP itself --
that is not a weaker proof on the client's side, it is simply which peer the log line belongs to. So
clause 1 (FAST_DROP) is checked ONLY on the first directory given (the host); clauses 2-3 (the
outcome/wording sanity) are checked on EVERY directory given -- both peers independently prove they
reached the correct dialog, and only the host additionally proves it took the fast route to get
there. Call with the host directory FIRST: `check_transport_death.py <host-dir> <client-dir>...`.

ABSENCE IS A FAILURE (check_module_bind.py's rule, kept here): a directory that cannot be read or
carries no SESSION_END is a REFUSAL, never a vacuous pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_transport_death.py <host-run-dir> <client-run-dir>
  python tools/check_transport_death.py --selftest             planted logs; every negative RED
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
# Registered in tools/data/log_formats.json (net.fast_drop, net.gameover_enter,
# net.u19d_outcome_correction, net.rx_garbled). Keep these literals textually identical to the
# registry: lint_log_formats arm A compares them.
FAST_DROP = "; U17 fast-drop: transport-dead peer"
GAMEOVER_ENTER = "on_gameover ENTER"
OUTCOME_RE = re.compile(re.escape(GAMEOVER_ENTER) + r"[^\n]*?outcome=(\d+)")
LAST_PLAYER_OUTCOME = 8  # the normal below-quorum "you are the last player !" outcome (G270)
NETWORK_ERROR_OUTCOME = 7  # dead in the guarded build (G270) -- must NEVER appear here
U19D_CORRECTION = "U19d: outcome-dialog said"
GARBLED = "; [rx] garbled:"
SESSION_END = "SESSION_END"

SESSION_DIR_RE = re.compile(r"^\d{8}T\d{6}Z_[0-9a-f]{8}_\d+_[A-Za-z0-9]+$")


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


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
    """All mh_net.log lines of one peer: the process directory's, then each of its SESSION
    directories' (check_data_timeout.py's / check_cheat_gate.py's pattern) -- needed because
    `txdeath_ingame` is a lane SHARER (share_lanes: match_launch), so a peer's `logs/` can hold more
    than this process's own session. Refuses when nothing is readable."""
    run_dir = os.path.abspath(run_dir)
    if not os.path.isdir(run_dir):
        raise Refusal("%s is not a directory" % run_dir)
    logs_dir = os.path.dirname(run_dir)
    leaf = os.path.basename(run_dir)
    out = []
    proc = read_lines(run_dir)
    if proc is not None:
        out.extend(proc)
    for s in session_runs(logs_dir, leaf):
        ln = read_lines(s)
        if ln is not None:
            out.extend(ln)
    if not out:
        raise Refusal("%s: no mh_net.log in the process directory or its sessions" % run_dir)
    return out


def check_one(run_dir, require_fastdrop):
    """(ok, [report lines]) for a single peer directory. Raises Refusal when unjudgeable.

    `require_fastdrop`: only the HOST directory should be called with this True -- see the module
    docstring's FAST_DROP-IS-HOST-ONLY note. A client whose own uplink died reaches the same
    outcome=8 dialog without ever logging FAST_DROP itself; that is not a weaker proof, it is which
    peer the line structurally belongs to (net_lockstep.cpp: MH_Net_TakeDeadPeer() excludes a
    client's own host-conn death by returning -1 for it)."""
    lines = peer_lines(run_dir)
    out, bad = [], []

    if SESSION_END not in "\n".join(lines):
        raise Refusal("%s: no SESSION_END -- this peer's match never ended" % run_dir)

    fd = [ln for ln in lines if FAST_DROP in ln]
    if require_fastdrop and not fd:
        bad.append(
            "no %r line -- the host never saw the fast-drop route; the run may have ended some "
            "other way (a quit, a match that legitimately finished)" % FAST_DROP
        )
    elif fd:
        out.append("fast-drop: %s" % fd[0].strip())
    else:
        out.append("no fast-drop line (expected -- this is a client, not the host; G270)")

    outcomes = [int(m.group(1)) for ln in lines for m in [OUTCOME_RE.search(ln)] if m]
    if not outcomes:
        bad.append("no %r line at all" % GAMEOVER_ENTER)
    elif LAST_PLAYER_OUTCOME not in outcomes:
        bad.append(
            "on_gameover fired with outcome(s) %s, never %d (the normal below-quorum outcome) -- "
            "this run did not end through a transport death" % (outcomes, LAST_PLAYER_OUTCOME)
        )
    elif NETWORK_ERROR_OUTCOME in outcomes:
        bad.append(
            "on_gameover fired with outcome %d (NETWORK_ERROR) -- per G270 this is dead code for a "
            "clean departure in the guarded build; seeing it means the gone_peer_frame_guard fix "
            "regressed" % NETWORK_ERROR_OUTCOME
        )
    else:
        out.append("outcome: %d (last player) seen, no outcome 7" % LAST_PLAYER_OUTCOME)

    g = [ln for ln in lines if GARBLED in ln]
    if g:
        bad.append(
            "the dispatcher hit the unknown-outer-tag arm (%s) -- a real transport death must "
            "never route through mp:U19e's garbled-frame arm" % g[0].strip()
        )

    c = [ln for ln in lines if U19D_CORRECTION in ln]
    if c:
        bad.append(
            "U19d's correction fired (%s) -- per G270 it is gated on outcome==7, which this run "
            "must never reach; seeing it fire anyway is the correction itself misbehaving"
            % c[0].strip()
        )
    else:
        out.append("no spurious U19d correction")

    return (not bad), out, bad


def check(run_dirs):
    """(ok, [report lines]). Every directory given is ANDed -- see the module docstring. The FIRST
    directory is treated as the host (FAST_DROP required there only); every directory is checked for
    the outcome/wording sanity."""
    if not run_dirs:
        raise Refusal("no run directories given")
    out = []
    all_ok = True
    for i, d in enumerate(run_dirs):
        ok, lines, bad = check_one(d, require_fastdrop=(i == 0))
        out.append(
            "peer: %s%s" % (os.path.basename(os.path.abspath(d)), " (host)" if i == 0 else "")
        )
        out.extend("  " + ln for ln in lines)
        out.extend("  FAIL: " + b for b in bad)
        all_ok = all_ok and ok
    return all_ok, out


# ---- selftest ----------------------------------------------------------------------------------
GREEN = [
    "[02:10:00.100] ; GameRecv sender=1 len=14 type=0x04",
    "[02:10:10.442] " + FAST_DROP + " side=1 -> broadcast removal",
    "[02:10:10.443] ; " + GAMEOVER_ENTER + " sess=2 outcome=8 gclk=13020 (downgrade=0)",
    "[02:10:10.500] ; [session] SESSION_END match_id=deadbeef reason=gameover final_clock_ms=13020 stall=0",
]
# G270: a CLIENT's own log never carries FAST_DROP (host-only by construction) -- this is what a
# real client's mh_net.log looks like for the SAME correctly-handled transport death.
GREEN_CLIENT = [ln for ln in GREEN if FAST_DROP not in ln]


def _plant(root, name, lines):
    run = os.path.join(root, name)
    os.makedirs(run)
    open(os.path.join(run, "mh_net.log"), "w").write("\n".join(lines) + "\n")
    return run


def selftest():
    arms = []

    def arm(name, lines, want_ok):
        arms.append((name, lines, want_ok))

    arm("green: fast-drop + outcome 8, no outcome 7, no correction", GREEN, True)
    arm(
        "red: no fast-drop line at all",
        [ln for ln in GREEN if FAST_DROP not in ln],
        False,
    )
    arm(
        "red: on_gameover never fired",
        [ln for ln in GREEN if GAMEOVER_ENTER not in ln],
        False,
    )
    arm(
        "red: on_gameover fired but not with outcome 8 (e.g. an ordinary game-over path)",
        [ln.replace("outcome=8", "outcome=4") if GAMEOVER_ENTER in ln else ln for ln in GREEN],
        False,
    )
    arm(
        "red (G270): on_gameover reached outcome 7 (NETWORK_ERROR) at all -- dead in the guarded build",
        [ln.replace("outcome=8", "outcome=7") if GAMEOVER_ENTER in ln else ln for ln in GREEN],
        False,
    )
    arm(
        "red (U19e): the survivor hit the garbled-stream arm instead of the fast-drop route",
        GREEN
        + [
            "[02:10:10.300] "
            + GARBLED
            + " sender=1 tag=0x70 at off=6 of len=9 head=04 0a 01 00 00 00 70 0b 40"
        ],
        False,
    )
    arm(
        "red (G270): U19d's correction fired even though outcome never reached 7",
        GREEN
        + [
            "[02:10:10.451] ; " + U19D_CORRECTION + " 'Connection to server lost' with no real "
            "transport failure -- corrected to 'you are the last player'"
        ],
        False,
    )
    arm("refusal: no SESSION_END at all", [ln for ln in GREEN if SESSION_END not in ln], False)

    failures = 0
    for name, lines, want_ok in arms:
        root = tempfile.mkdtemp(prefix="txdeath_selftest_")
        try:
            run = _plant(root, "peer0", lines)
            try:
                ok, _report = check([run])
            except Refusal as exc:
                ok = False
                _report = ["REFUSAL: %s" % exc]
            verdict = "PASS" if ok == want_ok else "SELFTEST FAILURE"
            if ok != want_ok:
                failures += 1
            print("  [%s] %s (got ok=%s, wanted ok=%s)" % (verdict, name, ok, want_ok))
        finally:
            shutil.rmtree(root, ignore_errors=True)

    # The 2-peer AND: a green host plus a genuinely-bad client must fail the whole call.
    root = tempfile.mkdtemp(prefix="txdeath_selftest_and_")
    try:
        good = _plant(root, "peer0", GREEN)
        bad = _plant(
            root,
            "peer1",
            [
                ln.replace("outcome=8", "outcome=7") if GAMEOVER_ENTER in ln else ln
                for ln in GREEN_CLIENT
            ],
        )
        ok, _report = check([good, bad])
        want = False
        verdict = "PASS" if ok == want else "SELFTEST FAILURE"
        if ok != want:
            failures += 1
        print(
            "  [%s] host green, client hit outcome 7 -> the pair FAILS (got ok=%s)" % (verdict, ok)
        )
    finally:
        shutil.rmtree(root, ignore_errors=True)

    # G270: host + client is the REAL shape -- a green host plus a green CLIENT (no fast-drop line,
    # which is expected and correct for a client) must PASS as a pair.
    root = tempfile.mkdtemp(prefix="txdeath_selftest_hostclient_")
    try:
        host = _plant(root, "peer0", GREEN)
        client = _plant(root, "peer1", GREEN_CLIENT)
        ok, _report = check([host, client])
        want = True
        verdict = "PASS" if ok == want else "SELFTEST FAILURE"
        if ok != want:
            failures += 1
        print(
            "  [%s] host + client (client has no fast-drop line) -> the pair PASSES (got ok=%s)"
            % (verdict, ok)
        )
    finally:
        shutil.rmtree(root, ignore_errors=True)

    print(
        "check_transport_death --selftest: %s -- %d arm(s)"
        % ("FAIL" if failures else "PASS", len(arms) + 2)
    )
    return 1 if failures else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("run_dirs", nargs="*", help="one or more peers' run directories")
    ap.add_argument(
        "--selftest", action="store_true", help="planted logs; every negative must go red"
    )
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.run_dirs:
        ap.error("give at least one run directory or --selftest")
    try:
        ok, report = check(args.run_dirs)
    except Refusal as exc:
        print("check_transport_death: REFUSED -- %s" % exc)
        return 2
    for line in report:
        print("  " + line)
    print("check_transport_death: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
