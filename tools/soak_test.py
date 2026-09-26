#!/usr/bin/env python3
"""The all-AI soak: an N-way AI match through the real menu, with shape rules and golden hashes.

    python tools/soak_test.py --steps 3000 --soak-speed 1000 [--soak-golden PATH]

Split out of tools/test_ui.py (tooling:TL-SUITE-SPLIT); `test_ui.py --soak` still forwards here.
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lane_alloc  # noqa: E402  fork F4H: the ONE place a lane NUMBER comes from
import ui_test  # noqa: E402  boot_lock + wait_past_pack_load, shared with the UI suite
import machine_config as machine  # noqa: E402
import make_lane  # noqa: E402  LANE_ROOT + the lane builder used by --local
from ui_suite_common import (  # noqa: E402
    LOCAL_PORT_BASE,
    LOCAL_TIMEOUT_FRAMES,
    REPO,
    RunnerConfig,
    add_extra_ini_arg,
    add_harness_extra_arg,
    add_runner_args,
    add_steps_arg,
    build_scenario_argv,
    parse_mode_args,
    print_desktop_banner,
    soak_ai_premise,
    sp_newest_run,
    ui_test_cmd,
)

# Where the community saves live -- the same directory the save-sweep tool indexes into
# tools/data/save_index.json, so a name printed by the session driver's `--start` resolves here.
SAVE_STORAGE = machine.SAVE_STORAGE


def sp_net_text(log_dir):
    """mh_net.log for a PROCESS run dir -- its own, plus every SESSION sibling's the process opened.

    Since the per-session log split, mh_net.log is per SESSION: the lines the DLL writes once a
    match is open (`[promote] sim_step body served`, `[promote] sim_tick: call #`, a PARTIAL install
    reported at landing) land in `<ts>_<session>_0_solo/mh_net.log`, while the process dir
    (`<ts>_menu_solo`, the one holding the harness log) keeps only the pre-session lines. The soak's
    clause-6b liveness rule read the process file alone and failed every --soak run at HEAD with
    "liveness signal ABSENT" while the signal sat in the sibling (found by the TL-GATE-D25FX
    re-record, 2026-09-20). Concatenated, newest last, so a substring search sees the whole run."""
    parts = []
    own = os.path.join(log_dir, "mh_net.log")
    if os.path.isfile(own):
        parts.append(open(own, encoding="utf-8", errors="replace").read())
    parent = os.path.dirname(log_dir)
    stamp = os.path.basename(log_dir).split("_")[0]
    for d in sorted(glob.glob(os.path.join(parent, "*_solo"))):
        if d == log_dir or "_menu_" in os.path.basename(d):
            continue
        if os.path.basename(d).split("_")[0] < stamp:
            continue  # an older session, not this process's
        p = os.path.join(d, "mh_net.log")
        if os.path.isfile(p):
            parts.append(open(p, encoding="utf-8", errors="replace").read())
    return "\n".join(parts)


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
    ntext = sp_net_text(log_dir)
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
    import mp_analyze as _m  # TL-GATE8: the hash-input epoch, stamped and refused like fp

    ep = _m.harness_input_epoch(seg.get("harness_log") if isinstance(seg, dict) else None)[0]
    if not os.path.isfile(path):
        with open(path, "w", encoding="utf-8") as f:
            json.dump(
                {
                    "hash_fingerprint": fp,
                    "hash_input_epoch": ep,
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
    bad = _m.epoch_mismatch(blob.get("hash_input_epoch"), ep)
    if bad:
        return False, [
            "      STALE GOLDEN: %s -- nothing compared. Delete %s and re-record." % (bad, path)
        ]
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

    argv = build_scenario_argv(
        script=SOAK_SCRIPT,
        harness=True,
        steps=args.steps,
        harness_extra=hextra,
        host_dir=lane_dir,
        timeout_frames=LOCAL_TIMEOUT_FRAMES,
        # step-bound at ~100 steps/s: derive the wall budget so a long soak is not truncated
        timeout=max(args.timeout, 240 + args.steps // 40),
        headless=not args.visible,
        # shadow-arming fragments ride the soak too (RI-AI batch C)
        extra_ini=args.extra_ini,
        net_extra="game_speed_pct=%d" % args.soak_speed if args.soak_speed else None,
        # the runner re-copies the Release DLL at launch unless told otherwise
        dll=getattr(args, "soak_dll", "") or None,
    )
    if getattr(args, "soak_dll", ""):
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
    rc = subprocess.call(ui_test_cmd(argv))
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


def add_args(ap):
    """The soak's flags (--soak itself is implied; kept so old command lines parse)."""
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
        "--soak-dll",
        default="",
        metavar="PATH",
        help="--soak: deploy THIS mh.dll into the lane instead of the Release build. For "
        "tools/coverage.py, which needs the unoptimised one -- a Release /O2+LTCG binary reports "
        "inlined-away bodies as 0%% covered, which is indistinguishable from never executed. The "
        "sibling .pdb rides along; without it the collector emits a report with no source at all.",
    )


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    add_args(ap)
    add_runner_args(ap)
    add_steps_arg(ap)
    add_extra_ini_arg(ap)
    add_harness_extra_arg(ap)
    return ap


def main(argv=None, lenient=False):
    ap = build_parser()
    args = parse_mode_args(ap, argv, lenient)
    print_desktop_banner(RunnerConfig.from_args(args))
    return run_soak(args)


if __name__ == "__main__":
    import hostlock

    raise SystemExit(hostlock.run_rig_tool(main, "soak_test"))
