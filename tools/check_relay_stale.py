#!/usr/bin/env python3
"""check_relay_stale -- mp:R4a: a peer that dialled a relay BEHIND its own protocol level said so.

The done_when: "a client against a relay built with the op table one step behind logs
`relay protocol <theirs> < <ours>` naming both levels and shows the notice in the lobby/browser; a
matching pair logs nothing". The relay_stale_notice scenario stages the older relay with
`--advertise-level 0` (the 4-byte WELCOME a pre-R4a build sends) and captures the browser with the
notice on its status line; this reads the two logs for the half the pixels cannot show:

  * a peer's mh_net.log carries `net: udp relay -- relay protocol <theirs> < <ours>` (udp_relay.cpp,
    registry id net.relay_protocol_older) with theirs < ours -- the one named line, both levels.
  * the SAME log carries `; R4a: relay notice queued for the browser:` (net_discovery.cpp's
    MH_Seam_RelayNotice) -- the text reached mh.dll's carrier, which is what the capture then shows.
  * with --relay-log: the relay's `listening` line says which level it claimed and its final
    `counters` line has hello_level_mismatch >= 1 -- the relay saw the gap from its side too.

`--expect matching` is the negative: NO `relay protocol` line at all in any peer log (a matching
pair logs nothing), which is what every other relay scenario's peers must satisfy.

    python tools/check_relay_stale.py [--expect stale|matching] [--relay-log <log>] <run dir> ...
    python tools/check_relay_stale.py --selftest
"""

import argparse
import os
import re
import sys

# The needles are the registered ones (tools/data/log_formats.json, net.relay_protocol_*).
OLDER_RE = re.compile(r"net: udp relay -- relay protocol (\d+) < (\d+)")
NEWER_RE = re.compile(r"net: udp relay -- relay protocol (\d+) > (\d+)")
QUEUED_NEEDLE = "; R4a: relay notice queued for the browser: "
NET_CONTENT_MARKERS = ("net: udp RELAY mode -- ", "net: udp relay leg UP", "net: udp ")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
LISTENING_LEVEL_RE = re.compile(r'event="listening".*?protocol_level=(\d+)')
COUNTERS_MISMATCH_RE = re.compile(r'event="counters".*?hello_level_mismatch=(\d+)')


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
        if any(m in _read(p) for m in NET_CONTENT_MARKERS):
            return p
    return cands[0]


def check(peer_dirs, expect="stale", relay_log=None):
    out, fails = [], []
    saw_older = saw_queued = False
    any_line = False
    for d in peer_dirs:
        log = find_log(d)
        if not log:
            continue
        body = _read(log)
        m = OLDER_RE.search(body)
        if m:
            any_line = True
            theirs, ours = int(m.group(1)), int(m.group(2))
            if theirs < ours:
                saw_older = True
                out.append("%s: relay protocol %d < %d" % (log, theirs, ours))
            else:
                fails.append(
                    "%s: the `<` line names %d < %d, which is not a lower level"
                    % (log, theirs, ours)
                )
        if NEWER_RE.search(body):
            any_line = True
            out.append("%s: %s" % (log, NEWER_RE.search(body).group(0)))
        if QUEUED_NEEDLE in body:
            saw_queued = True
            i = body.index(QUEUED_NEEDLE) + len(QUEUED_NEEDLE)
            out.append("%s: notice reached mh.dll: %s" % (log, body[i:].split("\n", 1)[0].strip()))
    if expect == "stale":
        if not saw_older:
            fails.append(
                "no `net: udp relay -- relay protocol <theirs> < <ours>` line in any peer log -- the "
                "peer did not report the older relay (R4a done_when: the line naming both levels)"
            )
        if not saw_queued:
            fails.append(
                "no `%s` line in any peer log -- the notice never reached mh.dll's browser carrier "
                "(MH_Seam_RelayNotice), so nothing could have painted" % QUEUED_NEEDLE.strip()
            )
        if relay_log:
            body = ANSI_RE.sub("", _read(relay_log))
            lv = LISTENING_LEVEL_RE.findall(body)
            mm = COUNTERS_MISMATCH_RE.findall(body)
            if not lv:
                fails.append(
                    "relay log %s has no `listening` line with a protocol_level" % relay_log
                )
            else:
                out.append("relay claimed protocol_level=%s" % lv[-1])
            if not mm:
                fails.append(
                    "relay log %s has no `counters` line with hello_level_mismatch" % relay_log
                )
            elif int(mm[-1]) < 1:
                fails.append("relay counted no level mismatch (hello_level_mismatch=%s)" % mm[-1])
            else:
                out.append("relay counted the gap: hello_level_mismatch=%s" % mm[-1])
    else:
        if any_line:
            fails.append(
                "a `relay protocol` line is present but the pair should match (logs nothing)"
            )
        if saw_queued:
            fails.append("a relay notice was queued but the pair should match")
    for f in fails:
        out.append("FAIL: %s" % f)
    return not fails, out


STALE_LOG = (
    "net: udp RELAY mode -- 127.0.0.1:7100 room=0 as client\n"
    "net: udp relay leg UP -- 127.0.0.1:7100 room=0, handle 2\n"
    "net: udp relay -- relay protocol 0 < 1 (the relay at 127.0.0.1:7100 advertised no level: a "
    "pre-R4a build; ...) (mp:R4a)\n"
    "; R4a: relay notice queued for the browser: Relay outdated (protocol 0 < 1)\n"
)
STALE_NO_NOTICE = (
    "net: udp relay leg UP -- 127.0.0.1:7100 room=0, handle 2\n"
    "net: udp relay -- relay protocol 0 < 1 (...) (mp:R4a)\n"
)
MATCHING_LOG = (
    "net: udp RELAY mode -- 127.0.0.1:7100 room=0 as client\n"
    "net: udp relay leg UP -- 127.0.0.1:7100 room=0, handle 2\n"
)
NEWER_LOG = MATCHING_LOG + "net: udp relay -- relay protocol 2 > 1 (the relay is newer) (mp:R4a)\n"
RELAY_STALE = (
    '2026-09-19T00:00:00Z  INFO mh_relay event="listening" addr=0.0.0.0:7100 protocol_level=0 '
    '"mh_relay listening"\n'
    '2026-09-19T00:00:10Z  INFO mh_relay counters event="counters" peers=2 peers_registered=2 '
    'hello_level_mismatch=2 "counters"\n'
)
RELAY_MATCHING = (
    '2026-09-19T00:00:00Z  INFO mh_relay event="listening" addr=0.0.0.0:7100 protocol_level=1 '
    '"mh_relay listening"\n'
    '2026-09-19T00:00:10Z  INFO mh_relay counters event="counters" peers=2 peers_registered=2 '
    'hello_level_mismatch=0 "counters"\n'
)


def selftest():
    import subprocess
    import tempfile

    cases = [
        ("stale: line + notice + relay counted", "stale", STALE_LOG, RELAY_STALE, 0),
        (
            "stale: line but the notice never reached mh.dll",
            "stale",
            STALE_NO_NOTICE,
            RELAY_STALE,
            1,
        ),
        ("stale expected but the pair matched", "stale", MATCHING_LOG, RELAY_MATCHING, 1),
        ("stale: peer said so, relay counted nothing", "stale", STALE_LOG, RELAY_MATCHING, 1),
        ("stale, no relay log given", "stale", STALE_LOG, None, 0),
        ("matching: silent pair", "matching", MATCHING_LOG, None, 0),
        ("matching expected but a stale line is present", "matching", STALE_LOG, None, 1),
        ("matching expected, a newer-relay info line", "matching", NEWER_LOG, None, 1),
    ]
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, expect, client_text, relay_text, want) in enumerate(cases):
            clid = os.path.join(tmp, "c%d_client" % i)
            os.makedirs(clid, exist_ok=True)
            with open(os.path.join(clid, "mh_net.log"), "w", encoding="utf-8") as fh:
                fh.write(client_text)
            argv = [sys.executable, os.path.abspath(__file__), "--expect", expect]
            if relay_text is not None:
                rl = os.path.join(tmp, "relay%d.log" % i)
                with open(rl, "w", encoding="utf-8") as fh:
                    fh.write(relay_text)
                argv += ["--relay-log", rl]
            argv += [clid]
            r = subprocess.run(argv, capture_output=True, text=True)
            got = r.returncode
            print("  %-52s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d\n%s" % (name, got, want, r.stdout))
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print("check_relay_stale --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect", choices=["stale", "matching"], default="stale")
    ap.add_argument("--relay-log", help="the relay process's log for this run")
    ap.add_argument("dirs", nargs="+", help="each peer's session run dir (or a lane dir above one)")
    a = ap.parse_args()
    ok, lines = check(a.dirs, expect=a.expect, relay_log=a.relay_log)
    for ln in lines:
        print(ln)
    print("check_relay_stale: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
