#!/usr/bin/env python3
"""UI regression SUITE -- walk every committed UI test and report pass/fail.

Thin driver over tools/ui_test.py: each entry in TESTS is one canonical scenario (a solo local walk or a
multi-peer VM topology) with a committed baseline under tools/uiscripts/baselines/<script-label>/. This
runs them all (or a filtered subset) and aggregates PASS/FAIL/SKIP so a regression is caught in one command.

    python tools/test_ui.py                 # run every test (solo + VM)
    python tools/test_ui.py --solo          # only the single-peer tests (host-only on the first VM)
    python tools/test_ui.py --only s7_occ_cap match_launch
    python tools/test_ui.py --list          # list tests + exit
    python tools/test_ui.py --update-baselines   # regenerate every baseline (careful)

THE SUITE RUNS LOCALLY BY DEFAULT (since 2026-07-29): one lane folder per peer per test on this box,
headless, `--jobs N` to run tests concurrently. Two reasons that is the default rather than an option --
each test is isolated from the setup.dat / [video] state the previous one left behind, and the peer VMs
stay FREE, so the UI suite and a 2-machine determinism run no longer contend for the same two machines.

Pass --no-local to run across the real VMs instead (the IPs come from --vms, which is the IP list, not
the topology switch). Do that when the thing under test is genuinely machine-dependent -- a cross-machine
transport or timing question. A pure UI/render regression does not need it, and pays VM latency plus
contention with whatever rig run is in flight. With --no-local, multi-peer tests place the host on the
first --vms IP and each client on the next, and a test is SKIPPED (not failed) when a VM is unreachable.
"""

import argparse
import concurrent.futures as cf
import glob
import gzip
import io as _io
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import desktop  # noqa: E402  the raw CreateProcessW launch that honours lpDesktop
import lane_alloc  # noqa: E402  fork F4H: the ONE place a lane NUMBER comes from
import ui_test  # noqa: E402  boot_lock + wait_past_pack_load, shared with the UI suite
import machine_config as machine  # noqa: E402
import make_lane  # noqa: E402  LANE_ROOT + the lane builder used by --local

# Where the community saves live -- the same directory the save-sweep tool indexes into
# tools/data/save_index.json, so a name printed by the session driver's `--start` resolves here.
SAVE_STORAGE = machine.SAVE_STORAGE

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_TEST = os.path.join(REPO, "tools", "ui_test.py")
# --desktop <name>, forwarded to every ui_test.py invocation from run_ui_test. Empty = the
# interactive desktop, i.e. today's behaviour.
DESKTOP = ""
NO_DESKTOP = False
# Forwarded to ui_test.py at the run_ui_test choke point, same as DESKTOP -- see the comment
# there about the five argv builders a per-builder flag would have to be added to.
# DEFAULT since 2026-08-27 (I6b): the rig measures the SHIPPING
# configuration -- retail exe bytes + the msvfw32 proxy shim. --patched-exe opts out.
STOCK_EXE = True

# Each test: name -> {kind, desc, script | (host, clients)}. Script names resolve under tools/uiscripts/.
# The baseline label is the script's basename (ui_test.py convention), so every script here must have a
# committed tools/uiscripts/baselines/<label>/ (regenerate with --update-baselines).
# The tactical journal scenarios. A DIFFERENT KIND of test from everything else in this list: the
# rest drive the UI and compare rendered frames, while these replay a recorded human session and
# compare the simulation it produces. They are registered here so `--list` shows them and so the
# committed journal has an owner. THEIR `equiv` ARM NOW RUNS IN THE DEFAULT SUITE (2026-09-04):
# the old exclusion -- "one is ~20 minutes against ~5 for all eleven capture scenarios" -- was true
# when written and became false when the mission-end exit landed (G118, ~22 s per arm). The full
# verify/replay pair stays behind `--tact-suite` as diagnostics; only the differential gates.
# THE STRATEGIC-SIM FIXTURE REGISTRY (SIM1-P clause 9, added 2026-09-05). The sim counterpart of
# TACT_SCENARIOS, and the thing whose ABSENCE the clause names: 137 saves are indexed in
# save_index.json and until now NOTHING said which of them a gate runs, so "what does the sim gate
# exercise" had no mechanical answer. tools/coverage.py --domain sim reads this list and reports the
# per-file union with the gaps named.
#
# DELIBERATELY A FLOOR, NOT A TARGET (user, 2026-09-05). Sim is 530 rows against tact's 98; the point
# is that the number and the gap list are COMPUTED and PRINTED, never that the set is complete. Add a
# scenario when a run proves it reaches something the registered ones do not -- and only after
# actually running it, because a registered scenario nobody has run is worse than an absent one.
#
# NO `extra_ini` ANY MORE (fork F2E). Both rows used to carry sim_cov_promote.ini, a one-key fragment
# STATING `[promote] sim_resid=1` -- the compiled-in default, restated so a reader of the scenario
# could see that the 32 session-entry bodies were in the measurement. That statement was worth making
# while the key existed and while the all-AI landing detour made the two configs mutually exclusive
# (the install came back 30/31 and printed PARTIAL -- an intra-slice DIRECT call). C10 fixed THAT in code --
# entry_claim::rebind plus mh::sim::set_land_players_observer cover the intra-slice edge -- and F2E
# deleted the key, so the fragment would now REFUSE the run as a stale config rather than state
# anything. What selects these bodies is `[config] mode`, whose default is brokered; the scenarios say
# nothing and get them.
SIM_SCENARIOS = [
    {
        "name": "soak_default",
        "desc": "8-way all-AI soak from the default start -- the broad strategic sweep, and the one "
        "scenario that rewards DEPTH: 185 rows at 600 steps, 281 at 3000, 388 at 10000",
        # 10000 AND 1000%: BOTH ARE MEASURED, NOT PICKED (depth curve, 2026-09-05, user's call). The
        # union over this registry goes 303 -> 353 -> 401 -> 409 rows at 600 / 3000 / 10000 / 30000
        # steps, so 30000 costs three times what 10000 does and buys EIGHT rows. 10000 is where the
        # curve flattens. The speed is part of the measurement rather than a convenience: it sets how
        # much GAME time a step covers, so a run at the default speed does not reproduce these figures
        # -- and 1000% is the measured clean band for an 8-way match (8000% stalls it at step 1475).
        # Re-derive with `coverage.py --milestones 600,3000,10000,30000` before changing either.
        "steps": 10000,
        "speed": 1000,
    },
    {
        "name": "soak_saved",
        "desc": "developed save loaded at step 120 -- reaches the economy/production/research "
        "branches an empty default start cannot, and reaches them AT ONCE: 294 rows at 600 steps and "
        "296 at 30000, so depth buys it nothing",
        # 1500, RAISED FROM 600 ON 2026-09-05 (user's call) -- and the reason is no longer about row
        # count. On COVERAGE this scenario SATURATES almost immediately: 294 rows at 600 steps, +2 over
        # the following 29,400, because the save is already developed, so it arrives at its states
        # rather than growing into them. By that measure 600 was right and depth bought two rows.
        #
        # What changed is what this fixture is FOR. It now also drives the promoted-vs-original A/B in
        # the gate, which is not a breadth measure but a TRAJECTORY comparison: its value is how many
        # steps of divergence-free simulation it can attest, and there depth is the whole point -- a
        # 600-step golden cannot see a divergence that first appears at step 900. SIM-DEEP-DIV's own
        # divergence sat at step 3215, which is why migration_ab's default budget is 8000.
        #
        # `load_at` is 120, so a budget BELOW that never loads the save at all and silently measures the
        # default start twice (seen at a 60-step rung: both scenarios reported an identical 163 rows).
        # Do not lower this below ~300.
        #
        # THE COMMITTED COVERAGE BASELINE WAS RECORDED AT 600 AND HAS NOT BEEN RE-RECORDED (user's
        # call: "let recorded coverage baseline be in this state"). It is therefore a record of a
        # DIFFERENT fixture than this row now describes, and `coverage.py --baseline --domain sim
        # --check` will report gains against it. That is safe only because the coverage arm was taken
        # OUT of the gate in the same change -- re-record before ever putting it back, or the first
        # thing it says will be about the step budget rather than about the code.
        "steps": 1500,
        "speed": 1000,
        "save": "ayy30",
        "load_at": 120,
        # IT IS NOT AN ALL-AI SCENARIO AFTER THE LOAD. The ALLAI conversion happens at LANDING and is
        # verified in the log (`; ALLAI converted 1 ... VERIFY slot=0 ... OK`, mask [11111111] at the
        # step-60 probe); then `; [save] LOADGAME step=120 name=ayy30 -> rc=1` REPLACES the world, and
        # the save's own roster is 2 players with one AI (`AIPROBE step=120 nplayers=2
        # ai_enabled=[01000000]`). So "every seat thinks" is false by step 120 by construction. The
        # scenario is registered for COVERAGE BREADTH, not for all-AI-ness, and a quieter 2-player
        # developed world still reaches 71 more rows than a busy empty 8-way one -- the same lever
        # the AI shadow batches' own save-coverage fragment used to document (deleted at F2E).
        #
        # THIS USED TO CARRY `soak_shape_fails: True`, WHICH NOTHING EVER READ (2026-09-05). Its only
        # effect was to make a reader believe the case was handled while the run reported `soak: FAIL`
        # on all three passes and coverage.py pinned a baseline from them anyway. The shape check now
        # DERIVES the premise from the run's own LOADGAME line and judges a save-loaded soak by the
        # weaker, correct one, so this scenario PASSES on a true assertion instead of failing on the
        # wrong one. Nothing to declare here any more -- see soak_shape_report's premise block.
    },
]

# THE SAME SOAKS, ATTRIBUTED AGAINST THE sim_resid LEDGER. sim_resid is its own migration domain (32
# session-entry bodies: new_game_init, spawn_ai_base, map_FillDefaults, session_begin_multi ...), and
# its rows are NOT in sim's 530 -- a fact worth stating because it was assumed the other way for an
# afternoon. They run in exactly these scenarios, so the registry is an alias rather than a second set;
# `coverage.py --baseline --domain sim_resid` then measures them against their own census.
# Measured 2026-09-05, first time ever: 18 of 32 execute in a 600-step all-AI soak.
SIM_RESID_SCENARIOS = SIM_SCENARIOS

# SPCAMP: the game-start journals, TACT_SCENARIOS' sibling for the UI-REC arm. Named rather than
# hand-typed because SPCAMP-AB's done_when (c) asks for exactly that -- a fixture reachable only by a
# path in someone's shell history is not a standing check. `--ui-replay` / `--ui-equiv` accept a name
# from here or a path.
#
# NOT IN THE DEFAULT SUITE, deliberately (SPCAMP-AB's scope says so): an arm of this costs minutes of
# rig time, and the honest answer may be that only the 200-step rung ever belongs in a gate.
UIREC_SCENARIOS = [
    {
        "name": "spcamp_newgame",
        "journal": "tools/uiscripts/journals/spcamp-newgame.journal",
        "desc": "human session: menu -> NEW GAME -> race -> campaign start -> play -> planet "
        "transition -> second planet (36,068 records / 33,819 steps)",
        # RUNGS MEASURED 2026-09-07 (replay vs replay, both arms, `state`):
        #     200   PASS      2000  PASS      10000 FAIL, first mismatch at step 3264
        # The 10000 failure is NOT the landing (SPCAMP-SEED, fixed) and NOT a barrier: both arms
        # forced the same one. It is the in-game half of the cadence problem -- the journal is a
        # PRESENT schedule and the world runs on SIM STEPS, so an in-game click drifts by a step.
        # `TJ P` step barriers were added for it and this journal predates them, so proving the fix
        # needs a re-recording. See SPCAMP-REC.
        "rungs": (200, 2000, 10000),
        "proven_rung": 2000,
    },
    {
        "name": "spcamp_mission",
        "journal": "tools/uiscripts/journals/spcamp-mission.journal",
        "desc": "human session: menu -> NEW GAME -> race -> campaign start -> enemy eliminated -> "
        "planet transition -> second planet (28,574 records / 24,873 steps)",
        # THE RE-RECORDING spcamp_newgame's note asks for, and the first journal of the current
        # vintage: all 15 screen barriers carry the CONTENT id (6-field `S`, SPCAMP-SYNC) and it
        # holds 2,878 `TJ P` step barriers, the last at step 24,873 -- so the in-game half is
        # synchronised on the sim's own clock for the whole session, including past the transition.
        # The transition is in the recording rather than assumed: `LAND SESSION_MODE=1 (CAMPAIGN)`
        # appears twice, at planet=0 and again at planet=2.
        "rungs": (200, 2000, 10000, 24000),
        "proven_rung": 0,
    },
    {
        "name": "spcamp_solo",
        # COVERAGE walks this one and only this one (`coverage.py --baseline --domain uirec`). The
        # two journals above predate the `TJ P` step barriers -- spcamp_newgame's own note asks for
        # the re-recording that spcamp_solo IS -- so they do not replay to the end, and a coverage
        # figure off a run that stops at 3,264 of 33,819 steps measures the prefix, not the session.
        # A key rather than a hardcoded name in coverage.py: which fixture is measurable is a
        # property of the fixture.
        "coverage": True,
        "journal": "tools/uiscripts/journals/spcamp-solo.journal",
        "oracle": "tools/uiscripts/journals/spcamp-solo.oracle.gz",
        "desc": "human session recorded ALL-ORIGINAL: menu -> NEW GAME -> race -> campaign start -> "
        "play (incl. 3 squad merges by shift+click) -> planet transition -> second planet "
        "(37,383 records / 30,043 steps)",
        # THE A/B/C FIXTURE, and the first one with a C arm. Recorded 2026-09-07 on the UNPROMOTED
        # build, deliberately: a recording made on ship would bake any promotion bug into the
        # reference, and this scenario exists precisely because the user could not reproduce a squad
        # merge with the promotions live. Its `.oracle.gz` is that run's own per-step trajectory
        # (state + the four sim-integrated clock regions), so B-vs-C asks "does the replayer
        # reproduce the human" and A-vs-B asks "do our bodies reproduce the game's".
        #
        # THE RUNGS ARE RETIRED here (user, 2026-09-07): the claim is the whole journal. A prefix
        # cannot reach the planet transition, and the orders after it are the coverage nothing else
        # in the tree has.
        #
        # `orders` is the RECORDING's own order-code histogram, committed so the gate has a
        # non-vacuity reference rather than only an arm-vs-arm one. Code 33 is the squad merge: it
        # read 11 in the recording and 13 on ship until llm_strat_unit_state_squad_merge was fixed,
        # and it read 0 IN BOTH ARMS -- passing -- before the keystate channel was replayed.
        "orders": {
            "0A": 4954,
            "0B": 363,
            "1A": 19,
            "1C": 506,
            "33": 11,
            "34": 139,
            "35": 126,
            "36": 32,
            "6D": 15,
            "7E": 3,
            "7F": 3,
            "D4": 1,
            "DB": 1,
            "DD": 9,
            "E6": 11,
            "EA": 10,
            "EC": 2,
        },
        # The recording's orders AFTER its last campaign landing (step 29,308) -- the SECOND PLANET,
        # committed separately because the totals above cannot see it: ~30,000 pre-transition steps
        # dominate the histogram, so a completely dead second planet would move them by about 2%.
        "orders_post": {"0B": 351, "35": 1},
    },
    {
        "name": "tutorial_solo",
        "journal": "tools/uiscripts/journals/tutorial-solo.journal",
        "oracle": "tools/uiscripts/journals/tutorial-solo.oracle.gz",
        # THE FIXTURE'S OWN LANDING SHAPE, not the spcamp default. `start_tutorial` stamps the
        # injected planet slot 31 and boots through `session_begin_multi`, which takes the LOCAL
        # branch on a host count of 0 -- so this lands SESSION_MODE=2 once, where every spcamp
        # journal lands SESSION_MODE=1 twice (the second is the planet transition). Both verdict
        # clauses used to hardcode the spcamp shape and failed this fixture for being itself.
        "land_mode": "2",
        "landings": 1,
        "desc": "human session recorded ALL-ORIGINAL: menu -> TUTORIAL -> the full scripted "
        "tutorial played to the end (13,053 records / 18,317 steps)",
        # THE LIFT-RESID FIXTURE, recorded 2026-09-09 by the user on the UNPROMOTED build. It exists
        # because `llm_game_start_tutorial` + `llm_tutorial_step_driver` are EXPORT_REPLACE-promoted
        # -- live in every hosted run -- and had ZERO coverage: no UI scenario entered the tutorial,
        # no selftest suite mentions it, and neither body has a shadow site, so the whole gate went
        # green on whatever they did. LIFT-RESID rewrites both, and this is what makes that
        # measurable.
        #
        # It is NOT a spcamp variant, and the distinction is the point: `LAND SESSION_MODE=2`
        # (single-player skirmish) at `planet=31` -- 0x1f, the injected-map slot `start_tutorial`
        # stamps before calling `session_begin_multi` -- where every spcamp journal lands
        # SESSION_MODE=1 at planet 0. So this is the only fixture that executes the tutorial's own
        # session boot, its hardcoded 1v1 player setup, and the per-frame objective/komenda/panel
        # evaluation in `tutorial_step_driver`. One landing, no planet transition (the tutorial has
        # none) -- that coverage stays spcamp_solo's.
        #
        # ALL-ORIGINAL is load-bearing here, more than for spcamp_solo: the two bodies under test
        # are already promoted, so a ship-config recording would have baked their current behaviour
        # into the reference and A-vs-B would have compared them against themselves. Verified on the
        # recording rather than assumed -- `[promote] sim_step ROLLED BACK`, `tact_frame ROLLED
        # BACK`, lockstep `RUN-CONFIG: REFUSED ... NOT promoted`, and the tombstone instrument armed
        # exactly 2 promoted-fill bodies, both harness pins (`llm_time_get_ticks_ms`,
        # `utils_w_strlen`), i.e. none of ours ran.
        #
        # NO RUNGS: the claim is the whole journal. A prefix stops inside the scripted step list,
        # and the steps are exactly what the rewrite disturbs.
        #
        # `orders` is the RECORDING's own order-code histogram -- the non-vacuity reference. Codes
        # 20/E6/EA and the 49 code-33 squad merges are this fixture's; note 7E reads 48 here against
        # spcamp_solo's 3.
        "orders": {
            "0A": 498,
            "0B": 59,
            "20": 1,
            "33": 49,
            "34": 19,
            "35": 12,
            "36": 13,
            "6D": 10,
            "7E": 48,
            "E6": 4,
            "EA": 5,
        },
    },
]

TACT_SCENARIOS = [
    {
        "name": "poz_stand_exit",
        "journal": "tools/uiscripts/journals/poz-stand-exit.journal",
        "desc": "POZ11, 5,335-frame human session -- STAND/KNEEL + 88 attacks, exits by the building route",
        # THE ONE THAT COVERS STAND. poz1_combat has five KNEELs and ZERO STANDs, which is why the
        # differential gate passed with the real G115 stand_tick inversion live. Recorded 2026-09-04
        # specifically to close that hole; `journal_coverage.py` is what makes the difference visible
        # instead of assumed.
        "arms": ("verify", "replay"),
    },
    {
        "name": "poz1_combat",
        "journal": "tools/uiscripts/journals/poz1-combat.journal",
        "desc": "POZ1L, 4.4 min human session -- 14,106 input records, 211 orders, 10 units lost",
        # `equiv` is the GATE arm and runs in the default suite; verify/replay stay available under
        # --tact-suite as diagnostics. See run_tact_equiv for why the differential is the sound one.
        "arms": ("verify", "replay"),
    },
    {
        "name": "poz1_quiet",
        "journal": "tools/uiscripts/journals/poz1-quiet-patrol.journal",
        "desc": "POZ1L, 3,518-frame human session -- patrol only, no shot fired, ZERO casualties",
        # THE QUIET ARM, and the equivalence clause asks for it BY NAME: "a promoted body that only
        # diverges after damage is invisible to a quiet mission". Both other journals engage (10 and
        # 1 losses), so until this one the quiet side rested entirely on the scripted
        # --tact-determinism arm, and that arm supplies no player input at all (G117).
        # `units_lost_total=0`, `owner0 8->8(-0) owner1 16->16(-0)` -- read off the census, not
        # claimed from how the session felt to play.
        #
        # IT IS ALSO THE FIRST JOURNAL RECORDED WITH THE PROMOTION LIVE, and that shows in the
        # coverage report as `seam 0 / watch N` on EVERY op: the entry-hooked recorder saw ZERO
        # orders across the whole session while the hook-free queue watch saw 114. Recorded a day
        # earlier this file would have contained no orders at all and been useless -- which is
        # exactly what G120 predicts and the concrete reason the watch had to exist before more
        # fixtures could be collected.
        #
        # It also carries the FIRST op-6 (FACE/TURN) coverage in the tree, 94 records. Not effort:
        # llm_tact_frame @0x42a403 issues it continuously for selected units, the E recorder drops
        # it deliberately on volume, and the queue watch picks it up at the immediate FACE record.
        #
        # ITS TEETH, MEASURED IN BOTH DIRECTIONS RATHER THAN ASSUMED -- registering a gate fixture
        # whose only demonstrated property is that it passes is the exact trap poz1_combat set:
        #   * The seeded G115 stand_tick inversion PASSES here. This journal has 5 KNEELs and no
        #     STAND, so it cannot defend that body -- `journal_coverage.py` predicts it from the op
        #     list, and this is the second fixture to prove the prediction right. Do NOT read a
        #     green run on this journal as evidence about the stand path.
        #   * A one-tick-early kneel (`progress <= 0xe`) in a body it DOES reach FAILS it, and only
        #     the ORDER STREAM fires: an op-6 record shifts from frame 317 to 320, 1 missing /
        #     1 extra of 114. The RNG series and the casualty census both stay green. On a quiet
        #     mission the queue watch is the FINEST-GRAINED of the three verdicts, which is the
        #     substantive argument for this fixture existing at all.
        "arms": ("replay",),
    },
    {
        "name": "poz3_teleport",
        "journal": "tools/uiscripts/journals/poz3-teleport.journal",
        "desc": "POZ3L, 8,882-frame human session -- the ONLY mission with teleports; 59-unit roster",
        # THE TELEPORT FIXTURE, and the only one possible: POZ3 is the sole mission in the game whose
        # script contains `TELE` at all (counted across all six POZ*.DAT: POZ1 0, POZ2 0, POZ3 20).
        # It is also the only one with `WALK`.
        #
        # THREE INSTRUMENT DEFECTS had to be fixed before this journal could reach any of that, all
        # found by asking why a POZ3 recording contained zero teleports:
        #   1. The queue watch was OWNER-0 ONLY (inherited from the direct-write watch, where the
        #      restriction is necessary because those records are replayed). Every TELE/WALK/RUN in
        #      POZ3 belongs to a `; obcy` (alien) unit -- `; nasi` (ours) has ZERO of any of them --
        #      so the whole population was structurally unrecordable.
        #   2. The watch PRIMED on frame 1, adopting the mission's entire scripted command load as
        #      baseline. llm_tact_mission_load enqueues before the first llm_tact_frame.
        #   3. --tact-equiv ignored the journal's own provenance header and provisioned from the CLI
        #      defaults, so this journal replayed against the DEFAULT mission -- 24 units against the
        #      recorded 59 -- and PASSED, because both arms were equally wrong. That one is guarded
        #      mechanically now (tact_journal_roster_guard).
        #
        # After all three: 23,112 orders observed identically by both arms across 55 units, and the
        # op histogram finally contains the ops nothing else in the tree reaches --
        # 0x0a TELEPORT x137, 0x1c walk-marker x98, 0x1e run-marker x423, plus 0x07 DIRECT and
        # 0x08 WAIT. `lost=4` matches the recording's own census exactly.
        #
        # NOTE ITS OWN Q RECORDS UNDER-REPORT: the file was recorded before fixes 1 and 2, so
        # journal_coverage.py reads 5 of 13 ops from it. The gate is unaffected -- --tact-equiv
        # compares the REPLAY's run logs, not the file -- but judge what this fixture reaches from a
        # replay, not from the committed Q stream.
        "arms": ("replay",),
    },
]

TESTS = [
    {
        "name": "tutorial_enter",
        "kind": "solo",
        "script": "tutorial_enter.txt",
        # PINNED: t3 is an IN-GAME capture, so its size follows the persisted setup.dat resolution.
        "extra_ini": "tools/uiscripts/ini/video_1024.ini",
        # THE REGRESSION GUARD for the 2026-09-09 async-callback-return bug. Nothing in this suite
        # entered the tutorial before, so `llm_game_start_tutorial` + `llm_tutorial_step_driver`
        # shipped EXPORT_REPLACE-promoted with zero coverage -- and a wrong return type left the
        # menu async-callback pump uninstalling the step driver after its first tick, which no gate
        # could see. t3 is the frame that says the driver is still installed and ticking, and the
        # run TIMES OUT if it stops (`gameclock 6000` is never satisfied) -- so the completion is
        # still the primary verdict and the pixels the secondary one.
        #
        # THE CLOCK IS PINNED AND THE TOL IS 0.0 (2026-09-10). The previous note said the opposite
        # ("the tolerance stays default on purpose... ~0.7% run-to-run") and predicted its own
        # failure: "if t3's diff ever climbs toward 2%, tighten the gate by pinning what varies --
        # do not raise the tol". Inside run_gate.py's 7-unit concurrency it climbed to 2.085% /
        # 2.737% and the unit went red while a standalone re-run passed. WHAT VARIES, named: the
        # PIONEER's rotating sprite plus its selection ellipse and ground shadow, top-left of the
        # map view -- the only pixels that move at all. The phase is the SIM STEP COUNT at the
        # capture, not wall time: `gameclock N` fires on the first present after the clock crosses
        # N, and llm_strat_time_tick derives its delta from GetCurrentTime, so a slow frame carries
        # several 0.1 s sim steps and the capture overshoots by a load-dependent number of steps.
        # That is why the diffs came in a DISCRETE set (0.000 / 0.708 / 0.928 / 1.382 / 2.085 /
        # 2.737 / 2.985 % over 42 archived runs) rather than a continuum -- one value per sprite
        # phase, which is also why two gate runs agreed to three decimals and it read as systematic
        # rather than as jitter.
        #
        # THE PIN is [harness] pin_wallclock=1: GetCurrentTime is whole-body-replaced by a counter
        # advancing pin_clock_dt_us once per present, so the game clock becomes a function of the
        # FRAME COUNT since session start and the capture lands on the same sim step at any frame
        # rate. Same instrument tact_panel uses for the same reason (its unit sprites' phase follows
        # a real clock), and the same result: measured byte-identical captures across a 12-way CPU
        # load that stretched the run 12 s -> 33 s, so "tol": 0.0 is measured, not hoped for.
        # fixed_step is deliberately NOT set -- one step per frame would make a +-1 present jitter
        # worth a whole 0.1 s sim step, where the pin alone puts six presents inside each step.
        # synth_move=0 IS LOAD-BEARING: mp_run's harness template arms the D6 moving-unit workload
        # at step 60 with a seed drawn per invocation, and this capture's gate IS step 60 -- see
        # an inherited runner DEFAULT that randomised a run which looked pinned.
        # region_hash_step=0 only spares the per-step hash the capture has no use for.
        #
        # WHAT tol 0.0 BOUGHT, measured by the negative arm: mutating llm_game_start_tutorial to
        # stamp the hint widget with tutorial_steps[1].body instead of [0] -- i.e. the wrong step's
        # instructions on screen, exactly this test's regression class -- moves t3 by 1.857%. The
        # OLD default 2% tol would have passed that regression silently. (Note also that the hint
        # text at t3 is written by llm_game_start_tutorial, NOT by the step driver: the driver's
        # label store runs only on a step ADVANCE, which has not happened by gameclock 6000. The
        # earlier note claiming otherwise was wrong -- mutating the driver's label store leaves t3
        # byte-identical.)
        "harness_extra": "pin_wallclock=1;pin_clock_dt_us=16667;pin_clock_base_s=1000;synth_move=0;region_hash_step=0",
        "tol": 0.0,
        "desc": "TUTORIAL from the main menu -> welcome dialog -> Start -> the step driver ticking",
    },
    {
        "name": "menu_walk",
        "kind": "solo",
        "script": "mp_menu_walk.txt",
        # THE SPINE-CROSSING ARM (fork F4D), carried by this scenario because it is the plainest
        # bound lane in the registry: a default lane, both satellites deployed, no ini of its own.
        #
        # WHAT IT ASSERTS THAT NOTHING ELSE DOES. Every other gate in this project reads the same on
        # a brokered lane that bound libmh.dll and on one that bound it and never called it: the
        # bind line is identical, the boot completes, the menu renders, the capture matches. That is
        # the RECORDED MISS this arm closes -- LIB-SPINE-API proved the spine crossing live ONCE
        # (13e7e4ec, 86 routed rows through the brokered entry) and nothing ever asserted it again.
        # `--libmh --expect bound` requires the bind verdict AND crossings > 0 AND absent-calls == 0.
        #
        # Measured on the two live boots that landed it: a bound lane reads crossings=13465
        # absent-calls=0, a config-(1) lane reads 0/796. The counters are therefore measuring
        # something -- which is the half a single-arm check could never show.
        #
        # AND THE INSTRUMENT'S BIND (fork F4E), on the same lane and for a quieter version of the
        # same reason. A missing mh_harness.dll changes NOTHING a pixel can see -- the harness only
        # observes and it ships disarmed -- so this is the only assertion in the registry that a
        # plain lane still has one. `--module mh_harness --expect bound` requires the bind verdict
        # AND `call-through ok`: resolving twelve exports is not the same as being able to call one,
        # and the probe behind that clause (MH_Harness_WantsPresentTick, pure, known answer 0 at
        # DLL_PROCESS_ATTACH) is a real call through the contract rather than into the module entry.
        "post_check": [
            ["tools/check_module_bind.py", "--libmh", "--expect", "bound"],
            ["tools/check_module_bind.py", "--module", "mh_harness", "--expect", "bound"],
        ],
        "desc": "main menu -> NETWORK GAME -> ... -> host lobby (single-peer, host-only on the first VM)",
    },
    {
        "name": "libmh_absent",
        "kind": "solo",
        "script": "libmh_absent.txt",
        # CONFIGURATION (1) AS A REGISTERED SCENARIO (fork F4D). The sibling of module_absent one
        # satellite over, and the asymmetry between them is the point: a missing mh_net.dll is a
        # DEGRADATION the player can see, a missing libmh.dll is a shipped CONFIGURATION nobody can
        # see -- the game simply runs the original binary's own bodies (ruling Q10, ratified).
        #
        # The absence is REAL: make_lane deploys libmh.dll into every lane since F4D, and this entry
        # is the one asking for the lane that has none.
        "omit_satellite": ["libmh.dll"],
        # BOTH satellites are asserted, and the second row is not decoration. Without it this
        # scenario would pass on a lane that lost the TRANSPORT too -- which is the same lane
        # module_absent is, so the suite would be running that test twice and config (1) never.
        "post_check": [
            ["tools/check_module_bind.py", "--libmh", "--expect", "absent"],
            ["tools/check_module_bind.py", "--expect", "bound"],
        ],
        # tol 0.0: the stock main menu, drawn by original code in every configuration. Anything but
        # pixel-identity here is a real change, and at the 2% default a completely different menu
        # would pass.
        "tol": 0.0,
        "desc": "F4D: boot with libmh.dll MISSING -- configuration (1), the menu renders, nothing crosses",
    },
    {
        "name": "harness_absent",
        "kind": "solo",
        "script": "harness_absent.txt",
        # THE UNINSTRUMENTED BOOT AS A REGISTERED SCENARIO (fork F4E). The third and quietest of the
        # absent arms: a missing mh_net.dll is a degradation, a missing libmh.dll is configuration
        # (1), and a missing mh_harness.dll is NOTHING a player or a pixel can see -- the harness
        # only observes, and it ships disarmed. Which is precisely why it needs an entry: a lane that
        # lost this file would keep passing every other scenario in this registry while
        # `--determinism` had no log to read.
        "omit_satellite": ["mh_harness.dll"],
        # ALL THREE SATELLITES ARE ASSERTED, and the last two rows are not decoration: without them
        # this scenario would pass just as green on a lane that had lost the transport or the spine
        # instead, i.e. it would be running module_absent or libmh_absent over again and the
        # uninstrumented boot never.
        "post_check": [
            ["tools/check_module_bind.py", "--module", "mh_harness", "--expect", "absent"],
            ["tools/check_module_bind.py", "--expect", "bound"],
            ["tools/check_module_bind.py", "--module", "libmh", "--expect", "bound"],
        ],
        # tol 0.0: the stock main menu, drawn by original code in every configuration. Anything but
        # pixel-identity here is a real change, and at the 2% default a completely different menu
        # would pass.
        "tol": 0.0,
        "desc": "F4E: boot with mh_harness.dll MISSING -- uninstrumented, the menu renders unchanged",
    },
    {
        "name": "module_absent",
        "kind": "solo",
        "script": "module_absent.txt",
        # THE MISSING-SATELLITE BOOT. Registered at F4A against the spike satellite; RETARGETED at
        # F4B onto mh_net.dll -- the real transport -- when the spike died with its knob (F4A ruling
        # (a): a real satellite is unconditional and absence IS the configuration).
        #
        # RETARGETED RATHER THAN FOLDED INTO no_net_boot, and the distinction is the point of having
        # both. They reach the same degraded UI by different routes: no_net_boot sets
        # `[net] module=none`, so mh.dll never LOOKS for the module; this lane has `module=auto` and
        # the file genuinely is not on disk, so LoadLibrary runs and fails. One proves the CONFIGURED
        # degradation and its visible half (the browser notice, gated in the script); this one proves
        # the LOADER one -- that a shipped install missing a file boots instead of dying at
        # 0xC0000135, which is the measured fate of the static-import alternative (docs/dll-split.md).
        # Folding them would delete the only test of the path a player can actually create.
        #
        # The absence is REAL, not a key: make_lane deploys mh_net.dll into every lane by default
        # since F4B, and `--omit-satellite` is this entry asking for the one lane that has none.
        "omit_satellite": ["mh_net.dll"],
        # THE ASSERTION IS THE post_check, NOT THE PIXELS. A capture proves the process reached its
        # menu; it cannot prove the bind was ever ATTEMPTED. check_module_bind reads this lane's own
        # mh_net.log and requires the `absent` outcome -- specifically NOT `declined`, which is what
        # a lane that quietly stopped deploying satellites at all would produce -- AND the arm's end
        # marker after it, and REFUSES a log with no `[modules]` line.
        "post_check": ["tools/check_module_bind.py", "--expect", "absent"],
        # tol 0.0: the frame is the stock main menu and nothing in this configuration draws on it,
        # so anything but pixel-identity is a real change.
        "tol": 0.0,
        # THE NEGATIVE ARMS, MEASURED rather than left to be re-derived -- and the first is the
        # reason the post_check exists at all:
        #   * the same lane WITH mh_net.dll (i.e. every other lane): the capture still reads 0.000%,
        #     so the pixels cannot tell the bound configuration from the absent one.
        #     check_module_bind FAILS it: "expected `absent`, got `bound`".
        #   * a lane with `[net] module=none` (no_net_boot's shape): outcome `declined`, and
        #     --expect absent FAILS it -- a run that never looked for the file is a different
        #     experiment, not a weaker pass.
        #   * a boot that died before the arm finished: no end marker -> REFUSED (selftest case).
        # The --selftest keeps all of them honest offline, in lint_repo.
        "desc": "F4B: boot with mh_net.dll MISSING -- menu renders, the bind refuses loudly, the arm finishes",
    },
    {
        "name": "no_net_boot",
        "kind": "solo",
        "script": "no_net_boot.txt",
        # fork F3F. `module=none` is the OTHER axis from `[net] enable`: enable=0 declines a
        # transport that is here, module=none says there is none -- the shape F4 produces when
        # mh_net.dll is missing. Until this entry existed no no-module configuration had ever
        # booted, so every graceful-degradation claim about one was a prediction.
        #
        # It is the only suite entry that proves ruling Q2's visible half: the NETWORK GAME button
        # arms anyway, and the browser carries a standing "No network module" line instead of an
        # unexplained empty list. The script gates the capture on `present No network module`, so a
        # regression that stops arming the notice TIMES OUT rather than capturing a panel the
        # pixel-diff might forgive.
        "net_extra": "module=none",
        # tol 0.0 IS MEASURED, not hoped for: three repeat runs read 0.000% on both captures with
        # the IP band masked. It has to be tight -- the notice is a single line of text on a 640x480
        # frame, far under the 2% default, so at the default it could render the WRONG SENTENCE and
        # still pass. (The notice VANISHING is caught earlier and harder, by the script's own
        # `present` gate.)
        "tol": 0.0,
        # NEGATIVE ARM, run by hand and recorded here rather than left to be re-derived: the same
        # script on a lane WITHOUT --net-extra module=none TIMES OUT at step 7, `present No network
        # module`, after 28 s -- so the assertion is non-vacuous and this test cannot pass a build
        # that stopped arming the notice.
        "desc": "F3F: boot with NO network module -- menu reached, NETWORK GAME armed, browser explains its empty list",
    },
    {
        "name": "ip_cancel",
        "kind": "solo",
        "script": "mp_ip_cancel.txt",
        "desc": "U38: browser -> Internet server -> Cancel comes back to a LIVE local browser (the de-duplicated slide-in)",
    },
    {
        "name": "match_launch",
        "kind": "multi",
        "host": "mp_host_start.txt",
        "clients": ["mp_client_start.txt"],
        # PINNED: m3_launched is an IN-GAME capture, so its size follows the persisted setup.dat
        # resolution. Unpinned it silently inherited 1024x768, and any run that left a different mode on
        # a peer broke it -- e.g. the D13b picker tests persist an index (4) that the stock 3-entry
        # engine cannot interpret, so the host fell back to 640x480 while the untouched client stayed at
        # 1024x768. Pin every test whose captures include an in-game frame.
        "extra_ini": "tools/uiscripts/ini/video_1024.ini",
        "desc": "2-peer: host waits for the client, clicks Start; BOTH enter the live game",
    },
    {
        "name": "mixed_race",
        "kind": "multi",
        "host": "mp_host_race.txt",
        "clients": ["mp_client_race.txt"],
        # PINNED for the same reason as match_launch: r3_launched is an in-game capture, so its size
        # follows the persisted setup.dat resolution.
        "extra_ini": "tools/uiscripts/ini/video_1024.ini",
        # U28/D19: the ONLY scenario where the two players have DIFFERENT races. Every other 2-peer
        # test leaves both on Human, so the client's 0x0c race push, the host applying it, and the
        # FLAG_START slot payload are all unexercised -- which is why the 2026-08-28 desync had no
        # test that could have caught it. The host's `race 1 2` makes propagation the gate: if that
        # path regresses this test TIMES OUT instead of launching a match the peers disagree about.
        "desc": "U28: client picks Alien, host stays Human -- the race reaches the host before Start",
    },
    {
        "name": "race_inflight",
        "kind": "multi",
        "host": "mp_host_inflight.txt",
        "clients": ["mp_client_inflight.txt"],
        "extra_ini": "tools/uiscripts/ini/video_1024.ini",
        # U28 (a)/(c): the client's own slot is poked to Alien WITHOUT a push, so the host still has
        # Human at the Start click -- the exact end-state of the 2026-08-28 lost in-flight edit, made
        # deterministic. The client must enter with the HOST's value. The negative arm is the same
        # scenario with the host on tools/uiscripts/ini/u28_off.ini ([net] start_slots=0), which sends
        # the legacy bare FLAG_START and must DESYNC at step 1 -- run it by hand, it is not in the
        # suite because a permanently-red test is not a gate.
        "desc": "U28: client's local slot disagrees with the host at Start -- the host's array wins",
    },
    {
        "name": "s7_occ_cap",
        "kind": "multi",
        "host": "mp_host_ai_closed.txt",
        "clients": ["mp_client_browse_s7.txt"],
        "desc": "S7: host = 1 human + 1 AI + 1 closed slot -> client browser row reads '2/7'",
    },
    {
        "name": "ai_closed_start",
        "kind": "solo",
        "script": "mp_ai_closed_start.txt",
        # F3E-COV. Clicking Start out of a lobby that has BOTH an AI slot and a CLOSED one -- the
        # combination no registered scenario performed before this entry. s7_occ_cap's host reaches
        # the same lobby STATE and stops there (it never clicks Start, so the clear loop is never
        # entered); res_hud clicks Start from an AI lobby but never CLOSES a slot. The fixup that
        # needs the combination is the 2026-07-11 peer_clear hang fix: the closed slot's AI-add
        # increment is still on the peer-table count when Start's `while (0 < count)` loop runs, and
        # nothing decrements it.
        #
        # IT DOES NOT COVER THE N2 GUARD'S REFUSAL BRANCH, though the lobby state is the one N2 was
        # written for -- a [trace] run measured llm_lobby_peer_slot_remove as never called on this
        # path at all (see the script header). What it pins is the
        # OBSERVABLE N2 protects: `occ 1` after the close says the host did not remove itself.
        #
        # THE FAILURE MODE IS A WAIT TIMEOUT, NOT A PIXEL DIFF -- `absent Network players` for the
        # hang -- so the captures are the secondary verdict here (see the script). MEASURED negative
        # arm, 2026-09-13: with on_peer_clear() neutered (the count not zeroed) the run dies exactly
        # at `clickl Start` and times out with both lobby captures still 0.000%.
        #
        # tol 0 = exact match, and it is measured: two repeat runs against the fresh baselines came
        # back 0.000% on both captures. It has to be tight -- the states these frames pin (one slot
        # row's state/name columns, the Start button's enabled border) are each a fraction of a
        # percent of the frame, so the default 2% tol is far wider than anything that could change.
        "tol": 0.0,
        "desc": "F3E-COV: AI + CLOSED slot then Start -- the peer_clear hang fix (and the N2 guard's install)",
    },
    {
        "name": "client_join",
        "kind": "multi",
        "host": "mp_host_lobby.txt",
        "clients": ["mp_client_join.txt"],
        "desc": "client discovers a host by IP and joins through to the shared lobby",
    },
    {
        "name": "leave_frees_slot",
        "kind": "multi",
        "host": "mp_host_watch_leave.txt",
        "clients": ["mp_client_leave.txt"],
        # F3E-COV, and it is the HOST half of U12 that is new here. client_join already clicks the
        # client's lobby Cancel and asserts what the CLIENT sees; its host is the capture-only
        # mp_host_lobby.txt fixture, which never re-reads its roster -- so nothing asserted that the
        # LEAVE actually reached a host and freed a slot. link_death frees the slot too, but by the
        # other route (the peer leaves the transport's active-id set), which is why this host checks
        # `peers 1` AFTER `occ <2`: the slot must go while the connection is still up, or the test is
        # measuring link_death's mechanism instead of U12's.
        #
        # IT IS ALSO THE N2 HOST-SLOT0 GUARD'S ONLY LIVE EXERCISER, which is not what anyone
        # expected. The guard sits on llm_lobby_remove_player_slot and refuses player 0; a [trace]
        # run (2026-09-13, funcs=0x4bf59f/0x4bf513/0x4bf65d) measured that the AI-slot close path
        # -- the trigger its own comment describes -- does not call remove_player_slot AT ALL on the
        # current build, while THIS scenario's U12 vacate calls it with player 1. So what is covered
        # here is the guard's PASS-THROUGH branch: a guard that over-blocked would strand every
        # departed player's slot forever. MEASURED negative arm: with the guard's `jz rps_skip`
        # forced to `jmp`, the host logs the U12 LEAVE and then never reaches `occ <2` (timeout).
        # The refusal branch itself (player 0) is recorded as unreachable-and-uncovered in the
        # F3E-COV coverage note.
        #
        # tol 0 = exact match, measured over two repeat runs (0.000% on all three captures). The
        # freed-slot frame's evidence -- one empty slot row, a greyed Start, the "client left the
        # game" announce line -- is well under 1% of the frame, so the default tol could not see it.
        "tol": 0.0,
        "desc": "U12: a client's plain lobby Cancel sends LEAVE -- the host frees its slot with the peer still connected",
    },
    {
        "name": "link_death",
        "kind": "multi",
        "host": "mp_host_linkdeath.txt",
        "clients": ["mp_client_linkdeath.txt"],
        "shim": True,
        "shim_delay": 100,  # ONE-WAY -> 200 ms rtt, the conditions the bug was reported at
        "shim_timeline": "tools/uiscripts/shim/rlive_link_death.txt",
        # The scripts WAIT -- for a peer to join, then for the link to die. The budget is PER STEP and
        # global to both peers, so the host's `peers 1` step has to outlast the client's whole launch +
        # menu walk, which is the slowest and most variable thing in the run (20000 was not enough once).
        # Frame rate also differs ~5x by screen (~200 fps in menus, ~40 fps idling in the lobby), so this
        # is deliberately generous; the wall-clock cap is --timeout, not this.
        #
        # THIS TEST WAITS ON A WALL-CLOCK TIMELINE, so a FRAME budget cannot express its bound: the shim
        # blackholes at 60 s, and headless buys only ~32 s with 60000 frames (measured 2026-07-28: the
        # client aborted `peers <1` at 60001 frames / 31.985 s, before the event it is testing for even
        # happened). On the VM the same number is 1000 s and was never binding, which is why this only
        # ever failed locally. Set it clear of the timeline at any plausible frame rate (60 s + margin at
        # ~8500 fps) and let --timeout below be the real cap -- for a test whose whole subject is elapsed
        # wall-clock, the wall-clock bound is the honest one.
        "timeout_frames": 1200000,
        # Wall clock: launch + menu walk + join, then the timeline's 60 s to the blackhole and ~10 s for
        # the watchdog. The suite default of 200 s cuts it off mid-experiment.
        "timeout": 900,
        "desc": "R-live: an 8s stall must NOT drop the link, a blackhole must -- client leaves the lobby, host frees the slot and can still Start",
    },
    {
        "name": "host_recreate",
        "kind": "multi",
        "host": "mp_host_recreate.txt",
        "clients": ["mp_client_rejoin.txt"],
        "desc": "U13: host leaves + re-creates a game; client re-discovers + re-joins -> fresh lobby (occ 2)",
    },
    {
        "name": "ip_retry",
        "kind": "multi",
        "host": "mp_host_lobby.txt",
        "clients": ["mp_client_ipretry.txt"],
        "client_dead_ip": machine.DEAD_PEER_IP,
        "desc": "S8: client types a dead IP -> empty browser -> corrects the IP via the MRU in-session -> joins",
    },
    {
        "name": "res_hud",
        "kind": "solo",
        "script": "res_hud.txt",
        "extra_ini": "tools/uiscripts/ini/video_1024.ini",
        # D13: the strategic HUD at display mode 2. Pinned EXPLICITLY via [video] rather than inherited:
        # the mode is a persisted setup.dat preference, so an unpinned in-game baseline silently depends
        # on whatever resolution the test machine last saved. (The older in-game baselines -- e.g.
        # mp_host_start's m3_launched -- are 1024x768 for exactly that inherited reason; see
        # the UI-testing suite.)
        "desc": "D13: menu -> lobby -> launch, strategic HUD rendered at 1024x768 (mode 2)",
    },
    {
        "name": "esc_menu",
        "kind": "solo",
        "script": "esc_menu.txt",
        # Proves the [uitest] `key` action: the ESC/in-game menu is opened from a scancode branch in
        # llm_strat_input_update, so a mouse-only driver could never reach it at all.
        # The mode is PINNED because the ESC menu renders at the GAMEPLAY resolution, which is a
        # persisted setup.dat preference -- unpinned, this passed standalone and failed in the suite
        # purely because res_hud/res_picker had just written a different mode. Same trap as the older
        # in-game baselines silently being 1024x768.
        "extra_ini": "tools/uiscripts/ini/video_640.ini",
        "desc": "keyboard injection: reach the ESC/in-game menu, which no mouse-only walk can open",
    },
    {
        "name": "devchange_guard",
        "kind": "solo",
        "script": "esc_menu.txt",
        # U21. Same walk as esc_menu, plus ONE malformed WM_DEVICECHANGE (wParam=DBT_DEVNODES_CHANGED,
        # lParam=0) posted to the game's own window 6 s in -- the message Windows really broadcasts on
        # any device-node change, and the one llm_wnd_on_devicechange dereferences without a NULL
        # check. Measured 2026-08-27: with [compat] devchange_guard=0 this wedges the game 3 times out
        # of 3 (the script never gets past `clickl Start` while the render loop keeps running, i.e. the
        # "harness STALLED" signature); with the guard on it COMPLETEs in ~9 s, 2 of 2. The registry
        # entry is the ON arm, so a regression in the guard shows up as this test hanging.
        # devchange_probe_off.ini is the negative arm, run by hand -- it is expected to fail.
        "extra_ini": [
            "tools/uiscripts/ini/video_640.ini",
            "tools/uiscripts/ini/devchange_probe_on.ini",
        ],
        "desc": "U21: a malformed WM_DEVICECHANGE (lParam=0) must not wedge the game",
    },
    # res_picker (D13b: 12-entry options list pinned to entry 4, 1280x768) REMOVED from the suite
    # 2026-08-19: chronically unstable. It is the only solo scenario that runs IN-GAME and waits for a
    # game-clock milestone (`gameclock 6000`) before capturing, so its wall time tracks machine speed
    # and it timed out at 201-202s against a 200s budget on the 2026-08-17 machine -- a timeout, not a
    # pixel regression (the game starts: `[promote] time_tick` fires). Per the user it "always was
    # unstable". The extended-resolution-list feature it covered is also exercised statically by
    # res_hud; the script (`tools/uiscripts/res_picker.txt`) + baseline are kept on disk for a manual
    # `tools/ui_test.py` run if that path needs re-checking.
    {
        "name": "debug_overlay",
        "kind": "solo",
        "script": "debug_overlay.txt",
        "extra_ini": "tools/uiscripts/ini/debug_overlay.ini",
        # tol 0 = exact match. The overlay is only ~0.8% of the frame, so the default 2% tol cannot see
        # it disappear; and everything OUTSIDE the overlay rect is pixel-identical run-to-run (measured),
        # so exactness is achievable here rather than merely aspirational.
        "tol": 0.0,
        "desc": "P0 debug overlay renders on 2 different screens (the ONLY test that enables [debug])",
    },
    {
        "name": "pause_hotkey",
        "kind": "solo",
        "script": "pause_hotkey.txt",
        # Pins BOTH the display mode (the capture is in-game, so its size follows the persisted
        # setup.dat preference) and the [pause] scancode, so the script tests the feature rather
        # than whatever the seam's default happens to be that week.
        "extra_ini": "tools/uiscripts/ini/pause.ini",
        # AND THE WALL CLOCK, for the same reason and by the same measurement as tutorial_enter's
        # (read that entry's note for the mechanism). b0/b1 are in-game strategic frames whose only
        # moving pixels are the Pioneer's rotating sprite + selection ellipse, and `gameclock 1500`
        # lands on a sim step whose index follows the frame rate: under run_gate.py's concurrency
        # both captures went red at 2.654% / 2.913% while a standalone re-run gave 0.000%.
        # pin_wallclock=1 makes the game clock a function of the frame count, and the two captures
        # then came back byte-identical across a 12-way CPU load that stretched the run 11 s -> 40 s
        # -- hence "tol": 0.0, measured. synth_move=0 is the same load-bearing guard as there.
        #
        # THE PIN ALSO REPAIRED THE b1 CAPTURE, which was vacuous: before it, b0 and b1 were the
        # SAME FILE byte-for-byte (md5 668dc59f...), i.e. the pair proved nothing about mode 5 at
        # all. The frames now differ by the sprite phase the sim advanced through between them. NB
        # that difference is NOT the banner the script header claims -- llm_strat_draw_floating_
        # messages' mode-5 HUD title does not appear in either frame. Open item.
        #
        # NEGATIVE ARM (2026-09-10): with [pause] key rebound to a scancode the script does not
        # send, the run FAILS on the `gamemode 5` wait (timeout, b1 never captured) while b0 still
        # diffs 0.000% -- so the pin did not turn this into a test that passes whatever happens.
        "harness_extra": "pin_wallclock=1;pin_clock_dt_us=16667;pin_clock_base_s=1000;synth_move=0;region_hash_step=0",
        "tol": 0.0,
        "desc": "D19: Tab enters the orphaned mode-5 big map from the strategic view, any key returns (2 -> 5 -> 2)",
    },
    {
        "name": "pause_mp_gate",
        "kind": "multi",
        "host": "mp_host_pause_gate.txt",
        "clients": ["mp_client_pause_gate.txt"],
        # In-game captures -> the display mode must be pinned (it is a persisted setup.dat preference);
        # the fragment pins the [pause] scancode with it.
        "extra_ini": "tools/uiscripts/ini/pause.ini",
        # D19's SAFETY half, and the only one a single-peer run cannot express: pressing the big-map
        # key inside a lockstep match must do nothing, because mode 5 skips llm_strat_sim_tick and a
        # frozen peer stalls the match. The assertion is that the host's game clock keeps advancing
        # AFTER the keypress, which a mode-5 peer could not do; each peer takes ONE secondary
        # in-game capture on top of that (the "No captures" this line used to claim stopped being
        # true when the scripts grew g1_host_ingame / g1_client_ingame -- a capture-free peer is a
        # FAIL by the runner's rules).
        #
        # NOT WALL-CLOCK-PINNED, AND THAT IS STILL THE RIGHT CALL -- but the reason changed at F5J.
        #
        # WHAT WAS MEASURED AT F4H. Both captures sat on the same Pioneer-sprite phase that
        # tutorial_enter and pause_hotkey were red on, and the host's had already flapped across the
        # 2% tol on one build (1.343 / 1.839 / 1.839 / 2.018%). pin_wallclock was tried on both
        # peers: the HOST capture became near-exact (three runs, worst pair one pixel apart -- the
        # HUD clock dial) and the CLIENT did not converge AT ALL (three pinned runs, two distinct
        # frames 3.5% apart, the ship and its ellipse one step out). pin_wallclock fixes how much
        # game time a PRESENT is worth; a lockstep client's steps do not arrive on its own presents
        # -- they arrive in bursts as the host's horizon is delivered -- so the step count at
        # `gameclock 6000` is set by the wire. No capture-timing pin has purchase on that, and the
        # change was reverted rather than shipped half-working.
        #
        # WHAT F5J DID INSTEAD: both scripts gate their capture on `simstep 600`, a predicate that
        # fires ON the step. It arms the harness's SIM FENCE, which truncates that frame's catch-up
        # burst at step 600 from inside the sim_step hook and then holds the clock there -- so every
        # present from 600 renders the identical sim state on EITHER peer, wire or no wire. The
        # `[harness]` block below is what makes that counter exist (`simstep` REFUSES by name in a
        # lane without one); synth_move=0 keeps the D6 workload's per-run random seed out of a pixel
        # comparison, and region_hash_step=0 keeps the per-step hash lines out of a run nobody
        # analyses. No pin_wallclock: the fence pins the SIM, which is what the frame is made of,
        # and adding a frame-count clock on top would only re-introduce a second cadence to get
        # wrong. Measured result and the resulting tol are on the "tol" line.
        "harness_extra": "synth_move=0;region_hash_step=0",
        # MEASURED over seven consecutive runs (fork F5J): every capture on both peers came back at
        # 0.000% -- fourteen of fourteen -- and two archived runs' raw capture BMPs are md5-equal on
        # BOTH peers, so it is byte identity rather than a diff under a threshold. tol 0.0 is
        # therefore the honest setting, and the 2.018%-against-2.0 shape is gone by construction
        # instead of by margin.
        #
        # THE RED PROOF, because a wait predicate can mask a divergence by waiting for it to pass.
        # It is a committed, re-runnable arm rather than a remembered command line:
        # tools/uiscripts/ini/pause_mp_gate_desync_client.ini, passed as --extra-ini-client, flips
        # the CLIENT's strategic PRNG 400 steps before the fenced capture so its sim leaves the
        # host's. Measured: g1_client_ingame RED at 19.292% (the mothership and its fog shadow
        # somewhere else on the map -- a different world, not a sprite phase out) while the host
        # stays 0.000% and the client's own fence still reports HELD at step 600. So the predicate
        # waits for a STEP, not for agreement. That file carries the full invocation.
        #
        # SECOND NEGATIVE ARM, and it is why `simstep` refuses instead of waiting: run this pair
        # WITHOUT --harness-extra and both peers ABORT at step 19 in 0 frames, naming the missing
        # `[harness] enable=1` and producing no captures. A predicate whose instrument is absent
        # fails as a configuration error, not as a 1500-frame "the sim stalled".
        "tol": 0.0,
        "desc": "D19: the big-map hotkey is inert in a lockstep match (host presses it, the sim keeps ticking)",
    },
    {
        "name": "key_repeat",
        "kind": "solo",
        "script": "key_repeat.txt",
        # tol 0 = exact match. One typed glyph is ~0.02% of the frame, so ANY non-zero tol large
        # enough to absorb jitter would also absorb the character going missing -- and missing
        # characters (over-suppression by the U24 fix) is the entire regression this guards. See the
        # script header for what this test can and cannot see.
        "tol": 0.0,
        "desc": "U24: one keypress in the player-name field types exactly ONE character",
    },
    {
        "name": "tact_panel",
        "kind": "solo",
        "script": "tact_panel.txt",
        # The ONLY scenario that renders a tactical frame. Every other entry is menu, lobby or
        # strategic, so before this one the whole tactical HUD could stop drawing and the suite
        # would stay green -- which is precisely the blind spot LIFT-TACT's sink conversions sit in.
        "extra_ini": "tools/uiscripts/ini/tact_panel.ini",
        # A mission is entered from a played game, not from a menu (see the script header), so this
        # scenario is driven by the DLL's --tactical verb over the polygon's save 11.
        "launch_args": "--tactical 11",
        "deploy_save": "11",
        # THE ARMING IS NO LONGER WHAT MAKES OUR BODY RUN -- BUT IT IS STILL WHAT MAKES THE FRAME
        # REPRODUCIBLE, and those turned out to be the same knob. Until ROOTS-LIVE (2026-09-04) a
        # default run printed `; [promote] tact_frame NOT promoted`, the ORIGINAL body ran, the whole
        # converted tactical cluster was dead code, and a capture could not see it (measured
        # 2026-09-03: with all three converted scopes no-op'd the frame was byte-identical). That
        # reason is gone -- llm_tact_frame now promotes by DIRECT ENTRY INSTALL and ours runs in the
        # shipping configuration whether or not this line is here.
        #
        # DROPPING IT WAS TRIED AND MEASURED, 2026-09-04, and the result is why it stays. The pinned
        # wall clock advances from INSIDE the tactical cadence hook (seams/harness.cpp, "the two
        # cadences never run in the same frame"), so tact_hash_step is not merely the arming knob:
        # remove it and the pin freezes in mode 6 while pin_wallclock=1 still reads as set (measured
        # 1.798% against the pinned baseline). Removing the whole harness_extra instead gives a
        # genuinely unpinned tactical frame, and three repeats of THAT against their own fresh
        # baseline came back 0.004% / 0.004% / 1.800% -- the unit sprites' animation phase follows a
        # real clock (time_GetCurrentTime is read from 20 tactical functions, the tactical-probe work
        # section 9e). A tol-0.0 tactical capture and an unpinned clock are not compatible, and the
        # 1.8% jitter floor is wider than any tactical mutation this frame showed, so a tolerance
        # would not rescue it either.
        #
        # WHAT THAT COSTS, STATED RATHER THAN HIDDEN: this scenario is a PINNED gate, so it is not the
        # tactical root's default-config coverage. That job belongs to the force-armed tombstones over
        # the tact domain, which are exact and terminate on a hit. See the ROOTS-LIVE session report.
        "harness_extra": "tact_hash_step=1;pin_wallclock=1;pin_clock_dt_us=16667;"
        "pin_clock_base_s=1000;pin_rand=1;rand_seed=12345",
        # tol 0 = exact match, and it is MEASURED, not hoped for: two repeat runs after the baseline
        # came back at 0.000% diff. It has to be exact. The two-line active/selected readout this
        # test exists to watch is ~0.1% of the frame, so the default 2% tol is twenty times its whole
        # footprint -- the readout could vanish completely and a defaulted test would still pass,
        # which is the same hole the debug_overlay test was found sitting in on 2026-07-25.
        "tol": 0.0,
        "desc": "LIFT-TACT: tactical mission running -- selection panel + active-count HUD on a real frame",
    },
    {
        "name": "boot_snapshot",
        "kind": "solo",
        "script": "boot_snapshot_capture.txt",
        # LIB-BOOT's capture path, kept from rotting. `boot_snapshot=1` is what ARMS it: the capture
        # fires inside mh.dll off the first present with _G_LLM_GAME_MODE == 3, walks the derived
        # schema, emits mh_boot_snapshot.bin, and -- for the tutorial deferred-parse block -- CALLS
        # the parser and then RESTORES the 7 regions it perturbed, refusing (fatal) if the restore
        # finds a mismatch or if nothing changed at all. So an armed run exercises schema walk +
        # emit + call-and-restore on every build.
        #
        # WHAT THIS ENTRY CAN AND CANNOT SEE, stated rather than assumed. It CAN see the capture
        # path dying or refusing: the script's only wait is `settled`, and a capture that faults or
        # aborts the process never reaches a settled main menu, so the run fails as a timeout /
        # "did not present a frame" rather than as a pixel diff. It CANNOT see a capture that
        # silently emits a WRONG blob -- the frame is a main menu either way, and the registry has
        # no per-test log/blob assertion hook. The blob's own correctness is gated offline by
        # `gen_boot_snapshot.py --check/--selftest` in lint_repo plus boot_snapshot_selftest's
        # mutation + content arms; this entry is the RUN-side half (does the armed path still run
        # at all), not a second copy of them.
        #
        # tol stays at the suite default: the menu frame is the same one menu_walk's first capture
        # covers, and nothing this test watches is a small-footprint feature that a 2% tol could
        # swallow -- the verdict that matters here is completion, not the pixels.
        "harness_extra": "boot_snapshot=1",
        "desc": "LIB-BOOT: the post-cfg snapshot capture path is armed and the process still reaches the menu",
    },
]


def vm_reachable(ip, port=22, timeout=4):
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except OSError:
        return False


# ---- C7: the standard determinism shapes --------------------------------------------------------
# Promotion was not one gate but two runs, because they answered different questions:
#
#   ASYMMETRIC (fixes off)  ours on the host, the ORIGINAL on the client. The only shape that can
#                           catch a deterministic-but-WRONG engine: a symmetric run makes both peers
#                           wrong identically and goes green.
#   SYMMETRIC (ship config) promotion on both peers, fixes at their shipping defaults -- what players
#                           actually run.
#
# ASYMMETRIC WAS REMOVED FROM `--det-standard` ON 2026-09-01 (user's call), because the question it
# asks is not a question about shipped behaviour. It gates OURS against the ORIGINAL netcode, and
# THE ORIGINAL NETCODE IS DEAD: retail mh.exe has no socket layer at all -- its lobby builds and
# CRC-checks packets that are never transmitted ("dead at the wire", the lobby RE) -- and MH's
# working multiplayer IS the restored one, with the injected DLL supplying the transport. The
# original lockstep path executes only because the byte patches animate it. So "does ours match the
# original" is a COMPATIBILITY question, and gating the determinism suite on it was measuring the
# wrong thing: C2's "expires for dead-in-retail seams, never for live-retail-logic ones" reads as a
# live/dead distinction that the netcode does not actually have.
#
# The mechanism is kept for that compat use -- run_det_standard's docstring carries the by-hand
# command, and BOTH ini fragments are required (dead-ends G96).
#
# FORK F2E: BOTH FRAGMENTS ARE NOW ONE `[config] mode` LINE EACH, and the pair below is the SAME two
# files the SP oracle uses. The asymmetry used to be `[promote] lockstep=1` against `lockstep=0`;
# G96's point survives the change unaltered -- brokered is the shipping default, so a host-only
# fragment states what already holds and the run is silently symmetric. The client fragment is what
# creates the asymmetry, now by selecting the original engine whole rather than one closure.

PROMOTE_INI = os.path.join(REPO, "tools", "uiscripts", "ini", "ship_config.ini")
# The CLIENT half of the asymmetric shape. Brokered is the shipping default, so a client that
# receives no fragment runs OURS -- the asymmetry has to be created by selecting `original` here, not
# only `brokered` on the host. See the fragment's own banner.
UNPROMOTE_INI = os.path.join(REPO, "tools", "uiscripts", "ini", "rollback_original.ini")


def _manifest_fix_knobs():
    """`[net]` knobs whose BYTE PATCH targets a body we can promote, read from the patch manifest.

    Derived rather than listed, so a fix migrated tomorrow is covered without editing this file. A
    knob qualifies when its carrier is `migrated:` (the fix is carried in our code) or `pending:`
    (the collision is real and NOT carried yet).
    """
    path = os.path.join(REPO, "tools", "data", "dll_patch_manifest.json")
    try:
        with open(path, encoding="utf-8") as f:
            patches = json.load(f)["patches"]
    except Exception:
        return set()
    return {
        p["knob"]
        for p in patches
        if p.get("knob") and str(p.get("carrier") or "").split(":")[0] in ("migrated", "pending")
    }


# Every `[net] <key>` named by a `reimpl_fixes` member comment in the header below. The struct's own
# contract says each member "keeps the NAME AND MEANING of the ini key", so the comment is the
# authoritative key list and this regex is reading a convention the header documents, not guessing at
# one.
_REIMPL_FIX_KEY_RE = re.compile(r"^\s*//\s*\[net\]\s+([A-Za-z_][A-Za-z0-9_]*)")


def _reimpl_only_fix_knobs():
    """`[net]` knobs that exist ONLY as a `reimpl_fixes` member -- no byte patch, so the manifest
    cannot see them.

    THIS IS THE HALF THE MANIFEST DERIVATION STRUCTURALLY CANNOT COVER, and it was a live hole, not a
    hypothetical one (U20 (f), fixed 2026-08-30). `migrated_fix_knobs()` derived its
    list from BYTE PATCHES; a fix that only ever existed in our reimplemented body has none, so it
    never entered the list, so `write_fixes_off_ini()` never forced it off, so the asymmetric oracle
    silently compared ours-with-fix against original-without-fix.

    `resync_trigger_gate` was already through that hole and is NOT harmless: its ini default is 1
    (net_lockstep.cpp), its byte-patch carriers were retired by C8-e, and our promoted body applies
    it -- so every asymmetric run to date had the fix live on the promoted peer and absent on the
    original one. `desync_icon_gate` (U20) would have been the second.

    Parsed from the header rather than listed here for the same reason the manifest half is derived:
    a member added tomorrow is covered without editing this file. Rig-only members (rig_fixed_step_loop)
    are deliberately INCLUDED -- an asymmetric rig knob corrupts the comparison exactly as a fix does.
    """
    path = os.path.join(REPO, "src", "mh_dll", "libmh", "lockstep", "turn_engine.h")
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except Exception:
        return set()
    out, inside = set(), False
    for line in lines:
        if line.startswith("struct reimpl_fixes"):
            inside = True
            continue
        if inside:
            if line.startswith("}"):
                break
            m = _REIMPL_FIX_KEY_RE.match(line)
            if m:
                out.add(m.group(1))
    return out


def migrated_fix_knobs():
    """The `[net]` knobs naming a fix our promoted body carries, from BOTH derivations.

    Two sources, because neither can see the other's fixes:
      * the patch manifest -- a fix that still has a byte patch aimed at a promotable body. Catches
        `resync_wait_fix`, which our body carries UNCONDITIONALLY and so is not a `reimpl_fixes`
        member at all.
      * the `reimpl_fixes` struct -- a fix that exists only in our code and therefore has no manifest
        row to be derived from.
    Union, not either alone: U20 (f).
    """
    knobs = _manifest_fix_knobs() | _reimpl_only_fix_knobs()
    return sorted(knobs)


def migrated_fix_knob_sources():
    """Which derivation found each knob. For the arming line -- a run that cannot say WHY a knob is
    on the list cannot notice when one silently drops off it."""
    manifest, reimpl = _manifest_fix_knobs(), _reimpl_only_fix_knobs()
    out = {}
    for k in sorted(manifest | reimpl):
        tags = []
        if k in manifest:
            tags.append("manifest")
        if k in reimpl:
            tags.append("reimpl_fixes")
        out[k] = "+".join(tags)
    return out


def asymmetric_fix_config_error(host_ini):
    """Refuse a host-only fragment that turns a MIGRATED FIX on for one peer only. Pure, so the rule
    is testable without a rig.

    Enabling a reimpl-side fix makes the promoted body deliberately differ from the original. Setting
    one on ONE peer means the oracle compares ours-with-fix against original-without-fix, where every
    difference is expected -- which is exactly how a real divergence gets waved through. Promotion
    itself is the one asymmetry these runs are FOR; a fix knob is not.
    """
    if not host_ini or not os.path.isfile(host_ini):
        return None
    knobs = set(migrated_fix_knobs())
    hits = []
    for raw in open(host_ini, encoding="utf-8", errors="replace"):
        line = raw.split(";", 1)[0].strip()
        if "=" not in line:
            continue
        k = line.split("=", 1)[0].strip()
        if k in knobs:
            hits.append(k)
    if not hits:
        return None
    return (
        "REFUSED: %s sets the migrated-fix knob(s) %s for the HOST ONLY. A reimpl-side fix enabled on "
        "one peer makes our body deliberately differ from the original, so every difference the oracle "
        "sees is expected and a real divergence would be waved through. Put fix knobs in --extra-ini "
        "(both peers); --extra-ini-host is for the PROMOTION asymmetry only."
        % (os.path.basename(host_ini), ", ".join(sorted(set(hits))))
    )


def write_fixes_off_ini(path):
    """An --extra-ini fragment that turns every migrated fix OFF, for BOTH peers.

    This is the asymmetric shape's precondition, and it is generated rather than committed so it
    cannot fall behind either source it is derived from.
    """
    sources = migrated_fix_knob_sources()
    knobs = sorted(sources)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(
            "; GENERATED by tools/test_ui.py -- the asymmetric determinism shape's precondition.\n"
        )
        f.write(
            "; Every knob naming a fix our promoted body carries, forced OFF on BOTH peers so the\n"
        )
        f.write("; oracle compares ours against the original rather than ours-with-fix against\n")
        f.write(
            "; original-without-fix. Two derivations, because neither sees the other's fixes:\n"
        )
        f.write(";   manifest      -- the fix still has a byte patch aimed at a promotable body\n")
        f.write(";   reimpl_fixes  -- the fix exists ONLY in our body, so there is no patch to\n")
        f.write(";                    derive it from\n")
        f.write("[net]\n")
        for k in knobs:
            f.write("%s=0   ; %s\n" % (k, sources[k]))
    return knobs


def peer_promotion(det_dir, peer):
    """(RUN-CONFIG string or None, first-call liveness lines) for one peer of a finished run."""
    path = os.path.join(det_dir, peer, "mh_net.log")
    if not os.path.isfile(path):
        return None, []
    text = open(path, encoding="utf-8", errors="replace").read()
    cfg = next(
        (ln.split("RUN-CONFIG:")[1].strip() for ln in text.splitlines() if "RUN-CONFIG:" in ln),
        None,
    )
    return cfg, [ln.strip() for ln in text.splitlines() if "(OURS is live)" in ln]


def det_run_report(det_dir, want):
    """Read a finished determinism run's artifacts. Returns (ok, lines).

    `want` maps peer dir -> whether that peer is supposed to be running OUR engine. It is checked in
    BOTH directions, because a shape can lose its meaning either way:

      * promotion requested and nothing went live -- the run compared the original against the
        original. It PASSES the hash comparison (there is nothing asymmetric left to disagree), which
        is precisely why it has to fail here. O3's first asymmetric run came back ALL PAIRS IDENTICAL
        with the promotion never installed.
      * promotion NOT requested and it went live anyway -- the asymmetric shape's client picking up
        the host-only fragment would make the run symmetric, and a symmetric run cannot see a
        deterministic-but-wrong engine. Same green, same worthlessness, opposite cause.

    WHICH SIGNAL ANSWERS WHICH DIRECTION, and getting this wrong kept the ASYMMETRIC shape red after
    it had been repaired (2026-09-01). The `(OURS is live)` lines are NOT subsystem-scoped: both
    mh::lockstep AND mh::orders emit that exact text with bare seam names, and the orders closure is
    1, so a client running the ORIGINAL lockstep closure still logs ~18 of them from the orders
    closure it is quite correctly still running. Reading "any liveness line" as "the lockstep closure
    is live" therefore fails a correctly-asymmetric run.
      -> "is the LOCKSTEP closure promoted here" is answered by the RUN-CONFIG line, which is exactly
         the machine-readable verdict C5 added for tools to act on. install_promotion returns BEFORE
         emitting it when `[promote] lockstep=0`, so ABSENT means not promoted; `SHIP` is the only
         value a ship verdict may be read from (DIAGNOSTIC/INVALID/REFUSED are all "not this").
      -> the liveness lines still answer the OTHER direction, which RUN-CONFIG cannot: the seams
         installed but nothing ever CALLED them. That is O3's original failure and it is kept.
    """
    lines, ok = [], True
    for peer, expect in want.items():
        cfg, live = peer_promotion(det_dir, peer)
        promoted = bool(cfg) and cfg.startswith("SHIP")
        lines.append(
            "      %-8s RUN-CONFIG: %-42s liveness lines: %d"
            % (peer, cfg or "(absent -- not promoted)", len(live))
        )
        if promoted and not expect:
            ok = False
            lines.append(
                "      FAIL: %s was supposed to run the ORIGINAL and is running OURS -- the shape's "
                "asymmetry evaporated, so this run compared ours against ours." % peer
            )
        elif expect and not promoted:
            ok = False
            lines.append(
                "      FAIL: %s was supposed to run OURS and the lockstep closure is not promoted "
                "(RUN-CONFIG %s) -- this run compared the original against the original."
                % (peer, cfg or "absent")
            )
        elif expect and not live:
            ok = False
            lines.append(
                "      FAIL: %s was supposed to run OURS and nothing went live -- this run compared "
                "the original against the original and its green means nothing." % peer
            )
    first = next(
        (ln for peer in want for ln in peer_promotion(det_dir, peer)[1]),
        None,
    )
    if first:
        lines.append("      e.g. %s" % first)
    try:
        with open(os.path.join(det_dir, "host", "mp_analyze.json"), encoding="utf-8") as f:
            j = json.load(f)
        compared = [p.get("combined_mismatch_count", 0) for p in (j.get("desync_pairs") or [])]
        lines.append("      compared steps per pair: %s" % (compared or "(none)"))
    except Exception:
        lines.append("      compared steps per pair: (mp_analyze.json unreadable)")
    return ok, lines


# ---- P0-SPDET: the SINGLE-PLAYER determinism oracle ---------------------------------------------
# Two SEQUENTIAL runs of ONE local lane -- unpromoted, then promoted -- over the same scripted
# session with the same synth seed and a PINNED WALL CLOCK. Not a two-peer shape: there is no second
# machine and no lockstep, so nothing is peer-local and every channel must agree.
#
# WHY IT EXISTS: C2 measured NINE promoted seams as LIVE RETAIL LOGIC, and the shipped determinism
# gate is 2-peer MP which cannot see single-player paths at all. C8 flips promotion default-on, so
# our C++ lands on the SP campaign clock and order path with nothing watching.
SP_SCRIPT = "sp_det.txt"  # seats an AI and launches ALONE -> LOBBY_SCAN_HOST_COUNT < 2, so
# SESSION_MODE is 2: the non-3 branch, which is what "LIVE" means. (Mode 1, the campaign entry, is
# NOT covered -- tracker P0-SPCAMP.) NOT mp_host_start_ai.txt, which looks right and waits on
# `peers 1` for a human client, so a solo run aborts in the lobby.
SP_HARNESS_EXTRA = "pin_wallclock=1;fixed_step=0;region_hash_step=1"
# THE SHIP CONFIGURATION, stated rather than assumed. This used to be promote_sp.ini, a second file
# holding the same one line as ship_config.ini for a reason that expired with the per-key surface:
# it named BOTH closures because `[promote] lockstep=1` alone was measured to execute exactly one
# seam in a solo run. One selector value cannot be partial, so the two files collapsed into one.
SP_PROMOTE_INI = os.path.join(REPO, "tools", "uiscripts", "ini", "ship_config.ini")
# C8-f (2026-07-30): promotion is the SHIPPING DEFAULT, so the BASELINE arm has to ASK for the
# original -- an arm that passes no ini is now a promoted arm. This oracle caught the flip itself the
# first time it ran after it: both arms came back promoted and it printed "baseline was supposed to
# run the ORIGINAL and is running OURS -- both arms are promoted, so the comparison has nothing to
# say", rather than reporting the identical hashes as a pass. That refusal is the reason the arm
# check exists, and it is why this line is a rollback ini and not a `pass`.
#
# F2C (2026-09-12): BOTH fragments are now one `[config] mode` line each. The rollback file used to
# be a hand-listed 20-key block whose own header records three gate runs lost to a key that shipped
# ON and was not named in it -- the treadmill D11 exists to end. Since the two arms now differ in
# exactly one key, the arm check can also be asked BEFORE the rig is spent (sp_mode_refusal).
SP_ROLLBACK_INI = os.path.join(REPO, "tools", "uiscripts", "ini", "rollback_original.ini")


def sp_newest_run(lane_dir):
    runs = sorted(glob.glob(os.path.join(lane_dir, "logs", "*")), key=os.path.getmtime)
    return runs[-1] if runs else None


def sp_arm_game_speed(log_dir):
    """The game speed an arm ACTUALLY RAN AT, in percent, read out of its own log.

    Deliberately not "the speed we asked for". A run at `game_speed_pct=1000` carries 10x the game
    time per sim step, so per-step hashes recorded at two speeds are not the same measurement --
    and every previous consumer of these logs had no way to notice, because nothing wrote the speed
    down. The DLL now emits `; game_speed: N%` unconditionally (net_lockstep.cpp), so an ABSENT line
    means an old build rather than a default, and is reported as unknown rather than assumed to be
    100.
    """
    for fn in ("mh_net.log", "mh_harness.log"):
        p = os.path.join(log_dir, fn)
        if not os.path.isfile(p):
            continue
        m = re.search(r"game_speed:\s*(\d+)%", open(p, encoding="utf-8", errors="replace").read())
        if m:
            return int(m.group(1))
    return None


def sp_speed_refusal(speeds, na, nb):
    """(None, note) if the two arms are comparable; (message, None) if the comparison must be REFUSED.

    Extracted from run_sp_determinism so it can be tested WITHOUT A RIG -- and it had to be, for a
    reason worth recording. The obvious live mutation (put `game_speed_pct=200` into the promoted
    arm's --extra-ini fragment) DOES NOT WORK: an --extra-ini fragment is appended as a whole extra
    section, so the lane's mh_net.ini ends up with TWO `[net]` blocks and GetPrivateProfile* reads
    only the FIRST -- which is the template's own `game_speed_pct=0`. The fragment's key is in the
    file and unreachable. That is the same trap det_standard_selftest's fourth rule covers between
    two fragments, here between the TEMPLATE and a fragment, and it is exactly the shape that makes
    an unrunnable mutation look like a broken check. So the refusal is a pure function with unit
    tests, and the live path's evidence is that a matched-speed run prints the comparable line.
    """
    if speeds.get(na) != speeds.get(nb):
        fmt = lambda s: ("%d%%" % s) if s is not None else "UNKNOWN"  # noqa: E731
        return (
            "the two arms ran at DIFFERENT game speeds (%s: %s, %s: %s).\n"
            "      A step's game-time size scales with game_speed_pct, so per-step hashes recorded\n"
            "      at two speeds are not comparable -- a comparison here would go red and be read as\n"
            "      a determinism failure. No verdict is produced."
            % (na, fmt(speeds.get(na)), nb, fmt(speeds.get(nb))),
            None,
        )
    if speeds.get(na) is None:
        # An old DLL that predates the `; game_speed:` line. Not fatal -- it is exactly the state
        # every historical run is in -- but it must not read as "both arms were at 100%".
        return (
            None,
            "NOTE: neither arm reported a game speed (a build older than the `; game_speed:` line)."
            " The cross-speed refusal could not run.",
        )
    return (None, "game speed: both arms at %d%% -- comparable" % speeds[na])


def sp_arm_report(log_dir, name, want_promoted):
    """Run-shape rules, checked BEFORE the hash verdict. Returns (ok, lines).

    Every rule here is a way for the comparison to be green while meaning nothing -- the same family
    as det_run_report's. A not-armed pin and an armed-but-never-called one look identical in the
    hashes, so the ARMED line is read explicitly rather than inferred from the numbers.
    """
    lines, ok = [], True
    hl = os.path.join(log_dir, "mh_harness.log")
    nl = os.path.join(log_dir, "mh_net.log")
    htext = open(hl, encoding="utf-8", errors="replace").read() if os.path.isfile(hl) else ""
    ntext = open(nl, encoding="utf-8", errors="replace").read() if os.path.isfile(nl) else ""
    armed = "pin_wallclock ARMED" in htext or "pin_wallclock ARMED" in ntext
    # PER-SEAM, not a line count. Measured 2026-07-29: a promoted run reported "4 liveness lines"
    # and all four were time_tick's call milestones -- ONE seam of the ten installed. Every other
    # lockstep seam is mode-3 gated and a solo run is SESSION_MODE 2, so they installed and were
    # never called. A count cannot tell those apart; the seam names can.
    seams = {}
    for ln in ntext.splitlines():
        m = re.search(r"\[promote\]\s+([A-Za-z0-9_]+): call #(\d+) \(OURS is live\)", ln)
        if m:
            seams[m.group(1)] = max(seams.get(m.group(1), 0), int(m.group(2)))
    live = [ln.strip() for ln in ntext.splitlines() if "(OURS is live)" in ln]
    speed = sp_arm_game_speed(log_dir)
    lines.append(
        "      %-10s pin_wallclock: %-10s game_speed: %-9s seams EXECUTED: %s"
        % (
            name,
            "ARMED" if armed else "NOT ARMED",
            ("%d%%" % speed) if speed is not None else "UNKNOWN",
            ", ".join("%s(>=%d)" % (k, v) for k, v in sorted(seams.items())) or "(none)",
        )
    )
    if not armed:
        ok = False
        lines.append(
            "      FAIL: %s ran without the pinned wall clock, so time_tick read the REAL clock and "
            "its three regions are wall-clock garbage -- they cannot agree and their agreeing would "
            "be worse." % name
        )
    if want_promoted and not live:
        ok = False
        lines.append(
            "      FAIL: %s was supposed to run OURS and nothing went live -- this compared the "
            "original against the original." % name
        )
    if live and not want_promoted:
        ok = False
        lines.append(
            "      FAIL: %s was supposed to run the ORIGINAL and is running OURS -- both arms are "
            "promoted, so the comparison has nothing to say." % name
        )
    return ok, lines


# ---- THE ALL-AI SOAK (2026-08-05) ---------------------------------------------------------------
# A LONG-RUNNING N-WAY AI MATCH, reached through the real menu, whose product is a state-evolution
# trajectory rather than a pass/fail comparison.
#
# WHY IT IS A MODE HERE AND NOT ITS OWN SCRIPT: it wants exactly what --determinism/--sp-determinism
# already own -- lane provisioning, headless, the ini key-merge that avoids the duplicate-[section]
# trap, and above all the RUN-SHAPE REFUSALS. A soak has more ways to be vacuously green than either
# oracle: the conversion can arm and convert nothing, a converted slot can miss spawn_ai_base, the
# match can resolve at step 300 of a 30000-step budget, or the run can be silently truncated by
# ui_test's wall-clock timeout. Every one of those produces a log full of plausible numbers.
SOAK_SCRIPT = "sp_soak.txt"
# region_hash_step=1 is what makes the run an integration-test artifact rather than just a long game:
# it emits the per-step per-region hash stream that --soak-golden records and compares.
#
# synth_move=0 IS LOad-BEARING, and it cost a red golden to find. ui_test.py arms the D6 moving-unit
# workload BY DEFAULT (SYNTH_MOVE=1, SYNTH_AT=60) with a seed drawn FRESH PER INVOCATION, so two
# identical soaks diverged at step 60 in order_queue and cascaded into units/tile_objects -- the run
# was not reproducible and the golden was comparing two different worlds. Two reasons it is off here
# rather than merely seed-pinned:
#   1. D6 exists because a determinism run over an IDLE world proves nothing. A soak is eight AI
#      players playing a match; it is the least idle world this project can produce, and a random
#      walk adds nothing to it.
#   2. The workload issues move orders AS PlayerSide -- which under all_ai is an AI-controlled
#      player. It would countermand, every single step, the decisions of the AI the soak exists to
#      observe. That is not noise on top of the measurement, it is inside it.
SOAK_HARNESS_EXTRA = "pin_wallclock=1;fixed_step=0;region_hash_step=1;all_ai=1;synth_move=0"


def soak_report(log_dir, args):
    """Run-shape rules for a soak, checked BEFORE anything is read as coverage.

    Same discipline as sp_arm_report: each rule here is a way for a run to LOOK like an all-AI soak
    and not be one. They are read from the log's own ARMED/VERIFY lines rather than inferred from
    counts, because a not-armed knob and an armed one whose world never developed produce the same
    quiet numbers -- the lesson the shadow sites taught and the AIPROBE line was added for.
    """
    lines, ok = [], True
    hl = os.path.join(log_dir, "mh_harness.log")
    text = open(hl, encoding="utf-8", errors="replace").read() if os.path.isfile(hl) else ""

    armed = "; ALLAI ARMED" in text
    conv = re.search(r"; ALLAI converted (\d+) human slot\(s\)", text)
    n_conv = int(conv.group(1)) if conv else 0
    verify = re.findall(r"; ALLAI VERIFY slot=(\d+) .*?(OK|NOT-SPAWNED)", text)
    bad = [s for s, v in verify if v != "OK"]
    lines.append(
        "      all_ai: %s  converted=%d  spawn-verified=%d/%d"
        % ("ARMED" if armed else "NOT ARMED", n_conv, len(verify) - len(bad), len(verify))
    )
    if not armed:
        ok = False
        lines.append("      FAIL: all_ai never armed -- this is an ordinary 1-human skirmish.")
    elif n_conv == 0:
        ok = False
        # TWO DIFFERENT FAILURES, SEPARATED 2026-09-05, and conflating them cost most of a session.
        # `n_conv == 0` used to print "No enabled HUMAN slot existed at landing" unconditionally --
        # which is an INFERENCE about status_flags, not an observation, and it is wrong whenever the
        # detour never executed at all. That sentence sent a bisect after a translation defect in
        # llm_strat_session_begin_multi; the body is faithful. What actually happens with
        # `[promote] sim_resid=1` is that our promoted session_begin_multi reaches
        # land_players_on_planet by a DIRECT intra-slice C++ call
        # (sim/resid/sim_session_begin_multi.cpp), never through the game VA the detour's trampoline
        # sits on -- so on_land_players() (harness.cpp:1179, reached from land_players_detour and
        # nothing else) is simply not called.
        #
        # The discriminator is whether the detour left ANY trace: it logs a per-slot `; ALLAI slot=`
        # line for each conversion, so no such line AND no conversion means the body never ran.
        ran = "; ALLAI slot=" in text
        if ran:
            lines.append(
                "      FAIL: the conversion detour RAN and converted nothing -- no enabled HUMAN slot "
                "existed at landing. This is about status_flags: the local player is not an AI, so the "
                "run is not an all-AI match."
            )
        else:
            lines.append(
                "      FAIL: the conversion detour NEVER RAN -- armed, but no `; ALLAI slot=` line at "
                "all. It is BYPASSED, not ineffective: check for `[promote] sim_resid=1`, whose "
                "promoted session_begin_multi calls land_players_on_planet directly and skips the "
                "entry the trampoline lives on. Also expect `sim_resid: N/M seams installed -- "
                "PARTIAL, treat this run as invalid` in mh_net.log. This is NOT a status_flags "
                "finding: a promoted body's intra-slice DIRECT call bypasses the callee's game VA, "
                "so an instrument living on that entry vanishes."
            )
    if bad:
        ok = False
        lines.append(
            "      FAIL: slot(s) %s were converted but never went through llm_strat_spawn_ai_base "
            "-- no AI home tile, so their distance-to-home decisions run from the map origin."
            % ", ".join(bad)
        )

    # THE DLL'S OWN INVALIDITY VERDICT, HONOURED (added 2026-09-05). A promotion installer that could
    # not take every seam it owns prints `N/M seams installed -- PARTIAL, treat this run as invalid`,
    # and until now NOTHING on the Python side read it: a soak with `[promote] sim_resid=1` reports
    # 30/31 PARTIAL in mh_net.log every time -- because `[harness] all_ai=1`'s landing detour owns
    # llm_game_land_players_on_planet's entry and the seam is refused -- and the runner called such runs
    # PASS. Two coverage baselines were recorded off runs the DLL had already declared invalid before
    # this check existed. The configuration is documented as mutually exclusive in
    # tools/data/dll_patch_manifest.json; this is what makes that documentation bite.
    nl = os.path.join(log_dir, "mh_net.log")
    ntext = open(nl, encoding="utf-8", errors="replace").read() if os.path.isfile(nl) else ""
    for m in re.finditer(
        r"; \[promote\] (\w+): (\d+)/(\d+) seams installed -- PARTIAL[^\r\n]*", ntext
    ):
        ok = False
        lines.append(
            "      FAIL: the DLL declared this run INVALID -- [promote] %s installed only %s of %s "
            "seams. Do not read any verdict, hash or coverage figure off it." % m.group(1, 2, 3)
        )

    # SIM1-P CLAUSE 6b: A YIELD MAY NOT COST US A PROMOTION (added 2026-09-05). The harness disarms a
    # rebind row whose entry an instrument claims, so callers funnel through the instrumented VA. That
    # is right ONLY if our body still runs -- i.e. the instrument's fall-through was pointed at our
    # thunk -- or if the instrument is a genuine SUBSTITUTION for the row. Otherwise arming the
    # instrument silently runs the ORIGINAL, which is how the TJ order-enqueue recorder un-promoted
    # llm_tact_unit_enqueue_command for 12 hours and cost 34 covered lines in every journal scenario
    # (tracker TACT-COV-YIELD). The dispositions are a HAND list because "does our body still run"
    # turns on a per-instrument decision that cannot be derived from the yield itself.
    ytext = ""
    hl = os.path.join(log_dir, "mh_harness.log")
    if os.path.isfile(hl):
        ytext = open(hl, encoding="utf-8", errors="replace").read()
    yielded = sorted(set(re.findall(r"; \[rebind\] (\S+) YIELDED to ", ytext)))
    if yielded:
        try:
            disp = json.load(
                open(
                    os.path.join(REPO, "tools", "data", "rebind_yield_dispositions.json"),
                    encoding="utf-8",
                )
            )["rows"]
        except (OSError, ValueError, KeyError):
            disp = {}
            ok = False
            lines.append(
                "      FAIL: %d row(s) YIELDED but tools/data/rebind_yield_dispositions.json is "
                "unreadable -- the clause-6b check cannot run, so this run is not evidence."
                % len(yielded)
            )
        for row in yielded:
            d = disp.get(row)
            if d is None:
                ok = False
                lines.append(
                    "      FAIL: row %s was YIELDED and has NO disposition. Either point the claiming "
                    "instrument's fall-through at our body (an MH_Harness_Rebind*) or record it in "
                    "tools/data/rebind_yield_dispositions.json -- an instrument may not cost us a "
                    "promotion (SIM1-P clause 6b)." % row
                )
            elif d.get("disposition") == "substituted":
                continue  # the instrument IS the replacement; no liveness is expected or wanted
            elif d.get("exercised_when") and not re.search(d["exercised_when"], ytext):
                # The row was disarmed but nothing in this run drives that entry, so there is no
                # liveness to have and no promotion to lose. llm_strat_sim_step in a TACTICAL run is
                # the case that forced this branch: mode 6 never calls it, and without the condition
                # the check failed every tactical run for nothing.
                continue
            elif d.get("liveness") and d["liveness"] not in ntext:
                ok = False
                lines.append(
                    "      FAIL: row %s was YIELDED, is dispositioned %r, and its liveness signal "
                    "(%r) is ABSENT -- so the yield disarmed the row and the ORIGINAL served those "
                    "calls, not our body." % (row, d.get("disposition"), d["liveness"])
                )

    # THE ENTRY-CLAIM TABLE OVERFLOW, same doctrine (added 2026-09-05). `[owner] TABLE FULL` means
    # claim_entry ran out of slots, so further claims are DROPPED -- detour_refusal stops refusing a
    # second patch on those entries and clause 6's derived yield cannot see them. It overflowed on
    # EVERY promoted run from 93a4b8da until the cap was raised, emitting 241 near-identical lines per
    # run, and nothing read them: the A/B, the determinism gate and the whole UI suite all ran on a
    # table the DLL knew was truncated. A capacity limit that degrades a safety interlock is not a
    # diagnostic, so it fails here by the same rule as a PARTIAL install.
    for m in re.finditer(
        r"; \[owner\] TABLE FULL \((\d+) slots\) at ([0-9A-Fa-f]+)[^\r\n]*", ntext
    ):
        ok = False
        lines.append(
            "      FAIL: the DLL's entry-claim table FILLED (%s slots, first drop at %s) -- claims "
            "past it are dropped, so the double-patch interlock and the derived yield are both "
            "incomplete. Raise MAX_OWNED in mh/hook/promoted.cpp; do not read this run."
            % m.group(1, 2)
        )

    # The synth workload must be OFF, read from the banner the DLL writes rather than from what this
    # runner intended to pass. It is armed by ui_test's own defaults with a per-invocation seed, so a
    # soak that inherits it is not reproducible AND has the harness issuing orders as an AI player.
    # An override can re-arm it; this makes that visible instead of silent.
    syn = re.search(r"; synth_move=(\d+) armed=(\d+) seed=(-?\d+)", text)
    if syn and syn.group(2) == "1":
        ok = False
        lines.append(
            "      FAIL: the D6 synth workload is ARMED (seed=%s). It draws a fresh seed per "
            "invocation, so the run is not reproducible, and it issues move orders as PlayerSide -- "
            "an AI player under all_ai. Pass synth_move=0." % syn.group(3)
        )

    # Every CLAIMED player must be AI-enabled. Read from the last AIPROBE line, and require the probe
    # to exist at all: without it the mask is unknown, which is not the same as correct.
    probes = re.findall(
        r"; AIPROBE step=(\d+) ai_on=(\d+) nplayers=(\d+) ai_enabled=\[(\d+)\]", text
    )
    if not probes:
        ok = False
        lines.append(
            "      FAIL: no AIPROBE line -- pass ai_probe_step so the AI mask is observed."
        )
    else:
        pok, plines = soak_ai_premise(text, *probes[-1][1:])
        lines += plines
        if not pok:
            ok = False

    # Did the match resolve, and where? Not a failure -- it is the run's most important datum -- but
    # steps after it simulate a finished world and must not be counted as coverage.
    over = re.search(r"; GAMEOVER survivor=(\d+) units=(-?\d+) buildings=(-?\d+)", text)
    watch = re.findall(r"; GAMEOVER-WATCH step=(\d+) alive=(\d+)", text)
    if watch:
        lines.append(
            "      alive-player trajectory: %s" % " -> ".join("%s@%s" % (a, s) for s, a in watch)
        )
    if over:
        lines.append(
            "      RESOLVED: survivor=%s with %s units / %s buildings. Steps past the GAMEOVER line "
            "are a finished world -- do not read them as AI coverage." % over.groups()
        )
    else:
        lines.append("      not resolved within the step budget (the usual outcome; see notes).")

    # Truncation. ui_test's wall-clock timeout ends a run SILENTLY at whatever step it reached, and a
    # short soak reads exactly like a long one until the step count is compared with what was asked.
    seg = None
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import mp_analyze as _m

        seg = _m.parse_harness(hl)
        if seg is not None:
            # The log this segment came FROM, so soak_golden can read that run's hash
            # fingerprint rather than guess which run it is holding.
            seg["harness_log"] = hl
    except Exception as e:
        lines.append("      (mh_harness.log unparsable: %s)" % e)
    if seg is not None:
        got = max(seg["regions"]) if seg["regions"] else 0
        lines.append("      hashed steps: %d of %d requested" % (got, args.steps))
        if got < args.steps and not over:
            # A SHORT RUN IS NOT AUTOMATICALLY A TIMEOUT, and saying so cost a wrong diagnosis
            # (2026-08-05): llm_strat_player_presence_lost's MP path ends the match by raising the
            # OUTCOME DIALOG, which stops the sim while the render loop keeps running -- so the rig
            # reports `harness STALLED` and this rule called a resolved 8-way match "a wall-clock
            # timeout or a crash". The alive count falling is the evidence that separates them; only
            # a short run with no player ever lost is actually unexplained.
            declined = len(watch) > 1 and int(watch[-1][1]) < int(watch[0][1])
            if declined:
                lines.append(
                    "      RESOLVED (inferred): the alive count fell to %s and the sim stopped at "
                    "step %d. The match ended on the outcome dialog -- which halts the sim while the "
                    "render loop continues, so the rig reports a STALL. Not a truncation."
                    % (watch[-1][1], got)
                )
            else:
                ok = False
                lines.append(
                    "      FAIL: the sim stopped at step %d of %d with NO player eliminated, so the "
                    "match did not end. Distinguish the causes from the run's own artifacts rather "
                    "than assuming: if mh_frametime.log keeps growing past the last step the process "
                    "is alive and only the SIM stopped; if it stops too, check WER. A very high "
                    "--soak-speed is the first thing to rule out -- 8000%% stalls reproducibly at "
                    "step 1475 (1333 ms per step) while 1000%% runs the same game time clean."
                    % (got, args.steps)
                )
    return ok, lines, seg


def hash_fingerprint(harness_log):
    """The fingerprint of the hash implementation that produced a run, or None.

    The DLL hashes a fixed vector with the live `hash_sink` and logs the result, so this value moves
    whenever the algorithm, block size, seed, tail handling or mixing constant does -- automatically,
    with nobody having to remember to bump a version. See harness.cpp hash_fingerprint_report."""
    if not harness_log:
        return None
    try:
        f = open(harness_log, encoding="utf-8", errors="replace")
    except OSError:
        return None
    with f:
        for ln in f:
            if ln.startswith("; HASH FINGERPRINT "):
                return ln.split()[3]
    return None


def soak_golden(seg, path, args):
    """Record or compare the per-step region-hash trajectory -- the integration-test half.

    The artifact is deliberately the WHOLE per-step per-region stream and not a single end-state
    hash: an end-state compare says only that two runs finished alike, while the stream says WHERE
    they first stopped agreeing, which is the question anybody debugging a promoted seam actually
    has. It is the same data --determinism compares across two peers, compared here across two runs
    in time instead.
    """
    cur = {int(k): v for k, v in seg["regions"].items()}
    # The CLOCK stream is stored beside the region hashes because it separates the two failure modes
    # a bare state diff cannot: a clock that diverges first means TIME leaked into the run (a pacing
    # or wall-clock path the pin does not cover), while identical clocks with diverging state mean
    # the sim took a different decision at the same instant. Those want completely different
    # investigations, and reporting only "diverged at step N" sends you down the wrong one.
    clk = {int(k): v["clock"] for k, v in seg["steps"].items()}
    # STAMPED WITH THE HASH IMPLEMENTATION THAT PRODUCED IT. A golden is the one artifact here
    # that outlives its build, and the values in it are only meaningful under the hash that made
    # them. Comparing across a hash change diverges at the first compared step in EVERY region at
    # once -- which reads as "the simulation broke catastrophically", the most expensive possible
    # misdiagnosis, rather than as "this file is stale".
    fp = hash_fingerprint(seg.get("harness_log") if isinstance(seg, dict) else None)
    if not os.path.isfile(path):
        with open(path, "w", encoding="utf-8") as f:
            json.dump(
                {
                    "hash_fingerprint": fp,
                    "steps": {str(k): v for k, v in cur.items()},
                    "clock": {str(k): v for k, v in clk.items()},
                },
                f,
            )
        return True, ["      golden RECORDED: %s (%d steps)" % (path, len(cur))]
    with open(path, encoding="utf-8") as f:
        blob = json.load(f)
    old = {int(k): v for k, v in blob["steps"].items()}
    oldclk = {int(k): v for k, v in (blob.get("clock") or {}).items()}
    oldfp = blob.get("hash_fingerprint")
    if fp and oldfp and fp != oldfp:
        return False, [
            "      STALE GOLDEN: recorded under hash implementation %s, this build computes %s."
            % (oldfp, fp),
            "      Every value in it is incomparable -- this is NOT a simulation divergence.",
            "      Delete %s and re-record." % path,
        ]
    if fp and not oldfp:
        return False, [
            "      GOLDEN PREDATES THE FINGERPRINT (no hash_fingerprint field), so it cannot be "
            "shown to have been recorded under this build's hash.",
            "      Delete %s and re-record; a false red here is indistinguishable from a real one."
            % path,
        ]
    common = sorted(set(cur) & set(old))
    if not common:
        return False, ["      FAIL: golden and run share NO steps -- nothing was compared."]
    if oldclk:
        cbad = [s for s in common if s in oldclk and s in clk and oldclk[s] != clk[s]]
        if cbad:
            return False, [
                "      CLOCK diverged from golden first, at step %d -- time itself is not "
                "reproducible here, so every state difference downstream is a symptom." % cbad[0]
            ]
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import mp_analyze as _m

        names = _m.REGION_NAMES
    except Exception:
        names = []
    for s in common:
        if cur[s] != old[s]:
            diff = [
                (names[i] if i < len(names) else "col%d" % i)
                for i in range(min(len(cur[s]), len(old[s])))
                if cur[s][i] != old[s][i]
            ]
            return False, [
                "      DIVERGED from golden at step %d, region(s): %s"
                % (s, ", ".join(diff) or "?"),
                "      (%d steps compared before the divergence)" % common.index(s),
            ]
    return True, ["      golden MATCHED over %d compared steps" % len(common)]


# ---- TACT-PREP: the TACTICAL determinism oracle -------------------------------------------------
# Two sequential runs of one local lane through the SAME --tactical entry, compared on the per-frame
# `T`/`TR` lines the llm_tact_frame cadence emits.
#
# WHY IT DOES NOT GO THROUGH ui_test.py LIKE THE OTHER ORACLES. Every other mode drives the game with
# a UI SCRIPT; this one has nothing to drive. The `--tactical` launch verb IS the driver -- it fires
# at menu-idle, synthesises the squad blackboard and enters the mission -- and after that the
# mission runs itself, because tactical is all-AI as shipped (the tactical-probe work 5b: both sides
# go through llm_tact_unit_owner_tick, and every shipped POZ*.DAT assigns DEFENSE:GUARD/ATTACK to
# the player's own units). So there is no script to pass and no input to record; there is a command
# line and a stop frame.
#
# THE SELFTEST ARM IS NOT OPTIONAL DECORATION. A hash over a region nothing writes is identical
# across two runs too, so a green comparison alone cannot distinguish "deterministic" from
# "measuring nothing". --tact-selftest pokes ONE region mid-run and requires the compare to fail at
# EXACTLY that frame and to name EXACTLY that region. Default is tact_doors, deliberately a region
# that does not otherwise change during the run -- poking a moving region would still divergence-
# match if the hash only tracked the units slice.
# The DirectInput divisor a HUMAN-DRIVEN tactical session needs in a VM (the VM-input notes 9e).
#
# 32767/640: a hypervisor presents an ABSOLUTE pointer whose DirectInput range is 0..32767, and the
# game maps counts onto a 640-wide screen -- so this is the scale the device is actually reporting
# at, derived, not tuned. It was proposed on that basis and then WITHDRAWN in 9d, because in the
# shipped path a divisor this large creates a dead zone (the poll's `dwData / divisor` is a signed
# IDIV that carries no remainder, so every count below the divisor contributes exactly zero). With
# dinputto8 in front of the game that objection does not hold -- measured 4.9% swallowed
# event-frames at div=51 against 38% at div=40 without the wrapper -- so the derivation stands
# again, and it is the default rather than something a human has to remember mid-recording.
TACT_PLAY_MOUSE_DIV = 51

# Where a HUMAN-PLAYED session is kept. Outside the lane on purpose -- see the copy site in
# run_tact_play. Gitignored (tmp/), like the divergence-pair archive.
SESSION_ARCHIVE = os.path.join(REPO, "tmp", "tact_sessions")

TACT_LANE = "ui_tact_det"
# ALLOCATED, NOT PICKED (fork F4H). This used to be a hand-chosen 33 under a comment explaining
# which other lanes it was clear of -- true when written, and false by the time the capture suite had
# grown to 36 lanes and taken 33..36 for itself. A lane NUMBER is the machine-wide single-instance
# mutex ("MHMutNN"), so the collision kills the second game at boot with no log line; tools/
# lane_alloc.py carries the whole diagnosis and the gate that keeps the blocks disjoint.
TACT_LANE_NO = lane_alloc.lane("tact", 0)
# Region names, positional, mirroring TACT_HASH_REGIONS[] in addr/mh_regions.gen.h. Same contract as
# mp_analyze.REGION_NAMES: the `TR` line is positional, so this list must stay in that order.
TACT_REGION_NAMES = [
    "tact_units",
    "tact_fx_pool",
    "tact_doors",
    "tact_teleports",
    "tact_char_types",
    "tact_fx_types",
    "tact_fov_stencil",
    "tact_active_unit_count",
    "tact_active_unit_count_cached",
    "tact_squad_size",
    "tact_map_width",
    "tact_map_height",
    "tile_objects",
    "passable",
    # TACT-REC 2026-08-25: an ALIAS of tact_units -- the same bytes with the render-written
    # animation window dropped. It is `excluded` in TACT_HASH_REGIONS[], so it is NOT folded into
    # the combined `T` hash (that value stays comparable with every historical run); it occupies a
    # positional column here and feeds the separate `TS` line.
    "tact_units_sim",
]
# Positional indices into the list above. tact_units is index 0 in TACT_HASH_REGIONS[] and always
# has been (the table is append-only), and the alias is the entry this file just added.
TACT_UNITS_IDX = TACT_REGION_NAMES.index("tact_units")
TACT_UNITS_SIM_IDX = TACT_REGION_NAMES.index("tact_units_sim")
# The byte the second red arm flips: tact_unit_record +0x2b is anim_frame_time, and +0x2b itself is
# the LOW byte of that double -- a mantissa LSB, so the poke is numerically inert and cannot make
# the game do anything it would not otherwise do. It is inside the window the sim slice drops, which
# is the whole point: the full verdict must see it and the sim verdict must not.
TACT_ANIM_POKE_OFF = 0x2B


def _tact_range(spec):
    """'LO-HI' (or 'N') -> (lo, hi); None/'' -> (0, 0), which is how the DLL spells 'off'."""
    if not spec:
        return 0, 0
    if "-" in spec:
        lo, hi = spec.split("-", 1)
    else:
        lo = hi = spec
    return int(lo), int(hi)


def tact_lane_name(slot=0):
    """Slot 0 keeps the historical lane name, so single-arm paths and existing habits are unchanged."""
    return TACT_LANE if slot == 0 else "%s%d" % (TACT_LANE, slot)


def tact_lane_dir(slot=0):
    return os.path.join(make_lane.LANE_ROOT, tact_lane_name(slot))


def tact_write_config(
    lane_dir,
    frames,
    poke_at,
    poke_idx,
    squad,
    hp_pct,
    target_owner,
    synth,
    system=0,
    mouse_absolute=0,
    mouse_div=0,
    mouse_accel=0,
    mouse_trace=0,
    exit_at=None,
    pin_dt_us=16667,
    unit_hash=0,
    detail_lo=0,
    detail_hi=0,
    gate_lo=0,
    gate_hi=0,
    field_lo=0,
    field_hi=0,
    poke_off=0,
    journal_rec=0,
    journal="",
    journal_verify=0,
    hash_step=1,
    profile_hz=0,
    relocate=0,
    relocate_poison=1,
    relocate_corrupt="",
):
    """The lane's harness + [tactical] config for one arm. Rewritten per arm, never appended to --
    an appended ini silently inherits the previous arm's poke.

    `synth` is None (workload off) or the TACT-SYNTH dict the caller built ONCE for the whole
    invocation. Built once on purpose: every arm must carry the SAME seed or the two runs would
    order different things and the comparison would be measuring the runner, not the game."""
    # ONE FILE (fork F2G, D12): the [harness] block is appended to the lane's mh_net.ini at the
    # bottom of this function instead of being written as its own mh_harness.ini, and `enable=1`
    # is what arms the harness -- it used to arm off that file merely existing, which is a
    # configuration written in the filesystem rather than in the config file. Built as a LIST
    # here and spliced there, so there is exactly one [harness] header in the result: a second
    # one would be unreachable to GetPrivateProfile*, which is this module's oldest trap.
    harness_lines = [
        "[harness]",
        "enable=1",
        "; TACT-PREP tactical oracle (tools/test_ui.py --tact-determinism).",
        "; The strategic apparatus stays quiet: a tactical excursion never calls",
        "; llm_strat_sim_step, so none of the step_/synth_/order_ keys can fire.",
        "pin_fpu=1",
        "pin_wallclock=1",
        # 0 FREEZES the pinned clock instead of advancing it -- the discriminator for
        # "is the divergence clock-derived at all", since tactical reads
        # time_GetCurrentTime from 57 sites across 20 functions.
        "pin_clock_dt_us=%d" % pin_dt_us,
        "pin_clock_base_s=1000",
        "pin_rand=1",
        "rand_seed=12345",
        # 0 disables hashing AND its log stream entirely -- tools/tact_profile.py
        # subtracts that row from the next to price the instrument itself.
        "tact_hash_step=%d" % hash_step,
        # An opt-in EIP sampler over the game thread (harness.cpp prof_start). 0 = the
        # thread is never created, so a normal run is unaffected.
        "profile_hz=%d" % profile_hz,
        "tact_stop_step=%d" % frames,
        # TACT-REC: exit once the requested frames are logged. tact_stop_step ends the
        # LOGGING only, and a tactical mission has no end condition, so before this every
        # arm burned the whole --tact-wall budget -- 40 s of wall clock for ~1.6 s of
        # simulation at ~240 frames/s. --tact-wall is now the backstop it reads like.
        "tact_exit_at=%d" % (frames if exit_at is None else exit_at),
        # Sect. 9f: per-RECORD hashes, so a divergence names a unit index and not just
        # "tact_units". Off by default -- it is ~1.2 KB of log a frame.
        "tact_unit_hash=%d" % unit_hash,
        # Chunk hashes inside a unit record, so a divergence names a BYTE OFFSET. Scoped
        # to a unit range because the cost is per unit.
        "tact_detail_lo=%d" % detail_lo,
        "tact_detail_hi=%d" % detail_hi,
        # Sect. 9i: the gate fields of llm_tact_unit_owner_tick, raw, one line per frame.
        "tact_gate_lo=%d" % gate_lo,
        "tact_gate_hi=%d" % gate_hi,
        # Per-FIELD hashes inside a unit record: the resolution tact_detail cannot reach,
        # because a 32-byte chunk straddles cmd_wait_until_time (which the sim slice
        # KEEPS) and anim_frame_time (which it drops).
        "tact_field_lo=%d" % field_lo,
        "tact_field_hi=%d" % field_hi,
        "tact_poke_at=%d" % poke_at,
        "tact_poke_idx=%d" % poke_idx,
        # WHICH BYTE of the poked region to flip. 0 is the region base; pointing it into
        # the animation window is the red arm for the sim/presentation SPLIT (see
        # --tact-selftest's second poked arm).
        "tact_poke_off=%d" % poke_off,
        # TACT-REC. Recording and replaying are mutually exclusive and the DLL refuses
        # the pair at arm time -- see the arm banner.
        "tact_journal_rec=%d" % journal_rec,
        "tact_journal_verify=%d" % journal_verify,
        "tact_journal=%s" % journal,
        # SB-HOSTFREE: the RELOCATING state bind. Set on ONE arm only, so the mode's
        # existing run_a/run_b comparison becomes the item's claim directly -- an
        # in-place tactical excursion against a relocated one, on the per-frame hash the
        # tactical oracle already trusts. The bind moves every MOVABLE region and poisons
        # the .bss it leaves, so a tactical consumer that did not follow the registry
        # reads 0xCD rather than a plausible stale copy.
        "relocate_state=%d" % relocate,
        # A DIAGNOSTIC, never an acceptance: with poison off a consumer that never
        # followed the bind reads a stale but CORRECT copy and agrees with itself, so a
        # green run says the copy and the bind work and nothing about stale readers.
        "relocate_poison=%d" % relocate_poison,
        # THE MUTATION: fill this region's ARENA copy with 0xCD, so a consumer that
        # correctly follows the bind gets garbage and the comparison MUST go red.
        "relocate_corrupt=%s" % relocate_corrupt,
    ] + (
        [
            "; TACT-SYNTH: the synthetic player-command workload. The seed is drawn once",
            "; per INVOCATION and written identically into every arm -- fresh across runs",
            "; (so a fixed-path fluke cannot hide) but shared within one (so the arms",
            "; compare the game rather than each other's dice).",
            "tact_synth=1",
            "tact_synth_seed=%d" % synth["seed"],
            "tact_synth_at=%d" % synth["at"],
            "tact_synth_every=%d" % synth["every"],
            "tact_synth_stop=%d" % frames,
            "tact_synth_units=%d" % synth["units"],
            "tact_synth_direct=%d" % synth["direct"],
        ]
        if synth
        else []
    )
    # The lane's own mh_net.ini carries its identity (lane number, headless); re-emit it rather than
    # appending, then add the [tactical] and [harness] blocks.
    ident = make_lane.read_identity(lane_dir) or {}
    lines = ["[net]", "enable=1"]
    if ident.get("port"):
        lines.append("port=%d" % ident["port"])
    # `lane` MOVED INTO [uitest] at fork F2G (its own one-key [test] section is refused now).
    lines += [
        "",
        "[uitest]",
        "lane=%d" % ident.get("lane", TACT_LANE_NO),
        "",
        "[video]",
        "size_mode=0",
    ]
    if ident.get("headless", True):
        lines += ["no_present=1", "no_window=1"]
    lines += [
        "",
        "[tactical]",
        "squad=%d" % squad,
        "hp_pct=%d" % hp_pct,
        "commando=0",
        "target_owner=%d" % target_owner,
        "target_building=0",
        # WHICH MISSION. 0 = the save's own CurrentSystem (the shipping default, changes nothing).
        # >0 overrides it, which is the only way to reach all six shipped POZ files from one save --
        # a save pins exactly one system, and the O/L half comes from target_owner's race. The
        # override makes the run a MEASUREMENT run: the DLL says so in mh_launch.log and does not
        # restore the value.
        "system=%d" % system,
        "",
        "[input]",
        # TACT-REC: the VM mouse fix. 1 = drop the DirectInput mouse device so llm_input_wndproc_tap
        # falls through to its ABSOLUTE client-coordinate arm. A hypervisor hands the guest an
        # absolute pointer, whose synthesised relative counts trip the DI path's 2x ballistic boost
        # and slam the cursor into the clamp -- unplayable, and it reproduces on stock retail.
        # 0 for automated runs: they inject into the event ring directly and never touch either path.
        "mouse_absolute=%d" % mouse_absolute,
        # TUNE the DirectInput path instead of replacing it. Ship values are div=1 (no attenuation
        # at all) and accel=100 (|delta| past it is DOUBLED). 0 here means "leave the shipped value
        # alone" for both -- and for div that is not merely a convention: it is a SIGNED IDIV
        # divisor, so writing a real 0 would fault the game.
        "mouse_div=%d" % mouse_div,
        "mouse_accel=%d" % mouse_accel,
        # Diagnostic only, no behaviour change: per-present ring telemetry into mh_uidrive.log.
        "mouse_trace=%d" % mouse_trace,
        "",
    ]
    lines += harness_lines + [""]
    with open(os.path.join(lane_dir, "mh_net.ini"), "w", newline="\r\n") as f:
        f.write("\n".join(lines))


def tact_journal_drop_clicks(src, dst, window=90):
    """Copy `src` with one whole mouse CLICK -- a press and its release -- removed.

    The mutation for the verify arm, and what gets removed is the point. Deleting a random record
    proves little: two thirds of the journal is cursor motion, and a dropped move is re-stated by
    the next one a frame later. A click is a CAUSE -- it selects, it orders -- so removing one
    removes something whose effect the comparison is already watching for.

    THE EVENT TYPES ARE A BITMASK OF BUTTON TRANSITIONS, read off a real recording rather than
    assumed: 1 = move (8,925 of them), 2 = left down and 4 = left up (60 each), 8 = right down and
    16 = right up (133 each). The first cut of this mutation looked for types "2 or 3" in a fixed
    90-frame window before an order, found nothing -- type 3 does not exist and left-clicks are
    sparse -- and reported that it could not build a mutation at all. Which is the right failure to
    have had: an arm that cannot mutate says so instead of passing.

    Returns (path, press_frame, n_removed) or (None, 0, 0)."""
    DOWN = {"2": "4", "8": "16"}  # press -> its matching release
    ev = tact_journal_read(src)
    presses = [(n, f, p) for n, (f, k, p) in enumerate(ev) if k == "M" and p[2] in DOWN]
    if not presses:
        return None, 0, 0
    # The middle click, for the same reason the shift arm takes a middle order: an early one may
    # land before the units are doing anything, and a divergence that would have happened anyway is
    # not evidence.
    idx, frame, press = presses[len(presses) // 2]
    doomed = {" ".join(press)}
    want = DOWN[press[2]]
    for _f, k, p in ev[idx + 1 :]:
        if k == "M" and p[2] == want:
            doomed.add(" ".join(p))
            break
    n = 0
    with (
        open(src, encoding="utf-8", errors="replace") as fh,
        open(dst, "w", encoding="utf-8", newline="\n") as out,
    ):
        for line in fh:
            if line.strip() in doomed:
                n += 1
                continue
            out.write(line)
    return dst, frame, n


def tact_last_frame(run_dir):
    """The last tactical frame a run actually reached, or None.

    Read from the hash stream rather than from any report line, deliberately: a run killed by the
    wall never writes its report, and the whole point of this reading is to characterise runs that
    were killed."""
    last = None
    for ln in open(os.path.join(run_dir, "mh_harness.log"), encoding="utf-8", errors="replace"):
        if ln.startswith("T "):
            try:
                last = int(ln.split()[1])
            except (IndexError, ValueError):
                pass
    return last


def tact_orders_emitted(run_dir):
    """The player orders the GAME emitted during a run, as comparable tuples.

    Frame-keyed and kind-keyed, with the raw fields kept: this is what gets compared against the
    recording's own records, so it has to preserve everything a divergence could live in."""
    out = []
    for ln in open(os.path.join(run_dir, "mh_harness.log"), encoding="utf-8", errors="replace"):
        if ln.startswith("TJ E ") or ln.startswith("TJ G ") or ln.startswith("TJ D "):
            p = ln[3:].split()
            out.append((int(p[1]), p[0], tuple(p[2:])))
    return out


def tact_queue_observed(run_dir):
    """The order stream as the HOOK-FREE queue watch saw it: [(frame, unit, slot, flag, op, a0..a3)].

    Separate from tact_orders_emitted() on purpose -- these are the SAME events read through an
    instrument with a different blind spot, and conflating them would throw away the only part of
    the comparison that survives a promotion. `TJ Q` records come from polling the command queue at
    the top of each frame, so they see an order whoever issued it: the original body, a rebound call
    site, or one of our own bodies calling its own sibling. `TJ E`/`TJ G` come from trampolines on
    the original entries and see only the first of those three."""
    out = []
    for ln in open(os.path.join(run_dir, "mh_harness.log"), encoding="utf-8", errors="replace"):
        if ln.startswith("TJ Q "):
            p = ln[5:].split()
            out.append((int(p[0]), tuple(p[1:])))
    return out


def tact_queue_diff(a, b, slack=2):
    """(only_in_a, only_in_b) between two runs' queue-watch streams, with the same frame slack.

    Reuses tact_verify_diff's matching so the two order comparisons cannot drift apart in how they
    treat the one-frame convention -- the slack is a property of the recorder, not of the caller."""
    ax = [(f, "Q", fields) for f, fields in a]
    bx = [(f, "Q", fields) for f, fields in b]
    matched, missing, extra, shifted = tact_verify_diff(ax, bx, slack=slack)
    return matched, missing, extra, shifted


def tact_verify_diff(recorded, emitted, slack=2):
    """Compare two order streams and return (matched, missing, extra, shifted).

    THE FRAME SLACK IS DELIBERATE and is the one concession this comparison makes. Input is observed
    at the top of a frame but produced during the previous one, so a replayed event lands one frame
    later than it originally did (the one-frame convention, harness.cpp). A uniform shift is not a
    behavioural difference, so an order matching in KIND and FIELDS within +/-`slack` frames counts
    as matched, and its shift is reported separately -- if the shifts are all the same number, that
    is the convention showing through; if they scatter, that is timing drift and worth seeing.

    What it does NOT forgive: a missing order, an extra one, or one whose fields differ. Those are
    the semantic failures -- the class that let a 63,563-frame recording replay to zero combat while
    every hash arm stayed green."""
    pool = {}
    for f, k, fields in emitted:
        pool.setdefault((k, fields), []).append(f)
    for v in pool.values():
        v.sort()

    matched, missing, shifted = [], [], []
    for f, k, fields in recorded:
        cands = pool.get((k, fields))
        hit = None
        if cands:
            best = min(cands, key=lambda g: abs(g - f))
            if abs(best - f) <= slack:
                hit = best
        if hit is None:
            missing.append((f, k, fields))
        else:
            pool[(k, fields)].remove(hit)
            matched.append((f, k, fields))
            if hit != f:
                shifted.append(hit - f)
    extra = [(f, k, fields) for (k, fields), fs in pool.items() for f in fs]
    extra.sort()
    return matched, missing, extra, shifted


def run_tact_trim(args):
    """Trim a journal and PROVE the trim by replaying it, rather than assuming it is harmless."""
    if not os.path.isfile(args.tact_trim):
        print("FAIL: no such journal: %s" % args.tact_trim)
        return 1
    dst = args.tact_trim.replace(".journal", "") + "-trimmed.journal"
    path, kept, dropped = tact_journal_trim(args.tact_trim, dst, args.tact_trim_lead)
    if not path:
        print("FAIL: nothing to trim")
        return 1
    before = os.path.getsize(args.tact_trim)
    after = os.path.getsize(path)
    print("  trimmed   %s" % path)
    print(
        "            kept %d record(s), dropped %d (%.1f%%); %.0f KB -> %.0f KB"
        % (kept, dropped, 100.0 * dropped / (kept + dropped), before / 1024, after / 1024)
    )
    print("")
    print("  Now PROVING it -- a trim is a claim about what the simulation depends on, and the")
    print("  cursor also drives unit facing and camera scroll (which reaches animation, 9k).")
    print("")
    sub_args = argparse.Namespace(**vars(args))
    sub_args.tact_verify = path
    sub_args.tact_trim = None
    return run_tact_verify(sub_args)


def tact_rand_series(run_dir):
    """The pinned RNG draw counter per `; TACT CLOCK` line: [(tact_frame, draws, state_hash)].

    The cheapest first-divergence signal this rig produces, and it is already in every tactical log.
    It sees drift THOUSANDS of frames before the order stream does: on poz1_combat the counters agree
    at frames 1 and 513 and part at 1025, while the earliest order-level symptom is at frame 2765.
    """
    out = []
    path = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(path):
        return out
    pat = re.compile(r"tact_frame=(\d+) rand=(\d+)/([0-9A-F]+)")
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            if ln.startswith("; TACT CLOCK"):
                m = pat.search(ln)
                if m:
                    out.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    return out


def run_tact_equiv(args):
    """SHIP config vs the WHOLE DLL ON ORIGINAL BODIES, over one recorded journal.

    WHY THIS AND NOT `--tact-verify`'s absolute 211-of-211. That number is a comparison against a
    session recorded on 2026-08-25, and it is NOT reproducible: replaying the same journal at its own
    2026-09-02 commit today yields 71/211, and the RNG draw counters diverge from the recording at
    frame 1025 even with every promotion and every rebind row rolled back. Something in the
    environment moved that our code does not control, so an absolute gate would either be permanently
    red or would have to be re-baselined to whatever the tree happens to do -- and a gate re-baselined
    to current behaviour cannot fail.

    The differential IS sound, because both arms run in the SAME environment minutes apart: whatever
    drifted since August cancels, and what remains is exactly the question worth gating -- does
    running OUR bodies change what the player's input causes? That is not a hypothetical: it is the
    measurement that found the tact_frame promotion dropping 184 of 211 orders (27 vs 208) after a
    12000-frame all-AI trajectory A/B had called the same body equivalent.

    Three verdict signals, because they fail at different depths: the ORDER STREAM as the hook-free
    queue watch sees it (what the player's input actually caused), the pinned RNG counter series
    (drift, thousands of frames early), and the mission's own end frame + casualties.

    The order signal is a VERDICT only because it is watched rather than hooked. It used to be a
    printed note explicitly marked not-to-be-believed: the E/G records come from trampolines on the
    ORIGINAL order entries, and a promoted body calling its own sibling crosses neither, so the diff
    went silent exactly when our code started running (zero E and zero G under
    `[promote] tact_frame=1`, on both recorded sessions, with every other signal identical). The
    `TJ Q` stream polls the command queue instead, which no promotion or rebind can route around."""
    journal = args.tact_equiv
    if not os.path.isfile(journal):
        print("FAIL: no such journal: %s" % journal)
        return 1
    ev = tact_journal_read(journal)
    recorded = [(f, k, tuple(p[2:])) for f, k, p in ev if k in ("E", "G", "D")]
    last = max(f for f, _k, _p in ev)
    frames = last + 200
    wall = max(args.tact_wall, int(frames / 55) + 30)

    # THE JOURNAL DEFINES THE SESSION, so its own provenance header wins over the CLI defaults.
    # Without this the lane is provisioned from `--tact-system`'s default (0 = "whatever the save
    # says") and a journal recorded on another mission replays against the wrong one -- see
    # tact_journal_meta() for the measurement.
    jm = tact_journal_meta(journal)
    for key, attr in (("system", "tact_system"), ("squad", "tact_squad"), ("owner", "tact_owner")):
        raw = jm.get(key)
        if raw is not None and str(raw).strip().isdigit():
            setattr(args, attr, int(str(raw).strip()))
    if jm.get("save"):
        args.tact_save = jm["save"]

    # Slot-suffixed, because the suite tail now runs journals CONCURRENTLY (2026-09-10): four
    # equivs writing one shared tmp/tact_all_original.ini is a write-while-read race even though
    # the content is identical.
    slot = int(getattr(args, "tact_slot", 0) or 0)
    rollback = os.path.join(
        REPO, "tmp", "tact_all_original%s.ini" % ("" if slot == 0 else "_%d" % slot)
    )
    ctrl_mode = write_all_original_ini(rollback)
    print("  journal   %s" % journal)
    print(
        "  recorded  system=%s save=%s squad=%s owner=%s roster=%s (from the journal header)"
        % (
            jm.get("system", "?"),
            jm.get("save", "?"),
            jm.get("squad", "?"),
            jm.get("owner", "?"),
            jm.get("roster_total", "?"),
        )
    )
    print("  arms      SHIP vs ALL-ORIGINAL ([config] mode=%s)" % ctrl_mode)

    lane_dir = tact_provision_lane(args, slot=slot)
    if lane_dir is None:
        return 1

    arms = {}
    for name, frag in (("ship", None), ("original", rollback)):
        tact_write_config(
            lane_dir,
            frames,
            0,
            -1,
            args.tact_squad,
            args.tact_hp,
            args.tact_owner,
            None,
            args.tact_system,
            journal=os.path.abspath(journal),
            journal_verify=1,
        )
        # The SHIP arm takes --extra-ini so a specific configuration can be put on trial against
        # the original (that is how this gate was mutation-checked: `[promote] tact_frame=1` must
        # make it red). The ORIGINAL arm never does -- it is the fixed reference, and letting a
        # fragment reach it would let a caller quietly move both sides and call the result agreement.
        frags = [frag] if frag else list(getattr(args, "extra_ini", None) or [])
        if frags:
            try:
                tact_merge_ini(lane_dir, frags)
            except FileNotFoundError as e:
                print("FAIL: %s" % e)
                return 1
        run_dir = tact_run_arm(lane_dir, args.tact_save, wall)
        if not run_dir:
            print("FAIL: arm %s produced no run folder" % name)
            return 1
        emitted = tact_orders_emitted(run_dir)
        matched, missing, extra, _shift = tact_verify_diff(recorded, emitted)
        lost, surv, combat_line = tact_combat(run_dir)
        arms[name] = {
            "reach": tact_last_frame(run_dir),
            "matched": len(matched),
            "missing": missing,
            "extra": extra,
            "lost": lost,
            "surv": surv,
            "combat": combat_line,
            "rand": tact_rand_series(run_dir),
            "queue": tact_queue_observed(run_dir),
        }
        good, why = tact_journal_roster_guard(journal, name, combat_line)
        if not good:
            print("FAIL: %s" % why)
            return 1
        print(
            "  arm %-9s reach=%s matched=%d/%d queue=%d lost=%s"
            % (
                name,
                arms[name]["reach"],
                len(matched),
                len(recorded),
                len(arms[name]["queue"]),
                lost,
            )
        )

    a, b = arms["ship"], arms["original"]
    ok = True
    # A vacuity guard first: two arms that measured nothing agree perfectly.
    #
    # KEYED ON THE RNG SERIES AND THE QUEUE WATCH, NEVER ON `matched`. `matched` counts the
    # ENTRY-HOOKED E/G/D stream, and the promotion pins its E/G half at zero by construction (G120)
    # -- so using it as "did this run measure anything" makes the guard fire on healthy runs and stay
    # quiet on the ones it was built for. It did exactly that on the POZ3 journal: 15,428 queue
    # orders observed identically by both arms, reported as "the ship arm measured nothing".
    # The queue-stream guard below is the real one; this half only checks the RNG series.
    if not a["rand"]:
        print(
            "FAIL: the ship arm logged no RNG samples -- a comparison over an empty run is not "
            "a pass."
        )
        ok = False
    # THE ORDER STREAM, AS A VERDICT. This is the signal the entry-hooked E/G diff below could never
    # be: the queue watch polls state, so it sees an order whichever body issued it, and a promotion
    # cannot make it go quiet. Its vacuity guard is separate and load-bearing -- a run that recorded
    # NO orders would otherwise "agree" with another that recorded none, which is exactly the empty
    # pass this gate exists to refuse.
    if not a["queue"]:
        print(
            "FAIL: the ship arm's queue watch observed NO orders at all -- either the journal drives "
            "nothing or the watch is not armed. Agreement over an empty stream is not a pass."
        )
        ok = False
    else:
        q_matched, q_missing, q_extra, q_shift = tact_queue_diff(a["queue"], b["queue"])
        if q_missing or q_extra:
            print(
                "FAIL: the ORDER STREAM diverges -- %d order(s) the original issued that ship did "
                "not, %d that ship issued and the original did not (of %d / %d observed)"
                % (len(q_missing), len(q_extra), len(a["queue"]), len(b["queue"]))
            )
            for lbl, rows in (("ship MISSING", q_missing), ("ship EXTRA", q_extra)):
                for f, _k, fields in rows[:8]:
                    print(
                        "      %-12s frame %-7s unit %-4s slot %-4s op %s args %s"
                        % (lbl, f, fields[0], fields[1], fields[3], " ".join(fields[4:]))
                    )
                if len(rows) > 8:
                    print("      %-12s ... and %d more" % (lbl, len(rows) - 8))
            ok = False
        elif q_shift and len(set(q_shift)) > 1:
            # A UNIFORM shift is the one-frame convention; a SCATTERED one is timing drift between
            # the arms, which the RNG series would normally catch first but need not.
            print(
                "  note      %d queue order(s) landed off-frame by %s -- scattered, not the uniform"
                % (len(q_shift), sorted(set(q_shift)))
            )
            print("            one-frame convention. Worth a look; not failed on alone.")
    if a["rand"] != b["rand"]:
        first = next((i for i, (x, y) in enumerate(zip(a["rand"], b["rand"])) if x != y), None)
        where = (
            (
                "frame %d: ship %d/%s vs original %d/%s"
                % (
                    a["rand"][first][0],
                    a["rand"][first][1],
                    a["rand"][first][2],
                    b["rand"][first][1],
                    b["rand"][first][2],
                )
            )
            if first is not None
            else "sample counts differ (%d vs %d)" % (len(a["rand"]), len(b["rand"]))
        )
        print("FAIL: the RNG draw series DIVERGES -- %s" % where)
        print("      Our bodies changed how much randomness the sim consumed. This is the earliest")
        print("      signal available; the order diff below may still look fine.")
        ok = False
    for field, label in (
        ("reach", "last tactical frame"),
        ("lost", "units lost"),
        ("combat", "combat census"),
    ):
        if a[field] != b[field]:
            print("FAIL: %s differs -- ship %s, original %s" % (label, a[field], b[field]))
            ok = False
    # THE ENTRY-HOOKED E/G DIFF IS ADVISORY, and it stays advisory for a reason that is now MEASURED
    # rather than argued. Those records come from trampolines on the ORIGINAL entries
    # (ADDR_TACT_GROUP / ADDR_TACT_ENQUEUE). A promoted body calling its own translated sibling --
    # ours -> ours -- never crosses either, so its orders are ISSUED and NOT RECORDED. Under
    # `[promote] tact_frame=1` the count goes to ZERO E and ZERO G on both recorded sessions while
    # the outcome, survivor set, end frame and RNG series are byte-identical; the survivors are
    # exclusively `D` records, which come from a state watch and not from a hook. Failing on that
    # difference reports the INSTRUMENT'S BLIND SPOT as a defect in the code -- which it did once,
    # and a ship default was backed off on it before a probe showed the orders were being issued all
    # along.
    #
    # What changed is that the order question is no longer ASKED here. The queue watch above answers
    # it properly, so this is a diagnostic on the two instruments rather than a claim about the game:
    # a gap between them is the measure of how much of the order path has moved onto our bodies.
    if a["matched"] != b["matched"] or a["missing"] != b["missing"] or a["extra"] != b["extra"]:
        # NAME THE DIMENSION THAT ACTUALLY DIFFERS. Reporting only `matched` printed
        # "streams differ (ship 24, original 24)" on the quiet journal -- a line that contradicts
        # itself in its own sentence, which is how a reader learns to skim past a diagnostic.
        bits = []
        if a["matched"] != b["matched"]:
            bits.append("matched %d vs %d" % (a["matched"], b["matched"]))
        if a["missing"] != b["missing"]:
            bits.append("missing %d vs %d" % (len(a["missing"]), len(b["missing"])))
        if a["extra"] != b["extra"]:
            bits.append("extra %d vs %d" % (len(a["extra"]), len(b["extra"])))
        print("  note      the ENTRY-HOOKED order streams differ (%s) -- the" % ", ".join(bits))
        print(
            "            hook-free queue watch above is the verdict and it %s."
            % ("AGREES" if ok else "is reported above")
        )
        print("            A gap here means our bodies are issuing orders the trampolines on the")
        print("            original entries no longer see. Diagnostic, not a failure.")
    print("tact-equiv: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def run_tact_suite(args):
    """Every registered tactical journal, through both arms.

    TWO ARMS PER SCENARIO, because they answer different questions and neither implies the other:
    `verify` asks whether the replayed input re-causes the recorded session (semantic), `replay`
    asks whether the replay is deterministic and depends on its journal at all (hashes). A journal
    can pass either alone while failing the other -- a deterministic replay of the wrong session
    passes `replay`, and a semantically correct replay with a nondeterministic sim passes `verify`."""
    if not TACT_SCENARIOS:
        print("FAIL: no tactical scenarios registered.")
        return 1
    rc = 0
    for sc in TACT_SCENARIOS:
        path = os.path.join(REPO, sc["journal"])
        print("=" * 78)
        print("tactical scenario: %s -- %s" % (sc["name"], sc["desc"]))
        print("=" * 78)
        if not os.path.isfile(path):
            print("FAIL: %s: its journal is missing (%s)" % (sc["name"], sc["journal"]))
            rc = 1
            continue
        for arm in sc["arms"]:
            sub_args = argparse.Namespace(**vars(args))
            sub_args.tact_verify = path if arm == "verify" else None
            sub_args.tact_replay = path if arm == "replay" else None
            fn = run_tact_verify if arm == "verify" else run_tact_replay
            if fn(sub_args):
                rc = 1
    print("tact-suite: %s" % ("PASS" if rc == 0 else "FAIL"))
    return rc


def run_tact_verify(args):
    """TACT-REC: the SEMANTIC arm -- replay the input, compare the orders it caused.

    WHY THIS EXISTS, and why the hash arms do not replace it. Two replays hashing identically proves
    the REPLAYER is deterministic; it says nothing about whether the replay is the same session the
    human played. The distinction is not academic -- a real 63,563-frame recording once replayed to
    zero combat while every hash arm passed, because the journal was missing selection and every
    group order applied to an empty set. A stream of orders the game emitted BY ITSELF, compared
    against the stream a human caused, is the arm that catches that on the first run.

    The mode replays ONLY the input records and withholds the derived ones (E/G/D). If the input
    journal is complete, the game re-emits them unaided and the two streams agree. If it is not, the
    missing records name precisely what input does not reproduce."""
    if not os.path.isfile(args.tact_verify):
        print("FAIL: no such journal: %s" % args.tact_verify)
        return 1
    ev = tact_journal_read(args.tact_verify)
    recorded = [(f, k, tuple(p[2:])) for f, k, p in ev if k in ("E", "G", "D")]
    n_input = len(ev) - len(recorded)
    if not recorded:
        print(
            "FAIL: %s holds no E/G/D records, so there is nothing to compare the replay against. "
            "An input-only journal can be replayed but not VERIFIED." % args.tact_verify
        )
        return 1
    if not n_input:
        print(
            "FAIL: %s holds no M/K/C records -- it is an order journal, and verify mode replays "
            "INPUT. There is nothing to drive the run with." % args.tact_verify
        )
        return 1

    last = max(f for f, _k, _p in ev)
    frames = args.tact_frames if args.tact_frames > last else last + 200
    wall = max(args.tact_wall, int(frames / 55) + 30)  # 81 frames/s measured, +45% margin
    print("  journal   %s" % args.tact_verify)
    print(
        "            %d input record(s) replayed; %d order/direct record(s) held back as the "
        "expectation" % (n_input, len(recorded))
    )
    print("            %d frame(s), wall %d s" % (frames, wall))

    lane_dir = tact_provision_lane(args)
    if lane_dir is None:
        return 1
    tact_write_config(
        lane_dir,
        frames,
        0,
        -1,
        args.tact_squad,
        args.tact_hp,
        args.tact_owner,
        None,
        args.tact_system,
        journal=os.path.abspath(args.tact_verify),
        journal_verify=1,
    )
    if tact_apply_extra_ini(lane_dir, args) is None:
        return 1
    print("  arm verify ...")
    run_dir = tact_run_arm(lane_dir, args.tact_save, wall)
    if not run_dir:
        print("FAIL: the verify arm produced no run folder")
        return 1

    ok = True
    armed = tact_journal_armed(run_dir)
    dropped = tact_journal_dropped(run_dir)
    if armed is None:
        print("FAIL: the journal never armed.")
        return 1
    if armed != len(ev):
        print("FAIL: armed %d record(s) from a journal holding %d." % (armed, len(ev)))
        ok = False
    if dropped:
        print(
            "FAIL: %d record(s) REJECTED by the parser -- one unreadable FIELD discards the "
            "WHOLE record, and the run still reports ARMED." % dropped
        )
        ok = False

    # THE ARM MUST OUTLIVE THE LAST THING IT IS CHECKING FOR. A run killed by the wall stops
    # emitting orders, and every recorded order after that point would be reported as MISSING --
    # or, worse, if the comparison were ever loosened, a short run that matched its prefix would
    # read as a pass. Neither is a statement about the journal, so establish the run's reach first
    # and refuse to interpret a comparison that outran it.
    reached = tact_last_frame(run_dir)
    need = max(f for f, _k, _p in recorded)
    print("  reach     ran to frame %s; last recorded order is at %d" % (reached, need))
    if reached is None or reached < need:
        print(
            "FAIL: the arm stopped at frame %s, BEFORE the last recorded order at %d. Any "
            "comparison below covers a prefix of the session only -- raise --tact-wall."
            % (reached, need)
        )
        return 1

    emitted = tact_orders_emitted(run_dir)
    matched, missing, extra, shifted = tact_verify_diff(recorded, emitted)
    print("  emitted   %d player order/direct record(s) from replayed input alone" % len(emitted))
    print("  matched   %d of %d recorded" % (len(matched), len(recorded)))
    if shifted:
        uniq = sorted(set(shifted))
        print(
            "  shift     %d matched record(s) landed off-frame by %s%s"
            % (len(shifted), uniq[:6], " ..." if len(uniq) > 6 else "")
        )
        if len(uniq) == 1:
            print("            one uniform offset -- the one-frame convention, not drift")

    def show(label, rows):
        print("  %-9s %d" % (label, len(rows)))
        for f, k, fields in rows[:8]:
            print("            frame %-6d %s %s" % (f, k, " ".join(fields)))
        if len(rows) > 8:
            print("            ... and %d more" % (len(rows) - 8))

    if missing:
        show("MISSING", missing)
        print(
            "            ^ the human's input caused these and the replay's did not. Each one is "
            "an action the input journal does not reproduce."
        )
        ok = False
    if extra:
        show("EXTRA", extra)
        print("            ^ the replay emitted these and the recording did not.")
        ok = False

    lost, surv, combat_line = tact_combat(run_dir)
    print("  combat    %s" % (combat_line or "NO TACT COMBAT LINE"))
    print("  survivors owner0 %s" % (surv or "(none)"))
    if not lost:
        print("FAIL: the verify replay reached no combat (units_lost_total=%s)." % lost)
        ok = False

    if not ok:
        print("tact-verify: FAIL")
        return 1

    # ---- THE NEGATIVE ARM ------------------------------------------------------------------------
    # Everything above says the orders matched. It does NOT yet say they matched BECAUSE of the
    # replayed input -- a mission whose save already contains the same scripted situation could
    # produce the same orders with no input at all, and this arm would applaud. So remove a cause
    # and require the effect to go.
    mpath, mframe, mcount = tact_journal_drop_clicks(
        args.tact_verify, os.path.join(REPO, "tmp", "tact_verify_mutated.journal")
    )
    if not mpath:
        print(
            "  NEG ARM: FAIL -- could not build a mutated journal, so nothing here shows the "
            "match depends on the input at all."
        )
        return 1
    print("")
    print("  NEG ARM: removed one whole click -- %d event(s), press at frame %d" % (mcount, mframe))
    tact_write_config(
        lane_dir,
        frames,
        0,
        -1,
        args.tact_squad,
        args.tact_hp,
        args.tact_owner,
        None,
        args.tact_system,
        journal=os.path.abspath(mpath),
        journal_verify=1,
    )
    print("  arm mutated ...")
    mrun = tact_run_arm(lane_dir, args.tact_save, wall)
    if not mrun:
        print("  NEG ARM: FAIL -- the mutated arm produced no run folder")
        return 1
    m_emitted = tact_orders_emitted(mrun)
    m_matched, m_missing, m_extra, _sh = tact_verify_diff(recorded, m_emitted)
    # REPORT THE MUTATED ARM'S REACH TOO. Its pass condition is "something differed", which a
    # truncated run can only UNDER-report -- so truncation makes this arm stricter, never vacuous,
    # and it is safe to interpret. But the MAGNITUDE scales with how far the run got: the same
    # mutation read as "1 missing" under a wall that cut the run short and "27 missing" once it ran
    # to the end. Printing the reach stops that from looking like instability.
    m_reach = tact_last_frame(mrun)
    print(
        "           ran to frame %s; emitted %d, matched %d of %d, missing %d, extra %d"
        % (m_reach, len(m_emitted), len(m_matched), len(recorded), len(m_missing), len(m_extra))
    )
    if not m_missing and not m_extra:
        print(
            "  NEG ARM: FAIL -- deleting %d button press(es) changed NOTHING. The orders above "
            "are not being caused by the replayed input, so the match is measuring something "
            "else." % mcount
        )
        return 1
    print(
        "  NEG ARM: ok -- the mutation cost %d recorded order(s) and added %d spurious one(s), so "
        "the match above is genuinely input-driven" % (len(m_missing), len(m_extra))
    )

    print(
        "tact-verify: PASS -- replayed input re-caused the recorded session, and removing input "
        "breaks it"
    )
    return 0


def run_tact_replay(args):
    """TACT-REC clauses 2-4: replay a journal, twice, and say what was compared.

    THREE ARMS, and each answers a question the others cannot:

      run_a / run_b   TWO replays of the same journal. Byte-identical is clause 2 -- but on its own
                      it only proves the REPLAYER is deterministic. A replay that ignored the journal
                      entirely would pass this perfectly, which is why the third arm exists.
      shifted         the SAME journal with ONE order moved forward a frame. It MUST diverge, at or
                      after that frame. This is the arm that proves the run depends on the journal
                      at all.

    Combat is REPORTED, not assumed (clause 3): the verdict prints units_lost_total and the
    squad-survivor set, and refuses a journal that reached no engagement."""
    if not os.path.isfile(args.tact_replay):
        print("FAIL: no such journal: %s" % args.tact_replay)
        return 1
    ev = tact_journal_read(args.tact_replay)
    if not ev:
        print(
            "FAIL: %s contains no E/G/D records -- an empty journal replays as an idle run "
            "and would compare identical for free." % args.tact_replay
        )
        return 1
    last = max(f for f, _k, _p in ev)
    frames = args.tact_frames if args.tact_frames > last else last + 200
    print("  journal   %s" % args.tact_replay)
    # THE WALL HAS TO SCALE WITH THE JOURNAL. --tact-wall defaults to 40 s, which was sized for a
    # 400-frame determinism probe; a recorded human session is two orders of magnitude longer. The
    # first replay of a real 15,831-frame recording was killed at frame 8,468 with the report line
    # never written, and the arm failed as "the journal never armed" -- a diagnosis pointing at the
    # journal when the truth was a stopwatch. Measured headless throughput on this rig is ~210
    # frames/s -- but that was measured on a 400-frame probe with an empty journal. MEASURED on a
    # real 14,317-record replay it is 81 frames/s: injecting a human's input and letting the game
    # consume it is not free, and the first scaled attempt was still killed (14,681 of 16,031) by a
    # wall derived from the empty-journal figure. 55 is 81 with ~45% of margin, plus 30 s of launch
    # and menu. --tact-wall stays the floor so an explicit larger value still wins.
    wall = max(args.tact_wall, int(frames / 55) + 30)
    print(
        "            %d record(s), last at frame %d -> replaying %d frame(s)"
        % (len(ev), last, frames)
    )
    print(
        "            wall %d s per arm (scaled from the journal; --tact-wall %d is the floor)"
        % (wall, args.tact_wall)
    )

    shifted_path, shifted_frame, shifted_what = tact_journal_shift(
        args.tact_replay, os.path.join(REPO, "tmp", "tact_shifted.journal")
    )
    arms = [("run_a", args.tact_replay), ("run_b", args.tact_replay)]
    if shifted_path:
        arms.append(("shifted", shifted_path))
    # THE POKE ARM, on the replay path. Everything else here compares one replay against another,
    # so all of it would stay green if the hash went blind to the region it is supposed to watch.
    # This arm corrupts a region mid-run and REQUIRES the comparison to fail -- it is the only check
    # here that fails when the oracle stops reading rather than when the game changes.
    poke_arm = args.tact_poke_at > 0 and args.tact_poke_idx >= 0
    if poke_arm:
        arms.append(("poked", args.tact_replay))

    # ONE LANE PER ARM WHEN PARALLEL, and that is forced rather than chosen: an arm's journal is
    # named in its lane's mh_net.ini, so two arms sharing a lane are mutually exclusive by
    # construction. The arms are ~80 s of simulation each on a box with 8 cores that this rig has
    # been using one of; the UI suite has run --jobs 4 since 2026-08-02.
    #
    # DETERMINISM UNDER PARALLELISM IS NOT ASSUMED -- it is the thing the arms already measure. The
    # clock and rand are pinned, so contention should not reach the simulation; if it did, run_a and
    # run_b would stop hashing alike and the run would fail loudly. It is the one property this
    # cannot quietly break, because breaking it IS the failure the comparison reports.
    # ONE LANE PER ARM, always -- `--tact-jobs` caps CONCURRENCY, not lane count. Sizing the lane
    # pool by the job count instead put two arms in one lane whenever there were more arms than
    # jobs (4 arms, 3 jobs), and they collided on that lane's mh_net.ini and its single-instance
    # mutex: run_a never presented a frame and both it and `poked` reported zero frames. A lane is
    # cheap (3.5 MB, packs shared by symlink); a shared one is not.
    jobs = max(1, min(args.tact_jobs, len(arms)))
    lanes = tact_provision_lanes(args, len(arms))
    if lanes is None:
        return 1
    if jobs > 1:
        print("  running %d arm(s) concurrently, one lane each (--tact-jobs)" % jobs)

    def launch(idx):
        name, journal = arms[idx]
        lane = lanes[idx]
        tact_write_config(
            lane,
            frames,
            args.tact_poke_at if name == "poked" else 0,
            args.tact_poke_idx if name == "poked" else -1,
            args.tact_squad,
            args.tact_hp,
            args.tact_owner,
            None,
            args.tact_system,
            journal=os.path.abspath(journal),
        )
        # Every arm gets the SAME fragment. A rollback that reached only some of them would make
        # the A-vs-B and shifted comparisons meaningless -- they would be comparing configurations,
        # not runs.
        if tact_apply_extra_ini(lane, args) is None:
            return name, None
        return name, tact_run_arm(lane, args.tact_save, wall)

    results, ok = {}, True
    if jobs > 1:
        with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
            done = list(ex.map(launch, range(len(arms))))
    else:
        done = []
        for i in range(len(arms)):
            print("  arm %-8s ..." % arms[i][0])
            done.append(launch(i))
    for name, run_dir in done:
        if not run_dir:
            print("FAIL: arm %s produced no run folder" % name)
            return 1
        results[name] = (tact_read(run_dir), run_dir)
        loaded, issued = tact_journal_verdict(run_dir)
        withheld = tact_journal_withheld(run_dir)
        armed, combined, _p, _k, _syn, simh = results[name][0]
        print(
            "  arm %-8s frames=%d distinct=%d  sim frames=%d distinct=%d  journal issued %s of %s"
            % (
                name,
                len(combined),
                len(set(combined.values())),
                len(simh),
                len(set(simh.values())),
                issued,
                loaded,
            )
        )
        # THE SHAPE RULES, before any hash verdict. Each of these is a way for the comparison below
        # to be true and mean nothing.
        dropped = tact_journal_dropped(run_dir)
        if dropped:
            print(
                "FAIL: arm %s REJECTED %d journal record(s). They are not comments the parser "
                "skipped -- they are records it recognised and could not read, so this replay is "
                "a mutilated copy of the session and every comparison below would be measuring "
                "the damage rather than the game." % (name, dropped)
            )
            ok = False
        if loaded is None:
            armed = tact_journal_armed(run_dir)
            if armed is None:
                print(
                    "FAIL: arm %s logged no TJ REPLAY ARMED line -- the journal never armed "
                    "(bad path? recording and replay both on?)." % name
                )
            else:
                print(
                    "FAIL: arm %s armed %d record(s) but was KILLED before it could report -- it "
                    "reached frame %d of %d. That is the wall clock (%d s), not the journal; "
                    "raise --tact-wall." % (name, armed, len(combined), frames, wall)
                )
            ok = False
        elif loaded == 0:
            print("FAIL: arm %s loaded 0 orders -- it replayed nothing." % name)
            ok = False
        elif loaded != len(ev):
            print(
                "FAIL: arm %s loaded %d record(s) from a journal holding %d. The DLL's own count "
                "disagrees with the file, so something between the two is being lost silently "
                "(TJ_MAX? a record kind the parser does not know?)." % (name, loaded, len(ev))
            )
            ok = False
        elif issued + withheld != loaded:
            print(
                "FAIL: arm %s issued %d and withheld %d of %d record(s) -- the run ended before "
                "the journal did, so this replay covered only part of the session."
                % (name, issued, withheld, loaded)
            )
            ok = False
        if len(set(combined.values())) < 2:
            print(
                "FAIL: arm %s never changed state -- an idle world compares equal for free." % name
            )
            ok = False
    if not ok:
        print("tact-replay: FAIL (shape)")
        return 1

    # ---- clause 2: two replays, byte-identical, and SAY what was compared ----------------------
    ffirst, fn, common, fregions = tact_compare(results["run_a"][0], results["run_b"][0], "full")
    sfirst, sn, _sc, sregions = tact_compare(results["run_a"][0], results["run_b"][0], "sim")
    print(
        "  A vs B  SIM : %s"
        % (
            "IDENTICAL over %d frames" % common
            if sfirst is None
            else "DIVERGED at frame %d (%d frames) regions=%s" % (sfirst, sn, sregions)
        )
    )
    print(
        "  A vs B  FULL: %s"
        % (
            "IDENTICAL over %d frames" % common
            if ffirst is None
            else "DIVERGED at frame %d (%d frames) regions=%s" % (ffirst, fn, fregions)
        )
    )
    if sfirst is not None:
        ok = False
    elif ffirst is not None:
        print(
            "                PRESENTATION DRIFT -- the full hash moved and the SIM hash did not "
            "(the tactical-probe work 9k/9l). Not a simulation divergence."
        )
        if args.tact_strict:
            ok = False

    # ---- clause 3: combat is reported, not assumed --------------------------------------------
    lost, surv, combat_line = tact_combat(results["run_a"][1])
    print("  combat    %s" % (combat_line or "NO TACT COMBAT LINE"))
    print("  survivors owner0 %s" % (surv or "(none)"))
    if lost is None:
        print(
            "FAIL: the replay logged no combat line, so its engagement is UNKNOWN -- which is a"
            " different failure from reaching no combat, and is the harness's fault, not the"
            " journal's."
        )
        ok = False
    elif lost == 0:
        print(
            "FAIL: units_lost_total=0 -- this journal reached NO COMBAT. Clause 3 refuses it: a "
            "recorded session that never engages cannot serve as the trajectory oracle."
        )
        ok = False

    # ---- clause 4, second half: the RED arm on the replay path ----------------------------------
    if poke_arm and "poked" in results:
        pfirst, pn, pcommon, pregions = tact_compare(
            results["run_a"][0], results["poked"][0], "full"
        )
        want = (
            TACT_REGION_NAMES[args.tact_poke_idx]
            if 0 <= args.tact_poke_idx < len(TACT_REGION_NAMES)
            else "?"
        )
        # Byte 0 of tact_units is `type`, which the sim emitter keeps, so the alias column moves too.
        want_full = [want, "tact_units_sim"] if args.tact_poke_idx == TACT_UNITS_IDX else [want]
        if pfirst is None:
            print(
                "  RED ARM: FAIL -- poking %s at frame %d changed NOTHING on the replay path. "
                "The oracle is not reading those bytes, so every comparison above is vacuous."
                % (want, args.tact_poke_at)
            )
            ok = False
        elif pfirst != args.tact_poke_at:
            print(
                "  RED ARM: FAIL -- diverged at frame %d, expected exactly %d (the poke frame)."
                % (pfirst, args.tact_poke_at)
            )
            ok = False
        elif sorted(pregions) != sorted(want_full):
            print(
                "  RED ARM: FAIL -- right frame, wrong region(s): named %s, expected %s."
                % (pregions, want_full)
            )
            ok = False
        else:
            print(
                "  RED ARM: ok -- poking %s at frame %d moved the verdict at exactly frame %d, "
                "naming %s (%d of %d frames differ)"
                % (want, args.tact_poke_at, pfirst, pregions, pn, pcommon)
            )
    elif not poke_arm:
        print(
            "  RED ARM: SKIPPED -- pass --tact-poke-at N --tact-poke-idx I to prove the oracle "
            "still reads the regions it compares on this path."
        )

    # ---- clause 4: the journal negative arm ------------------------------------------------------
    if not shifted_path:
        print(
            "  NEG ARM: FAIL -- could not build a shifted journal, so nothing proves this run "
            "depends on the journal at all."
        )
        ok = False
    else:
        nfirst, nn, _nc, nregions = tact_compare(results["run_a"][0], results["shifted"][0], "full")
        if nfirst is None:
            print(
                "  NEG ARM: FAIL -- %s and the replay did NOT diverge. The run does not depend on "
                "the journal's timing; everything green above is about something else."
                % shifted_what
            )
            ok = False
        elif nfirst < shifted_frame - 1:
            print(
                "  NEG ARM: FAIL -- diverged at frame %d, BEFORE the shifted order at %d. The two "
                "arms differ for a reason that is not the shift." % (nfirst, shifted_frame)
            )
            ok = False
        else:
            print(
                "  NEG ARM: ok -- %s diverged at frame %d naming %s (%d frames differ)"
                % (shifted_what, nfirst, nregions or ["?"], nn)
            )

    print("tact-replay: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def tact_merge_ini(lane_dir, fragments, section="shadow"):
    """Merge ini `fragments` into the lane's mh_net.ini BY SECTION. Returns (merged_text, armed_keys).

    Shared by --tact-arm (TACT-RIG, the shadow vehicle) and the promoted-vs-original A/B driver (TACT-AB, the
    promoted-golden A/B), which arm DIFFERENT sections of the same file -- `[shadow]` and
    `[promote]` -- through the same merge. Extracted rather than copied because the merge is where
    the trap lives: GetPrivateProfile* returns the FIRST matching section, so an APPENDED second
    `[shadow]` (or `[promote]`) block is in the file and unreachable, and the run comes back with
    every key unarmed while looking like a domain nothing calls.

    `armed_keys` is read back OUT of the merged text, from the named section only. Counting every
    `=1` in the file would fold in `enable=1`, `no_present=1` and the rest of the lane's identity and
    print a number that looks like a key count and is not one -- the sort of banner that makes a
    mis-merged fragment invisible. Reading it back rather than echoing what was passed in is the
    same discipline tact_read applies to the DLL's own banners: a caller cannot see the key it
    failed to set.
    """
    net_ini = os.path.join(lane_dir, "mh_net.ini")
    base = open(net_ini, encoding="utf-8").read()
    for frag in fragments:
        path = frag if os.path.isabs(frag) else os.path.join(REPO, frag)
        if not os.path.isfile(path):
            raise FileNotFoundError(path)
        base = ui_test.ini_merge_fragment(base, open(path, encoding="utf-8").read())
    with open(net_ini, "w", newline="\r\n") as f:
        f.write(base)

    armed, in_section = [], False
    for ln in base.splitlines():
        t = ln.strip()
        if t.startswith("["):
            in_section = t.lower() == "[%s]" % section.lower()
        elif in_section and t.endswith("=1") and not t.startswith(";"):
            armed.append(t.split("=")[0])
    return base, armed


# ---- THE D11 CONFIGURATION SELECTOR (fork F2C, 2026-09-12) ---------------------------------------
#
# `[config] mode` (src/mh_dll/mh/config/config.h) is ONE key naming which implementation the process
# runs: `original` (the game's own bodies), `brokered` (today's ship configuration). Every promotion
# default and the rebind-row default derive from it.
#
# WHAT THIS REPLACED, and why the replacement is not merely shorter. The all-original arm used to be
# DERIVED on every write -- 47 `[promote]` keys scraped out of the sources plus all 719 `[rebind]`
# rows read out of libmh_rebind.json -- because a hand-listed rollback goes stale silently, and a
# rollback arm that misses a knob is not a rollback but a quieter copy of the configuration it was
# meant to disprove. That derivation was right for a per-key world and it was still a treadmill:
# rollback_original.ini's own header records three separate gate runs lost to a key that shipped ON
# and was not named. F2A made the configuration a VALUE, so there is nothing left to enumerate, and
# F2A measured the two arms identical -- `mode=original` against the 47-key + 719-row fragment, 8000
# steps x 63 region channels, with perturbation and wrong-seed controls going red on cue.
#
# AND IT FIXES THE ROUTE, not only the size (the F2A hand-off note). The derived fragment harvested
# `wire` -- a key turn_engine.cpp REFUSES by name because C8-d retired it -- so the rollback arm
# reached "not promoted" through `RUN-CONFIG: REFUSED (retired key wire)`, i.e. the stale-config
# refusal, and not through the promotion gate at all. It compared correctly by accident: a fragment
# whose first act is to make the DLL reject its own configuration is not the configuration under
# test. The selector fragment carries no `[promote]` key at all, so `install_promotion` reaches its
# ordinary `lockstep` gate with a default of 0 and returns there. The retired-key refusal is
# untouched and still guards the real case it was built for -- an old fragment on disk naming `wire`
# / `wire_seams` / `lockstep_seams` -- and det_standard_selftest's sixth rule still scans every
# committed fragment for one.
CONFIG_SECTION = "config"
MODE_ORIGINAL = "original"
MODE_BROKERED = "brokered"

# NO BRACKETED `promote`/`rebind` TOKEN IN THIS BANNER, on purpose: F2E's drop-vocabulary gate scans
# for that spelling, and a generated file that trips it would make the gate argue with its own
# tooling. Say what the mode does instead of naming the sections it replaces.
ALL_ORIGINAL_WHY = (
    "; GENERATED by tools/test_ui.py (write_all_original_ini) -- do not hand-edit, do not commit.\n"
    "; THE ALL-ORIGINAL ARM: the game runs its OWN bodies, end to end. One key, because D11 made the\n"
    "; configuration a value -- every promotion default and the rebind-row default derive from it\n"
    "; (src/mh_dll/mh/config/config.h). Proven identical to the 47-key + 719-row fragment this\n"
    "; replaced over 8000 steps x 63 region channels (tracker fork F2A).\n"
)


def write_config_mode_ini(path, mode, banner):
    """Write an --extra-ini fragment selecting ONE D11 configuration. Returns the mode."""
    if mode not in (MODE_ORIGINAL, MODE_BROKERED):
        # `standalone` is a property of the BUILD (MH_LIBMH_BUILD) and is not selectable from an ini;
        # a hosted lane claiming it would be describing a binary layout it does not have.
        raise ValueError("%r is not an ini-selectable [config] mode" % (mode,))
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(banner)
        f.write("[%s]\n%s=%s\n" % (CONFIG_SECTION, "mode", mode))
    return mode


def write_all_original_ini(path):
    """An --extra-ini fragment putting the WHOLE DLL back on the game's original bodies."""
    return write_config_mode_ini(path, MODE_ORIGINAL, ALL_ORIGINAL_WHY)


def ini_selected_mode(text):
    """The `[config] mode` an ini TEXT actually selects, modelling GetPrivateProfile*'s reader.

    None when the file names none -- which is not the same as `brokered`, even though the DLL
    defaults there: a fragment that was supposed to select a mode and does not is the failure this
    exists to catch, and answering `brokered` for it would hide exactly that. Routed through
    ui_test.ini_effective so an UNREACHABLE key -- one in a duplicate `[config]` block, the trap
    ini_merge_section exists for -- reads as absent here too, the way the game reads it.
    """
    return ui_test.ini_effective(text, CONFIG_SECTION, "mode")


def ini_file_mode(path):
    """`ini_selected_mode` for a fragment/lane ini on disk. None if the file is missing."""
    if not os.path.isfile(path):
        return None
    return ini_selected_mode(open(path, encoding="utf-8", errors="replace").read())


def sp_mode_refusal(mode_a, mode_b, na, nb):
    """(message, None) if the two arms select the SAME configuration; (None, note) if they differ.

    THE ARM CHECK, MOVED AHEAD OF THE RUN. sp_arm_report already refuses two promoted arms -- it is
    the check that caught C8-f's own flip, printing "both arms are promoted, so the comparison has
    nothing to say" instead of reading the identical hashes as a pass. That refusal stays; it is the
    one that reads what the DLL actually DID. This one reads what the two arms ASKED FOR, before the
    rig is spent on them, and it is possible only because the configuration is now a single key: two
    arms naming the same `[config] mode` cannot produce a comparison whatever happens at runtime.

    Pure, so --det-selftest can exercise both directions without a rig (the reason sp_speed_refusal
    is a function too, and for the same class of bug: a refusal nobody can watch fail is not a gate).
    """
    if mode_a is None or mode_b is None:
        which = ", ".join(n for n, m in ((na, mode_a), (nb, mode_b)) if m is None)
        return (
            "an arm names NO [config] mode (%s).\n"
            "      The arm fragment is the whole statement of what that arm runs, so a run whose\n"
            "      fragment selects nothing is a run nobody configured -- it would silently take the\n"
            "      shipping default and be reported under the name of the arm it was meant to be."
            % which,
            None,
        )
    if mode_a == mode_b:
        return (
            "both arms select `[config] mode=%s` (%s and %s).\n"
            "      They are the same configuration, so the comparison has nothing to say: it would\n"
            "      be green for the one reason a green here must never mean anything."
            % (mode_a, na, nb),
            None,
        )
    return (None, "arms: %s=%s vs %s=%s -- different configurations" % (na, mode_a, nb, mode_b))


def tact_apply_extra_ini(lane_dir, args):
    """Merge --extra-ini into a tactical lane and REPORT what it did. Returns the merged text, or
    None if a fragment was missing (the caller returns 1).

    EVERY tactical arm must call this. Until 2026-09-04 only --tact-arm did: --tact-play,
    --tact-verify and --tact-replay accepted the flag and dropped it. That is worse than rejecting
    it -- a session played with `--extra-ini tmp/c5_baseline.ini` ran the SHIP config and read as
    evidence that rolling the domain back changed nothing, which is exactly backwards. The three
    journal/play arms are the ones a rollback fragment is FOR: they are how you ask "is this
    regression ours?" of a recorded human session.

    Reporting is per SECTION and counts BOTH polarities, because tact_merge_ini's own `armed` list
    is `[shadow]`-only and counts `=1` alone -- it would print "0 keys" for a rollback fragment, the
    fragment most likely to be passed here. The live count is read back OUT of the merged file, not
    echoed from the fragment: an appended duplicate section is present and unreachable
    (GetPrivateProfile* takes the FIRST match), which is the trap tact_merge_ini documents.
    """
    try:
        merged, _armed = tact_merge_ini(lane_dir, args.extra_ini or [])
    except FileNotFoundError as e:
        print("FAIL: --extra-ini fragment not found: %s" % e)
        return None
    if not getattr(args, "extra_ini", None):
        return merged

    want = {}
    for frag in args.extra_ini:
        path = frag if os.path.isabs(frag) else os.path.join(REPO, frag)
        sect = None
        for ln in open(path, encoding="utf-8"):
            t = ln.strip()
            if t.startswith("["):
                sect = t.strip("[]").lower()
            elif sect and "=" in t and not t.startswith(";"):
                _k, _, v = t.partition("=")
                want.setdefault(sect, [0, 0])[0 if v.strip() == "0" else 1] += 1
    for sect in sorted(want):
        off, on = want[sect]
        live, seen = False, 0
        for ln in merged.splitlines():
            t = ln.strip()
            if t.startswith("["):
                live = t.lower() == "[%s]" % sect
            elif live and "=" in t and not t.startswith(";"):
                seen += 1
        print(
            "  extra-ini [%s] %d off / %d on from the fragment(s); %d key(s) live in the lane ini"
            % (sect, off, on, seen)
        )
    return merged


def run_tact_arm(args):
    """TACT-RIG: ONE tactical arm with an ini fragment armed. The migration loop's tactical vehicle.

    WHY THIS EXISTS AND WHY IT IS NOT --tact-determinism. The determinism mode runs a PAIR and
    compares them; a shadow site needs neither -- it compares our C++ against the original inside
    ONE process and reports its own verdict. Running two arms for it would double the cost and
    produce two verdicts to reconcile.

    WHY IT IS NOT --mode soak / --mode sp. Those are the strategic vehicles, and neither ever sets
    _G_LLM_GAME_MODE to 6. A tactical site armed under either is installed, never entered, and reads
    ZERO CALLS -- which the anti-vacuity rule correctly reports as NOT COVERED, but only after a rig
    run has been spent proving it. This is the vehicle that actually reaches mode 6.

    The fragment is merged into mh_net.ini (where [shadow] and [promote] are read from), BY SECTION
    -- not appended. GetPrivateProfile* returns the FIRST matching section, so an appended second
    [shadow] block would be in the file and unreachable, and the run would come back with every site
    unarmed and look like a domain nothing calls."""
    lane_dir = tact_provision_lane(args, visible=args.visible)
    if lane_dir is None:
        return 1

    # TACT-SYNTH: without this, --tact-arm can only cover autonomous per-frame ticks -- any site
    # reached only through a PLAYER-issued order (llm_tact_group_issue_order,
    # llm_tact_unit_enqueue_command's other call sites) reads zero calls no matter how many frames
    # run, because nothing ever calls the order entry points. Same construction as
    # --tact-determinism's (a single seed, printed for the log), just not compared across a pair.
    synth = None
    if args.tact_synth:
        synth = {
            "seed": args.tact_synth_seed or (int.from_bytes(os.urandom(3), "big") + 1),
            "at": args.tact_synth_at,
            "every": args.tact_synth_every,
            "units": args.tact_synth_units,
            "direct": 0 if args.tact_synth_funnel_only else 1,
        }
        print(
            "  tact_synth: seed=%d at=%d every=%d units=%d direct=%d"
            % (synth["seed"], synth["at"], synth["every"], synth["units"], synth["direct"])
        )

    tact_write_config(
        lane_dir,
        args.tact_frames,
        0,
        -1,
        args.tact_squad,
        args.tact_hp,
        args.tact_owner,
        synth,
        args.tact_system,
    )

    try:
        base, armed = tact_merge_ini(lane_dir, args.extra_ini or [])
    except FileNotFoundError as e:
        print("FAIL: --extra-ini fragment not found: %s" % e)
        return 1
    print(
        "  lane %s, %d frames, %d instrumented site(s) armed from %d fragment(s): %s"
        % (
            os.path.basename(lane_dir),
            args.tact_frames,
            len(armed),
            len(args.extra_ini or []),
            ", ".join(armed) or "(none)",
        )
    )

    # --tact-wall defaults to 40s (sized for the old ~1,600-frame smoke arm; see the constant's own
    # comment). A --tact-frames budget above that at real wall-clock speed needs more: the
    # determinism path already scales (`max(wall, frames/55 + 30)`, 81 fps measured, +45% margin);
    # this path did not, so a plain `--tact-arm --tact-frames 15000` with the default wall got
    # TerminateProcess'd at frame ~8107 -- which reads exactly like a stall (frametime kept
    # incrementing, the tactical hash log simply stopped) until the run dir is actually inspected.
    wall = max(args.tact_wall, int(args.tact_frames / 55) + 30)
    run_dir = tact_run_arm(lane_dir, args.tact_save, wall)
    if not run_dir:
        print("FAIL: the tactical arm produced no run folder")
        return 1
    print("  run dir: %s" % run_dir)

    entered = tact_entered(run_dir)
    print("  %s" % (entered or "NO --tactical line in mh_launch.log"))
    # THE SHAPE RULE, before any verdict is believed: an arm that never reached mode 6 produces a
    # log with no tactical frames at all, and every armed site in it reads zero calls for a reason
    # that has nothing to do with the sites. Say which it was.
    _armed_ok, combined, _per, _poke, _syn, _sim = tact_read(run_dir)
    print("  tactical frames logged: %d" % len(combined))
    if not combined:
        print(
            "FAIL: no tactical frames were logged -- the arm did not reach GAME_MODE 6, so a zero "
            "call count below would be about the LAUNCH, not about the sites."
        )
        return 1
    return 0


_TACT_DESKTOP_HELD = False


_TACT_DESKTOP_LOCK = threading.Lock()


def tact_hold_desktop_once():
    """Create + hold the isolated desktop before the first tactical launch. Idempotent -- and
    locked, because concurrent arms (--tact-jobs, the parallel A/B/C arms, the suite's pooled
    journal tail) can reach the first launch on two threads in the same instant."""
    global _TACT_DESKTOP_HELD
    with _TACT_DESKTOP_LOCK:
        if _TACT_DESKTOP_HELD:
            return
        desktop.hold(DESKTOP)
        _TACT_DESKTOP_HELD = True


class _tact_pid_handle:
    """A subprocess.Popen-shaped wrapper over a bare PID.

    desktop.spawn returns a PID, not a Popen, because it is a raw CreateProcessW. The arm runner
    wants wait(timeout=)/kill(), so give it those over an OpenProcess handle rather than reshaping
    the caller around which launch path was taken -- the two paths must be interchangeable or they
    will drift, and the drift is what put a window on the operator's screen in the first place."""

    def __init__(self, pid):
        self.pid = pid

    def wait(self, timeout=None):
        import ctypes

        SYNCHRONIZE, PROCESS_TERMINATE, PROCESS_QUERY = 0x00100000, 0x0001, 0x0400
        h = ctypes.windll.kernel32.OpenProcess(
            SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY, False, self.pid
        )
        if not h:
            return 0  # already gone
        try:
            ms = 0xFFFFFFFF if timeout is None else int(timeout * 1000)
            if ctypes.windll.kernel32.WaitForSingleObject(h, ms) == 0x102:
                raise subprocess.TimeoutExpired(self.pid, timeout)
            code = ctypes.c_ulong(0)
            ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
            return code.value
        finally:
            ctypes.windll.kernel32.CloseHandle(h)

    def kill(self):
        import ctypes

        h = ctypes.windll.kernel32.OpenProcess(0x0001, False, self.pid)
        if h:
            ctypes.windll.kernel32.TerminateProcess(h, 1)
            ctypes.windll.kernel32.CloseHandle(h)


def tact_run_arm(lane_dir, save, wall_s):
    """Launch one arm and return its run-log directory, or None.

    ON THE ISOLATED DESKTOP when one is in force. This path used to call subprocess.Popen directly
    and so never honoured --desktop, which reaches ui_test.py children through run_ui_test's argv
    and nothing else -- while test_ui.py went on printing "game windows cannot reach your desktop"
    at startup, because that banner is emitted from argument PARSING rather than from anything that
    launches a process. A claim about isolation printed by a code path that does not perform it is
    worse than no claim; every tactical arm this rig has ever run put a window on the operator's
    screen. lpDesktop lives in STARTUPINFO, which subprocess does not expose, so the isolated path
    is a raw CreateProcessW in tools/desktop.py -- the same one ui_test.py uses."""
    exe = os.path.join(lane_dir, "mh.focus.exe")
    before = set(glob.glob(os.path.join(lane_dir, "logs", "*_solo")))

    # THE MACHINE-WIDE BOOT LOCK, held only across the pack-load window. Lanes SHARE their resource
    # packs by symlink; two instances reading them at once lose the race, and rsr::TryReadRsrFile
    # falls through to the same modal a missing disc raises. A modal blocks the frame loop, so the
    # losing peer does not crash -- it silently stops, having armed and presented nothing.
    #
    # ui_test.py has serialised this since 2026-07-28 (measured: of four lanes launched in the same
    # second, three came up with the modal). The tactical runner never took the lock, which cost
    # nothing while it launched one game at a time and became load-bearing the moment --tact-jobs
    # launched three: run_a and run_b simulated 15,853 frames each and the third stalled with an
    # empty harness log. Boot is seconds, an arm is minutes, so this serialises almost nothing.
    with ui_test.boot_lock(os.path.basename(lane_dir)):
        conflict = make_lane.lane_conflict(lane_dir)  # fork F4H
        if conflict:
            print(conflict)
            return None
        if DESKTOP:
            tact_hold_desktop_once()
            proc = _tact_pid_handle(
                desktop.spawn(
                    exe, "--tactical %s --skip-intro" % save, cwd=lane_dir, desktop=DESKTOP
                )
            )
        else:
            proc = subprocess.Popen(
                [exe, "--tactical", save, "--skip-intro"],
                cwd=lane_dir,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        ui_test.wait_past_pack_load(lane_dir, before, pid=getattr(proc, "pid", None))

    # The run has no self-stop: tact_stop_step ends the LOGGING, not the process, and a tactical
    # mission has no enemy-wipe end condition (the tactical-probe work 5b), so wall-clock is the
    # honest bound. The frames are counted from the log, never assumed from the elapsed time.
    # A NON-ZERO SELF-EXIT IS A CRASH, and it has to end the arm here rather than downstream. An
    # arm that dies mid-mission leaves a log that is a valid PREFIX of a healthy one -- every hash
    # line correct, just fewer of them -- so every comparison above still runs and reports the
    # difference in reach / order stream / RNG series as though the two arms had DISAGREED ABOUT
    # THE GAME. --tact-equiv did exactly that on 2026-09-09/10: the ship arm exited 0xC000041D
    # (STATUS_FATAL_USER_CALLBACK_EXCEPTION -- an exception escaping a WndProc, for which Windows
    # writes NO WER report, so crash_report.py had nothing to find) on roughly a third of runs, and
    # the gate returned PASS or a detailed order-stream FAIL depending on whether that run happened
    # to crash. Six builds and six rig runs went into bisecting a "regression" that was a crash.
    # Returning None routes into every caller's existing "produced no run folder" failure, so the
    # guard needs no call-site change and cannot be forgotten at a new one.
    #
    # A TIMEOUT IS NOT A CRASH: the kill above is the wall-clock bound this runner is built around
    # (see the paragraph above), so only a process that chose its own exit code is judged.
    rc = None
    try:
        rc = proc.wait(timeout=wall_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=30)
    after = set(glob.glob(os.path.join(lane_dir, "logs", "*_solo")))
    fresh = sorted(after - before)
    run_dir = fresh[-1] if fresh else None
    if rc:
        print(
            "  CRASH: the arm in %s exited %d (0x%08X) -- its log is a truncated prefix, not a "
            "disagreement" % (os.path.basename(lane_dir), rc, rc & 0xFFFFFFFF)
        )
        if run_dir:
            print("         run folder: %s" % run_dir)
        return None
    return run_dir


def tact_provision_lanes(args, n, visible=None):
    """Provision `n` tactical lanes and return their directories, or None if any failed.

    Provisioning is deliberately NOT parallelised even though the runs are: make_lane writes through
    a shared pack directory by symlink, and the boot lock exists precisely because concurrent
    pack-touching produces the "Insert CD" modal (ui_test.py's boot_lock comment has the measured
    numbers). Setup is seconds; the runs are minutes. Parallelise the part that costs."""
    dirs = []
    for slot in range(n):
        d = tact_provision_lane(args, visible=visible, slot=slot)
        if d is None:
            return None
        dirs.append(d)
    return dirs


def tact_provision_lane(args, visible=None, slot=0):
    """Provision the tactical lane and stage its save. Returns the lane dir, or None on failure.

    Shared by --tact-determinism and --tact-play so the two cannot drift on the one flag that has
    already been wrong once: passing NEITHER --headless nor --visible left make_lane to compute
    `headless = not args.visible`, i.e. always headless, so every "visible" run this project made
    presented nothing."""
    lane_dir = tact_lane_dir(slot)
    vis = args.visible if visible is None else visible
    print(
        "provisioning lane %s (%s) ..." % (tact_lane_name(slot), "visible" if vis else "headless")
    )
    r = subprocess.run(
        [
            sys.executable,
            os.path.join(REPO, "tools", "make_lane.py"),
            "--name",
            tact_lane_name(slot),
            "--lane",
            str(lane_alloc.lane("tact", slot)),
            "--port",
            str(LOCAL_PORT_BASE + lane_alloc.lane("tact", slot)),
        ]
        + (["--visible"] if vis else ["--headless"]),
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print("lane FAILED: %s" % (r.stderr or r.stdout).strip()[:300])
        return None
    # The save has to be IN the lane: --tactical loads save\<name>.sav relative to the exe, and a
    # lane is a fresh folder with no save dir at all. Committed copy (tools/uiscripts/saves/)
    # wins over the polygon's (fork F1D).
    src = ui_test.resolve_save(args.tact_save, os.path.join(machine.POLYGON, "save"))
    if not os.path.isfile(src):
        print("FAIL: no such save: %s (not in tools/uiscripts/saves/ either)" % src)
        return None
    dst_dir = os.path.join(lane_dir, "save")
    os.makedirs(dst_dir, exist_ok=True)
    shutil.copy2(src, os.path.join(dst_dir, args.tact_save + ".sav"))
    return lane_dir


def _tact_play_mouse_div(args):
    """The divisor for an interactive session: the caller's if given, else the derived default.

    `None` means "not passed" and gets the default; an explicit `0` still means "leave the shipped
    value alone", which is why the flag's default is None rather than 0."""
    return TACT_PLAY_MOUSE_DIV if args.tact_mouse_div is None else args.tact_mouse_div


def run_tact_play(args):
    """Launch ONE visible tactical mission and hand it to a human. The recording front end.

    Deliberately not an arm of --tact-determinism: there is no second run, no comparison and no wall
    cap. It provisions a VISIBLE lane (dgVoodoo caps the presented path at ~60 fps, so it is playable
    without any throttle), arms the mouse fix, and blocks until the player quits the game.

    THE MOUSE FIX IS THE POINT. Without `[input] mouse_absolute=1` the game is unplayable under a
    hypervisor: it runs on DirectInput RELATIVE counts, and a VM's absolute pointing device
    synthesises counts large enough to trip the DI path's 2x ballistic boost, so the cursor slams
    into its clamp. Dropping the DI device routes input down llm_input_wndproc_tap's absolute
    client-coordinate arm instead. Reproduces on stock retail -- see the tactical-probe work 9b."""
    mouse_div = _tact_play_mouse_div(args)
    lane_dir = tact_provision_lane(args, visible=True)
    if lane_dir is None:
        return 1
    # RECORDING LOGS THE WHOLE SESSION. tact_stop_step is what bounds the hash stream, and the
    # stream is not a nicety here -- it is the recording's own trajectory, the thing a replay has to
    # reproduce. Stopping it at --tact-frames would leave the journal running past the end of the
    # evidence that could check it. 0 = never stop (harness.cpp Config::tact_stop_step).
    tact_write_config(
        lane_dir,
        0 if args.tact_record else args.tact_frames,
        0,
        -1,
        args.tact_squad,
        args.tact_hp,
        args.tact_owner,
        None,
        args.tact_system,
        journal_rec=1 if args.tact_record else 0,
        # DEFAULT OFF (2026-08-24, second attempt): mouse_absolute=1 was MEASURED to leave the game
        # with NO mouse input at all -- no motion, no clicks -- which is worse than the
        # over-sensitivity it was meant to fix. Both explanations for that are refuted (see
        # The tactical-probe work 9b), so it is an explicit opt-in rather than a default.
        mouse_absolute=1 if args.tact_mouse_absolute else 0,
        mouse_div=mouse_div,
        mouse_accel=args.tact_mouse_accel,
        mouse_trace=1 if args.tact_mouse_trace else 0,
        exit_at=0,  # no self-stop: the human decides when the session ends
    )
    print("  lane      %s" % lane_dir)
    if tact_apply_extra_ini(lane_dir, args) is None:
        return 1
    print(
        "  mission   system=%s owner=%d (POZ<n>{O,L}.DAT -- the run log names the file)"
        % (args.tact_system or "from save", args.tact_owner)
    )
    print(
        "  mouse     %s"
        % (
            "[input] mouse_absolute=1 -- EXPERIMENTAL, measured to kill input entirely"
            if args.tact_mouse_absolute
            else "div=%s accel=%s"
            % (
                mouse_div or "1 (stock)",
                args.tact_mouse_accel or "100 (stock)",
            )
        )
    )
    # THE WRAPPER IS HALF THE FIX AND IT IS NOT A KNOB -- it is a file next to the exe, so the only
    # way a human learns it is missing is if something looks for it. A lane without it provisions
    # fine and every automated test still passes; only the person trying to PLAY finds out.
    wrapper = os.path.join(lane_dir, "dinput.dll")
    if args.tact_mouse_absolute:
        pass
    elif os.path.isfile(wrapper):
        print("            dinputto8 present (dinput.dll) -- the DI1-7 -> DI8 wrapper is what")
        print("            removes the VM input lag; the divisor above only fixes the scale.")
    else:
        print("            *** dinput.dll (dinputto8) is NOT in this lane. In a VM the mouse will")
        print("            *** lag badly and no divisor fixes that -- the wrapper does. Put it in")
        print("            *** the source install next to mh.exe. The VM-input notes 9e.")
    if args.tact_record:
        print("  RECORDING order journal: both seams + the three direct-write actions")
        print("            hashes logged for the WHOLE session (they are what a replay must match)")
    else:
        print("  hashes    tact_hash_step=1, logged for the first %d frames" % args.tact_frames)
        print("            NOT RECORDING -- pass --tact-record to journal this session")
    print("")
    print("  Play the mission. Close the game window when you are done.")
    print("")
    exe = os.path.join(lane_dir, "mh.focus.exe")
    before = set(glob.glob(os.path.join(lane_dir, "logs", "*_solo")))
    proc = subprocess.Popen([exe, "--tactical", args.tact_save, "--skip-intro"], cwd=lane_dir)
    proc.wait()
    fresh = sorted(set(glob.glob(os.path.join(lane_dir, "logs", "*_solo"))) - before)
    if not fresh:
        print("  NO run dir was produced -- the game wrote no log. Nothing to keep.")
        return 1
    run_dir = fresh[-1]
    print("  run dir   %s" % run_dir)
    entered = tact_entered(run_dir)
    if entered:
        print("  %s" % entered)

    # ARCHIVE IT, OUT OF THE LANE, BEFORE ANYTHING ELSE RUNS.
    #
    # `tact_provision_lane` DELETES logs/ -- so the next tactical run of any kind destroys this
    # session. That is not hypothetical: it ate a played session on 2026-08-25 (the one that
    # produced the dinputto8 measurements in the VM-input notes 9e), and before that it ate the first
    # captured divergence pair, which is why tools/tact_divergence_hunt.py archives too. A human
    # session is the most expensive artifact this rig produces -- minutes of someone's attention,
    # not a re-runnable script -- and it was the only one with no copy step.
    #
    # A warning printed for a human to act on is not a mechanism. Copying is.
    dst = os.path.join(SESSION_ARCHIVE, os.path.basename(run_dir))
    try:
        os.makedirs(SESSION_ARCHIVE, exist_ok=True)
        shutil.copytree(run_dir, dst, dirs_exist_ok=True)
    except OSError as e:
        print("  *** COULD NOT ARCHIVE the session: %s" % e)
        print("  *** COPY %s SOMEWHERE YOURSELF before running anything else tactical --" % run_dir)
        print("  *** the next lane provision deletes it.")
        return 1
    print("  archived  %s" % dst)
    print("            (the lane's copy dies at the next provision; this one does not)")

    if not args.tact_record:
        return 0

    jpath, n_orders, n_direct, n_input = tact_journal_extract(
        dst,
        os.path.join(dst, "journal.txt"),
        {
            "recorded": os.path.basename(run_dir),
            "save": args.tact_save,
            "system": args.tact_system or "from save",
            "squad": args.tact_squad,
            "hp_pct": args.tact_hp,
            "owner": args.tact_owner,
            "mission": (entered or "").split("MISSION")[-1].strip() if entered else "?",
        },
    )
    if not jpath:
        print("  *** NO ORDERS WERE JOURNALLED. The session produced no player order at all --")
        print("  *** either nothing was commanded, or the seams did not arm (look for")
        print("  *** '; TJ RECORD ARMED' in the run's mh_harness.log). Nothing to replay.")
        return 1
    print("  journal   %s" % jpath)
    print(
        "            %d order(s) + %d direct write(s) + %d input event(s)"
        % (n_orders, n_direct, n_input)
    )
    lost, surv, combat_line = tact_combat(dst)
    print("  combat    %s" % (combat_line or "NO TACT COMBAT LINE"))
    print("  survivors owner0 %s" % (surv or "(none)"))
    if lost is None:
        # NOT the same as "no combat", and conflating them once told a user to re-record a
        # 63,563-frame session that had fought a whole battle. A missing line is an INSTRUMENT gap.
        print("")
        print("  *** NO COMBAT LINE was emitted -- that is a gap in the harness, NOT a verdict")
        print("  *** about this session. The journal above is intact either way. Replay it to")
        print("  *** find out what happened: the replay reports its own census.")
    elif lost == 0:
        print("")
        print("  *** THIS SESSION REACHED NO COMBAT (units_lost_total=0).")
        print("  *** TACT-REC clause 3 refuses such a journal: the trajectory oracle has to reach")
        print("  *** an engagement, or it is measuring a walk. Re-record and pick a fight.")
    print("")
    print(
        "  Verify it:  python tools/test_ui.py --tact-replay %s"
        % os.path.relpath(jpath, REPO).replace(chr(92), "/")
    )
    print("  Keep it:    copy it into tools/uiscripts/journals/ and register the scenario")
    return 0


# ---- UI-REC: the GAME-START recording front end --------------------------------------------------
#
# The tactical recorder's sibling, and it exists because --tact-play cannot reach what it records:
# it launches `mh.focus.exe --tactical <save> --skip-intro`, i.e. straight past the menu, and the
# journal it writes is indexed on llm_tact_frame, which does not tick outside a mission. A game-start
# recording is the other side of both: launch at the MENU, and index on presents.
#
# WHAT THE RECORDING IS FOR. Not a pixel test -- the capture suite already owns those -- but an
# EQUIVALENCE fixture: a human's real path through menu -> new game -> in-game, replayed against the
# ship build and against the whole DLL rolled back to original bodies, compared on the per-step
# region-hash stream the strategic harness already emits. `sp_soak.txt` gives the same shape from an
# authored script; this gives it from a played session, which reaches the options, orders and screens
# no script was written to visit.
UI_LANE = "ui_play"
# ALLOCATED, NOT PICKED (fork F4H) -- see TACT_LANE_NO above and tools/lane_alloc.py. The comment
# that used to sit here ("clear of the capture suite (1..~20) ...") is the exact shape of the bug:
# every number it named had moved.
UI_LANE_NO = lane_alloc.lane("ui_play", 0)
# Kept OUT of the lane, for run_tact_play's reason: the next lane provision deletes logs/, and a
# human session is the most expensive artifact this rig produces.
UI_SESSION_ARCHIVE = os.path.join(REPO, "tmp", "ui_sessions")


def ui_lane_dir(slot=0):
    return os.path.join(make_lane.LANE_ROOT, UI_LANE if slot == 0 else "%s%d" % (UI_LANE, slot))


def ui_provision_lane(args, visible, slot=0):
    """Provision the game-start lane. Returns the lane dir, or None on failure.

    No save is staged: the whole point is to start from the menu. (A human is free to load one from
    inside the game -- that is part of the recorded session, not part of the provisioning.)"""
    lane_dir = ui_lane_dir(slot)
    name = os.path.basename(lane_dir)
    print("provisioning lane %s (%s) ..." % (name, "visible" if visible else "headless"))
    r = subprocess.run(
        [
            sys.executable,
            os.path.join(REPO, "tools", "make_lane.py"),
            "--name",
            name,
            "--lane",
            str(lane_alloc.lane("ui_play", slot)),
            "--port",
            str(LOCAL_PORT_BASE + lane_alloc.lane("ui_play", slot)),
        ]
        + (["--visible"] if visible else ["--headless"])
        # The wrapper's frame cap. Only meaningful with a blit -- headless has no present to pace --
        # so it is passed through whenever asked for and simply has nothing to do otherwise.
        + (
            ["--fps-limit", str(args.fps_limit)]
            if getattr(args, "fps_limit", None) is not None
            else []
        ),
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print("lane FAILED: %s" % (r.stderr or r.stdout).strip()[:300])
        return None
    for ln in (r.stdout or "").splitlines():
        if "FPSLimit" in ln or "--fps-limit" in ln:
            print(ln.rstrip())
    return lane_dir


def ui_write_config(
    lane_dir,
    journal_rec=0,
    journal="",
    stop_step=0,
    mouse_div=0,
    mouse_absolute=0,
    mouse_accel=0,
    visible=True,
    strat_seed=7,
    poke_at=0,
    skip_intro_avi=1,
    tj_trace=0,
    tj_trace_from=0,
    tj_ps_log=0,
    isolate_input=1,
    pin_menu_clock=1,
):
    """The lane's [harness] + [net]/[video]/[input] config for one game-start arm.

    THE PINS ARE THE STRATEGIC SET, NOT THE TACTICAL ONE, and the difference is `fixed_step`.
    tact_write_config pins nothing about the sim because a tactical excursion never calls
    llm_strat_sim_step; here the recording ENDS in strategic mode, so the run has to be reproducible
    on the sim's own cadence. `fixed_step=0` with `pin_wallclock=1` is the soak's shape and it is the
    right one: the pinned clock advances a fixed dt per frame (per PRESENT in the menu, which is what
    MH_Harness_OnPresent added), so time_tick sees a clean 60 fps world whatever the real frame rate
    was -- which is exactly what lets a session played at ~60 fps replay headless at several hundred.
    Pinning TOTAL_GAME_TIME on top of that (fixed_step=1) would override the recorded pacing with a
    flat one step per frame and change what the journal reproduces.

    region_hash_step=1 and order_log=1 are the COMPARISON channels, not diagnostics: the first is the
    per-step per-region hash stream the A/B diffs, the second is order_stream.py's hook-free input."""
    # ONE FILE (fork F2G, D12): the [harness] block is appended to the lane's mh_net.ini at the
    # bottom of this function instead of being written as its own mh_harness.ini, and `enable=1`
    # is what arms the harness -- it used to arm off that file merely existing, which is a
    # configuration written in the filesystem rather than in the config file. Built as a LIST
    # here and spliced there, so there is exactly one [harness] header in the result: a second
    # one would be unreachable to GetPrivateProfile*, which is this module's oldest trap.
    harness_lines = [
        "[harness]",
        "enable=1",
        "; UI-REC game-start recording (tools/test_ui.py --ui-play / --ui-replay).",
        "pin_fpu=1",
        "pin_wallclock=1",
        # The MENU's clock, which pin_wallclock does not reach (that replaces
        # GetCurrentTime, a seconds-valued FP leaf; the menu runs off
        # llm_time_get_ticks_ms). Without it _G_LLM_UI_MENU_INPUT_LOCK_TIMER drains on
        # a WALL-CLOCK duration while the journal is indexed in PRESENTS, so raising
        # the frame rate feeds replayed records into a window where
        # llm_ui_widget_input_tick's early return eats them -- measured at
        # 60/200/500/unbounded fps, progressively worse. harness.cpp pin_menu_clock.
        # 1 by default. The opt-out exists for ONE question, and it is a real one: a
        # journal recorded BEFORE this pin existed was played on the real ms clock, so
        # anything in the game that samples llm_time_get_ticks_ms took different values
        # then than a pinned replay takes now. --no-ui-pin-menu-clock reproduces the
        # recording's own configuration, which is how you tell "the replay is wrong"
        # apart from "the fixture predates the pin".
        "pin_menu_clock=%d" % pin_menu_clock,
        "pin_clock_dt_us=16667",
        "pin_clock_base_s=1000",
        "pin_rand=1",
        "rand_seed=12345",
        # THE PLANETS GFX MASK IS OFF FOR EVERY UIREC ARM, and it is the committed
        # oracles that force it. `.oracle.gz` stores ABSOLUTE per-step `state` hashes
        # extracted from a human recording (spcamp-solo 2026-09-07, tutorial-solo
        # 2026-09-09) -- the C arm -- and those sessions predate mh::state::emit_planets
        # (LIFT-TABLE S2, 2026-09-09). With the mask on, B-vs-C and A-vs-C would go red
        # from step 1 with nothing wrong, and a human recording cannot be re-derived
        # under a new hash definition: the log stores the hash, not the bytes.
        # The pin is sound because the UNMASKED walk reproduces the pre-mask flat hash
        # bit-for-bit -- asserted in net_selftest statetest, not assumed -- and it costs
        # nothing here: the mask is a VERDICT concern about bank[], which is gfx state a
        # host owns after S3, and every UIREC comparison is one build against itself or
        # against its own recording.
        "mask_planets_gfx=0",
        # SPCAMP-SEED. pin_rand ABOVE DOES NOT COVER THE CAMPAIGN, and that is the whole
        # of this pair. It replaces the Watcom CRT rand(); the strategic world draws from
        # a different generator entirely (llm_strat_rng_next over
        # _G_LLM_STRAT_RNG_STATE[4]), and llm_strat_planet_session_begin seeds it from
        # llm_strat_rng_seed_wallclock_seconds -- time()+localtime(), i.e. the real-world
        # CLOCK SECOND -- immediately before rolling each player's landing site. So a
        # campaign started at :17 past the minute lands elsewhere than one started at :43,
        # and a journal replay diverges at step 0 with the input reproduced perfectly.
        # That is the divergence-at-step-1 both recorded human sessions showed.
        # pin_wallclock replaces GetCurrentTime and does not reach time() either.
        "pin_strat_seed=1",
        # Overridable (--ui-strat-seed) because SPCAMP-SEED's done_when has a NEGATIVE
        # clause: changing this must CHANGE the landing spots. Without that, "two runs
        # landed identically" is equally consistent with the landing being constant for
        # some reason having nothing to do with the pin.
        "strat_seed=%d" % strat_seed,
        # The evidence half: RNG channel state either side of the landing roll, and every
        # enabled slot's chosen spot. Without it "the two runs agree" and "the landing is
        # constant for some unrelated reason" are the same observation.
        "land_log=1",
        # fixed_step=1, AND THE SOAK'S 0 IS WRONG HERE -- measured, not copied. With 0 the
        # sim's per-step delta is derived from the pinned wall clock, so it depends on how
        # much clock elapsed between session start and the first tick -- i.e. on how long
        # the run spent in the MENU, which is exactly the quantity a game-start replay
        # cannot reproduce. The first round trip with 0 diverged in `state` at step 1 with
        # `game_time_delta` differing on all 200 steps. The soak gets away with 0 because
        # both its arms enter through the same script at the same point; a recording and
        # its replay do not. 1 pins TOTAL_GAME_TIME to one deterministic 0.1s step per
        # frame, which takes the wall clock out of the simulation altogether.
        "fixed_step=1",
        "region_hash_step=1",
        "order_log=1",
        # OFF, and this is the one pin that is about WHAT GETS RECORDED rather than about
        # reproducing it. The default is 1: the game-over watch sets stop_step to the
        # detection step, and the stop block then sets g_active=false -- which makes
        # on_sim_step return early, so `++g_step` and the per-step region hashing both
        # STOP. exit_on_stop=0 keeps the process alive, so nothing looks wrong; the
        # session simply continues with a frozen sim clock, emitting no more `TJ P` step
        # barriers and no more of the hash stream that IS this fixture's oracle.
        # A campaign recording is precisely a session that outlives a game-over -- eliminate
        # the enemy, then transition to the next planet -- so the default would silently
        # discard the half of the recording that motivated making it.
        "gameover_stop=0",
        # Dismiss the new-game intro movie + NEWGAME.TXT briefing. The game's own key
        # path (harness.cpp avi_skip_tick), not a cut -- the proceed action that ENTERS
        # the game still fires exactly as a player's SPACE fires it.
        "skip_intro_avi=%d" % skip_intro_avi,
        # DIAGNOSTIC, default off (--ui-tj-trace N). Logs the first N records AS INJECTED
        # -- idx/seam/recorded frame/due step/actual step/shift -- plus the pinned clock
        # at each of the first 12 sim steps. This is the instrument that located G146: it
        # is what shows records due at steps 40-44 all landing on step 48, and the sim's
        # first step arriving at a different present/clock depending on the frame rate.
        "tj_trace=%d" % tj_trace,
        "tj_trace_from=%d" % tj_trace_from,
        "tj_ps_log=%d" % tj_ps_log,
        # SPCAMP-FLAKE: 1 (default) suppresses the GAME's own input-ring producer for
        # the length of a journal replay, so the journal is the ring's only writer.
        # `--ui-no-isolate` sets it to 0, which is the NEGATIVE arm: with real host
        # mouse activity during the run, 0 diverges and 1 does not.
        "replay_isolate_input=%d" % isolate_input,
        # 0 = run until the journal is exhausted (the DLL exits itself -- see the UI-REC EXIT
        # block in tj_replay_frame). Nonzero bounds the IN-GAME half, which is the A/B shape.
        "stop_step=%d" % stop_step,
        # WITHOUT THIS stop_step ONLY WRITES THE REPORT -- it does not end the process,
        # so both arms of the first round trip ran to the runner's wall clock and were
        # killed, 7 minutes each, for a 200-step comparison. exit_on_stop is what makes
        # stop_step a budget rather than a log marker. Paired: with stop_step=0 there is
        # nothing to exit ON, and the journal-exhaustion exit covers that case instead.
        "exit_on_stop=%d" % (1 if stop_step else 0),
        "ui_journal_rec=%d" % journal_rec,
        "ui_journal=%s" % journal,
        # SPCAMP-AB's go-red arm (--ui-gored N), written into ONE arm only. A comparison
        # that has never been driven red is not evidence that it could be: every green
        # this fixture produces is only worth what its red is worth.
        "region_poke_at=%d" % poke_at,
        "region_poke_min=%d" % (0 if poke_at else -1),
    ]
    ident = make_lane.read_identity(lane_dir) or {}
    lines = ["[net]", "enable=1"]
    if ident.get("port"):
        lines.append("port=%d" % ident["port"])
    # `lane` MOVED INTO [uitest] at fork F2G (its own one-key [test] section is refused now).
    lines += [
        "",
        "[uitest]",
        "lane=%d" % ident.get("lane", UI_LANE_NO),
        "",
        "[video]",
        "size_mode=0",
    ]
    if not visible:
        lines += ["no_present=1", "no_window=1"]
    lines += [
        "",
        "[input]",
        # The VM mouse fix, same knobs and same defaults as the tactical play front end -- a menu is
        # navigated with the mouse, so an un-attenuated hypervisor pointer makes a game-start
        # recording impossible in exactly the way it made a mission one impossible. See
        # The VM-input notes and run_tact_play's header.
        "mouse_absolute=%d" % mouse_absolute,
        "mouse_div=%d" % mouse_div,
        "mouse_accel=%d" % mouse_accel,
        "",
    ]
    lines += harness_lines + [""]
    with open(os.path.join(lane_dir, "mh_net.ini"), "w", newline="\r\n") as f:
        f.write("\n".join(lines))


def ui_journal_unsplit_clicks(records):
    """Move any screen barrier that fell BETWEEN a press and its release to just after the release.

    A BARRIER INSIDE A CLICK IS A DEADLOCK, and it is the only way this design can hang. The recorder
    emits a barrier when it notices the screen changed; a menu item that opens its screen on the
    mouse-DOWN changes it while the button is still held, so the release gets journalled behind the
    barrier:

        M <f> 2 1 159 160 ...   press
        S <f> 6634567 ...       barrier: the screen changed
        M <f> 4 0 159 160 ...   release  <-- queued behind the barrier

    On replay the press is injected, then the barrier waits for a screen that cannot appear until the
    release is injected -- and the release is behind the barrier. The replay holds forever, which the
    "no timeout" rule (right for every other case) turns into a hang rather than a verdict. Measured
    on a real 25,709-record session: held at record 134 with the game running at full speed.

    A press and its release are ONE GESTURE and nothing may be scheduled between them. Detection is on
    the `buttons` field rather than the event type, so it does not depend on which button or on the
    type encoding. Fixed in the recorder too (harness.cpp defers the barrier while a button is held);
    this half exists so journals recorded before that fix still replay."""
    out, pending, held = [], [], False
    for r in records:
        f = r.split()
        if r.startswith("S ") and held:
            pending.append(r)  # a barrier may not sit inside a click -- hold it for the release
            continue
        out.append(r)
        if r.startswith("M ") and len(f) > 3:
            held = f[3] != "0"  # `buttons`: nonzero while any button is down
        if not held and pending:
            out.extend(pending)
            pending = []
    out.extend(pending)
    return out


def ui_journal_extract(run_dir, dst, meta):
    """Pull the `TJ M/K/C` lines out of a recorded run and write a journal file.

    The tactical extractor's sibling, minus the order/direct-write channels: a UI journal carries
    INPUT ONLY (see the UI-REC block in harness.cpp for why the tactical E/G/D watches cannot run
    outside mode 6). Same one-file, comment-header convention, so the DLL parser reads both.
    Returns (path, n_input) or (None, 0)."""
    inputs = []
    log = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(log):
        return None, 0
    for line in open(log, encoding="utf-8", errors="replace"):
        # `S ` IS NOT OPTIONAL. The screen barriers are what make a menu replay synchronise on UI
        # state instead of on a present count, and dropping them here is exactly how the second round
        # trip failed: the DLL journalled 7 of them and this filter threw all 7 away, so the journal
        # was a bare schedule again and the replay never reached a live game.
        # `P ` (step barriers) joins `S ` for the same reason `S ` is here and for the half of the
        # problem `S ` cannot reach: a screen barrier synchronises the MENU, a step barrier the
        # in-game half, where the journal is a present schedule and the world runs on sim steps.
        if line.startswith("TJ ") and line[3:5] in ("M ", "K ", "C ", "S ", "P "):
            inputs.append(line[3:].rstrip())
    if not inputs:
        return None, 0
    # Stable sort on the frame column only: the recorder emits M, K and C in a fixed order within a
    # frame and that order is the one the game produced them in.
    inputs.sort(key=lambda b: int(b.split()[1]))
    inputs = ui_journal_unsplit_clicks(inputs)
    with open(dst, "w", newline="\n") as f:
        f.write("; UI-REC game-start input journal -- presents-indexed, INPUT ONLY.\n")
        f.write("; Replay: python tools/test_ui.py --ui-replay <this file>\n")
        for k in sorted(meta):
            f.write("; %s: %s\n" % (k, meta[k]))
        f.write("; records: %d\n" % len(inputs))
        for b in inputs:
            f.write(b + "\n")
    return dst, len(inputs)


def run_ui_play(args):
    """Launch ONE visible game at the MENU and hand it to a human. The game-start recording front end.

    Blocks until the player quits. Everything about the shape is run_tact_play's, for the reasons
    that function documents at length -- a visible lane, no wall cap, the mouse fix armed, and the
    session ARCHIVED out of the lane before anything else can run."""
    mouse_div = TACT_PLAY_MOUSE_DIV if args.ui_mouse_div is None else args.ui_mouse_div
    lane_dir = ui_provision_lane(args, visible=True)
    if lane_dir is None:
        return 1
    ui_write_config(
        lane_dir,
        journal_rec=1 if args.ui_record else 0,
        # A RECORDING SHOWS THE HUMAN WHAT THE GAME SHOWS. Replay arms skip the intro by default;
        # dismissing a movie under the hands of someone recording is a surprise, and their own
        # dismiss is a legitimate part of what the journal captures.
        skip_intro_avi=1 if getattr(args, "skip_intro_avi", False) else 0,
        tj_trace=int(getattr(args, "ui_tj_trace", 0) or 0),
        tj_trace_from=int(getattr(args, "ui_tj_trace_from", 0) or 0),
        tj_ps_log=int(getattr(args, "ui_tj_ps", 0) or 0),
        isolate_input=0 if getattr(args, "ui_no_isolate", False) else 1,
        pin_menu_clock=1 if getattr(args, "ui_pin_menu_clock", True) else 0,
        stop_step=0,  # a human decides when the session ends
        mouse_div=mouse_div,
        mouse_absolute=1 if args.ui_mouse_absolute else 0,
        mouse_accel=args.ui_mouse_accel,
        visible=True,
    )
    print("  lane      %s" % lane_dir)
    # --ui-all-original: RECORD THE ORIGINAL BINARY, not our promoted bodies.
    #
    # A recording is a REFERENCE, and a reference built on the code under test is not one. Recorded on
    # the ship config, every promoted C++ body's behaviour is baked into the journal's expected hash
    # stream -- so a later replay-vs-recording comparison asks only "do our bodies agree with
    # themselves", and a promotion bug present at record time is invisible forever after. Recorded
    # all-original, the stream is the ORIGINAL binary's behaviour and that comparison becomes a real
    # test of every promotion.
    #
    # Same mechanism as --ui-equiv's `original` arm, deliberately: write_all_original_ini selects the
    # D11 configuration rather than listing knobs, so a promotion added later cannot leak into a
    # recording by being forgotten here. Routed through --extra-ini so tact_apply_extra_ini's
    # per-section readback reports how many keys are LIVE IN THE LANE -- read that line before playing,
    # because an appended duplicate section is present and unreachable (its own documented trap).
    if getattr(args, "ui_all_original", False):
        orig = os.path.join(REPO, "tmp", "ui_all_original.ini")
        os.makedirs(os.path.dirname(orig), exist_ok=True)
        mode = write_all_original_ini(orig)
        args.extra_ini = [orig] + list(getattr(args, "extra_ini", None) or [])
        print(
            "  ALL-ORIGINAL: [config] mode=%s -- this journal will be a reference for the ORIGINAL "
            "binary" % mode
        )
    if tact_apply_extra_ini(lane_dir, args) is None:
        return 1
    print(
        "  mouse     div=%s accel=%s"
        % (mouse_div or "1 (stock)", args.ui_mouse_accel or "100 (stock)")
    )
    wrapper = os.path.join(lane_dir, "dinput.dll")
    if not args.ui_mouse_absolute and not os.path.isfile(wrapper):
        print("            *** dinput.dll (dinputto8) is NOT in this lane. In a VM the mouse will")
        print("            *** lag badly and no divisor fixes that -- the wrapper does. Put it in")
        print("            *** the source install next to mh.exe. The VM-input notes 9e.")
    if args.ui_record:
        print("  RECORDING input journal: presents-indexed, mouse + keys + cursor")
        print("            the whole session, from the first present at the menu")
    else:
        print("  NOT RECORDING -- pass --ui-record to journal this session")
    print("")
    print("  Navigate the menu and start a game. Close the game window when you are done.")
    print("")
    exe = os.path.join(lane_dir, "mh.focus.exe")
    before = set(glob.glob(os.path.join(lane_dir, "logs", "*_solo")))
    proc = subprocess.Popen([exe, "--skip-intro"], cwd=lane_dir)
    proc.wait()
    fresh = sorted(set(glob.glob(os.path.join(lane_dir, "logs", "*_solo"))) - before)
    if not fresh:
        print("  NO run dir was produced -- the game wrote no log. Nothing to keep.")
        return 1
    run_dir = fresh[-1]
    print("  run dir   %s" % run_dir)

    dst = os.path.join(UI_SESSION_ARCHIVE, os.path.basename(run_dir))
    try:
        os.makedirs(UI_SESSION_ARCHIVE, exist_ok=True)
        shutil.copytree(run_dir, dst, dirs_exist_ok=True)
    except OSError as e:
        print("  *** COULD NOT ARCHIVE the session: %s" % e)
        print("  *** COPY %s SOMEWHERE YOURSELF before running anything else -- the next" % run_dir)
        print("  *** lane provision deletes it.")
        return 1
    print("  archived  %s" % dst)

    if not args.ui_record:
        return 0
    steps = ui_steps_reached(dst)
    jpath, n_input = ui_journal_extract(
        dst,
        os.path.join(dst, "journal.txt"),
        {"recorded": os.path.basename(run_dir), "sim_steps": steps},
    )
    if not jpath:
        print(
            "  *** NO INPUT WAS JOURNALLED. Either nothing was clicked, or the arm did not take --"
        )
        print("  *** look for '; UI-REC ARMED' in the run's mh_harness.log. Nothing to replay.")
        return 1
    print("  journal   %s" % jpath)
    print("            %d input record(s), %s sim step(s) reached" % (n_input, steps))
    if steps == 0:
        print("")
        print("  *** THIS SESSION NEVER REACHED A LIVE GAME (no sim step logged). The journal is")
        print("  *** intact and will replay, but the A/B has nothing to compare: its oracle is the")
        print("  *** per-step hash stream, which only exists once a game is running.")
    print("")
    print(
        "  Replay it:  python tools/test_ui.py --ui-replay %s"
        % os.path.relpath(jpath, REPO).replace(chr(92), "/")
    )
    print(
        "  A/B it:     python tools/test_ui.py --ui-equiv %s"
        % os.path.relpath(jpath, REPO).replace(chr(92), "/")
    )
    return 0


def ui_steps_reached(run_dir):
    """How many sim steps the run logged -- 0 for a session that never left the menu."""
    hl = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(hl):
        return 0
    last = 0
    for line in open(hl, encoding="utf-8", errors="replace"):
        # UPPERCASE hex too -- the harness prints %llX. A lowercase-only class does not merely miss
        # these lines, it misses them INTERMITTENTLY: `[0-9a-f]+` still matches the leading digits of
        # a hash that happens to start with them, so the same runner read 176 steps off one run and 0
        # off the next with the identical binary, and the flake looked like the game's.
        m = re.match(r"^(\d+) [0-9A-Fa-f]+ [0-9A-Fa-f]+", line)
        if m:
            last = int(m.group(1))
    return last


# UI-REC: which comparison channels decide a verdict, and which cannot.
#
# THE EXCUSED SET IS NOT A TOLERANCE, it is a statement about what a game-start replay controls. The
# pinned wall clock advances once per PRESENT while the menu is up, and how many presents pass before
# a screen is ready is decided by asynchronous resource loading -- so two runs that click the same
# buttons on the same screens still enter the game with different clock readings. Every excused
# channel below is derived from that reading; every required one is the simulation.
#
# The split is measured, not assumed: on the first green round trip `state` matched on all 200 steps
# while `current_game_time` differed on all 200, and `total_game_time` / `sim_step_interval` /
# `game_time_delta` / `game_speed` -- the clock quantities the SIM integrates, as opposed to the
# wall-clock readings -- matched. So the sim's own clock is reproduced; only the reading of the host
# clock is not. `combined` is excused for one reason only: it hashes every region including the
# excused ones, so it can never be green while they are red.
UI_REQUIRED_CHANNELS = (
    "state",
    "total_game_time",
    "sim_step_interval",
    "game_time_delta",
    "game_speed",
)
UI_EXCUSED_CHANNELS = (
    "combined",
    "current_game_time",
    "last_game_time",
    "frame_ring",
    "fps_estimate",
)


def ui_verdict(res):
    """(ok, lines) for a UI-REC comparison -- the required channels only, with the rest named.

    Deliberately NOT sp_compare's own `ok`: that requires every channel, which is right for two runs
    launched identically and wrong for a recording compared against its replay. Reported rather than
    silently dropped, so nobody reads this verdict as "everything matched"."""
    lines, ok = [], True
    if not res["compared_steps"]:
        return False, ["      NO OVERLAPPING STEPS -- the two runs compared nothing"]
    for ch in UI_REQUIRED_CHANNELS:
        v = res["channels"].get(ch)
        if v is None:
            ok = False
            lines.append("      %-18s MISSING -- the channel was not compared at all" % ch)
            continue
        if v["compared"] == 0:
            ok = False
            lines.append("      %-18s compared NOTHING (vacuous)" % ch)
            continue
        if v["mismatch_count"]:
            ok = False
        lines.append(
            "      %-18s compared %-6d mismatches %-6d first %s   [required]"
            % (ch, v["compared"], v["mismatch_count"], v["first_mismatch"])
        )
    for ch in UI_EXCUSED_CHANNELS:
        v = res["channels"].get(ch)
        if v is None:
            continue
        lines.append(
            "      %-18s compared %-6d mismatches %-6d first %s   [excused: wall clock]"
            % (ch, v["compared"], v["mismatch_count"], v["first_mismatch"])
        )
    if res.get("uncompared_regions"):
        ok = False
        lines.append("      UNCOMPARED regions: %s" % ", ".join(res["uncompared_regions"]))
    return ok, lines


def ui_journal_steps(journal):
    """The sim-step count the journal's own header records, or 0 if it carries none."""
    try:
        for line in open(journal, encoding="utf-8", errors="replace"):
            if not line.startswith(";"):
                break
            m = re.match(r";\s*sim_steps:\s*(\d+)", line)
            if m:
                return int(m.group(1))
    except OSError:
        pass
    return 0


def ui_fixture_path(v):
    """Resolve `--ui-replay`/`--ui-equiv`'s argument: a path, or a UIREC_SCENARIOS name."""
    if not v or os.path.isfile(v):
        return v
    for s in UIREC_SCENARIOS:
        if s["name"] == v:
            return os.path.join(REPO, s["journal"])
    return v  # let the caller's "no such journal" report name it


def ui_scenario(name):
    """The UIREC_SCENARIOS entry for `name`, or None."""
    for s in UIREC_SCENARIOS:
        if s["name"] == name:
            return s
    return None


# ---- ARM C: the RECORDING's own trajectory, committed beside the journal -------------------------
#
# A and B are two replays. They can only disagree about a PROMOTION, because everything else about
# them is identical by construction -- so a harness bug that shifts both of them the same way is
# invisible to A-vs-B however green it looks. C is the human session the journal was cut from, and it
# is the only arm that was not produced by the replayer, so it is the only one that can see that
# class of bug. (It is a usable reference at all because the recording was itself made with the
# strat-seed pin armed -- see SPCAMP-REC's progress note, which reverses an earlier "impossible in
# principle" ruling.)
#
# WHAT IS STORED, AND WHAT IS NOT. The full R stream is ~34 MB (3.1 MB gzipped) of which almost
# everything is either excused or constant. Stored: `state` plus the four clock regions the SIM
# integrates -- exactly UI_REQUIRED_CHANNELS. Omitted BY DESIGN: `combined` and the wall-clock
# regions, which cannot agree between a recording and a replay (see UI_EXCUSED_CHANNELS), so keeping
# them would commit megabytes of guaranteed-red columns and invite someone to "fix" them later.
UI_ORACLE_REGIONS = ("total_game_time", "sim_step_interval", "game_time_delta", "game_speed")


def ui_oracle_path(journal):
    """The oracle that belongs to `journal`: same path, `.oracle.gz` instead of `.journal`."""
    base = journal[: -len(".journal")] if journal.endswith(".journal") else journal
    return base + ".oracle.gz"


def ui_oracle_extract(harness_log, dst, meta=None):
    """Write the committed C-arm oracle for a recording's harness log. Returns (path, n_steps)."""
    import mp_analyze as _m

    idx = {nm: i for i, nm in enumerate(_m.REGION_NAMES)}
    cols = [idx[nm] for nm in UI_ORACLE_REGIONS]
    seg = _m.parse_harness(harness_log)
    steps = sorted(set(seg["steps"]) & set(seg["regions"]))
    if not steps:
        return None, 0
    with gzip.open(dst, "wt", encoding="utf-8", newline="\n") as f:
        f.write(
            "; UI-REC recording oracle -- the per-step channels a replay is REQUIRED to match.\n"
        )
        f.write(
            "; Regenerate: python tools/test_ui.py --ui-oracle <run dir or harness log> "
            "--ui-oracle-out <path>\n"
        )
        for k, v in sorted((meta or {}).items()):
            f.write("; %s: %s\n" % (k, v))
        f.write("; regions: %s\n" % " ".join(UI_ORACLE_REGIONS))
        f.write("; steps: %d\n" % len(steps))
        for s in steps:
            r = seg["regions"][s]
            f.write("%d %s %s\n" % (s, seg["steps"][s]["state"], " ".join(r[c] for c in cols)))
    return dst, len(steps)


def ui_oracle_load(path):
    """Load a committed oracle into a parse_harness-shaped segment.

    `clock`/`combined` are deliberately None: sp_compare's channel() skips a step where either side
    is None, so those two channels report `compared 0` rather than a fabricated agreement. They are
    excused anyway -- but a zero here is the honest number, and ui_verdict prints it."""
    import mp_analyze as _m

    idx = {nm: i for i, nm in enumerate(_m.REGION_NAMES)}
    cols = [idx[nm] for nm in UI_ORACLE_REGIONS]
    width = len(_m.REGION_NAMES)
    seg = {"steps": {}, "regions": {}, "breakdown": {}, "banner": None, "path": path}
    with gzip.open(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith(";"):
                continue
            p = line.split()
            if len(p) != 2 + len(cols):
                continue
            s = int(p[0])
            seg["steps"][s] = {"clock": None, "combined": None, "state": p[1].upper()}
            row = [None] * width
            for c, v in zip(cols, p[2:]):
                row[c] = v.upper()
            seg["regions"][s] = row
    return seg


def ui_order_histogram(run_dir_or_log):
    """{order code -> count} from a run's `;ord` rows -- the NON-VACUITY channel.

    A hash comparison says two runs agreed; it does not say they DID anything. This is what says
    they did: before the keystate channel was replayed (G147) both arms of this very fixture ran
    30,000 steps with ZERO squad orders in either and agreed perfectly about a game neither of them
    played. Counting order-queue ROW OBSERVATIONS rather than distinct orders is fine and deliberate
    -- every arm and the recording are measured the same way, so the comparison is like-for-like."""
    path = run_dir_or_log
    if os.path.isdir(path):
        path = os.path.join(path, "mh_harness.log")
    hist = {}
    if not os.path.isfile(path):
        return hist
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith(";ord   "):
            m = re.search(r"code=([0-9A-Fa-f]+)", line)
            if m:
                k = m.group(1).upper()
                hist[k] = hist.get(k, 0) + 1
    return hist


def ui_land_modes(run_dir):
    """The `; LAND SESSION_MODE=` readings, in order -- the campaign + transition evidence."""
    hl = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(hl):
        return []
    return re.findall(
        r"; LAND SESSION_MODE=(\d+)", open(hl, encoding="utf-8", errors="replace").read()
    )


def ui_orders_after_last_landing(run_dir, land_mode="1"):
    """(landing_step, {code: count}) for the orders issued AFTER the run's LAST landing.

    `land_mode` is the SESSION_MODE the fixture lands in, a scenario property rather than a
    constant: the spcamp journals land 1 (CAMPAIGN), tutorial_solo lands 2 (single-player
    skirmish) at the injected planet 31. Hardcoding "1" here read the tutorial fixture as having
    never landed at all.

    THE POST-TRANSITION HALF, asked for by name. A whole-journal claim that stops being checked at
    the transition is a rung with extra steps: the second planet is a differently-initialised world
    (SESSION_MODE 1 entered twice, different planet), and it is the part no other fixture in the tree
    reaches at all. Totals alone cannot see it -- 30,000 steps of matching pre-transition orders
    dominate any histogram, so a completely dead second planet moves the numbers by ~2%.

    The landing step is read by carrying the most recent `R <step>` line forward, because the LAND
    line itself carries no step."""
    hl = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(hl):
        return 0, {}
    step, land, ord_step, hist = 0, 0, 0, {}
    rows = []
    for line in open(hl, encoding="utf-8", errors="replace"):
        if line.startswith("R "):
            p = line.split()
            if len(p) > 1 and p[1].isdigit():
                step = int(p[1])
        elif line.startswith("; LAND SESSION_MODE=%s" % land_mode):
            land = step
        elif line.startswith(";ord ") and not line.startswith(";ord   "):
            m = re.match(r";ord (\d+) ", line)
            if m:
                ord_step = int(m.group(1))
        elif line.startswith(";ord   "):
            m = re.search(r"code=([0-9A-Fa-f]+)", line)
            if m:
                rows.append((ord_step, m.group(1).upper()))
    for s, c in rows:
        if s > land:
            hist[c] = hist.get(c, 0) + 1
    return land, hist


def ui_budget(args, journal):
    """Sim steps to run an arm for: the caller's, else DERIVED FROM THE JOURNAL.

    A budget that is too small does not fail loudly -- it ends the run while input is still pending,
    so the journal never exhausts, no verdict line is written and the arm reports "the journal never
    ran" about a replay that did 97% of it. Measured on the first human session: 25,045 of 25,709
    records issued and the run stopped at the 27,000 steps I had guessed at the command line.
    The journal knows how many steps its recording took; +25% covers a replay that accrues them at a
    slightly different rate per present."""
    if args.ui_steps > 0:
        return args.ui_steps
    rec = ui_journal_steps(journal)
    return int(rec * 1.25) + 200 if rec else 0


# Which arm labels had to be KILLED by the wall clock. SPCAMP-REC's done_when (d) is "the run ends
# when the journal ends", and a killed run is precisely the failure that clause names -- but it is
# invisible from the logs alone, since a kill and a clean exit leave the same flushed report. So it
# is recorded here at the moment it happens rather than inferred afterwards.
ui_arm_timed_out = set()


def ui_run_arm(
    args, journal, slot=0, extra_ini=(), label="arm", poke_at=0, budget=None, lane_dir=None
):
    """Replay `journal` headless in its own lane. Returns the run dir, or None.

    One lane per arm and one arm per lane: the A/B runs two of these, and a lane provision deletes
    logs/, so sharing one would destroy the first arm's evidence while the second was still being
    read. Same rule the tactical replay follows for the same reason.

    `lane_dir` is the CONCURRENT-arms path (run_ui_abc): the caller provisions both lanes first and
    the arms then run in parallel, each in its pre-built lane. Left None, this provisions its own
    (the single-arm --ui-replay path, unchanged)."""
    if lane_dir is None:
        lane_dir = ui_provision_lane(args, visible=args.visible, slot=slot)
    if lane_dir is None:
        return None
    # `budget=0` is a REQUEST, not a missing value, so it is distinguished from `budget=None`. It is
    # what --ui-abc runs on: with stop_step=0 the DLL terminates the moment the journal is exhausted
    # (harness.cpp's "UI-REC EXIT: journal exhausted and stop_step=0"), which is the only way a
    # full-journal fixture ends on its own evidence rather than on the runner's wall clock.
    if budget is None:
        budget = ui_budget(args, journal)
    ui_write_config(
        lane_dir,
        journal=os.path.abspath(journal),
        stop_step=budget,
        mouse_div=0,  # a replay injects into the ring directly and never touches either mouse path
        visible=args.visible,
        strat_seed=getattr(args, "ui_strat_seed", 7),
        poke_at=poke_at,
        # OFF unless asked for, EVEN THOUGH it costs every visible replay ~2 minutes. Turning it on
        # for the replay arms took rung 200 red (110 mismatches, first at step 91) while rung 10000
        # stayed green -- G144's exact non-monotone signature, because dismissing the movie changes
        # the menu's present timeline and the two arms need not dismiss on the same present. The
        # oracle's arms must differ in NOTHING but their promotion config, so a viewing convenience
        # does not get to sit in that path by default.
        skip_intro_avi=1 if getattr(args, "skip_intro_avi", False) else 0,
        tj_trace=int(getattr(args, "ui_tj_trace", 0) or 0),
        tj_trace_from=int(getattr(args, "ui_tj_trace_from", 0) or 0),
        tj_ps_log=int(getattr(args, "ui_tj_ps", 0) or 0),
        isolate_input=0 if getattr(args, "ui_no_isolate", False) else 1,
        pin_menu_clock=1 if getattr(args, "ui_pin_menu_clock", True) else 0,
    )
    if extra_ini:
        saved = getattr(args, "extra_ini", None)
        args.extra_ini = list(extra_ini)
        merged = tact_apply_extra_ini(lane_dir, args)
        args.extra_ini = saved
        if merged is None:
            return None
    exe = os.path.join(lane_dir, "mh.focus.exe")
    before = set(glob.glob(os.path.join(lane_dir, "logs", "*_solo")))
    how = (
        " -- ends AT THE JOURNAL'S END"
        if budget == 0
        else ("" if args.ui_steps > 0 else " derived from the journal")
    )
    print("  %-14s launching (stop_step=%d%s) ..." % (label, budget, how))
    t0 = time.time()
    # ON THE ISOLATED DESKTOP WHEN ONE IS IN FORCE -- the same fix tact_run_arm already carries, and
    # it was missing here for the same reason: `subprocess` does not expose STARTUPINFO.lpDesktop, so
    # a plain Popen silently puts the window on the operator's screen while the `[rig] isolated
    # desktop` banner (printed from argument PARSING) claims otherwise. tact_run_arm's docstring calls
    # that "worse than no claim" and it was right twice: EVERY journal replay this rig has ever run --
    # --ui-replay, --ui-abc, and the SPCAMP-FLAKE batches -- put a live game window on the interactive
    # desktop, where real mouse messages reach it. That is not a cosmetic leak. It is the delivery
    # path for the foreign input SPCAMP-FLAKE turned out to be: the game's own
    # producer runs on every window message, so a window sharing the operator's desktop is a window
    # being fed the operator's mouse. `replay_isolate_input` closes the ring against that regardless of
    # which desktop the window is on -- but a harness should not need the second defence because the
    # first one was only ever printed.
    # THE MACHINE-WIDE BOOT LOCK, held only across the pack-load window -- the same serialisation
    # tact_run_arm has carried since --tact-jobs made its arms concurrent, and needed here since
    # run_ui_abc's arms run in parallel (2026-09-10): lanes share their resource packs by symlink,
    # and two instances reading them at once raise the Insert-CD modal (ui_test's boot_lock has the
    # measured numbers). Boot is seconds; an arm is minutes.
    with ui_test.boot_lock(os.path.basename(lane_dir)):
        conflict = make_lane.lane_conflict(lane_dir)  # fork F4H
        if conflict:
            # None, not an exit: these arms run in a thread pool, where a SystemExit is swallowed.
            print(conflict)
            return None
        if DESKTOP:
            tact_hold_desktop_once()
            proc = _tact_pid_handle(
                desktop.spawn(exe, "--skip-intro", cwd=lane_dir, desktop=DESKTOP)
            )
        else:
            proc = subprocess.Popen([exe, "--skip-intro"], cwd=lane_dir)
        ui_test.wait_past_pack_load(lane_dir, before, pid=getattr(proc, "pid", None))
    try:
        proc.wait(timeout=args.ui_timeout)
    except subprocess.TimeoutExpired:
        # NOT a silent kill: a replay that had to be killed did not reach its own end condition, and
        # every number read off it afterwards describes a truncated run.
        print("  %-14s TIMEOUT after %ds -- killed" % (label, args.ui_timeout))
        proc.kill()
        proc.wait()
        ui_arm_timed_out.add(label)
    print("  %-14s exited after %.0fs" % (label, time.time() - t0))
    fresh = sorted(set(glob.glob(os.path.join(lane_dir, "logs", "*_solo"))) - before)
    if not fresh:
        print("  %-14s produced no run dir" % label)
        return None
    return fresh[-1]


def ui_replay_report(run_dir, label, bounded=False, min_steps=0):
    """(ok, lines, facts) for one replay arm's SHAPE -- checked before any hash is read.

    Every rule here is a way for a replay to look green having done nothing: the arm can fail to
    arm at all, load a journal and issue none of it, or be killed before the journal ran out. All
    three produce a log full of plausible lines.

    `bounded` says the arm was stopped at a step budget ON PURPOSE (`--ui-steps N`), which makes two
    of those rules wrong rather than lenient -- see the comment at the `elif not bounded` branch.
    `facts` is what the CALLER compares across arms: an A/B is only meaningful if both arms did the
    same thing, and "same number of records issued, same number of barriers forced" is that check."""
    lines, ok = [], True
    hl = os.path.join(run_dir, "mh_harness.log")
    text = open(hl, encoding="utf-8", errors="replace").read() if os.path.isfile(hl) else ""
    if "; UI-REC ARMED" not in text:
        # AN EMPTY LOG IS NOT A FAILED ARM, and saying so cost two rounds of diagnosis. The harness
        # log is BUFFERED and flushed at report points, so a replay killed by the wall clock before
        # it finished writes nothing at all -- indistinguishable, from here, from a run whose config
        # never took. The two are separated by mh_net.log, which is written eagerly and carries the
        # present hook's own `ui_journal=1`.
        if os.path.getsize(hl) == 0 if os.path.isfile(hl) else False:
            return (
                False,
                [
                    "      %s: FAIL -- EMPTY harness log. The run was killed before any flush, so it "
                    "stalled rather than failed to arm; check mh_net.log for 'ui_journal=1' to "
                    "confirm the arm, and the '; TJ SCREENS: holding at' line for where it stuck."
                    % label
                ],
                {},
            )
        return (
            False,
            ["      %s: FAIL -- the UI-REC arm never armed (no '; UI-REC ARMED' line)" % label],
            {},
        )
    # FORCED BARRIERS ARE REPORTED, NEVER SILENT. A forced barrier means the replay proceeded past a
    # screen it could not confirm -- the run is still worth reading (the hashes decide), but a green
    # verdict with forced barriers is a weaker claim than one without, so it is always printed.
    forced = re.search(r"; TJ SCREENS: held \d+ present\(s\) at barriers, (\d+) FORCED", text)
    # THE SUMMARY LINE ONLY EXISTS IF THE JOURNAL EXHAUSTED, so a bounded rung has none -- and reading
    # 0 off its absence would report "no arm ever forced" about a run that forced ten times. Fall back
    # to counting the per-occurrence lines, which are written (and flushed) as they happen.
    n_forced = (
        int(forced.group(1)) if forced else len(re.findall(r"; TJ SCREENS: FORCED past", text))
    )
    # `TJ STEPS: FORCED` is the STEP barrier's stall hatch (added 2026-09-07 -- an unbounded `P` hold
    # deadlocked the all-original arm at step 29,999). It is counted with the screen forces rather
    # than beside them: both mean "the replay proceeded past a synchronisation point it could not
    # satisfy", which is the thing the zero-forced clause is about. It is reported separately below
    # so the two causes stay distinguishable.
    # The summary line's own counter ALREADY includes them (both hatches bump g_tj_forced), so they
    # are only added when falling back to counting per-occurrence lines on a run with no summary.
    n_stalled = len(re.findall(r"; TJ STEPS: FORCED past", text))
    if not forced:
        n_forced += n_stalled
    # How far into the journal the arm actually got, for the cross-arm equality check on a bounded
    # run where "issued N of M" is likewise never written. The barrier records are monotonic.
    held = re.findall(r"; TJ SCREENS: (?:holding at|FORCED past) record (\d+)", text)
    n_reached = max((int(x) for x in held), default=-1)
    loaded = re.search(r"; order_replay: loaded (\d+)|; TJ REPLAY: loaded (\d+)", text)
    # Both spellings: the line gained an explicit barrier count on 2026-09-07 (it used to compare
    # input records against a total that INCLUDED the barriers, and so called a complete replay
    # "SHORT"). The old form is still matched so an archived run remains readable.
    issued = re.search(
        r"; TJ REPLAY: issued (\d+) (?:input record\(s\) \+ \d+ barrier\(s\) of|of) (\d+)", text
    )
    done = "; TJ REPLAY COMPLETE" in text
    steps = ui_steps_reached(run_dir)
    if issued:
        n_iss, n_load = int(issued.group(1)), int(issued.group(2))
        lines.append(
            "      %s: issued %d/%d record(s), %d sim step(s)%s"
            % (label, n_iss, n_load, steps, "" if done else "  [journal NOT exhausted]")
        )
        if n_iss == 0:
            ok = False
            lines.append("      %s: FAIL -- the journal loaded and issued NOTHING." % label)
    elif not bounded:
        ok = False
        lines.append("      %s: FAIL -- no TJ REPLAY verdict line; the journal never ran." % label)
    else:
        # A RUNG DOES NOT EXHAUST ITS JOURNAL, and neither of the two rules above can tell that from a
        # dead run. `--ui-steps N` bounds the arm at N sim steps on purpose (SPCAMP-REC's 200 / 2000 /
        # 10000), so the process exits mid-journal and the verdict line -- which the DLL writes on
        # exhaustion -- is never reached. Both rules are right for an UNBOUNDED replay and wrong here,
        # so what replaces them is `bounded`'s own evidence: the step floor below, plus the cross-arm
        # equality run_ui_equiv checks (same records issued, same barriers forced). Without this a
        # bounded rung could not pass however clean its hashes were -- measured: `state` 0 mismatches
        # over 200 steps reported as FAIL twice, on both arms.
        lines.append(
            "      %s: %d sim step(s); BOUNDED by --ui-steps, so journal exhaustion is not "
            "required (see ui_replay_report)" % (label, steps)
        )
    if not done and not bounded:
        ok = False
        lines.append("      %s: FAIL -- the journal was not exhausted (run ended early)." % label)
    if steps == 0:
        ok = False
        lines.append(
            "      %s: FAIL -- no sim step: the replay never reached a live game, so "
            "there is no trajectory to compare." % label
        )
    elif min_steps and steps < min_steps:
        ok = False
        lines.append(
            "      %s: FAIL -- reached %d sim step(s), short of the %d-step rung. A comparison over "
            "fewer steps than the rung asked for is a different (easier) claim."
            % (label, steps, min_steps)
        )
    if n_forced:
        lines.append(
            "      %s: %d barrier(s) FORCED (%d screen, %d step-stall) -- the replay proceeded past a "
            "synchronisation point it could not satisfy (grep '; TJ SCREENS: FORCED' / '; TJ STEPS: "
            "FORCED' for which). The hash comparison still decides, but this run synchronised less "
            "than it wanted to." % (label, n_forced, n_forced - n_stalled, n_stalled)
        )
    facts = {
        "forced": n_forced,
        "issued": int(issued.group(1)) if issued else -1,
        "reached": n_reached,
        "steps": steps,
    }
    return ok, lines, facts


def run_ui_replay(args):
    """Replay a game-start journal once and report what it did. The diagnostic arm."""
    journal = ui_fixture_path(args.ui_replay)
    if not os.path.isfile(journal):
        print("FAIL: no such journal: %s" % journal)
        return 1
    print("UI-REC replay: %s" % journal)
    # --ui-all-original here too, and it is the symmetric half of the recording flag. A journal
    # recorded all-original is a reference for the ORIGINAL binary, so "does the replay reproduce the
    # recording" must be asked with the SAME bodies installed -- otherwise a difference means either a
    # harness bug or a promotion bug and the run cannot say which. Replay all-original to test the
    # harness; replay on ship (the default) against an all-original recording to test the promotions.
    # --extra-ini IS HONOURED HERE, and it was not until 2026-09-07. This arm accepted the flag and
    # dropped it -- the exact defect tact_apply_extra_ini's docstring describes for the tactical arms
    # ("worse than rejecting it"), reproduced at a second entry point. It cost a bisect: a run passed
    # `--extra-ini <one promotion off>` and silently executed the full SHIP config, which reads as
    # evidence that rolling that promotion back changed nothing. A replay arm is exactly where a
    # rollback fragment belongs -- it is how you ask "is this regression ours?" of a recorded session.
    extra = list(getattr(args, "extra_ini", None) or [])
    if getattr(args, "ui_all_original", False):
        orig = os.path.join(REPO, "tmp", "ui_all_original.ini")
        os.makedirs(os.path.dirname(orig), exist_ok=True)
        extra.insert(0, orig)
        print("  ALL-ORIGINAL replay: [config] mode=%s" % write_all_original_ini(orig))
    if extra:
        print("  extra-ini  %s" % ", ".join(os.path.basename(e) for e in extra))
    extra = tuple(extra)
    rd = ui_run_arm(args, journal, slot=0, extra_ini=extra, label="replay")
    if rd is None:
        return 1
    print("  run dir   %s" % rd)
    ok, lines, _ = ui_replay_report(
        rd, "replay", bounded=args.ui_steps > 0, min_steps=args.ui_steps
    )
    for ln in lines:
        print(ln)
    print("\nui-replay: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def run_ui_selftest(args):
    """Prove the game-start record -> replay round trip WITHOUT A HUMAN.

    THE SAME REASON tact_input_selftest EXISTS, and it was learned the expensive way there: the only
    other way to exercise this is a person playing, which is the loop the whole feature is meant to
    make cheap, and two sessions were spent finding gaps a self-contained test would have caught.

    HOW IT CAN BE SELF-CONTAINED AT ALL: the recorder samples the two global input RINGS, and
    ui_drive's script interpreter injects into those same rings. So a scripted menu walk is, as far
    as the recorder can tell, a player -- and sp_det.txt is already a scripted walk from the main
    menu into a live game, which is exactly the sequence this feature exists to record.

    Arm 1 records the scripted walk; arm 2 replays the resulting journal with NO script, so the only
    thing driving it is the journal. If the two trajectories agree on the per-step region hashes,
    then input recorded at the menu re-causes the same game -- which is the whole claim."""
    import mp_analyze as _m  # the comparison, same channel --sp-determinism reads

    steps = args.ui_steps if args.ui_steps > 0 else 200
    script = args.ui_selftest_script
    src = script if os.path.isabs(script) else os.path.join(REPO, "tools", "uiscripts", script)
    if not os.path.isfile(src):
        print("FAIL: no such script: %s" % src)
        return 1
    print("UI-REC selftest: record %s, then replay the journal (%d steps/arm)" % (script, steps))

    lane = ui_provision_lane(args, visible=args.visible, slot=0)
    if lane is None:
        return 1
    ui_write_config(lane, journal_rec=1, stop_step=steps, visible=args.visible)
    shutil.copy2(src, os.path.join(lane, os.path.basename(script)))
    # MERGED, NOT APPENDED (fork F2G). ui_write_config's lane ini now carries a [uitest] block of
    # its own (the lane number moved there out of the retired [test] section), so appending a
    # second one would put every key here in a section GetPrivateProfile* never reaches -- the
    # scripted walk would simply not run, on a lane that looks correctly configured.
    _lane_ini = os.path.join(lane, "mh_net.ini")
    _merged = ui_test.ini_merge_fragment(
        open(_lane_ini, encoding="utf-8").read(),
        "[uitest]\nenable=1\nscript=%s\ndump_screens=0\ntimeout_frames=%d\n"
        % (os.path.basename(script), args.ui_selftest_frames),
    )
    with open(_lane_ini, "w", newline="\r\n") as f:
        f.write(_merged)
    exe = os.path.join(lane, "mh.focus.exe")
    before = set(glob.glob(os.path.join(lane, "logs", "*_solo")))
    print("  record         launching the scripted walk ...")
    proc = subprocess.Popen([exe, "--skip-intro"], cwd=lane)
    try:
        proc.wait(timeout=args.ui_timeout)
    except subprocess.TimeoutExpired:
        print("  record         TIMEOUT after %ds -- killed" % args.ui_timeout)
        proc.kill()
        proc.wait()
    fresh = sorted(set(glob.glob(os.path.join(lane, "logs", "*_solo"))) - before)
    if not fresh:
        print("  record         produced no run dir")
        return 1
    rec_dir = fresh[-1]
    print("  record    -> %s" % rec_dir)
    rec_text = ""
    hl = os.path.join(rec_dir, "mh_harness.log")
    if os.path.isfile(hl):
        rec_text = open(hl, encoding="utf-8", errors="replace").read()
    if "; UI-REC ARMED" not in rec_text:
        print("      FAIL: the UI-REC arm never armed -- no '; UI-REC ARMED' line.")
        return 1
    rec_steps = ui_steps_reached(rec_dir)
    # ARCHIVE BEFORE THE SECOND LANE IS PROVISIONED. Slot 1 is a different folder so this one is not
    # at risk from it, but the NEXT selftest run reprovisions slot 0 and deletes this evidence -- and
    # a red arm whose recording is gone cannot be diagnosed.
    keep = os.path.join(UI_SESSION_ARCHIVE, os.path.basename(rec_dir))
    try:
        os.makedirs(UI_SESSION_ARCHIVE, exist_ok=True)
        shutil.copytree(rec_dir, keep, dirs_exist_ok=True)
    except OSError:
        keep = rec_dir
    jpath, n_input = ui_journal_extract(
        keep,
        os.path.join(keep, "journal.txt"),
        {
            "recorded": os.path.basename(rec_dir),
            "script": os.path.basename(script),
            "sim_steps": rec_steps,
        },
    )
    if not jpath:
        print(
            "      FAIL: the arm armed but journalled NO input. The script drove the menu, so the"
        )
        print("      recorder is not seeing the rings ui_drive injects into.")
        return 1
    print("      recorded %d input record(s) over %d sim step(s)" % (n_input, rec_steps))
    if rec_steps == 0:
        print("      FAIL: the scripted walk never reached a live game -- nothing to compare.")
        return 1

    rep_dir = ui_run_arm(args, jpath, slot=1, label="replay")
    if rep_dir is None:
        return 1
    print("  replay    -> %s" % rep_dir)
    ok, lines, _ = ui_replay_report(rep_dir, "replay")
    for ln in lines:
        print(ln)

    res = _m.sp_compare(
        _m.parse_harness(os.path.join(rec_dir, "mh_harness.log")),
        _m.parse_harness(os.path.join(rep_dir, "mh_harness.log")),
        "recording",
        "replay",
    )
    print("      compared steps: %d" % res["compared_steps"])
    v_ok, v_lines = ui_verdict(res)
    for ln in v_lines:
        print(ln)
    print("      journal   %s" % jpath)
    final = ok and v_ok
    print("\nui-selftest: %s" % ("PASS" if final else "FAIL"))
    return 0 if final else 1


def run_ui_equiv(args):
    """THE GATE ARM: replay one game-start journal on the SHIP config and against the whole DLL
    rolled back to original bodies, and require the two trajectories to be identical.

    This is --tact-equiv's shape, moved to the strategic oracle. The comparison is mp_analyze's
    sp_compare over the per-step per-region hash stream, which is the same channel --sp-determinism
    and the soak golden use -- so a divergence names the step and the region, not just a verdict.

    WHY ALL-ORIGINAL AND NOT A PER-MODULE FLIP: write_all_original_ini selects `[config]
    mode=original`, which turns EVERY promotion default and the rebind-row default off at their one
    source, so the control arm is the game running its own code from the menu onward. A per-module
    control would leave the rest of the DLL promoted and quietly narrow the claim."""
    import mp_analyze as _m  # the comparison, same channel --sp-determinism reads

    journal = ui_fixture_path(args.ui_equiv)
    if not os.path.isfile(journal):
        print("FAIL: no such journal: %s" % journal)
        return 1
    if ui_budget(args, journal) <= 0:
        # A REFUSAL, NOT A DEFAULT -- but only when the journal cannot supply one either. With
        # stop_step=0 the DLL exits the moment the journal runs out, so both arms would stop at
        # whatever step the recording happened to reach, comparing a trajectory whose length is a
        # property of the human's mouse. A journal that records its own sim_steps answers this.
        print("FAIL: --ui-equiv needs a step budget -- the in-game steps AFTER the journal are the")
        print(
            "      comparison, and this journal's header carries no sim_steps to derive one from."
        )
        print("      compare past the start sequence.")
        return 1
    print("UI-REC equivalence: %s (%d in-game steps per arm)" % (journal, args.ui_steps))
    orig = os.path.join(REPO, "tmp", "ui_all_original.ini")
    os.makedirs(os.path.dirname(orig), exist_ok=True)
    write_all_original_ini(orig)
    gored = getattr(args, "ui_gored", 0)
    if gored:
        print(
            "  GO-RED ARM: the `original` arm pokes every hashed region at sim step %d. This run "
            "PASSES only if the comparison goes RED at that step." % gored
        )
    arms = [("ship", ())] + [("original", (orig,))]
    runs, ok, report, facts = {}, True, [], {}
    for slot, (name, extra) in enumerate(arms):
        rd = ui_run_arm(
            args,
            journal,
            slot=slot,
            extra_ini=extra,
            label=name,
            poke_at=gored if name == "original" else 0,
        )
        if rd is None:
            return 1
        print("  arm %-10s -> %s" % (name, rd))
        # THE POKED ARM STOPS AT THE POKE, by design -- region_poke reports and ends there -- so the
        # rung's step floor is the wrong floor for it. What it must reach is the poke step; requiring
        # the full rung would fail a go-red for doing exactly what a go-red does (measured: `state`
        # diverging at step 100 reported as FAIL because the arm stopped at 100).
        floor = gored if (gored and name == "original") else args.ui_steps
        a_ok, a_lines, a_facts = ui_replay_report(
            rd, name, bounded=args.ui_steps > 0, min_steps=floor
        )
        ok &= a_ok
        report += a_lines
        facts[name] = a_facts
        runs[name] = _m.parse_harness(os.path.join(rd, "mh_harness.log"))

    # THE ARMS MUST HAVE DONE THE SAME THING, and this is the clause session 1 failed while its red
    # looked ordinary: its two arms forced 10 and 11 barriers and ran 30,123 vs 24,260 steps, i.e.
    # they played different games, so the hash comparison attributed nothing -- and a GREEN there
    # would have been worse than the red, because it would have been believed. Forcing is not itself
    # disqualifying (measured 2026-09-07: both arms of the 200-step rung forced the same barrier and
    # hashed identically); forcing DIFFERENTLY is.
    for key, what in (
        ("forced", "screen barriers FORCED"),
        ("issued", "journal records issued"),
        ("reached", "furthest journal record reached at a barrier"),
    ):
        a, b = facts["ship"].get(key), facts["original"].get(key)
        if a != b:
            ok = False
            report.append(
                "      FAIL -- the arms did not do the same thing: %s ship=%s original=%s. They "
                "played different games, so the hash comparison below attributes nothing."
                % (what, a, b)
            )
    print("\n" + "=" * 78 + "\nUI-REC equivalence verdict\n" + "=" * 78)
    for ln in report:
        print(ln)
    res = _m.sp_compare(runs["ship"], runs["original"], "ship", "original")
    print("      compared steps: %d" % res["compared_steps"])
    v_ok, v_lines = ui_verdict(res)
    for ln in v_lines:
        print(ln)
    if gored:
        # THE VERDICT IS INVERTED, and the SHAPE checks are not. An arm that crashed also fails to
        # match, so "it went red" is only evidence if both arms otherwise ran the fixture properly --
        # `ok` is what says they did, and it is required in BOTH directions.
        st = res["channels"].get("state")
        first = st["first_mismatch"] if st else None
        red_here = st is not None and st["mismatch_count"] > 0
        final = ok and red_here
        print(
            "      GO-RED: `state` %s (poked at step %d, first mismatch at %s)"
            % (
                "DIVERGED" if red_here else "did NOT diverge -- the comparison is blind",
                gored,
                first,
            )
        )
        print("\nui-equiv (go-red): %s" % ("PASS" if final else "FAIL"))
        return 0 if final else 1
    final = ok and v_ok
    print("\nui-equiv: %s" % ("PASS" if final else "FAIL"))
    return 0 if final else 1


def run_ui_abc(args):
    """THE GATE SCENARIO (SPCAMP-SYNC + SPCAMP-REC): three arms over the WHOLE journal.

        A = ship            every promotion on -- what we ship
        B = all-original    every [promote] key and [rebind] row off -- the game's own code
        C = the recording   the human session the journal was cut from, committed as an oracle

    A-vs-B is the PROMOTION oracle and B-vs-C is the HARNESS oracle, and the pair is the point: a
    promotion bug moves A away from B and C together; a replay bug moves A and B away from C
    together. Either one alone can be green while the other is red, and a two-arm comparison cannot
    tell you which.

    THE RUNGS ARE GONE (user, 2026-09-07). 200 / 2000 / 10000 were milestones while the fixture was
    being built, not separate claims -- a prefix cannot see the planet transition or any order after
    it, which is the coverage this scenario exists for. `--ui-steps N` still bounds a run for
    diagnosis and turns this into a rung; it is not what the item is measured by.

    BOTH ARMS RUN WITH stop_step=0 so the DLL exits the moment the journal is exhausted. That is
    done_when (d): a replay that has done everything asked of it and then waits out the runner's wall
    clock is reported as a TIMEOUT for a pass, and charges the gate the whole difference."""
    import mp_analyze as _m

    name = args.ui_abc
    scen = ui_scenario(name)
    journal = ui_fixture_path(name)
    if not os.path.isfile(journal):
        print("FAIL: no such journal: %s" % journal)
        return 1
    oracle = (
        os.path.join(REPO, scen["oracle"])
        if scen and scen.get("oracle")
        else ui_oracle_path(journal)
    )
    if not os.path.isfile(oracle):
        # A REFUSAL, not a two-arm fallback. Quietly dropping to A-vs-B would keep printing PASS
        # while measuring strictly less than the name `--ui-abc` claims.
        print("FAIL: no C arm -- the recording oracle is missing: %s" % oracle)
        print("      Build it from the session the journal was cut from:")
        print("      python tools/test_ui.py --ui-oracle <run dir> --ui-oracle-out %s" % oracle)
        return 1
    bounded = args.ui_steps > 0
    gored = getattr(args, "ui_gored", 0)
    print("UI-REC A/B/C: %s" % journal)
    print("  oracle    %s" % oracle)
    if bounded:
        print(
            "  NOTE: --ui-steps %d bounds this to a RUNG. The scenario's own claim is the WHOLE "
            "journal; a bounded run is diagnosis, not the gate." % args.ui_steps
        )
    if gored:
        print(
            "  GO-RED ARM: `ship` pokes every hashed region at sim step %d. This run PASSES only "
            "if the comparison goes RED at that step." % gored
        )

    orig = os.path.join(REPO, "tmp", "ui_all_original.ini")
    os.makedirs(os.path.dirname(orig), exist_ok=True)
    print("  arm B runs the ORIGINAL binary: [config] mode=%s" % write_all_original_ini(orig))

    ok, report, facts, runs, dirs = True, [], {}, {}, {}
    arm_specs = (("ship", ()), ("original", (orig,)))
    # PROVISION SERIALLY, RUN CONCURRENTLY (2026-09-10). The two arms were always independent --
    # each has its own lane slot, port and mutex -- and ran one after the other only by loop shape,
    # which cost the gate the shorter arm's whole wall time (~2.5 min on spcamp_solo). Provisioning
    # stays sequential here (make_lane self-serialises anyway, so a pool would only queue), the
    # boots serialise on ui_test's machine-wide boot lock inside ui_run_arm, and each worker gets a
    # COPY of args because ui_run_arm mutates args.extra_ini around tact_apply_extra_ini -- a shared
    # namespace between concurrent arms is a race even when today's arms happen not to overlap on
    # that field. Correctness runs only, per the parallel-lane notes: a replay is step/present-
    # scheduled, so contention moves its seconds, never its verdict.
    arm_lanes = {}
    slot_base = int(getattr(args, "ui_slot", 0) or 0)
    for i, (arm, _extra) in enumerate(arm_specs):
        slot = slot_base + i
        ld = ui_provision_lane(args, visible=args.visible, slot=slot)
        if ld is None:
            return 1
        arm_lanes[arm] = (slot, ld)

    def _run_one_arm(arm, extra):
        slot, ld = arm_lanes[arm]
        sub = argparse.Namespace(**vars(args))
        return ui_run_arm(
            sub,
            journal,
            slot=slot,
            extra_ini=extra,
            label=arm,
            poke_at=gored if arm == "ship" else 0,
            budget=args.ui_steps if bounded else 0,
            lane_dir=ld,
        )

    with cf.ThreadPoolExecutor(max_workers=2) as ex:
        arm_futs = {arm: ex.submit(_run_one_arm, arm, extra) for arm, extra in arm_specs}
        arm_dirs = {arm: f.result() for arm, f in arm_futs.items()}

    for arm, _extra in arm_specs:
        rd = arm_dirs[arm]
        if rd is None:
            return 1
        # ARCHIVE THE ARM BEFORE ANYTHING ELSE RUNS. A lane provision DELETES logs/, so the next
        # invocation of anything that uses slot 0 destroys this arm's evidence -- and the first thing
        # anyone does with a red A/B/C is run a rollback probe, which is exactly that. Measured the
        # expensive way: the ship arm's log of the very first full run was gone before its order
        # histogram could be read back, and the arm had to be re-run to recover it.
        # The scenario name is part of the archive key: two concurrent --ui-abc units (run_gate)
        # can produce same-second run dirs, and a timestamp+arm key would merge their trees.
        keep = os.path.join(
            UI_SESSION_ARCHIVE, "%s_%s_%s" % (os.path.basename(rd), os.path.basename(name), arm)
        )
        try:
            os.makedirs(UI_SESSION_ARCHIVE, exist_ok=True)
            shutil.copytree(rd, keep, dirs_exist_ok=True)
            rd = keep
        except OSError as e:
            print("  arm %-10s NOT archived (%s) -- a later run may delete it" % (arm, e))
        print("  arm %-10s -> %s" % (arm, rd))
        dirs[arm] = rd
        # A poked arm STOPS at the poke, so neither journal exhaustion nor the full step count is its
        # bar -- same rule run_ui_equiv already carries, and for the same reason.
        poked = bool(gored) and arm == "ship"
        a_ok, a_lines, a_facts = ui_replay_report(
            rd, arm, bounded=bounded or poked, min_steps=(gored if poked else args.ui_steps)
        )
        ok &= a_ok
        report += a_lines
        facts[arm] = a_facts
        runs[arm] = _m.parse_harness(os.path.join(rd, "mh_harness.log"))
        if arm in ui_arm_timed_out:
            ok = False
            report.append(
                "      %s: FAIL -- the arm was KILLED by the %ds wall clock. done_when (d) asks for "
                "a run that ends when the journal ends; this one ended when the runner gave up, and "
                "every number read off it describes a truncated run." % (arm, args.ui_timeout)
            )
    runs["recording"] = ui_oracle_load(oracle)

    # ---- shape: the arms must have done the same thing -------------------------------------------
    for key, what in (
        ("forced", "screen barriers FORCED"),
        ("issued", "journal records issued"),
        ("reached", "furthest journal record reached at a barrier"),
        ("steps", "sim steps run"),
    ):
        a, b = facts["ship"].get(key), facts["original"].get(key)
        if a != b and not gored:
            ok = False
            report.append(
                "      FAIL -- the arms did not do the same thing: %s ship=%s original=%s. They "
                "played different games, so the hash comparison below attributes nothing."
                % (what, a, b)
            )
    # SPCAMP-SYNC (a): ZERO forced, not merely equal. Identical forcing was the weaker clause that
    # made a rung comparison meaningful; the full-journal fixture is held to the stronger one.
    if not bounded and not gored:
        for arm in ("ship", "original"):
            if facts[arm].get("forced"):
                ok = False
                report.append(
                    "      FAIL -- %s FORCED %d screen barrier(s). SPCAMP-SYNC (a) asks for zero: a "
                    "forced barrier is the replay proceeding past a screen it could not confirm."
                    % (arm, facts[arm]["forced"])
                )

    # ---- non-vacuity: what the arms actually DID --------------------------------------------------
    hists = {a: ui_order_histogram(dirs[a]) for a in ("ship", "original")}
    # The recording's own histogram is committed IN THE SCENARIO (a dict, not a file) -- it is a
    # couple of dozen small integers, and putting it where a reader of the registry can see it is
    # worth more than another artifact to keep in step.
    rec_hist = scen.get("orders") if scen else None
    report.append("")
    report.append("      ORDER-CODE HISTOGRAM (non-vacuity -- a hash says they agreed, this says")
    report.append("      they did something). Row = order code, then one column per arm:")
    codes = sorted(set(hists["ship"]) | set(hists["original"]) | set(rec_hist or {}))
    if not codes:
        ok = False
        report.append(
            "      FAIL -- NO ORDERS IN ANY ARM. The comparison is vacuous: two runs that"
        )
        report.append(
            "      issued nothing agree about nothing. (This is not hypothetical -- it is"
        )
        report.append(
            "      exactly what this fixture did before the keystate channel was replayed.)"
        )
    for c in codes:
        s, o = hists["ship"].get(c, 0), hists["original"].get(c, 0)
        r = (rec_hist or {}).get(c)
        bad = (s != o) or (r is not None and r != o)
        report.append(
            "        code %-3s ship=%-6d original=%-6d recording=%-6s %s"
            % (c, s, o, "-" if r is None else r, "  <-- DIFFERS" if bad else "")
        )
        if bad and not gored:
            ok = False
    if rec_hist is None:
        report.append("        (the scenario carries no recorded histogram to compare against)")

    # WHICH LANDING, AND HOW MANY, ARE PROPERTIES OF THE FIXTURE -- not constants. Both clauses below
    # were written against the spcamp journals and hardcoded their shape: SESSION_MODE=1 (CAMPAIGN)
    # and a second landing for the planet transition. tutorial_solo lands SESSION_MODE=2
    # (single-player skirmish) at the injected planet 31 and has no transition at all, so the
    # hardcoded pair failed it twice for doing exactly what the tutorial does. Defaults keep every
    # spcamp scenario reading identically.
    land_mode = str((scen or {}).get("land_mode", "1"))
    land_name = {"1": "CAMPAIGN", "2": "single-player skirmish"}.get(
        land_mode, "mode %s" % land_mode
    )
    want_lands = int((scen or {}).get("landings", 2))
    for arm in ("ship", "original"):
        modes = ui_land_modes(dirs[arm])
        landings = [m for m in modes if m == land_mode]
        report.append(
            "      %s: LAND SESSION_MODE readings %s -- %d %s landing(s)"
            % (arm, ",".join(modes) or "(none)", len(landings), land_name)
        )
        if not landings:
            ok = False
            report.append(
                "      FAIL -- %s never logged `LAND SESSION_MODE=%s (%s)`, so the run's mode "
                "is assumed rather than read (SPCAMP-REC done_when (b))."
                % (arm, land_mode, land_name)
            )
        elif not bounded and len(landings) < want_lands and not (gored and arm == "ship"):
            ok = False
            report.append(
                "      FAIL -- %s landed %d time(s); this fixture's whole-journal claim includes %d "
                "(the extra one is the PLANET TRANSITION). Fewer means the run stopped short of the "
                "coverage this fixture exists for." % (arm, len(landings), want_lands)
            )
        # THE POKED ARM IS EXEMPT FROM THE SECOND LANDING, and only the poked one. It stops AT the
        # poke by design -- `region_poke` reports and ends there -- so a poke before the transition
        # can never produce two landings, and the clause failed a go-red for doing exactly what a
        # go-red does. Same exemption journal exhaustion and the step floor already carry a few lines
        # up ("A poked arm STOPS at the poke, so neither ... is its bar"); this clause was written
        # later and did not inherit it. The UNPOKED arm still has to land twice, so the go-red keeps
        # its evidence that the fixture ran the whole session in the arm that was supposed to.

    # ---- THE POST-TRANSITION ORDERS, checked separately from the totals ---------------------------
    # Because the totals cannot see them. ~30,000 pre-transition steps dominate any histogram, so a
    # completely dead second planet moves the numbers by about 2% -- well inside the noise a reader
    # would forgive. The second planet is a differently-initialised world and it is the part no other
    # fixture in the tree reaches, so it gets its own line.
    # ONLY WHEN THERE IS A FAR SIDE TO LOOK AT. This whole block exists because the totals cannot see
    # the second planet; a fixture that lands once (tutorial_solo) has no post-transition half, and
    # running it there re-reports the totals under a name that promises more than it checks.
    if not bounded and not gored and want_lands >= 2:
        post = {a: ui_orders_after_last_landing(dirs[a], land_mode) for a in ("ship", "original")}
        report.append("")
        for arm in ("ship", "original"):
            land, h = post[arm]
            report.append(
                "      %s: after the last campaign landing (step %d) -- %d order observation(s) "
                "across %d code(s): %s"
                % (
                    arm,
                    land,
                    sum(h.values()),
                    len(h),
                    " ".join("%s=%d" % kv for kv in sorted(h.items())) or "(none)",
                )
            )
        rec_post = scen.get("orders_post") if scen else None
        if rec_post is not None:
            report.append(
                "      recording: after ITS last campaign landing -- %s"
                % " ".join("%s=%d" % kv for kv in sorted(rec_post.items()))
            )
        if post["ship"][1] != post["original"][1]:
            ok = False
            report.append(
                "      FAIL -- the arms disagree about what happened AFTER the transition. That is "
                "the half of this fixture nothing else covers, and it is invisible in the totals "
                "above."
            )
        elif rec_post is not None and post["original"][1] != rec_post:
            ok = False
            report.append(
                "      FAIL -- both arms agree with each other and NEITHER agrees with the "
                "recording about the second planet. Two replays agreeing is not the claim; arm C "
                "exists for exactly this shape."
            )
        elif not post["ship"][1]:
            ok = False
            report.append(
                "      FAIL -- NO orders after the last campaign landing in either arm. The run "
                "reached the second planet and then did nothing there, so the post-transition claim "
                "is vacuous even though both arms agree."
            )

    # ---- the comparisons -------------------------------------------------------------------------
    print("\n" + "=" * 78 + "\nUI-REC A/B/C verdict\n" + "=" * 78)
    for ln in report:
        print(ln)
    pairs = (
        ("A vs B", "ship", "original", None, "the PROMOTION oracle"),
        ("B vs C", "original", "recording", UI_ORACLE_REGIONS, "the HARNESS oracle"),
        ("A vs C", "ship", "recording", UI_ORACLE_REGIONS, "implied by the other two, reported"),
    )
    results = {}
    for title, a, b, regions, why in pairs:
        kw = {"time_regions": regions} if regions else {}
        res = _m.sp_compare(runs[a], runs[b], a, b, **kw)
        results[title] = res
        print("\n  %s -- %s (%s)" % (title, why, "%d compared steps" % res["compared_steps"]))
        v_ok, v_lines = ui_verdict(res)
        for ln in v_lines:
            print(ln)
        if not gored:
            ok &= v_ok
    if gored:
        # Only A carries the poke, so A-vs-B and A-vs-C must BOTH go red -- and AT THE POKE STEP, not
        # merely somewhere. "It diverged" is satisfied by a crashed arm, by an unrelated promotion
        # bug, and by the harness residue this fixture already carries; "it diverged first at exactly
        # the step the poke names" is satisfied by the poke and very little else.
        #
        # B-vs-C IS REPORTED, NOT REQUIRED, and that is a deliberate weakening. The obvious stronger
        # clause -- "the unpoked pair must stay clean" -- reads well and is wrong here: B-vs-C has a
        # known residue at step 29,069, so requiring it would make every go-red
        # fail for a reason that has nothing to do with the poke, and the next person would either
        # delete the clause or stop running the arm. Tighten it when that residue is fixed.
        def st_of(t):
            return results[t]["channels"].get("state") or {}

        def red_at(t):
            s = st_of(t)
            return s.get("mismatch_count", 0) > 0 and s.get("first_mismatch") == gored

        bc = st_of("B vs C").get("first_mismatch")
        final = ok and red_at("A vs B") and red_at("A vs C")
        for t in ("A vs B", "A vs C"):
            s = st_of(t)
            print(
                "      GO-RED %s: `state` %s (poked at step %d, first mismatch %s)"
                % (
                    t,
                    "RED AT THE POKE"
                    if red_at(t)
                    else (
                        "RED but ELSEWHERE -- something other than the poke is being measured"
                        if s.get("mismatch_count")
                        else "did NOT diverge -- the comparison is blind"
                    ),
                    gored,
                    s.get("first_mismatch"),
                )
            )
        print(
            "      GO-RED B vs C (unpoked, reported only): first mismatch %s%s"
            % (bc, "  <-- at the poke step, so the poke LEAKED" if bc == gored else "")
        )
        print("\nui-abc (go-red): %s" % ("PASS" if final else "FAIL"))
        return 0 if final else 1
    print("\nui-abc: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def run_ui_oracle(args):
    """Build (or rebuild) a scenario's C-arm oracle from a recorded session."""
    src = args.ui_oracle
    log = src if os.path.isfile(src) else os.path.join(src, "mh_harness.log")
    if not os.path.isfile(log):
        print("FAIL: no harness log at %s" % log)
        return 1
    dst = args.ui_oracle_out
    if not dst:
        print("FAIL: --ui-oracle needs --ui-oracle-out <path>")
        return 1
    path, n = ui_oracle_extract(log, dst, {"source": os.path.basename(os.path.dirname(log) or log)})
    if not path:
        print("FAIL: the log carries no per-step region hashes (region_hash_step off?)")
        return 1
    print("wrote %s -- %d step(s), %.1f KB" % (path, n, os.path.getsize(path) / 1024.0))
    hist = ui_order_histogram(log)
    print("order-code histogram (paste into the scenario's `orders` key):")
    print("  " + json.dumps(hist, sort_keys=True))
    return 0


# ---- TACT-REC: the order journal ----------------------------------------------------------------

# Where a recorded journal is kept once it is worth keeping. A journal that survives is the whole
# point of the item, so it goes beside the archived session rather than in the lane.
JOURNAL_DIR = os.path.join(REPO, "tools", "uiscripts", "journals")


def tact_journal_extract(run_dir, dst, meta):
    """Pull the `TJ E/G/D` lines out of a recorded run and write a journal file.

    ONE FILE, and the DLL reads the same one a human commits: the parser skips anything that is not
    an E/G/D line, so the provenance header below costs nothing and a `diff` between two journals is
    meaningful. Returns (path, n_orders, n_direct) or (None, 0, 0)."""
    orders, direct, inputs = [], [], []
    combat, survivors, rec_line = "", "", ""
    for line in open(os.path.join(run_dir, "mh_harness.log"), encoding="utf-8", errors="replace"):
        if line.startswith("TJ "):
            body = line[3:].rstrip()
            if body[:2] in ("M ", "K ", "C "):
                inputs.append(body)
            elif body.startswith("D "):
                direct.append(body)
            else:
                orders.append(body)
        elif line.startswith("; TACT COMBAT"):
            combat = line.strip()
        elif line.startswith("; TACT SURVIVORS"):
            survivors = line.strip()
        elif line.startswith("; TJ RECORD:"):
            rec_line = line.strip()
    if not orders and not direct and not inputs:
        return None, 0, 0, 0
    # INPUT FIRST within a frame, then the direct writes, then the orders. The order matters on
    # replay: an injected click has to be in the ring before the frame body drains it, and a
    # selection write has to land before a group order qualifies on it.
    rank = {"M": 0, "K": 0, "C": 0, "D": 1}
    entries = sorted(
        orders + direct + inputs, key=lambda ln: (int(ln.split()[1]), rank.get(ln[0], 2))
    )
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("; mh TACTICAL ORDER JOURNAL (TACT-REC)\n")
        f.write(";\n")
        f.write("; Replayed by:  python tools/test_ui.py --tact-replay <this file>\n")
        f.write(
            "; Read directly by the DLL ([harness] tact_journal=<path>); every line that is not\n"
        )
        f.write("; a record line is skipped, which is why this header can live in the same file.\n")
        f.write(";\n")
        f.write(";   E <frame> <unit> <op> <iflag> <arg0> <arg1> <arg2> <arg3>   unit enqueue\n")
        f.write(";   G <frame> <op> <arg0> <arg1> <arg2> <arg3>                  group order\n")
        f.write(
            ";   D <frame> <unit> <field> <value>   direct write, no opcode exists for these:\n"
        )
        f.write(";                                     0=active_gun 1=squad_group_id 2=def_stat\n")
        f.write(";                                     3=SELECTED (status bit 0) -- group orders\n")
        f.write(";                                       act on `status & 1`, so without it a\n")
        f.write(";                                       group order replays onto an EMPTY set\n")
        f.write(";\n")
        f.write(
            "; INPUT -- the records that actually DRIVE a replay. Everything above is derived\n"
        )
        f.write("; from these by the game itself, and --tact-verify holds it back as the\n")
        f.write("; expectation rather than replaying it (the tactical-probe work 9p: on a real\n")
        f.write(
            "; session all 211 derived records were re-emitted from input alone, zero shift).\n"
        )
        f.write(";\n")
        f.write(";   M <frame> <type> <buttons> <x> <y> <dx> <dy> <wheel> <ts>   mouse event\n")
        f.write(";       type is a transition bitmask: 1=move 2=Ldown 4=Lup 8=Rdown 16=Rup\n")
        f.write(";       dx/dy/wheel are SIGNED; <ts> replays verbatim (double-click timing)\n")
        f.write(";   K <frame> <scancode> <type> <ts>                            key event\n")
        f.write(";   C <frame> <x> <y>                                           cursor position\n")
        f.write(";\n")
        for k, v in meta.items():
            f.write("; %-12s %s\n" % (k, v))
        f.write("; orders       %d\n" % len(orders))
        f.write("; direct       %d\n" % len(direct))
        f.write("; input        %d\n" % len(inputs))
        if rec_line:
            f.write("; %s\n" % rec_line.lstrip("; "))
        if combat:
            f.write("; %s\n" % combat.lstrip("; "))
        if survivors:
            f.write("; %s\n" % survivors.lstrip("; "))
        f.write(";\n")
        for e in entries:
            f.write(e + "\n")
    return dst, len(orders), len(direct), len(inputs)


def tact_journal_meta(path):
    """The `; key value` provenance header a recorded journal carries, as a dict.

    WHY THIS HAD TO EXIST (2026-09-05). The recorder has always written `system`, `save`, `squad`,
    `owner` and the recording's own `TACT COMBAT` census into the header -- and nothing ever read
    them back. `--tact-equiv` provisioned its lane from the CLI defaults, so a journal recorded on a
    non-default mission replayed against whatever the default save happened to load.

    That is not a cosmetic mismatch, and it is the worst shape a gate failure can take: BOTH arms
    load the same wrong mission, so they agree perfectly and the differential PASSES. Measured on the
    first POZ3 recording -- the journal's own census says `total 59->55` (8 player + 51 aliens) while
    the replay's says `total 24->24` (8 + 16). Green, and testing a different session than the one
    recorded. The three earlier journals hid it completely by being recorded on the default mission.
    """
    meta = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        t = line.strip()
        if not t.startswith(";"):
            if t:
                break  # header is over at the first record line
            continue
        parts = t[1:].split(None, 1)
        if len(parts) == 2 and parts[0].isalpha():
            meta.setdefault(parts[0].lower(), parts[1].strip())
        m = re.search(r"TACT COMBAT .*?total (\d+)->", t)
        if m:
            meta["roster_total"] = int(m.group(1))
    return meta


def tact_journal_roster_guard(journal, arm_name, combat_line):
    """(ok, message) -- does this arm's roster match the one the journal was recorded against?

    THE MECHANICAL HALF of the fix above, and the half that matters. Applying the journal's `system`
    is a thing a caller has to remember to do; this is a check that fires whether or not anybody
    remembered. A replay whose roster size differs from the recording's is not running the recorded
    mission, and no comparison over it means anything."""
    want = tact_journal_meta(journal).get("roster_total")
    if want is None or not combat_line:
        return True, None
    m = re.search(r"total (\d+)->", combat_line)
    if not m:
        return True, None
    got = int(m.group(1))
    if got == want:
        return True, None
    return False, (
        "arm %s loaded a roster of %d units but the journal was recorded against %d -- this is a "
        "DIFFERENT MISSION, and a differential over it agrees only because both arms are equally "
        "wrong." % (arm_name, got, want)
    )


def tact_journal_read(path):
    """[(frame, kind, [fields...])] from a journal file, header skipped."""
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        t = line.strip()
        if not t or t[0] not in "EGDMKC" or len(t) < 2 or t[1] != " ":
            continue
        p = t.split()
        out.append((int(p[1]), p[0], p))
    return out


def tact_journal_trim(src, dst, lead=1, edge=8):
    """Drop intermediate cursor motion, keeping the position that lands just before each action.

    94.7% of a recorded session is the mouse moving: 8,925 move events plus 4,633 cursor records out
    of 14,317. Only 386 button events, 162 keys and 211 derived records carry an action. Replaying a
    human's every twitch is most of the journal's size AND most of the injection work per frame, and
    a click carries its own x/y, so the intervening path is not needed to place the click.

    WHAT IS KEPT: every button event, every key event, and the last cursor record (`C`) plus the
    last `lead` move events in the frame window before each of those. The final cursor record is
    kept too, so a replay ends with the cursor where the recording left it.

    WHY THIS IS NOT OBVIOUSLY SAFE, and must be measured rather than assumed. The cursor is not only
    a pointer in this game:

      * unit FACING follows it continuously -- that is the op-6 "cursor-facing echo" the recorder
        drops, ~265 pseudo-orders in a run with no input at all;
      * moving to a screen EDGE scrolls the camera, and per the tactical-probe work 9k the camera
        viewport is what drives `llm_tact_unit_update_anim` -- so the cursor path reaches animation
        state, which is inside the hashed region.

    So a trimmed journal is a HYPOTHESIS about what the simulation depends on, and `--tact-verify`
    is exactly the instrument that tests it: if the trimmed input still makes the game emit all the
    recorded orders, the discarded motion did not matter. If it does not, the comparison names which
    orders it cost. Never ship a trimmed journal that has not passed that.

    Returns (path, kept, dropped)."""
    ev = tact_journal_read(src)
    if not ev:
        return None, 0, 0

    # EDGE MOTION IS NOT INTERMEDIATE -- it is the camera control, and dropping it is what made the
    # first cut of this trim fail. The symptom was exact enough to name the cause: every lost order
    # reappeared at the SAME frame with the SAME opcode and the SAME x, and a y off by a constant
    # +9 (the tactical-probe work 9q). A click resolves through the camera, so a view parked nine
    # tiles from where the human had it turns every later order into a different order.
    #
    # Cursor near a border scrolls the view, so those records are kept -- entry AND exit, since the
    # position persists between records: keeping only the entry parks the cursor at the edge and
    # scrolls forever. It is nearly free: 0.8% of this recording's 9,311 moves are within 8 px of a
    # border.
    xs = [int(p[4]) for _f, k, p in ev if k == "M"] or [0]
    ys = [int(p[5]) for _f, k, p in ev if k == "M"] or [0]
    scr_w, scr_h = max(xs), max(ys)

    def near_edge(p):
        x, y = int(p[4]), int(p[5])
        return x <= edge or y <= edge or x >= scr_w - edge or y >= scr_h - edge

    anchors = sorted({f for f, k, p in ev if (k == "M" and p[2] != "1") or k == "K"})
    keep = set()
    prev_edge = False
    for n, (f, k, p) in enumerate(ev):
        if k in ("E", "G", "D"):
            keep.add(n)  # derived records: the expectation, never replayed anyway
            continue
        if k == "K" or (k == "M" and p[2] != "1"):
            keep.add(n)  # a real action
        if k == "M":
            here = near_edge(p)
            if here or prev_edge:  # the exit record matters as much as the entry
                keep.add(n)
            prev_edge = here
        elif k == "C":
            x, y = int(p[2]), int(p[3])
            here = x <= edge or y <= edge or x >= scr_w - edge or y >= scr_h - edge
            if here or prev_edge:
                keep.add(n)
            prev_edge = here
    # For each anchor, walk BACKWARDS to the most recent cursor state before it.
    by_anchor = {a: [] for a in anchors}
    last_c = None
    last_moves = []
    ai = 0
    for n, (f, k, p) in enumerate(ev):
        while ai < len(anchors) and anchors[ai] < f:
            ai += 1
        if k == "C":
            last_c = n
        elif k == "M" and p[2] == "1":
            last_moves.append(n)
            del last_moves[:-lead]
        if ai < len(anchors) and anchors[ai] == f:
            if last_c is not None:
                by_anchor[anchors[ai]].append(last_c)
            by_anchor[anchors[ai]].extend(last_moves)
    for v in by_anchor.values():
        keep.update(v)
    # And the final cursor position, so a replay ends where the recording ended.
    for n in range(len(ev) - 1, -1, -1):
        if ev[n][1] == "C":
            keep.add(n)
            break

    kinds = {"E", "G", "D", "M", "K", "C"}
    header, out_records, seen = [], [], 0
    with open(src, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            t = line.rstrip("\n")
            if t[:1] in kinds and t[1:2] == " ":
                if seen in keep:
                    out_records.append(t)
                seen += 1
            else:
                header.append(t)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        for h in header:
            if h.startswith("; input ") or h.startswith("; orders ") or h.startswith("; direct "):
                continue
            f.write(h + "\n")
        f.write("; TRIMMED from %s -- intermediate cursor motion removed\n" % os.path.basename(src))
        f.write(
            "; kept %d of %d record(s); %d anchor(s) (click or key)\n"
            % (len(out_records), len(ev), len(anchors))
        )
        f.write(";\n")
        for t in out_records:
            f.write(t + "\n")
    return dst, len(out_records), len(ev) - len(out_records)


def tact_journal_shift(src, dst, index=None):
    """Write a copy of `src` with ONE REPLAYED record moved forward a frame.

    Clause 4's negative arm. Without it, "the replay is deterministic" is a claim about a journal
    nothing depends on: a replay that ignored its journal entirely would pass the two-replay check
    perfectly.

    WHAT GETS SHIFTED FOLLOWS WHAT GETS REPLAYED, and that changed under it. When the journal held
    orders, shifting an order was the arm. Now that a journal carrying input replays ONLY its input
    -- its orders being consequences the game re-emits by itself -- shifting an order moves a record
    that is never injected, and the arm would sit there proving nothing while looking rigorous. So
    shift a mouse BUTTON event when the journal has input, and fall back to an order only for a
    legacy order-only journal.

    Not a cursor move, for the same reason the verify mutation does not delete one: two thirds of an
    input journal is motion, and a move shifted by a frame is re-stated by the next one a frame
    later. A press is a cause. Returns (path, shifted_frame, description) or (None, 0, "")."""
    ev = tact_journal_read(src)
    if not ev:
        return None, 0, ""
    if index is not None:
        i = index
    else:
        ranked = [n for n, (_f, k, p) in enumerate(ev) if k == "M" and p[2] in ("2", "8")]
        if not ranked:  # a legacy order-only journal: there, the orders ARE the replayed records
            ranked = [n for n, (_f, k, _p) in enumerate(ev) if k in ("E", "G")]
        if not ranked:
            ranked = list(range(len(ev)))
        # The middle one: an early record may land before the units are doing anything, and a
        # divergence that would have happened anyway is not evidence.
        i = ranked[len(ranked) // 2]
    frame, kind, parts = ev[i]
    body = " ".join(parts)
    # RE-SORT AFTER SHIFTING. Editing the frame in place leaves the record one line ahead of where
    # its new frame belongs, and the DLL reads the file as a frame-ordered stream. That mattered
    # more than it looks: the replay cursor used to stall on the first out-of-order record and
    # silently issue nothing for the remaining 4,800 (the run played on and reported a complete
    # frame count). The cursor no longer jams, but a journal this tool writes should still be
    # well-formed -- a test artifact that only works because the reader tolerates it is a trap for
    # the next reader.
    header, records = [], []
    with open(src, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            t = line.rstrip("\n")
            if t.strip() == body:
                p = t.split()
                p[1] = str(frame + 1)
                records.append((frame + 1, len(records), " ".join(p)))
            elif t[:1] in ("E", "G", "D", "M", "K", "C") and t[1:2] == " ":
                records.append((int(t.split()[1]), len(records), t))
            else:
                header.append(t)
    records.sort(key=lambda r: (r[0], r[1]))
    with open(dst, "w", encoding="utf-8", newline="\n") as out:
        for h in header:
            out.write(h + "\n")
        for _f, _n, t in records:
            out.write(t + "\n")
    label = {
        "E": "unit order",
        "G": "group order",
        "M": "mouse event",
        "K": "key event",
        "D": "direct write",
        "C": "cursor move",
    }.get(kind, kind)
    if kind == "M":
        label = {
            "2": "left press",
            "4": "left release",
            "8": "right press",
            "16": "right release",
            "1": "mouse move",
        }.get(parts[2], label)
    return dst, frame + 1, "%s (record #%d) moved %d -> %d" % (label, i, frame, frame + 1)


def tact_combat(run_dir):
    """(units_lost, survivors, combat_line) from a run's TACT COMBAT / TACT SURVIVORS lines."""
    lost, surv, line = None, [], ""
    for ln in open(os.path.join(run_dir, "mh_harness.log"), encoding="utf-8", errors="replace"):
        if ln.startswith("; TACT COMBAT"):
            line = ln.strip().lstrip("; ")
            for tok in ln.split():
                if tok.startswith("units_lost_total="):
                    lost = int(tok.split("=")[1])
        elif ln.startswith("; TACT SURVIVORS"):
            surv = [int(x) for x in ln.split()[3:] if x.isdigit()]
    return lost, surv, line


def tact_journal_withheld(run_dir):
    """Derived records the replay deliberately did NOT inject, or 0.

    A journal carrying input replays only its input; its orders and direct writes are consequences
    the game re-emits unaided, so injecting them too would issue every order twice. Not
    hypothetical: that is what the hash arms were doing when they stopped at frame 14,681 under two
    different wall clocks, fighting a battle that lost 4 units where the recording lost 10."""
    for ln in open(os.path.join(run_dir, "mh_harness.log"), encoding="utf-8", errors="replace"):
        if ln.startswith("; TJ REPLAY WITHHELD"):
            return int(ln.split()[4])
    return 0


def tact_journal_armed(run_dir):
    """How many records the DLL said it LOADED, or None if it never got that far.

    Separate from tact_journal_verdict, which reads the line written when a run ENDS. Distinguishing
    the two is what tells a killed run ("armed 14317, then the wall") from a broken one ("never
    armed at all") -- and the first replay of a real recording reported the second while suffering
    the first."""
    for ln in open(os.path.join(run_dir, "mh_harness.log"), encoding="utf-8", errors="replace"):
        if ln.startswith("; TJ REPLAY ARMED:"):
            return int(ln.split()[4])
    return None


def tact_journal_dropped(run_dir):
    """How many journal lines the DLL REJECTED, or 0.

    Distinct from the lines it SKIPS. A journal deliberately carries a comment header, so "the
    parser ignored some lines" is normal and unremarkable; "the parser could not READ a record it
    recognised" is a mutilated replay wearing a green badge. G53: the field scanner rejected every
    negative mouse delta -- 2,154 of 9,311 events in the first real recording -- and the run still
    logged ARMED, still replayed, and would still have matched a second replay of the same damage."""
    for ln in open(os.path.join(run_dir, "mh_harness.log"), encoding="utf-8", errors="replace"):
        if ln.startswith("; TJ REPLAY DROPPED"):
            return int(ln.split()[4])
    return 0


def tact_journal_verdict(run_dir):
    """(loaded, issued) from the DLL's own TJ REPLAY line, or (None, None).

    Both line shapes: the pre-2026-09-07 `issued N of M loaded record(s)` and the widened
    (2fdfdc25) `issued N input record(s) + B barrier(s) of M loaded` -- the same alternation the
    --tact-equiv reader uses. The positional p[6] read broke on the widened line (ValueError)."""
    pat = re.compile(
        r"; TJ REPLAY: issued (\d+) (?:input record\(s\) \+ \d+ barrier\(s\) of|of) (\d+)"
    )
    for ln in open(os.path.join(run_dir, "mh_harness.log"), encoding="utf-8", errors="replace"):
        m = pat.search(ln)
        if m:
            return int(m.group(2)), int(m.group(1))
    return None, None


def tact_read(run_dir):
    """(armed, {frame: combined}, {frame: [per-region]}, poke_line, synth, {frame: sim}) per arm.

    `sim` is the SIM-ONLY verdict (the `TS` line): the same fold as `combined` with tact_units
    replaced by tact_units_sim, i.e. the roster without the render-written animation window. It is a
    SEPARATE hash rather than a mask on the first one on purpose -- masking would delete the signal,
    and the leading alternative cause of the tactical divergence is an occupancy loss in
    tile_objects, which is sim state (the tactical-probe work 9k). Two hashes can say WHICH; one
    masked hash cannot say anything.

    `synth` carries the workload's own self-report -- armed / summary / verdict / roster -- READ BACK
    OUT of the DLL's banner rather than assumed from what this runner passed. That distinction is the
    2026-08-05 soak lesson: a mode that trusts its own intent cannot see a key it
    failed to set, or one an inherited default overrode."""
    hp = os.path.join(run_dir, "mh_harness.log")
    combined, per, poke, sim = {}, {}, "", {}
    armed = {"cadence": False, "rand": False}
    synth = {"armed": False, "summary": "", "verdict": "", "roster": "", "orders": 0, "eff": 0}
    if not os.path.isfile(hp):
        return armed, combined, per, poke, synth, sim
    with open(hp, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("T "):
                fld = line.split()
                combined[int(fld[1])] = fld[2]
            elif line.startswith("TS "):
                fld = line.split()
                sim[int(fld[1])] = fld[2]
            elif line.startswith("TR "):
                fld = line.split()
                per[int(fld[1])] = fld[2:]
            elif "tact cadence ARMED" in line:
                armed["cadence"] = True
            elif "pin_rand ARMED" in line:
                armed["rand"] = True
            elif "TACT POKE" in line:
                poke = line.strip()
            elif "tact_synth ARMED" in line:
                synth["armed"] = True
            elif "TSYNTH roster" in line:
                synth["roster"] = line.strip().lstrip("; ")
            elif "TSYNTH SUMMARY" in line:
                synth["summary"] = line.strip().lstrip("; ")
                for tok in synth["summary"].split():
                    if tok.startswith("orders="):
                        synth["orders"] = int(tok.split("=")[1])
                    elif tok.startswith("effective="):
                        synth["eff"] = int(tok.split("=")[1])
            elif "TSYNTH VERDICT" in line:
                synth["verdict"] = line.strip().split(":")[-1].strip()
    return armed, combined, per, poke, synth, sim


def reloc_arm(args, name):
    """Is THIS arm the relocated one? (SB-HOSTFREE)

    `run_b` and only `run_b`, because the whole point is an A/B: run_a is the in-place reference and
    the mode's existing per-frame comparison then IS the claim. Relocating both would compare two
    relocated runs, which agree for the same reason two in-place runs do and would say nothing.
    """
    return bool(getattr(args, "tact_relocate", False)) and name == "run_b"


def tact_reloc_line(run_dir):
    """The `[reloc]` report line from an arm's harness log, or "" if the run did not relocate."""
    lp = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(lp):
        return ""
    with open(lp, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "[reloc]" in line:
                return line.strip()
    return ""


def tact_reloc_slices(line):
    """How many TACTICAL hash slices the relocated arm actually moved, off its own report line.

    Read back out of the DLL's banner rather than assumed from the flag, for the reason the whole
    item exists: a relocation that moved nothing the tactical hash reads would compare equal for
    free, and "I passed relocate_state=1" is not evidence that anything moved.
    """
    m = re.search(r"([0-9]+) tact", line or "")
    return int(m.group(1)) if m else 0


def tact_entered(run_dir):
    """The launch verb's own verdict line -- read, never inferred from the harness having logged."""
    lp = os.path.join(run_dir, "mh_launch.log")
    if not os.path.isfile(lp):
        return ""
    with open(lp, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "--tactical" in line:
                return line.strip()
    return ""


def tact_compare(a, b, which="full"):
    """(first_diverging_frame or None, n_diverging, n_common, regions_at_first), over the frames
    present in BOTH arms.

    `which` selects the verdict: "full" is the combined `T` hash over every slice, "sim" is the `TS`
    hash with the render-written animation window dropped. The per-region names reported at the
    first divergence come from the `TR` line either way, because that line carries both tact_units
    and tact_units_sim as separate columns -- so a "sim" verdict still names which slice moved."""
    (_, ca, pa, _, _, sa), (_, cb, pb, _, _, sb) = a, b
    if which == "sim":
        ca, cb = sa, sb
    common = sorted(set(ca) & set(cb))
    bad = [s for s in common if ca[s] != cb[s]]
    regions = []
    if bad and bad[0] in pa and bad[0] in pb:
        regions = [
            TACT_REGION_NAMES[i] if i < len(TACT_REGION_NAMES) else "idx%d" % i
            for i in range(min(len(pa[bad[0]]), len(pb[bad[0]])))
            if pa[bad[0]][i] != pb[bad[0]][i]
        ]
    return (bad[0] if bad else None), len(bad), len(common), regions


def run_tact_determinism(args):
    """TACT-PREP: the tactical determinism oracle. Returns a process exit code."""
    lane_dir = tact_provision_lane(args)
    if lane_dir is None:
        return 1

    # (name, poke_at, poke_idx, poke_off). TWO red arms when --tact-selftest is on, and the
    # second one is what makes the sim verdict mean anything:
    #
    #   poked      -- byte 0 of the poked region. For the default tact_units that is unit 0's
    #                 `type`, which BOTH verdicts hash, so both must go red. This is the standing
    #                 arm and it proves the oracle reads the region at all.
    #   poked_anim -- a byte INSIDE the render-written animation window (unit 0's anim_frame_time
    #                 low byte, a mantissa LSB, so the poke is numerically inert). The full verdict
    #                 MUST go red and the sim verdict MUST NOT. Without this arm, "the sim hash
    #                 ignores the animation window" is a claim about code nothing watched -- and a
    #                 sim hash that silently ignored the WHOLE roster would pass every run.
    arms = [("run_a", 0, -1, 0), ("run_b", 0, -1, 0)]
    if args.tact_selftest:
        arms.append(("poked", args.tact_poke_at, args.tact_poke_idx, 0))
        if args.tact_poke_idx == TACT_UNITS_IDX:
            arms.append(("poked_anim", args.tact_poke_at, TACT_UNITS_IDX, TACT_ANIM_POKE_OFF))

    # TACT-SYNTH: one seed for the whole invocation. Fresh per run so a fixed order sequence cannot
    # be the reason two arms agree; identical across the arms so the comparison is about the game.
    synth = None
    if args.tact_synth:
        synth = {
            "seed": args.tact_synth_seed or (int.from_bytes(os.urandom(3), "big") + 1),
            "at": args.tact_synth_at,
            "every": args.tact_synth_every,
            "units": args.tact_synth_units,
            "direct": 0 if args.tact_synth_funnel_only else 1,
        }
        print(
            "  tact_synth: seed=%d at=%d every=%d units=%d direct=%d"
            % (synth["seed"], synth["at"], synth["every"], synth["units"], synth["direct"])
        )

    ulo, uhi = _tact_range(args.tact_units_probe)
    flo, fhi = _tact_range(args.tact_fields)
    glo, ghi = _tact_range(args.tact_gates)
    dlo, dhi = _tact_range(args.tact_detail)

    results, report = {}, []
    reloc_lines = {}
    for name, poke_at, poke_idx, poke_off in arms:
        tact_write_config(
            lane_dir,
            args.tact_frames,
            poke_at,
            poke_idx,
            args.tact_squad,
            args.tact_hp,
            args.tact_owner,
            synth,
            args.tact_system,
            mouse_trace=1 if args.tact_mouse_trace else 0,
            unit_hash=1 if uhi else 0,
            detail_lo=dlo,
            detail_hi=dhi,
            gate_lo=glo,
            gate_hi=ghi,
            field_lo=flo,
            field_hi=fhi,
            poke_off=poke_off,
            relocate=1 if reloc_arm(args, name) else 0,
            relocate_poison=0 if getattr(args, "tact_relocate_nopoison", False) else 1,
            relocate_corrupt=(
                getattr(args, "tact_relocate_corrupt", "") if reloc_arm(args, name) else ""
            ),
        )
        print(
            "  arm %-10s (poke_at=%d idx=%d off=%d%s) ..."
            % (name, poke_at, poke_idx, poke_off, ", RELOCATED" if reloc_arm(args, name) else "")
        )
        run_dir = tact_run_arm(lane_dir, args.tact_save, args.tact_wall)
        if not run_dir:
            print("FAIL: arm %s produced no run folder" % name)
            return 1
        results[name] = tact_read(run_dir)
        armed, combined, _per, poke, syn, simh = results[name]
        entered = tact_entered(run_dir)
        report.append("  %-10s %s" % (name, os.path.basename(run_dir)))
        report.append("         %s" % (entered or "NO --tactical line in mh_launch.log"))
        if reloc_arm(args, name):
            reloc_lines[name] = tact_reloc_line(run_dir)
            report.append(
                "         %s"
                % (
                    reloc_lines[name].lstrip("; ")
                    or "NO [reloc] LINE -- the bind did not run, so this arm is NOT relocated"
                )
            )
        report.append(
            "         cadence ARMED=%s  rand ARMED=%s  frames=%d  distinct=%d  "
            "sim frames=%d distinct=%d"
            % (
                armed["cadence"],
                armed["rand"],
                len(combined),
                len(set(combined.values())),
                len(simh),
                len(set(simh.values())),
            )
        )
        if poke:
            report.append("         %s" % poke.lstrip("; "))
        if synth:
            report.append("         synth ARMED=%s  %s" % (syn["armed"], syn["roster"]))
            report.append("         %s" % (syn["summary"] or "NO TSYNTH SUMMARY line"))

    print("\n".join(report))
    ok = True

    # SHAPE RULES FIRST, before any hash verdict -- the lesson the other oracles already carry: an
    # arm that never armed, never entered, or never moved produces a green comparison for reasons
    # that have nothing to do with determinism.
    # THE SIM-VERDICT SHAPE RULE APPLIES TO EVERY ARM, the poked ones included. RED ARM 2's pass
    # condition is that the SIM verdict did NOT move -- which an absent `TS` line satisfies for free,
    # over an empty intersection. Checking it only on run_a/run_b would leave the arm that proves the
    # exclusion is real the one arm able to pass vacuously.
    for name in results:
        armed, combined, _p, _k, syn, simh = results[name]
        if len(simh) != len(combined):
            print(
                "FAIL: arm %s logged %d `T` frames but %d `TS` frames -- the sim verdict is not "
                "being emitted for every frame, so comparing it proves nothing."
                % (name, len(combined), len(simh))
            )
            ok = False
        if simh and len(set(simh.values())) < 2:
            print(
                "FAIL: arm %s's SIM slice never changed value over %d frames -- an unchanging "
                "verdict compares equal for free. Either the run is idle or the sim slice masks "
                "everything that moves." % (name, len(simh))
            )
            ok = False
        if name not in ("run_a", "run_b"):
            continue  # the rules below are about the arms being COMPARED
        if not armed["cadence"] or not armed["rand"]:
            print("FAIL: arm %s did not arm both hooks -- nothing was measured." % name)
            ok = False
        if synth:
            # The workload's OWN shape rules, and they are the reason --tact-synth is not just an
            # extra ini key: a zero-order run compares byte-identical for free and would otherwise
            # pass as coverage it never produced.
            if not syn["armed"]:
                print("FAIL: arm %s never armed tact_synth -- no player-command path ran." % name)
                ok = False
            if not syn["summary"]:
                print(
                    "FAIL: arm %s logged no TSYNTH SUMMARY -- the run stopped before the "
                    "verdict, so its coverage is unknown." % name
                )
                ok = False
            if syn["orders"] == 0 or syn["eff"] == 0:
                print(
                    "FAIL: arm %s issued %d orders of which %d changed anything -- an empty "
                    "workload is not coverage." % (name, syn["orders"], syn["eff"])
                )
                ok = False
            if syn["verdict"] and syn["verdict"] != "ok":
                print("FAIL: arm %s TSYNTH VERDICT: %s" % (name, syn["verdict"]))
                ok = False
        if len(combined) < args.tact_frames:
            print(
                "FAIL: arm %s logged %d frames, expected %d -- the run was cut short."
                % (name, len(combined), args.tact_frames)
            )
            ok = False
        if len(set(combined.values())) < 2:
            print(
                "FAIL: arm %s never changed state -- an IDLE world compares equal for free." % name
            )
            ok = False
    # SB-HOSTFREE: THE RELOCATED ARM MUST HAVE MOVED SOMETHING THE TACTICAL HASH READS. Passing
    # relocate_state=1 is not evidence; the count comes back out of the DLL's own banner. Without
    # this an arm that relocated nothing tactical would compare equal for free -- which is exactly
    # the vacuous shape the strategic side of this item spent a session repairing.
    if getattr(args, "tact_relocate", False):
        line = reloc_lines.get("run_b", "")
        moved = tact_reloc_slices(line)
        if not line:
            print(
                "FAIL: run_b has no [reloc] line -- the relocating bind never ran, so this is two "
                "in-place runs wearing an A/B label."
            )
            ok = False
        elif moved <= 0:
            print(
                "FAIL: run_b relocated no TACTICAL hash slice (%s) -- the comparison cannot see "
                "the move, so agreement proves nothing." % (line or "?")
            )
            ok = False
        else:
            report.append("  relocated arm moved %d tactical hash slice(s)" % moved)

    if not ok:
        print("tact-determinism: FAIL (shape)")
        return 1

    # TWO verdicts, reported separately and weighted differently (TACT-REC 2026-08-25).
    #
    # The full `T` hash covers the whole tactical arena INCLUDING the animation fields that
    # llm_tact_unit_update_anim writes -- and that function is reachable only through
    # llm_tact_render_view's camera-viewport tile scan (the tactical-probe work 9k), so those fields
    # are a function of what was drawn. The measured intermittent divergence lives exactly there.
    #
    # The SIM verdict is what a shadow arm or a journal replay actually needs to be true. So:
    # a sim divergence FAILS; a full-only divergence is reported loudly as PRESENTATION DRIFT and
    # does not fail unless --tact-strict. That is not a softened gate -- it is a narrower one whose
    # red arm is checked in both directions above, and the drift is printed with its frame and
    # regions every time rather than being masked out of existence.
    ffirst, fn, common, fregions = tact_compare(results["run_a"], results["run_b"], "full")
    sfirst, sn, _sc, sregions = tact_compare(results["run_a"], results["run_b"], "sim")

    if sfirst is None:
        print("  A vs B  SIM : IDENTICAL over %d frames" % common)
    else:
        print(
            "  A vs B  SIM : DIVERGED at frame %d (%d frames differ) regions=%s"
            % (sfirst, sn, sregions)
        )
        ok = False

    if ffirst is None:
        print("  A vs B  FULL: IDENTICAL over %d frames" % common)
    elif sfirst is None:
        print(
            "  A vs B  FULL: PRESENTATION DRIFT at frame %d (%d frames differ) regions=%s\n"
            "                 The full hash moved and the SIM hash did not, so what differs is "
            "inside the\n"
            "                 render-written animation window (anim_frame_time / frame_index /\n"
            "                 anim_cycle_time / frame_interval / sprite_id). Not a simulation "
            "divergence.\n"
            "                 Reported, never masked -- see the tactical-probe work 9k/9l."
            % (ffirst, fn, fregions)
        )
        if args.tact_strict:
            print("                 --tact-strict: counted as a FAILURE.")
            ok = False
    else:
        print(
            "  A vs B  FULL: DIVERGED at frame %d (%d frames differ) regions=%s"
            % (ffirst, fn, fregions)
        )

    if args.tact_selftest:
        ok = tact_red_arms(args, results, ok)

    print("tact-determinism: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def tact_red_arms(args, results, ok):
    """The negative arms. Returns the updated `ok`.

    Arm 1 (`poked`) is the standing one: a poke at the region base must diverge at exactly the poke
    frame and name exactly the region it hit -- with, for tact_units, its alias alongside, because
    byte 0 is `type` and both slices emit it.

    Arm 2 (`poked_anim`) is the SPLIT's red arm and only runs when tact_units is the poked region:
    a poke inside the animation window must move the FULL verdict and must leave the SIM verdict
    alone. Both halves are asserted. A sim hash that ignored the whole roster would pass the second
    half and fail the first; one that ignored nothing would pass the first and fail the second."""
    want = (
        TACT_REGION_NAMES[args.tact_poke_idx]
        if 0 <= args.tact_poke_idx < len(TACT_REGION_NAMES)
        else "?"
    )
    # Byte 0 of tact_units is `type`, which the sim emitter keeps -- so the alias column moves too.
    want_full = [want, "tact_units_sim"] if args.tact_poke_idx == TACT_UNITS_IDX else [want]

    pfirst, pn, pcommon, pregions = tact_compare(results["run_a"], results["poked"], "full")
    if pfirst is None:
        print(
            "  RED ARM 1: FAIL -- poking %s at frame %d changed NOTHING. The oracle is not "
            "reading those bytes; a green above proves nothing." % (want, args.tact_poke_at)
        )
        ok = False
    elif pfirst != args.tact_poke_at:
        print(
            "  RED ARM 1: FAIL -- diverged at frame %d, expected exactly %d (poke frame)."
            % (pfirst, args.tact_poke_at)
        )
        ok = False
    elif sorted(pregions) != sorted(want_full):
        print(
            "  RED ARM 1: FAIL -- diverged at the right frame but named %s, expected %s."
            % (pregions, want_full)
        )
        ok = False
    else:
        sfirst, _sn, _sc, _sr = tact_compare(results["run_a"], results["poked"], "sim")
        if sfirst != args.tact_poke_at:
            print(
                "  RED ARM 1: FAIL -- the FULL verdict fired at frame %d but the SIM verdict fired "
                "at %s. Byte 0 of tact_units is `type`, which the sim slice keeps, so it must fire "
                "too -- a sim hash that stayed quiet here is reading the wrong bytes."
                % (pfirst, sfirst)
            )
            ok = False
        else:
            print(
                "  RED ARM 1: ok -- poking %s at frame %d moved BOTH verdicts at exactly frame %d, "
                "naming %s (%d frames differ of %d)"
                % (want, args.tact_poke_at, pfirst, pregions, pn, pcommon)
            )

    if "poked_anim" not in results:
        return ok
    afirst, an, _ac, aregions = tact_compare(results["run_a"], results["poked_anim"], "full")
    asim, _asn, _asc, _asr = tact_compare(results["run_a"], results["poked_anim"], "sim")
    if afirst != args.tact_poke_at:
        print(
            "  RED ARM 2: FAIL -- poking the animation window (+0x%02x) at frame %d moved the FULL "
            "verdict at %s, expected exactly %d. The full hash is not reading those bytes, so "
            "'presentation drift' would be unfalsifiable."
            % (TACT_ANIM_POKE_OFF, args.tact_poke_at, afirst, args.tact_poke_at)
        )
        ok = False
    elif aregions != ["tact_units"]:
        print(
            "  RED ARM 2: FAIL -- the animation poke named %s, expected exactly ['tact_units']. "
            "Naming tact_units_sim too means the sim slice is hashing the window it claims to drop."
            % (aregions,)
        )
        ok = False
    elif asim is not None:
        print(
            "  RED ARM 2: FAIL -- the SIM verdict moved at frame %d from a poke inside the "
            "animation window. The exclusion is not doing what it says." % asim
        )
        ok = False
    else:
        print(
            "  RED ARM 2: ok -- poking +0x%02x (anim_frame_time) at frame %d moved the FULL verdict "
            "at exactly frame %d naming only tact_units, and left the SIM verdict untouched over "
            "the whole run (%d frames differ). The sim/presentation split is real in BOTH "
            "directions." % (TACT_ANIM_POKE_OFF, args.tact_poke_at, afirst, an)
        )
    return ok


def regions_mismatch(got, want):
    """The poked region must be the ONLY one named at the poke frame."""
    return got != [want]


def run_soak(args):
    """The all-AI soak. Returns a process exit code."""
    # --soak-slot: one lane per slot out of the `soak` block. migration_ab uses disjoint slots to run
    # its fixtures and verification arms concurrently (2026-09-10).
    #
    # THE NUMBERS ARE ALLOCATED NOW (fork F4H). They were `32 if slot == 0 else 50 + slot` under a
    # comment that listed the other consumers as "suite 1..~20, 31, 32, 33, tact ~34-36" -- and the
    # capture suite had since grown to 36 lanes, so ui_soak (32) WAS the suite's `pause_mp_gate` host.
    # Six gate reds came out of that one aliasing: the gate runs this soak for six minutes beside the
    # suite, and the second game to take the shared "MHMut32" dies at boot without a word.
    slot = int(getattr(args, "soak_slot", 0) or 0)
    lane = "ui_soak" if slot == 0 else "ui_soak%d" % slot
    lane_no = lane_alloc.lane("soak", slot)
    lane_dir = os.path.join(make_lane.LANE_ROOT, lane)
    print("provisioning lane %s ..." % lane)
    r = subprocess.run(
        [
            sys.executable,
            os.path.join(REPO, "tools", "make_lane.py"),
            "--name",
            lane,
            "--lane",
            str(lane_no),
            "--port",
            str(LOCAL_PORT_BASE + lane_no),
        ]
        + (["--dll", args.soak_dll] if getattr(args, "soak_dll", "") else [])
        + (["--visible"] if args.visible else ["--headless"]),
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print("lane FAILED: %s" % (r.stderr or r.stdout).strip()[:300])
        return 1

    hextra = SOAK_HARNESS_EXTRA
    hextra += ";all_ai_observer=%d" % args.soak_observer
    hextra += ";gameover_step=%d" % args.soak_gameover_step
    # CAPPED, not just derived. A bare steps//10 put the first probe at step 3000 of a 30000-step
    # budget -- and that run ended at 1475, so no AIPROBE line was ever emitted and the shape rule
    # reported the AI mask as unobserved. The probe has to fire early enough to describe a run that
    # ends early, which is exactly the run you most want described.
    hextra += ";ai_probe_step=%d" % max(1, min(args.steps // 10, 200))
    # The save-seeded start. Same lever the AI migration already uses for shadow coverage
    # (the save index): a developed map reaches branches no number of steps from the default
    # start will. Kept as an OPTION rather than the default because the default start is what makes
    # a soak's trajectory reproducible.
    #
    # THE FILE IS STAGED HERE, and that is a FIX rather than a convenience (2026-08-05). Until now
    # the flag only appended the harness keys and left staging to the operator -- which cannot work,
    # because make_lane.py rmtree()s the lane a few lines above, so anything copied into
    # <lane>/save/ beforehand is deleted by the very run that wants it. That is why the 2026-08-05
    # session's `--soak-save ayy30` logged `; [save] LOADGAME ... rc=0` and silently continued on
    # the default start: the save was fine (<SAVE_STORAGE>/ayy30.sav exists and the
    # index covers it), the lane simply never had it. Neither of the two hypotheses recorded at the
    # time was this one. A missing source file now REFUSES the run instead of producing a quiet
    # wrong-scenario result.
    if args.soak_save:
        # Committed copy (tools/uiscripts/saves/) wins over SAVE_STORAGE (fork F1D).
        src = ui_test.resolve_save(args.soak_save, SAVE_STORAGE)
        if not os.path.isfile(src):
            print("soak: no such save %s (not in tools/uiscripts/saves/ either)" % src)
            print("      (name it WITHOUT the extension; see tools/data/save_index.json)")
            return 1
        save_dir = os.path.join(lane_dir, "save")
        os.makedirs(save_dir, exist_ok=True)
        shutil.copy2(src, os.path.join(save_dir, args.soak_save + ".sav"))
        print("soak: staged %s -> %s" % (os.path.basename(src), save_dir))
        hextra += ";loadgame_at=%d;loadgame_name=%s" % (args.soak_load_at, args.soak_save)
    if args.harness_extra:
        hextra += ";" + args.harness_extra

    argv = [
        SOAK_SCRIPT,
        "--harness",
        "--steps",
        str(args.steps),
        "--harness-extra",
        hextra,
        "--host-dir",
        lane_dir,
        "--timeout-frames",
        str(LOCAL_TIMEOUT_FRAMES),
        # A soak is step-bound, and steps/s is ~100 in every configuration (measured 2026-08-01), so
        # the default 90 s timeout truncates anything past ~9000 steps. Derived from the budget
        # rather than left to the caller, because a silent truncation is the failure mode.
        "--timeout",
        str(max(args.timeout, 240 + args.steps // 40)),
        "--headless" if not args.visible else "--visible",
    ]
    # SHADOW FRAGMENTS. A soak is the coverage lever for AI sites that need things to DIE, and
    # arming one is an ini gate -- so the soak has to be able to carry the fragment, exactly as
    # --determinism does. Added 2026-08-05 for RI-AI batch C, whose notify_object_removed site is
    # unreachable in any quiet scenario. Repeatable; the arming-set checker ran over the set
    # first, because a checker cannot see what it was not given.
    for frag in args.extra_ini:
        argv += ["--extra-ini", frag]
    if args.soak_speed:
        argv += ["--net-extra", "game_speed_pct=%d" % args.soak_speed]
    # FORWARD THE DLL TO THE RUNNER TOO, not just to make_lane. ui_test.py re-copies the Release
    # build into every peer at launch (local_launch), which silently undid the lane's --dll and is
    # how the sim coverage baseline came to be recorded against an optimised binary.
    if getattr(args, "soak_dll", ""):
        argv += ["--dll", args.soak_dll]
        # MEASURED CEILING, 2026-08-05. The 2026-08-01 measurements establish that stock mode 2 scales to
        # 80x with no throughput shortfall and that the limit there is FIDELITY -- this is what that
        # costs in practice. At 8000% (1333 ms of game time per step) an 8-way AI match stops
        # stepping at step 1475, reproducibly, with no player eliminated; at 1000% the same ~33
        # minutes of game time runs clean and keeps going. Warned rather than clamped: the ceiling
        # is a property of the WORLD (eight AIs), so a different scenario may sit elsewhere and a
        # hard limit here would be a guess dressed as a rule.
        if args.soak_speed > 2000:
            print(
                "      WARNING: --soak-speed %d is above the measured usable band for a soak. An "
                "8-way AI match stalls at step 1475 at 8000%%; 1000%% is clean." % args.soak_speed
            )
    print(
        "soak: %d steps, observer=%d, speed=%s"
        % (args.steps, args.soak_observer, args.soak_speed or "default")
    )
    t0 = time.time()
    rc = subprocess.call([sys.executable, os.path.join(REPO, "tools", "ui_test.py")] + argv)
    elapsed = time.time() - t0
    log_dir = sp_newest_run(lane_dir)
    if not log_dir:
        print("soak: no run directory produced")
        return 1

    print("\n==== all-AI soak ====")
    print(
        "      lane %s, %.1f s wall (%.1f steps/s)" % (lane, elapsed, args.steps / max(elapsed, 1))
    )
    ok, lines, seg = soak_report(log_dir, args)
    for ln in lines:
        print(ln)
    if args.soak_golden and seg is not None:
        gok, glines = soak_golden(seg, args.soak_golden, args)
        for ln in glines:
            print(ln)
        ok = ok and gok
    # A nonzero ui_test rc is reported but does NOT override the shape rules: the scenario can end
    # "unsuccessfully" (no captures) while the harness ran a perfectly good soak, and conversely a
    # green scenario can carry a vacuous run. The shape rules are the verdict.
    if rc != 0:
        print(
            "      note: ui_test returned %d (scenario-level); the verdict above is the run's." % rc
        )
    print("soak: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def run_sp_determinism(args):
    """P0-SPDET: run the SP oracle. Returns a process exit code."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mp_analyze as _m

    # make_lane names the folder EXACTLY as --name, while the suite's own lanes carry a "ui_"
    # prefix (lane_names). Use the prefixed form for both so this lane sits with the others.
    lane = "ui_sp_det"
    lane_dir = os.path.join(make_lane.LANE_ROOT, lane)
    print("provisioning lane %s ..." % lane)
    r = subprocess.run(
        [
            sys.executable,
            os.path.join(REPO, "tools", "make_lane.py"),
            "--name",
            lane,
            "--lane",
            # fork F4H: allocated, not the literal 31 it used to be -- which the capture suite had
            # grown onto (`pause_hotkey`), so the gate's spdet unit and that scenario shared a mutex.
            str(lane_alloc.lane("sp_det", 0)),
            "--port",
            str(LOCAL_PORT_BASE + lane_alloc.lane("sp_det", 0)),
        ]
        + (["--visible"] if args.visible else ["--headless"]),
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print("lane FAILED: %s" % (r.stderr or r.stdout).strip()[:300])
        return 1

    # ONE seed for BOTH arms. Different seeds would make the synthetic workload differ and every
    # divergence would be expected -- the run would be red for a reason that is not the engine.
    # ONE seed, pushed through --harness-extra rather than a new flag: ui_test draws SYNTH_SEED per
    # INVOCATION, and two invocations is exactly what this shape is. The key-merge makes the extra
    # override the template's value in place instead of duplicating it.
    seed = str(args.sp_seed or (int.from_bytes(os.urandom(3), "big") + 1))
    hextra = SP_HARNESS_EXTRA + ";synth_seed=" + seed
    # THE GAME-SPEED PIN (AI0). Goldens are NOT portable across speeds -- measured 2026-08-01: two
    # stock mode-2 runs at 100% and 1000% diverge on essentially every region from step 1, because a
    # step at 1000% integrates ten times as much game time. So the speed is a property OF THE RUN,
    # REQUESTED here and then VERIFIED from each arm's own log below, never assumed from the request.
    # Pushed through --net-extra, which OVERRIDES the key in place; a second ini fragment carrying its
    # own [net] block would be the duplicate-section trap ini_merge_fragment exists for.
    net_extra = "game_speed_pct=%d" % args.sp_game_speed if args.sp_game_speed else None
    arms = [("baseline", False), ("promoted", True)]
    if args.sp_selftest:
        # (1) of done_when: the SAME build twice. If this is not identical the oracle is not
        # deterministic and clause (2) means nothing.
        arms = [("run_a", False), ("run_b", False)]

    # ---- THE ARM REFUSAL, ASKED BEFORE THE RIG (F2C) ---------------------------------------------
    #
    # Each arm is now one `[config] mode` line, so "do these two arms describe different
    # configurations" is answerable from the fragments -- no run needed. sp_arm_report's own refusal
    # stays and is the one that matters (it reads what the DLL DID, which is the only thing that can
    # catch a fragment that never took); this one costs nothing and fails in seconds instead of after
    # two headless arms. DELIBERATELY SKIPPED under --sp-selftest: that mode runs the SAME build
    # twice on purpose, so identical arms are its premise rather than its failure.
    if not args.sp_selftest:
        modes = {n: ini_file_mode(SP_PROMOTE_INI if p else SP_ROLLBACK_INI) for n, p in arms}
        (na0, _), (nb0, _) = arms[0], arms[1]
        refusal, note = sp_mode_refusal(modes[na0], modes[nb0], na0, nb0)
        if refusal:
            print("\n      REFUSED: " + refusal)
            print("\nP0-SPDET: FAIL (the two arms are the same configuration)")
            return 1
        print("  " + note)

    segs, ok, report, speeds = {}, True, [], {}
    for name, promoted in arms:
        argv = [
            SP_SCRIPT,  # POSITIONAL in ui_test.py -- the single-peer form is `ui_test.py <script>`
            "--harness",
            "--steps",
            str(args.steps),
            "--harness-extra",
            hextra,
            "--host-dir",
            lane_dir,
            # Headless runs at several thousand fps, so ui_test's 1500-frame per-step watchdog fires
            # in seconds. Same number the local capture suite passes.
            "--timeout-frames",
            str(LOCAL_TIMEOUT_FRAMES),
            "--timeout",
            str(max(args.timeout, 180 + args.steps)),
            "--headless" if not args.visible else "--visible",
        ]
        argv += ["--extra-ini", SP_PROMOTE_INI if promoted else SP_ROLLBACK_INI]
        if net_extra:
            argv += ["--net-extra", net_extra]
        if args.sp_perturb and name in ("promoted", "run_b"):
            # (3) of done_when: a deliberately wrong body must drive this RED. A gate never watched
            # to fail is not evidence.
            argv += ["--harness-extra", hextra + ";" + args.sp_perturb]
        print("\n=== SP arm: %s (promoted=%s) ===" % (name, promoted))
        run_ui_test(argv, max(args.per_test_timeout, 240 + args.steps))
        rd = sp_newest_run(lane_dir)
        if not rd:
            print("  no run directory produced -- arm %s did not launch" % name)
            return 1
        a_ok, a_lines = sp_arm_report(rd, name, promoted)
        ok &= a_ok
        report += a_lines
        hl = os.path.join(rd, "mh_harness.log")
        if not os.path.isfile(hl):
            print("  arm %s produced no mh_harness.log (%s)" % (name, rd))
            return 1
        segs[name] = _m.parse_harness(hl)
        speeds[name] = sp_arm_game_speed(rd)
        print("  arm %s -> %s" % (name, rd))

    print("\n" + "=" * 78 + "\nP0-SPDET verdict\n" + "=" * 78)
    for ln in report:
        print(ln)
    (na, _), (nb, _) = arms[0], arms[1]

    # ---- THE CROSS-SPEED REFUSAL (AI0) -----------------------------------------------------------
    #
    # A step at 1000% integrates ten times the game time of a step at 100%, so two runs recorded at
    # different speeds are two different experiments and their per-step hashes have nothing to say
    # about each other. Comparing them would go RED and read as a determinism bug -- which is the
    # expensive direction, since the real cause is a config difference nobody wrote down.
    #
    # REFUSE, DO NOT WARN, and refuse BEFORE printing a verdict: a warning above a red comparison is
    # read as noise above a result, and the point is that there IS no result. `sp_compare` is not
    # even called. The decision itself is sp_speed_refusal(), unit-tested in --det-selftest -- see
    # its docstring for why it could not be mutation-checked on a live run.
    refusal, note = sp_speed_refusal(speeds, na, nb)
    if refusal:
        print("\n      REFUSED: " + refusal)
        print("\nP0-SPDET: FAIL (cross-speed comparison refused)")
        return 1
    print("      " + note)

    # X-SPINE clause (6a): compare EVERY per-region column of the `R` line, not only the eight the
    # MP verdict excludes. This is the arm-symmetric verdict channel -- address-content, per step,
    # readable identically whichever implementation wrote the bytes -- and passing the whole column
    # set is what makes it a verdict rather than the localisation aid diff_peers uses it as. See
    # mp_analyze.SP_ALL_REGIONS for the framing and for the sampling-trigger caveat.
    res = _m.sp_compare(segs[na], segs[nb], na, nb, time_regions=_m.SP_ALL_REGIONS)
    print("      compared steps: %d" % res["compared_steps"])
    for ch, v in res["channels"].items():
        print(
            "      %-14s compared %-6d mismatches %-6d first %s"
            % (ch, v["compared"], v["mismatch_count"], v["first_mismatch"])
        )
    if res.get("uncompared_regions"):
        print("      UNCOMPARED regions: %s" % ", ".join(res["uncompared_regions"]))
    print("      %s" % res["verdict"])
    verdict_ok = res["ok"]
    if args.sp_perturb:
        # Inverted: with a perturbation armed the run MUST diverge.
        verdict_ok = not res["ok"]
        print("      (--sp-perturb: a RED comparison is the PASS condition)")
    final = ok and verdict_ok
    print("\nP0-SPDET: %s" % ("PASS" if final else "FAIL"))
    return 0 if final else 1


def soak_ai_premise(text, ai_on, nplay, mask):
    """(ok, report lines) for the AI-premise half of the soak shape rules.

    EXTRACTED so it can be tested WITHOUT A RIG (2026-09-05). The premise is per scenario kind and it
    is silent when it is working, which is exactly the class det_standard_selftest exists for -- and
    this rule spent the day asserting the wrong thing for save-loaded soaks with nobody able to check
    it offline. Every arm is exercised in that selftest.
    """
    lines, ok = [], True
    n = int(nplay)
    seated = mask[:n]
    lines.append(
        "      AI: master_gate=%s players=%s mask=[%s] (claimed slots: %s)"
        % (ai_on, nplay, mask, "all AI" if seated.count("1") == n else "MIXED")
    )
    # THE PREMISE IS PER SCENARIO KIND, AND IT IS DERIVED FROM THE RUN (2026-09-05, user's call).
    # "every seat thinks" is the DEFAULT-START soak's premise and is simply the wrong assertion for a
    # save-loaded one: the ALLAI conversion happens at LANDING, then LOADGAME REPLACES the world with
    # the save's own roster, so a developed 2-player save is MIXED by construction. That scenario is
    # registered for COVERAGE BREADTH -- a quieter developed world reaches branches an empty 8-way
    # start never does -- and holding it to the busy-start premise made it report `soak: FAIL` on all
    # three runs while coverage.py pinned a baseline from them anyway (tracker SOAK-SAVED-MIXED).
    #
    # DERIVED, not declared: the registry carried `soak_shape_fails: True` on that scenario for exactly
    # this, and nothing ever read it -- a dead flag whose only effect was to make a reader think the
    # case was handled. The LOADGAME line is a fact about THIS run, so the check asks the run instead.
    # Same reasoning as SIM1-P clause 6a preferring a derived list to a hand-kept one.
    loaded = re.search(r"; \[save\] LOADGAME step=(\d+) name=(\S+) -> rc=1", text)
    if ai_on == "0":
        ok = False
        lines.append(
            "      FAIL: the AI master gate is OFF, so no seat thinks at all. That is fatal for every "
            "soak shape, loaded or not."
        )
    elif loaded:
        # The weaker premise, and it still has teeth: a loaded world with ZERO AI simulates nothing and
        # would report coverage of an idle map.
        if seated.count("1") == 0:
            ok = False
            lines.append(
                "      FAIL: after LOADGAME at step %s the save's roster has NO AI-enabled slot, so "
                "nothing drives the world and any coverage from it is of an idle map."
                % loaded.group(1)
            )
        else:
            lines.append(
                "      AI: save-loaded premise applied (LOADGAME step=%s name=%s) -- %d of %d claimed "
                "slots think, which is what this scenario kind asserts; 'every seat thinks' is the "
                "default-start premise and does not survive a world replacement."
                % (loaded.group(1), loaded.group(2), seated.count("1"), n)
            )
    elif seated.count("1") != n:
        ok = False
        lines.append(
            "      FAIL: %d of %d claimed slots are not AI-enabled. The default-start soak's whole "
            "premise is that every seat thinks (a save-loaded run is judged by the weaker premise "
            "instead, and this run loaded nothing)." % (n - seated.count("1"), n)
        )
    return ok, lines


def det_standard_selftest():
    """C7's three rules, checked without a rig. Part of lint_repo, because all three are about a run
    NOT meaning what it appears to mean -- and every one of them is silent when it is working."""
    import shutil
    import tempfile

    fails = []

    def check(name, cond):
        print("   %-64s %s" % (name, "ok" if cond else "FAIL"))
        if not cond:
            fails.append(name)

    # THE SOAK AI-PREMISE ARMS (SOAK-SAVED-MIXED, 2026-09-05). Five, because the rule is per scenario
    # KIND and it spent a day asserting the default-start premise against a save-loaded scenario --
    # reporting `soak: FAIL` on every soak_saved run while the recorder pinned a baseline from them. A
    # rule that is silent when it works and wrong when it does not is precisely what this selftest is
    # for, and none of these needs a rig.
    _LOADED = "; [save] LOADGAME step=120 name=ayy30 -> rc=1 (container_load promoted=0)"
    check(
        "premise: default start, every seat thinks -> ok",
        soak_ai_premise("", "1", "8", "11111111")[0],
    )
    check(
        "premise: default start, one seat idle -> FAIL (the busy-start premise)",
        not soak_ai_premise("", "1", "8", "11111110")[0],
    )
    check(
        "premise: save-loaded, 1 of 2 thinks -> ok (the save's roster is not the soak's)",
        soak_ai_premise(_LOADED, "1", "2", "01000000")[0],
    )
    check(
        "premise: save-loaded with ZERO AI -> FAIL (coverage of an idle map)",
        not soak_ai_premise(_LOADED, "1", "2", "00000000")[0],
    )
    check(
        "premise: master gate OFF -> FAIL whatever the shape",
        not soak_ai_premise(_LOADED, "0", "2", "01000000")[0],
    )
    check(
        "premise: the save-loaded arm SAYS which premise it applied",
        any(
            "save-loaded premise applied" in l
            for l in soak_ai_premise(_LOADED, "1", "2", "01000000")[1]
        ),
    )

    knobs = migrated_fix_knobs()
    sources = migrated_fix_knob_sources()
    check("migrated-fix knob set is non-empty", bool(knobs))
    # U20 (f): the derivation must cover BOTH halves. A manifest-only list silently omits every fix
    # that never had a byte patch -- which is how `resync_trigger_gate` (ini default 1, its carriers
    # retired by C8-e) came to sit live on the promoted peer and absent on the original one while the
    # oracle reported green. Assert each half is actually contributing rather than trusting the union.
    check(
        "...and it is derived from BOTH sources (a manifest-only list is the U20 (f) hole)",
        any(v == "manifest" for v in sources.values())
        and any("reimpl_fixes" in v for v in sources.values()),
    )
    check(
        "...covering resync_trigger_gate, the fix that was already through that hole",
        "resync_trigger_gate" in knobs,
    )
    check(
        "...and desync_icon_gate, the reimpl-only fix U20 added",
        "desync_icon_gate" in knobs,
    )
    check(
        "the promotion fragment is NOT refused (promotion is the asymmetry these runs are for)",
        asymmetric_fix_config_error(PROMOTE_INI) is None,
    )
    d = tempfile.mkdtemp(prefix="c7_selftest_")
    try:
        bad = os.path.join(d, "bad.ini")
        with open(bad, "w", encoding="utf-8") as f:
            f.write("[net]\n%s=1\n" % knobs[0])
        msg = asymmetric_fix_config_error(bad)
        check("a host-only fragment setting a migrated-fix knob is REFUSED", bool(msg))
        check("...and the refusal NAMES the knob", bool(msg) and knobs[0] in msg)

        def peer(name, promoted):
            os.makedirs(os.path.join(d, name), exist_ok=True)
            with open(os.path.join(d, name, "mh_net.log"), "w", encoding="utf-8") as f:
                if promoted:
                    f.write("; [promote] lockstep: RUN-CONFIG: SHIP (whole closure)\n")
                    f.write("; [promote] pump: call #1 (OURS is live)\n")
                else:
                    f.write("; nothing promoted here\n")

        peer("host", True)
        peer("client1", False)
        check(
            "asymmetric shape PASSES when the host runs ours and the client the original",
            det_run_report(d, {"host": True, "client1": False})[0],
        )
        check(
            "...and FAILS when read as the symmetric shape (the client is not promoted)",
            not det_run_report(d, {"host": True, "client1": True})[0],
        )
        peer("host", False)
        check(
            "a run whose promotion never went live FAILS despite whatever the hash said",
            not det_run_report(d, {"host": True, "client1": False})[0],
        )
        peer("host", True)
        peer("client1", True)
        check(
            "an ASYMMETRIC shape whose client picked up promotion too FAILS (symmetry both ways)",
            not det_run_report(d, {"host": True, "client1": False})[0],
        )

        # THE LIVENESS LINES ARE NOT SUBSYSTEM-SCOPED, and reading them as if they were kept the
        # ASYMMETRIC shape red for a whole session after the shape itself had been repaired
        # (2026-09-01). mh::orders emits the identical `(OURS is live)` text with bare seam names and
        # SHIP_PROMOTE_ORDERS is 1, so a client correctly running the ORIGINAL lockstep closure still
        # logs ~18 of them. The verdict has to come from RUN-CONFIG, which IS scoped.
        peer("host", True)
        os.makedirs(os.path.join(d, "client1"), exist_ok=True)
        with open(os.path.join(d, "client1", "mh_net.log"), "w", encoding="utf-8") as f:
            f.write("; nothing promoted here\n")
            for nm in ("scratch_reset", "enqueue", "pending_enqueue"):
                f.write("; [promote] %s: call #1 (OURS is live)\n" % nm)
        check(
            "a client with NO RUN-CONFIG still passes the asymmetric shape though mh::orders logged "
            "liveness lines",
            det_run_report(d, {"host": True, "client1": False})[0],
        )
        # ...and the OTHER direction is still armed: a peer whose seams installed but were never
        # called must still fail, which is the case RUN-CONFIG alone cannot see.
        with open(os.path.join(d, "client1", "mh_net.log"), "w", encoding="utf-8") as f:
            f.write("; [promote] lockstep: RUN-CONFIG: SHIP (whole closure)\n")  # no liveness lines
        check(
            "a peer promoted but never CALLED still fails -- RUN-CONFIG alone is not enough",
            not det_run_report(d, {"host": True, "client1": True})[0],
        )
        # A DIAGNOSTIC subset is not a ship config and must not read as "ours is running".
        with open(os.path.join(d, "client1", "mh_net.log"), "w", encoding="utf-8") as f:
            f.write("; [promote] lockstep: RUN-CONFIG: DIAGNOSTIC (subset 3/33 seams)\n")
            f.write("; [promote] pump: call #1 (OURS is live)\n")
        check(
            "a DIAGNOSTIC subset does not satisfy a peer that is supposed to run OURS",
            not det_run_report(d, {"host": True, "client1": True})[0],
        )

        # FOURTH RULE, added 2026-07-29 after it cost a C6 acceptance run: composing the host ini from
        # two fragments that share a SECTION must not produce a duplicate block. `--extra-ini` and
        # `--extra-ini-host` used to be concatenated, so two fragments each carrying `[promote]` gave
        # the host two such sections and GetPrivateProfile* read only the first -- the host-only key
        # was in the file and unreachable, and the run passed with its asymmetry dead. (The example
        # below uses `[net]`: `[promote]` is retired at F2E and the drop gate forbids emitting it,
        # while the merge rule this tests is a property of the reader, not of any one section.)
        # Same family as the other three: silent when working, and invisible in the verdict.
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import ui_test as _u

        both = _u.ini_merge_fragment("[net]\nrx_spin=1\n", "[net]\nqpc_clock=1\n")
        check(
            "two fragments sharing a section merge into ONE readable block",
            _u.ini_effective(both, "net", "qpc_clock") == "1"
            and _u.ini_effective(both, "net", "rx_spin") == "1",
        )
        check(
            "...and the reader model REFUSES to see a key in a duplicate later section",
            _u.ini_effective("[net]\nrx_spin=1\n\n[net]\nqpc_clock=1\n", "net", "qpc_clock")
            is None,
        )

        # FIFTH RULE, added 2026-07-29 while wiring P0-SPDET: extra [harness] lines must override the
        # template BY KEY, not be appended after it. GetPrivateProfileInt returns the FIRST occurrence
        # in a section, so an appended `fixed_step=0` behind the template's `fixed_step=1` reads as 1
        # and the knob silently does nothing -- a run that looks armed and compares garbage. The
        # per-KEY twin of the duplicate-[promote]-SECTION rule above.
        merged = _u.harness_apply_extras(
            "[harness]\nfixed_step=1\npin_fpu=1\n", "fixed_step=0\npin_wallclock=1\n"
        )
        mk = [ln.split("=")[0] for ln in merged.splitlines() if "=" in ln]
        check(
            "a [harness] extra OVERRIDES the template key rather than duplicating it",
            "fixed_step=0" in merged and mk.count("fixed_step") == 1,
        )
        check(
            "...and a key the template lacks is still appended",
            "pin_wallclock=1" in merged,
        )

        # SIXTH RULE, added 2026-07-30 with C8-d and WIDENED at fork F2E: no committed ini fragment
        # may carry a RETIRED SECTION. It started as three retired KEYS inside `[promote]` (`wire` /
        # `wire_seams` went away when the sixteen emitters joined the `lockstep` closure), on the
        # reasoning that the DLL refuses such a run loudly but a refusal only helps whoever reads the
        # log -- the failure it guards is a stale fragment sitting in the tree for months and picked
        # up by a run whose author reads the NAME of the file and not its contents.
        #
        # F2E retired the SECTIONS themselves, so the check is the same shape one level up: any
        # `[promote]`/`[promote_skip]`/`[state_handler_skip]`/`[rebind]` header with keys under it.
        # This is the SAME population the F2 drop gate scans (tools/check_fork_f2_drop.py assertion
        # 3), asked from the side that can also see whether a fragment is REACHABLE -- kept in both
        # places deliberately: the drop gate proves the vocabulary stays dead in the tree, and this
        # proves the rig's own committed corpus stays runnable.
        #
        # WIDENED AGAIN AT F2G with the three sections D12 retired: `[pacing]` (fps_cap moved into
        # [video]), `[test]` (lane moved into [uitest]) and `[probe]` (the LT1C cell-grid diagnostic
        # went away with its stub). Same population as check_fork_f2_drop, same reason as F2E's four.
        _retired_sections = (
            "promote",
            "promote_skip",
            "state_handler_skip",
            "rebind",
            "pacing",
            "test",
            "probe",
        )
        _ini_dir = os.path.join(REPO, "tools", "uiscripts", "ini")
        _offenders = []
        for _fn in sorted(os.listdir(_ini_dir)):
            if not _fn.endswith(".ini"):
                continue
            _sec = None
            for _ln in open(os.path.join(_ini_dir, _fn), encoding="utf-8"):
                _s = _ln.split(";", 1)[0].strip()
                if _s.startswith("[") and _s.endswith("]"):
                    _sec = _s[1:-1].strip().lower()
                elif _sec in _retired_sections and "=" in _s:
                    _offenders.append("%s:[%s] %s" % (_fn, _sec, _s))
        check(
            "no committed ini fragment carries a RETIRED section (F2E+F2G): "
            + (", ".join(_offenders) if _offenders else "none"),
            not _offenders,
        )

        # ---- SEVENTH RULE (fork F2G, 2026-09-13): AN INI KEY BELONGS TO ITS SECTION --------------
        #
        # D12 merged mh_harness.ini into mh_net.ini and made `[harness] enable=1` the harness's
        # arming signal. Both halves land on ini_merge_section, whose "the target already sets this
        # key" test was scoped to the WHOLE TEXT rather than to the target section -- so merging
        # `enable=1` into `[harness]` was silently dropped, because every lane ini opens with
        # `[net] enable=1`. That is the worst available shape: nothing is reported, the file looks
        # right to anyone not modelling GetPrivateProfile*, and the run comes back with the
        # instrument it was supposed to arm never installed (no mh_harness.log at all, which reads
        # as a rig fault rather than a config bug).
        #
        # Both directions, because the fix must not become "never skip": a key the TARGET SECTION
        # already sets still wins, which is what makes an --extra-ini fragment the more specific one.
        _cross = _u.ini_merge_fragment(
            "[net]\nenable=1\nport=6501\n", "[harness]\nenable=1\nstop_step=5\n"
        )
        check(
            "a merged [harness] enable=1 survives a file whose [net] already sets `enable`",
            _u.ini_effective(_cross, "harness", "enable") == "1"
            and _u.ini_effective(_cross, "net", "enable") == "1",
        )
        _same = _u.ini_merge_section(
            "[harness]\nstop_step=9\n", "harness", ["stop_step=1", "enable=1"]
        )
        check(
            "...and a key the TARGET SECTION already sets is still not duplicated",
            _u.ini_effective(_same, "harness", "stop_step") == "9"
            and _u.ini_effective(_same, "harness", "enable") == "1"
            and _same.count("stop_step") == 1,
        )

        # ---- THE ARMING SIGNAL ITSELF (fork F2G): the composed lane ini says what it arms ---------
        #
        # The runner's two launch paths compose one ini now, so the question "does this run arm the
        # harness" is answerable without a rig -- and it must be asked through the READER, not by
        # searching the text, for the reason the rule above exists. A harness-less scenario that
        # grew a [harness] block would stop the sim at somebody else's stop_step mid-walk; a
        # determinism arm that lost `enable=1` would produce no hashes at all.
        _armed = _u.make_ini("walk.txt", 1500, harness_steps=800, is_host=True, ident={"lane": 3})
        _plain = _u.make_ini("walk.txt", 1500, harness_steps=0, is_host=False, ident={"lane": 3})
        check(
            "a determinism arm's lane ini reads back [harness] enable=1 and its stop_step",
            _u.ini_effective(_armed, "harness", "enable") == "1"
            and _u.ini_effective(_armed, "harness", "stop_step") == "800",
        )
        check(
            "a plain UI run's lane ini carries NO [harness] section at all",
            "[harness]" not in _plain and _u.ini_effective(_plain, "harness", "enable") is None,
        )
        check(
            "...and the lane number reads back out of [uitest], not the retired [test]",
            _u.ini_effective(_plain, "uitest", "lane") == "3"
            and _u.ini_effective(_plain, "uitest", "script") == "walk.txt",
        )

        # ---- EIGHTH RULE (fork F2C, 2026-09-12): THE PROMOTION ORACLES' ARMS ARE THE SELECTOR ----
        #
        # --sp-determinism, --ui-equiv, --ui-abc and migration_ab all build their control arm from
        # ONE thing now: `[config] mode` (src/mh_dll/mh/config/config.h). Three properties, and none
        # of them can be read off a green rig run, because every one of them fails as a GREEN:
        #
        #   (a) the fragment this tool WRITES carries no `[promote]`/`[rebind]` key. F2E deletes that
        #       vocabulary from the DLL; a writer still emitting it would go on producing a file the
        #       DLL ignores, and an ignored rollback arm is a promoted arm wearing the control's
        #       name -- exactly the C8-f failure sp_arm_report was built to catch, one layer earlier.
        #   (b) it is READABLE where it lands: merged into a lane ini that already has [net],
        #       [capture] and [uitest] blocks, `mode` must still be what GetPrivateProfile* returns.
        #       A second `[config]` section would be present and unreachable -- the trap the first
        #       four rules above cover for [promote]/[video]/[harness], here for the key that now
        #       decides the whole configuration.
        #   (c) the two arms differ. sp_mode_refusal is the pre-rig half of sp_arm_report's "both
        #       arms are promoted, so the comparison has nothing to say", and a refusal that has
        #       never been watched to fire is not a gate -- so both directions are exercised.
        _frag = os.path.join(d, "all_original.ini")
        _mode = write_all_original_ini(_frag)
        _txt = open(_frag, encoding="utf-8").read()
        check("the written all-original arm selects [config] mode=original", _mode == MODE_ORIGINAL)
        _bad_sec = [
            "[%s] %s" % (s, ", ".join(ls))
            for s, ls in _u.ini_split_sections(_txt)
            if ls and s.lower() in ("promote", "promote_skip", "rebind", "shadow")
        ]
        check(
            "...and carries NO [promote]/[rebind] key: " + ("; ".join(_bad_sec) or "none"),
            not _bad_sec,
        )
        _lane = _u.ini_merge_fragment(
            "[net]\nport=6501\ngame_speed_pct=0\n\n[capture]\nevery=0\n\n[uitest]\nenable=1\n", _txt
        )
        check(
            "...and survives the lane merge READABLE (no shadowed second [config] block)",
            ini_selected_mode(_lane) == MODE_ORIGINAL,
        )
        check(
            "an unreachable duplicate [config] reads as absent, the way the game reads it",
            ini_selected_mode("[config]\n\n[config]\nmode=original\n") is None,
        )
        # The two committed arms of the gate's own oracles, checked as a PAIR.
        check(
            "the committed rollback arm selects mode=original (got %s)"
            % ini_file_mode(SP_ROLLBACK_INI),
            ini_file_mode(SP_ROLLBACK_INI) == MODE_ORIGINAL,
        )
        check(
            "the committed promoted arm selects mode=brokered (got %s)"
            % ini_file_mode(SP_PROMOTE_INI),
            ini_file_mode(SP_PROMOTE_INI) == MODE_BROKERED,
        )
        check(
            "sp_mode_refusal PASSES the real pair",
            sp_mode_refusal(
                ini_file_mode(SP_ROLLBACK_INI),
                ini_file_mode(SP_PROMOTE_INI),
                "baseline",
                "promoted",
            )[0]
            is None,
        )
        check(
            "sp_mode_refusal REFUSES two arms on the same mode (the negative case, run not argued)",
            "same configuration"
            in (sp_mode_refusal(MODE_BROKERED, MODE_BROKERED, "a", "b")[0] or ""),
        )
        check(
            "...in the original direction too",
            sp_mode_refusal(MODE_ORIGINAL, MODE_ORIGINAL, "a", "b")[0] is not None,
        )
        check(
            "sp_mode_refusal REFUSES an arm that names no mode at all",
            sp_mode_refusal(None, MODE_ORIGINAL, "a", "b")[0] is not None,
        )

        # SEVENTH RULE, added 2026-08-01 with AI0's game-speed pin. Same family again: two arms run
        # at different game speeds compare fine mechanically and mean nothing, because a step's
        # game-time size scales with game_speed_pct. The refusal lives in run_sp_determinism, which
        # needs a rig; what can be checked here for free is the READER it depends on -- if
        # sp_arm_game_speed cannot see the DLL's line, the refusal never fires and every cross-speed
        # run is silently compared.
        os.makedirs(os.path.join(d, "spd"), exist_ok=True)
        with open(os.path.join(d, "spd", "mh_net.log"), "w", encoding="utf-8") as f:
            f.write("; some other line\n; game_speed: 1000% (pinned via [net] game_speed_pct)\n")
        check(
            "the game-speed reader parses the DLL's `; game_speed: N%` line",
            sp_arm_game_speed(os.path.join(d, "spd")) == 1000,
        )
        with open(os.path.join(d, "spd", "mh_net.log"), "w", encoding="utf-8") as f:
            f.write("; a build that predates the line\n")
        check(
            "...and reports UNKNOWN rather than assuming 100% when the line is absent, so an old "
            "build cannot pass the refusal by looking like a default",
            sp_arm_game_speed(os.path.join(d, "spd")) is None,
        )
        # THE REFUSAL ITSELF. It lives here rather than being mutation-checked on a live run for a
        # reason worth knowing: the obvious live mutation -- put `game_speed_pct=200` in the promoted
        # arm's --extra-ini fragment -- CANNOT WORK. A fragment is appended as a whole extra section,
        # so the lane's mh_net.ini gets a SECOND `[net]` block and GetPrivateProfile* reads only the
        # first, which is the template's own `game_speed_pct=0`. Measured 2026-08-01: the "mutated"
        # run reported both arms at 100% and passed, which reads exactly like a broken check. Same
        # first-match-wins trap as the fourth rule above, between the TEMPLATE and a fragment.
        r_diff, _ = sp_speed_refusal({"a": 100, "b": 1000}, "a", "b")
        check("two arms at DIFFERENT speeds are REFUSED", bool(r_diff) and "1000%" in r_diff)
        r_same, n_same = sp_speed_refusal({"a": 1000, "b": 1000}, "a", "b")
        check(
            "...and two arms at the SAME speed are not (the refusal is not just always-on)",
            r_same is None and "1000%" in n_same,
        )
        r_unk, n_unk = sp_speed_refusal({"a": None, "b": None}, "a", "b")
        check(
            "both arms UNKNOWN is a note, not a refusal -- every historical run is in that state",
            r_unk is None and "could not run" in n_unk,
        )
        r_half, _ = sp_speed_refusal({"a": None, "b": 100}, "a", "b")
        check(
            "...but ONE arm unknown IS refused: an unmeasured arm cannot be assumed to match",
            bool(r_half) and "UNKNOWN" in r_half,
        )

        # ---- P0-SPDET: the single-player comparator's own negative tests ------------------------
        # Same family as the four above -- every one of these is a way for the SP oracle to look
        # green while comparing nothing. They run here rather than in a separate test file because
        # lint_repo already runs this, and an oracle's falsification tests are worth nothing if they
        # are not on a path somebody actually executes.
        import mp_analyze as _m

        nreg = len(_m.REGION_NAMES)
        ridx = {nm: i for i, nm in enumerate(_m.REGION_NAMES)}

        def sp_log(path, steps, poke=None, regions=True):
            with open(path, "w", encoding="utf-8") as f:
                f.write("; ==== mh replay harness armed\n")
                for s in range(1, steps + 1):
                    f.write("%d %016X %016X %016X\n" % (s, s, 0xC0DE + s, 0x5747 + s))
                    if regions:
                        cols = ["%016X" % (1000 + i) for i in range(nreg)]
                        if poke is not None:
                            cols[ridx[poke]] = "%016X" % (9999 + s)
                        f.write("R %d %s\n" % (s, " ".join(cols)))

        pa, pb = os.path.join(d, "sp_a.log"), os.path.join(d, "sp_b.log")
        sp_log(pa, 50)
        sp_log(pb, 50)
        base = _m.sp_compare(_m.parse_harness(pa), _m.parse_harness(pb))
        check("SP: two identical runs compare IDENTICAL", base["ok"])
        check(
            "...having actually compared every channel on every step",
            all(c["compared"] == 50 for c in base["channels"].values()),
        )
        # THE ONE THAT MATTERS: each time_tick output region must be caught by ITS OWN channel while
        # the state-only hash stays clean. If these ever pass, the oracle is blind to time_tick again
        # and C8's whole single-player risk is back to being unwatched.
        for nm in _m.SP_TIME_REGIONS:
            sp_log(pb, 50, poke=nm)
            r = _m.sp_compare(_m.parse_harness(pa), _m.parse_harness(pb))
            check(
                "SP: a divergence in %-13s is caught (state-only hash sees NOTHING)" % nm,
                not r["ok"]
                and r["channels"][nm]["mismatch_count"] == 50
                and r["channels"]["state"]["mismatch_count"] == 0,
            )
        sp_log(pb, 50, regions=False)
        r = _m.sp_compare(_m.parse_harness(pa), _m.parse_harness(pb))
        check(
            "SP: a run with no per-region columns FAILS (it did not compare them, it skipped them)",
            not r["ok"] and set(r["uncompared_regions"]) == set(_m.SP_TIME_REGIONS),
        )
        sp_log(pb, 0)
        r = _m.sp_compare(_m.parse_harness(pa), _m.parse_harness(pb))
        check("SP: zero overlapping steps FAILS rather than passing vacuously", not r["ok"])

        # G35: the display-fit preflight. Here rather than in a new selftest because it is the same
        # class as everything above -- a run that does not mean what it appears to mean. A pinned
        # view larger than the desktop does not error; it produces one [promote] time_tick call and
        # then a script that waits out its timeout, which reads as a sim hang and once cost a whole
        # git bisect. Mutation direction matters: the check must go RED on the too-small case AND
        # stay green on the fitting one, so both are asserted.
        import ui_test as _u

        v1024 = _u.pinned_view_size(
            open(os.path.join(REPO, "tools/uiscripts/ini/video_1024.ini"), encoding="utf-8").read()
        )
        check("G35: video_1024.ini is read as a 1024x768 pin", v1024 == (1024, 768))
        check(
            "G35: a custom [video] width/height OVERRIDES size_mode (video.cpp forces mode 2)",
            _u.pinned_view_size("[video]\nsize_mode=0\nwidth=1280\nheight=800\n") == (1280, 800),
        )
        check(
            "G35: an ini that pins no view returns None",
            _u.pinned_view_size("[net]\nx=1\n") is None,
        )
        check(
            "G35: 1024x768 pinned on the 958x945 RDP desktop is REFUSED (the real 2026-08-20 case)",
            _u.view_exceeds_desktop((1024, 768), (958, 945)),
        )
        check(
            "G35: ...and a desktop short only in HEIGHT is refused too",
            _u.view_exceeds_desktop((1024, 768), (1920, 720)),
        )
        check(
            "G35: the same pin on 1920x1080 is allowed (the check can go green, not just red)",
            not _u.view_exceeds_desktop((1024, 768), (1920, 1080)),
        )
        check(
            "G35: an exact fit is allowed",
            not _u.view_exceeds_desktop((1024, 768), (1024, 768)),
        )
        check(
            "G35: unknown desktop size never blocks a run (guard degrades open)",
            not _u.view_exceeds_desktop((1024, 768), None),
        )

        # ---- U28 3-peer barrier: the vacuity guard's negative cases -------------------------------
        # These exist because the FIRST green 3-peer run was half-vacuous and looked perfect: one
        # client's poke had been repainted by a host lobby snapshot before Start, so only the other
        # actually exercised the barrier. A guard against that is worthless unless it can fail, and
        # this is where that is checked without a rig.
        b = os.path.join(d, "b3")

        def barrier_dir(c1, c2):
            """Build a fake determinism dir; each arg is that client's mh_launch.log body."""
            shutil.rmtree(b, ignore_errors=True)
            for name, body in (("client1", c1), ("client2", c2)):
                os.makedirs(os.path.join(b, name), exist_ok=True)
                with open(os.path.join(b, name, "mh_launch.log"), "w", encoding="utf-8") as f:
                    f.write(body)
            return b

        def diff_line(slot, offs="05"):
            return (
                "; U28 SLOT DIFF at Start slot[%d]: ours status=1 pid=%d race=2 color=%d | HOST "
                "status=1 pid=%d race=1 color=%d -- taking the host's; bytes differing: %s\n"
                % (slot, slot, slot, slot, slot, offs)
            )

        ADOPT = "; U28: adopted the host's authoritative lobby slots at Start (occ=3)\n"
        good1, good2 = diff_line(1) + ADOPT, diff_line(2) + ADOPT
        check(
            "U28-3P: both clients disagreed at their own slot on byte 05 -> PASS (guard goes green)",
            det3_barrier_report(barrier_dir(good1, good2))[0],
        )
        check(
            "U28-3P: a client that logged NO disagreement FAILS (the real 2026-08-29 half-vacuous run)",
            not det3_barrier_report(barrier_dir(good1, ADOPT))[0],
        )
        check(
            "U28-3P: ...and the failure says the poke never survived to Start",
            "never survived to Start"
            in " ".join(det3_barrier_report(barrier_dir(good1, ADOPT))[1]),
        )
        check(
            "U28-3P: a client disagreeing on the WRONG slot FAILS (slot 2 must report slot[2])",
            not det3_barrier_report(barrier_dir(good1, diff_line(1) + ADOPT))[0],
        )
        check(
            "U28-3P: EXTRA differing bytes FAIL (relation-row exclusion regressed / new drift)",
            not det3_barrier_report(barrier_dir(good1, diff_line(2, "05 0d") + ADOPT))[0],
        )
        check(
            "U28-3P: a disagreement with NO adopt FAILS (it kept its own slots)",
            not det3_barrier_report(barrier_dir(good1, diff_line(2)))[0],
        )
        check(
            "U28-3P: a missing mh_launch.log FAILS rather than passing on absent evidence",
            not det3_barrier_report(os.path.join(d, "nope"))[0],
        )
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print("det_standard_selftest:", "PASS" if not fails else "FAIL (%d)" % len(fails))
    return 0 if not fails else 1


# ---- U28: the 3-PEER START-BARRIER determinism shape ---------------------------------------------
# WHY IT IS A SHAPE AND NOT A CAPTURE TEST: it produces a determinism verdict, not pixels, so it has
# no baseline and belongs beside the promotion shapes rather than in TESTS.
#
# WHAT IT PROVES THAT THE 2-PEER SHAPES CANNOT. The Start barrier (U28) makes FLAG_START carry the
# host's authoritative 8-slot array so every peer builds Players[] from ONE copy. With two peers that
# is only ever exercised at slot 1, and "does it cover slot 1, or every occupied human slot?" is the
# question a 2-peer run is structurally unable to answer. Here BOTH clients poke their OWN slot to
# Alien locally with no 0x0c push, so at Start the host still has Human for both -- and the negative
# arm (`[net] start_slots=0`, the legacy bare signal) brings p3_ai_gates, the THIRD peer's own region,
# into the divergence.
#
# TOPOLOGY: host on vms[0], client1 on vms[1], client2 as a LOCAL LANE on this box. No third machine.
# Three requirements, each of which fails silently if you omit it (all three cost a run on 2026-08-29):
#   * `peers=2` -- NET_BLOCK has no `[net] peers`, so the host would log "(expect 1 peers)" and the
#     third peer has nowhere to go.
#   * the lane on the HOST'S GAME PORT -- peers of one match share `[net] port`, so a lane on its own
#     port dials a host that is not there. Hence DET3_PORT, not LOCAL_PORT_BASE + n.
#   * --timeout-frames -- a headless lane runs at thousands of fps, so ui_test's default 1500-frame
#     per-step watchdog expires ~1.1 s in, before the menu exists ("TIMEOUT at step 0 after 1501
#     frames (1.125s) -- ABORT: settled value:110").
# The lane is provisioned HEADLESS, unlike --det-local's deliberately blitted pair. The pacing reason
# that motivates --det-local does not bite here: the VM peers were measured at ~3115 fps WITH the blit
# on this rig, and lockstep paces the sim regardless of frame cadence.
DET3_LANE = "det3_client2"
DET3_LANE_NO = lane_alloc.lane("det3", 0)  # allocated, fork F4H -- see tools/lane_alloc.py
DET3_PORT = 6501  # the host's game port -- peers of ONE match must share it (see above)
DET3_HOST_SCRIPT = "mp_host_inflight3.txt"
DET3_CLIENT_SCRIPTS = ("mp_client_inflight3a.txt", "mp_client_inflight3.txt")
# Which lobby slot each client pokes, and therefore which slot MUST show up in its SLOT DIFF line.
DET3_EXPECT_SLOT = {"client1": 1, "client2": 2}


def det3_barrier_report(det_dir):
    """Prove the 3-peer run actually EXERCISED the barrier. Returns (ok, lines).

    A green hash here is worth nothing on its own, and that is not hypothetical -- it is what the
    first green 3-peer run looked like. Only ONE of the two clients had disagreed with the host,
    because the other's poke had been repainted by the host's periodic 0x09 lobby snapshot before
    Start. The run passed; it had tested the barrier once instead of twice. So: every client must
    have logged its OWN slot's disagreement, or this shape FAILS however clean the hashes are.

    The differing-byte list is checked EXACTLY, not merely for being non-empty. `pokerace` writes one
    field -- race at +0x05 -- so anything else appearing means either the relation-row exclusion has
    started over-reporting again or some other field has drifted between peers at Start. Both are
    findings, and neither should be able to hide behind "well, it differed".
    """
    lines, ok = [], True
    for peer, slot in sorted(DET3_EXPECT_SLOT.items()):
        path = os.path.join(det_dir, peer, "mh_launch.log")
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError as e:
            ok = False
            lines.append("      FAIL: %s -- cannot read mh_launch.log (%s)" % (peer, e))
            continue
        diff = [ln for ln in text.splitlines() if "U28 SLOT DIFF at Start slot[%d]" % slot in ln]
        adopted = [ln for ln in text.splitlines() if "adopted the host's authoritative" in ln]
        if not diff:
            ok = False
            lines.append(
                "      FAIL: %s logged NO disagreement at slot[%d] -- its poke never survived to "
                "Start (a host lobby snapshot repaints it if the handshake slips), so this peer "
                "did not exercise the barrier and the green hash below is vacuous for it."
                % (peer, slot)
            )
            continue
        got = (
            diff[0].split("bytes differing:", 1)[-1].strip()
            if "bytes differing:" in diff[0]
            else ""
        )
        if got != "05":
            ok = False
            lines.append(
                "      FAIL: %s slot[%d] differs on bytes '%s', expected exactly '05' (race). "
                "pokerace writes ONE field, so anything else is a real change: check the "
                "relation-row exclusion and any newly-diverging slot field." % (peer, slot, got)
            )
        if not adopted:
            ok = False
            lines.append("      FAIL: %s never logged the adopt -- it kept its OWN slots" % peer)
        lines.append(
            "      %-8s slot[%d] disagreed on '%s' and adopted the host's" % (peer, slot, got)
        )
    return ok, lines


def run_det_3peer(args):
    """The U28 start-barrier shape. Returns (ok, lines) so run_det_standard can report it alongside."""
    det_dir = os.path.join(REPO, "tmp", "ui_test", "determinism")
    down = [ip for ip in args.vms[:2] if not vm_reachable(ip)]
    if down:
        return None, ["      SKIP -- VM(s) unreachable: %s" % ", ".join(down)]
    cmd = [
        sys.executable,
        os.path.join(REPO, "tools", "make_lane.py"),
        "--name",
        DET3_LANE,
        "--lane",
        str(DET3_LANE_NO),
        "--port",
        str(DET3_PORT),
        "--headless",
    ]
    if not STOCK_EXE:
        cmd.append("--patched-exe")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return False, [
            "      FAIL: lane %s: %s" % (DET3_LANE, (r.stderr or r.stdout).strip()[:200])
        ]
    argv = [
        "--determinism",
        "--steps",
        str(args.steps),
        "--host",
        "%s:%s" % (args.vms[0], DET3_HOST_SCRIPT),
        "--client",
        "%s:%s" % (args.vms[1], DET3_CLIENT_SCRIPTS[0]),
        "--client",
        "lane=%s:%s" % (DET3_LANE, DET3_CLIENT_SCRIPTS[1]),
        "--connect-ip",
        args.vms[0],
        "--timeout-frames",
        str(LOCAL_TIMEOUT_FRAMES),
        "--net-extra",
        # `peers=2` is this shape's own requirement and is NOT negotiable (see the header) -- the
        # caller's --net-extra is APPENDED to it, never substituted for it. D24 needs this: the only
        # way to make a resync actually fire on this rig is `resync_trigger_gate=0`, and without a
        # route for it the shape can only ever be run in the regime where the thing under test never
        # happens. Passing [net] keys through --extra-ini instead does NOT work and does not fail
        # either: they land in a SECOND [net] section that GetPrivateProfile* never reads, which is
        # how three of D17's six runs A/B'd the same config against itself.
        ";".join(["peers=2"] + ([args.net_extra] if args.net_extra else [])),
        "--timeout",
        str(max(args.timeout, 120 + args.steps)),
        "--extra-ini",
        PROMOTE_INI,
    ]
    # MP D24: opt-in order recording. Off by default -- this is a diagnostic for a run that went red,
    # not something every gate run should pay for. mh_orders.bin is in ui_test's OPTIONAL_ARTIFACTS,
    # so it is pulled back for all three peers and mp_order_diff can be run over any pair.
    if getattr(args, "record", 0):
        argv += ["--record", str(args.record)]
    det_clear(det_dir)
    rc = run_ui_test(argv, max(args.per_test_timeout, 180 + args.steps))[0]
    ok, lines = det_run_report(det_dir, {"host": True, "client1": True, "client2": True})
    bok, blines = det3_barrier_report(det_dir)
    return (rc == 0 and ok and bok), lines + blines


def det_clear(det_dir):
    """Empty the shared determinism artifact dir before a shape runs.

    THE SHAPES SHARE ONE DIRECTORY, so a shape whose run dies before writing anything is reported
    from the PREVIOUS shape's logs -- the exact vacuous green det_run_report exists to prevent, and
    it hid behind plausible-looking output for as long as every shape had the same peer set. It
    stopped hiding when a 3-peer shape joined the roster and left a client2/ behind: a later 2-peer
    run was reported with "compared steps per pair: [300, 300, 300]" and quoted a log line timestamped
    hours earlier (2026-08-29). Clearing first turns "this shape produced nothing" into an obvious
    absence instead of someone else's numbers.
    """
    shutil.rmtree(det_dir, ignore_errors=True)
    os.makedirs(det_dir, exist_ok=True)


# U32's map: the only 2-start map, and the scenario's landing arithmetic assumes exactly two spots.
CONQUEST_MAP = "Last Question.mpm"
# How far before the elimination a mismatch may appear and still count as "the tail". The best run
# diverged on exactly ONE step; this leaves room for the resolution cascade without letting a real
# mid-match desync through.
CONQUEST_TAIL_SLACK = 60


def run_det_conquest(args):
    """U32: drive a 2-peer match to a REAL END CONDITION and check it was reached lawfully.

    WHY THIS SHAPE EXISTS. Every other determinism shape compares a world that cannot END. Until
    2026-08-29 `llm_strat_player_presence_lost` had never once taken its elimination branch on this
    rig -- measured three ways -- so the whole endgame (U30's leave-lockstep detour, D20's gate
    label, U19's survivor clauses, D21's detector) rested on a path nothing exercised. This runs the
    `conq` harness workload: the peer's mothership is landed into a building, the host builds an
    academy and a barracks, spawns attackers, groups them and destroys that building.

    WHY IT IS NOT A `--determinism` SHAPE like the other three. Those drive the peers through the
    real menu, which picks the alphabetically-first map (Blue Monday); this scenario needs Last
    Question, the only 2-start map. mp_run's force-entry takes --map, so it is the runner until a
    Last Question host uiscript exists. That is the ONE thing standing between this and the UI path.

    WHY THE PASS CONDITION IS NOT "ALL PAIRS IDENTICAL". An elimination legitimately parts the peers:
    presence_lost clears the loser's ALIVE bit and downgrades SESSION 3->2 on ONE side, so from that
    instant they are in different session states BY DESIGN. mp_analyze compares every step and will
    therefore report DESYNC on a run that SUCCEEDED. The best observed run diverged on exactly ONE
    step of 1650 -- the elimination step itself. So the verdict is built from four things that are
    each individually checkable, and the step count is NOT one of them.
    """
    if not vm_reachable(args.vms[1]):
        return None, ["[det] conquest SKIP -- VM unreachable: %s" % args.vms[1]]
    steps = max(args.steps, 3000)
    budget = max(args.per_test_timeout, 300 + steps)
    cmd = [
        sys.executable,
        os.path.join(REPO, "tools", "mp_run.py"),
        "--steps",
        str(steps),
        # synth_move re-orders the SAME mothership every step and overwrites the workload's deploy;
        # the harness refuses the combination by name, so this is belt and braces.
        "--synth-move",
        "0",
        "--map",
        CONQUEST_MAP,
        "--harness-extra-host",
        "conq=1;conq_seed=1;conq_probe_every=0",
    ]
    print("[det] conquest: %s" % " ".join(cmd[2:]))
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=budget).stdout
    except subprocess.TimeoutExpired:
        return False, ["  conquest run TIMED OUT after %ds" % budget]

    runs = sorted(glob.glob(os.path.join(machine.POLYGON, "logs", "*_host")), key=os.path.getmtime)
    if not runs:
        return False, ["  conquest: no host log folder was produced"]
    harness = os.path.join(runs[-1], "mh_harness.log")
    net = os.path.join(runs[-1], "mh_net.log")
    htxt = open(harness, errors="replace").read() if os.path.isfile(harness) else ""
    ntxt = open(net, errors="replace").read() if os.path.isfile(net) else ""

    lines, ok = [], True
    # (1) THE MATCH ENDED.
    go = re.search(r"; GAMEOVER survivor=(\d+) units=(\d+) buildings=(\d+)", htxt)
    watch = re.search(r"; GAMEOVER-WATCH step=(\d+) alive=1 ", htxt)
    if go and watch:
        lines.append("  ended: %s (at step %s)" % (go.group(0).lstrip("; "), watch.group(1)))
    else:
        ok = False
        lines.append("  NO GAMEOVER -- the match did not resolve inside %d steps" % steps)
    # (2) IT ENDED BY ELIMINATION, not by running out of steps -- read from PLAYER STATE, and that
    #     change is the point (2026-09-05, SIM1-P clause 7/8).
    #
    #     THIS CHECK USED TO READ AN ENTRY-HOOKED DIAGNOSTIC and it went silently wrong the moment
    #     the thing it watched stopped being entered. `; presence_lost ... gate=eliminated` is
    #     written by net_diag.cpp's presence_lost_detour, a trampoline on llm_strat_player_presence_
    #     lost's ENTRY. Once `[rebind] llm_strat_player_presence_lost` armed by ship default
    #     (2026-09-04), our own bldg_state_destroyed began calling OUR body directly and never
    #     touching that entry -- so the line vanished and this check reported "nothing was actually
    #     eliminated" about a run in which the elimination plainly happened (`GAMEOVER survivor=0`,
    #     `on_gameover ENTER sess=2 outcome=5`, alive 2 -> 1). That is the G120 shape exactly: an
    #     entry-hooked channel used as a VERDICT signal, with no compensation, silenced by a
    #     promotion nobody thought to connect to it.
    #
    #     `; GAMEOVER-WATCH ... alive=N (was M)` is written by harness.cpp's gameover_check(), which
    #     POLLS every profile's status_flags for ENABLED|ALIVE and emits on every change. It reaches
    #     no entry, hooks nothing, and cannot be silenced by any promotion or rebind -- so a DROP in
    #     that count is the elimination, stated by the state itself. The detour line stays as a
    #     DIAGNOSTIC and is printed when present, because it carries the gate's own reasoning; it
    #     just no longer decides anything.
    drop = None
    for m in re.finditer(r"; GAMEOVER-WATCH step=(\d+) alive=(\d+) \(was (-?\d+)\)", htxt):
        now, was = int(m.group(2)), int(m.group(3))
        if was > now:  # an enabled+ALIVE slot lost its bit: someone was eliminated
            drop = (int(m.group(1)), was, now)
    if drop:
        lines.append(
            "  eliminated: alive %d -> %d at step %d (state-watched: profile ENABLED|ALIVE poll, "
            "not an entry hook)" % (drop[1], drop[2], drop[0])
        )
    else:
        ok = False
        lines.append(
            "  NO alive-count DROP in the GAMEOVER-WATCH trajectory -- nothing was eliminated"
        )
    elim = re.search(r"; presence_lost player=\d+ mode=\d+ gate=eliminated[^\r\n]*", ntxt)
    lines.append(
        "  " + elim.group(0).lstrip("; ")[:150]
        if elim
        else "  note: no 'gate=eliminated' detour line -- DIAGNOSTIC ONLY. Expected whenever "
        "`[rebind] llm_strat_player_presence_lost` is armed (the caller reaches OUR body and "
        "never the hooked entry); it is not evidence either way."
    )
    # (3) A TRIPPED GUARD IS A FAILED RUN however the match ended -- the workload's own phase
    #     watchdog is more specific than any check here can be.
    fail = re.search(r"; CONQ FAIL[^\r\n]*", htxt)
    if fail:
        ok = False
        lines.append("  " + fail.group(0).lstrip("; ")[:150])
    # (4) THE DIVERGENCE IS CONFINED TO THE TAIL. Anything well before the ending is a real desync
    #     and must fail, which is what keeps this from being "any ending will do".
    mm = re.search(r"total mismatching steps: (\d+) \(combined-hash: (\d+)\)", out)
    first = re.search(r"first mismatch: step (\d+)", out)
    if mm and watch:
        bad, total, endstep = int(mm.group(1)), int(mm.group(2)), int(watch.group(1))
        firststep = int(first.group(1)) if first else total
        if firststep < endstep - CONQUEST_TAIL_SLACK:
            ok = False
            lines.append(
                "  DESYNC BEFORE THE ENDING: first mismatch at step %d, %d steps before the "
                "elimination at %d -- a real divergence, not the tail"
                % (firststep, endstep - firststep, endstep)
            )
        else:
            lines.append(
                "  divergence confined to the tail: %d/%d steps, first at %d, ending at %d"
                % (bad, total, firststep, endstep)
            )
    elif mm is None:
        lines.append("  note: mp_run printed no mismatch summary (could not check the tail)")
    return ok, lines


def run_det_standard(args):
    """C7: run the standard determinism shapes and report them separately.

    Deliberately sequential and deliberately one report each. Collapsing them into a single verdict
    is what made "the determinism gate is green" mean less than it looked.

    THE ASYMMETRIC SHAPE WAS REMOVED FROM THIS LIST (user's call, 2026-09-01). It gated OURS against
    the ORIGINAL netcode, and the original netcode is DEAD: retail mh.exe has no socket layer at all
    -- its lobby builds and CRC-checks packets that are never transmitted ("dead at the wire",
    The lobby RE) -- and MH's working multiplayer IS the restored one, the injected DLL supplying
    the transport. The original lockstep path runs only because the byte patches animate it. So the
    shape answers a COMPATIBILITY question rather than a question about shipped behaviour, and it
    does not belong in the gate that guards what players run.

    IT IS REMOVED, NOT DELETED, and future compat testing is what it is kept for. It runs by hand:

        python tools/ui_test.py --determinism --steps 3000             --host <hostip>:mp_host_start.txt --client <clientip>:mp_client_start.txt             --connect-ip <hostip> --ship-pacing             --extra-ini-host  tools/uiscripts/ini/ship_config.ini             --extra-ini-client tools/uiscripts/ini/rollback_original.ini

    BOTH fragments are required. Brokered is the shipping default, so the host-only fragment selects
    what already holds and the run silently becomes symmetric -- see dead-ends G96, which is what that
    second fragment exists to prevent.
    """
    if not os.path.isfile(PROMOTE_INI):
        print("[det] cannot find %s -- nothing to promote" % PROMOTE_INI)
        return 2
    down = [ip for ip in args.vms[:2] if not vm_reachable(ip)]
    if down:
        print("[det] standard SKIP -- VM(s) unreachable: %s" % ", ".join(down))
        return 0

    shapes = [
        # (label, why, extra_ini (both peers), net_extra (both peers), extra_ini_host,
        #  extra_ini_client, promotion EXPECTED)
        #
        # The ASYMMETRIC shape used to lead this list. It was removed 2026-09-01 (user's call) --
        # see the docstring for what that costs and for the command that still runs it by hand.
        (
            "SYMMETRIC (ship config)",
            "promotion on BOTH peers, fixes at their shipping defaults -- what players run",
            PROMOTE_INI,
            None,
            None,
            None,
            {"host": True, "client1": True},
        ),
    ]
    det_dir = os.path.join(REPO, "tmp", "ui_test", "determinism")
    results = []
    for label, why, extra_ini, net_extra, extra_ini_host, extra_ini_client, want in shapes:
        print("\n" + "=" * 78)
        print("[det] SHAPE: %s\n      %s" % (label, why))
        print("=" * 78)
        argv = [
            "--determinism",
            "--steps",
            str(args.steps),
            "--host",
            "%s:%s" % (args.vms[0], "mp_host_start_ai.txt" if args.ai else "mp_host_start.txt"),
            "--client",
            "%s:mp_client_start.txt" % args.vms[1],
            "--connect-ip",
            args.vms[0],
            "--timeout",
            str(max(args.timeout, 120 + args.steps)),
        ]
        if extra_ini:
            argv += ["--extra-ini", extra_ini]
        if net_extra:
            argv += ["--net-extra", net_extra]
        if extra_ini_host:
            argv += ["--extra-ini-host", extra_ini_host]
        if extra_ini_client:
            argv += ["--extra-ini-client", extra_ini_client]
        if args.ship_pacing:
            argv.append("--ship-pacing")
        if args.ai:
            argv += ["--ai-probe", "100"]
        det_clear(det_dir)
        rc = run_ui_test(argv, max(args.per_test_timeout, 180 + args.steps))[0]
        ok, lines = det_run_report(det_dir, want)
        results.append((label, rc == 0 and ok, lines))

    # THIRD SHAPE: the U28 start barrier at three peers. It is here rather than in the loop above
    # because it is not a promotion shape -- different scripts, a third peer, and a pass condition
    # that includes "both clients actually disagreed" on top of the hash comparison.
    print("\n" + "=" * 78)
    print("[det] SHAPE: U28 3-PEER START BARRIER")
    print("      both clients disagree with the host at Start; the barrier must override BOTH")
    print("=" * 78)
    ok3, lines3 = run_det_3peer(args)
    if ok3 is None:
        print("\n".join(lines3))  # VM down -> SKIP, same convention as the shapes above
    else:
        results.append(("U28 3-PEER BARRIER", ok3, lines3))

    # FOURTH SHAPE: U32's conquest. Like the 3-peer barrier it is not a promotion shape -- it runs
    # through mp_run force-entry (it needs Last Question, the only 2-start map) and its pass
    # condition is "the match ended BY ELIMINATION and only the tail diverged", not "ALL PAIRS
    # IDENTICAL" -- an elimination legitimately parts the peers, so the usual verdict would fail
    # every successful run.
    print("\n" + "=" * 78)
    print("[det] SHAPE: U32 CONQUEST -- drive the match to a real END CONDITION")
    print("      the peer's mother is landed, then destroyed; the endgame path is the point")
    print("=" * 78)
    okc, linesc = run_det_conquest(args)
    if okc is None:
        print("\n".join(linesc))
    else:
        results.append(("U32 CONQUEST", okc, linesc))

    print("\n" + "=" * 78)
    print("[det] STANDARD RUNS -- one verdict each, on purpose")
    print("=" * 78)
    for label, ok, lines in results:
        print("  %-24s %s" % (label, "PASS" if ok else "FAIL"))
        for ln in lines:
            print(ln)
    return 0 if all(ok for _, ok, _ in results) else 1


def run_ui_test(argv, timeout, capture=False):
    """Invoke ui_test.py with argv (list). Returns (exit code, captured text).

    `capture` buffers the child's output instead of streaming it -- required under --jobs, where
    several tests run at once and live streams would interleave into an unreadable mess. The buffered
    text is printed as one block when the test finishes.
    """
    # --desktop is applied HERE, at the one choke point every caller goes through, rather than in each
    # argv builder -- there are five of them and a new one would silently miss it.
    #
    # It reaches the DETERMINISM runs too, where it is a NO-OP, and that is worth saying because
    # 5ac09a3 and 59f99ff both claimed the opposite: run_determinism builds its peers from args.vms
    # and launches nothing on this box, so there is no local window to isolate. Determinism was never
    # the local focus thief -- the capture suite was, running local lanes whose window is periodically
    # VISIBLE at x=0 between keeper interventions. ui_test.py holds the desktop lazily at its first
    # LOCAL launch, so a VM run does not even print the banner.
    if not STOCK_EXE and "--patched-exe" not in argv:
        # FORWARD THE OPT-OUT, not just the opt-in: ui_test.py re-derives its own default, which is
        # now stock, so omitting this would leave `test_ui.py --patched-exe` running stock peers.
        # Fourth instance of this shape in this file -- see the --no-desktop note just below.
        argv = [*argv, "--patched-exe"]
    if DESKTOP and "--desktop" not in argv:
        argv = [*argv, "--desktop", DESKTOP]
    elif NO_DESKTOP and "--no-desktop" not in argv:
        # FORWARD THE OPT-OUT TOO. Forwarding only the ON case leaves the child to re-apply its OWN
        # default, which is ON -- so `test_ui.py --no-desktop` was silently isolating anyway. Third
        # instance today of one shape of bug: a flag that computes the right answer and then fails to
        # transmit it (see also HEADLESS's missing `global`, and provision_lanes never passing
        # --visible). Whenever a setting has two homes, BOTH branches have to be sent.
        argv = [*argv, "--no-desktop"]
    cmd = [sys.executable, UI_TEST, *argv]
    banner = "    $ python tools/ui_test.py %s" % " ".join(argv)
    if not capture:
        print(banner)
        try:
            return subprocess.run(cmd, cwd=REPO, timeout=timeout).returncode, ""
        except subprocess.TimeoutExpired:
            print("    !! ui_test.py exceeded the wrapper timeout (%ds)" % timeout)
            return 2, ""
    try:
        r = subprocess.run(cmd, cwd=REPO, timeout=timeout, capture_output=True, text=True)
        return r.returncode, banner + "\n" + (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired as e:
        tail = (
            (e.stdout or b"").decode("utf-8", "replace")
            if isinstance(e.stdout, bytes)
            else (e.stdout or "")
        )
        return (
            2,
            banner
            + "\n"
            + tail
            + "\n    !! ui_test.py exceeded the wrapper timeout (%ds)" % timeout,
        )


# ---- LOCAL LANES: one folder per test, and one per (peer x test) --------------------------------
#
# `--local` runs the whole suite on THIS machine instead of the VMs, giving every peer of every test
# its own lane folder. That is worth more than the ssh it saves: a shared install makes a test's
# captures depend on the setup.dat / [video] mode the PREVIOUS test left behind (it then
# passes standalone and fails in the suite), and a folder per peer removes the inheritance entirely.
# A lane is ~3.5 MB, so ~18 of them cost less than one copy of Res\.
#
# NAMING: solo -> ui_<test>;  multi -> ui_<test>_host, ui_<test>_c1, ui_<test>_c2 ...
#
# LANE NUMBERS are unique across the WHOLE suite, not per test, because the number is what renames the
# single-instance mutex -- two lanes sharing a number could never run at the same time, which is
# exactly what concurrency needs.
#
# PORTS ARE PER TEST, NOT PER PEER. `[net] port` is both the host's listen port and the port a client
# DIALS, so peers of one match must share it; distinct ports separate concurrent MATCHES. Getting this
# backwards leaves the host on `peers 1` and the client on `sessions 1`, which reads like a discovery
# failure.
LOCAL_PORT_BASE = 6600
# A shim test is the ONE case where peers of a match do NOT share a port. The shim and the host lane
# are both on this box, so they cannot both own the game port -- and the shim binds first, so the GAME
# is what fails (`net: bind(:6600) failed 10013`), leaving the client stuck on `sessions 1` looking
# exactly like a discovery bug (H3, 2026-07-28). The client dials the shim's port instead, and the shim
# forwards to the host's. Its own band, well clear of LOCAL_PORT_BASE + len(TESTS).
LOCAL_SHIM_PORT_BASE = 6700
# --det-local's own range. The lane numbers come from lane_alloc (fork F4H); the PORT does not,
# because peers of one match must share one port. 6620 sits inside the capture suite's own band
# (PORT_BASE + test index), which is tolerable only because --det-local is a by-hand diagnostic that
# never runs beside the suite -- the gate's det unit uses the VMs. Move it if that ever changes.
DET_LOCAL_PORT = 6620
# provision_lanes numbers from the BASE (lane_base + 1 is the first peer), so this is the block's
# base rather than its first lane. Allocated at fork F4H -- see tools/lane_alloc.py.
DET_LOCAL_LANE_BASE = lane_alloc.block("det_local")[0]
# --local implies --headless, and headless removes the vsync wait -- so the per-step watchdog, which is
# budgeted in FRAMES, expires far sooner in wall-clock than it does with the blit enabled. 1500 (the
# default) does not even survive boot on this host. This is a floor; a test with its own timeout_frames
# still wins. Headless frames tick so fast they blow a FRAME-budget watchdog.
LOCAL_TIMEOUT_FRAMES = 80000

# D15: a test this close to its wall-clock budget is reported as a NEAR-MISS. 0.7 rather than 0.9
# because the whole point is lead time -- the 2026-08-06 failures were tests that had been sitting
# near their budget while the suite reported them as ordinary passes, so the warning has to fire
# while there is still headroom to act on.
WARN_FRAC = 0.7


def lane_names(test):
    """Every lane a test needs, in peer order (host first)."""
    if test["kind"] == "solo":
        return ["ui_" + test["name"]]
    return ["ui_%s_host" % test["name"]] + [
        "ui_%s_c%d" % (test["name"], i + 1) for i in range(len(test["clients"]))
    ]


def provision_lanes(tests, headless=True, port_base=None, lane_base=0, stock_exe=True):
    """Build a lane folder per (peer x test). Returns {test_name: (port, shim_port, [lane names])}.

    `port_base`/`lane_base` exist so a caller that is NOT the capture suite can carve out its own
    range: the lane number is the mutex separation and the port is the game port, so two runs sharing
    either would fight. --det-local uses them for exactly that reason.
    """
    # THE DEMAND IS CHECKED BEFORE ANYTHING IS PROVISIONED (fork F4H). This loop derives its lane
    # numbers from the registry's length, which is exactly how the capture suite grew 20 -> 36 lanes
    # and silently took over ui_soak's 32, --sp-determinism's 31 and the tactical lanes' 33..36 --
    # every one of those a machine-wide single-instance mutex, and every collision a game that dies
    # at boot without a log line. Overflowing the block is now a refusal with the two numbers in it.
    need = sum(len(lane_names(t)) for t in tests)
    owner = next((n for n, (b, _c) in lane_alloc.BLOCKS.items() if b == lane_base), None)
    if owner:
        cap = lane_alloc.block(owner)[1]
        if need > cap:
            print(
                "  lane allocation REFUSED: %d test(s) need %d lane(s) and the %r block holds %d "
                "(lanes %d..%d). Widen it in tools/lane_alloc.py -- taking the next block's numbers "
                "is what fork F4H had to undo."
                % (len(tests), need, owner, cap, lane_base + 1, lane_base + cap)
            )
            return None
    plan, lane_no = {}, lane_base
    port_base = LOCAL_PORT_BASE if port_base is None else port_base
    for ti, t in enumerate(tests):
        port = port_base + ti
        shim_port = (LOCAL_SHIM_PORT_BASE + ti) if t.get("shim") else 0
        names = lane_names(t)
        for i, nm in enumerate(names):
            lane_no += 1
            # host lane (i == 0) keeps the game port; a shim test's CLIENTS dial the shim's port
            lane_port = shim_port if (shim_port and i > 0) else port
            cmd = [
                sys.executable,
                os.path.join(REPO, "tools", "make_lane.py"),
                "--name",
                nm,
                "--lane",
                str(lane_no),
                "--port",
                str(lane_port),
            ]
            # PASS THE OPT-OUT, not just the opt-in. make_lane.py computes
            # `args.headless = not args.visible`, so OMITTING --headless does not produce a visible
            # lane -- it produces a headless one. Until 2026-08-02 this branch only ever appended
            # --headless, which means `test_ui.py --visible` has been provisioning HEADLESS lanes and
            # then applying the visible 1500-frame watchdog budget to a run ticking at thousands of
            # fps: instant TIMEOUT at step 0. Same shape as the HEADLESS-global bug in ui_test.py --
            # a flag that computes the right answer and then fails to transmit it.
            cmd.append("--headless" if headless else "--visible")
            # Run the suite against a byte-for-byte RETAIL exe, with
            # mh.dll force-loaded by the msvfw32 proxy shim instead of an added import.
            if not stock_exe:
                cmd.append("--patched-exe")
            # fork F4B: a scenario may ask for a lane BUILT WITHOUT a satellite DLL, which is how
            # `module_absent` makes mh_net.dll's absence REAL instead of simulating it with a key.
            # Per-test rather than global: every OTHER lane must carry the transport, or the nine MP
            # scenarios would silently be measuring the degraded configuration.
            for sat in t.get("omit_satellite", []) or []:
                cmd += ["--omit-satellite", sat]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print("  lane %s FAILED: %s" % (nm, (r.stderr or r.stdout).strip()[:200]))
                return None
        plan[t["name"]] = (port, shim_port, names)
        print(
            "  %-14s port %d%s  lanes: %s"
            % (t["name"], port, ("  shim %d" % shim_port) if shim_port else "", ", ".join(names))
        )
    return plan


def shim_argv(test, forward_to, shim_port=0):
    """A link-condition test runs the peers through tools/net_shim.py so the LAN can act like the
    internet (latency) and the link can be killed on cue. ui_test owns the shim's lifetime and
    repoints the clients at this box, so --connect-ip is overridden there. Shared by the VM and the
    --local branch: having this inline in only one of them is what left local `link_death` shimless.
    """
    if not test.get("shim"):
        return []
    argv = ["--shim", forward_to, "--shim-delay", str(test.get("shim_delay", 0))]
    if shim_port:  # local: the shim needs its OWN port, the host lane keeps the game port (H3)
        argv += ["--shim-listen-port", str(shim_port)]
    if test.get("shim_timeline"):
        argv += ["--shim-timeline", test["shim_timeline"]]
    return argv


def build_argv(test, vms, common, plan=None):
    # A test may opt into extra ini SECTIONS (a feature that ships off, e.g. the [debug] overlay). Every
    # other test then keeps rendering the shipping configuration, which is what its baseline captured.
    # str or list: a scenario can need more than one fragment (the U21 devchange_guard test pins
    # the video mode AND arms the probe), and the alternative -- one combined fragment -- would
    # duplicate the video pin that already has its own trap history.
    _ei = test.get("extra_ini")
    _ei = [_ei] if isinstance(_ei, str) else list(_ei or [])
    # EVERY MULTI-PEER TEST GETS THE FRAME-RATE CAP (2026-09-10, user). Measured on the run-3 gate:
    # multi-peer lanes spin the headless frame loop at 800-7000 fps while their actual pace is set
    # by the other peer (lobby handshakes) or the lockstep clock (wall ms) -- pure heat, and under
    # --jobs it is heat taken from the concurrent tests. The [video] fps_cap=60 fragment converts
    # the spin into Sleep; state predicates are speed-independent by the grammar's own rule, so
    # only the menu-walk seconds move. A RULE rather than nine registry stanzas so a new multi test
    # cannot forget it; `"fps_cap": False` on an entry opts out (e.g. a test measuring frame rate).
    # SOLO tests stay uncapped on purpose -- menus advance per present, so their speed is the point.
    if test.get("kind") == "multi" and test.get("fps_cap", True):
        _ei.append("tools/uiscripts/ini/fps60.ini")
    extra = [a for f in _ei for a in ("--extra-ini", f)]
    # Per-test tolerance override. MUST come after `common` (argparse keeps the last --tol). Needed when
    # the thing under test occupies a small fraction of the frame: the default 2% tol is ~2.5x the debug
    # overlay's whole footprint, so the overlay could vanish entirely and the capture would still "PASS"
    # (observed 2026-07-25 -- a run with a completely different overlay page passed against the baseline).
    # Tighten such a test rather than trusting a green that cannot see its own regression.
    tail = ["--tol", str(test["tol"])] if test.get("tol") is not None else []
    # Per-test wall-clock override, same last-wins trick as --tol. The suite default (200 s) assumes a
    # walk-and-capture test; a scenario that WAITS for something to happen on the wire needs its own
    # budget, and without this the runner kills the peers mid-experiment -- which then looks exactly
    # like the failure under test (an early abort looks like the thing it aborted on).
    if test.get("timeout"):
        tail += ["--timeout", str(test["timeout"])]
    # A test's OWN frame budget must win over the suite-wide one --local sets, so it goes in `tail`
    # (argparse keeps the last occurrence).
    if test.get("timeout_frames"):
        tail += ["--timeout-frames", str(test["timeout_frames"])]
    # A scenario the MENU cannot reach carries its own launch verb and the save that verb needs.
    # Both are inert for every other test, and both belong here rather than in the ini: a verb is a
    # command line, and the save has to be COPIED into the lane before the process starts.
    if test.get("launch_args"):
        tail += ["--launch-args", test["launch_args"]]
    if test.get("deploy_save"):
        tail += ["--deploy-save", test["deploy_save"]]
    # [harness] knobs for a scenario that needs the harness to ARM something (not to run a step
    # budget). Goes to every peer -- the symmetric flag, never the host-only one, which is for
    # deliberately perturbing one side of a determinism pair.
    if test.get("harness_extra"):
        tail += ["--harness-extra", test["harness_extra"]]
    # `[net]` knobs, which CANNOT ride --extra-ini: ui_test builds the whole [net] block itself and
    # REFUSES a fragment carrying a second one (a duplicate section is unreachable, and Windows
    # GetPrivateProfile* would answer from the first). --net-extra writes into the base block, with
    # its own first-match-wins override guard. Fork F3F's no_net_boot is the first user ("module=none").
    if test.get("net_extra"):
        tail += ["--net-extra", test["net_extra"]]
    if plan is not None:  # --local: every peer of every test is its own lane on this machine
        port, shim_port, names = plan[test["name"]]
        if test["kind"] == "solo":
            # --port matters even with no client: ui_test's readiness gate polls host_listening(port),
            # and a LANE listens on its own per-test port, not on ui_test's 6501 default. Omitting it
            # here (as this branch did until 2026-07-28) left the gate watching a port nothing binds,
            # so the solo run could only clear it by COMPLETing inside the 50 s ready budget -- which a
            # 23-step walk does not -- and every local solo test aborted mid-walk with "[host] never
            # became ready". On the VM topology the bug is invisible: no lanes, every peer on 6501.
            return [
                "--host",
                "lane=%s:%s" % (names[0], test["script"]),
                "--port",
                str(port),
                *extra,
                *common,
                *tail,
            ]
        argv = [
            "--host",
            "lane=%s:%s" % (names[0], test["host"]),
            "--connect-ip",
            "127.0.0.1",
            "--port",
            str(port),  # peers of ONE match share the host's port; see LOCAL_PORT_BASE
            *extra,
        ]
        for i, cscript in enumerate(test["clients"]):
            argv += ["--client", "lane=%s:%s" % (names[1 + i], cscript)]
        if test.get("client_dead_ip"):
            argv += ["--client-dead-ip", test["client_dead_ip"]]
        # A link-condition test needs its shim LOCALLY too -- without it the link never dies, both
        # scripts wait for an event that cannot happen, and the test reports TIMED-OUT rather than a
        # verdict. This branch simply never wired it (2026-07-28): the flags existed only on the VM
        # path below. The shim forwards to the host on THIS box, so its target is 127.0.0.1.
        # The shim forwards to the host on THIS box, so its target is 127.0.0.1 -- at the HOST's port,
        # while it listens on its own (shim_port).
        argv += shim_argv(test, "127.0.0.1:%d" % port, shim_port)
        return argv + common + tail
    if test["kind"] == "solo":
        # single-peer (no-networking) walk -- run HOST-ONLY on the first VM (ui_test's multi path with no
        # --client), so it runs in the same environment as the multi tests and keeps the dev box free.
        return ["--host", "%s:%s" % (vms[0], test["script"]), *extra, *common, *tail]
    host_ip = vms[0]
    argv = ["--host", "%s:%s" % (host_ip, test["host"]), "--connect-ip", host_ip, *extra]
    for i, cscript in enumerate(test["clients"]):
        argv += ["--client", "%s:%s" % (vms[1 + i], cscript)]
    if test.get(
        "client_dead_ip"
    ):  # S8(b): pre-fill the client's IP field to a dead addr, live host in MRU
        argv += ["--client-dead-ip", test["client_dead_ip"]]
    argv += shim_argv(test, host_ip)
    return argv + common + tail


def required_vms(test, vms):
    if test["kind"] == "solo":
        return [vms[0]]  # host-only on the first VM
    return vms[: 1 + len(test["clients"])]


# ---- INSTALL-TIME REFUSALS: the shared cause behind a whole-suite failure ------------------------
#
# 2026-09-01 (dead-ends G99): the effects gate took llm_gfx_present_flip's entry, the present hook was
# refused it, and since capture / the UI automation driver / the overlay ALL piggyback on_present the
# whole harness went dark. What the suite printed was seventeen scenarios each burning its full budget
# on "did not present a frame" -- identical timeouts describing a UI regression that did not exist.
# The cause was one line in the run's OWN mh_net.log, written before the first script step, and
# nothing was reading it.
#
# The gate that caused G99 is gone (fork F2F dropped the deferred-effect machinery, so the present
# hook owns that entry outright and its `[effects] PARTIALLY ARMED` needle went with it). The
# present-hook needle STAYS: a promotion or a new detour can take the entry the same way, and the
# consequence for the suite is identical.
INSTALL_REFUSALS = (
    (
        "present hook NOT armed",
        "the present hook lost its entry -- capture, the UI automation driver and the overlay ALL "
        "piggyback on_present, so NO scenario in this suite can capture a frame",
    ),
)

# WHAT IS DELIBERATELY *NOT* A NEEDLE, because the first draft of this table got it wrong and the
# mistake is the interesting half. `[interlock] install_trampoline at ... ` looks like the ideal
# match -- it is the literal line that named the outage -- but a HEALTHY run carries one too:
# `install_trampoline at 0049D8EF DISPLACED`, the MP D14 resync detour whose target is promoted,
# which is expected and benign. Matching it would abort the whole suite on any ordinary
# single-scenario UI regression and blame an unrelated line for it -- a check whose candidate set
# is not its verdict, which is the shape this repo keeps having to unlearn. So the table matches
# only conditions that are install-WIDE and harness-FATAL: a lost present hook -- nothing can
# capture. (It had a sibling, `[effects] PARTIALLY ARMED`, until fork F2F deleted that layer.)
# Confirmed absent from a real healthy log before being trusted.


def install_refusal_lines(body):
    """[(why, line)] for every install-time refusal in one mh_net.log body. Empty when it is clean.

    Deliberately a pure text function: the rig half (finding the newest log per lane) is untestable
    without a rig, and this half is the half that decides. Mutation-tested by --selftest-refusals."""
    out = []
    for needle, why in INSTALL_REFUSALS:
        if needle in body:
            line = next((ln.strip() for ln in body.splitlines() if needle in ln), needle)
            out.append((why, line[:200]))
    return out


def selftest_refusals():
    """Prove the detector fires -- and, just as much, that it stays QUIET on a healthy log.

    The quiet half is the one that matters most here. This detector ABORTS the suite, so a false
    positive costs every remaining scenario and points the reader at the wrong line. The healthy
    fixture below is copied from a real passing run and deliberately includes the benign
    `install_trampoline at ... DISPLACED` line that the first draft of the table matched."""
    ok = True
    healthy = (
        "; [interlock] install_trampoline at 0049D8EF DISPLACED -- that entry is inside "
        "llm_net_lockstep_broadcast_resync_state, which is PROMOTED in this run\n"
        "; present hook armed (own detour): frametime_log=1 eager_advertise=1\n"
        "; [interlock] 1 detour install(s) REFUSED -- [the resync detour @0049D8EF]\n"
    )
    hits = install_refusal_lines(healthy)
    if hits:
        print("SELFTEST FAIL: a healthy log reported a refusal -- %r" % hits)
        ok = False
    for body, want in (("; present hook NOT armed -- see the [interlock] line\n", "present hook"),):
        hits = install_refusal_lines(body)
        if not hits:
            print("SELFTEST FAIL: missed the refusal in %r" % body[:60])
            ok = False
        elif want not in hits[0][0]:
            print("SELFTEST FAIL: wrong reason for %r -- %s" % (body[:40], hits[0][0]))
            ok = False
    print("[%s] test_ui install-refusal detector" % ("ok" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("only", nargs="*", help="run only these test names (default: all)")
    ap.add_argument(
        "--only", dest="only_flag", action="append", default=[], help="alias for a positional name"
    )
    ap.add_argument("--list", action="store_true", help="list the tests and exit")
    ap.add_argument(
        "--solo",
        action="store_true",
        help="run only the single-peer tests (host-only on the first VM)",
    )
    ap.add_argument(
        "--vms",
        nargs="+",
        default=list(machine.RIG_PEERS),
        help="VM IPs: [0]=host, [1..]=clients (default = machine_config.RIG_PEERS)",
    )
    ap.add_argument(
        "--update-baselines", action="store_true", help="regenerate baselines instead of diffing"
    )
    ap.add_argument(
        "--determinism",
        action="store_true",
        help="run the UI-PATH determinism check instead of the capture suite: drive both peers into the "
        "game via the real UI (match_launch scripts, no force-entry), run --steps in-game, mp_analyze -> "
        "ALL PAIRS IDENTICAL",
    )
    # 8000 => the --min-common floor below is 4000 (steps//2). RAISED FROM 800 on 2026-08-27, once
    # the harness flush bug was fixed and it became visible what the gate had been covering: 800
    # steps is 8 SECONDS of game time, while the run it gates plays for over two minutes. A cascading
    # desync -- the only kind that matters -- does not happen in the first eight seconds. 8000 steps
    # is 80 s of game time and costs the run nothing extra in wall clock, because the peers were
    # already alive that long waiting out the watchdog.
    ap.add_argument(
        "--steps", type=int, default=8000, help="--determinism: in-game steps to compare"
    )
    ap.add_argument(
        "--sp-determinism",
        action="store_true",
        help="P0-SPDET: the SINGLE-PLAYER oracle. Two sequential runs of one local lane -- unpromoted "
        "then promoted -- over the same scripted session, same synth seed, wall clock PINNED, compared "
        "on every channel including time_tick's own output regions. The shipped --determinism gate is "
        "2-peer MP and cannot see single-player paths, where C2 measured NINE promoted seams as LIVE.",
    )
    ap.add_argument(
        "--sp-selftest",
        action="store_true",
        help="--sp-determinism clause (1): run the SAME (unpromoted) build twice. If this is not "
        "identical the oracle is not deterministic and the promoted comparison means nothing.",
    )
    ap.add_argument(
        "--sp-perturb",
        default="",
        help="--sp-determinism clause (3): ';'-separated [harness] knobs applied to the SECOND arm "
        "only, to make it deliberately wrong (e.g. 'rng_perturb_slot=0;rng_perturb_step=200'). The "
        "PASS condition inverts -- a RED comparison is the expected result. A gate never watched to "
        "fail is not evidence.",
    )
    ap.add_argument(
        "--sp-seed",
        type=int,
        default=0,
        help="--sp-determinism: pin the synthetic-workload seed shared by both arms (0 = draw one).",
    )
    ap.add_argument(
        "--sp-game-speed",
        type=int,
        default=0,
        metavar="PCT",
        help="--sp-determinism: pin [net] game_speed_pct for BOTH arms (100 = normal, 1000 = 10x; "
        "0 = leave unpinned). Goldens are NOT portable across speeds -- a step at 1000%% carries ten "
        "times the game time -- so the speed is recorded in the run report and a comparison between "
        "two arms that ran at DIFFERENT speeds is refused rather than reported as a divergence.",
    )
    ap.add_argument(
        "--selftest-refusals",
        action="store_true",
        help="prove the install-time refusal detector fires (and stays quiet on a healthy log) "
        "without touching the rig; the lint gate runs this",
    )
    ap.add_argument(
        "--tact-determinism",
        action="store_true",
        help="TACT-PREP: the TACTICAL oracle. Two sequential runs of one local lane through the same "
        "--tactical entry, compared on the per-frame T/TR lines the llm_tact_frame cadence emits. "
        "Not a UI-script mode: the launch verb IS the driver, and the mission runs itself because "
        "tactical is all-AI as shipped.",
    )
    ap.add_argument(
        "--tact-relocate",
        action="store_true",
        help="SB-HOSTFREE: run the SECOND arm under `[harness] relocate_state=1`, so the mode's "
        "existing per-frame comparison becomes in-place vs RELOCATED. The relocating bind moves "
        "every movable region into a DLL arena and poisons the .bss it leaves, so a tactical "
        "consumer that did not follow the registry reads 0xCD. Refuses to pass if the arm moved no "
        "tactical hash slice.",
    )
    ap.add_argument(
        "--tact-relocate-corrupt",
        default="",
        metavar="REGION",
        help="--tact-relocate: THE MUTATION. Fill this region's ARENA copy with 0xCD, so a tactical "
        "consumer that correctly follows the bind reads garbage. The comparison MUST then go red -- "
        "a relocated arm that could not have diverged is not a pass. A run you discard.",
    )
    ap.add_argument(
        "--tact-relocate-nopoison",
        action="store_true",
        help="--tact-relocate: DIAGNOSTIC. Relocate without poisoning the abandoned .bss, which "
        "separates 'the relocation is broken' from 'something still reads the stock address' in one "
        "run. Never an acceptance: with poison off a stale reader gets a correct copy and agrees "
        "with itself.",
    )
    ap.add_argument(
        "--tact-selftest",
        action="store_true",
        help="--tact-determinism: add a THIRD arm that pokes one tactical region mid-run and require "
        "the compare to fail at exactly that frame, naming exactly that region. Without it a green "
        "cannot distinguish 'deterministic' from 'hashing bytes nothing writes'.",
    )
    ap.add_argument(
        "--tact-synth",
        action="store_true",
        help="--tact-determinism: arm the TACT-SYNTH synthetic player-command workload. A no-input "
        "tactical run exercises only the mission script and the FOV-engage AI; this drives the "
        "player-command paths (group orders, kneel/stand, mines, stance, facing) on a pinned seed "
        "and reports what it covered. The run must stay byte-identical WITH it armed.",
    )
    ap.add_argument(
        "--tact-synth-seed",
        type=int,
        default=0,
        metavar="N",
        help="--tact-synth: pin the workload seed (0 = draw one per invocation, shared by all arms).",
    )
    ap.add_argument(
        "--tact-synth-at",
        type=int,
        default=60,
        metavar="FRAME",
        help="--tact-synth: first ordering frame (default 60 -- after mission load settles).",
    )
    ap.add_argument(
        "--tact-synth-every",
        type=int,
        default=6,
        metavar="N",
        help="--tact-synth: re-issue cadence in tactical frames (default 6 -- the action rotation is 15 long, so this sets how many times each capability fires in a run).",
    )
    ap.add_argument(
        "--tact-synth-units",
        type=int,
        default=4,
        metavar="N",
        help="--tact-synth: how many player units the group arm selects (default 4).",
    )
    ap.add_argument(
        "--tact-synth-funnel-only",
        action="store_true",
        help="--tact-synth: drop the DIRECT arm, leaving only orders that go through "
        "llm_tact_unit_enqueue_command. Use it to MEASURE what the funnel alone cannot reach "
        "(weapon choice, the control-group slot, the sidebar def_stat cycle) rather than argue it.",
    )
    ap.add_argument(
        "--tact-jobs",
        type=int,
        default=3,
        metavar="N",
        help="TACT-REC: replay arms to run concurrently, one lane each (default 3 = all of them). "
        "1 keeps the single historical lane and runs them back to back.",
    )
    ap.add_argument(
        "--tact-slot",
        type=int,
        default=0,
        metavar="N",
        help="lane slot for --tact-equiv (default 0, the historical lane). The suite tail sets a "
        "distinct slot per journal so the four equivs can run concurrently without sharing a lane.",
    )
    ap.add_argument(
        "--tact-trim",
        metavar="JOURNAL",
        help="TACT-REC: write a trimmed copy of a journal keeping only clicks, keys and the cursor "
        "position just before each -- ~95%% of a recording is intermediate mouse motion. The trim "
        "is a HYPOTHESIS about what the sim depends on (the cursor also drives facing and camera "
        "scroll), so this verifies it with --tact-verify rather than trusting it.",
    )
    ap.add_argument(
        "--tact-trim-lead",
        type=int,
        default=1,
        metavar="N",
        help="--tact-trim: move events to keep before each action (default 1).",
    )
    ap.add_argument(
        "--tact-suite",
        action="store_true",
        help="TACT-REC: run every registered tactical journal scenario through both arms "
        "(semantic + hashes). ~20 min per scenario -- the expensive determinism tier, not the "
        "5-minute capture suite.",
    )
    ap.add_argument(
        "--tact-verify",
        metavar="JOURNAL",
        help="TACT-REC: the SEMANTIC arm. Replay a journal's INPUT only, withholding its recorded "
        "orders, and compare the orders the game emits by itself against the ones the human caused. "
        "Answers 'is this the same session', which the hash arms cannot.",
    )
    ap.add_argument(
        "--tact-equiv",
        metavar="JOURNAL",
        help="TACT-REC: replay a recorded journal TWICE -- ship config vs the whole DLL on original "
        "bodies -- and require the two to agree on the RNG draw series, the mission's end frame and "
        "casualties, and the order stream. The differential the absolute match count cannot be (the "
        "recording is not reproducible; see run_tact_equiv). This is the gate arm.",
    )
    ap.add_argument(
        "--tact-replay",
        metavar="JOURNAL",
        help="TACT-REC: replay a recorded order journal headless and verify it -- two replays "
        "compared, combat reported, and a shifted-order negative arm that proves the run depends "
        "on the journal at all.",
    )
    ap.add_argument(
        "--tact-record",
        action="store_true",
        help="--tact-play: journal the player's orders (both order seams + the three direct-write "
        "actions that have no opcode), log the WHOLE session rather than the first --tact-frames, "
        "and write a journal beside the archived session.",
    )
    ap.add_argument(
        "--tact-arm",
        action="store_true",
        help="TACT-RIG: run ONE tactical arm with --extra-ini fragment(s) merged into mh_net.ini. "
        "The migration loop's tactical shadow vehicle (migration_sweep.py --domain tact --mode "
        "tact); a shadow site compares inside one process, so it needs one arm, not a pair.",
    )
    ap.add_argument(
        "--tact-strict",
        action="store_true",
        help="--tact-determinism: fail on a FULL-hash divergence even when the SIM hash is "
        "identical. Default off: a full-only divergence is presentation drift, reported with its "
        "frame and regions but not counted against the simulation (the tactical-probe work 9k).",
    )
    ap.add_argument(
        "--tact-units-probe",
        metavar="LO-HI",
        help="--tact-determinism: arm the per-RECORD `TU` hash line (the range is only a switch "
        "here -- the DLL hashes all 129 records). Names the diverging UNIT.",
    )
    ap.add_argument(
        "--tact-detail",
        metavar="LO-HI",
        help="--tact-determinism: arm the 32-byte chunk `TD` lines for this unit range. Names the "
        "diverging BYTE RANGE inside a record.",
    )
    ap.add_argument(
        "--tact-fields",
        metavar="LO-HI",
        help="--tact-determinism: arm the per-FIELD `TF` lines for this unit range. Names the "
        "diverging FIELD -- the resolution --tact-detail cannot reach, because a 32-byte chunk "
        "straddles cmd_wait_until_time (which the sim slice keeps) and anim_frame_time (which it "
        "drops).",
    )
    ap.add_argument(
        "--tact-gates",
        metavar="LO-HI",
        help="--tact-determinism: arm the raw `TG` gate + animation + occupancy lines for this "
        "unit range. The occupancy column is the discriminator between a stuck anim_state and a "
        "lost tile_objects[col][row].building.",
    )
    ap.add_argument(
        "--tact-save",
        default="11",
        metavar="NAME",
        help="--tact-determinism: save to enter FROM (name WITHOUT .sav), staged into the lane.",
    )
    ap.add_argument(
        "--tact-play",
        action="store_true",
        help="launch ONE visible tactical mission and hand it to a human (the recording front end). "
        "Arms the VM mouse fix, no wall cap, no comparison; runs until you close the game.",
    )
    # ---- UI-REC: the game-start recorder ---------------------------------------------------------
    ap.add_argument(
        "--ui-play",
        action="store_true",
        help="launch ONE visible game at the MENU and hand it to a human (the game-start recording "
        "front end). Same shape as --tact-play, but it starts where --tact-play skips: the menu. "
        "Runs until you close the game.",
    )
    ap.add_argument(
        "--ui-record",
        action="store_true",
        help="--ui-play: journal the session's INPUT (mouse + keys + cursor), indexed on presents "
        "so the menu half is recorded too. Without this the session is played and not kept.",
    )
    ap.add_argument(
        "--ui-replay",
        metavar="JOURNAL",
        help="replay a game-start journal headless and report what it issued. The diagnostic arm.",
    )
    ap.add_argument(
        "--ui-equiv",
        metavar="JOURNAL|NAME",
        help="THE GATE ARM: replay a game-start journal on the SHIP config and against the whole "
        "DLL rolled back to original bodies, and require identical per-step region hashes. Needs "
        "--ui-steps > 0 -- the in-game steps after the start sequence are the comparison. Takes a "
        "path or a UIREC_SCENARIOS name (e.g. spcamp_newgame).",
    )
    ap.add_argument(
        "--ui-abc",
        metavar="NAME|JOURNAL",
        help="THE GATE SCENARIO: replay a game-start journal over its WHOLE length in three arms -- "
        "A ship, B all-original, C the committed recording oracle -- and require A==B (the promotion "
        "oracle) and B==C (the harness oracle). Ends when the journal ends. Takes a UIREC_SCENARIOS "
        "name (e.g. spcamp_solo) or a path whose sibling <name>.oracle.gz exists.",
    )
    ap.add_argument(
        "--ui-slot",
        type=int,
        default=0,
        metavar="N",
        help="lane slot BASE for the UI-REC arms (arms use N and N+1; default 0, the historical "
        "ui_play/ui_play1 pair). Two --ui-abc invocations running CONCURRENTLY (run_gate) must use "
        "disjoint bases or the second's provisioning deletes the first's live lane out from under "
        "it -- measured on run_gate's first run, 2026-09-10.",
    )
    ap.add_argument(
        "--ui-oracle",
        metavar="RUNDIR|LOG",
        help="build a scenario's C-arm oracle from a recorded session (its run dir or harness log). "
        "Needs --ui-oracle-out. Also prints the order histogram to paste into the registry entry.",
    )
    ap.add_argument(
        "--ui-oracle-out",
        metavar="PATH",
        help="--ui-oracle: where to write the .oracle.gz.",
    )
    ap.add_argument(
        "--ui-selftest",
        action="store_true",
        help="prove the game-start record->replay round trip with NO human: record a scripted menu "
        "walk (ui_drive injects into the same rings the recorder samples), then replay the journal "
        "with no script and require identical per-step region hashes.",
    )
    ap.add_argument(
        "--ui-selftest-script",
        default="sp_det.txt",
        metavar="NAME",
        help="--ui-selftest: the scripted walk to record (default sp_det.txt -- menu to a live game).",
    )
    ap.add_argument(
        "--ui-selftest-frames",
        type=int,
        default=80000,
        metavar="N",
        help="--ui-selftest: [uitest] per-step watchdog in frames (default 80000).",
    )
    ap.add_argument(
        "--ui-steps",
        type=int,
        default=0,
        metavar="N",
        help="--ui-replay/--ui-equiv: sim steps to run AFTER the journal is exhausted (0 = exit as "
        "soon as it is, which is right for --ui-replay and refused by --ui-equiv).",
    )
    ap.add_argument(
        "--ui-gored",
        type=int,
        default=0,
        metavar="STEP",
        help="--ui-equiv: THE GO-RED ARM. Poke every hashed region on the `original` arm at this sim "
        "step; the run then PASSES only if the comparison diverges. A fixture whose red has never "
        "been seen is not evidence when it is green.",
    )
    ap.add_argument(
        "--ui-strat-seed",
        type=int,
        default=7,
        metavar="N",
        help="[harness] strat_seed for the arm: the constant that replaces the campaign's "
        "wall-clock RNG seed (llm_strat_rng_seed_wallclock_seconds). Change it to drive "
        "SPCAMP-SEED's negative case -- a different seed MUST land the players elsewhere.",
    )
    ap.add_argument(
        "--ui-timeout",
        type=int,
        default=900,
        metavar="SEC",
        help="--ui-replay/--ui-equiv: per-arm wall-clock budget (default 900).",
    )
    ap.add_argument(
        "--ui-mouse-div",
        type=int,
        default=None,
        metavar="N",
        help="--ui-play: DirectInput mouse divisor (default %d, the measured VM value; 0 = leave "
        "the shipped value alone)." % TACT_PLAY_MOUSE_DIV,
    )
    ap.add_argument(
        "--ui-mouse-accel",
        type=int,
        default=0,
        metavar="N",
        help="--ui-play: DirectInput ballistic threshold (0 = shipped value, 100).",
    )
    ap.add_argument(
        "--ui-mouse-absolute",
        action="store_true",
        help="--ui-play: EXPERIMENTAL. Drop the DirectInput mouse device so the wndproc tap's "
        "absolute arm runs instead -- measured to kill input entirely; see the VM-input notes.",
    )
    ap.add_argument(
        "--tact-mouse-trace",
        action="store_true",
        help="--tact-play: log per-frame mouse-ring telemetry (produced/depth/max, and whether the "
        "DirectInput producer is live) to mh_uidrive.log. Diagnostic only -- changes no behaviour. "
        "Use it to locate an input BACKLOG rather than guessing at one.",
    )
    ap.add_argument(
        "--tact-mouse-div",
        type=int,
        default=None,
        metavar="N",
        help="--tact-play: divide every DirectInput mouse count by N before it is accumulated "
        "(ship value 1 = no attenuation). Higher = less sensitive. 0 = leave stock; a real 0 would "
        "fault the game, since it is a signed IDIV divisor. DEFAULTS TO %d for --tact-play, which "
        "is 32767/640 -- the absolute range a hypervisor reports over the screen width, i.e. a "
        "DERIVATION rather than a tuning constant. Measured playable with the dinputto8 wrapper in "
        "place (the VM-input notes 9e); scale it if the screen is not 640 wide."
        % TACT_PLAY_MOUSE_DIV,
    )
    ap.add_argument(
        "--tact-mouse-accel",
        type=int,
        default=0,
        metavar="N",
        help="--tact-play: DirectInput ballistic threshold -- a count whose magnitude exceeds N is "
        "DOUBLED (ship value 100). Set it huge (e.g. 100000) to disable the boost. 0 = leave stock.",
    )
    ap.add_argument(
        "--tact-mouse-absolute",
        action="store_true",
        help="--tact-play: EXPERIMENTAL. Drop the DirectInput mouse device so the wndproc "
        "absolute-coordinate path becomes the producer. Measured 2026-08-24 to leave the game with "
        "NO mouse input at all; kept because the reason is not understood. See "
        "the tactical-probe work, 9b, for the working route (a guest-device change).",
    )
    ap.add_argument(
        "--tact-system",
        type=int,
        default=0,
        metavar="N",
        help="--tact-determinism: override CurrentSystem, which is what selects the mission file "
        "POZ<N>{O,L}.DAT (NOT G_PLANET_INDEX -- a different global). 0 = leave the save's own "
        "value. The O/L half is the race of --tact-owner's player.",
    )
    ap.add_argument(
        "--tact-frames",
        type=int,
        default=400,
        metavar="N",
        help="--tact-determinism: tactical frames to log and compare (default 400).",
    )
    ap.add_argument(
        "--tact-wall",
        type=int,
        default=40,
        metavar="SEC",
        help="--tact-determinism: wall-clock cap per arm. A tactical mission has no "
        "enemy-wipe end condition and tact_stop_step ends the LOGGING, not the process, "
        "so the process is killed and the frame count read from the log.",
    )
    ap.add_argument(
        "--tact-poke-at",
        type=int,
        default=200,
        metavar="FRAME",
        help="--tact-selftest: frame to mutate at (default 200).",
    )
    ap.add_argument(
        "--tact-poke-idx",
        type=int,
        default=2,
        metavar="IDX",
        help="--tact-selftest: TACT_HASH_REGIONS[] index to mutate (default 2 = "
        "tact_doors, chosen because it does NOT otherwise change during a run).",
    )
    ap.add_argument(
        "--tact-squad",
        type=int,
        default=8,
        metavar="N",
        help="--tact-determinism: [tactical] squad size (blackboard slots filled).",
    )
    ap.add_argument(
        "--tact-hp",
        type=int,
        default=100,
        metavar="PCT",
        help="--tact-determinism: [tactical] per-soldier energy percent.",
    )
    ap.add_argument(
        "--tact-owner",
        type=int,
        default=1,
        metavar="PLAYER",
        help="--tact-determinism: [tactical] target building's owner -- its RACE picks "
        "POZ<CurrentSystem>{O,L}.DAT, so this scalar selects the mission map.",
    )
    ap.add_argument(
        "--soak",
        action="store_true",
        help="THE ALL-AI SOAK: an N-way AI match through the real menu, with the LOCAL slot converted "
        "to an AI too (`[harness] all_ai`, a pre-landing status_flags flip, so it goes through "
        "llm_strat_spawn_ai_base like any computer player rather than ticking with no home tile). "
        "Produces a state-evolution trajectory plus run-shape refusals, not a pass/fail comparison. "
        "The coverage lever for AI work that `loadgame_at` is for start state.",
    )
    ap.add_argument(
        "--soak-observer",
        type=int,
        default=-1,
        metavar="SLOT",
        help="--soak: repoint PlayerSide at this slot (-1 = leave it alone, the default -- slot 0 has "
        "a claimed landing site so the camera opens on that AI's base). Set an UNCLAIMED slot to "
        "detach local selection/ctrl-group state from every playing side.",
    )
    ap.add_argument(
        "--soak-gameover-step",
        type=int,
        default=100,
        metavar="N",
        help="--soak: check for match resolution every N steps (0 = off). A soak that keeps stepping "
        "past a game-over measures a finished world. Keep it SMALL: the match ends on an outcome "
        "dialog that stops the sim, so a coarse cadence can miss the last window entirely -- a 500 "
        "run saw alive=8 at 1000 and the sim stopped at 1475 with nothing in between.",
    )
    ap.add_argument(
        "--soak-speed",
        type=int,
        default=0,
        metavar="PCT",
        help="--soak: [net] game_speed_pct. steps/s is ~100 in EVERY configuration, so this does not "
        "shorten a step-counted run -- it changes how much GAME TIME each step carries, which is what "
        "matters when the thing being waited for is a game-clock milestone. Goldens are not portable "
        "across speeds.",
    )
    ap.add_argument(
        "--soak-slot",
        type=int,
        default=0,
        metavar="N",
        help="--soak: lane slot (default 0 = the historical ui_soak lane). Concurrent soaks "
        "(migration_ab's parallel fixtures/arms) must use disjoint slots -- each slot is its own "
        "lane folder, lane number and port.",
    )
    ap.add_argument(
        "--soak-save",
        default="",
        metavar="NAME",
        help="--soak: seed the run from a .sav (name WITHOUT extension, staged in the lane's save/ "
        "folder) instead of the default start -- the the save index lever. Confirm the "
        "'; [save] LOADGAME ... rc=1' line: a refused load leaves the skirmish running and looks "
        "like an ordinary quiet result.",
    )
    ap.add_argument(
        "--soak-load-at",
        type=int,
        default=120,
        metavar="STEP",
        help="--soak: step at which --soak-save is loaded (default 120, letting the session settle).",
    )
    ap.add_argument(
        "--soak-golden",
        default="",
        metavar="PATH",
        help="--soak: record (if absent) or compare (if present) the per-step per-region hash "
        "trajectory. The integration-test half -- it reports the FIRST diverging step and region, "
        "not just that two runs ended differently.",
    )
    ap.add_argument(
        "--harness-extra",
        default="",
        help="--soak: extra ';'-separated [harness] keys appended to the mode's own set.",
    )
    ap.add_argument(
        "--soak-dll",
        default="",
        metavar="PATH",
        help="--soak: deploy THIS mh.dll into the lane instead of the Release build. For "
        "tools/coverage.py, which needs the unoptimised one -- a Release /O2+LTCG binary reports "
        "inlined-away bodies as 0%% covered, which is indistinguishable from never executed. The "
        "sibling .pdb rides along; without it the collector emits a report with no source at all.",
    )
    ap.add_argument(
        "--det-selftest",
        action="store_true",
        help="check C7's three runner rules without a rig (part of lint_repo): the fixes-off knob set "
        "is derived, a host-only fix knob is refused BY NAME, and a shape whose asymmetry evaporated "
        "-- in either direction -- fails rather than passing on a green hash.",
    )
    ap.add_argument(
        "--det-3peer",
        action="store_true",
        help="--determinism: run ONLY the U28 3-PEER START-BARRIER shape (host on vms[0], client on "
        "vms[1], a third peer as a LOCAL LANE on this box). Both clients poke their own lobby slot "
        "with no 0x0c push, so the host disagrees with BOTH at Start and must override both. Fails "
        "if a client did not actually disagree -- a green hash over an injection that never "
        "survived to Start proves nothing. Included automatically in --det-standard.",
    )
    ap.add_argument(
        "--det-standard",
        action="store_true",
        help="--determinism: run the standard shapes and report them SEPARATELY, because a "
        "merged verdict hides WHICH shape failed -- SYMMETRIC at ship config (promotion on both "
        "peers), the U28 "
        "3-peer start barrier and U32's conquest. The ASYMMETRIC shape (ours vs the ORIGINAL) was "
        "REMOVED from this set on 2026-09-01: the original netcode is dead in retail and live only "
        "under the byte patches, so it is a COMPAT question, not this gate's. Run it by hand -- see "
        "run_det_standard's docstring. A shape whose promotion "
        "state is not what it asked for FAILS rather than passing on a green hash.",
    )
    ap.add_argument(
        "--ai",
        action="store_true",
        help="--determinism: seat an AI OPPONENT at slot index 2 (mp_host_start_ai.txt) and turn on the "
        "AIPROBE line. D10: every determinism run before this compared a world where the AI never drew "
        "a random number. Requires D11's widened manifest -- an AI lands at slot >= 2, whose player_data "
        "store was outside every hashed region until then.",
    )
    ap.add_argument(
        "--harness-extra-host",
        help="--determinism: extra [harness] lines for the HOST peer ONLY, ';'-separated. The "
        "NEGATIVE-test lever: `region_poke_at=200;region_poke_min=41` flips one byte in every region "
        "from index 41 up, on one peer, so a newly-hashed region can be shown to go RED before it is "
        "trusted to referee anything. `region_poke_off=N` moves the flipped byte N bytes into each "
        "region -- needed when byte 0 is live state the sim reads, since a poke that also changes "
        "BEHAVIOUR reddens other regions too and destroys the localization half of the proof (D23 "
        "pokes `players`+0x10, its name[32], which nothing reads).",
    )
    ap.add_argument(
        "--extra-ini-host",
        help="--determinism: ini fragment for the HOST peer ONLY, so the two peers differ on purpose. "
        "O3 uses it to run the ORIGINAL order container on one peer and the PROMOTED one on the other, "
        "which is the only shape in which the lockstep hash can referee a reimplementation -- a "
        "symmetric promoted run just proves the two peers agree with each other.",
    )
    ap.add_argument(
        "--extra-ini",
        action="append",
        default=[],
        metavar="FILE",
        help="--determinism AND --soak: append this ini fragment to BOTH peers' mh_net.ini "
        "(ui_test.py --extra-ini). The reason it exists is the reimpl loop: arming a shadow batch is "
        "an ini gate, so a shadow-arming fragment turned a determinism run into the "
        "differential oracle pass for that batch as well. The capture suite takes its fragment per "
        "test instead. REPEATABLE, and it has to be: this was a scalar until 2026-08-05, which "
        "silently kept only the LAST fragment -- the same defect ui_test.py's own --extra-ini "
        "carried until 2026-08-02, where it armed 2 of 4 shadow sites on a run whose arming set "
        "check_arming_set.py had validated over all 4.",
    )
    ap.add_argument(
        "--ship-pacing",
        action="store_true",
        help="--determinism: drop the rig's pinned lockstep_step_ms/sim_step_ms so the run uses the "
        "DLL's SHIPPING pacing (100 ms lookahead + adaptive controller, 20 ms sim sub-step) -- i.e. "
        "validate what players actually run, not the rig's historical 30/10 pin.",
    )
    ap.add_argument(
        "--record",
        type=int,
        default=0,
        metavar="MODE",
        help="--determinism: forwarded to ui_test.py --record (harness order_mode). 1 RECORDS every "
        "dispatched order to each peer's mh_orders.bin, which tools/mp_order_diff.py decodes into "
        "'peer A scheduled THIS order on step N and peer B on step N+2'. The reason it is worth a "
        "flag: mp_analyze can only say order_queue DIFFERS, because a region hash is opaque -- the "
        "recording is what turns that into a named order and a per-peer step (MP D14, and the "
        "measurement path D17 left behind). Default 0 = off; it costs a few hundred KB per peer.",
    )
    # LOCAL IS THE DEFAULT (2026-07-29). One lane folder per peer per test on THIS box, implying
    # --headless. Two reasons it is the default rather than an option: it isolates every test from the
    # setup.dat / [video] state the previous one left behind, and it FREES THE VMs -- so the UI suite
    # and a 2-machine determinism run stop contending for the same two peers and can run at once.
    # --no-local restores the old behaviour. NOT named --vms: that flag already exists and carries the
    # VM IP LIST, so reusing the name would be an argparse conflict, not an override.
    ap.add_argument(
        "--local",
        action="store_true",
        default=True,
        help="(default) run the whole suite on THIS machine instead of the VMs, with one lane folder "
        "per peer per test (LANE_ROOT/ui_<test> for a solo test, ui_<test>_host + ui_<test>_cN for a "
        "multi-peer one). Implies --headless.",
    )
    ap.add_argument(
        "--no-local",
        dest="local",
        action="store_false",
        help="opt OUT of --local: run the suite across the real peer VMs listed by --vms. Use when the "
        "thing under test is machine-dependent (a cross-machine transport or timing question); a pure "
        "UI/render regression does not need it and pays VM latency plus contention with any rig run.",
    )
    # PARALLEL BY DEFAULT (2026-08-02). Measured on this box (8C/16T): 12/12 in 1.8 min at --jobs 4
    # against 4.5 min serial, 2.5x, same verdicts. Each test owns its lane folders, its port and its
    # mutex number, so the only shared resource is the CPU.
    #
    # "CORRECTNESS RUNS ONLY" still holds and is now enforced by CONTROL FLOW rather than by the
    # caller remembering: --determinism and --sp-determinism both return from main() before `jobs` is
    # read at all, so contention cannot reach a run that measures pacing. Do not move those dispatches
    # below the jobs loop.
    #
    # Scaled by CPU rather than pinned to 4: this file is also run on the peer VMs and on whatever
    # box comes next, and a fixed 4 on a 4-core machine means ~8 game instances fighting over 4
    # cores, which is how a correctness suite starts producing timeouts that look like failures.
    ap.add_argument(
        "--net-jobs",
        type=int,
        default=4,
        metavar="N",
        help="the multi-peer tests' OWN pool width (default %(default)d). Separate from --jobs "
        "because a capped multi-peer test is wait-bound ([video] fps_cap: its peers sleep on the "
        "lockstep clock or on each other), so it should not hold a CPU-bound --jobs slot. Bounded "
        "by the number of multi tests; --jobs 1 disables the split entirely.",
    )
    ap.add_argument(
        "--jobs",
        type=int,
        # One core in four runs a solo lane; the only clamp is the floor. (Was also capped at 4,
        # which with the scaling term made the default a double clamp nobody could read at a
        # glance -- user, 2026-09-10. Big boxes were the cap's only subjects, and per-lane
        # isolation is exactly per-lane, so they can simply have the width.)
        default=max(1, (os.cpu_count() or 4) // 4),
        help="run this many TESTS concurrently (requires --local, which is the default; default "
        "scales with CPU count, %(default)d here). Note the peer count exceeds --jobs: a multi-peer "
        "test launches 2-3 game processes, so --jobs 4 can mean ~8 instances. Output is buffered per "
        "test and printed whole on completion. Pass --jobs 1 to serialise, e.g. when a failure's "
        "interleaved logs are hard to read.",
    )
    # HEADLESS IS THE DEFAULT (2026-07-28). The blit inside llm_gfx_present_flip is cut, but the frame
    # is still composed in software, so CAPTURES ARE BYTE-IDENTICAL (A/B-verified by SHA-256) and every
    # baseline applies unchanged -- the suite is 12/12 headless on both the VM and local topologies. It
    # costs no wall clock and steals no focus. See the parallel-lane notes.
    # DESKTOP ISOLATION IS THE DEFAULT (2026-08-02) -- see ui_test.py's resolve_desktop. Orthogonal to
    # --headless. Applies to LOCAL launches only, which is every capture test but no determinism run
    # (those are VM-only); see the note in run_ui_test.
    ap.add_argument(
        "--net-extra",
        default="",
        help="';'-separated k=v appended to [net], overriding the same key in place (including the "
        "rig's pinned pacing). Forwarded to --determinism runs.",
    )
    # The AUDIT LEVER (2026-09-01): --net-extra reaches only [net] keys and per-scenario extra_ini
    # lives in the TESTS registry, so a suite-wide [tombstone] arm_dead audit had NO route -- its
    # first "green" was vacuous (the fragment never reached any lane; the X-TOMB resolve session).
    ap.add_argument(
        "--extra-ini-all",
        help="append this ini fragment to EVERY suite scenario (the audit lever, e.g. "
        "tools/uiscripts/ini/tombstone_audit.ini); baselines are recorded without it",
    )
    ap.add_argument(
        "--det-local",
        action="store_true",
        help="run --determinism with BOTH peers on this box (local lanes) instead of the VM pair. "
        "Faster and needs no VMs, but a WEAKER test: same CPU, same libraries, same FP environment, "
        "so a divergence that only appears across machines cannot show up. For iteration; keep the "
        "VM pair as the gate.",
    )
    ap.add_argument(
        "--desktop",
        nargs="?",
        const="mh_rig",
        default=None,
        help="name of the isolated desktop (default: mh_rig). Isolation is ON unless --no-desktop or "
        "--visible.",
    )
    ap.add_argument(
        "--no-desktop",
        action="store_true",
        help="opt OUT of desktop isolation: run every game process on YOUR interactive desktop.",
    )
    ap.add_argument("--headless", action="store_true", help="(default; kept for compatibility)")
    ap.add_argument("--stock-exe", action="store_true", help="(default; kept for compatibility)")
    ap.add_argument(
        "--patched-exe",
        action="store_true",
        help="opt OUT: provision lanes and peers around the import-patched mh.focus.exe (the "
        "pre-2026-08-27 mechanism) instead of a byte-for-byte RETAIL mh.exe + the msvfw32 proxy "
        "shim. Lane/peer exes are NAMED mh.focus.exe either way; only the bytes differ.",
    )
    ap.add_argument(
        "--visible",
        action="store_true",
        help="opt OUT of headless: restore the blit and show each game window. --determinism and "
        "--ship-pacing do this for themselves (no blit = no vsync wait = the wrong frame rate to "
        "measure pacing at); ui_test.py --force-headless overrides that.",
    )
    ap.add_argument(
        "--skip-intro-avi",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="dismiss the new-game intro movie + NEWGAME.TXT briefing by pressing the game's own "
        "SPACE for it, saving ~120 s of wall time PER ARM. ON by default since 2026-09-07. It was "
        "off until then because it moves the menu's present timeline and that took a shallow rung "
        "red -- but the cause of that was G146 (the pinned clock advancing through a barrier's "
        "wall-clock settle dwell), and with that fixed the rungs are green with the skip on. "
        "--no-skip-intro-avi restores the old behaviour if a fixture ever needs the movie played.",
    )
    ap.add_argument(
        "--ui-all-original",
        action="store_true",
        help="--ui-play/--ui-record: run the session with EVERY [promote] key and [rebind] row rolled "
        "OFF, i.e. on the original binary. Use it when recording a journal meant to be a REFERENCE: a "
        "recording made on the ship config bakes our promoted bodies' behaviour into its expected hash "
        "stream, so a promotion bug present at record time can never be caught by replaying against "
        "it. Same derivation as --ui-equiv's `original` arm.",
    )
    ap.add_argument(
        "--ui-pin-menu-clock",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="--ui-replay/--ui-play: pin the menu ms clock (harness.cpp pin_menu_clock). ON by "
        "default. --no-ui-pin-menu-clock reproduces the configuration a journal recorded before the "
        "pin existed was played on, which is the only way to separate a replay bug from a fixture "
        "that predates the pin.",
    )
    ap.add_argument(
        "--ui-tj-ps",
        type=int,
        default=0,
        metavar="N",
        help="during a replay, log a per-step `; TJPS` census row every N sim steps -- present/step, "
        "the two UI-path entry counts, both input rings' cursors, the pinned clock, the cursor, and "
        "the ring producer census (fs/fo, plus a `; TJFOREIGN` dump of any ring slot the journal did "
        "not write). 0 = off.",
    )
    ap.add_argument(
        "--ui-no-isolate",
        action="store_true",
        help="SPCAMP-FLAKE's NEGATIVE arm: do NOT suppress the game's own input-ring producer during "
        "the replay ([harness] replay_isolate_input=0). A replay normally has two producers -- the "
        "journal and llm_input_wndproc_tap, which runs on every window message -- and real host mouse "
        "activity during the run then diverges the sim. With the default (suppressed) the same run is "
        "identical to a quiet one. Use this to reproduce the flake on demand, not to test with.",
    )
    ap.add_argument(
        "--ui-tj-trace-from",
        type=int,
        default=0,
        metavar="I",
        help="--ui-tj-trace's low end (default 0), so the trace is a WINDOW [I, N) rather than a "
        "prefix. Tracing a record at index 36000 from 0 costs 36000 lines of noise for the 40 that "
        "matter.",
    )
    ap.add_argument(
        "--ui-tj-trace",
        type=int,
        default=0,
        metavar="N",
        help="--ui-replay/--ui-play: log the first N journal records AS THEY ARE INJECTED (idx, "
        "seam, recorded frame, due step, the step it ACTUALLY landed on, shift) plus the pinned "
        "clock at each of the first 12 sim steps, into mh_harness.log. Default 0 = off. This is the "
        "instrument G146 was found with -- use it when a replay's input lands at the right place at "
        "one frame rate and the wrong one at another.",
    )
    ap.add_argument(
        "--fps-limit",
        type=int,
        default=None,
        metavar="N",
        help="dgVoodoo FPSLimit for every lane this run provisions; 0 = UNLIMITED. Pair with "
        "--visible to WATCH a long replay faster than it was played: the 60 fps cap is the "
        "wrapper's, not the game's, so a visible run is otherwise pinned there and a full-session "
        "replay costs the wall time of the session that produced it. Not for --ship-pacing (the "
        "cap is what ships) and pointless headless (no blit to pace).",
    )
    ap.add_argument(
        "--per-test-timeout", type=int, default=280, help="wrapper kill-timeout per test (s)"
    )
    # pass-throughs to ui_test.py
    ap.add_argument("--timeout", type=int, default=200, help="ui_test run wall-clock (s)")
    ap.add_argument("--tol", type=float, default=0.02)
    ap.add_argument("--pixdelta", type=int, default=40)
    args = ap.parse_args()

    global DESKTOP, NO_DESKTOP, STOCK_EXE
    NO_DESKTOP = bool(args.no_desktop)
    STOCK_EXE = not args.patched_exe
    # Resolved HERE and forwarded as an explicit --desktop, rather than letting each ui_test.py child
    # re-derive it: the children would each apply their own --visible/--no-desktop logic to flags the
    # suite may not have passed them, and two independent defaults is how a setting ends up on for
    # some tests and off for others.
    if not args.no_desktop and (args.desktop or not args.visible):
        DESKTOP = args.desktop or "mh_rig"
        print("[rig] isolated desktop: %s -- game windows cannot reach your desktop" % DESKTOP)

    if args.det_selftest:
        return det_standard_selftest()

    if args.sp_determinism:
        return run_sp_determinism(args)

    if args.tact_equiv:
        return run_tact_equiv(args)
    if args.tact_verify:
        return run_tact_verify(args)
    if args.tact_replay:
        return run_tact_replay(args)
    if args.tact_arm:
        return run_tact_arm(args)
    if args.selftest_refusals:
        return selftest_refusals()
    if args.tact_determinism:
        return run_tact_determinism(args)

    if args.tact_play:
        return run_tact_play(args)

    if args.ui_selftest:
        return run_ui_selftest(args)
    if args.ui_oracle:
        return run_ui_oracle(args)
    if args.ui_abc:
        return run_ui_abc(args)
    if args.ui_equiv:
        return run_ui_equiv(args)
    if args.ui_replay:
        return run_ui_replay(args)
    if args.ui_play:
        return run_ui_play(args)

    if args.soak:
        return run_soak(args)

    if args.tact_trim:
        return run_tact_trim(args)
    if args.tact_suite:
        return run_tact_suite(args)

    wanted = set(args.only) | set(args.only_flag)
    tests = [t for t in TESTS if not wanted or t["name"] in wanted]
    if args.solo:
        tests = [t for t in tests if t["kind"] == "solo"]
    if wanted:
        missing = wanted - {t["name"] for t in TESTS}
        if missing:
            ap.error("unknown test name(s): %s (see --list)" % ", ".join(sorted(missing)))

    if args.list:
        print("UI tests (%d):" % len(TESTS))
        for t in TESTS:
            peers = "solo" if t["kind"] == "solo" else "host+%d client(s)" % len(t["clients"])
            print("  %-14s [%s]  %s" % (t["name"], peers, t["desc"]))
        print("  (special) determinism  UI-path lockstep hash check -- `--determinism [--steps N]`")
        print(
            "Tactical journal scenarios (%d) -- `--tact-suite`, NOT in the default suite:"
            % len(TACT_SCENARIOS)
        )
        for t in TACT_SCENARIOS:
            print("  %-14s [%s]  %s" % (t["name"], "+".join(t["arms"]), t["desc"]))
        return 0

    if args.determinism:
        # C7: promotion is TWO runs, not one. --det-standard runs both shapes and reports them apart.
        if args.det_standard:
            return run_det_standard(args)
        if args.det_3peer:
            ok, lines = run_det_3peer(args)
            print("\n".join(lines))
            if ok is None:
                return 0  # VM down -> SKIP, not a failure
            print("[det] U28 3-PEER BARRIER: %s" % ("PASS" if ok else "FAIL"))
            return 0 if ok else 1
        err = asymmetric_fix_config_error(args.extra_ini_host)
        if err:
            print("[det] " + err)
            return 2
        # UI-PATH determinism: reuse the match_launch topology (host launches, client enters via real UI)
        # but let ui_test.py run the in-game [harness] logger + mp_analyze instead of diffing captures.
        host_script = "mp_host_start_ai.txt" if args.ai else "mp_host_start.txt"
        if args.det_local:
            # BOTH PEERS ON THIS BOX. A WEAKER TEST THAN THE VM PAIR, and the banner says so: two
            # peers here share one CPU, one set of libraries and one FP environment, so precisely the
            # class of divergence this gate exists to catch -- one that appears only ACROSS machines --
            # is the class it cannot see locally. Use it to iterate; keep the VM pair as the gate.
            #
            # headless=False on purpose: a lane's lane.json carries its headless choice and make_ini
            # ORs it with the flag, so provisioning these lanes headless would cut the blit and this
            # run would measure the ~8500 fps headless cadence instead of the real one -- which is the
            # exact thing resolve_headless refuses to let --determinism do.
            det_test = {
                "name": "determinism",
                "kind": "multi",
                "host": host_script,
                "clients": ["mp_client_start.txt"],
            }
            plan = provision_lanes(
                [det_test],
                headless=False,
                port_base=DET_LOCAL_PORT,
                lane_base=DET_LOCAL_LANE_BASE,
                stock_exe=STOCK_EXE,
            )
            if not plan:
                return 1
            port, _, names = plan["determinism"]
            print(
                "[det] LOCAL pair on this box -- weaker than the VM pair (shared CPU/libs/FP env)"
            )
            argv = [
                "--determinism",
                "--steps",
                str(args.steps),
                "--host",
                "lane=%s:%s" % (names[0], host_script),
                "--connect-ip",
                "127.0.0.1",
                "--port",
                str(port),
                "--client",
                "lane=%s:mp_client_start.txt" % names[1],
                "--timeout",
                str(max(args.timeout, 120 + args.steps)),
            ]
        else:
            argv = [
                "--determinism",
                "--steps",
                str(args.steps),
                "--host",
                "%s:%s" % (args.vms[0], host_script),
                "--client",
                "%s:mp_client_start.txt" % args.vms[1],
                "--connect-ip",
                args.vms[0],
                "--timeout",
                str(max(args.timeout, 120 + args.steps)),
                # DET-FLAKE (shape B, 2026-09-10). The host's `peers 1` step -- "wait for the client
                # to connect" -- is bounded by [uitest] timeout_frames, which resolves to 1500 FRAMES
                # here because a determinism run keeps the blit: ~29 s at the lobby's ~52 fps. The
                # driver needs ~22 s just to notice the host is LISTENING and launch the client (2 s
                # poll granularity + setup.dat scp + ssh), and the client then needs ~7.5 s of its own
                # to walk to Connect. That run missed by 0.5 s. 6000 frames is ~2 min at the same
                # cadence, comfortably past the driver's launch cost and still well inside
                # --launch-timeout (300 s), which is what actually bounds a wedged rendezvous.
                #
                # THE UNIT IS STILL WRONG and this does not fix it: a per-step watchdog counted in
                # FRAMES cannot express "wait N seconds for another machine", which is why the same
                # default means ~29 s blitted and minutes headless. Changing it is a DLL change
                # (the uidrive watchdog lives in the harness), and is an open item.
                "--timeout-frames",
                "6000",
            ]
        if args.ship_pacing:
            argv.append("--ship-pacing")
        for frag in args.extra_ini:
            argv += ["--extra-ini", frag]
        if args.extra_ini_host:
            argv += ["--extra-ini-host", args.extra_ini_host]
        if args.harness_extra_host:
            argv += ["--harness-extra-host", args.harness_extra_host]
        if args.net_extra:
            # Forwarded so a determinism run can override a PINNED_PACING key in place -- ui_test's
            # make_ini drops the NET_BLOCK line whose key an extra also sets, so this is a real
            # override rather than a duplicate the FIRST-match lookup would ignore. Added for the
            # rx_spin A/B: the rig pins rx_spin=0 while SHIP_RX_SPIN is 1, so the gate cannot see the
            # cost of a knob every shipped run has on.
            argv += ["--net-extra", args.net_extra]
        if args.ai:
            # The probe is not optional decoration on an AI run: D10 exists because a green verdict was
            # taken as covering the AI when the AI had never drawn a random number. An AI-active run
            # that cannot SHOW the AI running is the same mistake with an extra step.
            argv += ["--ai-probe", "100"]
        if not args.det_local:
            down = [ip for ip in args.vms[:2] if not vm_reachable(ip)]
            if down:
                print("determinism SKIP -- VM(s) unreachable: %s" % ", ".join(down))
                return 0
        return run_ui_test(argv, max(args.per_test_timeout, 180 + args.steps))[0]

    common = [
        "--timeout",
        str(args.timeout),
        "--tol",
        str(args.tol),
        "--pixdelta",
        str(args.pixdelta),
    ]
    if args.update_baselines:
        common.append("--update-baselines")
    # The AUDIT LEVER (2026-09-01): append one ini fragment to EVERY suite scenario. Exists because
    # --net-extra can only reach [net] keys and the per-scenario extra_ini lives in the TESTS
    # registry -- so a suite-wide [tombstone] arm_dead audit had NO lever, and its first "green"
    # was vacuous (the fragment never reached any lane). Baselines are recorded WITHOUT audit
    # fragments; anything visual in one would rightly fail the diff.
    if args.extra_ini_all:
        common += ["--extra-ini", args.extra_ini_all]
    # Pass the resolved choice DOWN explicitly rather than relying on ui_test's own default, so a
    # suite run and the ui_test.py command it prints mean the same thing when replayed by hand.
    headless = not args.visible
    common.append("--headless" if headless else "--visible")

    plan = None
    if args.local:
        print("provisioning %d local lane set(s) under %s ..." % (len(tests), make_lane.LANE_ROOT))
        plan = provision_lanes(tests, headless=headless, stock_exe=STOCK_EXE)
        if plan is None:
            return 1
        common += ["--timeout-frames", str(LOCAL_TIMEOUT_FRAMES)]

    jobs = max(1, args.jobs)
    if jobs > 1 and not args.local:
        print(
            "--jobs > 1 requires --local: the VM topology has ONE host VM and one client VM, so two "
            "tests at once would fight over the same two machines. Lanes are what make concurrency "
            "possible, and only --local provisions them."
        )
        return 2

    # ---- SHARED-CAUSE PREFLIGHT: an INSTALL-time entry loss, not 17 UI failures -----------------
    #
    # 2026-09-01 (dead-ends G99): the effects gate took llm_gfx_present_flip's entry, the present
    # hook was refused it, and since capture / UI drive / overlay ALL piggyback on_present the whole
    # harness went dark. What the suite printed was seventeen scenarios each burning its full budget
    # on "did not present a frame" -- identical timeouts describing a UI regression that did not
    # exist. The cause was one line in the run's OWN mh_net.log, written before the first script
    # step, and nothing was reading it.
    #
    # So: on the FIRST failure, read the install log. A reason that is install-WIDE will fail every
    # remaining scenario the same way, and running them proves nothing -- abort and NAME it. One
    # failure with a cause beats seventeen without one.
    def install_refusals(test):
        """Install-time refusals in this test's lanes, newest run each. [] when the logs say nothing.

        Read from the LANE's own mh_net.log rather than from ui_test's stdout: the refusal is
        printed by the injected DLL at install time, before any harness output exists to carry it."""
        found = []
        if plan is None:
            return found  # --no-local: the logs are on the VMs, not reachable from here
        _, _, lane_names = plan.get(test["name"], (None, None, []))
        for lane in lane_names:
            logs = sorted(
                glob.glob(os.path.join(make_lane.LANE_ROOT, lane, "logs", "*", "mh_net.log")),
                key=os.path.getmtime,
            )
            if not logs:
                continue
            try:
                with open(logs[-1], encoding="utf-8", errors="replace") as fh:
                    body = fh.read()
            except OSError:
                continue
            for why, line in install_refusal_lines(body):
                found.append((lane, why, line))
        return found

    # ---- PER-TEST POST-CHECK: an assertion the pixels cannot carry (fork F4A) -------------------
    #
    # A `[uitest]` script's predicates read UI STATE, so a scenario can assert what is on screen and
    # nothing else. Some claims are about the RUN rather than the frame -- F4A's is "a missing
    # sibling DLL refused loudly and the arm still finished", which has no pixels at all and whose
    # capture stays green whether or not the bind was even attempted. A test may therefore name a
    # command; it is run against that test's LANE after a green run, and a non-zero exit turns the
    # test red with the command's own output attached.
    #
    # The lane path is APPENDED rather than written into the entry: the runner is what knows which
    # lane a test got, and a hardcoded path in the registry would be a second answer that drifts.
    # Checkers take a lane folder and resolve its newest run themselves.
    def post_check(test):
        """(ok, text) for a test's `post_check`, or (True, "") when it has none / cannot run."""
        cmd = test.get("post_check")
        if not cmd:
            return True, ""
        # ONE COMMAND OR SEVERAL (fork F4D). A boot now has two satellites to make statements about,
        # and `libmh_absent` has to assert BOTH -- mh_net bound, libmh absent -- or it would pass on
        # a lane that lost the transport as well. Nested lists mean "run all of these"; the flat
        # form every existing entry uses is unchanged.
        cmds = cmd if isinstance(cmd[0], (list, tuple)) else [cmd]
        if len(cmds) > 1:
            ok_all, texts = True, []
            for c in cmds:
                ok, text = post_check(dict(test, post_check=list(c)))
                ok_all = ok_all and ok
                texts.append(text)
            return ok_all, "\n".join(t for t in texts if t)
        cmd = list(cmds[0])
        if plan is None:
            # --no-local: the lane logs live on the VM and this box cannot read them. SKIPPED and
            # SAID SO -- a silent pass here would make the local and VM topologies disagree about
            # what the suite proved.
            return (
                True,
                "  post-check SKIPPED: %s needs the local lane logs (--no-local run)" % cmd[0],
            )
        _, _, lane_names_ = plan.get(test["name"], (None, None, []))
        if not lane_names_:
            return False, "  post-check FAILED: no lane recorded for %s" % test["name"]
        lane_dir = os.path.join(make_lane.LANE_ROOT, lane_names_[0])
        argv = [sys.executable, os.path.join(REPO, *cmd[0].split("/"))] + list(cmd[1:]) + [lane_dir]
        r = subprocess.run(argv, capture_output=True, text=True)
        out = (r.stdout or "") + (r.stderr or "")
        head = "  post-check %s: %s" % ("PASS" if r.returncode == 0 else "FAILED", " ".join(cmd))
        return r.returncode == 0, head + "\n" + "\n".join("    " + ln for ln in out.splitlines())

    def run_one(idx, t):
        """Run one test. Returns (name, verdict, text-to-print). Safe to call from a worker thread."""
        head = (
            "\n"
            + "=" * 78
            + "\n[%d/%d] %s (%s) -- %s\n"
            % (
                idx,
                len(tests),
                t["name"],
                t["kind"],
                t["desc"],
            )
            + "=" * 78
        )
        if jobs == 1:
            print(head)
        if abort:
            msg = (
                "  SKIP -- suite aborted: %s hit an INSTALL-TIME refusal, which fails every "
                "scenario identically. Fix that first; this test was never run." % abort[0]
            )
            if jobs == 1:
                print(msg)
            return t["name"], "SKIP", head + "\n" + msg
        need = [] if args.local else required_vms(t, args.vms)
        down = [ip for ip in need if not vm_reachable(ip)]
        if down:
            msg = "  SKIP -- VM(s) unreachable: %s" % ", ".join(down)
            if jobs == 1:
                print(msg)
            return t["name"], "SKIP", head + "\n" + msg
        # The wrapper kill-timeout must outlast the test's OWN wall-clock budget, or it kills the run
        # before ui_test can report -- and a killed run has no verdict, only a missing one.
        t0 = time.time()
        rc, text = run_ui_test(
            build_argv(t, args.vms, common, plan),
            max(args.per_test_timeout, t["timeout"] + 120)
            if t.get("timeout")
            else args.per_test_timeout,
            capture=jobs > 1,
        )
        # Wall clock per test, because "the suite takes too long" is not actionable until you can see
        # WHICH test spends the time. Pairs with the per-step milliseconds ui_drive now logs.
        #
        # D15: report the wall clock AGAINST ITS BUDGET, not alone. A bare "[203s]" is unreadable --
        # you cannot tell a slow pass from a run that spent its entire budget waiting and then aborted,
        # and those are opposite findings. The budget is the ui_test --timeout this test was actually
        # given (its own override, else the suite default), which is the number that kills it.
        # A test over WARN_FRAC of its budget is a NEAR-MISS: still green, already unsafe, and the
        # thing that turns red first when the machine is busier. Printing it is what makes the drift
        # visible BEFORE it is a failure, which is exactly what nobody had on 2026-08-06.
        secs = time.time() - t0
        budget = t.get("timeout") or args.timeout
        frac = secs / budget if budget else 0.0
        el = "  [%.0fs / %ds budget, %.0f%%]" % (secs, budget, frac * 100)
        if frac >= WARN_FRAC:
            el += "  <-- NEAR-MISS: over %.0f%% of budget" % (WARN_FRAC * 100)
        if jobs == 1:
            print(el)
        timings[t["name"]] = (secs, budget)
        tail = el
        if rc != 0:
            refusals = install_refusals(t)
            if refusals:
                banner = "\n" + "!" * 78 + "\n"
                banner += (
                    "INSTALL-TIME REFUSAL in this run's own mh_net.log -- this is NOT a UI "
                    "failure, and every remaining scenario would fail the same way.\n"
                )
                for lane, why, line in refusals:
                    banner += "  [%s] %s\n      %s\n" % (lane, why, line)
                banner += (
                    "Two mechanisms wanted one entry; the log names the WRONG remedy, and "
                    "the suite then reads as N unrelated timeouts.\n"
                )
                banner += "!" * 78
                # PRINT IT HERE, not only into the returned text: under --jobs 1 the caller
                # discards that text and prints as it goes, so a banner that only rode home in
                # the return value would be invisible in exactly the serial run someone
                # debugging a dead suite reaches for first.
                if jobs == 1:
                    print(banner)
                tail += banner
                abort.append(t["name"])
        if rc == 0:
            # Only on a green run: a failed scenario's lane has nothing worth asserting about, and a
            # post-check red on top of a scenario red would name the wrong cause.
            ok, ptext = post_check(t)
            if ptext:
                if jobs == 1:
                    print(ptext)
                tail += "\n" + ptext
            if not ok:
                rc = 1
        return t["name"], ("PASS" if rc == 0 else "FAIL"), head + "\n" + text + tail

    # Set by the first failure whose cause is install-wide; every later test then SKIPs instead of
    # re-proving it. A list rather than a flag so the abort can name WHICH test found it.
    abort = []
    results = {}
    timings = {}  # name -> (elapsed_s, budget_s); feeds the summary table's budget column
    suite_t0 = time.time()
    if jobs == 1:
        for i, t in enumerate(tests, 1):
            name, verdict, _ = run_one(i, t)
            results[name] = verdict
    else:
        # Each test owns its own lane folder(s), port and mutex number, so the only shared resource is
        # the CPU. That makes this safe for CORRECTNESS runs and wrong for anything timing-sensitive --
        # the same rule headless already carries (the parallel-lane notes). Output is buffered per test
        # and printed whole on completion, in completion order.
        #
        # TWO POOLS SINCE 2026-09-10 (user: "I'd give them separate pool"). Since the [pacing]
        # fps_cap, a multi-peer test's peers SLEEP between frames -- their pace is the other peer
        # or the lockstep clock, not the CPU -- so one of them holding a --jobs slot for 30-95 s
        # starves the CPU-bound solo tests of a worker it barely uses. The multi tests get their
        # own wait-bound pool (--net-jobs wide) while --jobs stays the CPU-bound solo budget; the
        # machine-wide boot lock still serialises every launch instant, and per-lane isolation is
        # untouched. --jobs 1 keeps the fully-serial order for debugging (no split).
        solos = [(i, t) for i, t in enumerate(tests, 1) if t.get("kind") != "multi"]
        multis = [(i, t) for i, t in enumerate(tests, 1) if t.get("kind") == "multi"]
        net_jobs = max(1, min(args.net_jobs, len(multis) or 1))
        print(
            "\nrunning %d tests: %d solo at --jobs %d + %d multi-peer in their own pool "
            "(--net-jobs %d; capped peers wait more than they compute)"
            % (len(tests), len(solos), jobs, len(multis), net_jobs)
        )
        done = 0
        with (
            cf.ThreadPoolExecutor(max_workers=jobs) as ex_cpu,
            cf.ThreadPoolExecutor(max_workers=net_jobs) as ex_net,
        ):
            futs = {ex_cpu.submit(run_one, i, t): t for i, t in solos}
            futs.update({ex_net.submit(run_one, i, t): t for i, t in multis})
            for f in cf.as_completed(futs):
                name, verdict, text = f.result()
                results[name] = verdict
                done += 1
                print(text)
                print("  --> %s %s   (%d/%d complete)" % (name, verdict, done, len(tests)))

    print("\n" + "=" * 78)
    print("UI TEST SUITE")
    print("-" * 78)
    npass = nfail = nskip = 0
    nnear = 0
    for t in tests:
        r = results[t["name"]]
        npass += r == "PASS"
        nfail += r == "FAIL"
        nskip += r == "SKIP"
        # D15(b): the budget column lives in the SUMMARY, not only in the per-test block. Under --jobs
        # the per-test output is buffered and scrolls past; the summary is the part anyone actually
        # reads, so the near-miss has to be visible there or it is not visible at all.
        secs, budget = timings.get(t["name"], (0.0, 0))
        col = ""
        if budget:
            frac = secs / budget
            col = "  %5.0fs / %4ds  %3.0f%%" % (secs, budget, frac * 100)
            if frac >= WARN_FRAC and r != "SKIP":
                col += "  NEAR-MISS"
                nnear += 1
        print("  %-14s %-5s%s" % (t["name"], r, col))
    print("-" * 78)
    print("  %d passed, %d failed, %d skipped" % (npass, nfail, nskip))
    if nnear:
        # Loud on purpose: a suite that is 12/12 with three tests at 90% of budget is one busy machine
        # away from being 9/12, and the 12/12 line alone actively hides that.
        print(
            "  %d test(s) NEAR-MISS (>=%.0f%% of budget) -- green, but not by much"
            % (nnear, WARN_FRAC * 100)
        )

    # WHY THIS EXISTS. "The UI suite costs ~30 minutes" was written into the project instructions,
    # this repo's DLL README, the ui-testing skill AND a memory record, and every one of them was ~6x wrong by
    # 2026-08-02 -- true when written, then quietly obsoleted by parallel lanes and headless runs. It
    # went unnoticed because nothing measured it: the figure was only ever prose citing prose. An
    # unattended session budgeting against it has a real reason to skip a gate it could easily afford.
    # So the suite states its own cost, and writes it where the next reader can find it without
    # running anything.
    elapsed = time.time() - suite_t0
    print("  %.1f min wall clock (%d test(s), %d at a time)" % (elapsed / 60, len(tests), jobs))
    try:
        rec = os.path.join(REPO, "tmp", "ui_test", "last_suite_timing.json")
        os.makedirs(os.path.dirname(rec), exist_ok=True)
        with open(rec, "w", encoding="utf-8") as fh:
            json.dump(
                {
                    "seconds": round(elapsed, 1),
                    "tests": len(tests),
                    "jobs": jobs,
                    "passed": npass,
                    "failed": nfail,
                    "skipped": nskip,
                },
                fh,
                indent=1,
            )
    except OSError:
        pass  # a timing note is never worth failing a suite over

    # THE TACTICAL JOURNAL SCENARIOS, IN THE DEFAULT SUITE (2026-09-04). They were registered but
    # excluded, on a cost argument -- "one is ~20 minutes against ~5 for all eleven capture
    # scenarios" -- that is now false: the arm costs ~22 s since the mission-end exit landed (G118),
    # so both arms of poz1_combat add well under a minute to a ~3 min suite.
    #
    # Excluding them was not free. The capture scenarios cannot see the tactical order path at all
    # -- `tact_panel` passes with the tact_frame promotion on OR off, while that promotion was
    # dropping 184 of 211 of the player's orders. The only arm that could see it sat unrun for two
    # days and a human found the bug by playing the game.
    #
    # `equiv`, not `verify`: the differential, for the reason run_tact_equiv documents at length --
    # the recording is not reproducible in this environment, so an absolute match count would be
    # permanently red or re-baselined into vacuity, while the differential asks the question that
    # actually matters and is mutation-proven to go red on the real defect.
    if not wanted and not args.solo:
        # POOLED SINCE 2026-09-10 (user: "recorded scenarios aren't parallel"). The four journals
        # used to run one-lane-serial AFTER the parallel test pool finished, which made this tail
        # the suite's longest fully-serial stretch. Each journal now gets its own lane slot (its
        # two arms stay serial WITHIN the journal -- the equiv's premise is both arms in the same
        # environment, and one process per journal keeps the machine inside the core budget);
        # provisioning self-serialises in make_lane and boots on the machine-wide boot lock.
        # Output is routed per THREAD into a buffer and printed whole on completion, the same
        # contract the test pool has -- interleaved journal logs are unreadable exactly when a
        # red needs reading. --jobs 1 keeps the serial tail for debugging.
        runnable = []
        for i, sc in enumerate(TACT_SCENARIOS):
            path = os.path.join(REPO, sc["journal"])
            if not os.path.isfile(path):
                print("  %-14s FAIL   (journal missing: %s)" % (sc["name"], sc["journal"]))
                nfail += 1
                continue
            sub = argparse.Namespace(**vars(args))
            sub.tact_equiv = path
            sub.tact_slot = i
            runnable.append((sc["name"], sub))

        tail_jobs = 1 if jobs == 1 else min(4, len(runnable)) or 1

        class _ThreadRouter:
            """stdout proxy routing write() by thread: registered threads write to their own
            buffer, everything else passes through. Keeps run_tact_equiv's prints intact while
            the pool runs several journals at once."""

            def __init__(self, real):
                self.real = real
                self.routes = {}

            def register(self, buf):
                self.routes[threading.get_ident()] = buf

            def unregister(self):
                self.routes.pop(threading.get_ident(), None)

            def write(self, s):
                self.routes.get(threading.get_ident(), self.real).write(s)

            def flush(self):
                self.routes.get(threading.get_ident(), self.real).flush()

            def __getattr__(self, a):
                return getattr(self.real, a)

        def _one_journal(name, sub):
            buf = _io.StringIO()
            router.register(buf)
            try:
                rc = run_tact_equiv(sub)
            except Exception:
                import traceback

                traceback.print_exc()
                rc = 1
            finally:
                router.unregister()
            return name, rc, buf.getvalue()

        if tail_jobs == 1:
            for name, sub in runnable:
                print("-" * 78)
                print("  tactical journal: %s" % name)
                if run_tact_equiv(sub):
                    nfail += 1
                else:
                    npass += 1
        elif runnable:
            print("-" * 78)
            print(
                "running %d tactical journal(s), %d at a time (one lane each; output buffered "
                "per journal)" % (len(runnable), tail_jobs)
            )
            router = _ThreadRouter(sys.stdout)
            sys.stdout = router
            try:
                with cf.ThreadPoolExecutor(max_workers=tail_jobs) as ex:
                    futs = [ex.submit(_one_journal, n, s) for n, s in runnable]
                    for f in cf.as_completed(futs):
                        name, rc, text = f.result()
                        print("-" * 78)
                        print("  tactical journal: %s" % name)
                        print(text, end="")
                        print("  --> %s %s" % (name, "FAIL" if rc else "PASS"))
                        if rc:
                            nfail += 1
                        else:
                            npass += 1
            finally:
                sys.stdout = router.real

    return 1 if nfail else 0


if __name__ == "__main__":
    import hostlock

    raise SystemExit(hostlock.run_rig_tool(main, "test_ui"))
