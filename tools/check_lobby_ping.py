#!/usr/bin/env python3
"""check_lobby_ping -- mp:L1b: the lobby slot-row panel's per-slot ping cell really ran, and at
least one connected peer's row carried a real measurement.

WHY A CHECKER AND NOT A PIXEL DIFF, same reasoning as check_net_indicator.py (mp:L1): the ping
digits differ on every run, so a capture baseline can prove the CELL is drawn (with the number
masked) but not that the number means anything. This reads the `; [lobbyping]` lines
src/mh_dll/mh/ui/lobby_ping.cpp writes into mh_net.log (net.lobbyping_sample in
tools/data/log_formats.json) -- one line per occupied HUMAN slot (other than the local player's own
row), every time the lobby's own per-frame tick refreshes the column.

  python tools/check_lobby_ping.py <run dir>                  # the column drew, and something was measured
  python tools/check_lobby_ping.py <host dir> <client dir>     # pooled across peers (mp:L1e shape)
  python tools/check_lobby_ping.py <run dir> --srtt 0 250      # ... and it sat in a band
  python tools/check_lobby_ping.py <h> <c1> <c2> \
      --published --every-slot --agree 250                     # mp:L1f, the 3-peer shape

`<run dir>` is a session directory holding mh_net.log, or any directory above one (the newest
mh_net.log under it wins) -- the same CONTENT-FIRST search check_net_indicator.py uses, because the
same trap applies here: tools/test_ui.py hands a post_check the "menu" session directory, and the
lobby ping lines belong to whichever session actually held the lobby open.

WHAT "SANE" MEANS: at least one `; [lobbyping]` line was written at all (the tick ran and saw an
occupied human slot), and at least one of them has measured=1 with srtt_ms >= 0 (some connected
peer's row actually carried a number, not permanently `n/a`).

---- mp:L1f, AND WHY IT NEEDS ITS OWN CLAUSES --------------------------------------------------
The restored transport is a client-server STAR: a client holds exactly ONE connection, to the host,
so its MH_NetStats::lat[] has one row and it can NEVER measure another client. Before L1f every
other occupied row was blank on a client's screen and this checker's existing clauses were happy --
"something was measured somewhere in the pool" is satisfied by the host alone. The three flags
above are the claims that pooled reading cannot make:

  --published   the host really broadcast its summary (`; [lobbypub]`, net.lobbypub) AND some peer
                really rendered a row from it (`src=host` on a `; [lobbyping]` line).
  --every-slot  PER PEER LOG: every other occupied slot that peer's own tick saw carried a real
                measurement at least once, and it saw at least one per other pooled peer. This is
                the "every player sees every other player's ping" claim, asserted per screen rather
                than over the pool.
  --agree MS    a published number for player P and the number the peer that owns that link
                measured for P are within MS of each other -- so a published value that is real but
                WRONG (the wrong slot, a stale table, a units mix-up) still fails.
"""

import argparse
import os
import re
import sys
import tempfile

SAMPLE_RE = re.compile(
    r"; \[lobbyping\] slot=(-?\d+) pid=(-?\d+) measured=(\d+) srtt_ms=(-?\d+)"
    r"(?: relayed=(-?\d+))?(?: src=(\w+))?"
)
# mp:L1f -- the HOST's send side: `; [lobbypub] entries=<n> bytes=<n>`, ~1 Hz while it holds a lobby.
PUB_RE = re.compile(r"; \[lobbypub\] entries=(\d+) bytes=(\d+)")


def find_log(target):
    """Same content-first search as check_net_indicator.find_log -- see that docstring. The lobby
    ping lines are written to the SAME mh_net.log, from the SAME per-session log writer
    (net_seams.cpp), so a target that is itself a session directory widens to its siblings (the
    lane's other session runs) when its own log carries no `[lobbyping]` line; otherwise the newest
    log anywhere under the target wins, so a run where the column really never drew still fails on
    its own merits rather than on a missing file."""
    if os.path.isfile(target):
        return target

    # mp:L1f -- THE TARGET'S OWN LOG WINS WHENEVER IT CARRIES THE CONTENT, checked before any
    # widening. The widening below exists for a real case (the lobby lines can land in a SIBLING
    # session directory to the one test_ui names) but it is a menace when several peers' logs are
    # POOLED: three sibling directories, each holding its own mh_net.log, resolved to ONE log --
    # the newest -- for all three targets, and the 3-peer run then read 20 lines three times and
    # reported "0 host publications" because the host's log was never opened (measured on the first
    # green mp:L1f rig run, 2026-09-22). A directory that answers the question itself is never
    # ambiguous, so it is answered first.
    own = os.path.join(target, "mh_net.log")
    if os.path.isfile(own):
        try:
            with open(own, "r", encoding="utf-8", errors="replace") as fh:
                if "[lobbyping]" in fh.read():
                    return own
        except OSError:
            pass

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
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as fh:
                if "[lobbyping]" in fh.read():
                    return p
        except OSError:
            continue
    return cands[0]


NL = chr(10)
GOOD = (
    "[00:00:10.000] ; [lobbyping] slot=1 pid=0 measured=1 srtt_ms=4"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=1 pid=0 measured=1 srtt_ms=5"
    + NL
)
NEVER_MEASURED = (
    "[00:00:10.000] ; [lobbyping] slot=1 pid=0 measured=0 srtt_ms=-1"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=1 pid=0 measured=0 srtt_ms=-1"
    + NL
)
# mp:L1e -- a CLIENT's row: relayed is always a real 0/1 (MH_Seam_ClientDialIsRelayed, no bridge
# needed). Direct here; GOOD_RELAYED is the relayed=1 twin used below.
GOOD_DIRECT = (
    "[00:00:10.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=4 relayed=0"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=5 relayed=0"
    + NL
)
GOOD_RELAYED = (
    "[00:00:10.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=44 relayed=1"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=45 relayed=1"
    + NL
)
# mp:L1e -- a HOST's row TODAY: measured, but the per-peer relay classification is not yet bridged
# (udp_endpoint.cpp's get_stats always answers -1 for now), so no letter -- a real, expected state,
# not a bug this checker should flag on its own.
GOOD_UNKNOWN = (
    "[00:00:10.000] ; [lobbyping] slot=1 pid=1 measured=1 srtt_ms=6 relayed=-1"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=1 pid=1 measured=1 srtt_ms=7 relayed=-1"
    + NL
)

# ---- mp:L1f fixtures: a 3-peer lobby, host (pid 0) + client1 (pid 1) + client2 (pid 2) ----------
# THE HOST measures both clients itself (src=self) and publishes them (`; [lobbypub] entries=2`).
L1F_HOST = (
    "[00:00:10.000] ; [lobbypub] entries=2 bytes=10"
    + NL
    + "[00:00:10.000] ; [lobbyping] slot=1 pid=1 measured=1 srtt_ms=8 relayed=0 src=self"
    + NL
    + "[00:00:10.000] ; [lobbyping] slot=2 pid=2 measured=1 srtt_ms=12 relayed=0 src=self"
    + NL
    + "[00:00:11.000] ; [lobbypub] entries=2 bytes=10"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=1 pid=1 measured=1 srtt_ms=9 relayed=0 src=self"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=2 pid=2 measured=1 srtt_ms=13 relayed=0 src=self"
    + NL
)
# CLIENT 1 measures the host on its own single connection and takes client 2 from the publication.
L1F_CLIENT1 = (
    "[00:00:10.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=8 relayed=0 src=self"
    + NL
    + "[00:00:10.000] ; [lobbyping] slot=2 pid=2 measured=1 srtt_ms=12 relayed=0 src=host"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=9 relayed=0 src=self"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=2 pid=2 measured=1 srtt_ms=13 relayed=0 src=host"
    + NL
)
L1F_CLIENT2 = (
    "[00:00:10.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=12 relayed=0 src=self"
    + NL
    + "[00:00:10.000] ; [lobbyping] slot=1 pid=1 measured=1 srtt_ms=8 relayed=0 src=host"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=13 relayed=0 src=self"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=1 pid=1 measured=1 srtt_ms=9 relayed=0 src=host"
    + NL
)
# THE PRE-L1f STATE, which every OTHER clause in this file passes: the clients see the host's row
# and nothing else, because a client cannot measure another client. `--every-slot` is the clause
# that catches it -- the blank row is present in the log as measured=0.
L1F_CLIENT1_BLANK = (
    "[00:00:10.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=8 relayed=0 src=self"
    + NL
    + "[00:00:10.000] ; [lobbyping] slot=2 pid=2 measured=0 srtt_ms=-1 relayed=-2 src=none"
    + NL
)
# A published number that is REAL but WRONG (10x out -- a units slip, a stale table, the wrong pid).
L1F_CLIENT1_SKEWED = (
    "[00:00:10.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=8 relayed=0 src=self"
    + NL
    + "[00:00:10.000] ; [lobbyping] slot=2 pid=2 measured=1 srtt_ms=1200 relayed=0 src=host"
    + NL
)
# The host measured and published, but no client consumed it (a decoder/dispatch regression).
L1F_CLIENT1_NOCONSUME = (
    "[00:00:10.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=8 relayed=0 src=self"
    + NL
    + "[00:00:12.000] ; [lobbyping] slot=0 pid=0 measured=1 srtt_ms=9 relayed=0 src=self"
    + NL
)


def selftest():
    import subprocess

    cases = [
        ("healthy", GOOD, [], 0),
        ("no lines at all", "", [], 1),
        ("never measured (permanently n/a)", NEVER_MEASURED, [], 1),
        ("srtt band met", GOOD, ["--srtt", "0", "250"], 0),
        ("srtt band missed", GOOD, ["--srtt", "150", "250"], 1),
        (
            "srtt band no measured sample",
            NEVER_MEASURED,
            ["--srtt", "0", "250"],
            1,
        ),
        # mp:L1e -- a SINGLE target never gets the pooled relay-letter check even when every sample is
        # unknown (host-only capture, mp:L1e not yet bridged) -- see the len(a.targets)>=2 gate.
        ("single target, unknown-only relay -> no letter requirement", GOOD_UNKNOWN, [], 0),
    ]
    multi_cases = [
        ("multi: one peer measured", [NEVER_MEASURED, GOOD], [], 0),
        ("multi: neither peer measured", [NEVER_MEASURED, NEVER_MEASURED], [], 1),
        # mp:L1e -- the new pooled shape: a client's real 0/1 proves the R/D letter fired even though
        # the host's own row is honestly unknown (mp:L1e, not yet bridged).
        ("multi: client direct, host unknown -> letter proven", [GOOD_UNKNOWN, GOOD_DIRECT], [], 0),
        (
            "multi: client relayed, host unknown -> letter proven",
            [GOOD_UNKNOWN, GOOD_RELAYED],
            [],
            0,
        ),
        # both sides unknown (e.g. two hosts' own rows, or a module pre-dating mp:L1e) -- the letter
        # never fires anywhere, which IS a real fail for a pooled (2+-target) check.
        ("multi: both sides unknown -> letter never fires", [GOOD_UNKNOWN, GOOD_UNKNOWN], [], 1),
        # ---- mp:L1f: the published per-slot summary --------------------------------------------
        (
            "L1f: 3-peer, every screen shows every other slot",
            [L1F_HOST, L1F_CLIENT1, L1F_CLIENT2],
            ["--published", "--every-slot", "--agree", "250"],
            0,
        ),
        # THE REGRESSION THIS ROW EXISTS FOR, and the proof the clause has teeth: the pre-L1f
        # behaviour passes every OTHER clause in this file (the host measured plenty) and fails
        # --every-slot, because client1's own screen never carried client2's number.
        (
            "L1f: pre-L1f blank other-client row is caught",
            [L1F_HOST, L1F_CLIENT1_BLANK, L1F_CLIENT2],
            ["--every-slot"],
            1,
        ),
        (
            "L1f: nothing consumed the publication -> --published fails",
            [L1F_HOST, L1F_CLIENT1_NOCONSUME],
            ["--published"],
            1,
        ),
        (
            "L1f: no `[lobbypub]` anywhere -> --published fails",
            [L1F_CLIENT1, L1F_CLIENT2],
            ["--published"],
            1,
        ),
        # A real-but-wrong published number: --published and --every-slot both pass, --agree does not.
        (
            "L1f: a skewed published value passes --published",
            [L1F_HOST, L1F_CLIENT1_SKEWED],
            ["--published"],
            0,
        ),
        (
            "L1f: ...and is caught by --agree",
            [L1F_HOST, L1F_CLIENT1_SKEWED],
            ["--agree", "250"],
            1,
        ),
        (
            "L1f: --agree with nothing measured both ways refuses",
            [L1F_HOST, L1F_CLIENT1_NOCONSUME],
            ["--agree", "250"],
            1,
        ),
    ]
    fails = []
    # ONE ISOLATED TemporaryDirectory PER CASE, not siblings sharing a parent -- find_log() widens a
    # session-directory target to ITS SIBLINGS on purpose (the real usage this mirrors: the lobby
    # ping lines can land in a sibling session directory to the one the runner names), which means
    # two cases sharing a parent can answer each other's question. A case whose OWN log carries no
    # `[lobbyping]` line at all (this file's "no lines" / never-measured-looking-empty cases) would
    # silently read an EARLIER case's real content instead -- exactly the trap
    # check_net_indicator.py's find_log docstring names ("A run of a DIFFERENT scenario could answer
    # this one's question -- which it did, once, silently"). Isolating each case's tmp root removes
    # the ambiguity rather than relying on every case's fixture happening to carry the tag.
    for name, text, argv, want in cases:
        with tempfile.TemporaryDirectory() as tmp:
            d = os.path.join(tmp, "run")
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
                fh.write(text)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__), d] + argv,
                capture_output=True,
                text=True,
            )
            got = r.returncode
            print("  %-38s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d%s%s" % (name, got, want, NL, r.stdout))
    for name, texts, argv, want in multi_cases:
        with tempfile.TemporaryDirectory() as tmp:
            files = []
            for i, text in enumerate(texts):
                p = os.path.join(tmp, "peer%d.log" % i)
                with open(p, "w", encoding="utf-8") as fh:
                    fh.write(text)
                files.append(p)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__)] + files + argv,
                capture_output=True,
                text=True,
            )
            got = r.returncode
            print("  %-38s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d%s%s" % (name, got, want, NL, r.stdout))
    # ---- mp:L1f: POOLED SIBLING DIRECTORIES, the shape the rig actually hands over ---------------
    # The cases above pass FILE paths, so they never reach find_log's directory widening -- and that
    # widening is what made the first green 3-peer run read ONE peer's log three times (it resolved
    # every sibling directory to the newest log under their shared parent). These cases are three
    # sibling directories, one mh_net.log each, exactly as ui_test --pull-logs leaves them.
    dir_cases = [
        (
            "L1f dirs: three siblings resolve to their OWN logs",
            {"host": L1F_HOST, "c1": L1F_CLIENT1, "c2": L1F_CLIENT2},
            ["--published", "--every-slot", "--agree", "250"],
            0,
        ),
        # The same three directories with the HOST's publication removed: if the widening ever comes
        # back, every target resolves to a client's log, `--published` sees no `[lobbypub]` and this
        # case goes red -- which is what makes the case above a real test rather than a coincidence.
        (
            "L1f dirs: a host that never published is still caught",
            {"host": L1F_CLIENT1, "c1": L1F_CLIENT1, "c2": L1F_CLIENT2},
            ["--published"],
            1,
        ),
    ]
    for name, peers, argv, want in dir_cases:
        with tempfile.TemporaryDirectory() as tmp:
            targets = []
            for key, text in peers.items():
                d = os.path.join(tmp, key)
                os.makedirs(d, exist_ok=True)
                with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
                    fh.write(text)
                targets.append(d)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__)] + targets + argv,
                capture_output=True,
                text=True,
            )
            got = r.returncode
            print("  %-38s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d%s%s" % (name, got, want, NL, r.stdout))
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print("check_lobby_ping --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "targets",
        nargs="+",
        help="one or more session run dirs (or lane dirs above one), or mh_net.log files",
    )
    ap.add_argument(
        "--srtt",
        nargs=2,
        type=int,
        metavar=("LO", "HI"),
        help="every measured sample's srtt_ms must sit in [LO, HI]",
    )
    # ---- mp:L1f: the three clauses the published summary adds -----------------------------------
    ap.add_argument(
        "--published",
        action="store_true",
        help="mp:L1f: the HOST published (`; [lobbypub]`) AND at least one peer rendered a row from "
        "that publication (`src=host`) -- the star-topology half a client cannot measure itself",
    )
    ap.add_argument(
        "--every-slot",
        action="store_true",
        help="mp:L1f: on EVERY pooled peer's own log, every other occupied slot it saw carried a "
        "measured value at least once (and it saw at least one slot per other pooled peer)",
    )
    ap.add_argument(
        "--agree",
        type=int,
        metavar="MS",
        help="mp:L1f: a published value (src=host) for player P must sit within MS of what the "
        "peer that measured P itself (src=self) reported for it",
    )
    a = ap.parse_args()

    logs = []
    for t in a.targets:
        p = find_log(t)
        if p:
            logs.append(p)
    if not logs:
        print("REFUSED: no mh_net.log at or under %s" % ", ".join(a.targets))
        return 2
    # PER-LOG, not one concatenated blob: mp:L1f's claims are about what EACH PEER saw (every other
    # occupied slot carried a number ON ITS OWN SCREEN), which a pooled string cannot answer -- a
    # single peer measuring everything would satisfy it. The older clauses still read the pool.
    per_log = []
    for p in logs:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            t = f.read()
        per_log.append((p, SAMPLE_RE.findall(t), PUB_RE.findall(t)))
    log = ", ".join(logs)

    samples = [s for _p, ss, _pp in per_log for s in ss]
    pubs = [x for _p, _ss, pp in per_log for x in pp]
    print("read %s: %d lobby-ping lines, %d host publications" % (log, len(samples), len(pubs)))

    fails = []
    if not samples:
        fails.append(
            "no `; [lobbyping]` line at all -- the lobby's own tick never ran, or never saw an "
            "occupied HUMAN slot besides the local player's own row"
        )
    measured = [s for s in samples if int(s[2]) != 0 and int(s[3]) >= 0]
    if samples and not measured:
        fails.append(
            "%d lobby-ping lines, every one measured=0 (`n/a`) -- no connected peer's row ever "
            "carried a real number" % len(samples)
        )

    # mp:L1e -- THE NEW SHAPE: a measured sample now carries a `relayed` classification (-2 = the
    # sample predates this field / not measured, -1 = UNKNOWN, 0 = direct, 1 = relayed -- see
    # lobby_ping.cpp's header block and mh_net_export.h's MH_NetPeerLatency.relayed comment). Only
    # asserted when POOLING >=2 targets (the `<host dir> <client dir>` shape this docstring's second
    # usage line names): a single peer's log can legitimately show relayed=-1 on every sample forever
    # (the HOST side is UNWIRED as of mp:L1e -- see udp_endpoint.cpp's get_stats), so requiring a
    # letter there would fail an honest "don't know yet" answer. Pooling both peers is what proves
    # the R/D letter mechanism actually fires end-to-end: a client's row always carries a real 0/1
    # (MH_Seam_ClientDialIsRelayed, no cross-layer bridge needed), so a 2+-target run with no letter
    # anywhere means the wiring broke, not merely that the host side is still open work.
    if len(a.targets) >= 2 and measured:
        with_relay_field = [s for s in measured if s[4] != ""]  # re.findall: absent group = ''
        lettered = [s for s in with_relay_field if int(s[4]) in (0, 1)]
        if with_relay_field and not lettered:
            fails.append(
                "%d measured samples across %d targets, all `relayed` in {-2,-1} (unknown/n-a) -- "
                "the R/D letter never fired on either peer's row"
                % (len(with_relay_field), len(a.targets))
            )
        elif lettered:
            print(
                "  relay classification: %d/%d measured samples carried a real R/D (0=direct,1=relayed)"
                % (len(lettered), len(measured))
            )

    # ---- mp:L1f ---------------------------------------------------------------------------------
    # THE CLAIM: a client holds exactly ONE transport connection (to the host -- the client-server
    # star), so every other occupied row is one it physically cannot measure. The host publishes
    # those rows; these clauses assert the publication happened, was consumed, and agreed with what
    # the host itself measured.
    if a.published:
        if not pubs:
            fails.append(
                "--published: no `; [lobbypub]` line anywhere -- the HOST never broadcast its "
                "per-slot summary (is the host's log in the pool? is lat_supported set -- the TCP "
                "module measures nothing and publishes nothing?)"
            )
        rendered = [s for s in measured if s[5] == "host"]
        if not rendered:
            fails.append(
                "--published: %d measured samples, none with `src=host` -- no peer ever rendered a "
                "row from the host's publication, so every row on a client's screen is still one it "
                "measured itself (i.e. only the host's)" % len(measured)
            )
        else:
            print(
                "  published: %d host broadcast(s); %d/%d measured samples rendered from them "
                "(src=host)" % (len(pubs), len(rendered), len(measured))
            )

    if a.every_slot:
        # Per peer: every pid its own tick ever printed a line for must have carried a measured
        # value at least once. A pid that only ever shows measured=0 is a row that stayed blank on
        # that screen -- exactly the pre-L1f state, and the thing this row exists to end.
        for p, ss, _pp in per_log:
            if not ss:
                fails.append("--every-slot: %s wrote no `; [lobbyping]` line at all" % p)
                continue
            seen = {}
            for s in ss:
                pid = int(s[1])
                seen[pid] = seen.get(pid, False) or (int(s[2]) != 0 and int(s[3]) >= 0)
            blank = sorted(k for k, v in seen.items() if not v)
            if blank:
                fails.append(
                    "--every-slot: %s never measured player id(s) %s -- those rows stayed blank on "
                    "that peer's screen (saw %d slot(s) in all)"
                    % (p, ", ".join(str(b) for b in blank), len(seen))
                )
            elif len(seen) < len(per_log) - 1:
                fails.append(
                    "--every-slot: %s saw only %d other occupied slot(s); %d peers are pooled, so "
                    "it should have seen at least %d"
                    % (p, len(seen), len(per_log), len(per_log) - 1)
                )
            else:
                print(
                    "  every-slot: %s measured all %d other occupied slot(s)"
                    % (os.path.basename(os.path.dirname(p)) or p, len(seen))
                )

    if a.agree is not None:
        # The published number and the number measured on the link itself are two views of ONE RTT,
        # taken by different peers at different instants -- so they are compared with a window, not
        # for equality. A window that has to be wide is itself the finding; report the worst gap.
        self_by_pid, host_by_pid = {}, {}
        for s in measured:
            (host_by_pid if s[5] == "host" else self_by_pid).setdefault(int(s[1]), []).append(
                int(s[3])
            )
        shared = sorted(set(self_by_pid) & set(host_by_pid))
        if not shared:
            fails.append(
                "--agree %d: no player id was BOTH measured directly (src=self) and published "
                "(src=host) in this pool -- nothing to compare" % a.agree
            )
        worst = None
        for pid in shared:
            # min-to-min: SRTT is a smoothed floor-ish quantity and both peers sample it over the
            # same lobby, so the best sample each saw is the fairest pairing -- comparing maxima
            # would be a comparison of the two peers' worst scheduling hiccups.
            gap = abs(min(self_by_pid[pid]) - min(host_by_pid[pid]))
            if worst is None or gap > worst[1]:
                worst = (pid, gap)
            if gap > a.agree:
                fails.append(
                    "--agree %d: player %d -- measured %d ms directly, published %d ms (gap %d ms)"
                    % (a.agree, pid, min(self_by_pid[pid]), min(host_by_pid[pid]), gap)
                )
        if worst and worst[1] <= a.agree:
            print(
                "  agree: %d player id(s) seen both ways, worst gap %d ms (player %d), window %d ms"
                % (len(shared), worst[1], worst[0], a.agree)
            )

    if a.srtt:
        lo, hi = a.srtt
        out = [s for s in measured if not (lo <= int(s[3]) <= hi)]
        if not measured:
            fails.append("--srtt %d..%d: no measured sample to check" % (lo, hi))
        elif out:
            fails.append(
                "--srtt %d..%d: %d/%d measured samples outside the band (e.g. srtt_ms=%s)"
                % (lo, hi, len(out), len(measured), out[0][3])
            )
        else:
            print(
                "  srtt band %d..%d: all %d measured samples inside (min %d, max %d)"
                % (
                    lo,
                    hi,
                    len(measured),
                    min(int(s[3]) for s in measured),
                    max(int(s[3]) for s in measured),
                )
            )

    for f in fails:
        print("FAIL: %s" % f)
    print("check_lobby_ping: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
