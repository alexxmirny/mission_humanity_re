#!/usr/bin/env python3
"""
tools/check_u47_info_joiner.py -- mp:U47: the JOINER holds the info panel (game mode 4) open and the
match must not stall on it.

THE BUG IT GUARDS (dead-ends G327). The info panel is GAME_MODE 4 (llm_ui_paged_list_frame), which keeps
lockstep alive on its own, but its llm_ui_menu_state_tick ran mh.dll's `on_menu_tick`, and in rc5
(41db0de4) that ran the manual-MP CLIENT driver with no screen gate: MH_MP_ClientPollMap popped up to
128 datagrams a frame and DISCARDED every non-map one -- the host's lockstep horizons included. The
joiner parked, its horizon stopped advancing, and the HOST stalled `waiting_for=<joiner>` for as long as
the panel stayed open. 52352bd1 gates the driver to the lobby widget list and parks an in-game packet in
a one-slot pushback instead of dropping it.

The rc5 discard is SILENT (no log line), and so is the pushback, so "no discarded in-game packet" is
asserted from what the discard does and from the thing that runs it:
  clause 2  the joiner's sim kept advancing and kept RECEIVING through the window (rx_pkts grows, the
            clock advances, since_rx_ms stays small) -- a joiner whose horizons are being thrown away
            does none of those;
  clause 4  the client driver (`; DIAG manual-mp CLIENT driver tick`) logged NOTHING after the match
            entry -- in rc5 it kept ticking from every mode-3/4 screen.

CLAUSES (peers are the SESSION dirs, test_ui.py `post_check_session: True`; the window is cut on the
`wall_ms` axis, which is one clock only when both peers run on this box -- the local rig topology):
  clause 1  the joiner really was in game mode 4: its lockstep rows (`game` column = GAME_MODE, one row
            per ~100 ms) show >= --min-window-s seconds of wall time at mode 4. The window is the span of
            those rows; without it every other clause is vacuous. (mh_frametime.log is NOT usable for
            this: it logs hitches only -- one mode-4 row in the whole window. The script's own
            `gamemode 4` predicates, before and after the hold, are the second witness.);
  clause 2  (above) the joiner's sim advanced and received throughout the window;
  clause 3  the HOST kept running: its clock never stood still >= 1000 ms inside the window and advanced
            >= 90% of it, its wait icon (`icon_calls`) never armed, and the stall-episode counter
            (`stall`, what feeds `[netind] stall waiting_for=`) rose by <= 2 -- one hitch at the edge is
            tolerated (the open's AVI load can block the joiner's frame on a loaded box; one first run
            showed +1 and passed on rerun). The rc5 drain was ONE episode that never ended, so the
            counter alone would miss it; the freeze and the clock advance are the real tests;
  clause 4  (above) no client driver tick after match entry.

  python tools/check_u47_info_joiner.py <host-session-dir> <client-session-dir> [--min-window-s N]
  python tools/check_u47_info_joiner.py --selftest      planted rows; every negative RED
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mp_analyze  # noqa: E402

DRIVER_TICK = re.compile(r"; DIAG manual-mp CLIENT driver tick")
# The line the joiner logs when the lobby hands over to the match (launch.cpp, mp_lobby_entry_tick).
# TWO ways in, and which one a run takes is a TIMING property: the DLL's own lobby entry driver logs
# TRIGGERING (launch.cpp); when the host's START reaches retail's handler first -- which a loaded box
# makes common -- the joiner enters through the retail path and only on_begin_map_load's D36 residue
# line (written on EVERY begin_map_load, either path) marks the entry. Requiring the TRIGGERING line
# alone turned that scheduling difference into a "no entry" red (tooling:TL-U47-LOADRED).
MATCH_ENTRY = re.compile(
    r"TRIGGERING game entry via begin_map_load|D36: match-entry residue cleared"
)
NETIND_STALL = re.compile(r"\[netind\] stall waiting_for=")


def window_of(client_rows, mode=4):
    """(wall_first, wall_last, clock_first, clock_last, n) over the client's lockstep rows at `mode`."""
    rows = [r for r in client_rows if r.get("game") == mode]
    if not rows:
        return None
    return (
        rows[0]["wall_ms"],
        rows[-1]["wall_ms"],
        rows[0]["clock_ms"],
        rows[-1]["clock_ms"],
        len(rows),
    )


MAX_NEW_EPISODES = 2
MAX_FREEZE_MS = 1000
# LOAD-INVARIANT CLAUSES (tooling:TL-U47-LOADRED). A box whose cores are taken runs BOTH lockstep peers
# below real time (measured: host and joiner at 0.51-0.89 of wall, in the SAME ratio before and during
# the panel), so "the clock advanced >= 90% of the wall window" and "<= 2 new stall episodes" were
# really tests of the box. The bug (rc5) is a host that stops advancing for as long as the panel is
# open; against the same run's own pre-panel rate that is a ratio near 0, load or no load. Every
# rate/count clause therefore compares the panel window with the rows just BEFORE it (the landed
# match, panel closed), falling back to the old absolute bounds only when too little baseline exists.
BASE_MIN_CLOCK_MS = (
    6000  # the script's own `gameclock 6000` landing: earlier rows carry the entry hitches
)
BASE_MIN_SPAN_MS = 5000  # less than this of baseline is noise, not a rate
MIN_RATE_RATIO = 0.6  # window rate must keep >= this share of the pre-panel rate (bug: ~0)
ICON_SLACK = 2


def baseline_of(rows, w0):
    """The panel's pre-window baseline over `rows`: (rate clock_ms/wall_ms, wall span ms, rows) or None."""
    pre = [r for r in rows if r["wall_ms"] < w0 and r["clock_ms"] >= BASE_MIN_CLOCK_MS]
    if len(pre) < 2:
        return None
    span = pre[-1]["wall_ms"] - pre[0]["wall_ms"]
    if span < BASE_MIN_SPAN_MS:
        return None
    return (pre[-1]["clock_ms"] - pre[0]["clock_ms"]) / float(span), span, pre


def longest_freeze(rows):
    """Longest wall span (ms) over which consecutive rows carry the same clock_ms."""
    worst = run = 0
    for a, b in zip(rows, rows[1:]):
        if a["clock_ms"] == b["clock_ms"]:
            run += b["wall_ms"] - a["wall_ms"]
            worst = max(worst, run)
        else:
            run = 0
    return worst


def analyse(host_rows, client_rows, client_launch, min_window_s=10.0):
    fails = []
    win = window_of(client_rows)
    if win is None:
        return [
            "clause 1: the joiner never logged a mode-4 lockstep row -- the panel did not open"
        ], "no window"
    w0, w1, c0, c1, n = win
    span_s = (w1 - w0) / 1000.0
    if span_s < min_window_s:
        fails.append(
            "clause 1: mode-4 lockstep rows span %.1f s of wall time, need >= %.1f s"
            % (span_s, min_window_s)
        )

    # clause 2: the joiner kept running and receiving across the window
    inw = [r for r in client_rows if w0 <= r["wall_ms"] <= w1]
    clock_adv = (inw[-1]["clock_ms"] - inw[0]["clock_ms"]) if inw else 0
    rx_adv = (inw[-1]["rx_pkts"] - inw[0]["rx_pkts"]) if inw else 0
    max_since = max((r.get("since_rx_ms", 0) for r in inw), default=0)
    cbase = baseline_of(client_rows, w0)
    if cbase:
        # a loaded box slows the joiner before the panel as much as during it; a parked joiner (rc5)
        # drops to ~0 against its own pre-panel rate
        c_rate = clock_adv / float(max(1, w1 - w0))
        if c_rate < MIN_RATE_RATIO * cbase[0]:
            fails.append(
                "clause 2: the joiner's clock advanced %d ms over a %d ms window = %.2f of wall, "
                "against %.2f of wall just before the panel (parked?)"
                % (clock_adv, w1 - w0, c_rate, cbase[0])
            )
    elif clock_adv < (w1 - w0) * 0.5:
        fails.append(
            "clause 2: the joiner's clock advanced %d ms over a %d ms window (parked?)"
            % (clock_adv, w1 - w0)
        )
    if rx_adv < 50:
        fails.append("clause 2: the joiner received only %d packets across the window" % rx_adv)
    if max_since > 1000:
        fails.append("clause 2: the joiner went %d ms without receiving (since_rx_ms)" % max_since)

    # clause 3: the host's stall counters did not move over the window
    hin = [r for r in host_rows if w0 <= r["wall_ms"] <= w1]
    before = [r for r in host_rows if r["wall_ms"] < w0]
    if not hin:
        fails.append("clause 3: the host logged no lockstep row inside the window")
        h_stall = h_icon = -1
        h_adv = 0
    else:
        base_stall = before[-1]["stall"] if before else hin[0]["stall"]
        base_icon = before[-1].get("icon_calls", 0) if before else hin[0].get("icon_calls", 0)
        hbase = baseline_of(host_rows, w0)
        if hbase:
            # `stall` is the game's own STALL_COUNT gauge (it climbs while a wait lasts, falls back to 0
            # when it ends), so a loaded box peaks at ~8-10 BEFORE the panel too: compare peaks.
            base_stall = max(base_stall, max(r["stall"] for r in hbase[2]))
        h_stall = max(r["stall"] for r in hin) - base_stall
        h_icon = max(r.get("icon_calls", 0) for r in hin) - base_icon
        h_adv = hin[-1]["clock_ms"] - hin[0]["clock_ms"]
        # A one-row hitch is allowed at the window's edge (the open itself -- the AVI and INFO.Txt
        # loads -- briefly blocks the joiner's frame on a loaded box). What the rc5 drain produced was
        # an episode that never ENDED, which the `stall` counter shows as ONE increment, so the real
        # tests are the clock freeze, the wait icon and the clock advance; the counter is a backstop.
        freeze = longest_freeze(hin)
        icon_allow = 0
        max_freeze = MAX_FREEZE_MS
        if hbase:
            # the wait icon is drawn after the sim has been parked > 1 s; how often a loaded box does
            # that before the panel sets how often it may inside it (rc5: every frame, for the whole hold)
            icon_rate = (
                hbase[2][-1].get("icon_calls", 0) - hbase[2][0].get("icon_calls", 0)
            ) / float(hbase[1])
            icon_allow = int(2 * icon_rate * (w1 - w0)) + ICON_SLACK
            max_freeze = max(MAX_FREEZE_MS, 3 * longest_freeze(hbase[2]))
        if h_stall > MAX_NEW_EPISODES:
            fails.append(
                "clause 3: the host's stall gauge peaked %d above its pre-panel peak while the joiner held the panel (max %d)"
                % (h_stall, MAX_NEW_EPISODES)
            )
        if freeze >= max_freeze:
            fails.append(
                "clause 3: the host's clock stood still for %d ms inside the window (max %d)"
                % (freeze, max_freeze)
            )
        if h_icon > icon_allow:
            fails.append(
                "clause 3: the host's wait icon armed %d time(s) inside the window (allowed %d at its pre-panel rate)"
                % (h_icon, icon_allow)
            )
        if hbase:
            h_rate = h_adv / float(max(1, w1 - w0))
            if h_rate < MIN_RATE_RATIO * hbase[0]:
                fails.append(
                    "clause 3: the host's clock advanced %d ms over a %d ms window = %.2f of wall, against %.2f of wall just before the panel"
                    % (h_adv, w1 - w0, h_rate, hbase[0])
                )
        elif h_adv < (w1 - w0) * 0.9:
            fails.append(
                "clause 3: the host's clock advanced %d ms over a %d ms window" % (h_adv, w1 - w0)
            )

    # clause 4: the client driver never ran after the match began
    entry = [i for i, ln in enumerate(client_launch) if MATCH_ENTRY.search(ln)]
    ticks = 0
    if not entry:
        fails.append(
            "clause 4: no `TRIGGERING game entry via begin_map_load` line in the joiner's launch log"
        )
    else:
        ticks = sum(1 for ln in client_launch[entry[0] + 1 :] if DRIVER_TICK.search(ln))
        if ticks:
            fails.append(
                "clause 4: the client driver ticked %d time(s) AFTER match entry (it drains the game queue)"
                % ticks
            )
    summary = (
        "window %.1f s at mode 4, joiner clock +%d ms rx +%d max since_rx %d ms; host new stalls %d icons %d clock +%d ms; in-match driver ticks %d"
        % (span_s, clock_adv, rx_adv, max_since, h_stall, h_icon, h_adv, ticks)
    )
    return fails, summary


def load(host_dir, client_dir):
    a = mp_analyze.load_peer(host_dir)
    b = mp_analyze.load_peer(client_dir)
    launch = []
    lp = b["logs"].get("launch")
    if lp and os.path.isfile(lp):
        launch = open(lp, encoding="utf-8", errors="replace").read().splitlines()
    return a.get("lockstep_rows", []), b.get("lockstep_rows", []), launch


def check(host_dir, client_dir, min_window_s=10.0):
    host_rows, client_rows, launch = load(host_dir, client_dir)
    if not host_rows or not client_rows:
        return [
            "no lockstep rows (host=%d client=%d)" % (len(host_rows), len(client_rows))
        ], "no rows"
    return analyse(host_rows, client_rows, launch, min_window_s)


# ---------------------------------------------------------------- selftest


def _plant(
    window_s=12,
    host_stall_in_window=0,
    host_freeze_ms=0,
    client_parked=False,
    ticks=0,
    panel=True,
    load=1.0,
    host_slow_in_window=1.0,
    host_icons_per_row=0,
    saw=0,
    entry="trigger",
):
    """Planted lockstep rows, one per 100 ms of wall. `load` slows BOTH peers' sim clock everywhere (a
    box with its cores taken); `saw` gives the host's stall gauge a sawtooth of that peak everywhere
    (loaded boxes peak before the panel too); `host_slow_in_window` slows only the host, only inside
    the panel window (the bug shape without a freeze)."""
    host, client = [], []
    stall = 3
    icon = 3
    clock_h = clock_c = 0.0
    W0 = 26000
    for i in range(0, 70000, 100):
        t = 1000 + i
        in_win = panel and W0 <= i < W0 + window_s * 1000
        if in_win and host_stall_in_window and i == W0 + 2000:
            stall += host_stall_in_window
        step = 100 * load
        hstep = step * (host_slow_in_window if in_win else 1.0)
        if host_freeze_ms and in_win and W0 + 2000 <= i < W0 + 2000 + host_freeze_ms:
            hstep = 0.0
        clock_h += hstep
        clock_c += step
        if in_win and host_icons_per_row:
            icon += host_icons_per_row
        gauge = stall + ((i // 100) % (saw + 1) if saw else 0)
        host.append(
            dict(
                wall_ms=t,
                clock_ms=int(clock_h),
                stall=gauge,
                icon_calls=icon,
                rx_pkts=i,
                since_rx_ms=10,
            )
        )
        parked = client_parked and in_win
        client.append(
            dict(
                wall_ms=t,
                clock_ms=client[-1]["clock_ms"] if parked else int(clock_c),
                rx_pkts=0 if parked else i,
                since_rx_ms=3000 if parked else 10,
                game=4 if in_win else 2,
            )
        )
    entry_ln = {
        "trigger": "; --mp-join: TRIGGERING game entry via begin_map_load (slots_occupied=2)",
        "retail": "; D36: match-entry residue cleared: player_data nonzero=0 of 1329136 B",
        "none": "; U29: left the lobby screen",
    }[entry]
    launch = [entry_ln] + [
        "; DIAG manual-mp CLIENT driver tick #%d occ=0 map=0" % i for i in range(ticks)
    ]
    return host, client, launch


def selftest():
    cases = [
        ("fixed: 12 s window, no stall, no driver tick", {}, True),
        ("one hitch episode at the edge is tolerated", dict(host_stall_in_window=1), True),
        ("NEG: three new stall episodes", dict(host_stall_in_window=3), False),
        (
            "NEG: ONE episode that never ends (host clock frozen 5 s, rc5 shape)",
            dict(host_stall_in_window=1, host_freeze_ms=5000),
            False,
        ),
        ("NEG: joiner parked (no rx, clock frozen)", dict(client_parked=True), False),
        ("NEG: driver ticked in-match (rc5)", dict(ticks=2), False),
        ("NEG: window too short (6 s)", dict(window_s=6), False),
        ("NEG: panel never opened", dict(panel=False), False),
        # the load-invariant clauses (tooling:TL-U47-LOADRED): a slow box is not a stall
        (
            "LOADED box: both peers at 0.5x real time throughout, stall gauge sawtooth to 8",
            dict(load=0.5, saw=8),
            True,
        ),
        (
            "LOADED box: both peers at 0.7x, entry through retail's START handler (no TRIGGERING)",
            dict(load=0.7, entry="retail"),
            True,
        ),
        (
            "NEG: LOADED box AND the host drops to 20% of its own rate inside the window",
            dict(load=0.5, host_slow_in_window=0.2),
            False,
        ),
        (
            "NEG: wait icon armed every frame inside the window (rc5), clock still moving",
            dict(host_icons_per_row=1),
            False,
        ),
        ("NEG: no entry marker at all", dict(entry="none"), False),
    ]
    bad = 0
    for label, kw, want in cases:
        fails, _ = analyse(*_plant(**kw))
        got = not fails
        ok = got == want
        print(
            "  %s  %s%s"
            % ("ok " if ok else "BAD", label, "" if got else "  -> " + "; ".join(fails))
        )
        bad += 0 if ok else 1
    print("check_u47_info_joiner selftest: %d/%d" % (len(cases) - bad, len(cases)))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--min-window-s", type=float, default=10.0)
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if len(args.dirs) != 2:
        print(
            "usage: check_u47_info_joiner.py <host-session-dir> <client-session-dir> [--min-window-s N]"
        )
        return 2
    fails, summary = check(args.dirs[0], args.dirs[1], args.min_window_s)
    if fails:
        print("check_u47_info_joiner: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print("check_u47_info_joiner: PASS (%s)" % summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
