#!/usr/bin/env python3
"""check_relay_rooms -- mp:R6: every relayed HOST in a run registered under a MINTED room, never its
`[net] port`; hosts in one run hold DISTINCT rooms; every relayed CLIENT ended up in one of them; and
the relay refused no registration.

WHY A CHECKER, NOT PIXELS. R6 changed a number nobody can see on a frame: the room a host names in
its relay HELLO. Before R6 it was `[net] port` -- 6501 for every player of the shipped build -- so on
the shared VPS relay the second player to host was refused `room_busy` (measured 2026-09-19: 30
refusals over 16 s while a killed run's slot aged out). A rig scenario walks green either way: one
host per relay, and the port is as good a code as any when nobody else is on it. The claim is only
written down in the logs, so this reads the lines that ARE the claim:

  * every host's mh_net.log carries `net: udp relay -- host room <n>` (udp_relay.cpp start(),
    log_formats.json id `net.relay_host_room`), and the LAST such line -- the room the host
    finally registered under, after any `room_busy` re-mint -- names a room that is not 0, fits
    the 30 bits the browser's sender int routes, and is NOT the port the endpoint bound
    (`net: udp HOST listening on :<port>`);
  * the rooms of every host in the run are pairwise distinct (two hosts on one relay are what
    R6 exists for; with one host the clause is vacuous and says so);
  * every client's log carries a `net: udp relay leg UP -- ... room=<n>` naming one of the hosts'
    rooms -- the directory named it and the client dialled it (mp:R2), so the room it landed in
    is a minted one and not the port it guessed first (its first leg comes up in the directory
    room 0 after that guess is refused; a later one, after a host leaves, may again);
  * with --relay-log: the relay's final `counters` line reads `register_refused=0`, and no
    `refused ... why="room_busy"` event appears at all.

    python tools/check_relay_rooms.py [--relay-log <relay log>] <run dir> [<run dir> ...]
    python tools/check_relay_rooms.py --selftest

`<run dir>` is a peer's session directory holding mh_net.log, or a lane directory above one (the
newest log carrying relay content wins -- check_relay_restart's find_log shape, for the same
reason: test_ui hands over the menu session and the relay lines may be in a sibling). A directory
whose log carries no relay content at all is a FAIL, not a skip: this checker is registered on
relayed scenarios only, and a relayed lane whose log never mentions the relay ran the wrong build.
"""

import argparse
import os
import re
import sys

HOST_ROOM_RE = re.compile(r"net: udp relay -- host room (\d+) \(")
LISTEN_RE = re.compile(r"net: udp HOST listening on :(\d+)")
LEG_UP_RE = re.compile(r"net: udp relay leg UP -- \S+ room=(\d+), handle (\d+)")
MODE_RE = re.compile(r"net: udp RELAY mode -- \S+ room=(\d+) as (host|client)")
ROOM_MAX = 0x3FFFFFFF  # session_info.h SESSION_RELAY_ROOM_MAX -- the 30 bits the browser routes
RELAY_CONTENT_MARKERS = ("net: udp RELAY mode -- ", "net: udp relay leg")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
COUNTERS_REFUSED_RE = re.compile(r'event="counters".*?register_refused=(\d+)')
ROOM_BUSY_RE = re.compile(r'event="refused".*?why="room_busy"')


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
        return own
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


def read_rooms(text):
    """(role, rooms_minted, port, leg_up_rooms) out of one mh_net.log's text. `role` is what the
    LAST `RELAY mode` line says (a lane that browsed as a client and then hosted -- mp:R7 -- is a
    host); None when the log never entered relay mode."""
    modes = MODE_RE.findall(text)
    role = modes[-1][1] if modes else None
    minted = [int(m) for m in HOST_ROOM_RE.findall(text)]
    ports = LISTEN_RE.findall(text)
    port = int(ports[-1]) if ports else None
    ups = [int(u[0]) for u in LEG_UP_RE.findall(text)]
    return role, minted, port, ups


def check_peer(label, log):
    """Returns (ok, role, rooms, lines). `rooms` is [the host's final minted room], or every room a
    client's leg came UP in."""
    try:
        with open(log, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as e:
        return False, None, None, ["%s: cannot read %s: %s" % (label, log, e)]
    role, minted, port, ups = read_rooms(text)
    out = []
    if role is None:
        return (
            False,
            None,
            None,
            [
                "%s: FAIL -- no `net: udp RELAY mode` line in %s: this peer never dialled the relay"
                % (label, log)
            ],
        )
    if role == "host":
        if not minted:
            return (
                False,
                role,
                None,
                [
                    "%s: FAIL -- a relayed host with no `net: udp relay -- host room` line: the room "
                    "was not minted (a build older than mp:R6, or start() never ran as host)"
                    % label
                ],
            )
        room = minted[-1]
        ok = True
        if room == 0 or room > ROOM_MAX:
            ok = False
            out.append(
                "%s: FAIL -- host room %d is not a routable host room (0 is the directory, "
                "and the browser carries 30 bits)" % (label, room)
            )
        if port is not None and room == port:
            ok = False
            out.append(
                "%s: FAIL -- host room %d IS the bound port: the room is still port-derived"
                % (label, room)
            )
        out.append(
            "%s: host room %d (port %s; %d mint line(s)%s)"
            % (
                label,
                room,
                port if port is not None else "?",
                len(minted),
                ", re-minted after room_busy" if len(minted) > 1 else "",
            )
        )
        return ok, role, [room], out
    if not ups:
        return False, role, None, ["%s: FAIL -- a relayed client whose leg never came UP" % label]
    out.append("%s: client, leg UP in room(s) %s" % (label, ", ".join(str(u) for u in ups)))
    return True, role, ups, out


def check_relay(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = ANSI_RE.sub("", fh.read())
    except OSError as e:
        return False, ["relay: cannot read %s: %s" % (path, e)]
    out = []
    ok = True
    busy = len(ROOM_BUSY_RE.findall(text))
    if busy:
        ok = False
        out.append("relay: FAIL -- %d `room_busy` refusal(s) in %s" % (busy, path))
    counters = COUNTERS_REFUSED_RE.findall(text)
    if not counters:
        ok = False
        out.append("relay: FAIL -- no `counters` line in %s (the relay never reported)" % path)
    elif int(counters[-1]) != 0:
        ok = False
        out.append("relay: FAIL -- final counters line has register_refused=%s" % counters[-1])
    else:
        out.append("relay: register_refused=0, no room_busy (%d counters line(s))" % len(counters))
    return ok, out


def run(dirs, relay_log):
    ok = True
    hosts = {}
    clients = {}
    for i, d in enumerate(dirs):
        log = find_log(d)
        # Keyed by position as well as name: every peer's session directory is called the same
        # thing on the rig, and a name-only key would fold two hosts into one.
        label = "peer%d[%s]" % (i, os.path.basename(os.path.normpath(d)))
        if log is None:
            print("%s: no mh_net.log under %s" % (label, d))
            return 2
        pok, role, rooms, lines = check_peer(label, log)
        for ln in lines:
            print("  " + ln)
        ok = ok and pok
        if role == "host" and rooms:
            hosts[label] = rooms[0]
        elif role == "client" and rooms:
            clients[label] = rooms
    # Distinct rooms across hosts.
    if len(hosts) >= 2:
        rooms = list(hosts.values())
        if len(set(rooms)) != len(rooms):
            ok = False
            print("  FAIL -- two hosts hold the same room: %s" % hosts)
        else:
            print("  %d hosts, %d distinct rooms" % (len(hosts), len(set(rooms))))
    elif len(hosts) == 1:
        print("  one host in this run: the distinct-rooms clause is vacuous here")
    # Every client dialled a minted room at some point (the directory named it).
    for label, rooms in clients.items():
        if hosts and not any(r in hosts.values() for r in rooms):
            ok = False
            print(
                "  FAIL -- %s came up only in room(s) %s; no host in this run minted any of them "
                "(%s) -- the client never dialled a listed lobby" % (label, rooms, hosts)
            )
    if relay_log:
        rok, lines = check_relay(relay_log)
        for ln in lines:
            print("  " + ln)
        ok = ok and rok
    print("check_relay_rooms: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---- the negative arm ----------------------------------------------------------------------------

HOST_A = (
    "[00:00:01.000] net: udp RELAY mode -- 127.0.0.1:7100 room=712345678 as host\n"
    "[00:00:01.001] net: udp relay -- host room 712345678 (minted for this lobby; mp:R6)\n"
    "[00:00:01.002] net: udp HOST listening on :6501 as player 0 (K=1) [key set]\n"
    "[00:00:01.500] net: udp relay leg UP -- 127.0.0.1:7100 room=712345678, handle 1\n"
)
HOST_B = HOST_A.replace("712345678", "9876543")
HOST_B_REMINTED = (
    "[00:00:01.000] net: udp RELAY mode -- 127.0.0.1:7100 room=712345678 as host\n"
    "[00:00:01.001] net: udp relay -- host room 712345678 (minted for this lobby; mp:R6)\n"
    "[00:00:01.002] net: udp HOST listening on :6501 as player 0 (K=1) [key set]\n"
    "[00:00:01.400] net: udp relay refused us -- another host already holds this room code\n"
    "[00:00:01.401] net: udp relay -- host room 9876543 (re-minted: room_busy on 712345678, "
    "retry 1/4; mp:R6)\n"
    "[00:00:01.900] net: udp relay leg UP -- 127.0.0.1:7100 room=9876543, handle 2\n"
)
HOST_PORT_ROOM = (
    "[00:00:01.000] net: udp RELAY mode -- 127.0.0.1:7100 room=6501 as host\n"
    "[00:00:01.001] net: udp relay -- host room 6501 (minted for this lobby; mp:R6)\n"
    "[00:00:01.002] net: udp HOST listening on :6501 as player 0 (K=1) [key set]\n"
    "[00:00:01.500] net: udp relay leg UP -- 127.0.0.1:7100 room=6501, handle 1\n"
)
HOST_NO_MINT = (
    "[00:00:01.000] net: udp RELAY mode -- 127.0.0.1:7100 room=6501 as host\n"
    "[00:00:01.002] net: udp HOST listening on :6501 as player 0 (K=1) [key set]\n"
    "[00:00:01.500] net: udp relay leg UP -- 127.0.0.1:7100 room=6501, handle 1\n"
)
CLIENT_A = (
    "[00:00:02.000] net: udp RELAY mode -- 127.0.0.1:7100 room=6501 as client (the transport "
    "dials loopback; the relay carries it)\n"
    "[00:00:02.400] net: udp relay refused us -- no host has claimed this room -- the host must "
    "be on the relay first\n"
    "[00:00:02.401] net: udp relay room 6501 is not hosted -- browsing the relay's session "
    "directory instead (join a listed game to dial its room)\n"
    "[00:00:02.900] net: udp relay leg UP -- 127.0.0.1:7100 room=0, handle 3\n"
    "[00:00:05.000] net: udp RELAY mode -- 127.0.0.1:7100 room=712345678 as client (the transport "
    "dials loopback; the relay carries it)\n"
    "[00:00:05.500] net: udp relay leg UP -- 127.0.0.1:7100 room=712345678, handle 4 (host handle "
    "known)\n"
)
CLIENT_STRANDED = CLIENT_A.splitlines(keepends=True)[:4]
CLIENT_STRANDED = "".join(CLIENT_STRANDED)
NOT_RELAYED = "[00:00:01.002] net: udp HOST listening on :6501 as player 0 (K=1) [key set]\n"
RELAY_CLEAN = (
    '2026-09-19T00:00:00Z  INFO mh_relay listening event="listening" addr=0.0.0.0:7100 strict=false\n'
    '2026-09-19T00:00:10Z  INFO mh_relay counters event="counters" peers=2 rooms=2 leg_rx=40 '
    "register_refused=0 list_requests=3\n"
)
RELAY_BUSY = (
    RELAY_CLEAN.splitlines(keepends=True)[0]
    + '2026-09-19T00:00:01Z  WARN mh_relay datagram refused event="refused" addr=127.0.0.1:5 '
    'why="room_busy"\n' + RELAY_CLEAN.splitlines(keepends=True)[1]
)
RELAY_REFUSED_REGISTER = RELAY_CLEAN.replace("register_refused=0", "register_refused=1")


def selftest():
    import subprocess
    import tempfile

    cases = [
        (
            "two hosts, distinct minted rooms, a client in one, clean relay",
            [HOST_A, HOST_B, CLIENT_A],
            RELAY_CLEAN,
            0,
        ),
        (
            "second host re-minted after room_busy -- its FINAL room counts",
            [HOST_A, HOST_B_REMINTED],
            RELAY_CLEAN,
            0,
        ),
        ("one host only: distinct clause vacuous, still PASS", [HOST_A, CLIENT_A], RELAY_CLEAN, 0),
        ("host room equals the bound port", [HOST_PORT_ROOM], RELAY_CLEAN, 1),
        ("host never minted (pre-R6 build)", [HOST_NO_MINT], RELAY_CLEAN, 1),
        ("two hosts in the SAME room", [HOST_A, HOST_A], RELAY_CLEAN, 1),
        ("client stranded in the directory room", [HOST_A, CLIENT_STRANDED], RELAY_CLEAN, 1),
        ("a lane that never dialled the relay", [HOST_A, NOT_RELAYED], RELAY_CLEAN, 1),
        ("relay saw a room_busy", [HOST_A, HOST_B], RELAY_BUSY, 1),
        ("relay refused a registration", [HOST_A, HOST_B], RELAY_REFUSED_REGISTER, 1),
        ("a peer with no mh_net.log at all", [HOST_A, None], RELAY_CLEAN, 2),
    ]
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, logs, relay, want) in enumerate(cases):
            base = os.path.join(tmp, "c%d" % i)
            dirs = []
            for j, text in enumerate(logs):
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
            print("  %-64s exit %d (want %d)" % (name, r.returncode, want))
            if r.returncode != want:
                fails.append("%s: exit %d, wanted %d\n%s" % (name, r.returncode, want, r.stdout))
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print("check_relay_rooms --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument("--relay-log", default=None, help="the relay process's own log (optional)")
    ap.add_argument("dirs", nargs="+", help="peer run directories (or lanes above them)")
    args = ap.parse_args()
    return run(args.dirs, args.relay_log)


if __name__ == "__main__":
    sys.exit(main())
