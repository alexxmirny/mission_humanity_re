#!/usr/bin/env python3
"""check_relay_leg -- mp:R7a: a peer that joined from the FIRST browser really went THROUGH the relay.

The positive control for R7a's done_when clause (a): "joining from the first browser goes through the
relay (relay counters show the session; `udp path` lines present)". relay_browse_local walks the first
browser (the relay-discovery screen) and joins a relay-listed game; this asserts the CLIENT's log
carries the two lines that only a relayed dial writes -- so the scenario cannot silently regress into a
direct connection (which is exactly the OTHER half of R7a, guarded by check_direct_dial). Its mirror
image: check_direct_dial fails if these lines are PRESENT; this fails if they are ABSENT.

  * `net: udp relay leg UP` -- the client's leg registered with the relay (its HELLO was answered).
  * a `net: udp path ...` line -- `RELAY (forced)` on a force_relay lane, or `DIRECT`/`RELAY` on a
    punching one. Either way the relay was armed for this connection; a direct dial writes none.

  With --relay-log the relay's own `counters` line must show it carried the session (peers_registered
  >= 1), so "the relay counters show the session" is checked at the relay too, not only inferred from
  the client.

    python tools/check_relay_leg.py [--relay-log <relay log>] <run dir> [<run dir> ...]
    python tools/check_relay_leg.py --selftest
"""

import argparse
import os
import re
import sys

LEG_UP = "net: udp relay leg UP"
PATH_RE = re.compile(r"net: udp path (DIRECT|RELAY)")
RELAY_CONTENT = (LEG_UP, "net: udp RELAY mode -- ", "net: udp path ")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
COUNTERS_REG_RE = re.compile(r'event="counters".*?peers_registered=(\d+)')

# mp:R2c -- the FIRST browser (this scenario's whole walk: "the IP window never visited") is the
# no-typed-address case R2c fixed: before it, every such join guessed `[net] port` as a room, ate a
# guaranteed `no_host` refusal, and a guaranteed 4-second peer-handshake timeout, all before the
# directory ever got a chance to answer. None of that is specific to THIS scenario's own clause
# above -- a regression could reintroduce the wasted round trip and this checker would still
# eventually see a leg UP and a path line -- so it needs its own assertion.
NO_HOST_REFUSED = "net: udp relay refused us -- no host has claimed this room"
HANDSHAKE_TIMEOUT = "net: udp handshake FAILED"
RELAY_NO_HOST_RE = re.compile(r'event="refused".*?why="no_host"')


def _read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def find_log(target):
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
        if any(m in _read(p) for m in RELAY_CONTENT):
            return p
    return cands[0]


def check(peer_dirs, relay_log=None):
    out, fails = [], []
    # At least one peer's log must show the relay leg + a udp path line (the client that joined).
    saw_leg = saw_path = False
    no_host_hits = []
    timeout_hits = []
    for d in peer_dirs:
        log = find_log(d)
        if not log:
            continue
        body = _read(log)
        if LEG_UP in body:
            saw_leg = True
            out.append("relay leg UP in %s" % log)
        if PATH_RE.search(body):
            saw_path = True
            out.append("udp path line in %s: %s" % (log, PATH_RE.search(body).group(0)))
        # mp:R2c -- this scenario has no typed address, so its first dial must go straight to the
        # directory: no guessed-room refusal, no peer-handshake timeout against a room nobody named.
        if NO_HOST_REFUSED in body:
            no_host_hits.append(log)
        if HANDSHAKE_TIMEOUT in body:
            timeout_hits.append(log)
    if no_host_hits:
        fails.append(
            "`%s` in %s -- the FIRST dial (no typed address) guessed a room and was refused; "
            "mp:R2c: a dial with no typed address and no directory pick must register directly "
            "under the directory room, never guess `[net] port` as a room"
            % (NO_HOST_REFUSED, no_host_hits)
        )
    if timeout_hits:
        fails.append(
            "`%s` in %s -- the endpoint armed a peer handshake against a room nobody dialled; "
            "mp:R2c: a browse-only dial must not arm the handshake at all (Config::browse_only)"
            % (HANDSHAKE_TIMEOUT, timeout_hits)
        )
    if not no_host_hits and not timeout_hits:
        out.append("mp:R2c: no room-guess refusal and no handshake timeout in any peer log")
    if not saw_leg:
        fails.append(
            "no `%s` in any peer log -- the first-browser join did NOT go through the relay (R7a "
            "clause (a): a relayed join must show the leg coming up)" % LEG_UP
        )
    if not saw_path:
        fails.append(
            "no `net: udp path` line in any peer log -- the relay was never armed for this connection "
            "(R7a clause (a): `udp path` lines present)"
        )
    if relay_log:
        body = ANSI_RE.sub("", _read(relay_log))
        regs = COUNTERS_REG_RE.findall(body)
        if not regs:
            fails.append("relay log %s has no `counters` line" % relay_log)
        elif int(regs[-1]) < 1:
            fails.append(
                "relay registered no peers (peers_registered=%s) -- the session did not go through it"
                % regs[-1]
            )
        else:
            out.append("relay carried the session: peers_registered=%s" % regs[-1])
        # mp:R2c -- "also in the relay's own log as `refused why=no_host` from both players" (the
        # 2026-09-20 field report). The relay is the other witness to the same claim: it must never
        # have been ASKED to referee a room this peer had no reason to name.
        no_host_events = RELAY_NO_HOST_RE.findall(body)
        if no_host_events:
            fails.append(
                "relay log %s shows %d `no_host` refusal event(s) -- mp:R2c: a no-typed-address "
                "dial must never ask the relay about a room" % (relay_log, len(no_host_events))
            )
        else:
            out.append("relay log shows no `no_host` refusal")
    for f in fails:
        out.append("FAIL: %s" % f)
    return not fails, out


DIRECT_DIAL_LOG = (
    "; R7a: client dial is DIRECT\nnet: CLIENT connected to 192.168.0.61:6501 as player 1\n"
)
RELAYED_LOG = (
    "net: udp RELAY mode -- 127.0.0.1:7100 room=12345 as client\n"
    "net: udp relay leg UP -- 127.0.0.1:7100 room=12345, handle 3\n"
    "net: udp path RELAY (forced) -- [net] force_relay=1, so no candidates are published\n"
)
RELAYED_NO_PATH = "net: udp relay leg UP -- 127.0.0.1:7100 room=12345, handle 3\n"
RELAY_BUSY = (
    '2026-09-19T00:00:10Z  INFO mh_relay counters event="counters" peers=2 peers_registered=2 '
    "\"counters\"\n"
)
RELAY_IDLE = (
    '2026-09-19T00:00:10Z  INFO mh_relay counters event="counters" peers=0 peers_registered=0 '
    "\"counters\"\n"
)
# mp:R2c -- the pre-fix shape (the 2026-09-20 field report, and this checker's own docstring): a
# guessed-room refusal followed by the fallback and then the real dial. Still has leg UP + a path
# line (so the OLDER clauses above stayed green on it), which is exactly why it needs its own
# assertion rather than relying on those.
RELAYED_LOG_R2C_REGRESSION = (
    "net: udp RELAY mode -- 127.0.0.1:7100 room=6501 as client\n"
    "net: udp relay refused us -- no host has claimed this room -- the host must be on the relay "
    "first\n"
    "net: udp relay room 6501 is not hosted -- browsing the relay's session directory instead "
    "(join a listed game to dial its room)\n"
    "net: udp handshake FAILED -- no answer from the host within 4000 ms. Either nothing is "
    "listening behind that address, the host is running transport=tcp while this peer is on udp, "
    "or the two of you hold different mh_key.txt.\n"
    "net: udp relay leg UP -- 127.0.0.1:7100 room=12345, handle 3\n"
    "net: udp path RELAY (forced) -- [net] force_relay=1, so no candidates are published\n"
)
RELAY_BUSY_WITH_NO_HOST = (
    RELAY_BUSY.rstrip("\n")
    + '\n2026-09-20T00:00:00Z  WARN mh_relay datagram refused event="refused" addr=127.0.0.1:5 '
    'why="no_host"\n'
)


def selftest():
    import subprocess
    import tempfile

    cases = [
        ("a real relayed first-browser join", RELAYED_LOG, RELAY_BUSY, 0),
        ("relayed but no udp path line", RELAYED_NO_PATH, RELAY_BUSY, 1),
        ("a direct dial (no relay contact)", DIRECT_DIAL_LOG, RELAY_IDLE, 1),
        ("relayed client but relay registered nobody", RELAYED_LOG, RELAY_IDLE, 1),
        ("relayed, no relay log given", RELAYED_LOG, None, 0),
        (
            "mp:R2c regression -- guessed-room refusal + handshake timeout before the real dial",
            RELAYED_LOG_R2C_REGRESSION,
            RELAY_BUSY,
            1,
        ),
        (
            "mp:R2c regression seen only in the relay's own log",
            RELAYED_LOG,
            RELAY_BUSY_WITH_NO_HOST,
            1,
        ),
    ]
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, client_text, relay_text, want) in enumerate(cases):
            clid = os.path.join(tmp, "c%d_client" % i)
            os.makedirs(clid, exist_ok=True)
            with open(os.path.join(clid, "mh_net.log"), "w", encoding="utf-8") as fh:
                fh.write(client_text)
            argv = [sys.executable, os.path.abspath(__file__)]
            if relay_text is not None:
                rl = os.path.join(tmp, "relay%d.log" % i)
                with open(rl, "w", encoding="utf-8") as fh:
                    fh.write(relay_text)
                argv += ["--relay-log", rl]
            argv += [clid]
            r = subprocess.run(argv, capture_output=True, text=True)
            got = r.returncode
            print("  %-48s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d\n%s" % (name, got, want, r.stdout))
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print("check_relay_leg --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument("--relay-log", help="the relay process's log for this run")
    ap.add_argument("dirs", nargs="+", help="each peer's session run dir (or a lane dir above one)")
    a = ap.parse_args()
    ok, lines = check(a.dirs, relay_log=a.relay_log)
    for ln in lines:
        print(ln)
    print("check_relay_leg: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
