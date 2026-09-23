#!/usr/bin/env python3
"""check_browser_rows -- mp:R2b: a browsing client LISTED every relay-hosted lobby as its own row, and
each JOIN it clicked dialled the room of the row it clicked -- not the row the auto-dial had landed on.

WHY A CHECKER, NOT PIXELS. The browser_two_rows scenario's frames show two rows and two lobbies, but
a frame cannot show WHICH ROOM a join dialled: a build that listed two rows and always joined the
first would still render "a lobby" after the click. The claim is written down in the client's
mh_net.log, so this reads the lines that ARE the claim:

  * the client's log carries one `; R2 relay directory: <id> ... room=<n> -> listed` line per lobby
    (net_discovery.cpp relay_rows_drain, log_formats.json id `net.r2_directory_row`); at least
    --rows DISTINCT rooms must be listed, and they are numbered in FIRST-LISTED order (1-based),
    which is the directory SLOT order the browser renders (a slot is allocated lowest-free);
  * every `; R2b join: row <r> (slot <s>) <id> room=<n> -> ...` line (net_discovery.cpp
    on_join_connect, id `net.r2b_join_row`) names a room that IS one of the listed rooms, and the
    k-th such line names the room of listed lobby --joins[k] (so `--joins 2,1` says: the first
    click joined the second listed lobby, the second click the first);
  * with the HOST lanes handed over too: every listed room was MINTED by one of the hosts in the run
    (`net: udp relay -- host room <n>`, id `net.relay_host_room`), so the rows were the run's own
    lobbies and not a stale directory.

    python tools/check_browser_rows.py --rows 2 --joins 2,1 <run dir> [<run dir> ...]
    python tools/check_browser_rows.py --selftest

`<run dir>` is a peer's PROCESS ("menu") run directory (test_ui hands that over) or a lane directory
above one. A peer's lines are split across its process log and the SESSION directories each JOIN
opens (SES1) -- the first `R2b join` line is written before its session exists, the re-send that
opens the session is inside it -- so every mh_net.log of the same run (the given directory and its
later-stamped siblings under the same logs/ folder) is read as ONE timeline, in stamp order. The
browsing client is the peer whose timeline carries `R2b join` lines; a run with none is a FAIL (the
scenario clicked Join twice, so a log without the line ran an older build).
"""

import argparse
import os
import re
import sys

LISTED_RE = re.compile(
    r"; R2 relay directory: (.+?) map=(.*?) players=(\d+)/(\d+) room=(\d+) -> listed"
)
JOIN_RE = re.compile(r"; R2b join: row (\d+) \(slot (\d+)\) (.+?) room=(\d+) -> (.*)")
HOST_ROOM_RE = re.compile(r"net: udp relay -- host room (\d+) \(")
# The needles lint_log_formats.py matches against the registry (verbatim string literals):
NEEDLE_LISTED = "; R2 relay directory: "
NEEDLE_JOIN = "; R2b join: "
NEEDLE_HOST_ROOM = "net: udp relay -- host room "


def run_logs(target):
    """Every mh_net.log of the run `target` belongs to, oldest first: the directory itself and its
    later-stamped siblings (the session directories a JOIN opens). A lane directory (no mh_net.log
    of its own) yields every log under its logs/ folder."""
    target = os.path.abspath(target)
    if os.path.isfile(target):
        return [target]
    own = os.path.join(target, "mh_net.log")
    if os.path.isfile(own):
        parent = os.path.dirname(target)
        stamp = os.path.basename(target)
        sibs = []
        for d in sorted(os.listdir(parent)):
            p = os.path.join(parent, d, "mh_net.log")
            if os.path.isfile(p) and d >= stamp:
                sibs.append(p)
        return sibs or [own]
    logs = []
    for root, _dirs, files in os.walk(target):
        if "mh_net.log" in files:
            logs.append(os.path.join(root, "mh_net.log"))
    return sorted(logs)


def read_timeline(target):
    parts = []
    for p in run_logs(target):
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as fh:
                parts.append(fh.read())
        except OSError:
            pass
    return "\n".join(parts)


def parse_peer(text):
    """(listed, joins, host_rooms): listed = [(lobby_id, room)] in first-listed order, one per
    DISTINCT room; joins = [(row, slot, lobby_id, room, verb)] in log order; host_rooms = the rooms
    this peer minted as a host (empty for a client)."""
    listed, seen = [], set()
    for m in LISTED_RE.finditer(text):
        room = int(m.group(5))
        if room in seen:
            continue
        seen.add(room)
        listed.append((m.group(1), room))
    joins = [
        (int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4)), m.group(5).strip())
        for m in JOIN_RE.finditer(text)
    ]
    hosts = [int(r) for r in HOST_ROOM_RE.findall(text)]
    return listed, joins, hosts


def run(dirs, rows, joins_want):
    ok = True
    host_rooms = set()
    clients = []
    for i, d in enumerate(dirs):
        label = "peer%d[%s]" % (i, os.path.basename(os.path.normpath(d)))
        text = read_timeline(d)
        if not text:
            print("%s: no mh_net.log under %s" % (label, d))
            return 2
        listed, joins, hosts = parse_peer(text)
        if hosts:
            host_rooms.add(hosts[-1])  # the FINAL minted room (a room_busy re-mint replaces it)
            print("  %s: host, room %d" % (label, hosts[-1]))
        if joins:
            clients.append((label, listed, joins))
    if not clients:
        print(
            "  FAIL -- no peer's log carries a `; R2b join:` line: nothing joined through the "
            "browser (an older build, or the click never happened)"
        )
        print("check_browser_rows: FAIL")
        return 1
    for label, listed, joins in clients:
        rooms = [r for _id, r in listed]
        print(
            "  %s: %d lobby(ies) listed: %s"
            % (
                label,
                len(listed),
                ", ".join("%d:%s room=%d" % (k + 1, i, r) for k, (i, r) in enumerate(listed)),
            )
        )
        if len(listed) < rows:
            ok = False
            print(
                "  FAIL -- %s listed %d distinct lobby(ies), the scenario hosts %d"
                % (label, len(listed), rows)
            )
        if host_rooms:
            stale = [r for r in rooms if r not in host_rooms]
            if stale:
                ok = False
                print(
                    "  FAIL -- %s listed room(s) %s that no host in this run minted (%s): a stale "
                    "directory, not this run's lobbies" % (label, stale, sorted(host_rooms))
                )
            missing = [r for r in sorted(host_rooms) if r not in rooms]
            if missing:
                ok = False
                print(
                    "  FAIL -- host room(s) %s were never listed on %s: a hosted lobby the browser "
                    "did not show" % (missing, label)
                )
        for k, (row, slot, lid, room, verb) in enumerate(joins):
            print(
                "  %s: join #%d -> row %d (slot %d) %s room=%d: %s"
                % (label, k + 1, row, slot, lid, room, verb)
            )
            if room not in rooms:
                ok = False
                print("  FAIL -- join #%d dialled room %d, which was never listed" % (k + 1, room))
                continue
            if k < len(joins_want):
                want_pos = joins_want[k]
                if want_pos < 1 or want_pos > len(rooms):
                    ok = False
                    print(
                        "  FAIL -- --joins names listed lobby %d but only %d were listed"
                        % (want_pos, len(rooms))
                    )
                elif rooms[want_pos - 1] != room:
                    ok = False
                    print(
                        "  FAIL -- join #%d was meant to be listed lobby %d (room %d) but dialled "
                        "room %d (listed lobby %d): the JOIN did not follow the clicked row"
                        % (k + 1, want_pos, rooms[want_pos - 1], room, rooms.index(room) + 1)
                    )
        if len(joins) < len(joins_want):
            ok = False
            print(
                "  FAIL -- %s made %d join(s); --joins expects %d"
                % (label, len(joins), len(joins_want))
            )
    print("check_browser_rows: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---- the negative arm ----------------------------------------------------------------------------

HOST_A = (
    "[00:00:01.000] net: udp RELAY mode -- 127.0.0.1:7100 room=712345678 as host\n"
    "[00:00:01.001] net: udp relay -- host room 712345678 (minted for this lobby; mp:R6)\n"
)
HOST_B = HOST_A.replace("712345678", "9876543")
CLIENT_OK = (
    "[00:00:05.000] ; R2 relay directory: uitest#0A0B0C0D map=blue monday players=1/8 room=712345678 -> listed\n"
    "[00:00:09.000] ; R2 relay directory: bravo#1A1B1C1D map=blue monday players=1/8 room=9876543 -> listed\n"
    "[00:00:11.000] ; R2 relay directory: uitest#0A0B0C0D map=blue monday players=1/8 room=712345678 -> listed\n"
    "[00:00:12.000] ; R2b join: row 1 (slot 1) bravo#1A1B1C1D room=9876543 -> re-dialling (linked to room 712345678), JOIN re-sent when it lands\n"
    "[00:00:20.000] ; R2b join: row 0 (slot 0) uitest#0A0B0C0D room=712345678 -> re-dialling (linked to room 9876543), JOIN re-sent when it lands\n"
)
CLIENT_ONE_ROW = (
    "[00:00:05.000] ; R2 relay directory: uitest#0A0B0C0D map=blue monday players=1/8 room=712345678 -> listed\n"
    "[00:00:12.000] ; R2b join: row 0 (slot 0) uitest#0A0B0C0D room=712345678 -> linked, JOIN sent now\n"
)
CLIENT_WRONG_ROOM = CLIENT_OK.replace(
    "; R2b join: row 1 (slot 1) bravo#1A1B1C1D room=9876543",
    "; R2b join: row 1 (slot 1) bravo#1A1B1C1D room=712345678",
)
CLIENT_UNLISTED_ROOM = CLIENT_OK.replace("room=9876543 -> re-dialling", "room=5555 -> re-dialling")
CLIENT_NO_JOIN = "\n".join(ln for ln in CLIENT_OK.splitlines() if "R2b join" not in ln) + "\n"
CLIENT_ONE_JOIN = "\n".join(CLIENT_OK.splitlines()[:4]) + "\n"
CLIENT_STALE_ROW = CLIENT_OK.replace("room=9876543", "room=424242")


def selftest():
    import subprocess
    import tempfile

    cases = [
        ("two hosts, two rows, joins 2 then 1", [HOST_A, HOST_B, CLIENT_OK], "2", "2,1", 0),
        (
            "no host lanes handed over: the client's own lines still decide",
            [CLIENT_OK],
            "2",
            "2,1",
            0,
        ),
        ("one row listed where two were hosted", [HOST_A, HOST_B, CLIENT_ONE_ROW], "2", "2,1", 1),
        (
            "the first join dialled the FIRST lobby's room (the pre-R2b shape)",
            [HOST_A, HOST_B, CLIENT_WRONG_ROOM],
            "2",
            "2,1",
            1,
        ),
        (
            "a join dialled a room never listed",
            [HOST_A, HOST_B, CLIENT_UNLISTED_ROOM],
            "2",
            "2,1",
            1,
        ),
        ("no R2b join line at all", [HOST_A, HOST_B, CLIENT_NO_JOIN], "2", "2,1", 1),
        ("only one of the two expected joins", [HOST_A, HOST_B, CLIENT_ONE_JOIN], "2", "2,1", 1),
        (
            "a listed room no host in the run minted",
            [HOST_A, HOST_B, CLIENT_STALE_ROW],
            "2",
            "2,1",
            1,
        ),
        ("a peer with no mh_net.log at all", [HOST_A, None, CLIENT_OK], "2", "2,1", 2),
    ]
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, logs, rows, joins, want) in enumerate(cases):
            base = os.path.join(tmp, "c%d" % i)
            dirs = []
            for j, text in enumerate(logs):
                d = os.path.join(base, "lane%d" % j, "logs", "20260922T000000Z_menu_solo")
                os.makedirs(d, exist_ok=True)
                if text is not None:
                    # split the client's lines the way SES1 does: the second join's re-send in a
                    # later SESSION directory, to prove the timeline join reads both
                    lines = text.splitlines(keepends=True)
                    if len(lines) > 4 and "R2b join" in text:
                        with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
                            fh.write("".join(lines[:4]))
                        d2 = os.path.join(
                            base, "lane%d" % j, "logs", "20260922T000010Z_deadbeef_2_client"
                        )
                        os.makedirs(d2, exist_ok=True)
                        with open(os.path.join(d2, "mh_net.log"), "w", encoding="utf-8") as fh:
                            fh.write("".join(lines[4:]))
                    else:
                        with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
                            fh.write(text)
                dirs.append(d)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__), "--rows", rows, "--joins", joins]
                + dirs,
                capture_output=True,
                text=True,
            )
            print("  %-64s exit %d (want %d)" % (name, r.returncode, want))
            if r.returncode != want:
                fails.append("%s: exit %d, wanted %d\n%s" % (name, r.returncode, want, r.stdout))
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print("check_browser_rows --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--rows", type=int, default=2, help="distinct lobbies the browser must have listed"
    )
    ap.add_argument(
        "--joins",
        default="",
        help="comma-separated 1-based LISTED-lobby positions the successive joins must have dialled "
        "(e.g. 2,1: the first click joined the second listed lobby, the second click the first)",
    )
    ap.add_argument("dirs", nargs="+", help="peer run directories (or lanes above them)")
    args = ap.parse_args()
    joins = [int(x) for x in args.joins.split(",") if x.strip()]
    return run(args.dirs, args.rows, joins)


if __name__ == "__main__":
    sys.exit(main())
