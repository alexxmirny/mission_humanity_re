#!/usr/bin/env python3
"""check_relay_restart -- mp:R4b: a relayed pair SURVIVED a relay restart by re-HELLOing, not by
luck and not by the restart never having happened.

WHY A CHECKER, NOT PIXELS. `relay_restart` (tools/test_ui.py) proves the link lived by waiting for
the game clock to advance after the restart -- a dead link stalls the clock and the WAIT times out.
That is the right liveness oracle and it cannot say HOW: a pair that stayed connected because the
runner's restart request never fired, or because the relay came back before the peers noticed,
renders the same frame as a pair that did the whole re-HELLO dance. The claim R4b makes is the
dance -- dead-ends G245 is the run where nothing in either peer's log moved between the restart and
`dropped -- no data from peer` -- so this reads the lines that ARE the claim:

  * every peer's mh_net.log carries `udp relay leg LOST` (the relay said NOT_REGISTERED and the peer
    kept its handle) and, AFTER it, `udp relay leg RESTORED` (the re-HELLO's WELCOME landed);
  * no peer's log carries `dropped -- no data from peer` (the endpoint's 10 s silence timeout);
  * the relay's own log carries at least two `listening` lines (it really did come back up).

    python tools/check_relay_restart.py --relay-log <relay log> <run dir> [<run dir> ...]
    python tools/check_relay_restart.py --selftest

`<run dir>` is a peer's session directory holding mh_net.log, or a lane directory above one (the
newest log carrying relay content wins -- the same shape check_relay_path's find_log has, for the
same reason: test_ui hands over the menu session and the match lines are in a sibling).
"""

import argparse
import os
import re
import sys

LOST_NEEDLE = "net: udp relay leg LOST"
RESTORED_RE = re.compile(r"net: udp relay leg RESTORED after (\d+) ms -- (.*?) \(mp:R4b\)")
DROPPED_NEEDLE = "dropped -- no data from peer"
# The `listening` LINE, not any `addr=` field (peer_registered lines carry one too).
LISTEN_RE = re.compile(r'"listening"[^\n]*?addr=(?:[0-9.]+|\[[^\]]+\]):(\d+)')
RELAY_CONTENT_MARKERS = ("net: udp relay leg", "net: udp path RELAY")


def _has_relay_content(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return any(m in fh.read() for m in RELAY_CONTENT_MARKERS)
    except OSError:
        return False


def find_log(target):
    if os.path.isfile(target):
        return target
    own = os.path.join(target, "mh_net.log")
    if os.path.isfile(own) and _has_relay_content(own):
        return own  # the session handed over IS the match session: no need to look sideways
    roots = [target]
    if os.path.isfile(own):
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
        if _has_relay_content(p):
            return p
    return cands[0]


def check_peer(label, log):
    """Returns (ok, lines)."""
    try:
        with open(log, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as e:
        return False, ["%s: cannot read %s: %s" % (label, log, e)]
    out = []
    ok = True
    lost = text.find(LOST_NEEDLE)
    if lost < 0:
        ok = False
        out.append(
            "%s: FAIL -- no `%s` line: the relay never told this peer NOT_REGISTERED (the "
            "restart did not reach it, or the relay is older than mp:R4b)" % (label, LOST_NEEDLE)
        )
    m = RESTORED_RE.search(text, lost if lost >= 0 else 0)
    if m is None:
        ok = False
        out.append(
            "%s: FAIL -- no `udp relay leg RESTORED` after the LOST: the re-HELLO never got its "
            "WELCOME" % label
        )
    else:
        out.append("%s: restored after %s ms -- %s" % (label, m.group(1), m.group(2)))
    if DROPPED_NEEDLE in text:
        ok = False
        out.append(
            "%s: FAIL -- `%s`: the endpoint's silence timeout fired, the link died"
            % (label, DROPPED_NEEDLE)
        )
    return ok, out


def check_relay(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = re.sub(r"\x1b\[[0-9;]*m", "", fh.read())
    except OSError as e:
        return False, ["relay: cannot read %s: %s" % (path, e)]
    n = len(LISTEN_RE.findall(text))
    if n < 2:
        return False, [
            "relay: FAIL -- %d `listening` line(s) in %s; a restart writes a second one" % (n, path)
        ]
    return True, ["relay: %d listening lines (restarted %d time(s))" % (n, n - 1)]


# ---- the negative arm ----------------------------------------------------------------------------

GOOD_PEER = (
    "[00:01:00.000] net: udp relay leg UP -- 10.0.0.1:7100 room=6501, handle 1\n"
    "[00:02:00.000] net: udp relay refused us -- this peer is not registered (the relay restarted?)\n"
    "[00:02:00.001] net: udp relay leg LOST -- the relay does not know handle 1 (it restarted, or "
    "evicted us); re-HELLOing under the deployment key and asking for the same handle (mp:R4b)\n"
    "[00:02:00.512] net: udp relay leg RESTORED after 511 ms -- handle 1 kept, pair state kept; "
    "the leg runs on the deployment key from here (mp:R4b)\n"
)
NEVER_LOST = "[00:01:00.000] net: udp relay leg UP -- 10.0.0.1:7100 room=6501, handle 1\n"
LOST_NOT_RESTORED = GOOD_PEER.splitlines(keepends=True)[0:3]
LOST_NOT_RESTORED = "".join(LOST_NOT_RESTORED)
RESTORED_THEN_DROPPED = (
    GOOD_PEER
    + "[00:02:11.000] net: udp conn 0 dropped -- no data from peer within the link timeout\n"
)
# The relay's `--log human` shape (ANSI stripped), which is what test_ui's RelayProc runs it with.
RELAY_TWICE = (
    '2026-09-19T00:00:00Z  INFO mh_relay listening event="listening" addr=0.0.0.0:7100 strict=false\n'
    "[test_ui] relay restart requested -- respawning on :7100\n"
    '2026-09-19T00:00:03Z  INFO mh_relay listening event="listening" addr=0.0.0.0:7100 strict=false\n'
)
RELAY_ONCE = '2026-09-19T00:00:00Z  INFO mh_relay listening event="listening" addr=0.0.0.0:7100 strict=false\n'


def selftest():
    import subprocess
    import tempfile

    cases = [
        ("both peers LOST+RESTORED, relay listened twice", GOOD_PEER, GOOD_PEER, RELAY_TWICE, 0),
        ("one peer never LOST", GOOD_PEER, NEVER_LOST, RELAY_TWICE, 1),
        ("LOST but never RESTORED", LOST_NOT_RESTORED, GOOD_PEER, RELAY_TWICE, 1),
        (
            "restored, then the link dropped anyway",
            RESTORED_THEN_DROPPED,
            GOOD_PEER,
            RELAY_TWICE,
            1,
        ),
        ("relay never actually restarted", GOOD_PEER, GOOD_PEER, RELAY_ONCE, 1),
        ("a peer with no mh_net.log at all", None, GOOD_PEER, RELAY_TWICE, 2),
    ]
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, a, b, relay, want) in enumerate(cases):
            base = os.path.join(tmp, "c%d" % i)
            dirs = []
            for j, text in enumerate((a, b)):
                # One LANE per peer, as on the rig: the sideways look in find_log stays inside it.
                d = os.path.join(base, "lane%d" % j, "logs", "run")
                os.makedirs(d, exist_ok=True)
                if text is not None:
                    with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
                        fh.write(text)
                dirs.append(d)
            rl = os.path.join(base, "relay.log")
            with open(rl, "w", encoding="utf-8") as fh:
                fh.write(relay)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__), "--relay-log", rl] + dirs,
                capture_output=True,
                text=True,
            )
            print("  %-58s exit %d (want %d)" % (name, r.returncode, want))
            if r.returncode != want:
                fails.append("%s: exit %d, wanted %d\n%s" % (name, r.returncode, want, r.stdout))
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print(
        "check_relay_restart --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails))
    )
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument("--relay-log", required=True, help="the relay process's own log")
    ap.add_argument("targets", nargs="+", help="every peer's run dir (or lane dir), or mh_net.log")
    a = ap.parse_args()

    ok_all = True
    for i, t in enumerate(a.targets):
        log = find_log(t)
        if not log:
            print("REFUSED: no mh_net.log at or under %s" % t)
            return 2
        ok, lines = check_peer("peer%d" % i, log)
        for ln in lines:
            print(ln)
        ok_all = ok_all and ok
    rl = a.relay_log
    if not os.path.isabs(rl):
        rl = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), rl)
    ok, lines = check_relay(rl)
    for ln in lines:
        print(ln)
    ok_all = ok_all and ok
    print("check_relay_restart: %s" % ("PASS" if ok_all else "FAIL"))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
