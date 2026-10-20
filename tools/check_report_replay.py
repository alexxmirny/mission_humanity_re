#!/usr/bin/env python3
"""check_report_replay.py -- tooling:TL-REPORT-REPLAY-FIDELITY: a player report replays faithfully FROM THE REPORT ONLY.

The row rep_fidelity records a campaign session the way a player's mh.dll does (order + clock record, step-1 seed, scene dump, state
checkpoints every 256 steps). This checker takes that recording and replays it with tools/replay_match_segment.py over SEVERAL DIFFERENT
CARRIER SAVES -- the save the replay lane loads only to get the game into a session; the scene dump is meant to overwrite all of it -- and
requires every field checkpoint hash to be identical on every carrier. A carrier-dependent replay is RED: the report would then only
replay over the one save its recorder happened to have loaded.

  check_report_replay.py [--carriers NAME,NAME,...] [--dll PATH] <run-dir>
  check_report_replay.py --selftest

<run-dir> is the process (menu) run directory of the row; the recorded session is the first sibling folder at/after it that holds
mh_rec_seed. `--carriers` names saves under tools/uiscripts/saves/ ("" = the replay tool's default vehicle, the recording's own planet).
The first carrier is the row's own save (6-taalor-6-c), the others are other maps / planets.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SAVES = os.path.join(HERE, "uiscripts", "saves")
DEFAULT_CARRIERS = ["", "5-kofu-c", "7-abigan-5", "ayy30"]
IDENT = re.compile(r"IDENTICAL: state agrees at all (\d+) recorded hash checkpoint")


def session_dir(run_dir):
    run_dir = os.path.normpath(run_dir)
    logs = os.path.dirname(run_dir)
    names = sorted(n for n in os.listdir(logs) if os.path.isdir(os.path.join(logs, n)))
    for n in [n for n in names if n >= os.path.basename(run_dir)]:
        d = os.path.join(logs, n)
        if any(
            os.path.isfile(os.path.join(d, f)) for f in ("mh_rec_seed.bin", "mh_rec_seed.bin.gz")
        ):
            return d
    return None


def checkpoints(sess):
    p = os.path.join(sess, "mh_rec_hash.txt")
    if not os.path.isfile(p):
        return 0
    with open(p, encoding="utf-8", errors="replace") as fh:
        return sum(1 for ln in fh if ln.strip() and not ln.startswith(";"))


def verdict(text, want):
    """-> (ok, one-line why) for one replay's stdout."""
    m = IDENT.search(text)
    if m and int(m.group(1)) >= want:
        return True, "IDENTICAL at all %s checkpoint(s)" % m.group(1)
    for key in ("DIVERGED", "REFUSED"):
        for ln in text.splitlines():
            if key in ln:
                return False, ln.strip()[:240]
    if m:
        return False, "only %s of %d checkpoints were compared" % (m.group(1), want)
    return False, "no verdict line (the replay did not finish)"


def run(run_dir, carriers, dll=""):
    sess = session_dir(run_dir)
    if not sess:
        print(
            "check_report_replay: FAIL -- no recorded session (mh_rec_seed) at or after %s"
            % run_dir
        )
        return 1
    want = checkpoints(sess)
    if want < 2:
        print(
            "check_report_replay: FAIL -- %s holds %d state checkpoint(s); a replay of <2 judges nothing"
            % (sess, want)
        )
        return 1
    bad = 0
    print(
        "check_report_replay: %s, %d field checkpoint(s), carriers: %s"
        % (sess, want, ", ".join(c or "(default vehicle)" for c in carriers))
    )
    for c in carriers:
        # --raw-fixups: import the dump's trailer exactly as RECORDED. The replay tool otherwise repairs OLD reports (clean_scene_fixups);
        # this row judges the capture itself, so a capture that mistook data for pointers again must go red here
        argv = [
            sys.executable,
            os.path.join(HERE, "replay_match_segment.py"),
            sess,
            "--shipped",
            "--raw-fixups",
        ]
        if c:
            path = os.path.join(SAVES, c + ".sav")
            if not os.path.isfile(path):
                print("  %-14s FAIL -- no such carrier save %s" % (c, path))
                bad += 1
                continue
            argv += ["--carrier-save", path]
        if dll:
            argv += ["--dll", dll]
        p = subprocess.run(argv, cwd=REPO, capture_output=True, text=True, errors="replace")
        ok, why = verdict(p.stdout + p.stderr, want)
        print("  %-14s %s  %s" % (c or "(default)", "ok  " if ok else "FAIL", why))
        bad += 0 if ok else 1
    print(
        "check_report_replay: %s"
        % (
            "PASS -- the report replays identically on every carrier"
            if not bad
            else "FAIL -- %d carrier(s) differ from the field" % bad
        )
    )
    return 1 if bad else 0


def selftest():
    fails = []

    def ck(cond, what):
        if not cond:
            fails.append(what)

    ok, _ = verdict(
        "  IDENTICAL: state agrees at all 6 recorded hash checkpoint(s) (mh_rec_hash.txt)", 6
    )
    ck(ok, "an IDENTICAL line at the expected count passes")
    ok, _ = verdict("  IDENTICAL: state agrees at all 3 recorded hash checkpoint(s)", 6)
    ck(not ok, "fewer checkpoints than the field recorded fails")
    ok, why = verdict("  DIVERGED at hash checkpoint match step 256 (state)", 6)
    ck(not ok and "DIVERGED" in why, "a DIVERGED line fails and is quoted")
    ok, why = verdict("  REFUSED: only 1 common step(s)", 6)
    ck(not ok and "REFUSED" in why, "a REFUSED line fails and is quoted")
    ok, _ = verdict("", 6)
    ck(not ok, "no verdict fails")
    import tempfile

    with tempfile.TemporaryDirectory() as t:
        logs = os.path.join(t, "logs")
        menu = os.path.join(logs, "2026-01-01T00-00-00Z_menu_solo")
        sess = os.path.join(logs, "2026-01-01T00-00-02Z_abc_planet6_campaign")
        later = os.path.join(logs, "2026-01-01T00-01-00Z_menu_solo")
        for d in (menu, sess, later):
            os.makedirs(d)
        ck(session_dir(menu) is None, "no seed -> no session")
        with open(os.path.join(sess, "mh_rec_seed.bin.gz"), "wb") as fh:
            fh.write(b"x")
        with open(os.path.join(sess, "mh_rec_hash.txt"), "w") as fh:
            fh.write("; c\n1 AA\n256 BB\n; end\n")
        ck(session_dir(menu) == sess, "the session folder after the menu dir is found")
        ck(
            session_dir(later) is None,
            "a later process's menu dir does not borrow an earlier session",
        )
        ck(checkpoints(sess) == 2, "checkpoint lines are counted, comments are not")
    for f in fails:
        print("  FAIL: " + f)
    print("check_report_replay selftest: %s" % ("PASS" if not fails else "%d FAILED" % len(fails)))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    carriers, dll, rest = DEFAULT_CARRIERS, "", []
    i = 0
    while i < len(argv):
        if argv[i] == "--carriers":
            carriers = argv[i + 1].split(",")
            i += 2
        elif argv[i] == "--dll":
            dll = argv[i + 1]
            i += 2
        else:
            rest.append(argv[i])
            i += 1
    if len(rest) != 1:
        print(__doc__)
        return 2
    return run(rest[0], carriers, dll)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
