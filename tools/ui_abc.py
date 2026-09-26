#!/usr/bin/env python3
"""Recorded UI sessions (UI-REC): the A/B/C campaign oracle, equivalence, replay, record, oracle cut.

    python tools/ui_abc.py --ui-abc spcamp_solo
    python tools/ui_abc.py --ui-abc tutorial_solo --ui-slot 2
    python tools/ui_abc.py --ui-replay <journal>

Split out of tools/test_ui.py (tooling:TL-SUITE-SPLIT); `test_ui.py --ui-*` still forwards here.
"""

import argparse
import concurrent.futures as cf
import glob
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import desktop  # noqa: E402  the raw CreateProcessW launch that honours lpDesktop
import lane_alloc  # noqa: E402  fork F4H: the ONE place a lane NUMBER comes from
import ui_test  # noqa: E402  boot_lock + wait_past_pack_load, shared with the UI suite
import make_lane  # noqa: E402  LANE_ROOT + the lane builder used by --local
from ui_suite_common import (  # noqa: E402
    LOCAL_PORT_BASE,
    REPO,
    RunnerConfig,
    TACT_PLAY_MOUSE_DIV,
    UIREC_SCENARIOS,
    _tact_pid_handle,
    add_extra_ini_arg,
    add_runner_args,
    parse_mode_args,
    print_desktop_banner,
    tact_apply_extra_ini,
    write_all_original_ini,
)

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
        "  Replay it:  python tools/ui_abc.py --ui-replay %s"
        % os.path.relpath(jpath, REPO).replace(chr(92), "/")
    )
    print(
        "  A/B it:     python tools/ui_abc.py --ui-equiv %s"
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


# ---- DECLARED EXCUSALS (tooling TL-GATE-D25FX, 2026-09-20) ------------------------------------
#
# A second kind of excused channel, and it is nothing like the wall-clock set above. Those are a
# property of what a replay controls; these are a NAMED, BOUNDED divergence between the ship and the
# original bodies that the verdict is allowed to look past ON ONE LEG -- today exactly one: D25 made
# libmh's build-click probe side-effect-free while the original binary still pays + re-grants into
# the gains-only resource_spent counter, so ship-vs-original differs in that counter from the
# human's first build click by design (the fix IS the difference). The rows live in
# tools/data/abc_excusals.json, each with the region, the bytes it is for, the reason and the item;
# a lint cross-checks every row against the hash manifest.
#
# HOW IT IS APPLIED without touching the DLL or the other legs: `state` is the FNV fold of the
# per-region hashes the `R` line already prints (mp_analyze.state_fold re-derives it, and the
# consumer REFUSES the variant unless that re-derivation reproduces `state` on every step of both
# arms). So on the excused leg the required channel becomes `state~excused` -- the same fold with
# the excused region column(s) dropped -- while B-vs-C and A-vs-C still compare the full `state`.
# The excused column itself is printed with its own mismatch count, so the excusal is visible, and
# never a tolerance: a region the rows do not name is compared exactly as before.
ABC_EXCUSALS = os.path.join(REPO, "tools", "data", "abc_excusals.json")


def ui_load_excusals(leg="A-vs-B", path=ABC_EXCUSALS):
    """The declared excusal rows whose `legs` include `leg` -- [] when the file is absent.

    A row may name A-vs-B and A-vs-C, never B-vs-C: C is an all-original recording, so a
    ship-vs-C difference has the same cause as a ship-vs-original one and gets the same declared
    excusal, while original-vs-C is the replayer's own claim and takes none."""
    if not os.path.isfile(path):
        return []
    try:
        d = json.load(open(path, encoding="utf-8"))
    except (OSError, ValueError) as e:
        print("  WARNING: %s unreadable (%s) -- no excusal applied" % (path, e))
        return []
    return [r for r in d.get("excusals", []) if leg in (r.get("legs") or []) and r.get("region")]


def ui_excused_fold(seg, step, drop):
    """(full, excused) folds for one step of a segment, or None where the segment cannot supply
    them. A live arm supplies both from its R columns; a committed oracle supplies the excused
    fold it was cut with (its own R columns are not stored), and only if its excused set is the
    one being asked for."""
    import mp_analyze as _m

    row = seg["regions"].get(step)
    st = seg["steps"].get(step) or {}
    if row and all(
        v is not None for nm, v in zip(_m.REGION_NAMES, row) if nm not in _m.STATE_EXCLUDED
    ):
        return _m.state_fold(row), _m.state_fold(row, drop=drop)
    pre = st.get("state~excused")
    if pre is not None and set(seg.get("excused_regions") or ()) == set(drop):
        return st.get("state"), pre.upper()
    return None


def ui_apply_excusals(res, seg_a, seg_b, excusals, leg="A-vs-B"):
    """Add the `state~excused` channel to an sp_compare() result, per the declared rows.

    Fail-closed in both directions: with no rows this is a no-op (the verdict stays on `state`);
    with rows, the fold MUST reproduce each arm's own `state` on every common step first, or the
    derived channel is refused and the verdict falls back to the full `state` -- which is red, and
    says why. Returns the list of applied rows (for the summary line)."""
    import mp_analyze as _m

    if not excusals:
        return []
    names = set(_m.REGION_NAMES)
    unknown = [r["region"] for r in excusals if r["region"] not in names]
    if unknown:
        res["excusal_refused"] = "region(s) not in the hash manifest: %s" % ", ".join(unknown)
        return []
    drop = {r["region"] for r in excusals}
    common = sorted(set(seg_a["steps"]) & set(seg_b["steps"]))
    idx = {nm: i for i, nm in enumerate(_m.REGION_NAMES)}
    bad_self, compared, bad, unsupplied = 0, 0, [], 0
    per_region = {nm: {"compared": 0, "mismatch_count": 0, "first_mismatch": None} for nm in drop}
    for st in common:
        fa, fb = ui_excused_fold(seg_a, st, drop), ui_excused_fold(seg_b, st, drop)
        if fa is None or fb is None:
            unsupplied += 1
            continue
        # THE SELF-CHECK: the fold over ALL columns must be the side's own printed `state`.
        if (
            fa[0] != seg_a["steps"][st]["state"].upper()
            or fb[0] != seg_b["steps"][st]["state"].upper()
        ):
            bad_self += 1
            continue
        compared += 1
        if fa[1] != fb[1]:
            bad.append(st)
        ra, rb = seg_a["regions"].get(st), seg_b["regions"].get(st)
        for nm in drop:
            i = idx[nm]
            pr = per_region[nm]
            if ra and rb and ra[i] is not None and rb[i] is not None:
                pr["compared"] += 1
                if ra[i] != rb[i]:
                    pr["mismatch_count"] += 1
                    if pr["first_mismatch"] is None:
                        pr["first_mismatch"] = st
    if bad_self or compared == 0:
        res["excusal_refused"] = (
            "the R-column fold does not reproduce `state` on %d step(s) (compared %d, %d step(s) "
            "where a side could not supply the excused fold -- an oracle cut under another excusal "
            "set, or no R columns) -- the derived channel cannot be trusted, the full `state` "
            "decides" % (bad_self, compared, unsupplied)
        )
        return []
    res["channels"]["state~excused"] = {
        "compared": compared,
        "mismatch_count": len(bad),
        "first_mismatch": bad[0] if bad else None,
    }
    res["excusals"] = [
        {
            "id": r.get("id", "?"),
            "region": r["region"],
            "reason": r.get("reason", ""),
            "since": r.get("since", ""),
            "leg": leg,
            "stats": per_region[r["region"]],
        }
        for r in excusals
    ]
    return res["excusals"]


def ui_verdict(res):
    """(ok, lines) for a UI-REC comparison -- the required channels only, with the rest named.

    Deliberately NOT sp_compare's own `ok`: that requires every channel, which is right for two runs
    launched identically and wrong for a recording compared against its replay. Reported rather than
    silently dropped, so nobody reads this verdict as "everything matched".

    With declared excusals applied (ui_apply_excusals), `state~excused` is the required channel and
    `state` is printed, not required; each excused region is printed with its own mismatch count and
    the row's id + reason, so the excusal is on the page every time it is used."""
    lines, ok = [], True
    if not res["compared_steps"]:
        return False, ["      NO OVERLAPPING STEPS -- the two runs compared nothing"]
    excused = res.get("excusals") or []
    if res.get("excusal_refused"):
        lines.append("      EXCUSAL REFUSED: %s" % res["excusal_refused"])
    required = list(UI_REQUIRED_CHANNELS)
    if excused:
        required = ["state~excused"] + [c for c in required if c != "state"]
        v = res["channels"].get("state")
        if v is not None:
            lines.append(
                "      %-18s compared %-6d mismatches %-6d first %s   [excused: %s -- verdict is state~excused]"
                % (
                    "state",
                    v["compared"],
                    v["mismatch_count"],
                    v["first_mismatch"],
                    ", ".join(e["id"] for e in excused),
                )
            )
    for ch in required:
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
    for e in excused:
        st = e["stats"]
        if st["compared"]:
            lines.append(
                "      %-18s compared %-6d mismatches %-6d first %s   [EXCUSED on %s: %s, since %s]"
                % (
                    e["region"],
                    st["compared"],
                    st["mismatch_count"],
                    st["first_mismatch"],
                    e["leg"],
                    e["id"],
                    e["since"] or "?",
                )
            )
        else:
            # The recording stores no per-region column, so the excused region's own count is
            # not measurable on a C leg; its effect is the state / state~excused pair above.
            lines.append(
                "      %-18s (no per-region column on the recording side)   [EXCUSED on %s: %s, since %s]"
                % (e["region"], e["leg"], e["id"], e["since"] or "?")
            )
        lines.append("      %-18s   reason: %s" % ("", e["reason"]))
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
    # THE EXCUSED COLUMN (TL-GATE-D25FX): the recording's `state~excused` for the A-vs-C leg,
    # folded here from the source run's R columns under the declared A-vs-C excusal set (the
    # oracle does not store its R columns, so this is the one moment the fold can be taken). The
    # set it was cut under is stamped; a later change to the declared set makes the oracle stale
    # for that leg, and ui_excused_fold refuses to use it. Refused outright if the fold does not
    # reproduce the run's own `state` -- an oracle whose derived column is wrong is worse than none.
    drop = sorted({r["region"] for r in ui_load_excusals("A-vs-C")})
    for s in steps:
        if _m.state_fold(seg["regions"][s]) != seg["steps"][s]["state"].upper():
            print(
                "FAIL: the R-column fold does not reproduce `state` at step %d of %s -- the "
                "oracle's state~excused column cannot be derived; is the analyzer's REGION_NAMES "
                "current with the DLL's manifest?" % (s, harness_log)
            )
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
        # THE MANIFEST STAMP (TL-GATE-D25FX): `state` is a fold over the hash manifest's regions, so
        # an oracle is only comparable under the manifest it was cut under. Every world blob has
        # carried this fingerprint since LIB-WORLD; the oracle did not, and D25's appended region
        # turned both A/B/C fixtures red from step 1 with nothing saying "stale" -- see dead-ends.
        f.write("; hash_manifest_fp: %s\n" % _m.hash_manifest_fingerprint())
        # TL-GATE8: the epoch of the build that RAN the source session, from its own log.
        f.write("; hash_input_epoch: %s\n" % _m.harness_input_epoch(harness_log)[0])
        f.write("; excused_regions: %s\n" % (" ".join(drop) or "-"))
        f.write("; columns: step state state~excused %s\n" % " ".join(UI_ORACLE_REGIONS))
        f.write("; regions: %s\n" % " ".join(UI_ORACLE_REGIONS))
        f.write("; steps: %d\n" % len(steps))
        for s in steps:
            r = seg["regions"][s]
            f.write(
                "%d %s %s %s\n"
                % (
                    s,
                    seg["steps"][s]["state"],
                    _m.state_fold(r, drop=set(drop)),
                    " ".join(r[c] for c in cols),
                )
            )
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
    seg["hash_manifest_fp"] = None
    seg["hash_input_epoch"] = None
    seg["excused_regions"] = []
    with gzip.open(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith(";"):
                m = re.match(r";\s*hash_manifest_fp:\s*([0-9A-Fa-f]{8})", line)
                if m:
                    seg["hash_manifest_fp"] = m.group(1).upper()
                m = re.match(r";\s*hash_input_epoch:\s*(\d+)\s*$", line)
                if m:
                    seg["hash_input_epoch"] = int(m.group(1))
                m = re.match(r";\s*excused_regions:\s*(.+)", line)
                if m:
                    seg["excused_regions"] = [x for x in m.group(1).split() if x != "-"]
                continue
            p = line.split()
            # Two shapes: `step state r1..rN` (pre-2026-09-20) and `step state state~excused r1..rN`.
            if len(p) == 3 + len(cols):
                s = int(p[0])
                seg["steps"][s] = {
                    "clock": None,
                    "combined": None,
                    "state": p[1].upper(),
                    "state~excused": p[2].upper(),
                }
                tail = p[3:]
            elif len(p) == 2 + len(cols):
                s = int(p[0])
                seg["steps"][s] = {"clock": None, "combined": None, "state": p[1].upper()}
                tail = p[2:]
            else:
                continue
            row = [None] * width
            for c, v in zip(cols, tail):
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
    args,
    journal,
    slot=0,
    extra_ini=(),
    label="arm",
    poke_at=0,
    budget=None,
    lane_dir=None,
    cfg=None,
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
        cfg = cfg or RunnerConfig()
        if cfg.desktop:
            cfg.hold_desktop_once()
            proc = _tact_pid_handle(
                desktop.spawn(exe, "--skip-intro", cwd=lane_dir, desktop=cfg.desktop)
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


def run_ui_replay(args, cfg):
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
    rd = ui_run_arm(args, journal, slot=0, extra_ini=extra, label="replay", cfg=cfg)
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


def run_ui_selftest(args, cfg):
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

    rep_dir = ui_run_arm(args, jpath, slot=1, label="replay", cfg=cfg)
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


def run_ui_equiv(args, cfg):
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
            cfg=cfg,
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
    # ship-vs-original IS the A-vs-B leg, so the declared excusals apply here exactly as in --ui-abc.
    applied = ui_apply_excusals(res, runs["ship"], runs["original"], ui_load_excusals("A-vs-B"))
    print("      compared steps: %d" % res["compared_steps"])
    v_ok, v_lines = ui_verdict(res)
    for ln in v_lines:
        print(ln)
    print(
        "      excusals applied: %d%s"
        % (len(applied), " (" + ", ".join(e["id"] for e in applied) + ")" if applied else "")
    )
    if gored:
        # THE VERDICT IS INVERTED, and the SHAPE checks are not. An arm that crashed also fails to
        # match, so "it went red" is only evidence if both arms otherwise ran the fixture properly --
        # `ok` is what says they did, and it is required in BOTH directions.
        st = res["channels"].get("state~excused") or res["channels"].get("state")
        first = st["first_mismatch"] if st else None
        red_here = st is not None and st["mismatch_count"] > 0
        final = ok and red_here
        print(
            "      GO-RED: the deciding state channel %s (poked at step %d, first mismatch at %s)"
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


def run_ui_abc(args, cfg):
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
        print("      python tools/ui_abc.py --ui-oracle <run dir> --ui-oracle-out %s" % oracle)
        return 1
    bounded = args.ui_steps > 0
    gored = getattr(args, "ui_gored", 0)
    print("UI-REC A/B/C: %s" % journal)
    print("  oracle    %s" % oracle)
    # STALE BEFORE ANYTHING RUNS (TL-GATE-D25FX). The C arm's `state` column is a fold over the
    # hash manifest's regions, so an oracle cut under another manifest is incomparable, not wrong --
    # and until this check it read as "B-vs-C mismatches from step 1", indistinguishable from a
    # broken replayer, after two arms had spent their minutes. Checked here, before a lane is
    # provisioned, against the fingerprint the DLL was built with.
    cur_fp = _m.hash_manifest_fingerprint()
    orc_fp = ui_oracle_load(oracle).get("hash_manifest_fp")
    if orc_fp != cur_fp:
        print(
            "FAIL: STALE ORACLE: %s was captured under hash manifest %s, current is %s -- its "
            "`state` column is a fold over a different region set and cannot be compared. Re-cut "
            "it from a mode=original replay of the journal (python tools/ui_abc.py --ui-oracle "
            "<run dir> --ui-oracle-out %s --ui-oracle-note ...), with the equivalence proof in its "
            "header; the libref fixtures need the same re-capture (tools/replay_libref.py says so)."
            % (
                oracle,
                orc_fp or "UNSTAMPED",
                cur_fp,
                os.path.relpath(oracle, REPO).replace(chr(92), "/"),
            )
        )
        return 1
    print("  oracle manifest fingerprint %s == current (not stale)" % cur_fp)
    orc_ep = ui_oracle_load(oracle).get("hash_input_epoch")
    bad = _m.epoch_mismatch(orc_ep, _m.hash_input_epoch())
    if bad:
        print(
            "FAIL: STALE ORACLE: %s -- %s; nothing compared. Re-cut it: python tools/test_ui.py "
            "--ui-oracle <mode=original run dir> --ui-oracle-out <oracle>" % (oracle, bad)
        )
        return 1
    print("  oracle hash-input epoch %s == current (not stale)" % orc_ep)
    excusals = {leg: ui_load_excusals(leg) for leg in ("A-vs-B", "A-vs-C")}
    for leg in ("A-vs-B", "A-vs-C"):
        if excusals[leg]:
            print(
                "  declared excusal(s) on %s: %s (tools/data/abc_excusals.json)"
                % (leg, ", ".join("%s -> %s" % (e.get("id"), e["region"]) for e in excusals[leg]))
            )
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

    # --ui-abc-ini: the SHARED diagnostic fragment(s), merged into both arms before the arm's own
    # selector so the selector wins any overlap. Refused if a fragment touches a configuration
    # section: the arms differ in [config] mode and nothing else, and a shared fragment that armed
    # a promotion key would make the "promotion oracle" compare two ship configurations.
    both_ini = tuple(getattr(args, "ui_abc_ini", None) or ())
    for frag in both_ini:
        fp = frag if os.path.isabs(frag) else os.path.join(REPO, frag)
        if not os.path.isfile(fp):
            print("FAIL: --ui-abc-ini fragment not found: %s" % frag)
            return 1
        sects = [
            ln.strip().strip("[]").lower()
            for ln in open(fp, encoding="utf-8")
            if ln.strip().startswith("[")
        ]
        bad = sorted(set(sects) & {"config", "promote", "rebind", "shadow"})
        if bad:
            print(
                "FAIL: --ui-abc-ini %s names %s -- a fragment shared by both arms may carry "
                "diagnostics only, never a configuration key" % (frag, ", ".join(bad))
            )
            return 1
        print("  both arms: --ui-abc-ini %s (sections %s)" % (frag, ", ".join(sects) or "none"))

    def _run_one_arm(arm, extra):
        slot, ld = arm_lanes[arm]
        sub = argparse.Namespace(**vars(args))
        return ui_run_arm(
            sub,
            journal,
            slot=slot,
            extra_ini=both_ini + tuple(extra),
            label=arm,
            poke_at=gored if arm == "ship" else 0,
            budget=args.ui_steps if bounded else 0,
            lane_dir=ld,
            cfg=cfg,
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
    # TL-GATE8: the arms' OWN build, not the source header, must be of the oracle's epoch.
    for arm in [a for a in runs if a != "recording"]:
        bad = _m.epoch_mismatch(
            runs["recording"].get("hash_input_epoch"),
            _m.harness_input_epoch((runs[arm] or {}).get("path"))[0],
        )
        if bad:
            print("FAIL: arm %s REFUSED against the oracle -- %s; nothing compared." % (arm, bad))
            return 1

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
        applied = []
        leg = title.replace(" ", "-")
        if leg in excusals:
            # THE TWO SHIP LEGS. B-vs-C compares the full `state` and takes no excusal: a
            # replayer bug must never be excused as a promotion difference.
            applied = ui_apply_excusals(res, runs[a], runs[b], excusals[leg], leg)
        print("\n  %s -- %s (%s)" % (title, why, "%d compared steps" % res["compared_steps"]))
        v_ok, v_lines = ui_verdict(res)
        for ln in v_lines:
            print(ln)
        print(
            "      excusals applied: %d%s"
            % (len(applied), " (" + ", ".join(e["id"] for e in applied) + ")" if applied else "")
        )
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
            ch = results[t]["channels"]
            return ch.get("state~excused") or ch.get("state") or {}

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
    import mp_analyze as _m

    # TL-GATE8: `source` names the producing build (`<ver>+<sha>`) as well as the run.
    epoch, build = _m.harness_input_epoch(log)
    if epoch is None:
        print(
            "FAIL: %s has no `input_epoch=` (pre-TL-GATE8 build) -- cannot stamp the oracle" % log
        )
        return 1
    meta = {"source": "%s @ %s" % (os.path.basename(os.path.dirname(log) or log), build)}
    for kv in getattr(args, "ui_oracle_note", None) or []:
        k, _, v = kv.partition("=")
        if not k.strip() or not v.strip():
            print("FAIL: --ui-oracle-note wants KEY=VALUE, got %r" % kv)
            return 1
        meta[k.strip()] = v.strip()
    path, n = ui_oracle_extract(log, dst, meta)
    if not path:
        print("FAIL: the log carries no per-step region hashes (region_hash_step off?)")
        return 1
    print("wrote %s -- %d step(s), %.1f KB" % (path, n, os.path.getsize(path) / 1024.0))
    hist = ui_order_histogram(log)
    print("order-code histogram (paste into the scenario's `orders` key):")
    print("  " + json.dumps(hist, sort_keys=True))
    return 0


def add_args(ap):
    """The UI-REC modes' flags."""
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
        "--ui-oracle-note",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="--ui-oracle: a provenance line written into the oracle's header (repeatable) -- "
        "the human-recording it descends from, the recut date and the equivalence proof that "
        "justifies cutting a C arm from a replay. The manifest fingerprint is stamped by itself.",
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
        "--ui-abc-ini",
        action="append",
        default=[],
        metavar="FILE",
        help="--ui-abc: an ini fragment merged into BOTH arms' lanes, identically -- the diagnostic "
        "lever for a red A-vs-B (an rdump window over the differing region, say). It is NOT the "
        "promotion lever: the arms must differ in nothing but [config] mode, so a fragment that "
        "names a [promote]/[rebind]/[config] key is refused (TL-GATE-D25FX's byte-level proof "
        "was made with `[harness] rdump_rid=8;rdump_lo=796;rdump_hi=800`).",
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


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    add_args(ap)
    add_runner_args(ap)
    add_extra_ini_arg(ap)
    return ap


def main(argv=None, lenient=False):
    ap = build_parser()
    args = parse_mode_args(ap, argv, lenient)
    cfg = RunnerConfig.from_args(args)
    print_desktop_banner(cfg)
    if args.ui_selftest:
        return run_ui_selftest(args, cfg)
    if args.ui_oracle:
        return run_ui_oracle(args)
    if args.ui_abc:
        return run_ui_abc(args, cfg)
    if args.ui_equiv:
        return run_ui_equiv(args, cfg)
    if args.ui_replay:
        return run_ui_replay(args, cfg)
    if args.ui_play:
        return run_ui_play(args)
    ap.print_usage()
    print(
        "ui_abc.py: pick a mode (--ui-abc / --ui-equiv / --ui-replay / --ui-selftest / "
        "--ui-oracle / --ui-play)"
    )
    return 2


if __name__ == "__main__":
    import hostlock

    raise SystemExit(hostlock.run_rig_tool(main, "ui_abc"))
