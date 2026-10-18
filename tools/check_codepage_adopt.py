#!/usr/bin/env python3
"""check_codepage_adopt -- mp:F3c / MP-LANG: two peers with DIFFERENT codepages agree, or an old one is TOLD.

mp:MP-LANG (2026-09-29) RETIRED THE ADOPTION this file was written for. Chat is UTF-8 on every current
peer and `[input] codepage` is a purely local setting, so two peers pinned 1252 and 1251 no longer
negotiate anything: both advertise / echo CHAT_ENCODING_UTF8 (65001) and the host admits. The
refusal half now stages a PRE-MP-LANG peer (`[input] chat_legacy_codepage=1`, which puts its 8-bit
codepage on the wire the way an F3c build did) and asserts it is refused by name AND told. The file
keeps its name from the four registry rows it served (codepage_adopt / codepage_refused / name_forged / build_refused),
now segments of lobby_seq: pass `--client N` to judge client N of that row.

WHY A CHECKER, NOT PIXELS. The two F3c scenarios (tools/test_ui.py codepage_adopt / codepage_refused)
walk the same menus as client_join and render the same frames; what they claim is written in the two
peers' mh_net.log and nowhere on screen except one status line. Before F3c the measured failure was
precisely a frame that looked right (the joiner's lobby showed it seated) over a host log that said
REFUSED three times -- so the assertion has to be on the logs, both of them, and cross-peer.

    python tools/check_codepage_adopt.py --expect agree   <host run dir> <client run dir>
    python tools/check_codepage_adopt.py --expect refused <host run dir> <client run dir>
    python tools/check_codepage_adopt.py --expect forged  <host run dir> <client run dir>
    python tools/check_codepage_adopt.py --selftest

`--expect agree` (host pinned 1252, client pinned 1251 -- two local codepages, one chat encoding):
  client: `; MP-LANG: chat encoding 65001 agreed with the host (local codepage 1251 stays local)`
  host:   `-> ADMITTED` for that JOIN and NO `REFUSED (input codepage mismatch` at all.
`--expect refused` (client pinned 1251 with [input] chat_legacy_codepage=1, i.e. a pre-MP-LANG peer):
  host:   `REFUSED (input codepage mismatch; ours 65001, theirs 1251` AND `sent JOIN_REFUSED to player`
  client: `; F3c: JOIN REFUSED by the host: old client cp 1251` (the screen-short form)
          AND a session closed with reason `join_refused` (SESSION_END ... reason=join_refused),
          AND the mixed-pair line `; MP-LANG: host's chat encoding is 65001, ours is 1251`.
  The host must NOT log ADMITTED, and the client must not claim agreement.
`--expect forged` (client with [input] test_join_name=hex:4bebd0bb20696521, i.e. "K", a CP1252 byte,
  a UTF-8 Cyrillic letter and " ie!" -- a name no current client can type):
  client: `; MP-LANG TEST: JOIN carries a FORGED 8-byte name`
  host:   `; MP-LANG: JOIN from N carried a non-conforming name (8 bytes) -> normalized to 'Kie'`
          AND `JOIN from N 'Kie' ... -> ADMITTED`, i.e. the name was normalized BEFORE it was stored.

Both peers' lane directories are walked for EVERY mh_net.log (test_ui hands over the newest SESSION
directory; the client's adopt line is written by the JOIN click, a frame BEFORE that click opens the
session directory, so it lands in the run directory above it).
"""

import argparse
import os
import re
import sys

AGREE_NEEDLE = "; MP-LANG: chat encoding "
FORGED_CLIENT_NEEDLE = "; MP-LANG TEST: JOIN carries a FORGED 8-byte name"
FORGED_HOST_NEEDLE = "carried a non-conforming name (8 bytes) -> normalized to 'Kie'"
FORGED_ADMIT_NEEDLE = "'Kie' for '"
MIXED_NEEDLE = "; MP-LANG: host's chat encoding is "
ADMIT_NEEDLE = "-> ADMITTED"
REFUSE_HOST_NEEDLE = "REFUSED (input codepage mismatch; ours "
SENT_NEEDLE = "; F3c: sent JOIN_REFUSED to player "
REFUSE_CLIENT_NEEDLE = "; F3c: JOIN REFUSED by the host: "
CLOSE_NEEDLE = "reason=join_refused"
BUILD_CLIENT_NEEDLE = "; RL11 TEST: advertising forged build '0.0.1'"
BUILD_HOST_NEEDLE = "REFUSED (build (release version) mismatch; ours "


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

    if expect == "agree":
        need(AGREE_NEEDLE in client_text, "client logged the agreement line (%r)" % AGREE_NEEDLE)
        need(
            "chat encoding 65001 agreed with the host (local codepage 1251 stays local)"
            in client_text,
            "client agreed on UTF-8 (65001) with its own 1251 kept local",
        )
        need(MIXED_NEEDLE not in client_text, "client did not see a mixed-version host")
        need(REFUSE_HOST_NEEDLE not in host_text, "host never refused on codepage")
        need(ADMIT_NEEDLE in host_text, "host ADMITTED the JOIN")
        need(REFUSE_CLIENT_NEEDLE not in client_text, "client was not told of a refusal")
    elif expect == "forged":
        need(
            FORGED_CLIENT_NEEDLE in client_text,
            "client sent the forged name (%r)" % FORGED_CLIENT_NEEDLE,
        )
        need(
            FORGED_HOST_NEEDLE in host_text, "host normalized it to 'Kie' (%r)" % FORGED_HOST_NEEDLE
        )
        admitted = [
            ln for ln in host_text.splitlines() if FORGED_ADMIT_NEEDLE in ln and ADMIT_NEEDLE in ln
        ]
        need(bool(admitted), "host ADMITTED the JOIN under the normalized name 'Kie'")
        need(REFUSE_HOST_NEEDLE not in host_text, "host never refused on codepage")
    elif expect == "build":
        # RL11: the joiner forged [net] test_build_version=0.0.1; the host (real build) refuses it by
        # name, DELIVERS the refusal, and never admits. The host stays in its lobby (no SESSION_END).
        need(
            BUILD_CLIENT_NEEDLE in client_text,
            "client advertised the forged build (%r)" % BUILD_CLIENT_NEEDLE,
        )
        need(BUILD_HOST_NEEDLE in host_text, "host refused on build (%r)" % BUILD_HOST_NEEDLE)
        need(REFUSE_HOST_NEEDLE not in host_text, "host did not refuse on codepage")
        need(SENT_NEEDLE in host_text, "host sent JOIN_REFUSED (%r)" % SENT_NEEDLE)
        need(ADMIT_NEEDLE not in host_text, "host never ADMITTED")
        need(
            "JOIN REFUSED by the host: build " in client_text,
            "client's refusal text is the build form (screen: 'Host runs X, you run Y: update')",
        )
        need(CLOSE_NEEDLE in client_text, "client's session closed with reason=join_refused")
    else:
        need(REFUSE_HOST_NEEDLE in host_text, "host refused on codepage (%r)" % REFUSE_HOST_NEEDLE)
        need(
            "ours 65001, theirs 1251" in host_text,
            "host named both values (ours 65001 = UTF-8, theirs 1251)",
        )
        need(SENT_NEEDLE in host_text, "host sent JOIN_REFUSED (%r)" % SENT_NEEDLE)
        need(ADMIT_NEEDLE not in host_text, "host never ADMITTED")
        need(
            REFUSE_CLIENT_NEEDLE in client_text,
            "client logged the refusal (%r)" % REFUSE_CLIENT_NEEDLE,
        )
        need(
            "JOIN REFUSED by the host: old client cp 1251" in client_text,
            "client's refusal text names it an old client with its codepage",
        )
        need(CLOSE_NEEDLE in client_text, "client's session closed with reason=join_refused")
        need(
            "host's chat encoding is 65001, ours is 1251" in client_text,
            "client logged the mixed-version pair (host 65001, ours 1251)",
        )
    return ok, out


_TS = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]")
WINDOW_TAIL_S = 0.5  # host lines logged this long after the client's last line still belong to it


def _ts(line):
    m = _TS.match(line)
    if not m:
        return None
    h, mi, se, ms = (int(x) for x in m.groups())
    return h * 3600 + mi * 60 + se + ms / 1000.0


def scope_host_to_client(host_text, client_text):
    """tooling:TL-SUITE-FOLD step 4 -- ONE host session serves a SEQUENCE of clients, so its log holds
    every client's JOIN. Keep only the host lines logged while THIS client's process was running: from
    its first log line to its last (+WINDOW_TAIL_S). Clients never overlap (the runner ends one before
    launching the next), so the windows partition the host log. A line with no timestamp stays with the
    line before it. Returns (scoped_text, (t0, t1)) or (host_text, None) when the client log carries no
    timestamps (the caller then refuses -- an unscoped host log would read every segment's JOIN)."""
    stamps = [t for t in (_ts(ln) for ln in client_text.splitlines()) if t is not None]
    if not stamps:
        return host_text, None
    t0, t1 = min(stamps), max(stamps) + WINDOW_TAIL_S
    keep, inside = [], False
    for ln in host_text.splitlines():
        t = _ts(ln)
        if t is not None:
            inside = t0 <= t <= t1
        if inside:
            keep.append(ln)
    return chr(10).join(keep), (t0, t1)


def selftest():
    fails = 0

    # --- the sequence scoping: another client's JOIN outside this client's window must not count
    def stamp(t, msg):
        return "[%02d:%02d:%02d.000] %s" % (t // 3600, t // 60 % 60, t % 60, msg)

    host = chr(10).join(
        [
            stamp(
                100,
                "; S4 JOIN from 1 'client' for 'g' -> REFUSED (input codepage mismatch; ours 65001, theirs 1251 -- x)",
            ),
            stamp(200, "; S4 JOIN from 1 'client' for 'g' -> ADMITTED (open slot/map gate)"),
            stamp(300, "; S4 JOIN from 1 'Kie' for 'g' -> ADMITTED (open slot/map gate)"),
        ]
    )
    cli = chr(10).join([stamp(190, "; boot"), stamp(205, "; MP-LANG: chat encoding 65001 agreed")])
    scoped, win = scope_host_to_client(host, cli)
    if "REFUSED" in scoped or "'Kie'" in scoped or "ADMITTED" not in scoped or win is None:
        fails += 1
        print("selftest FAIL: scope_host_to_client kept the wrong lines: %r" % scoped)
    if scope_host_to_client(host, "no stamps here")[1] is not None:
        fails += 1
        print("selftest FAIL: an unstamped client log must report no window")

    def case(name, expect, host, client, want):
        nonlocal fails
        ok, lines = verdict(expect, host, client)
        if ok != want:
            fails += 1
            print("selftest FAIL: %s -> %s, wanted %s" % (name, ok, want))
            for ln in lines:
                print("    " + ln)

    good_host_agree = (
        "; S4 JOIN from 1 'client' for 'uitest#00000001' -> ADMITTED (open slot/map gate)\n"
    )
    good_client_agree = (
        "; MP-LANG: chat encoding 65001 agreed with the host (local codepage 1251 stays local)\n"
    )
    case("agree: both good", "agree", good_host_agree, good_client_agree, True)
    case("agree: client never agreed", "agree", good_host_agree, "", False)
    case(
        "agree: host refused anyway",
        "agree",
        good_host_agree + "-> REFUSED (input codepage mismatch; ours 65001, theirs 1251 -- x)\n",
        good_client_agree,
        False,
    )
    case(
        "agree: client saw a mixed-version host",
        "agree",
        good_host_agree,
        good_client_agree + "; MP-LANG: host's chat encoding is 1252, ours is 65001 (x)\n",
        False,
    )
    good_host_ref = (
        "; S4 JOIN from 1 'client' for 'uitest#1' -> REFUSED (input codepage mismatch; ours 65001, theirs 1251 -- x)\n"
        "; F3c: sent JOIN_REFUSED to player 1: old client cp 1251\n"
    )
    good_client_ref = (
        "; MP-LANG: host's chat encoding is 65001, ours is 1251 (65001 = UTF-8) -- a mixed-version pair\n"
        "; F3c: JOIN REFUSED by the host: old client cp 1251 -> leaving\n"
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
        "; MP-LANG: host's chat encoding is 65001, ours is 1251 (65001 = UTF-8) -- a mixed-version pair\n",
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
    good_host_forged = (
        "; MP-LANG: JOIN from 1 carried a non-conforming name (8 bytes) -> normalized to 'Kie' ([A-Za-z0-9])\n"
        "; S4 JOIN from 1 'Kie' for 'uitest#00000001' -> ADMITTED (open slot/map gate)\n"
    )
    good_client_forged = "; MP-LANG TEST: JOIN carries a FORGED 8-byte name (test_join_name)\n"
    case("forged: both good", "forged", good_host_forged, good_client_forged, True)
    case(
        "forged: host stored it raw (no normalize line)",
        "forged",
        good_host_forged.splitlines()[1] + "\n",
        good_client_forged,
        False,
    )
    case(
        "forged: admitted under the raw name",
        "forged",
        good_host_forged.replace("'Kie' for", "'K? ie!' for"),
        good_client_forged,
        False,
    )
    case("forged: client never forged", "forged", good_host_forged, "", False)
    good_host_build = (
        "; S4 JOIN from 1 'client' for 'uitest#1' -> REFUSED (build (release version) mismatch; ours 0.2.0, theirs 0.0.1)\n"
        "; F3c: sent JOIN_REFUSED to player 1: build 0.2.0/0.0.1\n"
    )
    good_client_build = (
        "; RL11 TEST: advertising forged build '0.0.1' (test_build_version)\n"
        "; F3c: JOIN REFUSED by the host: build 0.2.0/0.0.1 -> leaving\n"
        "; SESSION_END match_id=x reason=join_refused\n"
    )
    case("build: both good", "build", good_host_build, good_client_build, True)
    case(
        "build: host admitted", "build", good_host_build + "-> ADMITTED\n", good_client_build, False
    )
    case(
        "build: client never forged",
        "build",
        good_host_build,
        good_client_build.splitlines()[1],
        False,
    )
    case(
        "build: client not told",
        "build",
        good_host_build.splitlines()[0] + "\n",
        good_client_build,
        False,
    )
    print("check_codepage_adopt --selftest: %s" % ("PASS" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--expect", choices=("agree", "refused", "forged", "build"), default="agree")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument(
        "--client",
        type=int,
        default=0,
        help="client sequence row (client_sequence): dirs are <host> <client 1> .. <client N>; judge "
        "client K (1-based) against only the host lines logged during ITS process's lifetime",
    )
    ap.add_argument("dirs", nargs="*", help="<host run dir> <client run dir>")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.client:
        if not 1 <= a.client < len(a.dirs):
            print(
                "check_codepage_adopt: --client %d but %d run directories were handed over "
                "(host + clients)" % (a.client, len(a.dirs))
            )
            return 2
        a.dirs = [a.dirs[0], a.dirs[a.client]]
    elif len(a.dirs) != 2:
        print(
            "check_codepage_adopt: need exactly two run directories (host, client); got %d"
            % len(a.dirs)
        )
        return 2
    texts = []
    for role, d in zip(("host", "client"), a.dirs):
        logs = collect_logs(d)
        if a.client and role == "client":
            # a sequence lane can hold older runs (a rerun reuses it): judge, and take the host window
            # from, ONLY the run directory test_ui handed over -- never the lane's history above it
            # (the session directory is a SIBLING of the process directory, named by the same ISO
            # stamp, so "this run" = every log dir stamped at or after the handed one)
            since = os.path.basename(os.path.abspath(d))[:20]
            logs = [(p, t) for p, t in logs if os.path.basename(os.path.dirname(p))[:20] >= since]
        if not logs:
            print("check_codepage_adopt: no mh_net.log under %s (%s)" % (d, role))
            return 2
        print("  %s: %d mh_net.log(s) under %s" % (role, len(logs), d))
        texts.append("\n".join(t for _p, t in logs))
    if a.client:
        texts[0], win = scope_host_to_client(texts[0], texts[1])
        if win is None:
            print(
                "check_codepage_adopt: client %d's log carries no timestamps; cannot scope the host"
                % a.client
            )
            return 2
        print(
            "  host log scoped to client %d's window %.1f..%.1f s of day"
            % (a.client, win[0], win[1])
        )
    ok, lines = verdict(a.expect, texts[0], texts[1])
    for ln in lines:
        print("  " + ln)
    print("check_codepage_adopt (--expect %s): %s" % (a.expect, "PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
