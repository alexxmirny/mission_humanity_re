#!/usr/bin/env python3
"""run_gate.py -- the DLL gate (src/mh_dll/README.md "The gate") as ONE command, overlapped.

    python tools/run_gate.py                # the whole gate, ~7 cores of local work + the VMs
    python tools/run_gate.py --skip det,spdet  # e.g. on a box without the rig peers
    python tools/run_gate.py --list         # the unit roster and what each runs

WHY IT EXISTS (2026-09-10, user). Run serially, the documented steps cost ~25-35 min and most of
that is waiting: determinism is WALL-CLOCK bound on the VMs while this box idles, and the local
steps (suite, the two A/B/C recorded sessions, selftests, lint) are independent
correctness runs that only share the CPU. This driver runs the build first (everything below
executes its output), then schedules every remaining unit under a CORE BUDGET (default: all
logical CPUs minus one -- 7 on the dev box, the user's call), with determinism started first
since it costs the most wall and the least local CPU. Top-level wall becomes roughly
max(determinism, the local chain) instead of the sum.

WHAT MAKES THE OVERLAP SAFE -- each point is load-bearing, none is new policy:
  * ONE rig lease for the whole gate: this process acquires `rig` and exports the pass-through
    env (hostlock.run_rig_tool's own mechanism), so the child test_ui/mp_run invocations run
    lease-free instead of deadlocking on their parent's lease.
  * Correctness runs only, per the parallel-lane notes: every unit here produces a verdict, not a
    timing. The one pacing-sensitive step (determinism --ship-pacing) runs on the VMs, where this
    box's load is ssh chatter.
  * Lane provisioning self-serialises (make_lane's boot_lock) and every game boot holds the same
    machine-wide lock, so concurrent units cannot race the shared packs into the Insert-CD modal.
  * Weights are PROCESS-COUNT estimates, not measurements: a unit's weight is roughly the game
    instances + build load it puts on the box, and the budget caps the sum. They only need to be
    honest enough to keep the box responsive; the verdicts do not depend on them.

Output: each unit streams to tmp/gate/<unit>.log; the console gets start/finish lines, a failed
unit's log tail, and the final table. Exit 0 only if every unit passed."""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostlock  # noqa: E402
import machine_config as machine  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
LOG_DIR = os.path.join(REPO, "tmp", "gate")


# The roster. `weight` = the cores the unit roughly occupies (game processes + build load);
# `timeout` = the wall-clock kill bound, generous on purpose -- a stuck unit is reported as
# TIMEOUT, never silently waited out past it. Longest-wall units first: the scheduler starts
# them in this order, so the long poles begin immediately.
def units(args):
    rows = [
        {
            "name": "det",
            "why": "step 4 -- 2-VM lockstep determinism through the real menu->lobby->Start",
            "cmd": [
                PY,
                os.path.join(REPO, "tools", "det_arms.py"),
                "--determinism",
                "--ship-pacing",
                "--steps",
                str(args.steps),
            ],
            # WEIGHT 2 AND STARTED FIRST (fork F4H). It was 0 -- "the peers run ON the VMs; this box
            # only orchestrates over ssh, so charging it a core would delay a real local unit for
            # load that never materialises." The load materialises: the peers are **Hyper-V guests on
            # this host**, so their vCPUs are this box's cores and the gate was pricing two live game
            # instances at nothing. What that costs is specific rather than general -- the unit's
            # whole verdict hangs on a lobby RENDEZVOUS that must complete inside the host script's
            # ~100 s window, and a gate run whose det unit fails does so there, with the runner's own
            # ssh polls timing out beside it.
            #
            # `first` overrides longest-first for this one row, and the reason is that longest-first
            # is a rule about units competing for LOCAL cores. det's cost is a wall clock somewhere
            # else; its price here is two vCPUs it must have while it has them. Starting it at t0 at
            # its real weight also produces the STAGGER F4H's scope asked for, without a sleep and
            # without a special case in the dispatcher: the suite's 26 lane boots no longer fit in
            # the remaining budget at t0, so they run in the slot det leaves ~200 s later.
            "weight": 2,
            "first": True,
            # THE VM SLOT (gate diet block 5b): det is the roster's original VM-driving row -- see
            # the scheduler's `free_vm` note below for why this is now a marker other rows share
            # rather than an unstated assumption.
            "vm": True,
            "timeout": 2400,
        },
        {
            # mp:U19j into the gate (user 2026-09-24). Wave 6 lane D proved the shape on the rig by
            # hand (`--u19j-gpfg3` / `--u19j-gpfg3-unguarded`, 3/3 + 3/3) but left it OUT of any gate
            # unit -- a 3-peer topology (host vms[0], survivor vms[1], a SIM-FENCED peer as a local
            # DET3 lane) that no `multi` TESTS row can carry, same class as --u19b-quit3/--l1f-ping3.
            # BOTH ARMS in one unit, sequentially inside the child (det_arms.py's own dispatch already
            # runs guarded then unguarded and ORs the verdicts) -- a red on EITHER arm reds this row.
            "name": "u19j_gpfg3",
            "why": "step 4f -- mp:U19j's 3-peer gone-peer frame guard: the carrier FIRES (guarded) "
            "and the unguarded twin goes red on exactly the garbled frame (XFAIL)",
            "cmd": [
                PY,
                os.path.join(REPO, "tools", "det_arms.py"),
                "--u19j-gpfg3",
                "--u19j-gpfg3-unguarded",
            ],
            # weight 3, not det's 2: this shape ALSO provisions a THIRD peer as a local DET3 lane
            # (the sim-fenced side) beside the two VM vCPUs.
            "weight": 3,
            "vm": True,
            "timeout": 1200,
        },
        {
            # mp:X2a into the gate: the open-redirect witnessed on the two INDEPENDENT rig VMs (not a
            # local lane -- make_lane.py symlinks Maps to one shared image, so a local lane's client
            # base file IS the host's content and a broken redirect would still open matching bytes;
            # see run_x2a_map_variant's docstring in tools/det_arms.py). The client's own map is
            # mutated (tools/map_variant.py) and RESTORED in a `finally` regardless of verdict.
            "name": "x2a_map_variant",
            "why": "step 4g -- mp:X2a's open-redirect: the client's OWN, genuinely different "
            "blue monday.mpm is redirected to the downloaded mh_dl\\ copy, not opened directly",
            "cmd": [
                PY,
                os.path.join(REPO, "tools", "det_arms.py"),
                "--x2a-map-variant",
            ],
            # weight 2, matching det: two VM vCPUs, no local lane.
            "weight": 2,
            "vm": True,
            "timeout": 900,
        },
        {
            "name": "suite",
            "why": "step 3 -- the UI regression suite + the pooled tactical-journal tail",
            "cmd": [
                PY,
                os.path.join(REPO, "tools", "test_ui.py"),
                "--jobs",
                str(args.suite_jobs),
                # THE SECOND POOL, NAMED (fork F4H). The suite runs TWO pools -- `--jobs` solo tests
                # plus `--net-jobs` multi-peer ones at two game processes each -- and its default
                # --net-jobs is 4. So a unit priced at 2 was launching up to 2 + 4*2 = TEN game
                # instances, and the gate's core budget was bounding a number it had never counted.
                # Pinning it to the unit's own weight makes the price and the footprint the same
                # statement. The suite is not the gate's long pole (abc_spcamp is, and the dropped
                # promoted-vs-original unit was), so what this costs in suite wall is mostly
                # absorbed by the overlap.
                #
                # GATE DIET BLOCK 3 (2026-09-24): the multi-peer pool is now its OWN knob
                # (--suite-net-jobs) and is PRICED as what it is -- wait-bound. A capped multi-peer
                # lane sleeps between frames (gate_timeline measured avg 0.11 CPU-busy games across
                # the suite), so one net job costs NET_JOB_WEIGHT of a core, not two. Pinning it to
                # --suite-jobs (2) had the suite's 60-odd multi rows queue behind a CPU budget they
                # barely use.
                "--net-jobs",
                str(args.suite_net_jobs),
            ],
            # = jobs (2), down from jobs+1: since the [pacing] fps_cap every multi-peer lane
            # SLEEPS between frames instead of spinning a core, so the suite's real footprint is
            # about its job count. It was set while the weight-3 promoted-vs-original unit still
            # ran, and it is what let both A/B/C units start at t0 (3+2+2 = 7) instead of queueing
            # behind the suite -- the run-3 late-start defect. That unit is gone (F5M S4b), so the
            # budget it was competing for is slacker, not tighter; the figure stands unchanged
            # because it is a statement about the SUITE's own footprint, not about the field.
            "weight": args.suite_jobs + int(math.ceil(args.suite_net_jobs * NET_JOB_WEIGHT)),
            "timeout": 2400,
        },
        # ---- THE `ab` UNIT DROPPED AT FORK F5M S4b (the archive-tool cut) ---------------------
        # It ran the promoted-vs-original A/B over the registered sim fixtures -- step 3b of the
        # documented gate. WHAT IT GATED, precisely, because nothing else in this roster asks the
        # same question: for every sim function we had PROMOTED, it replayed a recorded fixture
        # twice inside the real game -- once with the original body reached, once with ours -- and
        # required the two state traces to agree, with a control arm that rolled the promotion key
        # back to prove the harness could still tell them apart and a go-red arm that poked the
        # fixture to prove a difference would be seen. It compared OUR body against the ORIGINAL
        # body rather than against a recorded expectation, per function and per fixture.
        #
        # WHERE THAT COVERAGE LIVES NOW, measured rather than assumed: MOSTLY IN `spdet`, which is
        # the same question asked at a coarser grain -- it runs the single-player oracle twice from
        # a pinned seed, UNPROMOTED then PROMOTED, and requires all 61 region channels to agree over
        # 800 steps. That is original-vs-ours inside the real binary, for every promoted body at
        # once. WHAT IS GENUINELY DROPPED, and it is dropped consciously: the PER-FIXTURE, per-
        # function grain (a divergence in spdet names a channel and a step, not a function), the
        # recorded sim fixtures the A/B replayed, and the poke-driven go-red arm that re-proved on
        # every run that a difference WOULD be seen. Nothing else gates that last one. Both losses
        # are era-bound: the grain mattered while bodies were still changing hands one at a time,
        # and the set is now frozen. The rest of the roster covers the remaining directions --
        # `libref` replays three fixtures x 5000 steps against recorded goldens (our body vs. its
        # own recorded behaviour, and the ONLY unit that needs no retail binary at all), `det`
        # proves two peers stay bit-identical in lockstep, `selftests` runs the per-domain suites
        # under ASan, and the A/B/C units replay recorded play. The driver itself is not deleted:
        # a private session can still run it by hand on a box that has the game.
        # The gate loses one unit (its long pole at ~260 s hint / 814 s serial before --jobs 2), so
        # the remaining weights are no longer competing with a weight-3 unit at t0.
        {
            "name": "selftests",
            "why": "step 2 -- ASan + plain, flaky suites repeated (the commit gate)",
            "cmd": [PY, os.path.join(REPO, "tools", "run_selftests.py")],
            "weight": 3,  # the ASan msbuild is the heavy half
            "timeout": 1200,
        },
        {
            "name": "abc_spcamp",
            "why": "step 3c -- the campaign session, three arms (A/B parallel since 2026-09-10)",
            "cmd": [PY, os.path.join(REPO, "tools", "ui_abc.py"), "--ui-abc", "spcamp_solo"],
            "weight": 2,
            "timeout": 1800,
        },
        {
            "name": "abc_tutorial",
            # --ui-slot 2: DISJOINT from abc_spcamp's default 0/1 pair. The first gate run had both
            # A/B/C units provisioning the same ui_play lanes concurrently, and the second's rmtree
            # ate the first's live lane -- distinct slot bases are what make the pair overlappable.
            "why": "step 3d -- the tutorial session, three arms",
            "cmd": [
                PY,
                os.path.join(REPO, "tools", "ui_abc.py"),
                "--ui-abc",
                "tutorial_solo",
                "--ui-slot",
                "2",
            ],
            "weight": 2,
            "timeout": 1200,
        },
        {
            # X-SPINE clause (6a), 2026-09-10: the ARM-SYMMETRIC VERDICT CHANNEL, gated.
            #
            # The `R <step> <h0>..<hN>` per-region line is address-content -- it reads the same bytes
            # whichever implementation wrote them -- so it is the one verdict channel that survives
            # promotion, un-promotion and rebinding. Until now it was compared for eight columns
            # (SP_TIME_REGIONS) and used only to LOCALISE an already-declared divergence; it now
            # carries the whole 61-column set as 61 independent channels, and this unit is what makes
            # that a gate rather than a diagnostic somebody remembers to run.
            #
            # WHY IT IS NOT COVERED BY `det`: that unit is 2-peer MP, and a symmetric MP run makes
            # both peers wrong identically. This is one peer run TWICE -- unpromoted, then promoted --
            # over the same scripted session with the wall clock pinned, so every channel must agree
            # and the single-player paths (which the MP gate cannot reach at all) are in scope.
            "name": "spdet",
            "why": "step 4b -- the single-player oracle: unpromoted vs promoted, all 61 region channels",
            "cmd": [
                PY,
                os.path.join(REPO, "tools", "det_arms.py"),
                "--sp-determinism",
                "--steps",
                "800",
                # PINNED, not drawn: the seed is the workload, and a gate whose input changes every
                # run cannot tell a regression from a different scenario.
                "--sp-seed",
                "424242",
            ],
            "weight": 1,  # one local lane, two sequential headless arms
            "timeout": 1800,
        },
        {
            # fork F4G: CONFIGURATION (3), which until now no tool and no unit ran. The committed
            # standalone fixtures are replayed against Release\standalone\libmh.dll by libref_host,
            # 5000 steps each, and every one must report ALL STEPS IDENTICAL over its full declared
            # step count. The unit also asserts its own SUBJECT -- it reads libref_host.exe's import
            # table and fails if libmh.dll is not in it, because a build that quietly went back to
            # linking the archive would replay exactly as green.
            #
            # PURELY LOCAL AND RIG-FREE: three console processes, no lane, no VM, no game copy
            # (measured: 21-24 s each, ~70 s serial). Weight 1 rather than 3 because they run one
            # after another inside the unit, not concurrently.
            "name": "libref",
            "why": "step 4c -- configuration (3): the standalone fixtures against libmh.dll",
            "cmd": [PY, os.path.join(REPO, "tools", "replay_libref.py")],
            "weight": 1,
            "timeout": 1800,
        },
        {
            # fork F4C-GATE: the IN-MEMORY STATIC PATCH, live. The offline half of this oracle is a
            # lint row (it runs the whole comparison over a synthetic dump and plants mutations);
            # this unit is the half only a process can answer -- mh.dll applying the manifest to its
            # OWN loaded image, and the dump read back OUT of that image compared against the file
            # mhpatch produces from the same manifest and the same clean exe.
            #
            # IT IS A GATE UNIT BECAUSE IT IS CHEAP, and that was measured before it was promised
            # (the item's own instruction). `[patch] dump_exit=1` terminates the process one
            # instruction after the apply, so a manifest costs one boot-lock window and no render
            # loop: 0.2 s of lane time for the 7,770-site grand manifest, 0.0 s for each pool one,
            # 1.3 s wall for all three INCLUDING lane provisioning and the three mhpatch reference
            # builds. The rig cost is the boot lock, held for a fraction of a second per manifest.
            #
            # WEIGHT 1, TIMEOUT GENEROUS: one local lane, three sequential launches. It takes the
            # same machine-wide boot lock every game launch does, so it queues behind the suite's
            # peers rather than racing them.
            "name": "inmem",
            "why": "step 4d -- in-memory static-patch parity, live, over every shipped manifest",
            "cmd": [PY, os.path.join(REPO, "tools", "check_inmem_patch_parity.py"), "--run"],
            "weight": 1,
            "timeout": 900,
        },
        {
            "name": "lint",
            "why": "step 5 -- lint_repo (clang-format, ruff, every drift gate)",
            "cmd": [PY, os.path.join(REPO, "tools", "lint_repo.py")],
            "weight": 2,
            "timeout": 900,
        },
    ]
    return [r for r in rows if r["name"] not in args.skip]


# Expected wall seconds per unit when no measured record exists yet -- ONLY a scheduling hint
# (longest-first), never a budget. The real record is tmp/gate/last_timings.json, written by
# every finished run, so the schedule self-corrects from its own history (the same lesson the
# suite's last_suite_timing.json exists for: a figure in prose is a figure nobody re-measures).
DUR_HINTS = {
    "suite": 280,
    "abc_spcamp": 200,
    "selftests": 180,
    "det": 150,
    # Measured STANDALONE (wave 7 lane D, not yet inside a real gate run -- this self-corrects from
    # tmp/gate/last_timings.json the first time the gate actually runs them): u19j_gpfg3's guarded
    # arm ran inside a combined 388 s run whose unguarded half hit a crashy VM (G315); a clean
    # unguarded-alone retry took 294 s, so ~480 s covers both arms with margin. x2a_map_variant ran
    # 71 s alone (host+client VM match plus the scp backup/push/restore).
    "u19j_gpfg3": 480,
    "x2a_map_variant": 90,
    "abc_tutorial": 120,
    "spdet": 100,
    "libref": 75,
    "lint": 60,
    "inmem": 10,
}
TIMINGS = os.path.join(LOG_DIR, "last_timings.json")
# The comparison base for the growth figures on a red line: the last run that was GREEN on every
# unit AND every rule below. Copied from TIMINGS only by a fully green run.
GREEN_TIMINGS = os.path.join(LOG_DIR, "last_green_timings.json")
# test_ui.py's own record (per-scenario seconds, budget_s, lane-group chains) -- read after the
# suite unit finishes. Written by every suite run, so it is only trusted when newer than the unit.
SUITE_RECORD = os.path.join(REPO, "tmp", "ui_test", "last_suite_timing.json")

# ---- THE ROUTINE (gate diet block 4, user-approved 2026-09-24) ---------------------------------
# A gate that only says PASS/FAIL lets its cost grow unseen: the suite went from "a couple of
# minutes" to 29 min with every run green. So cost is a verdict too. The gate goes RED when:
#   * a suite scenario runs longer than SCENARIO_OVER x its registry `budget_s`;
#   * the suite unit's wall (the gate's critical path since 2026-09) exceeds SUITE_WALL_MAX;
#   * the whole gate's wall exceeds GATE_WALL_MAX.
# Each red line names the offender and its growth against the last GREEN run. To raise a budget,
# edit the row's budget_s in tools/test_ui.py (and its long_why if > 120 s) with the measured
# number that justifies it -- the ui-testing skill's "Growing the regression suite" says how.
SCENARIO_OVER = 1.5
SUITE_WALL_MAX = 12 * 60
# 18 min since 2026-09-25 (user): wave 7 added the U19j 3-peer VM unit and x2a_map_variant plus two
# suite rows; the measured gate was 1168 s with a scheduling miss (selftests queued behind u19j_gpfg3).
GATE_WALL_MAX = 18 * 60
# What one --suite-net-jobs slot costs the core budget (block 3): two capped game processes that
# sleep most of every frame. Measured: avg 0.11 CPU-busy games per game, i.e. ~0.22 of a core per
# job; 1/3 keeps ~50 % headroom. It was 0.5 for the first diet gate (2026-09-24), which priced the
# suite at 5 of 7 cores: with det's 2 nothing else fit at t0, selftests (w3, 312 s) could only start
# when the suite ended at 556 s, and the gate wall was 867 s against the 900 s cap. At 1/3 the suite
# is 4, so selftests starts when det frees its 2 cores.
NET_JOB_WEIGHT = 1.0 / 3.0


def _load_json(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def _growth(now, then):
    if not then:
        return "no green base"
    return "last green %.0fs, %+.0f%%" % (then, (now - then) * 100.0 / then)


def diet_reds(unit_secs, gate_wall, suite_rec, green):
    """The routine's red lines (block 4b). unit_secs = {unit: seconds}; suite_rec = test_ui's
    record ({"per_test": {name: {"secs", "budget_s", "verdict"}}, ...}) or {}; green = the last
    green run's timings. Returns a list of strings, empty when every rule holds."""
    reds = []
    green = green or {}
    g_sc = green.get("_suite_scenarios", {})
    for name, r in sorted((suite_rec or {}).get("per_test", {}).items()):
        b = r.get("budget_s")
        secs = r.get("secs") or 0.0
        verdict = r.get("verdict")
        if verdict == "SKIP" or not b:
            continue
        # tooling:TL-SUITE-TIMEOUT-CLASS -- a timeout names ITS OWN class here, never the generic
        # "ran Xs > budget" wording: that phrasing reads as slowness, which is the one thing a
        # NEVER-STARTED or ASSERTION-NOT-RUN row did NOT do (TL-HARN19: a budget red and a
        # determinism red must never again be the same word in the gate's own output).
        if verdict in ("NEVER-STARTED", "WALK-STALLED", "ASSERTION-NOT-RUN"):
            reds.append(
                "scenario %s %s (killed at %.0fs against budget_s %ds) -- see the suite log, not a "
                "slow run" % (name, verdict, secs, b)
            )
            continue
        # tooling:TL-SUITE-LOADRED -- a red that only reproduces under suite contention gets its OWN
        # name too, the same reason the two verdicts above do: LOAD-RED (passed alone, not on
        # load_red_allow's allow-list) and RED-NOT-RERUN (over the suite's own rerun cap) must never
        # read as a bare cost overrun -- the suite's own solo-rerun summary is the evidence, not a
        # budget number. LOAD-RED-ALLOW is a real pass (falls through to the ordinary budget check).
        if verdict in ("LOAD-RED", "RED-NOT-RERUN"):
            reds.append(
                "scenario %s %s -- see the suite log's solo rerun summary, not a cost regression"
                % (name, verdict)
            )
            continue
        if secs > SCENARIO_OVER * b:
            reds.append(
                "scenario %s ran %.0fs > %.1fx its budget_s %ds (%s)"
                % (name, secs, SCENARIO_OVER, b, _growth(secs, g_sc.get(name)))
            )
    if unit_secs.get("suite", 0) > SUITE_WALL_MAX:
        reds.append(
            "suite wall %.0fs > %ds critical-path cap (%s)"
            % (unit_secs["suite"], SUITE_WALL_MAX, _growth(unit_secs["suite"], green.get("suite")))
        )
    if gate_wall > GATE_WALL_MAX:
        reds.append(
            "gate wall %.0fs > %ds (%s)"
            % (gate_wall, GATE_WALL_MAX, _growth(gate_wall, green.get("_gate_wall")))
        )
    return reds


def selftest():
    """The routine's negative cases (`run_gate.py --selftest`, a lint_repo row)."""
    ok = True
    green = {"suite": 500.0, "_gate_wall": 800.0, "_suite_scenarios": {"a": 40.0}}
    within = {"per_test": {"a": {"secs": 50, "budget_s": 60}}}
    over = {"per_test": {"a": {"secs": 97, "budget_s": 60}}}
    skipped = {"per_test": {"a": {"secs": 999, "budget_s": 1, "verdict": "SKIP"}}}
    # tooling:TL-SUITE-TIMEOUT-CLASS -- a NEVER-STARTED/ASSERTION-NOT-RUN row must still red (it
    # never passed), but its red must not use the "ran Xs > budget" wording that a real cost-growth
    # row (`over`, above) gets -- see the wording assertion below the main case loop.
    never_started = {"per_test": {"a": {"secs": 420, "budget_s": 66, "verdict": "NEVER-STARTED"}}}
    assertion_not_run = {
        "per_test": {"a": {"secs": 420, "budget_s": 66, "verdict": "ASSERTION-NOT-RUN"}}
    }
    # tooling:TL-SUITE-LOADRED -- LOAD-RED/RED-NOT-RERUN must red (same reason as the two above:
    # a real red is a real red), and LOAD-RED-ALLOW must NOT (the allow-list is what makes it a pass).
    load_red = {"per_test": {"a": {"secs": 55, "budget_s": 60, "verdict": "LOAD-RED"}}}
    red_not_rerun = {"per_test": {"a": {"secs": 55, "budget_s": 60, "verdict": "RED-NOT-RERUN"}}}
    load_red_allowed = {
        "per_test": {"a": {"secs": 55, "budget_s": 60, "verdict": "LOAD-RED-ALLOW"}}
    }
    cases = [
        ("all within", {"suite": 600}, 850, within, 0),
        ("scenario 1.6x its budget", {"suite": 600}, 850, over, 1),
        ("a SKIP is not timed", {}, 10, skipped, 0),
        ("suite over 12 min", {"suite": 721}, 850, {}, 1),
        ("gate over 18 min", {"suite": 600}, 1081, {}, 1),
        ("all three", {"suite": 800}, 1100, over, 3),
        ("a NEVER-STARTED scenario still reds", {"suite": 600}, 850, never_started, 1),
        ("an ASSERTION-NOT-RUN scenario still reds", {"suite": 600}, 850, assertion_not_run, 1),
        ("a LOAD-RED scenario still reds", {"suite": 600}, 850, load_red, 1),
        ("a RED-NOT-RERUN scenario still reds", {"suite": 600}, 850, red_not_rerun, 1),
        ("a LOAD-RED-ALLOW scenario does NOT red", {"suite": 600}, 850, load_red_allowed, 0),
    ]
    for label, units_, wall, rec, want in cases:
        got = diet_reds(units_, wall, rec, green)
        hit = len(got) == want
        ok = ok and hit
        print("  %-26s %s  %s" % (label, "ok" if hit else "XX", "; ".join(got)[:150]))
    growth = diet_reds({}, 0, over, green)
    hit = bool(growth) and "last green 40s" in growth[0]
    print("  %-26s %s" % ("a red names its growth", "ok" if hit else "XX"))
    ok = ok and hit
    timeout_wording = diet_reds({}, 0, never_started, green)
    hit = (
        bool(timeout_wording)
        and "NEVER-STARTED" in timeout_wording[0]
        and " ran " not in timeout_wording[0]
    )
    print("  %-26s %s" % ("a timeout names its class, not FAIL", "ok" if hit else "XX"))
    ok = ok and hit
    loadred_wording = diet_reds({}, 0, load_red, green)
    hit = (
        bool(loadred_wording)
        and "LOAD-RED" in loadred_wording[0]
        and " ran " not in loadred_wording[0]
    )
    print("  %-26s %s" % ("a LOAD-RED names its class, not a cost line", "ok" if hit else "XX"))
    ok = ok and hit
    print("run_gate selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def load_durations(rows):
    rec = {}
    try:
        rec = json.load(open(TIMINGS, encoding="utf-8"))
    except (OSError, ValueError):
        pass
    for r in rows:
        v = rec.get(r["name"], DUR_HINTS.get(r["name"], 300))
        r["dur"] = float(v) if isinstance(v, (int, float)) else 300.0
    return rows


def kill_tree(pid):
    """taskkill /T: a unit is a runner that spawns games; killing only the runner leaks them."""
    subprocess.run(
        ["taskkill", "/F", "/T", "/PID", str(pid)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def build():
    """Step 1, serial: everything else executes this build's output. Mirrors the README recipe
    (/m /nodeReuse:false -- node reuse leaves workers holding our stdout handle for ~15 min)."""
    msbuild = os.path.join(machine.VS_INSTALL_ROOT, "MSBuild", "Current", "Bin", "MSBuild.exe")
    if not os.path.isfile(msbuild):
        print("FAIL: no MSBuild at %s (machine_config VS_INSTALL_ROOT)" % msbuild)
        return False
    log = os.path.join(LOG_DIR, "build.log")
    t0 = time.time()
    print("[gate] build ...")
    with open(log, "w", encoding="utf-8") as fh:
        rc = subprocess.run(
            [
                msbuild,
                os.path.join(REPO, "src", "mh_dll", "mh.sln"),
                "/t:Build",
                "/p:Configuration=Release",
                "/p:Platform=x86",
                "/m",
                "/nodeReuse:false",
                "/v:minimal",
                "/nologo",
            ],
            stdout=fh,
            stderr=subprocess.STDOUT,
        ).returncode
    secs = time.time() - t0
    if rc != 0:
        print("[gate] build FAILED (%.0fs) -- aborting; log: %s" % (secs, log))
        for ln in open(log, encoding="utf-8", errors="replace").readlines()[-15:]:
            print("    " + ln.rstrip())
        return False
    print("[gate] build ok (%.0fs)" % secs)
    # THE THREE CONFIGURATIONS, NAMED (fork F4G). One msbuild produces all of them -- there is no
    # separate per-configuration build and there must not be, because configurations (1) and (2)
    # differ only in which files a deployment has beside mh.dll (ruling Q10) and (3) is its own
    # directory. What a single build DOES hide is an artifact that silently stopped being produced,
    # so the set is asserted here by name rather than inferred from a green build line.
    #
    # Read the three columns as the deployments they are:
    #   (1) all-original + net restoration: mh.dll + mh_net.dll [+ mh_harness.dll], NO libmh.dll
    #   (2) brokered:                       the same files, WITH Release\libmh.dll beside them
    #   (3) standalone reference:           Release\standalone\ -- libmh.dll + libref_host.exe,
    #                                       and no game binary anywhere
    rel = os.path.join(REPO, "src", "mh_dll", "Release")
    artifacts = [
        (
            "mh.dll",
            os.path.join(rel, "mh.dll"),
            "configs (1),(2),(3 n/a): the router and patch host",
        ),
        ("mh_net.dll", os.path.join(rel, "mh_net.dll"), "configs (1),(2): the transport"),
        ("mh_harness.dll", os.path.join(rel, "mh_harness.dll"), "configs (1),(2): the instrument"),
        ("libmh.dll", os.path.join(rel, "libmh.dll"), "config (2): the HOSTED spine"),
        (
            "standalone/libmh.dll",
            os.path.join(rel, "standalone", "libmh.dll"),
            "config (3): the STANDALONE spine",
        ),
        (
            "standalone/libref_host.exe",
            os.path.join(rel, "standalone", "libref_host.exe"),
            "config (3): the host",
        ),
        (
            "net_selftest.exe",
            os.path.join(rel, "net_selftest.exe"),
            "the offline suites, HOSTED arm",
        ),
        (
            "libmh_selftest.exe",
            os.path.join(rel, "libmh_selftest.exe"),
            "the offline suites, STANDALONE arm (F5I)",
        ),
    ]
    missing = [n for n, p, _w in artifacts if not os.path.isfile(p)]
    if missing:
        print("[gate] BUILD PRODUCED NO %s -- aborting" % ", ".join(missing))
        return False
    for n, p, why in artifacts:
        print("[gate]   %-28s %8d B   %s" % (n, os.path.getsize(p), why))
    # THE SUBSET RULE, checked HERE because here is the only place in the repo that is guaranteed to
    # be holding fresh Release binaries (fork F4A's rule, F4B's first subject -- docs/dll-split.md).
    # mh.dll LoadLibrary()s its satellites from inside DLL_PROCESS_ATTACH, and that is only safe
    # while every satellite's static imports are a SUBSET of mh.dll's own: otherwise the loader
    # initialises a fresh DLL under a lock we hold. It is not a lint row because lint has no build.
    #
    # It guards a one-instruction htons() anchor in module_bind.cpp that looks exactly like dead
    # code: mh.dll imported WS2_32 only because the transport used to be compiled into it, and
    # without the anchor the rule breaks on the very satellite it was written for.
    rc = subprocess.run(
        [
            sys.executable,
            os.path.join(REPO, "tools", "check_module_bind.py"),
            "--subset",
        ],
        capture_output=True,
        text=True,
    )
    if rc.returncode != 0:
        print("[gate] SUBSET RULE FAILED -- aborting")
        for ln in (rc.stdout + rc.stderr).strip().splitlines()[-12:]:
            print("    " + ln)
        return False
    print("[gate] subset rule ok")
    # THE EXPORT CONTRACT'S OBJECT HALF (fork F4D), here for exactly the reason above: it reads the
    # two images' OBJECTS, so it needs a build and cannot be a lint row. lint already checks that the
    # three emitted files re-render from the committed list; this checks the committed list against
    # what the tree actually says -- a symbol mh.dll started needing, or one libmh stopped exporting.
    #
    # The failure it stops is not subtle at runtime and is invisible at build time: a row missing
    # from libmh.def resolves to nothing at bind, the bind REFUSES the whole module by name, and the
    # game silently runs configuration (1). Everything still boots.
    rc = subprocess.run(
        [
            sys.executable,
            os.path.join(REPO, "tools", "gen_libmh_contract.py"),
            "--rederive",
            "--check",
        ],
        capture_output=True,
        text=True,
    )
    if rc.returncode != 0:
        print("[gate] LIBMH EXPORT CONTRACT STALE -- aborting")
        for ln in (rc.stdout + rc.stderr).strip().splitlines()[-14:]:
            print("    " + ln)
        return False
    print("[gate] libmh export contract ok")
    # THE HARNESS CONTRACTS' OBJECT HALF (fork F4E), here for the same reason: it intersects
    # mh_harness.dll's undefined externals with what mh.dll and libmh.dll define, so it needs the
    # Debug objects and cannot be a lint row. The failure it stops is the mirror of the one above,
    # one image over: a host row mh.dll stopped exporting makes the instrument REFUSE TO ARM, the
    # boot continues, and the only sign is that mh_harness.log was never written.
    rc = subprocess.run(
        [
            sys.executable,
            os.path.join(REPO, "tools", "gen_harness_contract.py"),
            "--rederive",
            "--check",
        ],
        capture_output=True,
        text=True,
    )
    if rc.returncode != 0:
        print("[gate] HARNESS CONTRACT STALE -- aborting")
        for ln in (rc.stdout + rc.stderr).strip().splitlines()[-14:]:
            print("    " + ln)
        return False
    print("[gate] harness contract ok")
    # THE STANDALONE ARM'S EXPORT CONTRACT (fork F4G), here for the third time for the same reason:
    # it intersects libref_host's objects with the libmh ARCHIVE's, so it needs a build. What it
    # stops is a DIFFERENT failure from the two above, and a milder one -- the standalone host
    # IMPORTS its spine statically, so a missing row is a LINK error naming the symbol rather than a
    # silent degradation. This row is what says "the committed list drifted" instead of leaving the
    # next reader to interpret a linker diagnostic.
    rc = subprocess.run(
        [
            sys.executable,
            os.path.join(REPO, "tools", "gen_libmh_std_contract.py"),
            "--rederive",
            "--check",
        ],
        capture_output=True,
        text=True,
    )
    if rc.returncode != 0:
        print("[gate] STANDALONE LIBMH EXPORT CONTRACT STALE -- aborting")
        for ln in (rc.stdout + rc.stderr).strip().splitlines()[-14:]:
            print("    " + ln)
        return False
    print("[gate] standalone libmh export contract ok")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--cores",
        type=int,
        default=max(1, (os.cpu_count() or 8) - 1),
        help="local core budget for the parallel units (default: all logical CPUs minus one "
        "-- %(default)d here; the user's 2026-09-10 call for the dev box)",
    )
    ap.add_argument(
        "--steps", type=int, default=3000, help="determinism step floor (default %(default)d)"
    )
    ap.add_argument(
        "--suite-jobs",
        type=int,
        default=2,
        help="--jobs handed to the UI suite (default %(default)d -- the suite's own scaled "
        "default on this box; it shares the budget with every other unit)",
    )
    ap.add_argument(
        "--suite-net-jobs",
        type=int,
        default=6,
        help="--net-jobs handed to the UI suite: the WAIT-BOUND multi-peer pool (default "
        "%(default)d; priced at NET_JOB_WEIGHT cores each -- gate diet block 3)",
    )
    ap.add_argument("--selftest", action="store_true", help="the cost rules' negative cases")
    ap.add_argument(
        "--skip",
        default="",
        help="comma-separated unit names to skip (e.g. 'det,ab' on a box without the rig)",
    )
    ap.add_argument("--no-build", action="store_true", help="skip step 1 (a just-built tree)")
    ap.add_argument("--list", action="store_true", help="print the unit roster and exit")
    args = ap.parse_args()
    args.skip = {s.strip() for s in args.skip.split(",") if s.strip()}
    if args.selftest:
        return selftest()

    rows = units(args)
    if args.list:
        for r in rows:
            print("  %-12s w=%d  t<=%ds  %s" % (r["name"], r["weight"], r["timeout"], r["why"]))
        return 0

    os.makedirs(LOG_DIR, exist_ok=True)
    t0 = time.time()
    if not args.no_build and not build():
        return 1

    rows = load_durations(rows)
    results = {}
    print(
        "[gate] running %d unit(s) under a %d-core budget, longest-first: %s"
        % (
            len(rows),
            args.cores,
            ", ".join(
                "%s(%ds)" % (r["name"], r["dur"]) for r in sorted(rows, key=lambda r: -r["dur"])
            ),
        )
    )

    # LONGEST-FIRST WITH BACKFILL (user, 2026-09-10 -- the first run started units in roster
    # order and finished with one unit holding 2 of 7 cores while 5 idled). The dispatcher scans
    # PENDING in descending expected duration and starts every unit that fits the free budget;
    # when the longest does not fit, a shorter one backfills. Durations come from the last run's
    # own record, so the order corrects itself as the units' costs drift.
    sched = threading.Condition()
    free = [args.cores]
    # THE VM SLOT (gate diet block 5b, mp:U19j/X2a into the gate): a SECOND, disjoint budget beside
    # `free`. `free` prices THIS box's cores; it says nothing about the two rig VMs, and until now
    # that was safe because exactly one roster row ever touched them (`det`; the roster's other
    # configuration-(1) row, `det_c1`, ran on LOCAL lanes only and was folded into match_launch_net's
    # own post_check, tooling:TL-SUITE-FOLD-DETC1) so the roster never had two VM-driving units in
    # flight together. mp:U19j's rig proof and mp:X2a's
    # both need the REAL vms[0]/vms[1] pair (independent installs / a 3rd peer past a drop -- neither
    # is expressible on a local lane, see their rows), so the roster now has three. Two rig tools
    # deploying to the SAME VM directory at once (remote_launch's fixed `args.vm_dir`, one scheduled
    # task name) is not a load number -- it is file-copy and schtasks corruption -- so this is a
    # second free-slot count (capacity 1: the VMs are ONE shared pair), not a bigger weight. A row
    # opts in with `"vm": True`; everything else is unaffected (free_vm starts at 1 and no non-VM
    # row ever touches it).
    free_vm = [1]
    # `first` before longest-first (fork F4H): a unit whose peers are not on this box's scheduler
    # cannot be ordered by its local core cost, and det is the one that has to hold its vCPUs while
    # the box is least busy -- see its row for why. Everything else keeps the longest-first rule.
    pending = sorted(rows, key=lambda r: (not r.get("first"), -r["dur"]))
    running = set()
    spans = {}  # unit -> (start, end) seconds from gate t0, for gate_timeline's critical path

    def run_unit(r, w):
        name = r["name"]
        log = os.path.join(LOG_DIR, name + ".log")
        t1 = time.time()
        print("[gate] %-12s START  (w=%d, log %s)" % (name, w, os.path.relpath(log, REPO)))
        try:
            with open(log, "w", encoding="utf-8") as fh:
                proc = subprocess.Popen(r["cmd"], stdout=fh, stderr=subprocess.STDOUT, cwd=REPO)
                try:
                    rc = proc.wait(timeout=r["timeout"])
                    verdict = "PASS" if rc == 0 else "FAIL"
                except subprocess.TimeoutExpired:
                    kill_tree(proc.pid)
                    proc.wait()
                    verdict = "TIMEOUT"
        except OSError as e:
            verdict = "FAIL"
            print("[gate] %-12s could not launch: %s" % (name, e))
        secs = time.time() - t1
        results[name] = (verdict, secs, log)
        spans[name] = (t1 - t0, time.time() - t0)
        print("[gate] %-12s %s  (%.0fs)" % (name, verdict, secs))
        if verdict != "PASS":
            print("  ---- tail of %s ----" % os.path.relpath(log, REPO))
            try:
                for ln in open(log, encoding="utf-8", errors="replace").readlines()[-20:]:
                    print("    " + ln.rstrip())
            except OSError:
                pass
        with sched:
            free[0] += w
            if r.get("vm"):
                free_vm[0] += 1
            running.discard(name)
            sched.notify_all()

    holder = "run_gate:%d" % os.getpid()
    cur = hostlock.held_by("rig")
    if cur:
        print("[gate] waiting for the rig lease -- held by %r ..." % cur.get("holder"))
    with hostlock.lease("rig", holder):
        # The pass-through env run_rig_tool's children key on: with the live holder's pid
        # exported, every child test_ui/mp_run runs inside OUR lease instead of queueing on it.
        os.environ[hostlock.RIG_LEASE_ENV] = str(os.getpid())
        try:
            with sched:
                while pending or running:
                    started = True
                    while started:
                        started = False
                        for r in list(pending):
                            w = min(r["weight"], args.cores)
                            if r.get("vm") and free_vm[0] < 1:
                                continue  # the VM slot is taken -- wait, never skip to a backfill
                            if w <= free[0]:
                                free[0] -= w
                                if r.get("vm"):
                                    free_vm[0] -= 1
                                pending.remove(r)
                                running.add(r["name"])
                                threading.Thread(target=run_unit, args=(r, w), daemon=True).start()
                                started = True
                    if pending or running:
                        sched.wait()
        finally:
            os.environ.pop(hostlock.RIG_LEASE_ENV, None)

    gate_wall = time.time() - t0
    suite_rec = {}
    if "suite" in spans:
        try:
            if os.path.getmtime(SUITE_RECORD) >= t0 + spans["suite"][0]:
                suite_rec = _load_json(SUITE_RECORD)
        except OSError:
            pass
    green = _load_json(GREEN_TIMINGS)
    reds = diet_reds({n: v[1] for n, v in results.items()}, gate_wall, suite_rec, green)
    all_pass = len(results) == len(rows) and all(v[0] == "PASS" for v in results.values())

    # The measured record the NEXT run schedules by (unit keys: every unit that ran to a verdict --
    # a red SUITE still ran its full length, and dropping it made the next run schedule the gate's
    # longest unit by a 280 s hint behind shorter ones; a TIMEOUT is left out), plus the routine's own
    # fields (underscore keys): per-scenario suite seconds, unit spans, the gate wall and verdict.
    rec = {n: round(v[1], 1) for n, v in results.items() if v[0] in ("PASS", "FAIL")}
    rec["_gate_wall"] = round(gate_wall, 1)
    rec["_units"] = {n: [round(a, 1), round(b, 1), results[n][0]] for n, (a, b) in spans.items()}
    rec["_suite_scenarios"] = {
        n: r.get("secs") for n, r in (suite_rec.get("per_test") or {}).items()
    }
    # tooling:TL-SUITE-TIMEOUT-CLASS -- the per-scenario CLASS (PASS/SLOW/FAIL/NEVER-STARTED/
    # ASSERTION-NOT-RUN), kept as its own key rather than folded into `_suite_scenarios` above so
    # every existing reader of that {name: seconds} shape (this file's own `_growth`, gate_timeline)
    # is untouched.
    rec["_suite_classes"] = {
        n: r.get("verdict") for n, r in (suite_rec.get("per_test") or {}).items()
    }
    rec["_suite_chains"] = suite_rec.get("chains") or []
    rec["_reds"] = reds
    rec["_verdict"] = "PASS" if all_pass and not reds else "FAIL"
    try:
        with open(TIMINGS, "w", encoding="utf-8") as fh:
            json.dump(rec, fh, indent=1)
        if rec["_verdict"] == "PASS":
            shutil.copyfile(TIMINGS, GREEN_TIMINGS)
    except OSError:
        pass

    print("\n" + "=" * 78)
    print("THE GATE")
    print("-" * 78)
    bad = 0
    for r in rows:
        verdict, secs, log = results.get(r["name"], ("NOT-RUN", 0.0, ""))
        bad += verdict != "PASS"
        print("  %-12s %-8s %6.0fs   %s" % (r["name"], verdict, secs, os.path.relpath(log, REPO)))
    print("-" * 78)
    for r in reds:
        print("  RED (cost): %s" % r)
    print(
        "  %s in %.1f min wall (%d-core budget; serial estimate is the sum of the seconds above)"
        % (
            "PASS"
            if not bad and not reds
            else "%d unit(s) NOT PASSED, %d cost red(s)" % (bad, len(reds)),
            gate_wall / 60,
            args.cores,
        )
    )
    return 1 if bad or reds else 0


if __name__ == "__main__":
    raise SystemExit(main())
