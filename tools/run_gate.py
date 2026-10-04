#!/usr/bin/env python3
"""run_gate.py -- the DLL gate (src/mh_dll/README.md "The gate") as ONE command, overlapped.

    python tools/run_gate.py                # the whole gate, ~7 cores of local work + the VMs
    python tools/run_gate.py --skip det,spdet  # e.g. on a box without the rig peers
    python tools/run_gate.py --list         # BOTH profiles' rosters and what each unit runs
    python tools/run_gate.py --light        # the LIGHT gate (== --profile light): iteration checkpoints
    python tools/run_gate.py --light --plan # what --light would run on this diff, and why; runs nothing
    python tools/run_gate.py --full-status  # distance from the last FULL green; exit 3 when one is due

TWO PROFILES (tooling:TL-GATE12, user 2026-09-29). `full` (the default) is the whole roster below and
is the gate for big checkpoints, merges of a long-lived branch, and releases. `light` is the
iteration checkpoint: build (plain selftest exes only), both determinism oracles (det + spdet), the
two recorded-session replays (abc_tutorial + abc_spcamp, SHIP ARM ONLY against the recording
oracle: --ui-abc-arms A,C), inmem, lint, the plain selftest pass and a
two-row UI smoke (menu_walk + match_launch_net) -- PLUS whatever the CHANGE-AWARE selection adds for
the files changed since the last FULL green (see LIGHT_RULES): C++ under libmh/ or mh_common/ forces
the ASan pass back in (dead-ends G32 -- the heap-corruption class lives there), gfx/input/ui/net
sources add the UI rows that exercise them, an edited uiscripts file adds the rows that use it. The
light gate never records a full green; it prints how far HEAD is from the last one and says FULL GATE
DUE past FULL_DUE_COMMITS / FULL_DUE_DAYS; the worktree loop's `land` step refuses a due gate.

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
import datetime
import json
import math
import os
import re
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
            ]
            # RELEASE TIER (user 2026-10-01, option 1): rows marked `tier: release` in the UI
            # registry are skipped by a bare suite run; --release puts them back.
            + (["--release-tier"] if getattr(args, "release", False) else []),
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
            # --no-build: step 1 compiled + staged both modes (`--build-only`) before any game
            # started, so this unit only RUNS suites. It used to compile the ASan and plain
            # selftest exes here, overlapped with the suite's live games -- cl.exe competing with
            # mh.exe for the same cores (user, 2026-09-27: build first, then play).
            # Under run_gate --no-build there was no step 1, so the unit builds its own exes as before.
            "cmd": [PY, os.path.join(REPO, "tools", "run_selftests.py")]
            + ([] if args.no_build else ["--no-build"]),
            "weight": 3 if args.no_build else 1,  # without step 1 the ASan msbuild is back in here
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
    if getattr(args, "release", False):
        # mp:U53 (2026-10-02): the 3-peer elimination arms -- host defeated (survivors play on
        # IDENTICAL) and client defeated (mp:U56's pinned roster flip). RELEASE ONLY: ~4 min per arm
        # on the 3-peer topology (VM host, VM client, a local DET3 lane), and the full gate's wall cap
        # has no room for it; same topology class as u19j_gpfg3, so the same weight.
        rows.append(
            {
                "name": "u53_elim",
                "why": "release -- mp:U53/U56 3-peer eliminations + mp:U58 3-peer GS2: host defeated, client "
                "defeated, survivors IDENTICAL to the end",
                "cmd": [
                    PY,
                    os.path.join(REPO, "tools", "det_arms.py"),
                    "--determinism",
                    "--u53-host-elim",
                    "--u53-client-elim",
                    "--u58-gs2-3peer",  # mp:U58: GS2 watches slot 2 from BOTH survivors
                ],
                "weight": 3,
                "vm": True,
                "timeout": 1200,
            }
        )
        rows.extend(host_migration_rows())
    return [r for r in rows if r["name"] not in args.skip]


def host_migration_rows():
    """mp:U65 (HM-M8): the host-migration arms (plan section 11: handover, crash failover, the 4-peer failure cases), RELEASE TIER only.
    Every row is a 3- or 4-peer match on the rig (VM host + VM client + local DET lanes), so they are all
    `vm: True` and run one after another. `budget_s` is the MEASURED wall (2026-10-03, one clean run each, the
    VMs warm) rounded up; `diet_reds` goes red at 1.5x it. Each arm costs ~1-3 min, mostly match boot + the
    hashed steps + the post-failover run-out, so none can be shortened without losing the
    ">= 1000 steps IDENTICAL after the incident" clause they assert."""
    det = [PY, os.path.join(REPO, "tools", "det_arms.py"), "--determinism"]
    one_arm = "one 3-peer arm: match boot + 4000 steps + failover"
    # (name, why, extra flags, weight, budget_s, long_why)
    spec = [
        (
            "hm_handover",
            "mp:U62 planned handover: ESC-quit and graceful exit hand the hub over (survivors IDENTICAL, stall < 1 s) "
            "+ the hub_migration=0 negative arm (U55's outcome 8)",
            ["--u62-host-quit", "--u62-host-exit", "--u62-neg"],
            3,
            600,
            "three 3-peer arms back to back (match boot + 4000 steps each; the negative arm waits out U55's 58 s end)",
        ),
        (
            "hm_crash",
            "mp:U63 crash failover, direct, reimpl: TerminateProcess of the hub, survivors elect and play on IDENTICAL, "
            "stall <= 6 s + the negative arm",
            ["--u63-crash", "--u63-neg"],
            3,
            400,
            "a 3-peer crash arm plus the negative arm that waits out U55's 58 s end",
        ),
        (
            "hm_crash_relay",
            "mp:U63 crash failover through a local mh_relay (relay-room fallback), reimpl",
            ["--u63-crash", "--u55-relay"],
            3,
            200,
            one_arm,
        ),
        (
            "hm_crash_c1",
            "mp:U63 crash failover in configuration (1) (mode=original): the retail emitters carry the failover",
            ["--u63-crash", "--u53-config1"],
            3,
            200,
            one_arm,
        ),
        (
            "hm_crash_relay_c1",
            "mp:U63 crash failover, relay AND configuration (1)",
            ["--u63-crash", "--u55-relay", "--u53-config1"],
            3,
            200,
            one_arm,
        ),
        (
            "hm_u64_elect",
            "mp:U64 4-peer: min-RTT election (elect4: client 2 elected, ranking 2>3>1), new hub lost (newhub: second "
            "failover) and the double loss that ends both survivors (double: even split -> MINORITY)",
            ["--u64", "elect4", "--u64", "newhub", "--u64", "double"],
            4,
            600,
            "three 4-peer arms (2400-3000 hashed steps each, two failovers in newhub)",
        ),
        (
            "hm_u64_part",
            "mp:U64 4-peer partition (the cut-off hub ends outcome 8, three survivors play on) and the heal (the old "
            "host's traffic is dropped, no second election)",
            ["--u64", "partition", "--u64", "heal"],
            4,
            250,
            "two 4-peer arms (partition ~100 s, heal ~125 s)",
        ),
    ]
    return [
        {
            "name": name,
            "why": "release -- " + why,
            "cmd": det + flags,
            "weight": weight,
            "vm": True,
            "timeout": max(900, budget * 2),
            "budget_s": budget,
            "long_why": long_why,
        }
        for name, why, flags, weight, budget, long_why in spec
    ]


# Expected wall seconds per unit when no measured record exists yet -- ONLY a scheduling hint
# (longest-first), never a budget. The real record is tmp/gate/last_timings.json, written by
# every finished run, so the schedule self-corrects from its own history (the same lesson the
# suite's last_suite_timing.json exists for: a figure in prose is a figure nobody re-measures).
DUR_HINTS = {
    "suite": 280,
    "abc_spcamp": 200,
    "selftests": 180,
    # ASan + plain with the six loopback suites skipped: ESTIMATED 2026-10-03 from the 529 s record
    # (loopback = 162 s plain + ~162 s ASan, wall-bound) -- self-corrects at the first real run.
    "selftests_noloop": 210,
    "det": 150,
    # Measured STANDALONE (wave 7 lane D, not yet inside a real gate run -- this self-corrects from
    # tmp/gate/last_timings.json the first time the gate actually runs them): u19j_gpfg3's guarded
    # arm ran inside a combined 388 s run whose unguarded half hit a crashy VM (G315); a clean
    # unguarded-alone retry took 294 s, so ~480 s covers both arms with margin. x2a_map_variant ran
    # 71 s alone (host+client VM match plus the scp backup/push/restore).
    "u19j_gpfg3": 480,
    "x2a_map_variant": 90,
    # mp:U53 release unit: two 3-peer arms, ~3-4 min each measured standalone (2026-10-02).
    "u53_elim": 600,  # +mp:U58 gs2 arm (~2 min)
    # mp:U65 host-migration release units: measured 2026-10-03 (one clean run each)
    "hm_handover": 545,
    "hm_crash": 364,
    "hm_crash_relay": 185,
    "hm_crash_c1": 183,
    "hm_crash_relay_c1": 186,
    "hm_u64_elect": 546,
    "hm_u64_part": 228,
    "abc_tutorial": 120,
    "spdet": 100,
    "libref": 75,
    "lint": 60,
    "inmem": 10,
    # the light profile's own two rows (TL-GATE12): a warm plain selftest pass is ~26 s of suites,
    # the two smoke rows ~60 s of lane time (menu_walk 14 s + match_launch_net 45 s, 2026-09-28).
    "selftests_plain": 60,
    # light, transport change: ASan without the loopback suites + plain with them (option A,
    # 2026-10-03): ~594 s full minus ~160 s of loopback ASan -- self-corrects at the first run.
    "selftests_loop_plain": 430,
    "smoke": 90,
    # the SMOKE profile's own rows (TL-LIGHT-WALL-CAP): mapped plain suites, one repeat each; one UI row.
    "selftests_smoke": 45,
    "ui_one": 45,
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
# 21 min since 2026-09-29 (user): the measured suite is ~21 min (1249-1388 s in the last two full
# gates) and the light profile now carries per-session iteration, so the full gate only has to stay
# BOUNDED, not fast. Lowering it again is tooling:TL-GATE-COST-0927.
# 23.5 min since 2026-10-01 (user): the player-feedback wave added 14 UI rows; 8 moved to the release
# tier (TL-GATE-RELEASE-TIER) and the full gate's suite then measured 1371 s (gate wall 23.7 min, under
# GATE_WALL_MAX). The kept fix-on rows (dialog_stall, d45_sp_then_host, ...) guard just-shipped fixes.
SUITE_WALL_MAX = int(23.5 * 60)
# 18 min since 2026-09-25 (user): wave 7 added the U19j 3-peer VM unit and x2a_map_variant plus two
# suite rows; the measured gate was 1168 s with a scheduling miss (selftests queued behind u19j_gpfg3).
# 25 min since 2026-09-29: the suite cap above plus the ~4 min the other units run after it (the
# last two full gates: 1297 s, 1508 s wall).
GATE_WALL_MAX = 25 * 60
# 75 min for `--release` (mp:U65): the host-migration units add ~37 min of strictly serial VM rig time (one rig, one
# `vm` slot) to the full gate, so the release profile is held to its own cap; each unit still answers to its budget_s.
RELEASE_GATE_WALL_MAX = 75 * 60
# What one --suite-net-jobs slot costs the core budget (block 3): two capped game processes that
# sleep most of every frame. Measured: avg 0.11 CPU-busy games per game, i.e. ~0.22 of a core per
# job; 1/3 keeps ~50 % headroom. It was 0.5 for the first diet gate (2026-09-24), which priced the
# suite at 5 of 7 cores: with det's 2 nothing else fit at t0, selftests (w3, 312 s) could only start
# when the suite ended at 556 s, and the gate wall was 867 s against the 900 s cap. At 1/3 the suite
# is 4, so selftests starts when det frees its 2 cores.
NET_JOB_WEIGHT = 1.0 / 3.0

# ---- THE LIGHT PROFILE (tooling:TL-GATE12, user 2026-09-29) ------------------------------------
# "We run selftests with ASan and the full UI suite every time, but we barely change the code they
# test." The light gate keeps the oracles that catch what an iteration usually breaks (determinism,
# the recorded play sessions, the patch parity, lint) and buys back the rest per CHANGED FILE rather
# than per run. Its own wall cap, its own timing record, and it never writes the full green record.
LIGHT_GATE_WALL_MAX = 9 * 60
LIGHT_TIMINGS = os.path.join(LOG_DIR, "last_light_timings.json")
LIGHT_GREEN_TIMINGS = os.path.join(LOG_DIR, "last_light_green_timings.json")
# ---- THE SMOKE PROFILE (tooling:TL-LIGHT-WALL-CAP, user 2026-10-03) ---------------------------------
# For ITERATING on code, between commits: build, the plain selftest suites of the domains the change
# touches, lint, and at most ONE change-aware UI row. No ASan, no VMs, no replays, no base UI rows.
# NOT a commit gate -- the commit gate is --light; merges/releases are the full gate / --release.
# Its own cap and its own records, so it neither reds on nor pollutes the light/full history.
SMOKE_GATE_WALL_MAX = 4 * 60
SMOKE_TIMINGS = os.path.join(LOG_DIR, "last_smoke_timings.json")
SMOKE_GREEN_TIMINGS = os.path.join(LOG_DIR, "last_smoke_green_timings.json")
SMOKE_NOT_A_GATE = (
    "SMOKE IS NOT A COMMIT GATE -- no ASan, no determinism, no replays, no VM units. It answers "
    "'did my last edit break the domain I touched'. Commit on `--light`; merge/release on the full gate."
)
# The one record the light gate reads and never writes: the commit the last FULL, un-skipped, green
# gate ran on. Its distance from HEAD is the escalation signal.
FULL_GREEN = os.path.join(LOG_DIR, "last_full_green.json")
# ~30 commits/day on master in late 2026-09 (git rev-list --since), so 60 commits is ~2 working days.
FULL_DUE_COMMITS = 60
FULL_DUE_DAYS = 3
# With no full green on record there is no honest diff base; the light gate then diffs the last
# FALLBACK_DEPTH commits and says FULL GATE DUE in its banner.
FALLBACK_DEPTH = 10
# Always in the light gate: one solo menu walk and one 2-peer MP launch through the real
# menu->lobby->Start (configuration (1) on the shipped ini).
SMOKE_ROWS = ("menu_walk", "match_launch_net")
LIGHT_BASE_UNITS = ("det", "spdet", "abc_tutorial", "abc_spcamp", "inmem", "lint")
# The two recorded-session units, which the light profile runs as `--ui-abc-arms A,C` (light_units).
LIGHT_ABC_UNITS = ("abc_tutorial", "abc_spcamp")
# The selftest unit's own long pole, MEASURED 2026-09-29 (plain pass, one run each): these six are
# WALL-bound loopback transport suites -- udpsnaptest 49 s, udploopbacktest 43, udpbulktest 34,
# udprelaytest 20, relinktest 8, udprelinktest 8 = 162 of the plain pass's 214 s, paid again under
# ASan. The light gate runs them only when a transport source changed (the `loopback` token).
LOOPBACK_SUITES = (
    "udpsnaptest",
    "udploopbacktest",
    "meshtest",
    "udpbulktest",
    "udprelaytest",
    "relinktest",
    "udprelinktest",
)

_GFX_ROWS = ("win_resize", "res_hud", "font_guard", "boot_snapshot", "gx1_overlay_residue")
_INPUT_ROWS = ("type_ascii", "key_repeat", "esc_menu", "imgui_swallow", "imgui_hover")
_UI_ROWS = ("esc_menu", "stats_panel", "debug_overlay", "tact_panel", "ip_cancel", "no_net_lobby")
_NET_ROWS = ("client_join", "relay_match", "graceful_quit", "link_death", "net_hud")
_MAP_ROWS = ("map_have", "map_absent")
_CXX = r"\.(c|cc|cpp|cxx|h|hpp|inl)$"
# (label, path regex, extra units, extra UI rows). First-class data so `--plan` can say WHY each
# addition happened and --selftest can prove every named row still exists in the registry.
# `asan` in the units column means "run_selftests.py with the ASan pass" (the full `selftests` unit
# replaces the light `selftests_plain`).
LIGHT_RULES = (
    (
        "spine/common C++ (G32: heap corruption)",
        r"^src/mh_dll/(libmh|libmh_dll|mh_common|mh_nettest|libmh_test)/.*" + _CXX,
        ("asan",),
        (),
    ),
    (
        "standalone spine / libref host",
        r"^src/mh_dll/(libmh|libmh_dll|libmh_std|libref_host)/",
        ("libref",),
        (),
    ),
    (
        "build configuration",
        r"^src/mh_dll/.*\.(vcxproj|props|sln|bat)$",  # CITATION-OK -- a regex, not a path
        ("asan", "libref"),
        (),
    ),
    (
        "gfx / present",
        r"^src/mh_dll/mh/(gfx/|seams/(gfx_|video))",
        (),
        _GFX_ROWS,
    ),
    (
        "input",
        r"^src/mh_dll/mh/(input/|seams/(ui_keyrepeat|ui_chat_input))",
        (),
        _INPUT_ROWS,
    ),
    ("menu / HUD UI", r"^src/mh_dll/mh/(ui/|seams/(ui_|mp_menu))", (), _UI_ROWS),
    # TL-LIGHT-WALL-CAP: the `loopback` token (the six wall-bound udp/link suites, ~162 s plain and
    # again under ASan) is earned only by sources those suites EXECUTE: the mh_net / mh_net_udp
    # transports, the loopback selftest sources themselves, and adaptive_window.h (the one seams header
    # net_selftest includes for them). The mh/seams/net_* files compile into net_selftest but no
    # loopback suite drives them -- a seams-only net change keeps u19j + the net UI rows and no longer
    # pays ~320 s of sockets (it did on 2026-10-02: every light gate went cost-red at 573-628 s).
    (
        "transport (loopback suites)",
        r"^src/mh_dll/(mh_net|mh_net_udp)/|^src/mh_dll/mh_nettest/(net_selftest|selftest_dispatch|udp_)"
        r"|^src/mh_dll/mh/seams/adaptive_window",
        ("u19j_gpfg3", "loopback"),
        _NET_ROWS,
    ),
    (
        "lockstep / net seams",
        r"^src/mh_dll/mh/seams/(net_|launch|gone_peer)",
        ("u19j_gpfg3",),
        _NET_ROWS,
    ),
    (
        "map transfer",
        r"^src/mh_dll/mh/seams/map_transfer",
        ("x2a_map_variant",),
        _MAP_ROWS,
    ),
    # Named so they do not read as unmapped: the base set already runs what exercises them
    # (det/spdet read mh_harness.log; inmem applies the manifests; the smoke boots the shipped ini).
    (
        "covered by the base set",
        r"^src/mh_dll/(mh_harness/|mh/patch/|mh_net\.example\.ini$|.*\.vcxproj\.filters$)",
        (),
        (),
    ),
)
# Paths that change nothing a gate unit executes (still linted by the base `lint` unit).
_INERT = r"^(docs|notes|tasks|tracker|session_reports|NOTES\.md|TASKS\.md|README|\.claude/)"


def _git(*argv):
    r = subprocess.run(["git"] + list(argv), capture_output=True, text=True, cwd=REPO)
    return r.stdout if r.returncode == 0 else None


def _registry_rows_touched(base):
    """Row names whose registry.yaml block a `git diff base` hunk touches (new-side line numbers ->
    the nearest preceding `  - name:` line in the working-tree file)."""
    path = "tools/uiscripts/registry.yaml"
    diff = _git("diff", "-U0", base, "--", path) or ""
    try:
        lines = open(os.path.join(REPO, path), encoding="utf-8").read().splitlines()
    except OSError:
        return set()
    starts = [(i + 1, m.group(1)) for i, ln in enumerate(lines) for m in [NAME_RE.match(ln)] if m]
    out = set()
    for m in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", diff, re.M):
        a = int(m.group(1))
        n = int(m.group(2)) if m.group(2) is not None else 1
        for ln in range(a, a + max(n, 1)):
            prev = [nm for s, nm in starts if s <= ln]
            if prev:
                out.add(prev[-1])
    return out


NAME_RE = re.compile(r"^  - name:\s*(\S+)")


def light_selection(files, tests=None, registry_rows=()):
    """PURE: the change-aware additions for a list of repo-relative changed paths.

    tests = the UI registry rows (dicts, test_ui.TESTS) -- used to map an edited uiscripts file to
    the rows that name it; registry_rows = rows whose registry.yaml block changed. Returns
    {asan, units, rows, reasons: [(label, [files])], unmapped: [files]}."""
    sel = {
        "asan": False,
        "loopback": False,
        "units": set(),
        "rows": set(),
        "reasons": [],
        "unmapped": [],
    }
    by_label = {}
    tests = tests or []
    blobs = [(t["name"], json.dumps(t, default=str)) for t in tests]
    for f in sorted(set(files)):
        f = f.replace("\\", "/")
        hit = False
        for label, rx, units_, rows_ in LIGHT_RULES:
            if re.search(rx, f):
                hit = True
                for u in units_:
                    if u in ("asan", "loopback"):
                        sel[u] = True
                    else:
                        sel["units"].add(u)
                sel["rows"].update(rows_)
                by_label.setdefault(label, []).append(f)
        if f.startswith("tools/uiscripts/"):
            hit = True
            parts = f.split("/")
            names = set()
            if len(parts) > 3 and parts[2] == "baselines":
                names.add(parts[3])
            elif parts[-1] == "registry.yaml":
                names.update(registry_rows)
            else:
                base = parts[-1]
                names.update(n for n, blob in blobs if base in blob)
            known = {t["name"] for t in tests}
            names = {n for n in names if n in known} if tests else names
            sel["rows"].update(names)
            label = ",".join(sorted(names)) or "(none names it; base set only)"
            by_label.setdefault("uiscripts -> rows " + label, []).append(f)
        if not hit and not re.search(_INERT, f) and not f.startswith("tools/"):
            if f.startswith("src/"):
                sel["unmapped"].append(f)
    sel["reasons"] = sorted(by_label.items())
    if tests:
        known = {t["name"] for t in tests}
        sel["rows"] = {r for r in sel["rows"] if r in known}
    return sel


def full_green_status(now=None, rec=None, head_count=None):
    """{commit, when, commits, days, due, why}. rec/head_count are injectable for --selftest;
    head_count(commit) -> commits since it, or None when it is not an ancestor of HEAD."""
    rec = _load_json(FULL_GREEN) if rec is None else rec
    now = now or time.time()
    if not rec.get("commit"):
        return {"commit": None, "due": True, "why": "no full green on record", "commits": None}
    if head_count is None:

        def head_count(c):
            if _git("merge-base", "--is-ancestor", c, "HEAD") is None:
                return None
            out = _git("rev-list", "--count", c + "..HEAD")
            return int(out.strip()) if out else None

    n = head_count(rec["commit"])
    days = (now - float(rec.get("epoch", 0))) / 86400.0
    st = {"commit": rec["commit"], "when": rec.get("when"), "commits": n, "days": days}
    if n is None:
        st.update(
            due=True, why="full-green commit %s is not an ancestor of HEAD" % rec["commit"][:10]
        )
    elif n > FULL_DUE_COMMITS:
        st.update(due=True, why="%d commits since (> %d)" % (n, FULL_DUE_COMMITS))
    elif days > FULL_DUE_DAYS:
        st.update(due=True, why="%.1f days since (> %d)" % (days, FULL_DUE_DAYS))
    else:
        st.update(due=False, why="%d commits / %.1f days since" % (n, days))
    return st


def format_full_status(st):
    if not st.get("commit"):
        return (
            "FULL GATE DUE -- %s (run `python tools/run_gate.py` before a merge or release)"
            % (st["why"])
        )
    head = "FULL GATE DUE" if st["due"] else "full gate ok"
    return "%s -- last full green %s (%s): %s" % (
        head,
        st["commit"][:10],
        st.get("when") or "?",
        st["why"],
    )


def changed_files(args, default_base):
    """([repo-relative paths], base). --files overrides git (a plan for a hypothetical change);
    --diff-base overrides the profile's default base. Untracked files count either way."""
    if getattr(args, "files", ""):
        fs = [x.strip().replace("\\", "/") for x in args.files.split(",") if x.strip()]
        return fs, "(--files)"
    base = getattr(args, "diff_base", "") or default_base
    files = (_git("diff", "--name-only", base) or "").split()
    files += (_git("ls-files", "--others", "--exclude-standard") or "").split()
    return files, base


def _registry_tests():
    try:
        import test_ui  # noqa: PLC0415 -- the registry, only when a plan is asked for

        return list(test_ui.TESTS)
    except Exception as e:  # noqa: BLE001 -- a broken registry must not hide the base gate
        print("[gate] WARNING: could not load the UI registry (%s) -- no row mapping" % e)
        return []


def light_plan(args):
    """The light profile for THIS tree: (rows, selection, base, files, status)."""
    st = full_green_status()
    base = st.get("commit") if st.get("commits") is not None else None
    if base is None:
        depth = int((_git("rev-list", "--count", "HEAD") or "1").strip() or 1)
        base = "HEAD~%d" % min(FALLBACK_DEPTH, max(depth - 1, 0)) if depth > 1 else "HEAD"
    files, base = changed_files(args, default_base=base)
    tests = _registry_tests()
    sel = light_selection(files, tests, _registry_rows_touched(base))
    return light_units(args, sel, tests), sel, base, files, st


def _unit(rows, name):
    return next(r for r in rows if r["name"] == name)


def release_rows():
    """[(name, budget_s)] of the UI registry's `tier: release` rows ([] if the registry won't load)."""
    try:
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import test_ui  # noqa: PLC0415

        return [
            (t["name"], t.get("budget_s") or 0) for t in test_ui.TESTS if t.get("tier") == "release"
        ]
    except Exception:  # noqa: BLE001 -- the registry is the suite's problem, not the plan's
        return []


def tier_line(args, rows):
    """One line saying which UI tier this gate runs (empty-safe for a light/--skip profile)."""
    if args.profile == "light":
        return "UI tier: light subset (named rows; release-tier rows run only if a rule names them)"
    if not any(r["name"] == "suite" for r in rows):
        return "UI tier: (no suite unit)"
    rel = release_rows()
    names = ", ".join("%s(%ds)" % nb for nb in rel)
    if getattr(args, "release", False):
        return "UI tier: RELEASE -- bare suite + %d release-tier row(s), %d s budgeted: %s" % (
            len(rel),
            sum(b for _, b in rel),
            names,
        )
    return (
        "UI tier: default -- %d release-tier row(s) NOT run (%d s budgeted; --release adds them): %s"
        % (
            len(rel),
            sum(b for _, b in rel),
            names,
        )
    )


def light_units(args, sel, tests=()):
    """The light roster: the base set + the change-aware additions, built from the FULL roster's own
    rows (same commands, same weights) so the two profiles cannot drift apart unit by unit."""
    full = units(argparse.Namespace(**dict(vars(args), skip=set())))
    names = list(LIGHT_BASE_UNITS) + sorted(sel["units"])
    rows = [dict(_unit(full, n)) for n in names if any(r["name"] == n for r in full)]
    # THE REPLAYS RUN SHIP-ONLY (user 2026-09-29): arm A against the committed oracle C, no
    # all-original arm B. C is a mode=original replay's own stream (every oracle's `source:` line),
    # so A-vs-C detects what A-vs-B + B-vs-C detect; it gives up only ATTRIBUTION (promotion bug vs
    # replay drift), which the full profile's three arms still provide. One game, so weight 1.
    for r in rows:
        if r["name"] in LIGHT_ABC_UNITS:
            r["cmd"] = r["cmd"] + ["--ui-abc-arms", "A,C"]
            r["weight"] = 1
            r["why"] = r["why"].split(",")[0] + ", SHIP ARM ONLY (A vs the recording oracle C)"
    skip_suites = [] if sel.get("loopback") else ["--skip-suites", ",".join(LOOPBACK_SUITES)]
    loop_note = "" if sel.get("loopback") else "; loopback transport suites skipped (no net change)"
    if sel["asan"]:
        st = dict(_unit(full, "selftests"))
        st["cmd"] = st["cmd"] + skip_suites
        st["why"] = st["why"] + loop_note
        if not sel.get("loopback"):
            # the loopback suites are ~320 s of the ASan+plain unit: a record of the skipped variant
            # must not schedule (or be scheduled by) the full-length one (TL-LIGHT-WALL-CAP)
            st["dur_key"] = "selftests_noloop"
        else:
            # OPTION A (user 2026-10-03): on a transport change the loopback suites run in the PLAIN
            # pass only; their ASan run (~160 s, wall-bound) stays in the full gate. Keeps the light
            # gate under its cap through the host-migration work (581 / 607 s before).
            st["cmd"] = st["cmd"] + ["--asan-skip-suites", ",".join(LOOPBACK_SUITES)]
            st["why"] = st["why"] + "; loopback suites PLAIN only (ASan in the full gate)"
            st["dur_key"] = "selftests_loop_plain"
        rows.append(st)
    else:
        rows.append(
            {
                "name": "selftests_plain",
                "why": "step 2, PLAIN only (--no-asan): no spine/common C++ changed since the last "
                "full green -- the ASan pass is the full gate's (G32 forces it back when one does)"
                + loop_note,
                "cmd": [PY, os.path.join(REPO, "tools", "run_selftests.py"), "--no-asan"]
                + ([] if args.no_build else ["--no-build"])
                + skip_suites,
                "weight": 1,
                "timeout": 900,
            }
        )
    known = {t["name"] for t in tests} if tests else None
    ui = [r for r in SMOKE_ROWS if known is None or r in known]
    ui += sorted(r for r in sel["rows"] if r not in ui)
    if ui:
        solo = {t["name"] for t in tests if t.get("kind") == "solo"} if tests else set()
        n_solo = sum(1 for r in ui if r in solo) or 1
        n_multi = max(1, len(ui) - n_solo)
        jobs, net_jobs = min(2, n_solo), min(3, n_multi)
        rows.append(
            {
                "name": "smoke",
                "why": "step 3, SUBSET -- the UI smoke rows + the change-aware ones: "
                + ", ".join(ui),
                "cmd": [
                    PY,
                    os.path.join(REPO, "tools", "test_ui.py"),
                    "--jobs",
                    str(jobs),
                    "--net-jobs",
                    str(net_jobs),
                ]
                + ui,
                "weight": jobs + int(math.ceil(net_jobs * NET_JOB_WEIGHT)),
                "timeout": 1200,
                "ui_rows": ui,
            }
        )
    return [r for r in rows if r["name"] not in args.skip]


# ---- THE SMOKE PROFILE'S SELFTEST MAP (path -> the roster suites that execute it) ---------------------
# First-class data like LIGHT_RULES. A changed C++ file under a libmh domain runs only that domain's
# suites; a C++ file under any other test-linked tree (libmh_dll, mh_common, mh_nettest, libmh_test
# helpers, mh/seams, ...) has no one-domain answer, so it falls back to the whole plain pass minus the
# wall-bound loopback suites (those only when the `loopback` token is set). A C++ file no selftest
# links (mh/ui, mh/gfx, mh/input, mh_harness, mh/patch, mh/addr) maps to nothing: lint + the UI row.
_LIBMH_STATE = ("statetest", "inchashtest", "staterectest", "worldtest", "boottest", "navtest")
SMOKE_SUITE_MAP = (
    (r"^src/mh_dll/(libmh/ai/|libmh_test/ai_)", ("aitest",)),
    (r"^src/mh_dll/(libmh/sim/|libmh_test/sim_)", ("simtest",)),
    (r"^src/mh_dll/libmh/tact/", ("tacttest",)),
    (
        r"^src/mh_dll/(libmh/orders/|libmh_test/(orders|issue|order_issue))",
        ("orderstest", "issuetest"),
    ),
    (
        r"^src/mh_dll/(libmh/lockstep/|libmh_test/(lockstep|resync|net_session))",
        ("lockstest", "resynctest", "netsessiontest"),
    ),
    (r"^src/mh_dll/(libmh/save/|libmh_test/save_)", ("savetest",)),
    (r"^src/mh_dll/(libmh/state/|libmh_test/(boot|nav))", _LIBMH_STATE),
    (r"^src/mh_dll/libmh_test/crt_", ("crttest",)),
    (r"^src/mh_dll/libmh_test/fp_", ("fptest",)),
    (r"^src/mh_dll/libmh_test/lib_trans", ("libtranstest",)),
)
# C++ trees that link into a selftest exe but have no one-domain suite answer (fallback = every suite).
_SMOKE_FALLBACK_RX = (
    r"^src/mh_dll/(libmh|libmh_dll|libmh_std|libmh_test|mh_common|mh_nettest|mh_net|mh_net_udp)/.*"
    + _CXX[1:]
    + r"|^src/mh_dll/mh/seams/.*"  # CITATION-OK -- a regex, not a path
    + _CXX[1:]
)
_ALL_CXX_RX = r"^src/mh_dll/.*" + _CXX[1:]  # CITATION-OK -- a regex, not a path


def roster_suites():
    """The selftest roster's suite names, in roster order ([] if the data file is unreadable)."""
    r = _load_json(os.path.join(REPO, "tools", "data", "selftest_roster.json"))
    return [x["suite"] for x in r.get("suites", [])]


def smoke_selftests(files, loopback, suites=None):
    """PURE: (run, why) -- the roster suites the smoke profile runs for these changed paths.

    run is a list of suite names, or None for 'no selftest unit' (no C++ a selftest executes changed).
    The wall-bound loopback suites are never in a fallback run unless `loopback`."""
    suites = list(suites if suites is not None else roster_suites())
    wanted, fallback, any_cxx = [], [], False
    for f in sorted(set(x.replace("\\", "/") for x in files)):
        if not re.search(_ALL_CXX_RX, f):
            continue
        any_cxx = True
        hit = False
        for rx, names in SMOKE_SUITE_MAP:
            if re.search(rx, f):
                hit = True
                wanted += [n for n in names if n not in wanted]
        if not hit and re.search(_SMOKE_FALLBACK_RX, f):
            fallback.append(f)
    if fallback:
        run = [x for x in suites if loopback or x not in LOOPBACK_SUITES]
        return run, "no one-domain map for %s -> every suite (loopback %s)" % (
            ", ".join(fallback[:2]) + (" ..." if len(fallback) > 2 else ""),
            "on" if loopback else "off",
        )
    if loopback:
        wanted += [x for x in LOOPBACK_SUITES if x not in wanted]
    wanted = [x for x in suites if x in wanted]  # roster order; unknown names drop out
    if wanted:
        return wanted, "mapped domains: " + ", ".join(wanted)
    return None, ("no C++ change" if not any_cxx else "changed C++ is linked into no selftest")


def smoke_ui_row(sel, tests=()):
    """PURE: the ONE UI row the smoke profile runs -- the cheapest (solo before multi-peer, then
    budget_s) row a change-aware rule named -- or None. No base smoke rows: no rule hit, no UI row."""
    cand = sorted(sel["rows"])
    if not cand:
        return None
    by = {t["name"]: t for t in tests}
    if not by:
        return cand[0]
    return min(
        cand,
        key=lambda n: (by[n].get("kind") != "solo", by[n].get("budget_s") or 9999, n),
    )


def smoke_units(args, sel, files, tests=()):
    """The smoke roster, built from the FULL roster's own rows (same commands, same drift rule as
    light_units): lint + the mapped plain selftest unit + at most one UI row."""
    full = units(argparse.Namespace(**dict(vars(args), skip=set())))
    rows = [dict(_unit(full, "lint"))]
    run, why = smoke_selftests(files, sel.get("loopback", False))
    sel["smoke_selftests"] = (run, why)
    if run is not None:
        every = roster_suites()
        skip = [x for x in every if x not in run]
        cmd = [PY, os.path.join(REPO, "tools", "run_selftests.py"), "--no-asan", "--repeats", "1"]
        cmd += [] if args.no_build else ["--no-build"]
        cmd += ["--skip-suites", ",".join(skip)] if skip else []
        rows.append(
            {
                "name": "selftests_smoke",
                "why": "PLAIN selftests, 1 repeat, %d/%d suite(s) -- %s"
                % (len(run), len(every), why),
                "cmd": cmd,
                "weight": 1,
                "timeout": 600,
            }
        )
    row = smoke_ui_row(sel, tests)
    sel["smoke_ui_row"] = row
    if row:
        rows.append(
            {
                "name": "ui_one",
                "why": "ONE change-aware UI row: %s" % row,
                "cmd": [
                    PY,
                    os.path.join(REPO, "tools", "test_ui.py"),
                    "--jobs",
                    "1",
                    "--net-jobs",
                    "1",
                    row,
                ],
                "weight": 2,
                "timeout": 600,
                "ui_rows": [row],
            }
        )
    return [r for r in rows if r["name"] not in args.skip]


BUILD_HINT_S = 45  # the serial build step before the units (light gate 2026-10-02: 44.7 s)
RECORDS = {
    # which timing records a profile schedules by, first hit wins; each profile WRITES only its own.
    "full": (TIMINGS,),
    "light": (LIGHT_TIMINGS, TIMINGS),
    "smoke": (SMOKE_TIMINGS, LIGHT_TIMINGS, TIMINGS),
}


def profile_wall_cap(profile, release=False):
    if release and profile == "full":
        return RELEASE_GATE_WALL_MAX
    return {"smoke": SMOKE_GATE_WALL_MAX, "light": LIGHT_GATE_WALL_MAX}.get(profile, GATE_WALL_MAX)


def smoke_plan(args):
    """The smoke profile for THIS tree: (rows, selection, base, files). The diff base is HEAD (the
    uncommitted edit under iteration) unless --diff-base / --files say otherwise."""
    files, base = changed_files(args, default_base="HEAD")
    tests = _registry_tests()
    sel = light_selection(files, tests, _registry_rows_touched(base))
    return smoke_units(args, sel, files, tests), sel, base, files


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


def diet_reds(unit_secs, gate_wall, suite_rec, green, gate_cap=GATE_WALL_MAX, unit_budgets=None):
    """The routine's red lines (block 4b). unit_secs = {unit: seconds}; suite_rec = test_ui's
    record ({"per_test": {name: {"secs", "budget_s", "verdict"}}, ...}) or {}; green = the last
    green run's timings (of the SAME profile); gate_cap = the profile's wall cap (the light
    gate's is LIGHT_GATE_WALL_MAX). Returns a list of strings, empty when every rule holds."""
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
    # mp:U65 -- a non-UI unit that declares `budget_s` (the host-migration release rows) is held to it
    # the same way a suite scenario is: SCENARIO_OVER x the measured figure, growth against last green.
    for name, b in sorted((unit_budgets or {}).items()):
        secs = unit_secs.get(name, 0)
        if b and secs > SCENARIO_OVER * b:
            reds.append(
                "unit %s ran %.0fs > %.1fx its budget_s %ds (%s)"
                % (name, secs, SCENARIO_OVER, b, _growth(secs, green.get(name)))
            )
    if unit_secs.get("suite", 0) > SUITE_WALL_MAX:
        reds.append(
            "suite wall %.0fs > %ds critical-path cap (%s)"
            % (unit_secs["suite"], SUITE_WALL_MAX, _growth(unit_secs["suite"], green.get("suite")))
        )
    if gate_wall > gate_cap:
        reds.append(
            "gate wall %.0fs > %ds (%s)"
            % (gate_wall, gate_cap, _growth(gate_wall, green.get("_gate_wall")))
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
        ("suite over its cap", {"suite": SUITE_WALL_MAX + 1}, 850, {}, 1),
        ("gate over its cap", {"suite": 600}, GATE_WALL_MAX + 1, {}, 1),
        ("all three", {"suite": SUITE_WALL_MAX + 80}, GATE_WALL_MAX + 20, over, 3),
        ("a NEVER-STARTED scenario still reds", {"suite": 600}, 850, never_started, 1),
        ("an ASSERTION-NOT-RUN scenario still reds", {"suite": 600}, 850, assertion_not_run, 1),
        ("a LOAD-RED scenario still reds", {"suite": 600}, 850, load_red, 1),
        ("a RED-NOT-RERUN scenario still reds", {"suite": 600}, 850, red_not_rerun, 1),
        ("a LOAD-RED-ALLOW scenario does NOT red", {"suite": 600}, 850, load_red_allowed, 0),
    ]
    ub = {"hm_x": 100}
    for label, secs, want in (("unit within budget", 140, 0), ("unit 1.6x its budget", 160, 1)):
        got = diet_reds({"hm_x": secs}, 10, {}, green, unit_budgets=ub)
        hit = len(got) == want
        ok = ok and hit
        print("  %-26s %s  %s" % (label, "ok" if hit else "XX", "; ".join(got)[:150]))
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
    hit = len(diet_reds({}, 500, {}, green, gate_cap=LIGHT_GATE_WALL_MAX)) == 0 and (
        len(diet_reds({}, LIGHT_GATE_WALL_MAX + 1, {}, green, gate_cap=LIGHT_GATE_WALL_MAX)) == 1
    )
    print("  %-26s %s" % ("the light profile's own wall cap", "ok" if hit else "XX"))
    ok = ok and hit
    ok = _selftest_light() and ok
    print("run_gate selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def _selftest_light():
    """TL-GATE12: the change-aware map's and the escalation's negative cases. Pure -- no git, no rig
    -- except the one row that checks every LIGHT_RULES/SMOKE_ROWS name against the live registry."""
    ok = True
    tests = [
        {"name": "menu_walk", "kind": "solo", "script": "mp_menu_walk.txt"},
        {"name": "win_resize", "kind": "solo", "script": "win_resize.txt"},
        {
            "name": "imgui_hover",
            "kind": "solo",
            "script": "imgui_hover.txt",
            "extra_ini": "tools/uiscripts/ini/imgui_hover.ini",
        },
    ]
    # Fixture paths are FAKE on purpose (the rules must classify a file that does not exist yet), so
    # each is written once, here, under the citation lint's escape rather than scattered as literals.
    fx = {
        "doc": "docs/x.md",  # CITATION-OK -- selftest fixture
        "trk": "tracker/x.yaml",  # CITATION-OK -- selftest fixture
        "cpp": "src/mh_dll/libmh/sim/a.cpp",  # CITATION-OK -- selftest fixture
        "hdr": "src/mh_dll/mh_common/include/x.h",  # CITATION-OK -- selftest fixture
        "md": "src/mh_dll/libmh/README.md",  # CITATION-OK -- selftest fixture
        "proj": "src/mh_dll/mh/mh.vcxproj",
        "gfx": "src/mh_dll/mh/gfx/p.cpp",  # CITATION-OK -- selftest fixture
        "txt": "tools/uiscripts/imgui_hover.txt",
        "ini": "tools/uiscripts/ini/imgui_hover.ini",
        "png": "tools/uiscripts/baselines/win_resize/a.png",  # CITATION-OK -- selftest fixture
        "tool": "tools/make_lane.py",
        "net": "src/mh_dll/mh/seams/net_seams.cpp",
        "udp": "src/mh_dll/mh_net_udp/udp_endpoint.cpp",  # CITATION-OK -- selftest fixture
        "ai": "src/mh_dll/libmh/ai/ai_x.cpp",  # CITATION-OK -- selftest fixture
        "aitest": "src/mh_dll/libmh_test/ai_x_selftest.cpp",  # CITATION-OK -- selftest fixture
        "ui": "src/mh_dll/mh/ui/lobby_ui.cpp",  # CITATION-OK -- selftest fixture
        "new": "src/mh_dll/mh/fix/new_thing.cpp",  # CITATION-OK -- selftest fixture
        "reg": "tools/uiscripts/registry.yaml",
    }
    cases = [
        ("docs only -> nothing", ["doc", "trk"], False, set(), set()),
        ("libmh C++ forces ASan", ["cpp"], True, {"libref"}, set()),
        ("mh_common header forces ASan", ["hdr"], True, set(), set()),
        ("a libmh .md does not", ["md"], False, {"libref"}, set()),
        ("vcxproj forces ASan", ["proj"], True, {"libref"}, set()),
        ("gfx -> gfx rows", ["gfx"], False, set(), {"win_resize"}),
        ("uiscript -> its row", ["txt"], False, set(), {"imgui_hover"}),
        ("ini -> its row", ["ini"], False, set(), {"imgui_hover"}),
        ("baseline dir -> its row", ["png"], False, set(), {"win_resize"}),
        ("tools/*.py -> nothing extra", ["tool"], False, set(), set()),
        ("net seam -> u19j", ["net"], False, {"u19j_gpfg3"}, set()),
    ]
    hit = (
        light_selection([fx["udp"]], tests)["loopback"]
        and not light_selection([fx["net"]], tests)["loopback"]
        and not light_selection([fx["cpp"]], tests)["loopback"]
    )
    print("  %-34s %s" % ("only transport brings loopback back", "ok" if hit else "XX"))
    ok = ok and hit
    for label, keys, asan, units_, rows_ in cases:
        s = light_selection([fx[k] for k in keys], tests)
        hit = s["asan"] == asan and s["units"] == units_ and s["rows"] == rows_
        ok = ok and hit
        print("  %-34s %s  %s" % (label, "ok" if hit else "XX", (s["asan"], s["units"], s["rows"])))
    s = light_selection([fx["new"]], tests)
    hit = s["unmapped"] == [fx["new"]]
    print("  %-34s %s" % ("an unmapped source is NAMED", "ok" if hit else "XX"))
    ok = ok and hit
    s = light_selection([fx["reg"]], tests, registry_rows={"win_resize"})
    hit = s["rows"] == {"win_resize"}
    print("  %-34s %s" % ("a registry hunk -> the row it edits", "ok" if hit else "XX"))
    ok = ok and hit

    ok = _selftest_smoke(fx, tests) and ok

    # the scheduler (pick_units): a blocked WIDE unit reserves the cores it is waiting for
    def row(n, d, w, **kw):
        return dict(name=n, dur=d, weight=w, **kw)

    rs = [row("det", 145, 2, first=True, vm=True), row("long", 380, 4), row("wide", 160, 3)]
    rs += [row("short", 100, 1), row("filler", 150, 1)]
    sp, wall = simulate(rs, 7)
    # t0: det(2) + long(4) = 6; wide(3) blocked until det ends at 145 -> spare 0 at that moment, so
    # `short` (ends 100 < 145) may backfill and `filler` (150 s, would still hold its core) may not.
    # (Greedy longest-first starts filler at t0 and wide only at 150.)
    hit = sp["short"][0] == 0 and sp["wide"][0] == 145 and sp["filler"][0] >= 145
    print("  %-34s %s  %s" % ("EASY: backfill never delays a wide unit", "ok" if hit else "XX", sp))
    ok = ok and hit
    hit = [r["name"] for r in priority(rs)][:2] == ["det", "long"]
    print("  %-34s %s" % ("priority: `first`, then longest", "ok" if hit else "XX"))
    ok = ok and hit
    vm2 = [row("a", 50, 1, vm=True), row("b", 50, 1, vm=True)]
    sp, wall = simulate(vm2, 7)
    hit = sp["b"][0] == 50 and wall == 100
    print("  %-34s %s" % ("the VM slot still serialises", "ok" if hit else "XX"))
    ok = ok and hit
    # the light replays are ship-only, and ui_abc refuses every arm set but A,B,C / A,C
    lrows = light_units(_selftest_args(), light_selection([], []), [])
    abc = [r for r in lrows if r["name"] in LIGHT_ABC_UNITS]
    hit = len(abc) == 2 and all(
        r["cmd"][-2:] == ["--ui-abc-arms", "A,C"] and r["weight"] == 1 for r in abc
    )
    full_abc = [r for r in units(_selftest_args()) if r["name"] in LIGHT_ABC_UNITS]
    hit = hit and not any("--ui-abc-arms" in r["cmd"] for r in full_abc)
    print("  %-34s %s" % ("light abc = A,C; full keeps A,B,C", "ok" if hit else "XX"))
    ok = ok and hit

    # release tier: the full suite unit carries --release-tier only under --release; light never does
    def suite_cmd(**kw):
        ns = argparse.Namespace(**dict(vars(_selftest_args()), **kw))
        return next(r for r in units(ns) if r["name"] == "suite")["cmd"]

    hit = (
        "--release-tier" not in suite_cmd()
        and "--release-tier" in suite_cmd(release=True)
        and not any("--release-tier" in r["cmd"] for r in lrows)
    )
    print("  %-34s %s" % ("--release adds --release-tier only", "ok" if hit else "XX"))
    ok = ok and hit
    try:
        import ui_abc  # noqa: PLC0415

        good = [ui_abc.parse_abc_arms(v)[0] for v in ("A,B,C", "c,a,b", None, "A,C", "C,A")]
        bad = [ui_abc.parse_abc_arms(v)[0] for v in ("A", "A,B", "B,C", "C", "A,C,D", "")]
        hit = all(good) and good[3] == ("ship",) and not any(bad[:5]) and bad[5] is not None
        print("  %-34s %s" % ("ui_abc --ui-abc-arms refusals", "ok" if hit else "XX"))
    except Exception as e:  # noqa: BLE001
        hit = False
        print("  %-34s XX  %s" % ("ui_abc --ui-abc-arms refusals", e))
    ok = ok and hit
    # escalation
    now = 1_000_000.0
    rec = {"commit": "abc123", "epoch": now - 3600, "when": "t"}
    esc = [
        ("no record -> due", {}, lambda c: 0, True),
        ("fresh -> not due", rec, lambda c: 5, False),
        ("too many commits -> due", rec, lambda c: FULL_DUE_COMMITS + 1, True),
        ("not an ancestor -> due", rec, lambda c: None, True),
        ("too old -> due", dict(rec, epoch=now - 86400 * (FULL_DUE_DAYS + 1)), lambda c: 1, True),
    ]
    for label, r, hc, want in esc:
        st = full_green_status(now=now, rec=r, head_count=hc)
        hit = st["due"] == want
        ok = ok and hit
        print("  %-34s %s  %s" % (label, "ok" if hit else "XX", st["why"]))
    hit = should_record_full_green("full", set(), "PASS") and not any(
        should_record_full_green(p, sk, v)
        for p, sk, v in (
            ("light", set(), "PASS"),
            ("full", {"det"}, "PASS"),
            ("full", set(), "FAIL"),
        )
    )
    print("  %-34s %s" % ("only an unskipped full PASS records", "ok" if hit else "XX"))
    ok = ok and hit
    # every name the rules promise must exist in the LIVE registry and roster, or the map has rotted
    try:
        import test_ui  # noqa: PLC0415

        live = {t["name"] for t in test_ui.TESTS}
        promised = set(SMOKE_ROWS).union(*(set(r[3]) for r in LIGHT_RULES))
        gone = sorted(promised - live)
        roster = {r["name"] for r in units(_selftest_args())}
        ugone = sorted(
            (set(LIGHT_BASE_UNITS).union(*(set(r[2]) for r in LIGHT_RULES)) - {"asan", "loopback"})
            - roster
        )
        roster_suites = {
            r["suite"]
            for r in _load_json(os.path.join(REPO, "tools", "data", "selftest_roster.json"))[
                "suites"
            ]
        }
        ugone += sorted(set(LOOPBACK_SUITES) - roster_suites)
        hit = not gone and not ugone
        print(
            "  %-34s %s  %s"
            % (
                "every rule row/unit exists",
                "ok" if hit else "XX",
                (gone, ugone) if not hit else "",
            )
        )
    except Exception as e:  # noqa: BLE001
        hit = False
        print("  %-34s XX  registry load failed: %s" % ("every rule row/unit exists", e))
    ok = ok and hit
    return ok


def _selftest_smoke(fx, tests):
    """TL-LIGHT-WALL-CAP: the smoke profile's map, row pick, roster shape and cap. Pure."""
    ok = True
    suites = [
        "aitest", "simtest", "udpsnaptest", "udploopbacktest", "relinktest", "wstest", "savetest",
    ]  # fmt: skip

    def chk(label, hit, detail=""):
        nonlocal ok
        ok = ok and bool(hit)
        print("  %-34s %s  %s" % (label, "ok" if hit else "XX", detail))

    run, _ = smoke_selftests([fx["ai"], fx["aitest"]], False, suites)
    chk("smoke: libmh/ai -> aitest only", run == ["aitest"], run)
    run, _ = smoke_selftests([fx["net"]], False, suites)
    chk(
        "smoke: seams -> all minus loopback",
        run == ["aitest", "simtest", "wstest", "savetest"],
        run,
    )
    run, _ = smoke_selftests([fx["udp"], fx["ai"]], True, suites)
    chk("smoke: transport -> loopback back", run == suites, run)
    run, _ = smoke_selftests([fx["ai"]], True, suites)
    chk(
        "smoke: domain + loopback token",
        run == ["aitest", "udpsnaptest", "udploopbacktest", "relinktest"],
        run,
    )
    run, _ = smoke_selftests([fx["gfx"], fx["ui"], fx["doc"], fx["tool"]], False, suites)
    chk("smoke: ui/gfx/docs/tools -> no selftest", run is None, run)
    sel = light_selection([fx["gfx"], fx["ui"]], tests)
    chk(
        "smoke: at most ONE ui row",
        smoke_ui_row(sel, tests) == "win_resize",
        smoke_ui_row(sel, tests),
    )
    t2 = [
        {"name": "a_multi", "kind": "multi", "budget_s": 10},
        {"name": "b_solo_slow", "kind": "solo", "budget_s": 90},
        {"name": "c_solo_fast", "kind": "solo", "budget_s": 20},
    ]
    pick = smoke_ui_row({"rows": {"a_multi", "b_solo_slow", "c_solo_fast"}}, t2)
    chk("smoke: cheapest solo row wins", pick == "c_solo_fast", pick)
    chk(
        "smoke: no rule -> no ui row",
        smoke_ui_row(light_selection([fx["cpp"]], tests), tests) is None,
    )
    ns = _selftest_args()
    s0 = light_selection([fx["cpp"], fx["gfx"]], tests)
    rows = smoke_units(ns, s0, [fx["cpp"], fx["gfx"]], tests)
    names = [r["name"] for r in rows]
    cmd = next(r for r in rows if r["name"] == "selftests_smoke")["cmd"]
    chk(
        "smoke roster: no VM/replay/ASan",
        names == ["lint", "selftests_smoke", "ui_one"]
        and not any(r.get("vm") for r in rows)
        and "--no-asan" in cmd
        and "--repeats" in cmd,
        names,
    )
    chk(
        "smoke has its own cap + records",
        SMOKE_GATE_WALL_MAX < LIGHT_GATE_WALL_MAX
        and profile_wall_cap("smoke") == SMOKE_GATE_WALL_MAX
        and len({SMOKE_TIMINGS, LIGHT_TIMINGS, TIMINGS, SMOKE_GREEN_TIMINGS}) == 4
        and RECORDS["smoke"][0] == SMOKE_TIMINGS,
    )
    chk(
        "smoke wall cap reds past it",
        len(diet_reds({}, SMOKE_GATE_WALL_MAX + 1, {}, {}, gate_cap=profile_wall_cap("smoke"))) == 1
        and not diet_reds({}, SMOKE_GATE_WALL_MAX, {}, {}, gate_cap=profile_wall_cap("smoke")),
    )
    chk("smoke never records a full green", not should_record_full_green("smoke", set(), "PASS"))
    roster = set(roster_suites())
    mapped = set(_LIBMH_STATE).union(*(set(n) for _rx, n in SMOKE_SUITE_MAP))
    chk("smoke map names exist in roster", not roster or mapped <= roster, sorted(mapped - roster))
    # the light plan's selftests unit, loopback skipped, schedules by its OWN record key
    lrows = light_units(ns, light_selection([fx["cpp"]], tests), tests)
    st = next(r for r in lrows if r["name"] == "selftests")
    chk("light: noloop variant has own record key", st.get("dur_key") == "selftests_noloop")
    return ok


def _selftest_args():
    return argparse.Namespace(
        steps=3000, suite_jobs=2, suite_net_jobs=6, no_build=False, skip=set()
    )


def should_record_full_green(profile, skip, verdict):
    """The FULL green record is the escalation clock: a partial run (--skip, or the light profile)
    proves less than the full gate and must not reset it. (Before TL-GATE12 a --skip run that ran ONE
    unit overwrote last_green_timings.json, so the next red's growth figures compared against 195 s.)"""
    return profile == "full" and not skip and verdict == "PASS"


def load_durations(rows, records=(TIMINGS,)):
    """Scheduling hints from the measured records, first record that names the unit wins (the light
    profile reads its own record, then the full one: the shared units run the same commands)."""
    recs = [_load_json(p) for p in records]
    for r in rows:
        k = r.get("dur_key", r["name"])
        v = next((rec[k] for rec in recs if k in rec), None)
        v = DUR_HINTS.get(k, 300) if v is None else v
        r["dur"] = float(v) if isinstance(v, (int, float)) else 300.0
    return rows


# ---- THE SCHEDULER, PURE (TL-GATE12 speed-up, 2026-09-29) ----------------------------------------
# Longest-first with backfill, as before, PLUS ONE RESERVATION: when the highest-priority pending
# unit does not fit the free cores, the dispatcher computes when it WILL fit (the running units'
# expected ends) and lets a lower-priority unit take free cores only if it is expected to finish
# before then, or if it fits in the cores the blocked unit will not need at that moment. That is
# EASY backfilling, and it closes the measured light-gate defect: the 17-row UI smoke (w=3, 158 s)
# waited while shorter units backfilled every core det released, and started at ~140 s instead of
# when det finished. The live dispatcher and `--plan`'s simulated timeline call the SAME function,
# so the plan is the schedule the run would take if every unit ran its recorded duration.
def priority(rows):
    """det-style `first` rows, then longest expected wall, then widest."""
    return sorted(rows, key=lambda r: (not r.get("first"), -r["dur"], -r["weight"]))


def pick_units(pending, free, free_vm, running, now, cores):
    """PURE: the units of `pending` (priority order) to start at `now`.

    free / free_vm = idle cores / VM slots; running = [(expected_end, weight)] of what holds cores.
    A unit waiting only for the VM slot never takes the reservation (the VM pair is its own queue)."""
    start, run, resv = [], list(running), None
    for r in pending:
        w = min(r["weight"], cores)
        if r.get("vm") and free_vm < 1:
            continue
        if resv is not None:
            if w > free:
                continue
            if now + r["dur"] > resv[0]:
                if w > resv[1]:
                    continue  # would still hold the blocked unit's cores when it could start
                resv[1] -= w
        elif w > free:
            acc, shadow = free, now
            for end, rw in sorted(run):
                if acc >= w:
                    break
                acc += rw
                shadow = max(end, now)
            resv = [shadow, acc - w]
            continue
        start.append(r)
        free -= w
        if r.get("vm"):
            free_vm -= 1
        run.append((now + r["dur"], w))
    return start


def simulate(rows, cores):
    """The timeline pick_units produces if every unit runs exactly its `dur`: ({name: (a, b)}, wall)."""
    pending, running, spans, now = priority(rows), [], {}, 0.0
    free, free_vm = cores, 1
    while pending or running:
        for r in pick_units(pending, free, free_vm, [(e, w) for e, w, _r in running], now, cores):
            w = min(r["weight"], cores)
            free -= w
            free_vm -= 1 if r.get("vm") else 0
            pending.remove(r)
            running.append((now + r["dur"], w, r))
            spans[r["name"]] = (now, now + r["dur"])
        if not running:
            break  # nothing fits and nothing runs: a weight > cores that min() did not cap
        running.sort(key=lambda x: x[0])
        end, w, r = running.pop(0)
        now = end
        free += w
        free_vm += 1 if r.get("vm") else 0
    return spans, now


def kill_tree(pid):
    """taskkill /T: a unit is a runner that spawns games; killing only the runner leaks them."""
    subprocess.run(
        ["taskkill", "/F", "/T", "/PID", str(pid)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def build(asan=True):
    """Step 1, serial: everything else executes this build's output. Mirrors the README recipe
    (/m /nodeReuse:false -- node reuse leaves workers holding our stdout handle for ~15 min).
    asan=False (the light profile with no spine/common C++ changed) stages the PLAIN selftest exes
    only: the ASan tree is the full gate's, and it rebuilds incrementally the next time one runs."""
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
    # THE SELFTEST EXES ARE STEP 1 TOO (2026-09-27). The `selftests` unit used to compile them (ASan +
    # plain, the heavy half of its old weight 3) while the suite, det and the A/B/C units were
    # already running games -- the compiler and the games fought for the same cores and the suite's
    # timing-sensitive rows paid for it. A compile saturates every core on its own, so serialising it
    # costs no wall time: it only stops it from being contended.
    t1 = time.time()
    slog = os.path.join(LOG_DIR, "selftests_build.log")
    print("[gate] selftest exes (%s) ..." % ("ASan + plain" if asan else "plain only"))
    with open(slog, "w", encoding="utf-8") as fh:
        src = subprocess.run(
            [PY, os.path.join(REPO, "tools", "run_selftests.py"), "--build-only"]
            + ([] if asan else ["--no-asan"]),
            stdout=fh,
            stderr=subprocess.STDOUT,
        ).returncode
    if src != 0:
        print(
            "[gate] selftest build FAILED (%.0fs) -- aborting; log: %s" % (time.time() - t1, slog)
        )
        for ln in open(slog, encoding="utf-8", errors="replace").readlines()[-15:]:
            print("    " + ln.rstrip())
        return False
    print("[gate] selftest exes ok (%.0fs)" % (time.time() - t1))
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
    ap.add_argument("--list", action="store_true", help="print both profiles' rosters and exit")
    ap.add_argument(
        "--profile",
        choices=("full", "light", "smoke"),
        default="full",
        help="full (default): the whole roster -- big checkpoints, merges, releases. light: the "
        "iteration checkpoint + change-aware additions (tooling:TL-GATE12)",
    )
    ap.add_argument("--light", action="store_true", help="alias for --profile light")
    ap.add_argument(
        "--smoke",
        action="store_true",
        help="alias for --profile smoke: the ITERATION run (build + the touched domains' plain "
        "selftests + lint + at most one UI row, < ~3 min). NOT a commit gate -- commit on --light "
        "(tooling:TL-LIGHT-WALL-CAP)",
    )
    ap.add_argument(
        "--files",
        default="",
        help="--plan only: comma-separated repo-relative paths to plan FOR instead of the git diff "
        "(a what-if for a hypothetical change)",
    )
    ap.add_argument(
        "--diff-base",
        default="",
        help="override the profile's git diff base (smoke: HEAD; light: the last full green)",
    )
    ap.add_argument(
        "--release",
        action="store_true",
        help="full profile only: also run the UI registry's `tier: release` rows (fix-off negatives, "
        "P17 watchdog/alt-tab, retail twins) -- the pre-release run; a bare full gate skips them",
    )
    ap.add_argument(
        "--plan",
        action="store_true",
        help="print what the profile would run on this tree (and, for light, why) and exit",
    )
    ap.add_argument(
        "--full-status",
        action="store_true",
        help="print the distance from the last FULL green; exit 3 when a full gate is due",
    )
    args = ap.parse_args()
    args.skip = {s.strip() for s in args.skip.split(",") if s.strip()}
    if args.light:
        args.profile = "light"
    if args.smoke:
        if args.light:
            ap.error("--smoke and --light are exclusive")
        args.profile = "smoke"
    if args.release and args.profile != "full":
        ap.error("--release is a full-profile flag (light/smoke run their own row subsets)")
    if args.selftest:
        return selftest()
    if args.full_status:
        st = full_green_status()
        print(format_full_status(st))
        return 3 if st["due"] else 0

    full_rows = units(args)
    if args.list:
        print("PROFILE full (default) -- %d unit(s):" % len(full_rows))
        for r in full_rows:
            print("  %-16s w=%d  t<=%ds  %s" % (r["name"], r["weight"], r["timeout"], r["why"]))
        lrows, sel, base, files, st = light_plan(args)
        print(
            "\nPROFILE light (--light) -- base %s + selftests_plain|selftests + smoke (%s); on "
            "THIS tree (diff vs %s, %d file(s)):"
            % (", ".join(LIGHT_BASE_UNITS), " + ".join(SMOKE_ROWS), base, len(files))
        )
        for r in lrows:
            print("  %-16s w=%d  t<=%ds  %s" % (r["name"], r["weight"], r["timeout"], r["why"]))
        print("  change-aware rules (path -> added units / UI rows; `asan` = the ASan pass):")
        for label, rx, u, rw in LIGHT_RULES:
            print("    %-40s -> %s" % (label, ", ".join(list(u) + list(rw)) or "(base set)"))
        print("    %-40s -> the rows that name the file" % "tools/uiscripts/**")
        print("  " + format_full_status(st))
        srows, ssel, sbase, sfiles = smoke_plan(args)
        print(
            "\nPROFILE smoke (--smoke) -- lint + mapped plain selftests + <=1 UI row; NOT a commit "
            "gate; on THIS tree (diff vs %s, %d file(s)):" % (sbase, len(sfiles))
        )
        for r in srows:
            print("  %-16s w=%d  t<=%ds  %s" % (r["name"], r["weight"], r["timeout"], r["why"]))
        return 0

    sel = None
    skipped = []
    if args.profile == "light":
        rows, sel, base, files, st = light_plan(args)
        every = units(argparse.Namespace(**dict(vars(args), skip=set())))
        skipped = sorted({r["name"] for r in every} - {r["name"] for r in rows})
        print("[gate] PROFILE light -- %s" % format_full_status(st))
        print(
            "[gate]   diff base %s: %d changed file(s); ASan %s"
            % (base, len(files), "FORCED" if sel["asan"] else "skipped (no spine/common C++)")
        )
        for label, fs in sel["reasons"]:
            more = " ..." if len(fs) > 3 else ""
            print("[gate]   + %-40s %s%s" % (label, ", ".join(fs[:3]), more))
        if sel["unmapped"]:
            print(
                "[gate]   ! %d changed source file(s) no rule maps -- base set only: %s"
                % (len(sel["unmapped"]), ", ".join(sel["unmapped"][:5]))
            )
        print("[gate]   skipped vs full: %s" % (", ".join(skipped) or "(none)"))
    elif args.profile == "smoke":
        rows, sel, base, files = smoke_plan(args)
        skipped = ["every other unit (smoke = lint + mapped plain selftests + <=1 UI row)"]
        print("[gate] PROFILE smoke -- %s" % SMOKE_NOT_A_GATE)
        print(
            "[gate]   diff base %s: %d changed file(s); ASan %s"
            % (base, len(files), "would be FORCED by --light" if sel["asan"] else "not needed")
        )
        for label, fs in sel["reasons"]:
            print(
                "[gate]   + %-40s %s%s" % (label, ", ".join(fs[:3]), " ..." if len(fs) > 3 else "")
            )
        print("[gate]   selftests: %s" % sel["smoke_selftests"][1])
        print("[gate]   ui row: %s" % (sel["smoke_ui_row"] or "(none -- no rule named one)"))
    else:
        rows = full_rows
    if args.plan:
        for r in rows:
            print("  %-16s w=%d  t<=%ds  %s" % (r["name"], r["weight"], r["timeout"], r["why"]))
        print("  " + tier_line(args, rows))
        records = RECORDS[args.profile]
        spans, wall = simulate(load_durations([dict(r) for r in rows], records), args.cores)
        print(
            "  simulated schedule (%d cores, recorded durations; the dispatcher's own rule):"
            % args.cores
        )
        for n, (a, b) in sorted(spans.items(), key=lambda kv: kv[1]):
            print("    %-16s %6.0f -> %6.0f s" % (n, a, b))
        print("    expected wall %.0f s (%.1f min)" % (wall, wall / 60))
        if args.profile == "full":
            print("  " + format_full_status(full_green_status()))
        else:
            # + the serial build step, which the simulated schedule does not include
            print(
                "    wall cap %d s (%s profile); build is serial before the units (~%d s measured), "
                "so expected total ~%.0f s"
                % (
                    profile_wall_cap(args.profile, getattr(args, "release", False)),
                    args.profile,
                    BUILD_HINT_S,
                    wall + BUILD_HINT_S,
                )
            )
        if args.profile == "smoke":
            print("  " + SMOKE_NOT_A_GATE)
        return 0

    os.makedirs(LOG_DIR, exist_ok=True)
    t0 = time.time()
    need_asan = args.profile == "full" or (args.profile == "light" and bool(sel and sel["asan"]))
    if not args.no_build and not build(asan=need_asan):
        return 1

    records = RECORDS[args.profile]
    rows = load_durations(rows, records)
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
    pending = priority(rows)
    running = set()
    ends = {}  # unit -> (expected end from gate t0, weight): pick_units' reservation input
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
            ends.pop(name, None)
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
                    now = time.time() - t0
                    for r in pick_units(
                        pending, free[0], free_vm[0], list(ends.values()), now, args.cores
                    ):
                        w = min(r["weight"], args.cores)
                        free[0] -= w
                        if r.get("vm"):
                            free_vm[0] -= 1
                        pending.remove(r)
                        running.add(r["name"])
                        ends[r["name"]] = (now + r["dur"], w)
                        threading.Thread(target=run_unit, args=(r, w), daemon=True).start()
                    if pending or running:
                        # a timed wait: an over-running unit moves the reservation, and the
                        # dispatcher must re-plan even when nothing finished
                        sched.wait(timeout=5)
        finally:
            os.environ.pop(hostlock.RIG_LEASE_ENV, None)

    gate_wall = time.time() - t0
    suite_rec = {}
    light = args.profile == "light"
    partial = args.profile != "full"  # light and smoke never write the full record
    ui_unit = {"light": "smoke", "smoke": "ui_one"}.get(args.profile, "suite")
    if ui_unit in spans:
        try:
            if os.path.getmtime(SUITE_RECORD) >= t0 + spans[ui_unit][0]:
                suite_rec = _load_json(SUITE_RECORD)
        except OSError:
            pass
    timings_path = {"light": LIGHT_TIMINGS, "smoke": SMOKE_TIMINGS}.get(args.profile, TIMINGS)
    green_path = {"light": LIGHT_GREEN_TIMINGS, "smoke": SMOKE_GREEN_TIMINGS}.get(
        args.profile, GREEN_TIMINGS
    )
    green = _load_json(green_path)
    reds = diet_reds(
        {n: v[1] for n, v in results.items()},
        gate_wall,
        suite_rec,
        green,
        gate_cap=profile_wall_cap(args.profile, getattr(args, "release", False)),
        unit_budgets={r["name"]: r["budget_s"] for r in rows if r.get("budget_s")},
    )
    all_pass = len(results) == len(rows) and all(v[0] == "PASS" for v in results.values())

    # The measured record the NEXT run schedules by (unit keys: every unit that ran to a verdict --
    # a red SUITE still ran its full length, and dropping it made the next run schedule the gate's
    # longest unit by a 280 s hint behind shorter ones; a TIMEOUT is left out), plus the routine's own
    # fields (underscore keys): per-scenario suite seconds, unit spans, the gate wall and verdict.
    dur_key = {r["name"]: r.get("dur_key", r["name"]) for r in rows}
    rec = {
        dur_key.get(n, n): round(v[1], 1) for n, v in results.items() if v[0] in ("PASS", "FAIL")
    }
    # A --skip run measured only what it ran; the units it skipped keep their last recorded seconds,
    # or the next run schedules them by the DUR_HINTS guesses (2026-09-29: a `--light --skip` run
    # left last_light_timings.json holding det alone).
    prev = _load_json(timings_path)
    for k, v in prev.items():
        if not k.startswith("_") and k not in rec and isinstance(v, (int, float)):
            rec[k] = v
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
    rec["_profile"] = args.profile
    rec["_tier"] = "release" if args.release else "default"
    rec["_skip"] = sorted(args.skip)
    head = (_git("rev-parse", "HEAD") or "").strip()
    rec["_commit"] = head
    rec["_dirty"] = bool((_git("status", "--porcelain", "--untracked-files=no") or "").strip())
    if partial:
        rec["_light_selection"] = {
            "asan": sel["asan"],
            "loopback": sel["loopback"],
            "units": sorted(sel["units"]),
            "rows": sorted(sel["rows"]),
            "unmapped": sel["unmapped"],
            "skipped_vs_full": skipped,
        }
    try:
        with open(timings_path, "w", encoding="utf-8") as fh:
            json.dump(rec, fh, indent=1)
        # a --skip run proves less than its profile, so it never becomes that profile's green base
        if rec["_verdict"] == "PASS" and not args.skip:
            shutil.copyfile(timings_path, green_path)
        if should_record_full_green(args.profile, args.skip, rec["_verdict"]) and head:
            with open(FULL_GREEN, "w", encoding="utf-8") as fh:
                json.dump(
                    {
                        "commit": head,
                        "dirty": rec["_dirty"],
                        "epoch": time.time(),
                        "when": datetime.datetime.now().isoformat(timespec="seconds"),
                        "gate_wall": round(gate_wall, 1),
                    },
                    fh,
                    indent=1,
                )
    except OSError:
        pass

    print("\n" + "=" * 78)
    print("THE GATE (%s profile)" % args.profile)
    print("  " + tier_line(args, rows))
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
    if args.profile == "smoke":
        print("  " + SMOKE_NOT_A_GATE)
    elif light:
        print("  SKIPPED vs full: %s" % (", ".join(skipped) or "(none)"))
        st = full_green_status()
        print("  " + format_full_status(st))
        if st["due"]:
            print("  -> run the FULL gate (`python tools/run_gate.py`) before a merge or release")
    elif should_record_full_green(args.profile, args.skip, rec["_verdict"]):
        print("  recorded FULL green at %s (tmp/gate/last_full_green.json)" % head[:10])
    return 1 if bad or reds else 0


if __name__ == "__main__":
    raise SystemExit(main())
