"""check_content_refusal.py -- DATAEXT1: judge a content-digest JOIN refusal from both peers' logs.

Companion of check_codepage_adopt.py (same log-only verdict shape, same reasons): what these rows
claim is written in mh_net.log and only echoed in one browser status line, so the LOGS are the
verdict and the capture is the witness.

    python tools/check_content_refusal.py --expect changed <host run dir> <client run dir>
    python tools/check_content_refusal.py --expect match   <host run dir> <client run dir>
    python tools/check_content_refusal.py --selftest

`--expect changed` (the two peers enable the same mod id but hold different payload bytes -- a
one ASCII text letter changed, or the client missing the host's active mod entirely and hashing its stock snapshot):
  host:   `-> REFUSED (content digest mismatch; expected <64 hex>, local <64 hex>` with the two
          digests UNEQUAL, AND `sent JOIN_REFUSED to player` (the F3c bounce), AND no `-> ADMITTED`.
  client: `; F3c: JOIN REFUSED by the host: content mismatch` (the localized row's English lead-in)
          AND a session closed with `reason=join_refused`.
`--expect match` (the same mod id and byte-identical payloads): the host ADMITTED the JOIN and
neither side logged a content refusal.
"""

import argparse
import os
import re
import sys

import _rundir

HOST_REFUSED = "-> REFUSED (content digest mismatch; expected "
HEX_PAIR_RE = re.compile(r"expected ([0-9a-f]{64}), local ([0-9a-f]{64})")
HOST_SENT = "; F3c: sent JOIN_REFUSED to player "
HOST_ADMITTED = "-> ADMITTED"
CLIENT_TOLD = "; F3c: JOIN REFUSED by the host: content mismatch"
CLIENT_CLOSE = "reason=join_refused"


def collect_logs(target, filename="mh_net.log"):
    """Read only this boot and sessions whose session.json names that boot."""
    texts = []
    for directory in _rundir.process_and_sessions(target):
        path = os.path.join(directory, filename)
        if os.path.isfile(path):
            with open(path, encoding="utf-8", errors="replace") as fh:
                texts.append(fh.read())
    return "\n".join(texts)


def verdict(expect, host_text, client_text):
    """Returns (ok, [lines]). Pure: the selftest plants texts here."""
    out = []
    ok = True

    def need(cond, msg):
        nonlocal ok
        out.append(("ok  " if cond else "FAIL") + " " + msg)
        ok = ok and cond

    if expect == "match":
        need(CLIENT_TOLD not in client_text, "client was never told of a content refusal")
        need(HOST_REFUSED not in host_text, "host never refused on content")
        need(HOST_ADMITTED in host_text, "host ADMITTED the JOIN")
        need(CLIENT_CLOSE not in client_text, "client's session did not close as join_refused")
        return ok, out

    # changed: a real digest disagreement (same mod id, different bytes -- or the missing-mod side).
    m = HEX_PAIR_RE.search(host_text)
    need(m is not None, "host named BOTH 64-hex digests (expected/local)")
    if m:
        need(
            m.group(1) != m.group(2),
            "the two digests actually DIFFER (%s.. vs %s..)" % (m.group(1)[:8], m.group(2)[:8]),
        )
    need(HOST_SENT in host_text, "host sent JOIN_REFUSED (%r)" % HOST_SENT)
    need(HOST_ADMITTED not in host_text, "host never ADMITTED")
    need(CLIENT_TOLD in client_text, "client was told ('%s')" % CLIENT_TOLD)
    need(CLIENT_CLOSE in client_text, "client's session closed with reason=join_refused")
    return ok, out


def _selftest():
    fails = 0

    def case(name, expect, host, client, want):
        nonlocal fails
        ok, _lines = verdict(expect, host, client)
        if ok != want:
            print("FAIL %s" % name)
            fails += 1
        else:
            print("ok   %s" % name)

    hex_a = "%064x" % 0x0123456789ABCDEF
    hex_b = "%064x" % 0xFEDCBA9876543210
    good_host = (
        "; S4 JOIN from 1 'client' for 'uitest#1' -> REFUSED (content digest mismatch; expected %s, local %s)\n"
        "; F3c: sent JOIN_REFUSED to player 1: content mismatch\n" % (hex_a, hex_b)
    )
    good_client = (
        "; F3c: JOIN REFUSED by the host: content mismatch -> leaving\n"
        "; SESSION_END match_id=x reason=join_refused\n"
    )
    case("changed: both good", "changed", good_host, good_client, True)
    case(
        "changed: digests equal would be a fake",
        "changed",
        good_host.replace(hex_b, hex_a),
        good_client,
        False,
    )
    case("changed: host admitted", "changed", good_host + "-> ADMITTED\n", good_client, False)
    case(
        "changed: host never sent the reply",
        "changed",
        good_host.splitlines()[0] + "\n",
        good_client,
        False,
    )
    case(
        "changed: client never told",
        "changed",
        good_host,
        good_client.splitlines()[1] + "\n",
        False,
    )
    case(
        "changed: client stayed seated",
        "changed",
        good_host,
        "; F3c: JOIN REFUSED by the host: content mismatch -> leaving\n"
        "; SESSION_END match_id=x reason=leave\n",
        False,
    )
    case(
        "match: both clean",
        "match",
        "-> ADMITTED\n",
        "; MP-LANG: chat encoding 65001 agreed\n",
        True,
    )
    case("match: host refused anyway", "match", good_host, good_client, False)
    case("match: client told anyway", "match", "-> ADMITTED\n", good_client, False)
    print("check_content_refusal --selftest: %s" % ("PASS" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--expect", choices=("changed", "missing", "match"), default="changed")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*", help="<host run dir> <client run dir>")
    a = ap.parse_args(argv)
    if a.selftest:
        return _selftest()
    if len(a.dirs) != 2:
        ap.error("need <host run dir> <client run dir>")
    ok, lines = verdict(a.expect, collect_logs(a.dirs[0]), collect_logs(a.dirs[1]))
    for side, directory in enumerate(a.dirs):
        video = collect_logs(directory, "mh_video.log")
        armed = "; [mods] overlay armed: ids=fixture" in video
        expected = side == 0 or a.expect != "missing"
        lines.append(
            ("ok  " if armed == expected else "FAIL")
            + " %s fixture mounted=%s, expected=%s"
            % ("host" if side == 0 else "client", armed, expected)
        )
        ok = ok and armed == expected
    for ln in lines:
        print("  " + ln)
    print("check_content_refusal (--expect %s): %s" % (a.expect, "PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
