#!/usr/bin/env python3
"""check_map_xfer_twice.py -- mp:X2j: A SECOND MAP DOWNLOAD IN ONE GAME PROCESS MUST COMPLETE.

THE BUG (2026-10-19 player report): a joiner downloaded a map, played, returned to the lobby, and the
next match's new map sat at 0% until the game was restarted. Cause: channel C's receive frontier
survives a finished download, so the second transfer's chunks counted as duplicates and the Receiver
never saw one (fix: map_transfer.cpp rewinds it with MH_Net_SnapshotCancel(MH_SNAP_DISCARD_RX) on
entering NEED and at session_reset).

WHAT IT ASSERTS, over each peer's whole PROCESS (the process mh_net.log plus every session directory
of that process, which is where each game's lines land):
  host    >= 2 `[map] send armed`, >= 2 `[map] peer ... holds the map`, >= 2 `[map] start OK`
          (each of the two games armed a transfer, saw it finish, and released the Start gate)
  client  >= 2 `[map] client stored` (each game stored its own download)
Fewer than two stored/holds lines is the bug: the second transfer armed (and re-armed every 90 s)
but never finished.

USAGE  python tools/check_map_xfer_twice.py <host-run-dir> <client-run-dir>
       python tools/check_map_xfer_twice.py --selftest
`<run-dir>` is the post_check_peers hand-over: the lane's newest PROCESS directory
("<stamp>_menu_<role>").
"""

import glob
import json
import os
import re
import shutil
import sys
import tempfile

RE_ARMED = re.compile(r"; \[map\] send armed to peer")
RE_HOLDS = re.compile(r"; \[map\] peer \d+ '[^']*' holds the map")
RE_START_OK = re.compile(r"; \[map\] start OK")
RE_STORED = re.compile(r"; \[map\] client stored ")
WANT = 2


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


def process_text(run_dir):
    """The process dir's mh_net.log plus those of every session dir whose session.json names it."""
    if not os.path.isdir(run_dir):
        raise Refusal("no such run directory: %s" % run_dir)
    leaf = os.path.basename(run_dir.rstrip("\\/"))
    logs = os.path.dirname(run_dir.rstrip("\\/"))
    if os.path.basename(logs) != "logs":
        raise Refusal("%s is not a run directory under a lane's logs/" % run_dir)
    dirs = [run_dir]
    for d in sorted(glob.glob(os.path.join(logs, "*"))):
        try:
            with open(os.path.join(d, "session.json"), encoding="utf-8", errors="replace") as fh:
                if json.load(fh).get("process_dir") == leaf:
                    dirs.append(d)
        except (OSError, ValueError, AttributeError):
            pass
    text = []
    for d in dirs:
        fp = os.path.join(d, "mh_net.log")
        if os.path.isfile(fp):
            with open(fp, encoding="utf-8", errors="replace") as fh:
                text.append(fh.read())
    if not text:
        raise Refusal("%s: no mh_net.log in the process or its sessions" % run_dir)
    return "\n".join(text)


def verdict(host_text, client_text):
    fails = []
    for what, rx, txt, role in (
        ("send armed", RE_ARMED, host_text, "host"),
        ("peer ... holds the map", RE_HOLDS, host_text, "host"),
        ("start OK", RE_START_OK, host_text, "host"),
        ("client stored", RE_STORED, client_text, "client"),
    ):
        n = len(rx.findall(txt))
        print("  %-6s %-24s x%d" % (role, what, n))
        if n < WANT:
            fails.append(
                "%s: %d `%s` line(s), want >= %d -- the second download never finished"
                % (role, n, what, WANT)
            )
    return fails


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) != 3:
        print(__doc__)
        return 2
    try:
        fails = verdict(process_text(argv[1]), process_text(argv[2]))
    except Refusal as e:
        print("[REFUSED] %s" % e)
        return 2
    for f in fails:
        print("[FAIL] %s" % f)
    if fails:
        return 1
    print("[PASS] both games downloaded their map in one process (mp:X2j)")
    return 0


def selftest():
    one_game = (
        "; [map] send armed to peer 1 'c' (1 B of m)\n"
        "; [map] peer 1 'c' holds the map (sha=ab)\n"
        "; [map] start OK -- x\n"
    )
    host_ok = one_game * 2
    host_rearm = one_game + "; [map] send armed to peer 1 'c' (1 B of m)\n"
    stored = "; [map] client stored mh_dl\\m.ab.mpm (1 B, sha=ab)\n"
    bad = 0

    def expect(name, h, c, want_fail):
        nonlocal bad
        got = bool(verdict(h, c))
        print("selftest %-32s %s" % (name, "ok" if got == want_fail else "WRONG"))
        if got != want_fail:
            bad += 1

    expect("clean", host_ok, stored * 2, False)
    expect("second download stuck (host)", host_rearm, stored * 2, True)
    expect("second download stuck (client)", host_ok, stored, True)
    expect("empty logs", "", "", True)
    tmp = tempfile.mkdtemp()
    try:
        logs = os.path.join(tmp, "logs")
        proc = os.path.join(logs, "2026-01-01T00-00-00Z_menu_solo")
        sess = os.path.join(logs, "2026-01-01T00-00-10Z_aaaaaaaa_m_client")
        for d in (proc, sess):
            os.makedirs(d)
        for d in (proc, sess):
            with open(os.path.join(d, "mh_net.log"), "w") as fh:
                fh.write(stored)
        with open(os.path.join(sess, "session.json"), "w") as fh:
            json.dump({"process_dir": os.path.basename(proc)}, fh)
        got = len(RE_STORED.findall(process_text(proc)))
        print("selftest %-32s %s" % ("process+session merge", "ok" if got == 2 else "WRONG"))
        bad += got != 2
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
