#!/usr/bin/env python3
"""check_no_crash_marker.py -- mp:CH1 (the AV clause): a run that QUITS a frozen lockstep match must
not write a crash marker on either peer.

THE BUG THIS GATES. The one captured field crash of 2026-09-20 (a952c570_2) was an access violation
one second after the player quit a match whose lockstep clock had frozen on a data-silent peer
(mp:GS1's ghost, before mp:GS2 dropped such peers). The `gs2_quit_frozen` scenario reproduces the
SHAPE -- a client fences its sim (transport alive), the host is lockstep-blocked on it for >= 1.5 s
and then quits via ESC -> Quit -> Yes -- and this post_check asserts the consequence that mattered:
no `mh_crash_<pid>.marker` written by either peer's process during the run (mh.dll's vectored
handler writes one on any fatal exception -- src/mh_dll/mh/seams/crash_marker.cpp -- under the
lane's `logs\\`, per pid, when no launcher names a path).

Usage:  check_no_crash_marker.py <host run dir> <client run dir>   (test_ui: post_check_peers)
        check_no_crash_marker.py --selftest

A run dir is the peer's PROCESS log directory (`<lane>\\logs\\<stamp>_menu_solo`), the one the runner
hands a post_check. The game does not log its pid, so a marker is attributed to a run by TIME: any
marker under the lane's logs\\ whose mtime is at or after the run dir's own creation belongs to this
run (a lane's logs\\ keeps markers from earlier runs; those are older than the run dir).
Exit 0 = PASS, 1 = a marker was written during a run, 2 = REFUSED (the evidence is not there).
"""

import glob
import os
import re
import shutil
import sys
import tempfile

MARKER_GLOB = "mh_crash_*.marker"
MARKER_RE = re.compile(r"mh_crash_(\d+)\.marker$")


class Refusal(Exception):
    pass


def run_start(run_dir):
    """When this run's process started: the run dir's own creation time (the DLL makes it at
    boot). Refuses a dir with no mh_net.log -- no evidence that a game ran there at all."""
    if not os.path.isfile(os.path.join(run_dir, "mh_net.log")):
        raise Refusal("%s has no mh_net.log" % run_dir)
    return os.path.getctime(run_dir)


def markers_since(run_dir, t0):
    logs = os.path.dirname(os.path.abspath(run_dir).rstrip("\\/"))
    if os.path.basename(logs) != "logs":
        raise Refusal("%s is not a run directory under a lane's logs/" % run_dir)
    out = []
    for fp in glob.glob(os.path.join(logs, MARKER_GLOB)):
        if MARKER_RE.search(os.path.basename(fp)) and os.path.getmtime(fp) >= t0 - 1.0:
            out.append(fp)
    return sorted(out)


def check(run_dirs):
    if len(run_dirs) != 2:
        raise Refusal(
            "want exactly two peer run directories (host, client), got %d" % len(run_dirs)
        )
    fails = []
    for role, rd in zip(("host", "client"), run_dirs):
        t0 = run_start(rd)
        found = markers_since(rd, t0)
        print("  %-6s %s  marker(s) since its start: %d" % (role, os.path.basename(rd), len(found)))
        for fp in found:
            head = ""
            try:
                with open(fp, "r", encoding="utf-8", errors="replace") as fh:
                    head = fh.readline().strip()[:120]
            except OSError:
                pass
            fails.append("%s wrote %s during the run -- %s" % (role, os.path.basename(fp), head))
    for f in fails:
        print("  [FAIL] %s" % f)
    if fails:
        print("check_no_crash_marker: FAIL (%d)" % len(fails))
        return 1
    print("check_no_crash_marker: PASS -- neither peer's process wrote a crash marker")
    return 0


# ---- selftest ---------------------------------------------------------------------------------


def plant(root, role, marker=False, net_log=True, stale=False):
    lane = os.path.join(root, role)
    logs = os.path.join(lane, "logs")
    run = os.path.join(logs, "20260921T060000Z_menu_" + role)
    os.makedirs(run)
    if net_log:
        with open(os.path.join(run, "mh_net.log"), "w", encoding="utf-8") as fh:
            fh.write("[00:00:00.000] ; [build] mh 0.0.0-dev\n")
    if marker:
        fp = os.path.join(logs, "mh_crash_%d.marker" % (7 if stale else 4242))
        with open(fp, "w", encoding="utf-8") as fh:
            fh.write("mh_crash_marker v1 code=C0000005 module=mh.exe+0x1234\n")
        if stale:  # an EARLIER run's marker: older than this run dir by an hour
            old = os.path.getctime(run) - 3600
            os.utime(fp, (old, old))
    return run


def selftest():
    cases = [
        ("green: no marker on either peer", dict(), dict(), 0),
        ("RED: the host wrote a marker", dict(marker=True), dict(), 1),
        ("RED: the client wrote a marker", dict(), dict(marker=True), 1),
        (
            "green: a STALE marker from an earlier run is not this run's",
            dict(marker=True, stale=True),
            dict(),
            0,
        ),
        ("REFUSED: no mh_net.log (no game ran here)", dict(net_log=False), dict(), 2),
    ]
    bad = 0
    for title, hk, ck, want in cases:
        root = tempfile.mkdtemp(prefix="no_crash_marker_selftest_")
        try:
            h = plant(root, "host", **hk)
            c = plant(root, "client", **ck)
            try:
                got = check([h, c])
            except Refusal as e:
                print("  [REFUSED] %s" % e)
                got = 2
        finally:
            shutil.rmtree(root, ignore_errors=True)
        ok = got == want
        bad += 0 if ok else 1
        print("  [%s] %-62s want=%s got=%s" % ("ok" if ok else "BAD", title, want, got))
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
