#!/usr/bin/env python3
"""check_net_indicator -- mp:L1: the player-visible connection indicator really ran, and its numbers
are real.

WHY A CHECKER AND NOT A PIXEL DIFF. The indicator's whole content is live measurement: the ping
digits differ on every run, and the command-latency number moves with the adaptive lookahead. A
capture baseline can prove the element is ON THE FRAME in the right place with the right labels --
and it does, with the number cells masked -- but it cannot prove the digits mean anything, because
the only way to keep a capture stable is to stop comparing exactly the cells that carry the claim.
So the frame proves the DRAWING and this proves the NUMBERS, off the `; [netind]` lines the seam
writes into mh_net.log (src/mh_dll/mh/seams/ui_net_indicator.cpp).

It is a `post_check` in the UI suite's registry, and it is also the tool to run by hand after a
net_shim run:

  python tools/check_net_indicator.py <run dir>                       # it drew, and the numbers are sane
  python tools/check_net_indicator.py <run dir> --srtt 150 250        # ... and the ping sat in a band
  python tools/check_net_indicator.py <run dir> --min-cmd 120         # ... and command latency grew
  python tools/check_net_indicator.py <run dir> --expect-stall        # ... and a stall NAMED a peer
  python tools/check_net_indicator.py <host dir> <client dir> --reject-placeholder-name
                                                                       # ... and every peer resolved a real name

`<run dir>` is a session directory holding mh_net.log, or any directory above one (the newest
mh_net.log under it wins), which is what lets the suite hand over a lane folder unchanged. ONE OR
MORE may be given (mp:L1c) -- net_hud's `post_check_peers` hands over every peer's directory, and
their samples are pooled before any check runs, which is what lets a client-only bug (`name=PLAYER 0`
when only the host's own log was ever read) actually fail the run instead of hiding behind whichever
peer's log the registry happened to name.

WHAT "SANE" MEANS, stated so a pass is falsifiable rather than a shrug:
  * at least one `; [netind] shown` line -- the indicator reached a DRAWN frame, not merely an ini;
  * at least one `; [netind] peer..` sample whose srtt_ms is >= 0 (a measured link, not `n/a`) and
    whose bar level is in 1..4;
  * cmd_ms == look_ms + step_ms on every sample, with both terms > 0. That identity is the display's
    own definition of command latency, so a sample that breaks it is a seam that has stopped
    computing what it says it computes -- the one arithmetic claim a log line can carry.
"""

import argparse
import os
import re
import sys
import tempfile

SHOWN_RE = re.compile(
    r"; \[netind\] shown at (-?\d+),(-?\d+) font=(\d+) peers=(\d+) lat_supported=(\d+)"
)
SAMPLE_RE = re.compile(
    r"; \[netind\] peer(\d+) name=(.*?) srtt_ms=(-?\d+) ipdv_ms=(-?\d+) loss_pm=(-?\d+) "
    r"bar=(\d+) cmd_ms=(\d+) look_ms=(\d+) step_ms=(\d+)"
)
STALL_RE = re.compile(r"; \[netind\] stall waiting_for=(.*?) peer=(-?\d+) ms=(\d+)")


def find_log(target):
    """The mh_net.log this checker's subject is actually IN.

    THE OBVIOUS IMPLEMENTATION IS WRONG HERE, and it went red on its first real run. mh_net.log is
    per SESSION (SES1), and tools/test_ui.py hands a post_check the PROCESS ("menu") session
    directory on purpose -- every other post_check in that registry asks a BOOT-TIME question, and
    the boot lines are in the menu session. This one asks an IN-MATCH question, and the match is a
    SIBLING directory: the menu log exists, is perfectly readable, and contains no `; [netind]` line
    because the indicator only draws in a live lockstep match. Taking the named directory's log
    without looking would therefore report "the indicator never drew" for a run in which it drew
    fine -- an absence produced by the search, not by the subject.

    So the rule is CONTENT-FIRST: among the named directory, its siblings (the lane's other session
    runs) and anything below it, prefer the newest log that actually carries a `; [netind]` line;
    fall back to the newest log at all, so a run where the indicator really was silent still gets
    read and still fails on its own merits rather than on a missing file.
    """
    if os.path.isfile(target):
        return target

    roots = [target]
    # SIBLINGS ONLY WHEN THE TARGET IS ITSELF A SESSION DIRECTORY. Widening unconditionally would
    # walk the whole lane ROOT when handed a lane folder, and then a run of a DIFFERENT scenario
    # could answer this one's question -- which it did, once, silently.
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
                if "[netind]" in fh.read():
                    return p
        except OSError:
            continue
    return cands[0]


# ---- the negative arm --------------------------------------------------------------------------
#
# This checker only ever runs when the rig does, so its own failure paths would otherwise never be
# exercised -- and a checker that cannot fail is a checker that reports PASS on an empty log. The
# selftest plants each failure into a throwaway lane and requires a RED, which is what lets the
# lint_repo row stand in for a rig run.

NL = chr(10)
GOOD = (
    "[00:00:01.000] ; [netind] shown at 8,44 font=0 peers=1 lat_supported=1"
    + NL
    + "[00:00:03.000] ; [netind] peer0 name=host srtt_ms=4 ipdv_ms=0 loss_pm=0 bar=4 cmd_ms=80 "
    + "look_ms=60 step_ms=20"
    + NL
    + "[00:00:05.000] ; [netind] peer0 name=host srtt_ms=5 ipdv_ms=1 loss_pm=0 bar=4 cmd_ms=80 "
    + "look_ms=60 step_ms=20"
    + NL
)
STALL_NAMED = "[00:00:06.000] ; [netind] stall waiting_for=client peer=1 ms=3011" + NL
STALL_BLANK = "[00:00:06.000] ; [netind] stall waiting_for= peer=1 ms=3011" + NL


def selftest():
    import subprocess

    cases = [
        ("healthy", GOOD, [], 0),
        ("never drawn", GOOD.split(NL, 1)[1], [], 1),
        ("no samples at all", GOOD.split(NL)[0] + NL, [], 1),
        (
            "unmeasured link reported as a link",
            GOOD.replace("srtt_ms=4", "srtt_ms=-1").replace("srtt_ms=5", "srtt_ms=-1"),
            [],
            1,
        ),
        ("cmd_ms no longer look+step", GOOD.replace("cmd_ms=80", "cmd_ms=999", 1), [], 1),
        ("a pacing term read as zero", GOOD.replace("step_ms=20", "step_ms=0", 1), [], 1),
        ("bar outside 1..4 on a measured link", GOOD.replace("bar=4", "bar=7", 1), [], 1),
        ("srtt band missed", GOOD, ["--srtt", "150", "250"], 1),
        (
            "srtt band met",
            GOOD.replace("srtt_ms=4", "srtt_ms=201").replace("srtt_ms=5", "srtt_ms=199"),
            ["--srtt", "150", "250"],
            0,
        ),
        ("min-cmd not reached", GOOD, ["--min-cmd", "400"], 1),
        ("stall expected, none logged", GOOD, ["--expect-stall"], 1),
        ("stall expected, one named", GOOD + STALL_NAMED, ["--expect-stall"], 0),
        ("stall logged but nameless", GOOD + STALL_BLANK, ["--expect-stall"], 1),
        # mp:L1c
        ("resolved name accepted", GOOD, ["--reject-placeholder-name"], 0),
        (
            "placeholder name rejected",
            GOOD.replace("name=host", "name=PLAYER 0"),
            ["--reject-placeholder-name"],
            1,
        ),
    ]
    # Multi-target cases (mp:L1c) -- a SECOND log with a placeholder name must fail the run even
    # though the FIRST one is clean, which is the exact shape net_hud's post_check_peers produces:
    # the host's log always resolved fine, so a single-target check never saw the client's bug.
    multi_cases = [
        ("multi: both resolved", [GOOD, GOOD], ["--reject-placeholder-name"], 0),
        (
            "multi: second target has placeholder",
            [GOOD, GOOD.replace("name=host", "name=PLAYER 0")],
            ["--reject-placeholder-name"],
            1,
        ),
    ]
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, text, argv, want in cases:
            d = os.path.join(tmp, name.replace(" ", "_"))
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
                fh.write(text)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__), d] + argv,
                capture_output=True,
                text=True,
            )
            got = r.returncode
            print("  %-38s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d%s%s" % (name, got, want, NL, r.stdout))
        for name, texts, argv, want in multi_cases:
            # FILE targets, not directories -- find_log() returns a file target immediately with no
            # sibling walk (see its docstring), so two throwaway cases here cannot accidentally
            # collide on each other's log the way two DIRECTORY targets sharing this tmp root would.
            # Real usage (net_hud's post_check_peers) hands over directory targets, but those live
            # under DIFFERENT lanes (LANE_ROOT/<lane>/...), so dirname(target) never reaches a
            # sibling lane's log either -- this is a faithful, simpler stand-in for that shape.
            files = []
            for i, text in enumerate(texts):
                p = os.path.join(tmp, "%s_%d.log" % (name.replace(" ", "_").replace(":", ""), i))
                with open(p, "w", encoding="utf-8") as fh:
                    fh.write(text)
                files.append(p)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__)] + files + argv,
                capture_output=True,
                text=True,
            )
            got = r.returncode
            print("  %-38s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d%s%s" % (name, got, want, NL, r.stdout))
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print(
        "check_net_indicator --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails))
    )
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    # nargs="+" -- mp:L1c. net_hud's post_check_peers hands over EVERY peer's run directory (the
    # runner appends them after any of this script's own flags -- see test_ui.py's post_check()),
    # and reading only one of them is exactly the gap that let the client's `name=PLAYER 0` fallback
    # through: a single target here used to mean "whichever peer's post_check the registry entry
    # happened to name", which for net_hud was always the HOST -- the one side that never had the
    # bug (the host learns real transport ids from HELLO; only a client's one connection is
    # declared-id -1). Multiple targets are read and their samples pooled before any check runs, so
    # a placeholder name on EITHER peer's log fails the run.
    ap.add_argument(
        "targets",
        nargs="+",
        help="one or more session run dirs (or lane dirs above one), or mh_net.log files",
    )
    ap.add_argument(
        "--srtt",
        nargs=2,
        type=int,
        metavar=("LO", "HI"),
        help="every measured sample's srtt_ms must sit in [LO, HI] -- the shim-band assertion",
    )
    ap.add_argument("--min-cmd", type=int, help="the LARGEST cmd_ms seen must be >= this")
    ap.add_argument(
        "--expect-stall",
        action="store_true",
        help="require at least one stall line, with a non-empty peer NAME",
    )
    ap.add_argument(
        "--reject-placeholder-name",
        action="store_true",
        help="mp:L1c -- fail if any measured sample's name= is the unresolved `PLAYER <n>` "
        "placeholder. Only meaningful where every peer's name IS known (e.g. net_hud's pinned "
        "setup.dat host/client names) -- peer_name()'s fallback is a legitimate answer for a "
        "genuinely unprintable/empty name elsewhere, so this is opt-in rather than a default check",
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

    shown = SHOWN_RE.findall(text)
    samples = SAMPLE_RE.findall(text)
    stalls = STALL_RE.findall(text)
    print(
        "read %s: %d shown, %d samples, %d stall lines"
        % (log, len(shown), len(samples), len(stalls))
    )

    fails = []
    if not shown:
        fails.append(
            "no `; [netind] shown` line -- the indicator never reached a drawn frame (armed but "
            "never visible is the failure this line exists to tell apart from working)"
        )
    measured = [s for s in samples if int(s[2]) >= 0]
    if not samples:
        fails.append("no `; [netind] peer..` sample lines at all")
    elif not measured:
        fails.append(
            "%d samples, every one with srtt_ms=-1 -- the transport measured nothing (a TCP run, or "
            "channel B never ran)" % len(samples)
        )

    for s in samples:
        peer, name, srtt, ipdv, loss, bar, cmd, look, step = s
        if int(cmd) != int(look) + int(step):
            fails.append(
                "sample peer%s: cmd_ms=%s but look_ms+step_ms=%d -- the displayed command latency is "
                "no longer the sum it is documented to be" % (peer, cmd, int(look) + int(step))
            )
            break
        if int(look) <= 0 or int(step) <= 0:
            fails.append(
                "sample peer%s: look_ms=%s step_ms=%s -- a pacing term read as zero"
                % (peer, look, step)
            )
            break
    for s in measured:
        if not (1 <= int(s[5]) <= 4):
            fails.append("sample peer%s: bar=%s outside 1..4 on a MEASURED link" % (s[0], s[5]))
            break

    if a.srtt:
        lo, hi = a.srtt
        out = [s for s in measured if not (lo <= int(s[2]) <= hi)]
        if not measured:
            fails.append("--srtt %d..%d: no measured sample to check" % (lo, hi))
        elif out:
            fails.append(
                "--srtt %d..%d: %d/%d measured samples outside the band (e.g. srtt_ms=%s)"
                % (lo, hi, len(out), len(measured), out[0][2])
            )
        else:
            print(
                "  srtt band %d..%d: all %d measured samples inside (min %d, max %d)"
                % (
                    lo,
                    hi,
                    len(measured),
                    min(int(s[2]) for s in measured),
                    max(int(s[2]) for s in measured),
                )
            )

    if a.min_cmd is not None:
        top = max((int(s[6]) for s in samples), default=-1)
        if top < a.min_cmd:
            fails.append("--min-cmd %d: the largest cmd_ms seen was %d" % (a.min_cmd, top))
        else:
            print("  cmd_ms peaked at %d (>= %d)" % (top, a.min_cmd))

    if a.expect_stall:
        named = [s for s in stalls if s[0].strip()]
        if not named:
            fails.append(
                "--expect-stall: %d stall lines, %d of them naming a peer -- an anonymous stall is "
                "exactly what this item replaced" % (len(stalls), len(named))
            )
        else:
            print("  stall named %r (peer %s, %s ms)" % (named[0][0], named[0][1], named[0][2]))

    if a.reject_placeholder_name:
        bad = [s for s in measured if re.match(r"^PLAYER \d+$", s[1])]
        if bad:
            fails.append(
                "--reject-placeholder-name: %d/%d measured samples carry the unresolved `PLAYER <n>` "
                "name (e.g. peer%s name=%s) -- the transport-id-to-strategic-slot resolution (mp:L1c) "
                "did not find a real name" % (len(bad), len(measured), bad[0][0], bad[0][1])
            )
        else:
            print(
                "  name resolution: %d measured samples, none fell back to `PLAYER <n>`"
                % len(measured)
            )

    for f in fails:
        print("FAIL: %s" % f)
    print("check_net_indicator: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
