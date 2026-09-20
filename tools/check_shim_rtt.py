#!/usr/bin/env python3
"""check_shim_rtt -- tooling:TL-SHIMUDP-C: a shimmed UDP match really measured a delayed round trip,
not a bypassed one.

WHY A CHECKER, NOT PIXELS. TL-SHIMUDP wired `tools/net_shim.py` into ui_test's `--shim-delay` so a
run's UDP traffic is genuinely routed through an added-latency proxy -- but nothing in the registry
EXERCISED that wiring on `transport=udp` with a plain per-packet delay (only `link_death` uses the
shim, and it arms `stall`, which the udp shim refuses -- see net_shim.py). A scenario can look green
while the shim was silently skipped (bind failure, a stale `--shim` flag lost between processes, a
transport that fell back to tcp) and nothing on screen would show it: the match still plays, the
frame still captures clean, and the only trace of "the delay never happened" is the ROUND-TRIP TIME
the transport itself measured -- `srtt0_ms` in mh_lockstep.log (net_lockstep.cpp's own RFC 6298
estimator, the same column L1's connection indicator reads). A shimmed run's median srtt sits close
to twice the configured one-way `shim_delay`; a bypassed one sits at the LAN's real round trip, a few
ms. This is the assertion `net_hud`'s pixel test cannot make (it does not pin a delay) and the shim
harness itself cannot make either (it delays packets; it does not read back what the GAME measured).

    python tools/check_shim_rtt.py <run dir>                    # some real (non-n/a) srtt was measured
    python tools/check_shim_rtt.py <run dir> --min-srtt 20       # ... and it is not near-zero (bypassed)
    python tools/check_shim_rtt.py --selftest

`<run dir>` is a session directory holding mh_lockstep.log, or any directory above one (the newest
log under it that actually carries measured rows wins) -- same shape as check_net_indicator's
`find_log`, and for the same reason: test_ui.py's post_check hands over the PROCESS ("menu") run
directory, and the match this checker cares about is a SIBLING session.

THE DEFAULT THRESHOLD (20 ms) is not the configured shim delay -- it is a discriminator between "the
shim ran" and "the shim was bypassed", chosen with margin on both sides: TL-SHIMUDP-C's registered
row pins `shim_delay=40` (one-way), so a real run's median srtt sits near 80 ms; a bypassed run
(direct LAN or loopback) sits under 5 ms. 20 ms is comfortably between the two without being pinned
to one exact shim configuration -- a caller measuring a different delay passes `--min-srtt` to match.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_pacing_report as pacing  # noqa: E402 -- reuse the ONE lockstep-column reader (log_formats.json)

DEFAULT_MIN_SRTT_MS = 20


def _has_measured_rows(path):
    rows, names = pacing.read_lockstep(path)
    if not names or "srtt0_ms" not in names:
        return False
    return any(r.get("srtt0_ms") not in (None, "n/a") for r in rows)


def find_log(target):
    """The mh_lockstep.log this checker's subject is actually IN -- see the module docstring for why
    a bare newest-file search is not enough (test_ui hands over the boot/menu session, not the match
    one). Mirrors check_net_indicator.find_log's shape -- WITH ONE DELIBERATE DIFFERENCE: the
    widen-to-siblings trigger cannot be "does `target` already have mh_lockstep.log" the way
    check_net_indicator gates on mh_net.log, because mh_lockstep.log is NOT universally present --
    lockstep pacing only runs during an actual match, so the boot/menu session test_ui hands over
    never has one at all (measured: its session dir carries mh_net.log, mh_launch.log, etc., but no
    mh_lockstep.log). Gating on the target file's own presence is circular here -- it would never
    widen for exactly the case this function exists to handle. mh_launch.log IS written by every
    session (boot and match alike), so it is what triggers the sibling search instead."""
    if os.path.isfile(target):
        return target
    roots = [target]
    if os.path.isfile(os.path.join(target, "mh_launch.log")):
        roots.append(os.path.dirname(os.path.abspath(target)))
    cands = []
    for d in roots:
        if not os.path.isdir(d):
            continue
        for root, _dirs, files in os.walk(d):
            if "mh_lockstep.log" in files:
                cands.append(os.path.join(root, "mh_lockstep.log"))
    if not cands:
        return None
    cands = sorted(set(cands), key=os.path.getmtime, reverse=True)
    for p in cands:
        if _has_measured_rows(p):
            return p
    return cands[0]


def measured_srtt(rows):
    out = []
    for r in rows:
        raw = r.get("srtt0_ms")
        if raw is None or raw == "n/a":
            continue
        try:
            out.append(float(raw))
        except ValueError:
            continue
    return out


def median(vals):
    s = sorted(vals)
    n = len(s)
    if n == 0:
        return float("nan")
    mid = n // 2
    return s[mid] if n % 2 else (s[mid - 1] + s[mid]) / 2.0


# ---- the negative arm ----------------------------------------------------------------------------
#
# This checker only runs when the rig does, so its OWN failure paths would otherwise never be
# exercised. The selftest plants each case into a throwaway log and requires the matching verdict --
# the same discipline check_net_indicator's selftest uses, and for the same reason: a checker that
# cannot fail is a checker that reports PASS on an empty log.

HEADER = (
    "# wall_ms clock_ms total_ms local_h_ms committed_ms peer0_ms peer1_ms step_ms stall pcount "
    "tx_pkts rx_pkts since_rx_ms sess game flags grace_ms syncwait countdn p54bc sync_ms sim_burst "
    "icon_calls icon_shown srtt0_ms srtt1_ms rttvar0_ms rttvar1_ms ipdv0_ms ipdv1_ms loss0_pm "
    "loss1_pm late_p50_ms late_tail95_ms late_tail99_ms late_n late_peer"
)
# A data row's fields are built FROM the header's own name list -- never hand-counted -- because
# read_lockstep silently DROPS a row whose field count does not match the header ("a torn final
# write, or a row from an older DLL"); a hand-typed literal here could go stale exactly that way and
# the selftest would still "pass" on zero rows, which is the one failure this file exists to prevent
# in the checker it is testing.
_COLS = HEADER.lstrip("# ").split()


def _row(wall_ms, srtt0, srtt1="n/a"):
    vals = {c: "0" for c in _COLS}
    vals.update(
        wall_ms=str(wall_ms),
        sess="1",
        game="uitest",
        late_peer="none",
        srtt0_ms=str(srtt0),
        srtt1_ms=str(srtt1),
    )
    return " ".join(vals[c] for c in _COLS)


def _log(rows):
    return HEADER + "\n" + "\n".join(rows) + "\n"


NL = "\n"
SHIMMED = _log(
    [_row(0, "n/a"), _row(500, 79), _row(1000, 81), _row(1500, 80), _row(2000, 82)]
)  # a real ~40ms-one-way shim: srtt settles near 80
BYPASSED = _log(
    [_row(0, "n/a"), _row(500, 1), _row(1000, 2), _row(1500, 1), _row(2000, 2)]
)  # the shim never ran: LAN/loopback round trip, a few ms
NO_MEASUREMENT = _log(
    [_row(0, "n/a"), _row(500, "n/a"), _row(1000, "n/a")]
)  # tcp, or never sampled
HEADER_ONLY = HEADER + "\n"  # the boot/menu session's empty log


def selftest():
    import subprocess
    import tempfile

    cases = [
        ("shimmed run: real delay measured", SHIMMED, [], 0),
        ("bypassed shim: srtt near zero", BYPASSED, [], 1),
        ("no measurement at all (n/a every row)", NO_MEASUREMENT, [], 1),
        ("header only, no rows (menu session)", HEADER_ONLY, [], 1),
        ("no mh_lockstep.log at all", None, [], 2),
        ("custom --min-srtt met", SHIMMED, ["--min-srtt", "70"], 0),
        ("custom --min-srtt missed", SHIMMED, ["--min-srtt", "90"], 1),
    ]
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, text, argv, want in cases:
            d = os.path.join(
                tmp, name.replace(" ", "_").replace(":", "").replace("(", "").replace(")", "")
            )
            os.makedirs(d, exist_ok=True)
            if text is not None:
                with open(os.path.join(d, "mh_lockstep.log"), "w", encoding="utf-8") as fh:
                    fh.write(text)
            r = subprocess.run(
                [sys.executable, os.path.abspath(__file__), d] + argv,
                capture_output=True,
                text=True,
            )
            got = r.returncode
            print("  %-42s exit %d (want %d)" % (name, got, want))
            if got != want:
                fails.append("%s: exit %d, wanted %d%s%s" % (name, got, want, NL, r.stdout))

        # ---- the REAL shape test_ui.py's post_check hands this checker: the boot/menu session
        # directory, which has NO mh_lockstep.log at all, with the match session (the one that
        # actually has it) as a SIBLING under the same lane's logs/ dir. This is not a hypothetical --
        # it is exactly the case that shipped broken once already (the widen-to-siblings trigger was
        # gated on mh_lockstep.log's own presence, which is circular: the boot session never has one,
        # so the search never widened, and a real rig run REFUSED with "no mh_lockstep.log at or
        # under ..." despite a perfectly good one sitting right next to it). See find_log's docstring.
        lane = os.path.join(tmp, "sibling_lane", "logs")
        menu = os.path.join(lane, "20260101T000000Z_menu_solo")
        match = os.path.join(lane, "20260101T000005Z_abcdef01_0_solo")
        os.makedirs(menu, exist_ok=True)
        os.makedirs(match, exist_ok=True)
        for f in ("mh_launch.log", "mh_net.log"):  # present in EVERY session, boot included
            open(os.path.join(menu, f), "w", encoding="utf-8").close()
        open(os.path.join(match, "mh_launch.log"), "w", encoding="utf-8").close()
        with open(os.path.join(match, "mh_lockstep.log"), "w", encoding="utf-8") as fh:
            fh.write(SHIMMED)
        r = subprocess.run(
            [sys.executable, os.path.abspath(__file__), menu], capture_output=True, text=True
        )
        got = r.returncode
        print(
            "  %-42s exit %d (want %d)" % ("boot session handed over, match is a sibling", got, 0)
        )
        if got != 0:
            fails.append(
                "boot session handed over, match is a sibling: exit %d, wanted 0%s%s"
                % (got, NL, r.stdout)
            )
    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print("check_shim_rtt --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "target", help="a session run dir (or a lane dir above one), or an mh_lockstep.log"
    )
    ap.add_argument(
        "--min-srtt",
        type=float,
        default=DEFAULT_MIN_SRTT_MS,
        metavar="MS",
        help="the median MEASURED srtt0_ms must be >= this (default %(default)s ms) -- the "
        "shim-really-ran assertion; a bypassed shim measures the LAN's real round trip, a few ms",
    )
    a = ap.parse_args()

    log = find_log(a.target)
    if not log:
        print("REFUSED: no mh_lockstep.log at or under %s" % a.target)
        return 2
    rows, names = pacing.read_lockstep(log)
    print("read %s: %d rows" % (log, len(rows)))
    if not names or "srtt0_ms" not in names:
        print("FAIL: mh_lockstep.log has no srtt0_ms column -- an old build, or mp:T3 was reverted")
        return 1

    samples = measured_srtt(rows)
    fails = []
    if not rows:
        fails.append("no rows at all in %s" % log)
    elif not samples:
        fails.append(
            "%d rows, every srtt0_ms is n/a -- the transport measured nothing (not on udp, or the "
            "shim/estimator never got a sample)" % len(rows)
        )
    else:
        m = median(samples)
        print(
            "  srtt0_ms: %d measured samples, median %.1f ms (min %.1f, max %.1f)"
            % (len(samples), m, min(samples), max(samples))
        )
        if m < a.min_srtt:
            fails.append(
                "median measured srtt0_ms=%.1f ms < --min-srtt %.1f ms -- reads as a BYPASSED shim "
                "(a real link, not one routed through the added-latency proxy)" % (m, a.min_srtt)
            )

    for f in fails:
        print("FAIL: %s" % f)
    print("check_shim_rtt: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
