#!/usr/bin/env python3
"""check_resync_storm.py -- mp:P9: the resync-trigger gate in CONFIGURATION (1), judged off the logs.

WHAT THIS IS FOR. The 2026-09-20 field crawl (9-21 % realtime) is the leader's mode-8 resync barrier
firing every ~2 s (mp:P8's re-derivation of the 2026-09-20 field logs: a game_mode-8 frame before
every ~1.4 s gap, 65-88 % of wall frozen): RESYNC_TRIGGER_COUNT (the leader's
cumulative stall-nag counter) passes ACTIVE_PLAYER_COUNT*100 and `force_resync` freezes every peer
for ~2 s. The fix, `[net] resync_trigger_gate`, only counts a nag while SYNC_RETRY_COUNTDOWN < 0x38
(genuine silence). Until mp:P9 it lived ONLY in libmh's promoted bodies -- and the players run the
`-net.zip` drop-in, configuration (1): NO libmh.dll, nothing promoted, `; [net] UNCARRIED FIX` in
every field boot log. P9 (1/2) re-instated the two byte-patch carriers (INC sites @0x49d8cb SENT and
@0x49c508 RECEIVED -> a gated thunk) and added the `[resync]` WATCH lines so one peer's mh_net.log
names the counter, the threshold and the countdown. This checker reads both.

THREE ARMS, one checker (the check_cancel_task.py shape):

  --expect carried   the `match_launch_net` row: a configuration-(1) pair (libmh.dll OMITTED from
                     both lanes) on the shipped udp transport. The gate must be CARRIED by the
                     byte patches and the match must stay clean. Also `resync_gate_proof` (mp:P9
                     2/2): the reproduction's exact shape with the gate ON and `resync_count_init=0`
                     -- the gate alone must hold the barrier count at 0 for 5 min.
  --expect storm     the `resync_storm_repro` row (registered `expect_red: "mp:P9"` and KEPT red):
                     the same pair with `[net] resync_trigger_gate=0` + `resync_count_init=0` (the
                     field's pre-fix state) and `defang_overlay=0` (the SHIPPED value -- see WATCH
                     v2 below), host 100 / joiner 60 ms lookahead through a 100 ms one-way shim (the
                     field link), for 5 min in-game. THE ARM IS THE ROW'S ASSERTION, "the stock
                     start does not storm", WHICH IS KNOWN FALSE: it goes RED naming the count when
                     >= --min-barriers (5) barrier BEGIN lines are seen, so the row's XFAIL carries
                     the storm's numbers, and an XPASS (fewer than 5 over a real match) is the
                     finding that the stock configuration stopped storming -- rule 7's "turned
                     XPASS, key dropped" shape, for a row whose fix can only ever be retail's.
  --expect countinit the `resync_countinit_proof` row (mp:P9 2/2): gate OFF, `resync_count_init=1`
                     (the count fix alone) -- the `count_init` line must read the threshold up
                     from 0 to players*100 on both peers, and every barrier that still fires must
                     BEGIN at count == threshold (retail's intended 200-nag trigger), never at the
                     stock 0. NOT "0 barriers": measured 2026-09-22, at the field's 100/60 shape
                     through 200 ms the ungated counter climbs ~15 nags/s, so the fix alone turns
                     one barrier per ~2 s into one per ~15 s (160 -> 19 in 5.5 min); the zero is
                     the gate's, and the ship carries both.

WATCH v2 (mp:P9 2/2, 2026-09-22) -- WHY THE BARRIER, NOT THE COUNTER, IS THE STORM'S WITNESS. The
first reproduction (15 min, 0 FIRED lines) was blind twice over: (1) force_resync zeroes
RESYNC_TRIGGER_COUNT at ENTRY and the stock threshold ACTIVE_PLAYER_COUNT*100 is 0 (nothing calls
count_active_players at session begin), so a fire is 0 -> 1 -> 0 inside one call and a per-frame
sampler never sees the drop; (2) the rig ran ui_test's `--defang` default 1, which NOPs the mode-8
store the barrier's wait-screen frame needs, so RESYNC_IN_PROGRESS latched after the first fire and
every later one was a silent no-op. The DLL now logs RESYNC_IN_PROGRESS's edges directly
(`; [resync] barrier #k BEGIN ...` / `; [resync] barrier #k END after M ms ...`), and the storm
arm counts BEGIN lines; the counter-drop `FIRED` line is a secondary witness and is only reported.

THE CLAUSES, each named on failure:

  0. CONFIGURATION (1), on EVERY peer (both arms): the process ("menu") dir's mh_net.log reads
     `[modules] libmh: NOT BOUND` (check_module_bind's `absent`), `[modules] mh_net: BOUND` (the
     transport is there -- a lane that lost it too would be module_absent, not this row) and the
     `; [libmh] crossings=0 ... configuration (1)` witness. A configuration-(2) lane -- libmh bound,
     the gate displaced into the promoted body -- is a DIFFERENT experiment and FAILS here: it is
     the one every rig run since C8-e already exercised, and the one the players do not run.
  1. NO `; [net] UNCARRIED FIX` line on either peer (both arms). In the carried arm that line means
     a site was neither patched nor displaced; in the storm arm the knob is off and the installer
     never runs, so the line cannot appear either -- in both arms it is a build that lost the
     carrier.
  2. THE GATE INSTALL LINE. carried: `; resync-trigger gate: N/2 INC sites patched ... M displaced
     by promotion, K MISMATCHED` on every peer, reading 2/2 patched (named `patched`) OR 2 displaced
     (named `displaced` -- reachable only in configuration (2), which clause 0 already refuses, but
     the line's own vocabulary is honoured) and K == 0. storm: the line must be ABSENT on every peer
     -- `resync_trigger_gate=0` really was the run's configuration, so the reproduction is of the
     ungated counter and not of a gated one that happened to fire.
  3. carried: ZERO STORM-CLASS `; [resync] barrier #k BEGIN` lines on EITHER peer (the barrier
     flag is set on every peer) and ZERO `; [resync] force_resync FIRED` lines on the leader (the
     FIRST dir given = the host; the client's count never moves, but its lines are counted too)
     over a match of at least --min-steps sim steps, AND the pair stayed identical. STORM-CLASS =
     a BEGIN whose `countdown` is >= 0x38 (56): a fire while nags were still being answered, i.e.
     the threshold-0 storm. A BEGIN at countdown < 0x38 is a GATE-CONFORMANT fire -- the gate's
     whole contract is "count only under SYNC_RETRY_COUNTDOWN < 0x38" -- and it is reported, not
     red: it happens on a dead link (the blackhole ending) and after a >= 2 s machine-wide freeze
     (measured 2026-09-22 12:06: both lanes' presents stopped for 1.88 s at the same qpc, and on
     resume the accumulated SYNC_WAIT_ELAPSED let the host nag five times in one 100 ms frame,
     55 < 56, one counted nag, threshold 0, fire). The ship carries count_init too, which makes
     that single counted nag 1 < 200 and removes even this fire. Configuration (1) has no determinism
     harness (mh_harness REFUSES TO ARM without libmh: ruling Q4), so mp_analyze has no R rows to
     compare; the in-band D21 desync watch IS live there (its sim_step trampoline installs in every
     configuration) and, with `[desync] verbose=1` (tools/uiscripts/ini/desync_verbose.ini), logs
     one `; [desync] step=N peer=P MATCH state=...` line per agreeing comparison. So: max MATCH
     step >= --min-steps on the host, >= 1 MATCH line on the client, ZERO `*** DESYNC` lines on
     either. A log with no MATCH line at all is REFUSED (the row lost its verbose fragment, or the
     walk never reached a live match) -- never passed.
     storm: NO `count_init` line on either peer (the knob really was off: this is the stock
     threshold-0 start), an `armed` line on the leader (the match was reached), and then RED iff
     the leader carries >= --min-barriers (5) barrier BEGIN lines -- the storm, named with its
     count, cadence and the ~1.7 s present hitches of mh_frametime.log. The `[resync]` watch
     lines are summarised either way (barriers + END durations, first/last count, threshold,
     lowest countdown seen, the per-10 s deltas) -- the numbers the field logs could not give.
     countinit: a `; [resync] count_init: ACTIVE_PLAYER_COUNT a -> b (threshold t)` line on BOTH
     peers with b >= 2 and t == b*100; every barrier BEGIN on the leader reads `threshold t` and
     `count was t` (the fire sat at the intended threshold -- a BEGIN at a lower count is the
     threshold-0 fire this fix removes, or a stale threshold); identical over >= --min-steps, no
     `*** DESYNC`. The gate line must be ABSENT (gate off), as in storm.

USAGE
  python tools/check_resync_storm.py --expect carried|storm|countinit <host-run-dir> <client-run-dir>
  python tools/check_resync_storm.py --selftest        planted logs; every negative RED

The run dirs are the SESSION dirs (test_ui.py `post_check_session: True`): `resync_lines()` reads
that dir's own mh_net.log (match-time: the [resync] and [desync] lines) PLUS the process dir its
session.json names (boot-time: the [modules], gate-install and UNCARRIED lines), the
check_cancel_task.net_log_lines shape.
"""

import argparse
import json
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_module_bind  # noqa: E402

# The needles, verbatim from net_lockstep.cpp (tools/data/log_formats.json net.resync_watch /
# net.resync_fired / net.resync_gate_install name this file as their parser -- lint_log_formats ARM A
# checks these literals are here).
WATCH_NEEDLE = "[resync] trigger_count="
FIRED_NEEDLE = "[resync] force_resync FIRED"
ARMED_NEEDLE = "[resync] armed:"
BARRIER_NEEDLE = "[resync] barrier #"
COUNT_INIT_NEEDLE = "[resync] count_init:"
GATE_NEEDLE = "resync-trigger gate:"
UNCARRIED_NEEDLE = "[net] UNCARRIED FIX"
# The gate's own predicate: a nag counts only while SYNC_RETRY_COUNTDOWN < 0x38. A barrier whose
# BEGIN samples a countdown at or above this fired while nags were still being answered -- the
# storm's shape -- and one below it is the gate's designed response to genuine silence.
GATE_COUNTDOWN = 0x38

WATCH_RE = re.compile(
    r"\[resync\] trigger_count=(\d+) threshold=(\d+) countdown=(-?\d+) \(\+(\d+) in 10s\)"
)
FIRED_RE = re.compile(
    r"\[resync\] force_resync FIRED #(\d+): count (\d+) -> (\d+) \(threshold (\d+), countdown (-?\d+)\)"
)
GATE_RE = re.compile(
    r"resync-trigger gate: (\d)/2 INC sites patched .*?, (\d) displaced by promotion, (\d) MISMATCHED"
)
ARMED_RE = re.compile(
    r"\[resync\] armed: trigger_count=(\d+) threshold=(\d+) \(ACTIVE_PLAYER_COUNT=(\d+)\) countdown=(-?\d+)"
)
# mp:P9C -- how far BELOW the threshold a barrier's sampled count may read and still be a fire.
# The sampler runs once per on_time_tick; the counter is advanced by arriving keepalives (the leader
# increments twice per episode, sending and receiving), so a long frame can straddle increments and
# the sample lands short. Measured deficit 2026-09-23: 1, on 1 of 15 fires. 10 is a deliberate 20x
# margin on that and still two orders of magnitude clear of the threshold-0 fire this file hunts.
SAMPLE_SLACK = 10

BARRIER_BEGIN_RE = re.compile(
    r"\[resync\] barrier #(\d+) BEGIN \(count was (\d+), threshold (\d+), countdown (-?\d+)\) flags=0x([0-9a-f]+)"
)
BARRIER_END_RE = re.compile(r"\[resync\] barrier #(\d+) END after (\d+) ms flags=0x([0-9a-f]+)")
COUNT_INIT_RE = re.compile(
    r"\[resync\] count_init: ACTIVE_PLAYER_COUNT (-?\d+) -> (-?\d+) \(threshold (\d+)\)"
)
DESYNC_MATCH_RE = re.compile(r"\[desync\] step=(\d+) peer=(-?\d+) MATCH state=")
DESYNC_BAD_NEEDLE = "*** DESYNC"
CROSSINGS_RE = check_module_bind.CROSSINGS_RE


def net_log_lines(run_dir):
    """mh_net.log lines of the run dir AND of the process dir its session.json names -- the boot
    lines (modules / gate install / UNCARRIED) are written at DllMain into the process dir, the
    match-time lines ([resync] / [desync]) into the session dir."""
    cands = [os.path.join(run_dir, "mh_net.log")]
    sj = os.path.join(run_dir, "session.json")
    if os.path.isfile(sj):
        try:
            pd = json.load(open(sj, encoding="utf-8")).get("process_dir")
        except Exception:
            pd = None
        if pd:
            parent = os.path.dirname(os.path.abspath(run_dir).rstrip("\\/"))
            cands.append(os.path.join(parent, pd, "mh_net.log"))
    out = []
    for fp in cands:
        if os.path.isfile(fp):
            try:
                out.extend(open(fp, encoding="utf-8", errors="replace").read().splitlines())
            except OSError:
                pass
    return out


def resync_lines(run_dir):
    """The mp:P9 evidence of one peer's run, parsed:
    {watch: [(count, threshold, countdown, delta10)], fired: [(no, before, after, threshold,
    countdown)], barriers: [(no, count_before, threshold, countdown, flags)] (BEGIN edges),
    barrier_ends: [(no, duration_ms, flags)], count_init: [(before, after, threshold)],
    gate: [(armed, displaced, mismatched)], uncarried: [line], desync_match_steps: [int],
    desync_bad: [line], mode8_rows / mode8_runs: the mh_lockstep.log `game` column's 8s (rows and
    entries into 8; corroboration only), lines: [every line]}."""
    lines = net_log_lines(run_dir)
    d = {
        "armed": [],
        "watch": [],
        "fired": [],
        "resets": [],
        "barriers": [],
        "barrier_ends": [],
        "count_init": [],
        "mode8_rows": 0,
        "mode8_runs": 0,
        "gate": [],
        "uncarried": [],
        "desync_match_steps": [],
        "desync_bad": [],
        "lines": lines,
    }
    for ln in lines:
        m = ARMED_RE.search(ln)
        if m:
            d["armed"].append(tuple(int(x) for x in m.groups()))
            continue
        m = BARRIER_BEGIN_RE.search(ln)
        if m:
            g = m.groups()
            d["barriers"].append(tuple(int(x) for x in g[:4]) + (int(g[4], 16),))
            continue
        m = BARRIER_END_RE.search(ln)
        if m:
            g = m.groups()
            d["barrier_ends"].append((int(g[0]), int(g[1]), int(g[2], 16)))
            continue
        m = COUNT_INIT_RE.search(ln)
        if m:
            d["count_init"].append(tuple(int(x) for x in m.groups()))
            continue
        m = WATCH_RE.search(ln)
        if m:
            d["watch"].append(tuple(int(x) for x in m.groups()))
            continue
        m = FIRED_RE.search(ln)
        if m:
            # THE WATCH INFERS A FIRE FROM THE COUNTER DROPPING, and the other writer that can
            # lower it is `resync_trigger_reset` zeroing RESYNC_TRIGGER_COUNT in time_tick's
            # recovery branch. CORRECTED 2026-09-23 (mp:P9C): the comment here used to say that
            # knob was "a byte patch carried in every configuration, default ON". It is NOT --
            # net_lockstep.cpp reads it with a default of 0 (`GetPrivateProfileIntA("net",
            # "resync_trigger_reset", 0, g_ini)`, "default OFF pending validation"), its carrier is
            # installed only when the knob is set, and its libmh body ships in libmh.dll, which
            # configuration (1) omits. So in every arm this checker runs, a drop is a FIRE unless
            # something else is proven.
            t = tuple(int(x) for x in m.groups())
            # The sampler reads the count ONCE PER FRAME while the counter is advanced by ARRIVING
            # KEEPALIVES, so the sample before a fire is whatever the last frame happened to catch:
            # usually threshold (200), but threshold-1 when one frame straddled two increments.
            # The old test was `t[1] >= t[3]` with a comment resting on "19 of 19 fires at threshold
            # 200 read exactly 200" -- falsified 2026-09-23 at 1 in 15 (barrier #4, `count was 199`),
            # which is a property of the SAMPLER, not of the trigger: tx_emit.cpp increments and
            # compares on the same memory in the same call, so the trigger cannot fire below the
            # threshold. SAMPLE_SLACK admits that jitter while still separating a fire from the
            # threshold-0 start this checker exists to catch (0 vs 200).
            (d["fired"] if t[1] >= t[3] - SAMPLE_SLACK else d["resets"]).append(t)
            continue
        m = GATE_RE.search(ln)
        if m:
            d["gate"].append(tuple(int(x) for x in m.groups()))
            continue
        if UNCARRIED_NEEDLE in ln:
            d["uncarried"].append(ln)
            continue
        m = DESYNC_MATCH_RE.search(ln)
        if m:
            d["desync_match_steps"].append(int(m.group(1)))
            continue
        if DESYNC_BAD_NEEDLE in ln:
            d["desync_bad"].append(ln)
    d["mode8_rows"], d["mode8_runs"] = mode8_of(os.path.join(run_dir, "mh_lockstep.log"))
    d["hitches"] = frametime_hitches(os.path.join(run_dir, "mh_frametime.log"))
    return d


def frametime_hitches(path, min_us=1000000):
    """Presents whose interval was >= min_us (1 s) in a session's mh_frametime.log -- the barrier
    as the PLAYER feels it. The mode-8 barrier is ONE blocked frame: its present lands after
    llm_net_mp_leave_reset_game_mode has restored the mode, so the outlier row (`qpc_us game_mode
    interval_us`, the frametime.row format) reads game_mode 2 with a ~1.7-2.0 s interval, and
    no row in the file ever says 8. Count the interval, not the mode. Corroboration only."""
    n = 0
    if not os.path.isfile(path):
        return n
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                if ln.startswith("#"):  # the `# qpc_us game_mode` header and the `# 1s` summaries
                    continue
                parts = ln.split()
                if len(parts) >= 3 and parts[2].isdigit() and int(parts[2]) >= min_us:
                    n += 1
    except OSError:
        pass
    return n


def mode8_of(path):
    """(rows, runs) of GAME_MODE 8 in a session's mh_lockstep.log -- the barrier's own frame, read
    off the `game` column by header name (the file is change-gated, so a run of 8s is one barrier
    whatever its length). Corroboration for the barrier lines, never a clause: a lane without the
    lockstep log reads (0, 0) and says nothing."""
    rows = runs = 0
    if not os.path.isfile(path):
        return rows, runs
    col = None
    prev8 = False
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                if ln.startswith("#"):
                    names = ln[1:].split()
                    if "game" in names and col is None:
                        col = names.index("game")
                    continue
                if col is None:
                    continue
                parts = ln.split()
                if len(parts) <= col:
                    continue
                is8 = parts[col] == "8"
                if is8:
                    rows += 1
                    if not prev8:
                        runs += 1
                prev8 = is8
    except OSError:
        pass
    return rows, runs


def watch_summary(ev):
    """One line describing the [resync] watch of a peer -- the number the field could not give."""
    w = ev["watch"]
    parts = []
    if ev["armed"]:
        a = ev["armed"][-1]
        parts.append(
            "armed: count %d, threshold %d (ACTIVE_PLAYER_COUNT=%d), countdown %d"
            % (a[0], a[1], a[2], a[3])
        )
    if ev["count_init"]:
        ci = ev["count_init"][-1]
        parts.append("count_init %d -> %d (threshold %d)" % ci)
    if ev["barriers"]:
        ends = [e[1] for e in ev["barrier_ends"]]
        silent = sum(1 for b in ev["barriers"] if b[3] < GATE_COUNTDOWN)
        parts.append(
            "%d barrier BEGIN(s)%s, %d END(s)%s, flags at BEGIN %s; frametime hitches >= 1 s: %d; "
            "mode-8 rows in mh_lockstep.log: %d (%d run(s))"
            % (
                len(ev["barriers"]),
                (" (%d gate-conformant: countdown < 0x38 at BEGIN)" % silent) if silent else "",
                len(ends),
                (" of %d..%d ms" % (min(ends), max(ends))) if ends else "",
                "/".join(sorted({"0x%02x" % b[4] for b in ev["barriers"]})),
                ev["hitches"],
                ev["mode8_rows"],
                ev["mode8_runs"],
            )
        )
    if not w and not ev["fired"] and not ev["resets"]:
        if not ev["barriers"]:
            parts.append(
                "no barrier/watch/FIRED lines (nothing fired and the counter never moved, or the "
                "run never reached a match)"
            )
        return "; ".join(parts)
    if ev["resets"]:
        parts.append(
            "%d recovery reset(s) (resync_trigger_reset zeroing a sub-threshold count: %s)"
            % (
                len(ev["resets"]),
                ",".join(str(x[1]) for x in ev["resets"][:8])
                + ("..." if len(ev["resets"]) > 8 else ""),
            )
        )
    if w:
        parts.append(
            "watch %d line(s): count %d -> %d, threshold %s, countdown min %d, +%d..+%d per 10 s"
            % (
                len(w),
                w[0][0],
                w[-1][0],
                "/".join(sorted({str(x[1]) for x in w})),
                min(x[2] for x in w),
                min(x[3] for x in w),
                max(x[3] for x in w),
            )
        )
    if ev["fired"]:
        f = ev["fired"]
        parts.append(
            "FIRED %d time(s): counts %s (threshold %s)"
            % (
                len(f),
                ",".join(str(x[1]) for x in f[:8]) + ("..." if len(f) > 8 else ""),
                "/".join(sorted({str(x[3]) for x in f})),
            )
        )
    return "; ".join(parts)


def config1_fails(label, lines):
    """Clause 0 for one peer: the boot really was configuration (1) with the transport bound."""
    fails = []
    try:
        key, _at, _text, _end = check_module_bind.classify(lines, "libmh")
        if key != "absent":
            fails.append(
                "clause 0: %s's libmh bind outcome is `%s`, not `absent` -- this is not "
                "configuration (1) (the gate would be DISPLACED into the promoted body, which is "
                "the run every rig run since C8-e already was)" % (label, key)
            )
    except check_module_bind.Refusal as e:
        fails.append("clause 0: %s: %s" % (label, e))
    try:
        key, _at, text, _end = check_module_bind.classify(lines, "mh_net")
        if key != "bound" or not check_module_bind.BOUND_OK_RE.search(text):
            fails.append(
                "clause 0: %s's transport bind outcome is `%s` -- the lane lost mh_net too, which "
                "is module_absent's configuration, not this row's" % (label, key)
            )
    except check_module_bind.Refusal as e:
        fails.append("clause 0: %s: %s" % (label, e))
    cross = [m for m in (CROSSINGS_RE.search(ln) for ln in lines) if m]
    if not cross:
        fails.append(
            "clause 0: %s has no `; [libmh] crossings=` line -- the run never presented a frame"
            % label
        )
    elif int(cross[-1].group(3)) != 1 or int(cross[-1].group(1)) != 0:
        fails.append(
            "clause 0: %s's crossing report reads crossings=%s configuration (%s), not 0 / (1)"
            % (label, cross[-1].group(1), cross[-1].group(3))
        )
    return fails


def check(host_dir, client_dir, expect, min_steps=1000, min_barriers=5):
    fails = []
    peers = [("host", resync_lines(host_dir)), ("client", resync_lines(client_dir))]
    for label, ev in peers:
        if not ev["lines"]:
            fails.append(
                "%s: no mh_net.log under %s" % (label, host_dir if label == "host" else client_dir)
            )
    if fails:
        return fails, "no logs"
    for label, ev in peers:
        fails += config1_fails(label, ev["lines"])
        # clause 1
        if ev["uncarried"]:
            fails.append(
                "clause 1: %s carries %d `; %s` line(s) -- a site is neither patched nor "
                "promoted: %s"
                % (label, len(ev["uncarried"]), UNCARRIED_NEEDLE, ev["uncarried"][0].strip())
            )
        # clause 2
        gate = ev["gate"]
        if expect == "carried":
            if not gate:
                fails.append(
                    "clause 2: %s has no `; %s` install line -- the carriers did not run (knob off, "
                    "or a build without P9 (1/2))" % (label, GATE_NEEDLE)
                )
            else:
                armed, displaced, mism = gate[-1]
                if armed == 2 and mism == 0:
                    pass  # carried: patched
                elif displaced == 2 and mism == 0:
                    pass  # carried: displaced (configuration (2) -- clause 0 refuses it separately)
                else:
                    fails.append(
                        "clause 2: %s's gate line reads %d/2 patched, %d displaced, %d MISMATCHED "
                        "-- neither `2/2 patched` nor `2 displaced`"
                        % (label, armed, displaced, mism)
                    )
        else:  # storm / countinit: the gate knob is OFF by construction
            if gate:
                fails.append(
                    "clause 2: %s has a `; %s` install line (%d/2 patched, %d displaced) -- the "
                    "gate was ON, so this is not the ungated %s arm"
                    % (label, GATE_NEEDLE, gate[-1][0], gate[-1][1], expect)
                )
        # count_init: the storm arm is the STOCK threshold-0 start (knob off, no line); the countinit
        # arm is the root fix alone (a line on every peer reading the threshold up to players*100).
        if expect == "storm" and ev["count_init"]:
            fails.append(
                "clause 3: %s carries a `; %s` line (%d -> %d) -- resync_count_init was ON, so the "
                "threshold was not the stock 0 and this is not the reproduction"
                % (label, COUNT_INIT_NEEDLE, ev["count_init"][-1][0], ev["count_init"][-1][1])
            )
        if expect == "countinit":
            if not ev["count_init"]:
                fails.append(
                    "clause 3: %s has no `; %s` line -- resync_count_init did not run (knob off, "
                    "or the run never reached a lockstep match)" % (label, COUNT_INIT_NEEDLE)
                )
            else:
                before, after, thr = ev["count_init"][-1]
                if after < 2 or thr != after * 100:
                    fails.append(
                        "clause 3: %s's count_init reads %d -> %d (threshold %d) -- expected >= 2 "
                        "players and threshold == players*100" % (label, before, after, thr)
                    )
    host_ev, client_ev = peers[0][1], peers[1][1]
    carrier = "none"
    if host_ev["gate"]:
        a, dsp, _m = host_ev["gate"][-1]
        carrier = "patched" if a == 2 else ("displaced" if dsp == 2 else "mixed")
    # clause 3
    if expect == "countinit":
        # The count fix's own claim: the threshold-0 fire is gone. A barrier is allowed, but only
        # one that BEGINs at count == threshold == players*100 on the leader (the receiver's BEGIN
        # carries ITS count, 0 -- it never counts -- so only the leader's are judged).
        want = host_ev["count_init"][-1][2] if host_ev["count_init"] else -1
        # b[2] is the THRESHOLD the barrier carried and b[1] the count the once-per-frame sampler
        # last saw. The threshold is the fix's actual claim and is tested exactly. The COUNT is not:
        # see the SAMPLE_SLACK note above -- a frame that straddles two keepalive increments reports
        # threshold-1 for a fire that happened at threshold+1 (mp:P9C, 2026-09-23). Tolerating that
        # is not loosening the assertion: a threshold-0 fire -- the thing resync_count_init exists
        # to abolish -- reads 0 against 200 and is still caught by a mile.
        low = [b for b in host_ev["barriers"] if b[2] != want or b[1] < want - SAMPLE_SLACK]
        if low:
            fails.append(
                "clause 3: the leader began %d barrier(s) below/off the initialised threshold %d "
                "(first: count was %d, threshold %d) -- a threshold-0 fire, or a stale count"
                % (len(low), want, low[0][1], low[0][2])
            )
    if expect == "carried":
        # Classified on the LEADER only: a BEGIN line's `countdown` is the writing peer's own
        # SYNC_RETRY_COUNTDOWN, and the receiver's barrier is set by the leader's resync-state frame
        # while its own countdown sits at 60 -- its lines MIRROR the leader's, they carry no class.
        storm_class = [b for b in host_ev["barriers"] if b[3] >= GATE_COUNTDOWN]
        if storm_class:
            fails.append(
                "clause 3: the leader began %d storm-class barrier(s) (countdown >= 0x%x at BEGIN: "
                "a fire while nags were still answered) -- the gate did not hold: %s"
                % (len(storm_class), GATE_COUNTDOWN, watch_summary(host_ev))
            )
        if len(client_ev["barriers"]) > len(host_ev["barriers"]):
            fails.append(
                "clause 3: the client began %d barrier(s) but the leader only %d -- a barrier the "
                "leader did not fire (the receiver's flag has no other writer than the leader's "
                "frame): %s"
                % (len(client_ev["barriers"]), len(host_ev["barriers"]), watch_summary(client_ev))
            )
    if expect in ("carried", "countinit"):
        for label, ev in peers:
            if expect == "carried" and ev["fired"]:
                fails.append(
                    "clause 3: %s carries %d `; %s` line(s) with the gate carried -- the storm is "
                    "not stopped: %s" % (label, len(ev["fired"]), FIRED_NEEDLE, watch_summary(ev))
                )
            if ev["desync_bad"]:
                fails.append(
                    "clause 3: %s carries %d `%s` line(s) -- the pair did not stay identical: %s"
                    % (label, len(ev["desync_bad"]), DESYNC_BAD_NEEDLE, ev["desync_bad"][0].strip())
                )
            if not ev["desync_match_steps"]:
                fails.append(
                    "clause 3: %s has no `; [desync] step=N peer=P MATCH` line -- REFUSED: either "
                    "the row lost its `[desync] verbose=1` fragment (ini/desync_verbose.ini) or the "
                    "walk never reached a live match; a run the watch never judged is not a clean "
                    "run" % label
                )
        hmax = max(host_ev["desync_match_steps"] or [0])
        if host_ev["desync_match_steps"] and hmax < min_steps:
            fails.append(
                "clause 3: the host's last agreeing sample is at step %d < %d -- the match was too "
                "short to say anything about a counter that fires on cumulative nags"
                % (hmax, min_steps)
            )
    else:
        # THE ROW IS expect_red AND STAYS SO: this arm asserts the desired behaviour ("no storm"),
        # which the stock start violates, so its red is the reproduction and carries the numbers.
        if not host_ev["armed"]:
            fails.append(
                "clause 3: the leader has no `; %s` line -- the run never reached a lockstep match "
                "(or ended before the watch's first 10 s tick), so it says nothing about the storm"
                % ARMED_NEEDLE
            )
        nb = len(host_ev["barriers"])
        if nb >= min_barriers:
            b = host_ev["barriers"]
            fails.append(
                "clause 3: THE STOCK START STORMS -- the leader began %d barrier(s) >= %d "
                "(threshold %s, count at BEGIN %s) -- %s"
                % (
                    nb,
                    min_barriers,
                    "/".join(sorted({str(x[2]) for x in b})),
                    "/".join(sorted({str(x[1]) for x in b})),
                    watch_summary(host_ev),
                )
            )
    summary = (
        "carrier=%s | host: %s | client: %s | host desync MATCH lines=%d (max step %d), bad=%d"
        % (
            carrier,
            watch_summary(host_ev),
            watch_summary(client_ev),
            len(host_ev["desync_match_steps"]),
            max(host_ev["desync_match_steps"] or [0]),
            len(host_ev["desync_bad"]),
        )
    )
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------

MODULES_ABSENT = (
    "; [modules] mh_net: BOUND at DllMain (under the loader lock) -- 27 exports resolved, init "
    "returned F4B00005, call-through ok; module DLL_PROCESS_ATTACH was AFTER mh.dll's by 1 us",
    "; [modules] libmh: NOT BOUND -- LoadLibrary(x\\libmh.dll) failed, Win32 error 126 (the file "
    "is not there). This run is CONFIGURATION (1)",
)
MODULES_BOUND = (
    MODULES_ABSENT[0],
    "; [modules] libmh: BOUND at DllMain (under the loader lock) -- 81 exports resolved, init "
    "returned F4D00001, call-through ok",
)
END_MARKER = "; [interlock] 0 detour install(s) refused -- every site got its entry"
CROSS_1 = "; [libmh] crossings=0 absent-calls=809 -- the spine boundary was NOT ENTERED in this run (configuration (1))"
CROSS_2 = "; [libmh] crossings=13678 absent-calls=0 -- the spine boundary was ENTERED in this run (configuration (2))"
GATE_LINE = "; resync-trigger gate: %d/2 INC sites patched (gated on SYNC_RETRY_COUNTDOWN<0x38), %d displaced by promotion, %d MISMATCHED"
UNCARRIED_LINE = (
    "; [net] UNCARRIED FIX: resync_trigger_gate is set, but neither its byte-patch carrier at "
    "0049C508 nor a promoted owner (llm_net_lockstep_dispatch) is in this run"
)


def plant(
    root,
    name,
    config1=True,
    gate=(2, 0, 0),
    uncarried=False,
    fires=0,
    match_steps=1600,
    verbose=True,
    desync=False,
    watch=True,
    resets=0,
    barriers=0,
    barrier_ends=None,
    count_init=None,
    lockstep_game=None,
    armed=True,
    barrier_count=0,
    barrier_thr=0,
    barrier_countdown=59,
):
    proc = os.path.join(root, name, "logs", "20260922T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20260922T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    boot = list(MODULES_ABSENT if config1 else MODULES_BOUND)
    if uncarried:
        boot.append(UNCARRIED_LINE)
    if gate is not None:
        boot.append(GATE_LINE % gate)
    boot.append(END_MARKER)
    boot.append(CROSS_1 if config1 else CROSS_2)
    with open(os.path.join(proc, "mh_net.log"), "w", encoding="utf-8") as fh:
        fh.write("".join("[00:00:00.000] %s\n" % ln for ln in boot))
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        fh.write("[00:00:01.000] ; [desync] session reset -- sampling from step 1\n")
        if verbose:
            for s in range(50, match_steps + 1, 50):
                fh.write(
                    "[00:00:02.000] ; [desync] step=%d peer=1 MATCH state=%016X\n" % (s, 0xA000 + s)
                )
        if desync:
            fh.write(
                "[00:00:03.000] ; [desync] *** DESYNC step=%d peer=1 ours=1 theirs=2 first_region=41 order_queue\n"
                % (match_steps // 2)
            )
        if armed:
            fh.write(
                "[00:00:11.000] ; [resync] armed: trigger_count=0 threshold=%d (ACTIVE_PLAYER_COUNT=%d) countdown=60\n"
                % ((200, 2) if count_init else (0, 0))
            )
        cnt = 0
        k = 0
        for i in range(8 if watch else 0):
            cnt += 26
            fh.write(
                "[00:00:%02d.000] ; [resync] trigger_count=%d threshold=200 countdown=%d (+26 in 10s)\n"
                % (10 + i, cnt, 60 - i)
            )
            if fires and cnt > 200 and k < fires:
                k += 1
                fh.write(
                    "[00:00:%02d.500] ; [resync] force_resync FIRED #%d: count %d -> 0 (threshold 200, countdown 12)\n"
                    % (10 + i, k, cnt)
                )
                cnt = 0
        while k < fires:  # more fires than the watch loop produced: a burst
            k += 1
            fh.write(
                "[00:01:%02d.000] ; [resync] force_resync FIRED #%d: count 201 -> 0 (threshold 200, countdown 3)\n"
                % (k % 60, k)
            )
        if count_init is not None:
            fh.write(
                "[00:00:01.500] ; [resync] count_init: ACTIVE_PLAYER_COUNT %d -> %d (threshold %d)\n"
                % count_init
            )
        # watch v2: one BEGIN per barrier, an END ~2 s later unless the rig's defang latched it
        n_ends = len(range(barriers)) if barrier_ends is None else barrier_ends
        for i in range(barriers):
            fh.write(
                "[00:02:%02d.000] ; [resync] barrier #%d BEGIN (count was %d, threshold %d, countdown %d) flags=0x00\n"
                % ((2 * i) % 60, i + 1, barrier_count, barrier_thr, barrier_countdown)
            )
            if i < n_ends:
                fh.write(
                    "[00:02:%02d.100] ; [resync] barrier #%d END after 2003 ms flags=0x40\n"
                    % ((2 * i + 1) % 60, i + 1)
                )
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20260922T000000Z_menu_solo"}, fh)
    with open(os.path.join(sess, "mh_lockstep.log"), "w", encoding="utf-8") as fh:
        if lockstep_game:  # a `game` column with the planted mode sequence (corroboration only)
            fh.write("# wall_ms clock_ms sess game flags\n")
            for i, g in enumerate(lockstep_game):
                fh.write("%d %d 3 %d 0x00\n" % (i * 500, i * 500, g))
    return sess


def selftest():
    cases = [
        ("carried: 2/2 patched, clean, 1600 steps", "carried", {}, {}, True),
        (
            "carried: 2 displaced (configuration (2)) -> clause 0 refuses it",
            "carried",
            dict(config1=False, gate=(0, 2, 0)),
            dict(config1=False, gate=(0, 2, 0)),
            False,
        ),
        ("carried NEG: UNCARRIED line on the client", "carried", {}, dict(uncarried=True), False),
        ("carried NEG: no gate install line on the host", "carried", dict(gate=None), {}, False),
        ("carried NEG: 1 MISMATCHED site", "carried", dict(gate=(1, 0, 1)), {}, False),
        ("carried NEG: the leader fired twice", "carried", dict(fires=2), {}, False),
        ("carried NEG: match too short (600 steps)", "carried", dict(match_steps=600), {}, False),
        ("carried NEG: a *** DESYNC line", "carried", dict(desync=True), {}, False),
        (
            "carried NEG: no MATCH lines (verbose fragment lost) -> refused",
            "carried",
            dict(verbose=False),
            {},
            False,
        ),
        (
            "carried NEG: client lane still carried libmh (configuration (2))",
            "carried",
            {},
            dict(config1=False),
            False,
        ),
        (
            "storm RED (the row's expected state): gate off, 6 barriers (watch v2), no count_init",
            "storm",
            dict(gate=None, barriers=6, lockstep_game=[2, 8, 2, 8, 2, 8, 2, 8, 2, 8, 2, 8, 2]),
            dict(gate=None, barriers=6),
            False,
        ),
        (
            "storm RED: 6 barriers with NO END lines (the defang latch) still count as seen",
            "storm",
            dict(gate=None, barriers=6, barrier_ends=0),
            dict(gate=None),
            False,
        ),
        (
            "storm XPASS: only 4 barriers (< 5) over a reached match -- the stock start did not storm",
            "storm",
            dict(gate=None, barriers=4),
            dict(gate=None),
            True,
        ),
        (
            "storm XPASS: 4 FIRED lines but 0 barrier BEGINs -- the counter is the secondary witness",
            "storm",
            dict(gate=None, fires=4),
            dict(gate=None),
            True,
        ),
        (
            "storm RED: no armed line (the match was never reached) is not an XPASS",
            "storm",
            dict(gate=None, watch=False, armed=False),
            dict(gate=None),
            False,
        ),
        (
            "storm NEG: a count_init line (the knob was on -> threshold 200, not the stock start)",
            "storm",
            dict(gate=None, barriers=6, count_init=(0, 2, 200)),
            dict(gate=None),
            False,
        ),
        (
            "carried NEG: one barrier BEGIN on the client the leader never fired (0 on the host)",
            "carried",
            {},
            dict(barriers=1),
            False,
        ),
        (
            "carried: one leader barrier at countdown 55 (gate-conformant) + its mirror on the client passes",
            "carried",
            dict(barriers=1, barrier_countdown=55),
            dict(barriers=1, barrier_countdown=60),
            True,
        ),
        (
            "carried NEG: one leader barrier at countdown 59 (storm-class: nags still answered)",
            "carried",
            dict(barriers=1, barrier_countdown=59),
            dict(barriers=1, barrier_countdown=60),
            False,
        ),
        (
            "carried NEG: a leader barrier at countdown 55 but TWO on the client (one the leader never fired)",
            "carried",
            dict(barriers=1, barrier_countdown=55),
            dict(barriers=2, barrier_countdown=60),
            False,
        ),
        (
            # mp:P9C. THE REGRESSION ARM FOR SAMPLE_SLACK. Before 2026-09-23 this reddened clause 3
            # as "a threshold-0 fire, or a stale count". It is neither: the trigger increments and
            # compares the same memory in one call and cannot fire below the threshold, so a sample
            # of 199 is the once-per-frame watch having straddled two keepalive increments. Measured
            # live at 1 of 15 fires under box load, 14 of which read exactly 200.
            "countinit: a barrier whose SAMPLED count landed one short (199 of 200) is still a fire",
            "countinit",
            dict(gate=None, count_init=(0, 2, 200), barriers=1, barrier_count=199, barrier_thr=200),
            dict(gate=None, count_init=(0, 2, 200)),
            True,
        ),
        (
            # The negative half, and the reason SAMPLE_SLACK is 10 rather than "any shortfall": the
            # threshold-0 fire is the whole point of resync_count_init, and it must still be caught.
            "countinit NEG: a threshold-0 fire is still caught (count 0 of 200), slack or no slack",
            "countinit",
            dict(gate=None, count_init=(0, 2, 200), barriers=1, barrier_count=0, barrier_thr=200),
            dict(gate=None, count_init=(0, 2, 200)),
            False,
        ),
        (
            # And the boundary itself is asserted, so nobody widens SAMPLE_SLACK without a reason:
            # one past the slack still reds.
            "countinit NEG: a sample 11 short (189 of 200) is outside SAMPLE_SLACK and reds",
            "countinit",
            dict(gate=None, count_init=(0, 2, 200), barriers=1, barrier_count=189, barrier_thr=200),
            dict(gate=None, count_init=(0, 2, 200)),
            False,
        ),
        (
            "countinit: gate off, count_init 0->2 on both, 0 barriers, 1600 steps",
            "countinit",
            dict(gate=None, count_init=(0, 2, 200)),
            dict(gate=None, count_init=(0, 2, 200)),
            True,
        ),
        (
            "countinit NEG: no count_init line on the client",
            "countinit",
            dict(gate=None, count_init=(0, 2, 200)),
            dict(gate=None),
            False,
        ),
        (
            "countinit NEG: count_init read 0 -> 0 (no ALIVE|HUMAN slot -- the pre-P9 rig model)",
            "countinit",
            dict(gate=None, count_init=(0, 0, 0)),
            dict(gate=None, count_init=(0, 0, 0)),
            False,
        ),
        (
            "countinit: 3 barriers that BEGIN at count 200 == threshold 200 are the fix working",
            "countinit",
            dict(gate=None, count_init=(0, 2, 200), barriers=3, barrier_count=200, barrier_thr=200),
            dict(gate=None, count_init=(0, 2, 200)),
            True,
        ),
        (
            "countinit NEG: a barrier BEGIN at count 0 / threshold 0 (the stock fire) on the host",
            "countinit",
            dict(gate=None, count_init=(0, 2, 200), barriers=1),
            dict(gate=None, count_init=(0, 2, 200)),
            False,
        ),
        (
            "countinit NEG: a barrier BEGIN at count 37 / threshold 200 (below the threshold)",
            "countinit",
            dict(gate=None, count_init=(0, 2, 200), barriers=1, barrier_count=37, barrier_thr=200),
            dict(gate=None, count_init=(0, 2, 200)),
            False,
        ),
        (
            "countinit NEG: the gate install line is present (gate was on -> not the fix alone)",
            "countinit",
            dict(count_init=(0, 2, 200)),
            dict(gate=None, count_init=(0, 2, 200)),
            False,
        ),
        (
            "storm XPASS: 5 sub-threshold drops are recovery resets, not barriers",
            "storm",
            dict(gate=None, fires=0, resets=5),
            dict(gate=None),
            True,
        ),
        (
            "carried: recovery resets with the gate carried are fine",
            "carried",
            dict(resets=3),
            {},
            True,
        ),
        (
            "storm NEG: the gate line is present (knob was on)",
            "storm",
            dict(barriers=6),
            dict(gate=None),
            False,
        ),
        (
            "storm NEG: UNCARRIED line (a build that lost the carrier)",
            "storm",
            dict(gate=None, barriers=6, uncarried=True),
            dict(gate=None),
            False,
        ),
        (
            "storm NEG: libmh bound on the host",
            "storm",
            dict(gate=None, barriers=6, config1=False),
            dict(gate=None),
            False,
        ),
    ]
    bad = 0
    for label, expect, hkw, ckw, want in cases:
        root = tempfile.mkdtemp(prefix="p9chk_")
        try:
            h = plant(root, "host", **hkw)
            c = plant(root, "client", **dict(dict(fires=0, watch=False), **ckw))
            fails, _ = check(h, c, expect)
            got = not fails
            ok = got == want
            print(
                "  %s  %s%s"
                % ("ok " if ok else "BAD", label, "" if got else "  -> " + "; ".join(fails)[:300])
            )
            bad += 0 if ok else 1
        finally:
            shutil.rmtree(root, ignore_errors=True)
    # a dead boot (no mh_net.log anywhere) is a failure, never a pass
    root = tempfile.mkdtemp(prefix="p9chk_")
    try:
        h = os.path.join(root, "host")
        c = os.path.join(root, "client")
        os.makedirs(h)
        os.makedirs(c)
        fails, _ = check(h, c, "carried")
        ok = bool(fails)
        print("  %s  either arm NEG: no mh_net.log at all" % ("ok " if ok else "BAD"))
        bad += 0 if ok else 1
    finally:
        shutil.rmtree(root, ignore_errors=True)
    n = len(cases) + 1
    print("check_resync_storm selftest: %d/%d" % (n - bad, n))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--expect", choices=("carried", "storm", "countinit"), default="carried")
    ap.add_argument(
        "--min-steps",
        type=int,
        default=1000,
        help="carried: the host's last agreeing desync-watch sample must be at least this sim step",
    )
    ap.add_argument(
        "--min-barriers",
        type=int,
        default=5,
        help="storm: the leader must have BEGUN at least this many mode-8 barriers (watch v2)",
    )
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if len(args.dirs) != 2:
        print(
            "usage: check_resync_storm.py --expect carried|storm|countinit <host-run-dir> <client-run-dir>"
        )
        return 2
    fails, summary = check(
        args.dirs[0], args.dirs[1], args.expect, args.min_steps, args.min_barriers
    )
    if fails:
        print("check_resync_storm: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print("check_resync_storm: PASS -- %s arm (%s)" % (args.expect, summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
