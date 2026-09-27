#!/usr/bin/env python3
"""check_map_transfer -- the post_check for mp:X2's rig scenarios (map_absent / map_conflict).

WHAT IT ASSERTS, AND WHY EACH CLAUSE NEEDS BOTH PEERS' LOGS. X2's acceptance is a statement about
two machines that disagreed about a file and then did not: the HOST's log carries the content claim
it advertised and the transfer it armed, and the CLIENT's carries what it held before, what it
stored, and what its own file hashed to afterwards. A checker handed one of them can only ever see
half the claim, which would read as a pass -- so this runs under `post_check_peers`.

THE FOUR ASSERTIONS, in the order the tracker states them:

  1. THE HOST MADE A CONTENT CLAIM.  `; [map] host claim <name> sha=<hex> size=<n>`. Without it
     nothing downstream means anything: a run where the host silently fell back to "no claim" would
     otherwise satisfy every other line here by doing nothing at all, which is the vacuous-green
     shape this suite keeps refusing.

  2. THE CLIENT DID NOT HAVE IT, AND THEN DID.  `; [map] client want ... sha=<H>` with a
     `; [map] client resolve missing` (so the run really was the download case and not a peer that
     happened to hold the map already), followed by `; [map] client stored <file> (<n> B, sha=<H>)`
     with the SAME hash the host claimed. The stored name must also be the content-addressed form --
     `<stem>.<16 hex>.<ext>` -- because storing under the base name is the one failure that would
     look identical in every other line.

  3. THE CLIENT'S OWN FILE IS BYTE-UNCHANGED.  `; [map] local before ...` and `; [map] local after
     ...` bracket the whole session (the first fires when the advert lands, the second on the host's
     FLAG_START) and must agree -- both hash and size, or both "absent". This is the assertion the
     mechanism exists for and the one a human would forget to make.

  4. THE PEERS PLAYED.  Both logs reach the match. Asserted through the scenario's own launch
     assertions rather than re-derived here; what this file adds is that the HOST's Start was HELD
     first: `; [map] start REFUSED` naming the client is expected in the download scenarios, and its
     absence means the gate never closed, i.e. the Start-gate clause was not exercised.

WHAT THESE SCENARIOS DO NOT PROVE, said here rather than left to be assumed. The client's starting
state is staged by `[net] map_test_pretend=none|other`, which changes what that peer REPORTS holding
and whether its resolver considers the base-name candidate -- it moves no file, because a local
lane's `Maps` is a symlink to the one shared game image and the file-moving first version of the
knob took the map away from the host too. So on a local lane the client's base file IS the host's
content, and a broken open-redirect would still open matching bytes and still pass here. The
redirect's own proof is `net_selftest.exe maptest` arms E and W, where the local file is genuinely
different bytes. What these scenarios prove is the INTEGRATION: the claim, the gate, the transfer,
the content-addressed write and a match that plays.

`--expect-nothing` inverts clause 2 for the third scenario: a joiner that already holds the content
must store NOTHING and the host must arm NO transfer, asserted as the absence of `; [map] send armed`
together with the presence of `; [map] peer ... holds the map`, so the absence is read beside a
positive line rather than on its own.

`--expect-early-join` (mp:X2d, with --expect-nothing) is the relay-join RACE: the client's first
advert is held by `[net] map_test_advert_hold_ms` until its JOIN has gone out, which is the order the
rc3 field match lost in. The client log must show the hold AND its release after a JOIN (else the
race was not staged and a green says nothing), and the host must never have said `needs the map` --
the JOIN itself carried the hash, rather than a later re-report correcting it.

`--expect-leave-rejoin` (mp:X2f clause (3)) is a joiner (client1) that leaves mid-download
(Cancel/LEAVE) and whose PROCESS then exits for real; a FRESH process (client2) re-joins and
finishes the download -- two client logs, not one. Asserts TWO `; [map] send armed` lines (client1,
then client2's fresh join), the fix's own release evidence for client1's in-flight transfer --
EITHER map_transfer.cpp's `; [map] peer ... is gone at the transport (its link dropped)` OR the
transport layer's `channel-C transfer cancelled with it (mp:X2f)`, whichever this shape's timing
reaches, or udp_transport.cpp's `net: snapshot SEND to player N superseded the UNFINISHED transfer
to player M` (client2's arm replaced client1's still-running transfer before client1's link timed
out -- the fast-lane timing, measured 2026-09-27) -- and exactly ONE `; [map] client stored ...` across
BOTH client logs combined (client1
never finishes; client2 does). The base clauses (host claim, `client resolve missing`, the stored
hash/form, the player's own file unchanged) apply to client2 alone -- client1 is expected to show
none of them, and is identified as "the one that didn't finish" rather than checked against them.
"""

import argparse
import os
import re
import sys

# The needles are the registered ones (tools/data/log_formats.json, the map.* rows). Kept as module
# constants rather than inline so the registry lint can find them as string literals.
RE_HOST_CLAIM = re.compile(r"; \[map\] host claim (.+?) sha=([0-9a-f]{16}) size=(\d+)")
RE_HOST_NOCLAIM = re.compile(r"; \[map\] host noclaim ")
RE_WANT = re.compile(r"; \[map\] client want (.+?) sha=([0-9a-f]{16}) size=(\d+)")
RE_RESOLVE_MISSING = re.compile(r"; \[map\] client resolve missing")
RE_STORED = re.compile(r"; \[map\] client stored (.+?) \((\d+) B, sha=([0-9a-f]{16})\)")
RE_LOCAL = re.compile(
    r"; \[map\] local (before|after) (.+?) (?:sha=([0-9a-f]{16}) size=(\d+)|absent)"
)
RE_SEND_ARMED = re.compile(r"; \[map\] send armed to peer (\d+) '(.+?)' \((\d+) B")
RE_PEER_HOLDS = re.compile(r"; \[map\] peer (\d+) '(.+?)' holds the map")
RE_PEER_NEEDS = re.compile(r"; \[map\] peer (\d+) '(.+?)' needs the map")
# mp:X2d -- the advert-hold knob: the held line, and a release that says a JOIN went out first.
RE_ADVERT_HELD = re.compile(r"; \[map\] uitest advert_hold: first SESSION_INFO from (\d+) HELD")
RE_ADVERT_RELEASED = re.compile(
    r"; \[map\] uitest advert_hold: released after (\d+) ms \(a JOIN went out first\)"
)
RE_START_REFUSED = re.compile(r"; \[map\] start REFUSED -- waiting for '(.+?)'")
# mp:X2e -- the lobby-entry map check. The staging (pretend=absent + a refused open) and the fix's
# own line; the retail error box is named by the DLG logger's caller (the return address inside
# llm_cfg_map_verify_version, EN 0x004be151).
RE_PRETEND_ABSENT = re.compile(r"; \[map\] uitest pretend=absent ")
RE_ABSENT_HID = re.compile(r"; \[map\] uitest absent: refused the game's open of (.+?) -- staged")
RE_ENTRY_DEFERRED = re.compile(r"; \[map\] entry check (.+?): (not readable here|checksum differs)")
RE_ENTRY_RETAIL = re.compile(r"; \[map\] entry check retail ")
RE_ENTRY_DLG = re.compile(r"; DLG savegame_io_error caller=0x004be151")
# mp:X2b -- the transport-cannot-carry refusal (TCP): the host still CLAIMS, arms nothing, and
# refuses Start naming the peer and the map.
RE_HOST_NOCARRY = re.compile(r"; \[map\] host nocarry ")
RE_START_REFUSED_NOCARRY = re.compile(
    r"; \[map\] start REFUSED -- '(.+?)' holds a different (.+?) and this transport cannot carry"
)
RE_START_OK = re.compile(r"; \[map\] start OK ")
# mp:X2/X2b enforcement (launch.cpp on_begin_map_load): a Start CLICK arrived while the gate was shut
# and was swallowed before begin_map_load ran.
RE_CLICK_REFUSED = re.compile(r"; \[map\] start CLICK REFUSED -- '(.+?)'")
# The stored file is `mh_dl\<stem>.<16 hex>.<ext>`: the NAME is the content-addressed form the
# tracker asks for, and the `mh_dl\` prefix says it landed OUTSIDE the map directory. Both halves
# are asserted, and both were wrong once. A copy beside the player's maps would appear in their
# picker -- and the local rig's lanes share one `Maps` by symlink, so it would change what every
# other scenario defaults to. A subdirectory of `Maps\` is worse: llm_mp_mappicker_populate_list
# scans that directory for SUB-FOLDERS before it scans for `*.mpm`, so the download folder became
# the picker's pre-selected first row and a rig host never reached its lobby.
RE_STORED_FORM = re.compile(r"^mh_dl\\(.*)\.([0-9a-f]{16})(\.[^.]*)?$")

# mp:X2f clause (3) -- a joiner leaves mid-download and re-joins the same host (as a fresh process:
# see mp_host_map_leaverejoin.txt). TWO lines the fix can write for a peer whose transfer the
# transport released without a stale re-arm -- map_transfer.cpp's own (a peer the pump finds gone
# while STILL SEATED, reap_gone_locked's path -- only reachable if the peer never sent a graceful
# LEAVE first) and udp_endpoint.cpp's transport-layer one (any dropped connection that still had a
# channel-C transfer targeting it, regardless of lobby-seat state -- reached when client1's link
# drops BEFORE client2 arms). Either is "not a stale re-arm into a freed Sender" -- the crash the
# field hit. The third path, RE_SUPERSEDED below, is the one the rig row measured.
RE_PEER_GONE = re.compile(
    r"; \[map\] peer (\d+) '(.+?)' is gone at the transport \(its link dropped\)"
)
RE_CHANNELC_CANCELLED = re.compile(
    r"net: udp conn (\d+)'s channel-C transfer cancelled with it -- its destination is gone"
)
# The THIRD release path, and the one this row's process-exit shape reaches on a fast lane (measured
# 2026-09-27): a LEAVE clears the lobby seat but cancels nothing on channel C, so client1's transfer
# keeps running while client1 sits linked on the browser; client2 is admitted on a NEW conn before
# client1's link times out, and its arm REPLACES the unfinished transfer (snap::Outbox detaches it
# first) -- the exact re-arm-into-a-running-transfer sequence the rc4 host died in. By the time
# conn 0 drops there is nothing left on it to cancel, so neither line above is written.
RE_SUPERSEDED = re.compile(
    r"net: snapshot SEND to player (\d+) superseded the UNFINISHED transfer to player (\d+)"
)

FAILS = []
NOTES = []


def fail(msg):
    FAILS.append(msg)


def note(msg):
    NOTES.append(msg)


def read_one(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def read(path):
    """One peer's whole `mh_net.log` history, as a single text.

    THE ARGUMENT IS A RUN DIRECTORY, and one run directory is not enough. `post_check_peers` hands
    each peer's NEWEST run (SES1's per-session log split), and X2's evidence straddles that split by
    construction: the host's content claim is written while it is still on the menu/lobby process
    log, and the peer report, the transfer and the Start gate are written in the match session's.
    A checker reading only the newest directory sees the gate and no claim, and would report "the
    host made no claim" about a host that plainly did. So every run of the same LANE is read, oldest
    first -- the peer is the process, not the session.

    A plain file path is still accepted, which is what makes the checker runnable by hand on a log.
    """
    if not os.path.isdir(path):
        return read_one(path)
    # ONLY THIS PROCESS'S RUNS -- from its `*_menu_*` run (the process's first directory, written at
    # boot) up to the given one. Reading every run in the lane was wrong on a SHARED lane: in the
    # 2026-09-24 gate, map_refuse_tcp borrowed match_launch's lanes (since TL-SUITE-FOLD-ML,
    # d28_canceltask's), and the joiner's `local after` from the anchor's earlier (started) match
    # read as "the refusal did not hold" (green alone, red in the suite). Run directory names start
    # with a UTC stamp, so name order is time order.
    logs_dir = os.path.dirname(os.path.normpath(path))  # <lane>/logs
    me = os.path.basename(os.path.normpath(path))
    names = sorted(n for n in os.listdir(logs_dir) if os.path.isdir(os.path.join(logs_dir, n)))
    # The process runs from its menu dir (the last one at or before `me`) up to the NEXT process's
    # menu dir -- and that includes session dirs AFTER `me`: post_check_peers without
    # post_check_session hands over the MENU dir, and the match-time lines (transfer, gate, `local
    # after`) are in the later session dirs (2026-09-24: cutting at `me` read the menu dir alone and
    # all five map rows went red with "the host never armed a transfer").
    menu_idx = [i for i, n in enumerate(names) if "_menu_" in n]
    starts = [i for i in menu_idx if names[i] <= me]
    if starts:
        nxt = [i for i in menu_idx if i > starts[-1]]
        mine = names[starts[-1] : (nxt[0] if nxt else len(names))]
    else:  # no menu dir (a hand-laid fixture): every run up to the given one
        mine = [n for n in names if n <= me]
    runs = [
        os.path.join(logs_dir, n, "mh_net.log")
        for n in mine
        if os.path.isfile(os.path.join(logs_dir, n, "mh_net.log"))
    ]
    if os.path.isfile(os.path.join(path, "mh_net.log")) and not runs:
        runs = [os.path.join(path, "mh_net.log")]
    return "\n".join(read_one(f) for f in runs)


def classify(paths):
    """Split the given logs into (host_text, client_texts) by what each one claims to be.

    The harness hands the peers' logs in an order this file should not depend on, and "which peer was
    the host" is written in the logs themselves -- the host is the one that made a content claim (or
    armed a send). Deriving it beats trusting an argument position that a future runner may reorder.
    """
    host, clients = None, []
    for p in paths:
        t = read(p)
        if RE_HOST_CLAIM.search(t) or RE_SEND_ARMED.search(t) or RE_PEER_NEEDS.search(t):
            if host is None:
                host = (p, t)
                continue
        clients.append((p, t))
    return host, clients


def peer_label(path):
    """A name for this peer in the report -- the LANE, not the run directory.

    basename() of a run directory is a timestamp, and of a path with a trailing separator it is the
    empty string, which is how the first green run printed `: stored ...` with nothing in front of
    the colon. The lane name is the thing a reader can act on.
    """
    p = os.path.normpath(path)
    parts = p.split(os.sep)
    if "logs" in parts:
        return parts[parts.index("logs") - 1]
    return os.path.basename(p) or p


def check_client(path, text, claim_hash, expect_nothing):
    name = peer_label(path)
    want = RE_WANT.search(text)
    if not want:
        fail(
            "%s: no `; [map] client want` line -- this peer never learned the host's map claim"
            % name
        )
        return
    if want.group(2) != claim_hash:
        fail(
            "%s: the client wants sha=%s but the host claimed sha=%s"
            % (name, want.group(2), claim_hash)
        )

    stored = RE_STORED.search(text)
    if expect_nothing:
        if stored:
            fail(
                "%s: the client STORED a map it should already have had (%s)"
                % (name, stored.group(1))
            )
        else:
            note("%s: stored nothing, as expected" % name)
    else:
        if not RE_RESOLVE_MISSING.search(text):
            fail(
                "%s: the client never reported `resolve missing` -- it already held the content, so "
                "this run did not exercise a download at all" % name
            )
        if not stored:
            fail("%s: the client never stored the host's map" % name)
        else:
            if stored.group(3) != claim_hash:
                fail(
                    "%s: the stored map hashes to %s, not the advertised %s"
                    % (name, stored.group(3), claim_hash)
                )
            m = RE_STORED_FORM.match(stored.group(1))
            if not m or m.group(2) != claim_hash:
                fail(
                    "%s: the stored file '%s' is not `mh_dl\\<stem>.<16 hex>.<ext>` -- a download "
                    "under the base name would have overwritten the player's map, and one beside "
                    "it would have appeared in their map picker" % (name, stored.group(1))
                )
            else:
                note("%s: stored %s (%s B)" % (name, stored.group(1), stored.group(2)))

    # Clause 3: the player's own file, before and after.
    befores = RE_LOCAL.findall(text)
    before = [b for b in befores if b[0] == "before"]
    after = [b for b in befores if b[0] == "after"]
    if not before:
        fail(
            "%s: no `; [map] local before` sample -- the byte-unchanged claim has no baseline"
            % name
        )
    elif not after:
        fail(
            "%s: no `; [map] local after` sample -- the client never reached the host's Start, so "
            "nothing can be said about its own file afterwards" % name
        )
    else:
        b, a = before[0], after[-1]
        if (b[2], b[3]) != (a[2], a[3]):
            fail(
                "%s: THE PLAYER'S OWN %s CHANGED: before sha=%s size=%s, after sha=%s size=%s"
                % (name, b[1], b[2] or "absent", b[3] or "-", a[2] or "absent", a[3] or "-")
            )
        else:
            note(
                "%s: own file unchanged (%s sha=%s size=%s)"
                % (name, b[1], b[2] or "absent", b[3] or "-")
            )


def main(argv=None):
    del FAILS[:], NOTES[:]
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("logs", nargs="*", help="mh_net.log from each peer (host and client(s))")
    ap.add_argument(
        "--expect-nothing",
        action="store_true",
        help="the joiner already holds the content: assert NO transfer was armed and "
        "nothing was stored (read beside the positive `holds the map` line)",
    )
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="plant a log pair per outcome and assert this reader's verdict on each",
    )
    ap.add_argument(
        "--expect-gate",
        action="store_true",
        help="assert the host's Start was HELD at least once (the refusal names the peer)",
    )
    ap.add_argument(
        "--expect-early-join",
        action="store_true",
        help="mp:X2d (with --expect-nothing): the client's JOIN preceded the host's first advert "
        "(the advert-hold knob announced the hold and a release after a JOIN), and the host never "
        "logged `needs the map` -- the JOIN itself carried the hash",
    )
    ap.add_argument(
        "--expect-entry-deferred",
        action="store_true",
        help="mp:X2e: the joiner's game could NOT open the map at lobby entry (pretend=absent, a "
        "refused open logged), the entry check deferred to the transfer instead of raising retail's "
        "error box, and the download finished in ONE join (a single `client want`)",
    )
    ap.add_argument(
        "--expect-refused",
        action="store_true",
        help="mp:X2b: the transport cannot carry maps (TCP) and the joiner holds different "
        "bytes -- assert the host claimed, logged `host nocarry`, armed NO transfer, refused Start "
        "naming the peer and the map, and that neither peer reached Start",
    )
    ap.add_argument(
        "--expect-leave-rejoin",
        action="store_true",
        help="mp:X2f clause (3): a joiner leaves mid-download and its process exits; a fresh process "
        "re-joins the same host -- assert TWO `send armed` lines, one of the fix's three release "
        "lines (gone at the transport / channel-C cancelled at the drop / superseded by the next "
        "arm), and exactly ONE `client stored` (the interrupted attempt never landed bytes)",
    )
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()

    host, clients = classify(args.logs)
    if host is None:
        fail(
            "none of the given logs is a host log (no `; [map] host claim`, `send armed` or `peer "
            "... needs the map` line in any of them)"
        )
        print("check_map_transfer: FAIL")
        for f in FAILS:
            print("  - %s" % f)
        return 1
    hpath, htext = host

    claim = RE_HOST_CLAIM.search(htext)
    if not claim:
        fail(
            "%s: the host made NO content claim (%s) -- every other assertion here would pass "
            "vacuously"
            % (
                peer_label(hpath),
                "a `host noclaim` line is present"
                if RE_HOST_NOCLAIM.search(htext)
                else "not even a noclaim line",
            )
        )
        print("check_map_transfer: FAIL")
        for f in FAILS:
            print("  - %s" % f)
        return 1
    claim_hash = claim.group(2)
    note("host claimed %s sha=%s size=%s" % (claim.group(1), claim_hash, claim.group(3)))

    if args.expect_refused:
        return check_refused(hpath, htext, clients, claim)

    armed = RE_SEND_ARMED.search(htext)
    if args.expect_nothing:
        if armed:
            fail(
                "%s: the host ARMED a transfer to a peer that already held the map (%s)"
                % (peer_label(hpath), armed.group(0).strip())
            )
        if not RE_PEER_HOLDS.search(htext):
            fail(
                "%s: the host never logged `peer ... holds the map` -- so 'nothing was transferred' "
                "cannot be told apart from 'no peer ever joined'" % peer_label(hpath)
            )
        else:
            note("host: the peer held the map; no transfer armed")
    else:
        if not armed:
            fail("%s: the host never armed a transfer" % peer_label(hpath))
        else:
            note(
                "host armed a %s B transfer to peer %s '%s'"
                % (armed.group(3), armed.group(1), armed.group(2))
            )

    if args.expect_early_join:
        check_early_join(hpath, htext, clients)

    if args.expect_entry_deferred:
        check_entry_deferred(clients)

    if args.expect_leave_rejoin:
        check_leave_rejoin(hpath, htext, clients, claim_hash, args.expect_nothing)

    if args.expect_gate:
        held = RE_START_REFUSED.search(htext)
        if not held:
            fail(
                "%s: the Start gate NEVER CLOSED -- no `; [map] start REFUSED` line, so this run "
                "did not exercise the host refusing to Start" % peer_label(hpath)
            )
        else:
            note("host held Start for '%s'" % held.group(1))

    if not clients:
        fail("no client log was given -- the receiver-side half of every clause is unread")
    if not args.expect_leave_rejoin:
        # clause (3)'s two clients play asymmetric roles (one exits mid-download, one finishes it) --
        # check_leave_rejoin does its OWN per-client work above, on the SURVIVOR only.
        for p, t in clients:
            check_client(p, t, claim_hash, args.expect_nothing)

    for n in NOTES:
        print("  %s" % n)
    if FAILS:
        print("check_map_transfer: FAIL")
        for f in FAILS:
            print("  - %s" % f)
        return 1
    print("check_map_transfer: OK")
    return 0


def check_early_join(hpath, htext, clients):
    """mp:X2d: the JOIN went out before the advert, and still carried the map hash."""
    needs = RE_PEER_NEEDS.search(htext)
    if needs:
        fail(
            "%s: the host logged `%s` -- the JOIN reached it without the map hash (the X2d race)"
            % (peer_label(hpath), needs.group(0).strip())
        )
    for p, t in clients:
        name = peer_label(p)
        if not RE_ADVERT_HELD.search(t):
            fail(
                "%s: no `advert_hold ... HELD` line -- [net] map_test_advert_hold_ms was not in "
                "effect, so the JOIN-before-advert race was not staged" % name
            )
        elif not RE_ADVERT_RELEASED.search(t):
            fail(
                "%s: the held advert was not released after a JOIN -- the JOIN never went out "
                "ahead of it, so the race was not staged" % name
            )
        else:
            note("%s: advert held until the JOIN went out (race staged)" % name)


def check_entry_deferred(clients):
    """mp:X2e: a joiner whose game cannot open the map enters the lobby and downloads it in ONE join.

    Every clause is read beside a positive line: the staging line and a refused open say the lobby
    entry really met an absent file (without them the retail check would have passed on the shared
    image's copy and a green would say nothing -- the pre-X2e map_absent shape); the deferral line
    says the fix saw it; and a single `client want` says no leave-and-rejoin was needed.
    """
    for p, t in clients:
        name = peer_label(p)
        if not RE_PRETEND_ABSENT.search(t):
            fail(
                "%s: no `uitest pretend=absent` line -- the map was not staged absent, so the "
                "lobby-entry check was not exercised" % name
            )
            continue
        if not RE_ABSENT_HID.search(t):
            fail(
                "%s: no `uitest absent: refused the game's open` line -- nothing ever tried to open "
                "the map while it was absent, so the entry check was not exercised" % name
            )
        if RE_ENTRY_RETAIL.search(t):
            fail("%s: [net] map_entry_check=retail -- the fix was disarmed (the red arm)" % name)
        if RE_ENTRY_DLG.search(t):
            fail(
                "%s: retail's 'can't create map file' box was raised at lobby entry (DLG caller "
                "0x004be151)" % name
            )
        ent = RE_ENTRY_DEFERRED.search(t)
        if not ent:
            fail(
                "%s: no `; [map] entry check <map>: not readable here` line -- the lobby-entry "
                "check never deferred to the transfer" % name
            )
        else:
            note("%s: lobby-entry check deferred (%s: %s)" % (name, ent.group(1), ent.group(2)))
        wants = len(RE_WANT.findall(t))
        if wants != 1:
            fail(
                "%s: %d `client want` lines -- the download took %d joins, not one"
                % (name, wants, wants)
            )


def check_leave_rejoin(hpath, htext, clients, claim_hash, expect_nothing):
    """mp:X2f clause (3): a joiner leaves mid-download (Cancel/LEAVE) and its PROCESS exits for real;
    a FRESH process re-joins and finishes the download (mp_host_map_leaverejoin.txt's process-exit
    shape -- GS1(b)'s own client_after_exit/client_shares_lane topology, TWO client logs: client1
    exits early and never finishes, client2 is an ordinary fresh join that does).

    Every clause here exists to rule out a specific vacuous green. TWO `send armed` lines (not one)
    say the download was really re-initiated for client2, not merely inherited from client1's
    never-released transfer; the fix's own release evidence -- EITHER map_transfer.cpp's `peer ...
    is gone at the transport` (a peer the pump found gone while still seated) OR the transport-layer
    `channel-C transfer cancelled with it (mp:X2f)` (any dropped connection that still had a transfer
    targeting it, reached when client1's link drops before client2 arms) OR udp_transport.cpp's
    `snapshot SEND ... superseded the UNFINISHED transfer` (client2's arm replaced the transfer
    client1's LEAVE left running -- the timing the rig row measured) -- says client1's in-flight transfer
    was safely detached, not a stale re-arm into a freed Sender (the crash the field hit); and exactly
    ONE `client stored` across BOTH client logs (client1 must show zero: it left before finishing;
    client2 exactly one) says the download completed once, on the fresh process, not twice and not
    never.
    """
    hl = peer_label(hpath)
    armed = RE_SEND_ARMED.findall(htext)
    if len(armed) < 2:
        fail(
            "%s: only %d `send armed` line(s) -- clause (3) needs one for client1 and a SECOND for "
            "client2's fresh join" % (hl, len(armed))
        )
    else:
        note("%s: armed %d transfers (client1, then client2's fresh join)" % (hl, len(armed)))

    gone = RE_PEER_GONE.search(htext)
    cancelled = RE_CHANNELC_CANCELLED.search(htext)
    superseded = RE_SUPERSEDED.search(htext)
    if not gone and not cancelled and not superseded:
        fail(
            "%s: no `peer ... is gone at the transport`, no `channel-C transfer cancelled with "
            "it (mp:X2f)` and no `snapshot SEND ... superseded the UNFINISHED transfer` line -- "
            "client1's in-flight transfer was released by none of the mp:X2f paths" % hl
        )
    elif superseded:
        note(
            "%s: client2's arm (player %s) superseded client1's unfinished transfer (player %s) -- "
            "detached before its copy was freed" % (hl, superseded.group(1), superseded.group(2))
        )
    elif gone:
        note(
            "%s: peer %s '%s' released at the transport, not re-armed"
            % (hl, gone.group(1), gone.group(2))
        )
    else:
        note(
            "%s: conn %s's channel-C transfer cancelled at the drop, not inherited by client2"
            % (hl, cancelled.group(1))
        )

    total_stored = sum(len(RE_STORED.findall(t)) for _, t in clients)
    if total_stored != 1:
        fail(
            "%d `client stored` line(s) across both client logs -- clause (3) wants exactly one "
            "(client1's interrupted attempt must never land a complete file, client2's must)"
            % total_stored
        )
    else:
        note("exactly one `client stored` across both client logs (client2's completed download)")

    # client2 is whichever log actually finished (stored the map, or at least reached the host's
    # Start) -- the base clauses (host claim already read, `resolve missing`, the stored hash/form,
    # the player's own file unchanged) apply to IT alone. client1 is expected to show none of that:
    # calling the generic per-client check on it would fail on assertions it was never meant to meet.
    survivor = None
    for p, t in clients:
        if RE_STORED.search(t) or any(b[0] == "after" for b in RE_LOCAL.findall(t)):
            survivor = (p, t)
            break
    if survivor is None:
        fail(
            "neither client log shows a `client stored` or a `local after` sample -- no client ever "
            "finished the download and reached the host's Start"
        )
        return
    check_client(survivor[0], survivor[1], claim_hash, expect_nothing)


def check_refused(hpath, htext, clients, claim):
    """mp:X2b's rig clause: a refusal beats a desync. Every line is read beside a positive one."""
    hl = peer_label(hpath)
    if not RE_HOST_NOCARRY.search(htext):
        fail(
            "%s: no `; [map] host nocarry` line -- this run was not on a transport without a bulk "
            "channel, so it cannot speak for the TCP refusal" % hl
        )
    armed = RE_SEND_ARMED.search(htext)
    if armed:
        fail("%s: the host ARMED a transfer on a no-carry transport (%s)" % (hl, armed.group(0)))
    ref = RE_START_REFUSED_NOCARRY.search(htext)
    if not ref:
        fail(
            "%s: the host never refused Start with the named no-carry notice (`start REFUSED -- "
            "'<peer>' holds a different <map> ...`)" % hl
        )
    else:
        if ref.group(2) != claim.group(1):
            fail(
                "%s: the refusal names map '%s', not the claimed '%s'"
                % (hl, ref.group(2), claim.group(1))
            )
        note("host REFUSED Start: '%s' holds a different %s" % (ref.group(1), ref.group(2)))
    if RE_START_OK.search(htext):
        fail("%s: the host logged `start OK` -- the gate opened for a mismatching joiner" % hl)
    click = RE_CLICK_REFUSED.search(htext)
    if not click:
        fail(
            "%s: no `start CLICK REFUSED` line -- either nobody clicked Start (the refusal was never "
            "tested) or the click went through" % hl
        )
    else:
        note("host swallowed a Start click for '%s'" % click.group(1))
    if not clients:
        fail("no client log was given -- cannot show the joiner never started")
    for p, t in clients:
        name = peer_label(p)
        if not RE_WANT.search(t):
            fail("%s: no `; [map] client want` -- the joiner never learned the host's claim" % name)
        if RE_STORED.search(t):
            fail("%s: the joiner STORED a map over a no-carry transport" % name)
        after = [b for b in RE_LOCAL.findall(t) if b[0] == "after"]
        if after:
            fail(
                "%s: a `local after` sample exists -- the joiner saw the host's Start, so the "
                "refusal did not hold" % name
            )
        else:
            note("%s: never reached Start (no `local after`), stored nothing" % name)
    for n in NOTES:
        print("  %s" % n)
    if FAILS:
        print("check_map_transfer: FAIL")
        for f in FAILS:
            print("  - %s" % f)
        return 1
    print("check_map_transfer: OK")
    return 0


# ---- the reader's own negative cases -------------------------------------------------------------
#
# A POST-CHECK IS AN ORACLE, AND AN ORACLE THAT CANNOT GO RED IS DECORATION. Every clause above
# exists to catch a specific wrong run, so each one is driven here against a planted log pair that
# is wrong in exactly that way -- and against the right pair, which must stay green. The planted
# logs are real files in a temp directory laid out the way a lane is (`<lane>/logs/<run>/mh_net.log`),
# because the directory walk that stitches a peer's menu log to its session log is itself one of the
# things that was wrong once and read as "the host made no claim".

HASH_OK = "d3c5707f54d7ecfb"
HASH_NO = "49475958b6537b6e"

HOST_MENU = "; [map] host claim blue monday.mpm sha=%s size=462065\n" % HASH_OK
HOST_RUN = (
    "; [map] peer 1 'client' needs the map (has=none want=%s)\n"
    "; [map] send armed to peer 1 'client' (462065 B of blue monday.mpm)\n"
    "; [map] start REFUSED -- waiting for 'client' to finish downloading the map\n"
    "; [map] peer 1 'client' holds the map (sha=%s) -- nothing to transfer\n"
    "; [map] start OK -- every joiner reports the map we advertised\n" % (HASH_OK, HASH_OK)
)
CL_MENU = (
    "; [map] client want blue monday.mpm sha=%s size=462065\n"
    "; [map] local before blue monday.mpm sha=%s size=462065\n"
    "; [map] client resolve missing -- waiting for the host's copy over channel C\n"
    % (HASH_OK, HASH_OK)
)
CL_RUN = (
    "; [map] client stored mh_dl\\blue monday.%s.mpm (462065 B, sha=%s) -- the local blue "
    "monday.mpm is untouched\n"
    "; [map] local after blue monday.mpm sha=%s size=462065\n" % (HASH_OK, HASH_OK, HASH_OK)
)

# mp:X2e -- the absent-at-lobby-entry stage: the menu-run lines of a fixed client, and the retail
# (unfixed) client's, which raises the error box and never stores.
EN_CL_MENU = (
    "; [map] uitest pretend=absent -- this client behaves as though it holds nothing\n"
    + CL_MENU
    + "; [map] uitest absent: armed at the JOIN -- the game's opens of blue monday.mpm now fail\n"
    "; [map] uitest absent: refused the game's open of Maps\\blue monday.mpm -- staged as not "
    "on this disk (mp:X2e)\n"
    "; [map] entry check blue monday.mpm: not readable here -- retail's 'can't create map file' "
    "box (text 0x30f) and the lobby teardown it runs are NOT raised\n"
)
EN_CL_RETAIL = (
    "; [map] entry check retail -- the lobby-entry map read raises retail's error box\n"
    "; [map] uitest pretend=absent -- this client behaves as though it holds nothing\n"
    + CL_MENU
    + "; [map] uitest absent: refused the game's open of Maps\\blue monday.mpm -- staged as not "
    "on this disk (mp:X2e)\n"
    "; DLG savegame_io_error caller=0x004be151 sess=2 gclk=0\n"
)

HOST_NOCARRY = "; [map] host nocarry -- this transport has no bulk channel\n"
HOST_REFUSED_TCP = (
    "; [map] peer 1 'client' needs the map (has=%s want=%s)\n"
    "; [map] start REFUSED -- 'client' holds a different blue monday.mpm and this transport "
    "cannot carry maps\n"
    "; [map] start CLICK REFUSED -- 'client' holds a different map and this transport cannot "
    "carry it; begin_map_load NOT run (mp:X2/X2b)\n" % (HASH_NO, HASH_OK)
)
CL_MENU_TCP = (
    "; [map] client want blue monday.mpm sha=%s size=462065\n"
    "; [map] local before blue monday.mpm sha=%s size=462065\n"
    "; [map] client BLOCKED blue monday.mpm -- this transport has no bulk channel\n"
    % (HASH_OK, HASH_OK)
)


# mp:X2d fixtures: the field shape (JOIN before advert) with and without the fix.
CL_HAVE = (
    "; [map] client want blue monday.mpm sha=%s size=462065\n"
    "; [map] local before blue monday.mpm sha=%s size=462065\n"
    "; [map] client resolve base -- the local file already IS the host's content\n"
    % (HASH_OK, HASH_OK)
)
EJ_CL_MENU = (
    "; [map] uitest advert_hold: first SESSION_INFO from 0 HELD until a JOIN has gone out "
    "(+250 ms)\n" + CL_HAVE + "; [map] uitest advert_hold: released after 612 ms (a JOIN went "
    "out first)\n"
)
EJ_CL_RUN = "; [map] local after blue monday.mpm sha=%s size=462065\n" % HASH_OK
EJ_HOST_OK = "; [map] peer 1 'client' holds the map (sha=%s) -- nothing to transfer\n" % HASH_OK
EJ_HOST_BUG = (
    "; [map] peer 1 'client' needs the map (has=none want=%s)\n"
    "; [map] send armed to peer 1 'client' (462065 B of blue monday.mpm)\n" % HASH_OK
)


# mp:X2f clause (3) fixtures: client1 joins and downloads (armed once), leaves + its process exits
# mid-transfer -- the transport-layer release evidence (client1's link drops before client2 arms;
# LR_HOST_RUN_SUPERSEDED below is the other order, the one the rig measured) -- and
# client2's fresh join arms a SECOND transfer that completes (one `client stored`, on client2's log).
LR_HOST_RUN = (
    "; [map] peer 1 'client' needs the map (has=none want=%s)\n"
    "; [map] send armed to peer 1 'client' (462065 B of blue monday.mpm)\n"
    "; [map] start REFUSED -- waiting for 'client' to finish downloading the map\n"
    "net: udp conn 0 dropped -- no data from peer within the link timeout (keepalives: sent 2, "
    "received 1)\n"
    "net: udp conn 0's channel-C transfer cancelled with it -- its destination is gone, and the "
    "next peer on this index must not inherit it (mp:X2f)\n"
    "; [map] peer 1 'client' needs the map (has=none want=%s)\n"
    "; [map] send armed to peer 1 'client' (462065 B of blue monday.mpm)\n"
    "; [map] peer 1 'client' holds the map (sha=%s) -- nothing to transfer\n"
    "; [map] start OK -- every joiner reports the map we advertised\n" % (HASH_OK, HASH_OK, HASH_OK)
)
# The order the rig row measured (2026-09-27): client2 is admitted on conn 1 while client1 still sits
# linked on its browser, so client2's arm SUPERSEDES client1's unfinished transfer; conn 0 drops later
# with nothing left on it to cancel.
LR_HOST_RUN_SUPERSEDED = (
    "; [map] peer 1 'client' needs the map (has=none want=%s)\n"
    "; [map] send armed to peer 1 'client' (462065 B of blue monday.mpm)\n"
    "; [map] start REFUSED -- waiting for 'client' to finish downloading the map\n"
    "; [map] start OK -- every joiner reports the map we advertised\n"
    "; [map] peer 2 'client2' needs the map (has=none want=%s)\n"
    "net: snapshot SEND armed -- 462065 B in 29 body + 1 manifest chunk(s) to player 2, root 5059f1a8\n"
    "net: snapshot SEND to player 2 superseded the UNFINISHED transfer to player 1 -- channel C "
    "detached from it before its copy was freed (mp:X2f)\n"
    "; [map] send armed to peer 2 'client2' (462065 B of blue monday.mpm)\n"
    "; [map] peer 2 'client2' holds the map (sha=%s) -- nothing to transfer\n"
    "net: udp conn 0 dropped -- no data from peer within the link timeout (keepalives: sent 24, "
    "received 28)\n"
    "; [map] start OK -- every joiner reports the map we advertised\n" % (HASH_OK, HASH_OK, HASH_OK)
)
# The field bug's shape: only ONE armed line, and the host times out and re-arms into the SAME
# (never-released) seat instead of logging either release line -- the crash this rig row exists to
# rule out. Kept as the checker's own negative case; the pre-fix DLL never reaches this rig row.
LR_HOST_RUN_NO_GONE = (
    "; [map] peer 1 'client' needs the map (has=none want=%s)\n"
    "; [map] send armed to peer 1 'client' (462065 B of blue monday.mpm)\n"
    "; [map] start REFUSED -- waiting for 'client' to finish downloading the map\n"
    "; [map] send timed out for peer 1 after 90000 ms -- re-arming\n"
    "; [map] peer 1 'client' holds the map (sha=%s) -- nothing to transfer\n"
    "; [map] start OK -- every joiner reports the map we advertised\n" % (HASH_OK, HASH_OK)
)


def _plant(root, lane, runs):
    """Lay a peer out the way a lane is: <lane>/logs/<run>/mh_net.log, one file per run."""
    out = None
    for i, text in enumerate(runs):
        # A ("MENU", text) entry is laid out as a process's first (`*_menu_*`) directory -- how the
        # shared-lane case plants an EARLIER process's runs ahead of this one's.
        tag = "menu" if isinstance(text, tuple) else "sess"
        text = text[1] if isinstance(text, tuple) else text
        d = os.path.join(root, lane, "logs", "20260101T0000%02dZ_%s_run%d" % (i, tag, i))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
            fh.write(text)
        out = d
    return out  # the NEWEST run, which is what post_check_peers hands over


def selftest():
    import shutil
    import tempfile

    cases = [
        # (name, host runs, client runs, argv flags, expected rc)
        (
            "the honest download run is GREEN",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN],
            ["--expect-gate"],
            0,
        ),
        (
            "a host that made NO CLAIM is red (every other clause would pass vacuously)",
            ["; [map] host noclaim -- the packs answer for this name\n", HOST_RUN],
            [CL_MENU, CL_RUN],
            ["--expect-gate"],
            1,
        ),
        (
            "a client that stored NOTHING is red",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, ""],
            ["--expect-gate"],
            1,
        ),
        (
            "a download stored under the BASE NAME is red -- the one failure that looks like success",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN.replace("mh_dl\\blue monday.%s.mpm" % HASH_OK, "blue monday.mpm")],
            ["--expect-gate"],
            1,
        ),
        (
            "a download stored BESIDE the player's maps is red (the picker would list it)",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN.replace("mh_dl\\blue", "Maps\\blue")],
            ["--expect-gate"],
            1,
        ),
        (
            "THE PLAYER'S OWN FILE CHANGED is red",
            [HOST_MENU, HOST_RUN],
            [
                CL_MENU,
                CL_RUN.replace(
                    "local after blue monday.mpm sha=%s" % HASH_OK,
                    "local after blue monday.mpm sha=%s" % HASH_NO,
                ),
            ],
            ["--expect-gate"],
            1,
        ),
        (
            "a stored map that does not hash to the advert is red",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN.replace("sha=%s)" % HASH_OK, "sha=%s)" % HASH_NO)],
            ["--expect-gate"],
            1,
        ),
        (
            "a run where the gate NEVER CLOSED is red under --expect-gate",
            [
                HOST_MENU,
                HOST_RUN.replace(
                    "; [map] start REFUSED -- waiting for 'client' to finish downloading the map\n",
                    "",
                ),
            ],
            [CL_MENU, CL_RUN],
            ["--expect-gate"],
            1,
        ),
        (
            "--expect-nothing: a peer that held the map and transferred nothing is GREEN",
            [
                HOST_MENU,
                "; [map] peer 1 'client' holds the map (sha=%s) -- nothing to transfer\n" % HASH_OK,
            ],
            [
                "; [map] client want blue monday.mpm sha=%s size=462065\n"
                "; [map] local before blue monday.mpm sha=%s size=462065\n"
                "; [map] client resolve base -- the local file already IS the host's content\n"
                % (HASH_OK, HASH_OK),
                "; [map] local after blue monday.mpm sha=%s size=462065\n" % HASH_OK,
            ],
            ["--expect-nothing"],
            0,
        ),
        (
            "--expect-nothing: a transfer that WAS armed is red",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN],
            ["--expect-nothing"],
            1,
        ),
        (
            "--expect-nothing with no `holds the map` line is red -- 'nothing transferred' and "
            "'nobody joined' must be different verdicts",
            [HOST_MENU, ""],
            [
                "; [map] client want blue monday.mpm sha=%s size=462065\n"
                "; [map] local before blue monday.mpm sha=%s size=462065\n" % (HASH_OK, HASH_OK),
                "; [map] local after blue monday.mpm sha=%s size=462065\n" % HASH_OK,
            ],
            ["--expect-nothing"],
            1,
        ),
        # ---- mp:X2d --expect-early-join (the JOIN raced ahead of the advert) ----
        (
            "--expect-early-join: the JOIN carried the hash though it beat the advert -- GREEN",
            [HOST_MENU, EJ_HOST_OK],
            [EJ_CL_MENU, EJ_CL_RUN],
            ["--expect-nothing", "--expect-early-join"],
            0,
        ),
        (
            "--expect-early-join: the field bug (has=none, a transfer armed) is red",
            [HOST_MENU, EJ_HOST_BUG],
            [EJ_CL_MENU, EJ_CL_RUN],
            ["--expect-nothing", "--expect-early-join"],
            1,
        ),
        (
            "--expect-early-join: `needs` corrected by a re-report (no send) is still red -- the "
            "JOIN itself must carry the hash",
            [HOST_MENU, EJ_HOST_BUG.split("; [map] send armed")[0] + EJ_HOST_OK],
            [EJ_CL_MENU, EJ_CL_RUN],
            ["--expect-nothing", "--expect-early-join"],
            1,
        ),
        (
            "--expect-early-join: no hold line (knob not in effect) is red -- the race was not staged",
            [HOST_MENU, EJ_HOST_OK],
            [CL_HAVE, EJ_CL_RUN],
            ["--expect-nothing", "--expect-early-join"],
            1,
        ),
        (
            "--expect-early-join: a hold released by the cap (no JOIN first) is red",
            [HOST_MENU, EJ_HOST_OK],
            [EJ_CL_MENU.replace("a JOIN went out first", "no JOIN within the cap"), EJ_CL_RUN],
            ["--expect-nothing", "--expect-early-join"],
            1,
        ),
        # ---- mp:X2e --expect-entry-deferred (the map absent to the game at lobby entry) ----
        (
            "--expect-entry-deferred: the check deferred and ONE join downloaded it -- GREEN",
            [HOST_MENU, HOST_RUN],
            [EN_CL_MENU, CL_RUN],
            ["--expect-gate", "--expect-entry-deferred"],
            0,
        ),
        (
            "--expect-entry-deferred: the rc4 field shape (retail box, no store) is red",
            [HOST_MENU, HOST_RUN],
            [EN_CL_RETAIL, ""],
            ["--expect-gate", "--expect-entry-deferred"],
            1,
        ),
        (
            "--expect-entry-deferred: no refused open (the stage never met an absent file) is red",
            [HOST_MENU, HOST_RUN],
            [EN_CL_MENU.replace("refused the game's open", "(none)"), CL_RUN],
            ["--expect-gate", "--expect-entry-deferred"],
            1,
        ),
        (
            "--expect-entry-deferred: a download that took a second join (two `client want`) is red",
            [HOST_MENU, HOST_RUN],
            [EN_CL_MENU + CL_MENU, CL_RUN],
            ["--expect-gate", "--expect-entry-deferred"],
            1,
        ),
        (
            "--expect-entry-deferred: a pretend=none run (not staged absent) is red",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN],
            ["--expect-gate", "--expect-entry-deferred"],
            1,
        ),
        # ---- mp:X2b --expect-refused (TCP, joiner holds different bytes) ----
        (
            "--expect-refused: the honest TCP refusal is GREEN",
            [HOST_MENU + HOST_NOCARRY, HOST_REFUSED_TCP],
            [CL_MENU_TCP, ""],
            ["--expect-refused"],
            0,
        ),
        (
            "--expect-refused: no refusal line is red",
            [HOST_MENU + HOST_NOCARRY, ""],
            [CL_MENU_TCP, ""],
            ["--expect-refused"],
            1,
        ),
        (
            "--expect-refused: a run NOT on a no-carry transport is red",
            [HOST_MENU, HOST_REFUSED_TCP],
            [CL_MENU_TCP, ""],
            ["--expect-refused"],
            1,
        ),
        (
            "--expect-refused: a joiner that reached Start (local after) is red",
            [HOST_MENU + HOST_NOCARRY, HOST_REFUSED_TCP],
            [CL_MENU_TCP, "; [map] local after blue monday.mpm sha=%s size=462065\n" % HASH_OK],
            ["--expect-refused"],
            1,
        ),
        (
            "--expect-refused: a refusal nobody clicked through (no CLICK REFUSED) is red",
            [HOST_MENU + HOST_NOCARRY, HOST_REFUSED_TCP.split("; [map] start CLICK")[0]],
            [CL_MENU_TCP, ""],
            ["--expect-refused"],
            1,
        ),
        (
            "--expect-refused: a transfer armed on TCP is red",
            [HOST_MENU + HOST_NOCARRY, HOST_REFUSED_TCP + HOST_RUN],
            [CL_MENU_TCP, ""],
            ["--expect-refused"],
            1,
        ),
        (
            # The 2026-09-24 gate red: map_refuse_tcp borrowed match_launch's lanes (since
            # TL-SUITE-FOLD-ML, d28_canceltask's), whose EARLIER process started a match (a `local
            # after`). Only this process's runs -- from its own menu directory on -- may be read, so
            # the honest refusal stays GREEN on a shared lane.
            "--expect-refused: an EARLIER process's `local after` on a SHARED lane is not read",
            [HOST_MENU + HOST_NOCARRY, HOST_REFUSED_TCP],
            [
                ("MENU", CL_MENU),
                "; [map] local after blue monday.mpm sha=%s size=462065\n" % HASH_OK,
                ("MENU", CL_MENU_TCP),
                "",
            ],
            ["--expect-refused"],
            0,
        ),
        # ---- mp:X2f clause (3) --expect-leave-rejoin (client1 leaves mid-download + exits its
        # process; a fresh client2 re-joins and finishes it) ----
        (
            "--expect-leave-rejoin: two armed transfers, the fix's release evidence, ONE stored "
            "across both client logs -- GREEN",
            [HOST_MENU, LR_HOST_RUN],
            [CL_MENU, ""],  # client1: joined, never finished, never stored
            [CL_MENU, CL_RUN],  # client2: fresh join, completed
            ["--expect-leave-rejoin"],
            0,
        ),
        (
            "--expect-leave-rejoin: client2's arm superseded client1's unfinished transfer (the rig's "
            "measured order) -- GREEN",
            [HOST_MENU, LR_HOST_RUN_SUPERSEDED],
            [CL_MENU, ""],
            [CL_MENU, CL_RUN],
            ["--expect-leave-rejoin"],
            0,
        ),
        (
            "--expect-leave-rejoin: only ONE armed line is red -- client2's fresh join never got "
            "its own send",
            [
                HOST_MENU,
                "; [map] peer 1 'client' needs the map (has=none want=%s)\n"
                "; [map] send armed to peer 1 'client' (462065 B of blue monday.mpm)\n"
                "; [map] start REFUSED -- waiting for 'client' to finish downloading the map\n"
                "net: udp conn 0's channel-C transfer cancelled with it -- its destination is gone, "
                "and the next peer on this index must not inherit it (mp:X2f)\n"
                "; [map] peer 1 'client' holds the map (sha=%s) -- nothing to transfer\n"
                "; [map] start OK -- every joiner reports the map we advertised\n"
                % (HASH_OK, HASH_OK),
            ],
            [CL_MENU, ""],
            [CL_MENU, CL_RUN],
            ["--expect-leave-rejoin"],
            1,
        ),
        (
            "--expect-leave-rejoin: no release-evidence line at all is red -- the field bug's own "
            "shape (a stale re-arm instead of a safely detached transfer)",
            [HOST_MENU, LR_HOST_RUN_NO_GONE],
            [CL_MENU, ""],
            [CL_MENU, CL_RUN],
            ["--expect-leave-rejoin"],
            1,
        ),
        (
            "--expect-leave-rejoin: TWO `client stored` lines (across both logs) is red -- client1's "
            "interrupted attempt must never have landed a complete file",
            [HOST_MENU, LR_HOST_RUN],
            [CL_MENU, CL_RUN],  # client1 ALSO stored -- the drop never really interrupted it
            [CL_MENU, CL_RUN],
            ["--expect-leave-rejoin"],
            1,
        ),
        (
            "--expect-leave-rejoin: ZERO `client stored` lines (across both logs) is red -- neither "
            "client ever finished the download",
            [HOST_MENU, LR_HOST_RUN],
            [CL_MENU, ""],
            [CL_MENU, ""],
            ["--expect-leave-rejoin"],
            1,
        ),
    ]

    root = tempfile.mkdtemp(prefix="mh_maptransfer_selftest_")
    bad = 0
    try:
        for i, case_tuple in enumerate(cases):
            # A 6th element is a SECOND client's runs (mp:X2f clause (3): client1 + client2, two
            # separate peer logs) -- absent for every other flag's single-client cases.
            if len(case_tuple) == 6:
                name, host_runs, cl_runs, cl2_runs, flags, want = case_tuple
            else:
                name, host_runs, cl_runs, flags, want = case_tuple
                cl2_runs = None
            case = os.path.join(root, "c%d" % i)
            h = _plant(case, "host", host_runs)
            c = _plant(case, "client", cl_runs)
            argv = flags + [h, c]
            if cl2_runs is not None:
                argv.append(_plant(case, "client2", cl2_runs))
            got = main(argv)
            ok = got == want
            if not ok:
                bad += 1
            print("  %-4s %s (rc=%s, wanted %s)" % ("ok" if ok else "FAIL", name, got, want))
        # 2026-09-24: post_check_peers (no post_check_session) hands over the MENU dir, and the
        # match lines are in the LATER session dirs -- which must be read too. Cutting the walk at
        # the given dir made all five map rows red ("the host never armed a transfer").
        case = os.path.join(root, "menu_handed")
        _plant(case, "host", [("MENU", HOST_MENU), HOST_RUN])
        _plant(case, "client", [("MENU", CL_MENU), CL_RUN])

        def _menu(lane):
            logs = os.path.join(case, lane, "logs")
            return os.path.join(logs, sorted(n for n in os.listdir(logs) if "_menu_" in n)[0])

        got = main(["--expect-gate", _menu("host"), _menu("client")])
        ok = got == 0
        bad += 0 if ok else 1
        cases.append(None)
        print(
            "  %-4s --expect-gate: the MENU dir handed over, session dirs after it are read (rc=%s)"
            % ("ok" if ok else "FAIL", got)
        )
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print(
        "check_map_transfer --selftest: %s (%d case(s), %d failure(s))"
        % ("PASS" if bad == 0 else "FAIL", len(cases), bad)
    )
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
