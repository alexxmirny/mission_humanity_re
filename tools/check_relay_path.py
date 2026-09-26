#!/usr/bin/env python3
"""check_relay_path -- mp:R3a: a punching relay pair really went DIRECT, not just "the scenario
passed".

WHY A CHECKER, NOT PIXELS. `relay_punch` (tools/test_ui.py) is registered and walks green, but its
actual claim -- mp:R3's mid-handshake path switch does not break the link -- was, until this,
provable only by re-reading R3's own determinism-run residue: nothing in the capture grammar can see
a log line, so a promotion that silently stopped happening (a regression in udp_punch.h's candidate
exchange, or the pair simply staying relayed for the whole run) would leave the scenario exactly as
green as a real promotion. The one place the claim is actually written down is `net: udp path DIRECT
-- peer N via ADDR after MS ms of punching` in mh_net.log (src/mh_dll/mh_net_udp/udp_relay.cpp,
`punch_log_transition`, log_formats.json id `net.path_direct`) -- so this reads THAT line instead of
inferring the promotion from the scenario having finished.

    python tools/check_relay_path.py <run dir>      # at least one real DIRECT promotion happened
    python tools/check_relay_path.py --selftest

`<run dir>` is the HOST lane's session directory holding mh_net.log, or any directory above one (the
newest log under it that actually carries punch content wins) -- the same shape check_net_indicator's
`find_log` uses, and for the same reason (test_ui.py's post_check hands over the boot/menu session).

THE NEGATIVE THIS EXISTS FOR: a lane pinned with `[net] force_relay=1` (relay_match, relay_browse)
writes `net: udp path RELAY (forced) -- ...` ONCE at start, INSTEAD of ever arming the punch (see
log_formats.json id `net.punch_forced`) -- it NEVER promotes, by design, so this checker MUST fail
against one. Running it there is exactly the negative proof mp:R3a's done_when asks for: a real
assertion refuses a forced-relay lane rather than passing on any relay-shaped log.
"""

import argparse
import os
import re
import sys

DIRECT_RE = re.compile(
    r"net: udp path DIRECT -- peer (\d+) via (\S+) after (\d+) ms of punching.*?promotion (\d+)\)"
)
FORCED_NEEDLE = "net: udp path RELAY (forced) -- "
DEMOTED_RE = re.compile(r"net: udp path RELAY -- peer (\d+) demoted after (\d+) ms direct")
PUNCH_CONTENT_MARKERS = ("net: udp path DIRECT", "net: udp path RELAY", "net: udp punch --")


def _has_punch_content(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return False
    return any(m in text for m in PUNCH_CONTENT_MARKERS)


def find_log(target):
    """The mh_net.log this checker's subject is actually IN. See the module docstring: test_ui hands
    over the boot/menu session, and the punch transition is logged in the MATCH session, a sibling.
    Mirrors check_net_indicator.find_log's shape (content-first, not merely newest-file)."""
    if os.path.isfile(target):
        return target
    roots = [target]
    if os.path.isfile(os.path.join(target, "mh_net.log")):
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
        if _has_punch_content(p):
            return p
    return cands[0]


# ---- the negative arm ----------------------------------------------------------------------------
#
# This checker only runs when the rig does, so its OWN failure paths would otherwise never be
# exercised. Same discipline as check_net_indicator / check_shim_rtt: plant each case, require the
# matching verdict.

PROMOTED_LOG = (
    "[00:00:01.000] net: udp punch -- peer 1 offered 3 candidate(s)\n"
    "[00:00:01.312] net: udp path DIRECT -- peer 1 via 10.0.0.5:41230 after 312 ms of punching "
    "(the relay stays up underneath; promotion 1)\n"
)
FORCED_LOG = (
    "[00:00:00.010] net: udp path RELAY (forced) -- [net] force_relay=1, so no candidates are "
    "published and punching never starts\n"
)
NO_PUNCH_LOG = "[00:00:00.010] net: WELCOME -- host assigned us player 0\n"
DEMOTED_ONLY_LOG = (
    PROMOTED_LOG
    + "[00:00:45.000] net: udp path RELAY -- peer 1 demoted after 43688 ms direct (no keepalive "
    "for 4000 ms); traffic is back on the relay and punching has restarted (demotion 1)\n"
)


def selftest():
    import subprocess
    import tempfile

    cases = [
        ("a real promotion: DIRECT line present", PROMOTED_LOG, 0),
        ("force_relay=1 pinned lane: never promotes", FORCED_LOG, 1),
        ("no punch traffic at all", NO_PUNCH_LOG, 1),
        ("promoted then later demoted -- still counts as having gone direct", DEMOTED_ONLY_LOG, 0),
        ("no mh_net.log at all", None, 2),
    ]
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, text, want in cases:
            d = os.path.join(tmp, re.sub(r"[^a-zA-Z0-9]+", "_", name))
            os.makedirs(d, exist_ok=True)
            if text is not None:
                with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
                    fh.write(text)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__), d], capture_output=True, text=True
            )
            got = r.returncode
            print("  %-58s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d\n%s" % (name, got, want, r.stdout))
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print("check_relay_path --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    # mp:L1h -- one or MORE targets (relay_punch hands over every peer's dir since it also runs the
    # lobby-ping path clause); the claim is asserted on EACH, and both ends of a punch log it.
    ap.add_argument(
        "targets", nargs="+", help="session run dir(s) (or lane dirs above one), or mh_net.log(s)"
    )
    a = ap.parse_args()

    fails = []
    for target in a.targets:
        log = find_log(target)
        if not log:
            print("REFUSED: no mh_net.log at or under %s" % target)
            return 2
        with open(log, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()

        promotions = DIRECT_RE.findall(text)
        demotions = DEMOTED_RE.findall(text)
        forced = FORCED_NEEDLE in text
        print(
            "read %s: %d DIRECT promotion(s), %d demotion(s), forced=%s"
            % (log, len(promotions), len(demotions), forced)
        )
        if not promotions:
            if forced:
                fails.append(
                    "%s: no `net: udp path DIRECT` line, and the log shows `%s` -- this lane is "
                    "PINNED to the relay (force_relay=1) and never even attempts to promote. That is "
                    "the correct behaviour for a force_relay lane, and the wrong one for relay_punch."
                    % (log, FORCED_NEEDLE)
                )
            else:
                fails.append(
                    "%s: no `net: udp path DIRECT` line -- the pair never promoted off the relay (no "
                    "force_relay marker either, so this is not the pinned case: the punch simply did "
                    "not complete)" % log
                )
        else:
            print(
                "  promoted: peer %s via %s after %s ms of punching (promotion %s)" % promotions[0]
            )

    for f in fails:
        print("FAIL: %s" % f)
    print("check_relay_path: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
