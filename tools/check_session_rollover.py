#!/usr/bin/env python3
"""check_session_rollover.py -- SES1b: THE THREE-MATCH ROLLOVER, PROVEN FROM THE SESSION RECORDS
THEMSELVES rather than from the pixels.

SES1 gave a match its own directory (mh_session_dir.h: "<UTC>Z_<mid8>_<slot>_<role>", opened at
lobby create / JOIN-sent, closed on leave|gameover|host_left|link_lost|timeout|quit) and PROVED the
three-match rollover once, by hand, from a scratchpad script (the mp tracker's SES1 progress
note: "3+3 directories with pairwise-identical ids"). This is that proof turned into a `post_check`
(tools/test_ui.py), so a regression in the rollover state machine (mh_session_state_begin/_end,
mh_session_dir.h) is caught by the suite instead of by the next player report -- SES1's own residue
that opened SES1b.

WHAT IT ASSERTS, read off SES1/SES1b's own acceptance criteria (the mp tracker's done_when): after N
rounds of Create/Join/Cancel (tools/uiscripts/mp_host_rollover.txt + mp_client_rollover.txt), the
HOST's lane and the CLIENT's lane each hold EXACTLY N session directories matched by shape, and
round i's match_id is IDENTICAL between host and client and DISTINCT from every other round's --
three matches, not one match captured three times.

ABSENCE IS A FAILURE (check_module_bind.py's rule, kept here): no lane, no session directories, a
session.json this tool cannot parse, or a count that is not exactly N is a REFUSAL or a FAIL, never
a vacuous pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_session_rollover.py <host-run-dir>              N defaults to 3
  python tools/check_session_rollover.py <host-run-dir> --rounds N
  python tools/check_session_rollover.py --selftest                  planted lanes; every negative RED

`<host-run-dir>` is what test_ui.py's `post_check` machinery hands every checker -- the HOST lane's
newest run directory (its own `sp_newest_run`). This tool derives the host LANE directory from it
(two levels up: run-dir -> logs/ -> lane/), then the CLIENT's sibling lane by the registry's own
naming convention (`lane_names()` in test_ui.py: "ui_<test>_host" / "ui_<test>_c1") -- the same
directory with "_host" replaced by "_c1". Both are read LOCALLY, so this only runs meaningfully in
the same --local / VM-pulled topology every other post_check in the registry already requires.

WHY A NEW STANDALONE CHECKER RATHER THAN A HELPER test_ui.py CALLS DIRECTLY: `post_check` (fork
F4A) is a subprocess contract on purpose -- a scenario names a command, it runs against that test's
lane after a green run, and a non-zero exit turns the test red with the command's own output
attached (test_ui.py's own comment above `def post_check(test)`). check_module_bind.py is the
existing example of the pattern; this is its session-directory sibling.
"""

import argparse
import glob
import json
import os
import re
import sys
import tempfile


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


# Textually identical to tools/mp_run.py's SESSION_DIR_RE (mh_session_dir.h's own shape:
# "<UTC YYYYMMDDTHHMMSSZ>_<8 hex>_<slot>_<role>"). Kept as a literal copy rather than an import so
# this checker carries no runtime dependency on mp_run.py's own (ssh/desktop-launch) import chain --
# the same reasoning check_module_bind.py's module docstring gives for reading logs directly instead
# of shelling out to a bigger tool.
SESSION_DIR_RE = re.compile(r"^\d{8}T\d{6}Z_[0-9a-f]{8}_\d+_[A-Za-z0-9]+$")

DEFAULT_ROUNDS = 3


def session_runs(logs_dir):
    """The SESSION directories under `logs_dir`, oldest first. Matched by SHAPE alone (not by a
    `*_host` / `*_client` glob, unlike mp_run.py's own session_runs) -- measured on a real rig run
    (2026-09-17): a `[uitest]`-driven peer (ui_test.py, the path every test_ui.py scenario takes)
    never sets a `--mp-host`/`--mp-join` cmdline verb or an `[net] role=` ini key, so
    run_context.cpp's detect_role() falls back to `role="solo"` for BOTH peers -- that fallback is
    only NOT hit by mp_run.py's own FORCE-ENTRY runs, which is the shape its session_runs(role) was
    written for. Peer separation here comes from the LANE directory (host vs client each have their
    own `logs/`), not from the role suffix, so shape-only matching is both correct and more robust:
    SESSION_DIR_RE already excludes the "<stamp>_menu_<role>" process directory (no 8-hex/slot
    segment), whatever the role string turns out to be."""
    c = [
        d
        for d in glob.glob(os.path.join(logs_dir, "*"))
        if os.path.isdir(d) and SESSION_DIR_RE.match(os.path.basename(d))
    ]
    return sorted(c, key=lambda d: os.path.basename(d))


def read_session_json(folder):
    """A session directory's session.json (mp_analyze.py's read_session_json), or None."""
    fp = os.path.join(folder, "session.json")
    if not os.path.isfile(fp):
        return None
    try:
        with open(fp, encoding="utf-8", errors="replace") as fh:
            d = json.load(fh)
    except (OSError, ValueError):
        return None
    return d if isinstance(d, dict) else None


def client_lane_dir(host_lane_dir):
    """The sibling CLIENT lane, by the registry's own naming convention (test_ui.py's
    lane_names(): "ui_<test>_host" + "ui_<test>_c1" for a two-peer scenario)."""
    base = os.path.basename(host_lane_dir.rstrip("\\/"))
    if not base.endswith("_host"):
        raise Refusal(
            "%r does not end in `_host` -- this checker only knows the host-lane -> client-lane "
            "naming convention (test_ui.py's lane_names()), and a lane it cannot name the sibling "
            "of is a lane it cannot cross-check for a pairwise-identical match_id" % base
        )
    parent = os.path.dirname(host_lane_dir.rstrip("\\/"))
    return os.path.join(parent, base[: -len("_host")] + "_c1")


def check(host_run_dir, rounds=DEFAULT_ROUNDS):
    if not os.path.isdir(host_run_dir):
        raise Refusal("no such run directory: %s" % host_run_dir)
    host_lane = os.path.dirname(os.path.dirname(host_run_dir.rstrip("\\/")))
    host_logs = os.path.join(host_lane, "logs")
    if not os.path.isdir(host_logs):
        raise Refusal("no logs/ under the host lane %s" % host_lane)
    client_lane = client_lane_dir(host_lane)
    client_logs = os.path.join(client_lane, "logs")
    if not os.path.isdir(client_logs):
        raise Refusal(
            "no logs/ under the sibling client lane %s -- this checker needs BOTH peers to assert "
            "pairwise-identical match_ids, and a host-only lane is not the multi-peer scenario "
            "this check is written for" % client_lane
        )

    host_sessions = session_runs(host_logs)
    client_sessions = session_runs(client_logs)
    print("check_session_rollover: host=%s (%d session dir(s))" % (host_lane, len(host_sessions)))
    print(
        "check_session_rollover: client=%s (%d session dir(s))"
        % (client_lane, len(client_sessions))
    )

    fails = []
    if len(host_sessions) != rounds:
        fails.append(
            "host lane has %d session directory(ies) matching the SES1 shape, want EXACTLY %d -- %s"
            % (
                len(host_sessions),
                rounds,
                ", ".join(os.path.basename(d) for d in host_sessions) or "(none)",
            )
        )
    if len(client_sessions) != rounds:
        fails.append(
            "client lane has %d session directory(ies) matching the SES1 shape, want EXACTLY %d -- "
            "%s"
            % (
                len(client_sessions),
                rounds,
                ", ".join(os.path.basename(d) for d in client_sessions) or "(none)",
            )
        )
    if fails:
        for f in fails:
            print("[FAIL] %s" % f)
        return 1

    seen_host_ids = set()
    for i in range(rounds):
        hd, cd = host_sessions[i], client_sessions[i]
        hj, cj = read_session_json(hd), read_session_json(cd)
        if hj is None:
            raise Refusal("%s has no readable session.json" % hd)
        if cj is None:
            raise Refusal("%s has no readable session.json" % cd)
        hid, cid = hj.get("match_id"), cj.get("match_id")
        if not hid or not cid:
            raise Refusal(
                "round %d: session.json is missing match_id (host=%r client=%r)" % (i + 1, hid, cid)
            )
        print(
            "  round %d  host=%-40s match_id=%s reason=%s"
            % (i + 1, os.path.basename(hd), hid, hj.get("reason") or "(open)")
        )
        print(
            "           client=%-38s match_id=%s reason=%s"
            % (os.path.basename(cd), cid, cj.get("reason") or "(open)")
        )
        # THE CLOSE HALF, not only the open half -- SES1's own claim is "closes on
        # leave|gameover|host_left|link_lost|timeout|quit", and mh_session_dir.h's mh_session_json
        # rewrites `reason`/`ended` whole at SESSION_END. An OPEN session (match_id already visible
        # at SESSION_BEGIN) would otherwise satisfy every check above this line even if the
        # close-on-leave seam call that writes SESSION_END never ran -- this is the assertion that
        # actually depends on it.
        if not hj.get("reason") or not hj.get("ended"):
            fails.append(
                "round %d: host session never closed (session.json reason=%r ended=%r) -- SES1's "
                "own claim is that a session closes on leave|gameover|host_left|link_lost|timeout|"
                "quit, and this one's SESSION_END never landed"
                % (i + 1, hj.get("reason"), hj.get("ended"))
            )
        if not cj.get("reason") or not cj.get("ended"):
            fails.append(
                "round %d: client session never closed (session.json reason=%r ended=%r)"
                % (i + 1, cj.get("reason"), cj.get("ended"))
            )
        if hid != cid:
            fails.append(
                "round %d: host match_id %s != client match_id %s -- the two peers did not pair "
                "into the same match this round" % (i + 1, hid, cid)
            )
        if hid in seen_host_ids:
            fails.append(
                "round %d: host match_id %s repeats an EARLIER round's -- this is one match "
                "captured %d times, not %d distinct matches" % (i + 1, hid, rounds, rounds)
            )
        seen_host_ids.add(hid)
        # The directory name's own handle (the trailing 8 hex of the match_id,
        # mh_session_dir_name's mid8) must agree with session.json's full id -- catches a
        # session.json copied into the wrong round's folder, which the count/distinctness checks
        # above cannot see on their own.
        short = hid[-8:] if len(hid) >= 8 else hid
        if short not in os.path.basename(hd):
            fails.append(
                "round %d: host directory %s does not carry match_id %s's short form (%s)"
                % (i + 1, os.path.basename(hd), hid, short)
            )
        if short not in os.path.basename(cd):
            fails.append(
                "round %d: client directory %s does not carry match_id %s's short form (%s)"
                % (i + 1, os.path.basename(cd), hid, short)
            )

    for f in fails:
        print("[FAIL] %s" % f)
    if fails:
        return 1
    print("check_session_rollover: PASS (%d rounds, pairwise-identical, all distinct)" % rounds)
    return 0


# ---- selftest --------------------------------------------------------------------------------
# Same shape as check_module_bind.py's: a check whose red has never been seen is not a check.


def _plant(d, host_rounds, client_rounds, host_lane_name="ui_selftest_rollover_host"):
    """Build a planted host+client lane pair.

    `host_rounds`/`client_rounds`: list of (dirname, match_id_or_None) or
    (dirname, match_id_or_None, closed) -- None for the match_id means "write no session.json at
    all" (the missing-record case); `closed` (default True) controls whether `reason`/`ended` are
    written (SESSION_END fields) -- False plants an OPEN session, the shape a missing close-on-leave
    call leaves behind. Returns the host lane's directory."""
    client_lane_name = host_lane_name[: -len("_host")] + "_c1"
    host_logs = os.path.join(d, host_lane_name, "logs")
    client_logs = os.path.join(d, client_lane_name, "logs")
    for logs_dir, rounds in ((host_logs, host_rounds), (client_logs, client_rounds)):
        os.makedirs(logs_dir, exist_ok=True)
        for entry in rounds:
            name, mid = entry[0], entry[1]
            closed = entry[2] if len(entry) > 2 else True
            p = os.path.join(logs_dir, name)
            os.makedirs(p, exist_ok=True)
            if mid is not None:
                rec = {"match_id": mid}
                if closed:
                    rec["reason"] = "leave"
                    rec["ended"] = "20260917T000099Z"
                with open(os.path.join(p, "session.json"), "w", encoding="utf-8") as fh:
                    json.dump(rec, fh)
    return os.path.join(d, host_lane_name)


def _round(i, role, short=None, closed=True):
    """A shape-matching (dirname, match_id, closed) triple for round `i` (0-based)."""
    short = short or ("aaaaaaa%d" % (i + 1))
    mid = ("0" * 24) + short
    slot = 0 if role == "host" else 1
    stamp = "20260917T%06dZ" % (i * 100)
    return "%s_%s_%d_%s" % (stamp, short, slot, role), mid, closed


def _good_rounds(role, n=3):
    return [_round(i, role) for i in range(n)]


def _run_case(name, host_rounds, client_rounds, rounds, must_pass, host_lane_name=None):
    with tempfile.TemporaryDirectory() as d:
        kwargs = {}
        if host_lane_name is not None:
            kwargs["host_lane_name"] = host_lane_name
        host_lane = _plant(d, host_rounds, client_rounds, **kwargs)
        # The real invocation is handed a RUN dir (a session dir or the menu dir), not the lane
        # itself -- so point at one of the planted session dirs if any exist, else the (missing)
        # logs dir, matching what sp_newest_run would return on an empty lane.
        target = (
            os.path.join(host_lane, "logs", host_rounds[-1][0])
            if host_rounds
            else os.path.join(host_lane, "logs")
        )
        if not os.path.isdir(target):
            os.makedirs(target, exist_ok=True)
        try:
            got = check(target, rounds) == 0
        except Refusal as e:
            print("check_session_rollover REFUSED: %s" % e)
            got = False
    verdict = "ok" if got == must_pass else "SELFTEST FAILED"
    print(
        "  [%s] %-58s wanted %s, got %s"
        % (verdict, name, "PASS" if must_pass else "RED", "PASS" if got else "RED")
    )
    return got == must_pass


def selftest():
    ok = True

    # 1. THE SHIPPED SHAPE: 3 rounds, pairwise-identical, all distinct.
    ok &= _run_case(
        "3 rounds, pairwise-identical, all distinct",
        _good_rounds("host"),
        _good_rounds("client"),
        3,
        True,
    )

    # 2. Host lane short a round (a round the scenario never reached, or a rollover that merged
    #    two rounds into one directory).
    ok &= _run_case(
        "host lane has only 2 session dirs",
        _good_rounds("host")[:2],
        _good_rounds("client"),
        3,
        False,
    )

    # 3. Client lane short a round.
    ok &= _run_case(
        "client lane has only 2 session dirs",
        _good_rounds("host"),
        _good_rounds("client")[:2],
        3,
        False,
    )

    # 4. Host has an EXTRA round nobody asked for (a lane that was not freshly provisioned, or a
    #    spurious rollover that opened a 4th session).
    ok &= _run_case(
        "host lane has 4 session dirs, want 3",
        _good_rounds("host", 4),
        _good_rounds("client"),
        3,
        False,
    )

    # 5. Round 2's match_id disagrees between host and client -- the two peers were not in the
    #    same match that round.
    mismatched_client = _good_rounds("client")
    name = mismatched_client[1][0]
    mismatched_client[1] = (name, ("0" * 24) + "deadbeef")
    ok &= _run_case(
        "round 2 match_id mismatched between host and client",
        _good_rounds("host"),
        mismatched_client,
        3,
        False,
    )

    # 6. THE VACUOUS CASE this checker exists to catch: one match captured three times (a rollover
    #    that never actually rolled over -- begin() re-asserting the SAME session, mh_session_dir.h's
    #    own "begin(X) while X is open -> nothing" branch never seeing the next match).
    same_id_host = [_round(i, "host", short="aaaaaaaa") for i in range(3)]
    same_id_client = [_round(i, "client", short="aaaaaaaa") for i in range(3)]
    ok &= _run_case(
        "all three rounds share ONE match_id (no real rollover)",
        same_id_host,
        same_id_client,
        3,
        False,
    )

    # 7. A session.json missing entirely for one round.
    missing_json_client = _good_rounds("client")
    name = missing_json_client[2][0]
    missing_json_client[2] = (name, None)
    ok &= _run_case(
        "round 3 client session.json missing",
        _good_rounds("host"),
        missing_json_client,
        3,
        False,
    )

    # 8. The directory name's short form disagrees with session.json's match_id (a copy-into-the-
    #    wrong-folder bug the count/pairwise checks alone cannot see).
    bad_dirname_host = _good_rounds("host")
    mid = bad_dirname_host[0][1]
    bad_dirname_host[0] = ("20260917T000000Z_ffffffff_0_host", mid)
    ok &= _run_case(
        "round 1 host directory name does not carry its own match_id's short form",
        bad_dirname_host,
        _good_rounds("client"),
        3,
        False,
    )

    # 8b. THE NEGATIVE ARM THIS CHECK EXISTS FOR: a round whose session.json shows an OPEN session
    # (no `reason`/`ended`) -- the shape a missing close-on-leave seam call leaves behind. match_id
    # is already readable at SESSION_BEGIN, so the count/pairwise/distinctness checks alone would
    # not catch this; the closed-ness assertion is what does.
    unclosed_host = _good_rounds("host")
    name, mid, _closed = unclosed_host[2]
    unclosed_host[2] = (name, mid, False)
    ok &= _run_case(
        "round 3 host session never closed (no reason/ended -- the close-on-leave arm)",
        unclosed_host,
        _good_rounds("client"),
        3,
        False,
    )

    # 9. No client lane at all (a solo/host-only lane, or the sibling was never provisioned).
    with tempfile.TemporaryDirectory() as d:
        host_lane = _plant(d, [], [], host_lane_name="ui_selftest_rollover_host")
        # Remove the client lane entirely.
        import shutil

        shutil.rmtree(os.path.join(d, "ui_selftest_rollover_c1"), ignore_errors=True)
        os.makedirs(os.path.join(host_lane, "logs", "dummy"), exist_ok=True)
        try:
            got = check(os.path.join(host_lane, "logs", "dummy"), 3) == 0
        except Refusal as e:
            print("check_session_rollover REFUSED: %s" % e)
            got = False
    print(
        "  [%s] %-58s wanted RED, got %s"
        % (
            "ok" if not got else "SELFTEST FAILED",
            "no client lane at all",
            "PASS" if got else "RED",
        )
    )
    ok &= not got

    # 10. No such run directory.
    try:
        got = (
            check(os.path.join(tempfile.gettempdir(), "does-not-exist-check-session-rollover"), 3)
            == 0
        )
    except Refusal as e:
        print("check_session_rollover REFUSED: %s" % e)
        got = False
    print(
        "  [%s] %-58s wanted RED, got %s"
        % (
            "ok" if not got else "SELFTEST FAILED",
            "no such run directory",
            "PASS" if got else "RED",
        )
    )
    ok &= not got

    # 11. A lane whose basename does not end in `_host` -- the naming convention this checker
    #     leans on does not hold, so it must REFUSE rather than guess a sibling.
    with tempfile.TemporaryDirectory() as d:
        odd_lane = os.path.join(d, "ui_selftest_rollover_solo", "logs", "dummy")
        os.makedirs(odd_lane, exist_ok=True)
        try:
            got = check(odd_lane, 3) == 0
        except Refusal as e:
            print("check_session_rollover REFUSED: %s" % e)
            got = False
    print(
        "  [%s] %-58s wanted RED, got %s"
        % (
            "ok" if not got else "SELFTEST FAILED",
            "lane basename does not end in _host",
            "PASS" if got else "RED",
        )
    )
    ok &= not got

    if ok:
        print("check_session_rollover --selftest: all cases ok")
        return 0
    print("check_session_rollover --selftest: one or more cases wrong")
    return 1


def main():
    ap = argparse.ArgumentParser(
        description="SES1b: three session directories per peer, pairwise-identical match_ids.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("run_dir", nargs="?", help="the HOST lane's newest run directory")
    ap.add_argument(
        "--rounds",
        type=int,
        default=DEFAULT_ROUNDS,
        help="expected session-directory count per peer",
    )
    ap.add_argument(
        "--selftest", action="store_true", help="planted lanes; every negative goes RED"
    )
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.run_dir:
        ap.error("run_dir is required unless --selftest")
    try:
        return check(args.run_dir, args.rounds)
    except Refusal as e:
        print("check_session_rollover REFUSED: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
