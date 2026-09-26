#!/usr/bin/env python3
"""check_direct_dial -- mp:R7a: a client with `[net] relay` set that used *Internet server* + a typed
address connected DIRECT, and the relay was never contacted for that dial.

WHY A CHECKER, NOT PIXELS. R7a's claim is an ABSENCE -- with a relay configured, the *Internet server*
path no longer tunnels; it dials the host's address itself. Nothing in a captured frame can show that
a datagram did NOT cross the relay (a direct lobby and a relayed lobby render the same). The claim is
only in the logs, so this reads the lines that ARE the claim:

  * the CLIENT's mh_net.log carries `; R7a: client dial is DIRECT` (net_seams.cpp lazy_start, the
    decision mh.dll made) and NO relay contact AFTER it -- no `net: udp RELAY mode`, no
    `net: udp relay leg UP`. Contact before it is the first browser's directory probe, which since
    mp:R7b runs for every peer with a relay configured. A build that regressed to always-relay would
    show a `leg UP` after the decision; a build that mis-decided would show `client dial is RELAYED`.
  * with --relay-log: the relay's final `counters` line reads `forwarded=0` -- it carried no peer
    traffic. `direct_dial_with_relay_set` puts `relay=` on the CLIENT only (relay_client_only), so
    the host never registers; the only registration is the client's directory-only browse leg.

    python tools/check_direct_dial.py [--relay-log <relay log>] <run dir> [<run dir> ...]
    python tools/check_direct_dial.py --selftest

`<run dir>` is a peer's session directory (or a lane dir above one). The peer whose log carries the
R7a decision is the client; a run where NO peer log carries it is a FAIL (the build under test did not
run the R7a path at all -- this checker is registered on direct_dial_with_relay_set only).
"""

import argparse
import os
import re
import sys

# The client's own decision line -- mh.dll chose DIRECT (net_seams.cpp lazy_start, mp:R7a).
DIRECT_NEEDLE = "; R7a: client dial is DIRECT"
RELAYED_NEEDLE = "; R7a: client dial is RELAYED"
# Any of these in a peer's log means it DID contact the relay -- exactly what a direct dial must not.
RELAY_CONTACT_MARKERS = (
    "net: udp RELAY mode -- ",
    "net: udp relay leg UP",
    "net: udp relay leg RESTORED",
    "net: udp path DIRECT",  # a promotion off a relay -> it was relayed first
    "net: udp path RELAY",
)
# Markers that identify a peer's net log at all (so find_log picks a real one, relay content or not).
NET_CONTENT_MARKERS = (DIRECT_NEEDLE, RELAYED_NEEDLE, "net: CLIENT connected", "net: udp ")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
# The relay's live peer count and cumulative registrations on its final counters line.
COUNTERS_PEERS_RE = re.compile(r'event="counters".*?(?<![a-z_])peers=(\d+)')
COUNTERS_REG_RE = re.compile(r'event="counters".*?peers_registered=(\d+)')
COUNTERS_FWD_RE = re.compile(r'event="counters".*?(?<![a-z_])forwarded=(\d+)')


def _read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def _has(path, markers):
    body = _read(path)
    return any(m in body for m in markers)


def find_log(target, markers=NET_CONTENT_MARKERS):
    """The mh_net.log this peer's dial is in -- content-first (the newest log that mentions the net),
    mirroring check_relay_rooms.find_log. test_ui hands over the menu session; the dial lines may be
    in a sibling match session."""
    if os.path.isfile(target):
        return target
    own = os.path.join(target, "mh_net.log")
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
        if _has(p, markers):
            return p
    return cands[0]


def check(peer_dirs, relay_log=None):
    """Returns (ok, [lines]). ok iff a client dialled DIRECT with no relay contact, and (if given)
    the relay registered nobody."""
    out = []
    fails = []

    # 1. Find the client -- the peer whose log carries the R7a decision line.
    client_log = None
    client_body = ""
    for d in peer_dirs:
        log = find_log(d, markers=(DIRECT_NEEDLE, RELAYED_NEEDLE))
        if log and (DIRECT_NEEDLE in _read(log) or RELAYED_NEEDLE in _read(log)):
            client_log = log
            client_body = _read(log)
            break
    if client_log is None:
        fails.append(
            "no peer log carries `%s` (or the RELAYED form) -- the R7a dial-mode decision never ran; "
            "the build under test does not have mp:R7a" % DIRECT_NEEDLE
        )
        # nothing more to check without the client
        for f in fails:
            out.append("FAIL: %s" % f)
        return False, out

    out.append("client log: %s" % client_log)
    if DIRECT_NEEDLE not in client_body:
        fails.append(
            "the client's dial was RELAYED, not direct -- `%s` present but not `%s`. *Internet server* "
            "+ a typed IP must be a direct dial with a relay configured (mp:R7a)."
            % (RELAYED_NEEDLE, DIRECT_NEEDLE)
        )
    else:
        out.append("  the client chose DIRECT (`%s`)" % DIRECT_NEEDLE)

    # mp:R7b -- the first browser probes the relay for every peer with one configured, so relay
    # contact BEFORE the direct decision is the browse and is expected. What must not happen is relay
    # contact for the direct connection itself: nothing after the LAST DIRECT decision line.
    after = client_body[client_body.rfind(DIRECT_NEEDLE) :] if DIRECT_NEEDLE in client_body else ""
    contacted = [m for m in RELAY_CONTACT_MARKERS if m in after]
    if contacted:
        fails.append(
            "the client CONTACTED the relay on a direct dial -- found %s after its DIRECT decision. "
            "A direct dial must not bring up a relay leg."
            % ", ".join("`%s`" % m for m in contacted)
        )
    else:
        out.append("  no relay contact after the DIRECT decision (no RELAY mode, no leg UP)")

    # 2. The relay itself, if its log was given: it carried no peer traffic. The client's browse
    # registers a directory-only leg (mp:R7b), so peers_registered may be nonzero; `forwarded` counts
    # datagrams relayed between peers, and a direct connection puts none through it.
    if relay_log:
        body = ANSI_RE.sub("", _read(relay_log))
        if not body:
            fails.append(
                "relay log %s is empty/absent -- cannot confirm the relay was idle" % relay_log
            )
        else:
            fwd = COUNTERS_FWD_RE.findall(body)
            regs = COUNTERS_REG_RE.findall(body)
            if not fwd:
                fails.append(
                    "relay log %s has no `counters` line (the relay never reported)" % relay_log
                )
            elif int(fwd[-1]) != 0:
                fails.append(
                    "relay forwarded %s datagram(s) (final counters line) -- the connection went "
                    "through the relay; a direct dial must leave it at 0" % fwd[-1]
                )
            else:
                out.append(
                    "  relay carried nothing: forwarded=0 (peers_registered=%s -- the browse leg) "
                    "over %d counters line(s)" % (regs[-1] if regs else "?", len(fwd))
                )

    for f in fails:
        out.append("FAIL: %s" % f)
    return not fails, out


# ---- the negative arm (this checker runs only when the rig does; plant each case) ----------------

DIRECT_LOG = (
    "; R7a: client dial is DIRECT (Internet server + typed IP) -- `[net] relay` is configured but "
    "NOT used for this connection\n"
    "net: CLIENT connected to 192.168.0.61:6501 as player 1\n"
)
RELAYED_LOG = (
    "; R7a: client dial is RELAYED via 127.0.0.1:7100 (room 12345)\n"
    "net: udp RELAY mode -- 127.0.0.1:7100 room=12345 as client\n"
    "net: udp relay leg UP -- 127.0.0.1:7100 room=12345, handle 3\n"
)
NO_DECISION_LOG = "net: CLIENT connected to 192.168.0.61:6501 as player 1\n"
RELAY_IDLE = (
    '2026-09-19T00:00:10Z  INFO mh_relay counters event="counters" peers=0 rooms=0 leg_rx=0 '
    "forwarded=0 peers_registered=0 register_refused=0 \"counters\"\n"
)
RELAY_BUSY = (
    '2026-09-19T00:00:10Z  INFO mh_relay counters event="counters" peers=2 rooms=1 leg_rx=8 '
    "forwarded=6 peers_registered=2 register_refused=0 \"counters\"\n"
)
# mp:R7b -- the first browser's directory probe, then the direct dial: relay contact BEFORE the
# decision, a directory-only registration on the relay, nothing forwarded.
BROWSE_THEN_DIRECT_LOG = (
    "net: udp RELAY mode -- 127.0.0.1:7100 room=0 as client\n"
    "net: udp relay leg UP -- 127.0.0.1:7100 room=0, handle 3\n" + DIRECT_LOG
)
DIRECT_THEN_RELAY_LOG = DIRECT_LOG + "net: udp relay leg UP -- 127.0.0.1:7100 room=5, handle 4\n"
RELAY_BROWSED = (
    '2026-09-19T00:00:10Z  INFO mh_relay counters event="counters" peers=0 rooms=0 leg_rx=4 '
    "forwarded=0 peers_registered=1 register_refused=0 \"counters\"\n"
)


def selftest():
    import subprocess
    import tempfile

    cases = [
        ("a real direct dial, relay idle", DIRECT_LOG, RELAY_IDLE, 0),
        ("the client dialled RELAYED (regression)", RELAYED_LOG, RELAY_IDLE, 1),
        ("no R7a decision at all (pre-R7a build)", NO_DECISION_LOG, RELAY_IDLE, 1),
        ("direct dial but the relay forwarded traffic", DIRECT_LOG, RELAY_BUSY, 1),
        ("browse leg first, then a direct dial (R7b)", BROWSE_THEN_DIRECT_LOG, RELAY_BROWSED, 0),
        ("a relay leg AFTER the direct decision", DIRECT_THEN_RELAY_LOG, RELAY_BROWSED, 1),
        ("direct dial, no relay log given", DIRECT_LOG, None, 0),
    ]
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, client_text, relay_text, want) in enumerate(cases):
            hostd = os.path.join(tmp, "c%d_host" % i)
            clid = os.path.join(tmp, "c%d_client" % i)
            os.makedirs(hostd, exist_ok=True)
            os.makedirs(clid, exist_ok=True)
            # host log: a plain UDP host, no relay content
            with open(os.path.join(hostd, "mh_net.log"), "w", encoding="utf-8") as fh:
                fh.write("net: udp HOST listening on :6501 as player 0\n")
            with open(os.path.join(clid, "mh_net.log"), "w", encoding="utf-8") as fh:
                fh.write(client_text)
            argv = [sys.executable, os.path.abspath(__file__)]
            if relay_text is not None:
                rl = os.path.join(tmp, "relay%d.log" % i)
                with open(rl, "w", encoding="utf-8") as fh:
                    fh.write(relay_text)
                argv += ["--relay-log", rl]
            argv += [hostd, clid]
            r = subprocess.run(argv, capture_output=True, text=True)
            got = r.returncode
            print("  %-52s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d\n%s" % (name, got, want, r.stdout))
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print("check_direct_dial --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--relay-log", help="the relay process's log for this run (assert it stayed idle)"
    )
    ap.add_argument("dirs", nargs="+", help="each peer's session run dir (or a lane dir above one)")
    a = ap.parse_args()
    ok, lines = check(a.dirs, relay_log=a.relay_log)
    for ln in lines:
        print(ln)
    print("check_direct_dial: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
