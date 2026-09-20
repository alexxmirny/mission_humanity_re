#!/usr/bin/env python3
"""check_codepage_adopt -- mp:F3c: the joiner ADOPTED the host's codepage, or was TOLD it was refused.

WHY A CHECKER, NOT PIXELS. The two F3c scenarios (tools/test_ui.py codepage_adopt / codepage_refused)
walk the same menus as client_join and render the same frames; what they claim is written in the two
peers' mh_net.log and nowhere on screen except one status line. Before F3c the measured failure was
precisely a frame that looked right (the joiner's lobby showed it seated) over a host log that said
REFUSED three times -- so the assertion has to be on the logs, both of them, and cross-peer.

    python tools/check_codepage_adopt.py --expect adopt   <host run dir> <client run dir>
    python tools/check_codepage_adopt.py --expect refused <host run dir> <client run dir>
    python tools/check_codepage_adopt.py --selftest

`--expect adopt` (host pinned 1252, client pinned 1251, adopt on -- the ship default):
  client: `; F3c: adopted the host's input codepage 1252 for this session (ours is 1251)`
  host:   `-> ADMITTED` for that JOIN and NO `REFUSED (input codepage mismatch` at all.
`--expect refused` (client pinned 1251 with [input] codepage_adopt=0):
  host:   `REFUSED (input codepage mismatch; ours 1252, theirs 1251` AND `sent JOIN_REFUSED to player`
  client: `; F3c: JOIN REFUSED by the host: codepage 1252/1251` (the screen-short form, host first)
          AND a session closed with reason `join_refused` (SESSION_END ... reason=join_refused).
  The client must NOT log the adopt line (it declined), and the host must NOT log ADMITTED.

Both peers' lane directories are walked for EVERY mh_net.log (test_ui hands over the newest SESSION
directory; the client's adopt line is written by the JOIN click, a frame BEFORE that click opens the
session directory, so it lands in the run directory above it).
"""

import argparse
import os
import sys

ADOPT_NEEDLE = "; F3c: adopted the host's input codepage "
DECLINE_NEEDLE = "; F3c: host pins input codepage "
ADMIT_NEEDLE = "-> ADMITTED"
REFUSE_HOST_NEEDLE = "REFUSED (input codepage mismatch; ours "
SENT_NEEDLE = "; F3c: sent JOIN_REFUSED to player "
REFUSE_CLIENT_NEEDLE = "; F3c: JOIN REFUSED by the host: "
CLOSE_NEEDLE = "reason=join_refused"


def collect_logs(target):
    """All mh_net.log text under `target` AND its parent (the lane root when test_ui hands over a
    session directory), newest first. A checker reading one file would miss the line written one
    directory up."""
    roots = []
    if os.path.isfile(target):
        return [(target, _read(target))]
    if os.path.isdir(target):
        roots.append(target)
        parent = os.path.dirname(os.path.abspath(target))
        # only climb if the target looks like a run/session directory (it holds an mh_net.log)
        if os.path.isfile(os.path.join(target, "mh_net.log")) and os.path.isdir(parent):
            roots.append(parent)
    found = {}
    for d in roots:
        for root, _dirs, files in os.walk(d):
            if "mh_net.log" in files:
                p = os.path.join(root, "mh_net.log")
                found[os.path.abspath(p)] = _read(p)
    return sorted(found.items(), key=lambda kv: os.path.getmtime(kv[0]), reverse=True)


def _read(p):
    try:
        with open(p, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def verdict(expect, host_text, client_text):
    """Returns (ok, [lines]). Pure: the selftest plants texts here."""
    out = []
    ok = True

    def need(cond, msg):
        nonlocal ok
        out.append(("ok  " if cond else "FAIL") + " " + msg)
        ok = ok and cond

    if expect == "adopt":
        need(ADOPT_NEEDLE in client_text, "client logged the adopt line (%r)" % ADOPT_NEEDLE)
        need(
            "input codepage 1252 for this session (ours is 1251)" in client_text,
            "client adopted 1252 with its own 1251 named",
        )
        need(REFUSE_HOST_NEEDLE not in host_text, "host never refused on codepage")
        need(ADMIT_NEEDLE in host_text, "host ADMITTED the JOIN")
        need(REFUSE_CLIENT_NEEDLE not in client_text, "client was not told of a refusal")
    else:
        need(REFUSE_HOST_NEEDLE in host_text, "host refused on codepage (%r)" % REFUSE_HOST_NEEDLE)
        need(
            "ours 1252, theirs 1251" in host_text,
            "host named both codepages (ours 1252, theirs 1251)",
        )
        need(SENT_NEEDLE in host_text, "host sent JOIN_REFUSED (%r)" % SENT_NEEDLE)
        need(ADMIT_NEEDLE not in host_text, "host never ADMITTED")
        need(
            REFUSE_CLIENT_NEEDLE in client_text,
            "client logged the refusal (%r)" % REFUSE_CLIENT_NEEDLE,
        )
        need(
            "JOIN REFUSED by the host: codepage 1252/1251" in client_text,
            "client's refusal text names both codepages, host first",
        )
        need(CLOSE_NEEDLE in client_text, "client's session closed with reason=join_refused")
        need(ADOPT_NEEDLE not in client_text, "client did not adopt (codepage_adopt=0)")
        need(DECLINE_NEEDLE in client_text, "client logged that it will not switch")
    return ok, out


def selftest():
    fails = 0

    def case(name, expect, host, client, want):
        nonlocal fails
        ok, lines = verdict(expect, host, client)
        if ok != want:
            fails += 1
            print("selftest FAIL: %s -> %s, wanted %s" % (name, ok, want))
            for ln in lines:
                print("    " + ln)

    good_host_adopt = (
        "; S4 JOIN from 1 'client' for 'uitest#00000001' -> ADMITTED (open slot/map gate)\n"
    )
    good_client_adopt = (
        "; F3c: adopted the host's input codepage 1252 for this session (ours is 1251)\n"
    )
    case("adopt: both good", "adopt", good_host_adopt, good_client_adopt, True)
    case("adopt: client never adopted", "adopt", good_host_adopt, "", False)
    case(
        "adopt: host refused anyway",
        "adopt",
        good_host_adopt + "-> REFUSED (input codepage mismatch; ours 1252, theirs 1251 -- x)\n",
        good_client_adopt,
        False,
    )
    good_host_ref = (
        "; S4 JOIN from 1 'client' for 'uitest#1' -> REFUSED (input codepage mismatch; ours 1252, theirs 1251 -- x)\n"
        "; F3c: sent JOIN_REFUSED to player 1: codepage 1252/1251\n"
    )
    good_client_ref = (
        "; F3c: host pins input codepage 1252, ours is 1251 and this peer will not switch\n"
        "; F3c: JOIN REFUSED by the host: codepage 1252/1251 -> leaving\n"
        "; SESSION_END match_id=x reason=join_refused\n"
    )
    case("refused: both good", "refused", good_host_ref, good_client_ref, True)
    case(
        "refused: host never sent the reply",
        "refused",
        good_host_ref.splitlines()[0] + "\n",
        good_client_ref,
        False,
    )
    case(
        "refused: client never told",
        "refused",
        good_host_ref,
        "; F3c: host pins input codepage 1252, ours is 1251 and this peer will not switch\n",
        False,
    )
    case(
        "refused: client stayed seated (no join_refused close)",
        "refused",
        good_host_ref,
        good_client_ref.replace("reason=join_refused", "reason=leave"),
        False,
    )
    case(
        "refused: host admitted", "refused", good_host_ref + "-> ADMITTED\n", good_client_ref, False
    )
    print("check_codepage_adopt --selftest: %s" % ("PASS" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--expect", choices=("adopt", "refused"), default="adopt")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*", help="<host run dir> <client run dir>")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if len(a.dirs) != 2:
        print(
            "check_codepage_adopt: need exactly two run directories (host, client); got %d"
            % len(a.dirs)
        )
        return 2
    texts = []
    for role, d in zip(("host", "client"), a.dirs):
        logs = collect_logs(d)
        if not logs:
            print("check_codepage_adopt: no mh_net.log under %s (%s)" % (d, role))
            return 2
        print("  %s: %d mh_net.log(s) under %s" % (role, len(logs), d))
        texts.append("\n".join(t for _p, t in logs))
    ok, lines = verdict(a.expect, texts[0], texts[1])
    for ln in lines:
        print("  " + ln)
    print("check_codepage_adopt (--expect %s): %s" % (a.expect, "PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
