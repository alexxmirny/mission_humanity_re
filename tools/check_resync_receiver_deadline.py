#!/usr/bin/env python3
"""check_resync_receiver_deadline -- mp:P9W: a non-leader whose leader vanished mid-barrier really
left the mode-8 wait screen on its OWN deadline, instead of waiting forever for a RESUME that will
never arrive.

WHY A CHECKER AND NOT A PIXEL DIFF. The visible symptom (the client reaching the below-quorum
"Continue game" dialog) is already asserted by the scenario's own `[uitest]` script predicate --
that is the USER-VISIBLE half. What a capture cannot show is WHICH mechanism produced it: retail's
own graceful degradation (a normal-frame data_timeout_tick/graceful_drop catching the dead
transport BETWEEN barriers) reaches the exact same dialog as this fix does, and a pixel diff cannot
tell them apart. This reads the `; [resync] receiver_deadline: ... ms in barrier with no RESUME --
leaving the wait screen and removing side_id=..` line src/mh_dll/mh/seams/net_lockstep.cpp writes
(net.resync_receiver_deadline in tools/data/log_formats.json) -- the one line that proves THIS
mechanism specifically fired: the receiver was still `RESYNC_IN_PROGRESS` when its own
max(2002, data_timeout_ms) ms clock expired, with no RESUME landing first.

  python tools/check_resync_receiver_deadline.py <run dir>                  # fired at least once
  python tools/check_resync_receiver_deadline.py <run dir> --expect absent  # the reproduction arm: never fires
  python tools/check_resync_receiver_deadline.py <host dir> <client dir>    # pooled across peers

`<run dir>` is a session directory holding mh_net.log, or any directory above one (the newest
mh_net.log under it wins) -- the same content-first search check_lobby_ping.py / check_net_indicator.py
use, because tools/test_ui.py hands a post_check the "menu" session directory and this line is
match-time (belongs to whichever session actually ran the match).

WHAT "FIRED" MEANS: at least one `; [resync] receiver_deadline:` line, with elapsed_ms >= 2002 (the
leader's own floor -- a shorter wait would mean the deadline math is broken) and a non-negative
side_id (a real leader was identified and removed, not -1's "nobody").
"""

import argparse
import os
import re
import sys
import tempfile

DEADLINE_RE = re.compile(
    r"; \[resync\] receiver_deadline: (\d+) ms in barrier with no RESUME -- "
    r"leaving the wait screen and removing side_id=(-?\d+)"
)


def find_log(target):
    """Same content-first search as check_lobby_ping.find_log -- see that docstring. The receiver-
    deadline line is written to the SAME mh_net.log, from the SAME per-session log writer
    (net_lockstep.cpp), so a target that is itself a session directory widens to its siblings when
    its own log carries no `[resync] receiver_deadline:` line; otherwise the newest log anywhere
    under the target wins, so a run where the line really never fired still fails on its own merits
    rather than on a missing file."""
    if os.path.isfile(target):
        return target

    roots = [target]
    if os.path.isfile(os.path.join(target, "mh_net.log")):
        roots.append(os.path.dirname(os.path.abspath(target)))
    cands = []
    for d in roots:
        if not os.path.isdir(d):
            continue
        for root, _dirs, files in os.walk(d):
            if "mh_net.log" in files:
                cands.append(os.path.join(root, "mh_net.log"))
    if not cands:
        return None
    cands = sorted(set(cands), key=os.path.getmtime, reverse=True)
    for p in cands:
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as fh:
                if "[resync] receiver_deadline:" in fh.read():
                    return p
        except OSError:
            continue
    return cands[0]


NL = chr(10)
FIRED = (
    # mp:P9D's format (`live` beside `count was`); NEVER_FIRED below keeps the pre-P9D one, so the
    # selftest reads both.
    "[00:05:00.000] ; [resync] barrier #1 BEGIN (count was 0, live 0, threshold 0, countdown 59) flags=0x00\n"
    "[00:05:02.010] ; [resync] receiver_deadline: 2002 ms in barrier with no RESUME -- leaving the "
    "wait screen and removing side_id=0\n"
)
NEVER_FIRED = (
    "[00:05:00.000] ; [resync] barrier #1 BEGIN (count was 0, threshold 0, countdown 59) flags=0x00\n"
    "[00:05:02.010] ; [resync] barrier #1 END after 2010 ms flags=0x00\n"
)


def selftest():
    import subprocess

    cases = [
        ("fired, default --expect", FIRED, [], 0),
        ("no lines at all, default --expect", "", [], 1),
        ("barrier resolved normally (RESUME), default --expect", NEVER_FIRED, [], 1),
        ("fired, --expect absent (wrong)", FIRED, ["--expect", "absent"], 1),
        (
            "never fired, --expect absent (right -- the reproduction arm)",
            NEVER_FIRED,
            ["--expect", "absent"],
            0,
        ),
        ("no lines at all, --expect absent (right)", "", ["--expect", "absent"], 0),
    ]
    multi_cases = [
        ("multi: one peer fired", [NEVER_FIRED, FIRED], [], 0),
        ("multi: neither peer fired", [NEVER_FIRED, NEVER_FIRED], [], 1),
    ]
    fails = []
    # ONE ISOLATED TemporaryDirectory PER CASE -- same reasoning as check_lobby_ping.selftest: find_log
    # widens a session-directory target to ITS SIBLINGS, so two cases sharing a parent could answer
    # each other's question.
    for name, text, argv, want in cases:
        with tempfile.TemporaryDirectory() as tmp:
            d = os.path.join(tmp, "run")
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
                fh.write(text)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__), d] + argv,
                capture_output=True,
                text=True,
            )
            got = r.returncode
            print("  %-52s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d%s%s" % (name, got, want, NL, r.stdout))
    for name, texts, argv, want in multi_cases:
        with tempfile.TemporaryDirectory() as tmp:
            files = []
            for i, text in enumerate(texts):
                p = os.path.join(tmp, "peer%d.log" % i)
                with open(p, "w", encoding="utf-8") as fh:
                    fh.write(text)
                files.append(p)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__)] + files + argv,
                capture_output=True,
                text=True,
            )
            got = r.returncode
            print("  %-52s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d%s%s" % (name, got, want, NL, r.stdout))
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print(
        "check_resync_receiver_deadline --selftest: %s"
        % ("PASS" if not fails else "FAIL (%d)" % len(fails))
    )
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "targets",
        nargs="+",
        help="one or more session run dirs (or lane dirs above one), or mh_net.log files",
    )
    ap.add_argument(
        "--expect",
        choices=["fired", "absent"],
        default="fired",
        help="'fired' (default): the deadline caught a leaderless barrier. 'absent': the reproduction "
        "arm -- the knob is off (or no leaderless barrier occurred), so the line must never appear.",
    )
    a = ap.parse_args()

    logs = []
    for t in a.targets:
        p = find_log(t)
        if p:
            logs.append(p)
    if not logs:
        print("REFUSED: no mh_net.log at or under %s" % ", ".join(a.targets))
        return 2
    text = ""
    for p in logs:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            text += f.read()
        if text and not text.endswith("\n"):
            text += "\n"
    log = ", ".join(logs)

    samples = DEADLINE_RE.findall(text)
    print("read %s: %d receiver_deadline line(s)" % (log, len(samples)))

    fails = []
    if a.expect == "absent":
        if samples:
            fails.append(
                "--expect absent: %d receiver_deadline line(s) present, e.g. elapsed=%s side_id=%s"
                % (len(samples), samples[0][0], samples[0][1])
            )
    else:
        if not samples:
            fails.append(
                "no `; [resync] receiver_deadline:` line at all -- either no leaderless barrier "
                "occurred, or the fix never caught it"
            )
        else:
            bad = [s for s in samples if int(s[0]) < 2002]
            if bad:
                fails.append(
                    "%d/%d line(s) fired UNDER the 2002 ms floor (e.g. elapsed=%s) -- the deadline "
                    "math is broken" % (len(bad), len(samples), bad[0][0])
                )
            nobody = [s for s in samples if int(s[1]) < 0]
            if nobody:
                fails.append(
                    "%d/%d line(s) removed side_id=-1 (\"nobody\") -- the leader could not be "
                    "identified" % (len(nobody), len(samples))
                )

    for f in fails:
        print("FAIL: %s" % f)
    print(
        "check_resync_receiver_deadline: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails))
    )
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
