#!/usr/bin/env python3
"""check_rehome_relay -- mp:U60 (HM-M2), the RELAY half of the rehome done_when.

    python tools/check_rehome_relay.py [--net-selftest <net_selftest.exe>] [--keep]

WHAT IT PROVES, against the REAL `mh_relay` (not a stand-in): three relayed peers play in an OLD room;
the OLD HOST is killed (TerminateProcess -- the relay still holds its registration, the 60 s idle
eviction is far away); then the two survivors re-home into a PRE-MINTED room the conductor names:

  * survivor c2 moves its tunnel to the new room FIRST -- no host is there yet, so the relay answers
    `no_host` and the tunnel keeps re-sending its HELLO (the retry is real, and is asserted from the
    relay's log);
  * survivor c1 then becomes the hub: it registers FRESH as ROLE_HOST in that room (a HELLO update
    cannot change a registered peer's role -- see src/relay/src/relay.rs, the unit test of the same
    name), its tunnel turns host-shaped, and its endpoint rehomes (Endpoint::rehome);
  * c2's retry succeeds (`rehomed old_room -> new_room`), its endpoint re-dials c1 through the
    tunnel with its OLD id, and the exactly-once reconcile runs over the relay.

THE ASSERTIONS, from the relay's log and the peers' ledgers:
  - the old room's host handle was NEVER dropped by the relay during the run (still registered);
  - the relay logged >= 1 `no_host` refusal (the pre-registration retry) and exactly one `rehomed`
    old_room -> new_room, and a `peer_registered role=host room=<new_room>`;
  - both survivors report `done` (reconcile reached the hub's TARGET), nothing unrecoverable/aborted,
    no range delivered without queue headroom;
  - per origin, the two survivors' delivered sequences are the same length, gapless and
    duplicate-free; each survivor delivered every frame the OTHER sent; and both hold the SAME
    prefix of the dead host's stream.

The three peers are three PROCESSES of `net_selftest.exe udprehomerelaytest` (the relay tunnel is a
process-wide singleton; src/mh_dll/mh_nettest/udp_rehome_selftest.cpp carries the role code). Files in
a scratch directory are the control channel. `--keep` leaves the directory + logs for inspection.
"""

import argparse
import os
import random
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def free_port_triple():
    """A base such that base, base+1, base+2 are all free UDP ports: the three peers bind them
    (the old host base, c1 base+1, c2 base+2) and the endpoints' sockets must be predictable."""
    for _ in range(200):
        base = random.randrange(20000, 60000)
        socks = []
        try:
            for k in range(3):
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                socks.append(s)
                s.bind(("0.0.0.0", base + k))
            return base
        except OSError:
            continue
        finally:
            for s in socks:
                s.close()
    raise RuntimeError("no three consecutive free UDP ports")


def parse_ledger(path):
    """-> (header dict, {origin: (n, run, last)})"""
    head, origins = {}, {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("role="):
                head = dict(kv.split("=", 1) for kv in line.split())
            elif line.startswith("origin "):
                m = re.match(r"origin (\d+) n=(\d+) run=(\d) last=(\d+)", line)
                if m:
                    origins[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    return head, origins


def analyse_relay_log(text, old_room, new_room):
    """Facts out of the relay's human log. Returns (facts dict, problems list)."""
    text = ANSI_RE.sub("", text)
    problems = []
    facts = {}
    host_handle = None
    for m in re.finditer(r'event="peer_registered"[^\n]*', text):
        line = m.group(0)
        if f"room={old_room} " in line and 'role="host"' in line and host_handle is None:
            h = re.search(r"handle=(\d+)", line)
            host_handle = int(h.group(1)) if h else None
    facts["old_host_handle"] = host_handle
    gone_old_host = [
        m.group(0)
        for m in re.finditer(r'event="peer_gone"[^\n]*', text)
        if host_handle is not None
        and re.search(rf"handle={host_handle}\b", m.group(0))
        and f"room={old_room}" in m.group(0)
    ]
    facts["old_host_dropped"] = gone_old_host
    facts["no_host_refusals"] = len(re.findall(r'event="refused"[^\n]*why="no_host"', text))
    facts["rehomed"] = re.findall(
        rf'event="rehomed"[^\n]*from_room={old_room}[^\n]*to_room={new_room}', text
    )
    facts["new_host_registered"] = re.findall(
        rf'event="peer_registered"[^\n]*room={new_room} role="host"', text
    )
    if host_handle is None:
        problems.append("the relay never logged the old host's registration")
    if gone_old_host:
        problems.append("the relay DROPPED the old host before the re-home: " + gone_old_host[0])
    if facts["no_host_refusals"] < 1:
        problems.append(
            "no `no_host` refusal in the relay log -- the survivor's retry path was not exercised"
        )
    if len(facts["rehomed"]) != 1:
        problems.append(
            f"expected exactly 1 rehomed {old_room}->{new_room}, saw {len(facts['rehomed'])}"
        )
    if not facts["new_host_registered"]:
        problems.append("the relay never logged a ROLE_HOST registration in the new room")
    return facts, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    tmp = os.environ.get("TEMP", tempfile.gettempdir())
    ap.add_argument("--net-selftest", default=os.path.join(tmp, "mh_nettest", "net_selftest.exe"))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument(
        "--base-port",
        type=int,
        default=0,
        help="3 consecutive free UDP ports start here (0 = find)",
    )
    args = ap.parse_args()

    if not os.path.isfile(args.net_selftest):
        print(
            "FAIL: no net_selftest.exe at %s (python tools/run_selftests.py --no-asan builds it)"
            % args.net_selftest
        )
        return 2
    import ui_suite_common as usc  # noqa: E402  the one place that knows where mh_relay.exe is

    relay_exe, why = usc.relay_binary()
    if not relay_exe:
        print("FAIL: %s" % why)
        return 2

    base_port = args.base_port or free_port_triple()
    work = tempfile.mkdtemp(prefix="mh_rehome_relay_")
    relay_log = os.path.join(work, "relay.log")
    rport = free_udp_port()
    old_room = random.randrange(1, 1 << 30)
    new_room = random.randrange(1, 1 << 30)
    procs = []
    fails = []

    def spawn(role):
        log = open(os.path.join(work, role + ".log"), "wb")
        p = subprocess.Popen(
            [
                args.net_selftest,
                "udprehomerelaytest",
                role,
                "127.0.0.1:%d" % rport,
                work,
                str(old_room),
                str(new_room),
                str(base_port),
            ],
            stdout=log,
            stderr=subprocess.STDOUT,
            cwd=work,
        )
        procs.append((role, p, log))
        return p

    def touch(name):
        with open(os.path.join(work, name), "w") as fh:
            fh.write("1")

    def wait_file(name, budget):
        end = time.time() + budget
        while time.time() < end:
            if os.path.exists(os.path.join(work, name)):
                return True
            time.sleep(0.1)
        return False

    rl = open(relay_log, "wb")
    relay = subprocess.Popen(
        [
            relay_exe,
            "--bind",
            "127.0.0.1:%d" % rport,
            "--local",
            "--log",
            "human",
            "--stats-secs",
            "0",
        ],
        stdout=rl,
        stderr=subprocess.STDOUT,
        cwd=REPO,
    )
    try:
        time.sleep(1.0)
        print(
            "relay %s on 127.0.0.1:%d, old room %d, new (pre-minted) room %d"
            % (relay_exe, rport, old_room, new_room)
        )
        h = spawn("h")
        time.sleep(1.0)
        spawn("c1")
        spawn("c2")
        for r in ("h", "c1", "c2"):
            if not wait_file(r + ".up", 60):
                fails.append("peer %s was never admitted in the old room" % r)
        if fails:
            raise RuntimeError("; ".join(fails))
        print(
            "all three admitted; letting traffic run, then killing the OLD HOST (TerminateProcess)"
        )
        time.sleep(3.0)
        h.kill()
        time.sleep(1.0)
        touch("go_c2")  # the survivor asks for the new room BEFORE anyone hosts it
        time.sleep(2.5)
        touch("go_c1")  # ...then the successor registers
        time.sleep(14.0)
        touch("stop_send")
        for r in ("c1", "c2"):
            p = [x for x in procs if x[0] == r][0][1]
            try:
                p.wait(timeout=60)
            except subprocess.TimeoutExpired:
                fails.append("peer %s did not finish" % r)
    except RuntimeError as e:
        fails.append(str(e))
    finally:
        for _r, p, log in procs:
            if p.poll() is None:
                p.kill()
            log.close()
        relay.kill()
        relay.wait()
        rl.close()

    # ---- verdicts ------------------------------------------------------------------------------
    led = {}
    for r in ("c1", "c2"):
        path = os.path.join(work, r + ".ledger")
        if not os.path.exists(path):
            fails.append("no ledger from %s" % r)
        else:
            led[r] = parse_ledger(path)
    for _r, p, _l in procs:
        if _r in ("c1", "c2") and p.returncode not in (0, None):
            fails.append("peer %s exited %s (a rehome call refused?)" % (_r, p.returncode))
    if len(led) == 2:
        (h1, o1), (h2, o2) = led["c1"], led["c2"]
        print("c1:", h1)
        print("c2:", h2)
        for r, hd in (("c1", h1), ("c2", h2)):
            if hd.get("done") != "1":
                fails.append("%s: the reconcile did not complete (done=%s)" % (r, hd.get("done")))
            if hd.get("unrec") != "0" or hd.get("aborted") != "0":
                fails.append("%s: unrecoverable/aborted" % r)
            if hd.get("noroom") != "0":
                fails.append("%s: a range arrived without queue headroom" % r)
        sent1, sent2 = int(h1["sent"]), int(h2["sent"])
        for r, o, other_id, other_sent in (("c1", o1, 2, sent2), ("c2", o2, 1, sent1)):
            for origin, (n, run, _last) in o.items():
                if n and not run:
                    fails.append("%s: origin %d is not a gapless duplicate-free run" % (r, origin))
            n_other = o.get(other_id, (0, 0, 0))[0]
            if n_other != other_sent:
                fails.append(
                    "%s delivered %d of the other survivor's %d frames" % (r, n_other, other_sent)
                )
        n_old1, n_old2 = o1.get(0, (0, 0, 0))[0], o2.get(0, (0, 0, 0))[0]
        print("dead host's stream: c1 holds %d, c2 holds %d" % (n_old1, n_old2))
        if n_old1 != n_old2 or n_old1 == 0:
            fails.append(
                "the survivors do not hold the SAME prefix of the dead host's stream (%d vs %d)"
                % (n_old1, n_old2)
            )
    with open(relay_log, encoding="utf-8", errors="replace") as fh:
        facts, problems = analyse_relay_log(fh.read(), old_room, new_room)
    print(
        "relay: old host handle %s; no_host refusals %d; rehomed lines %d; new-room host registrations %d"
        % (
            facts["old_host_handle"],
            facts["no_host_refusals"],
            len(facts["rehomed"]),
            len(facts["new_host_registered"]),
        )
    )
    fails.extend(problems)

    if fails:
        print("FAIL (logs kept in %s):" % work)
        for f in fails:
            print("  - " + f)
        return 1
    print(
        "PASS -- re-home into a pre-minted room succeeded with the old room's host handle still registered; "
        "sequences identical"
    )
    if args.keep:
        print("kept: %s" % work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
