#!/usr/bin/env python3
"""Tactical-mode harness: journal equivalence / verify / replay / one arm / determinism / play.

    python tools/tact_test.py --tact-equiv tools/uiscripts/journals/poz1-combat.journal
    python tools/tact_test.py --tact-determinism --tact-frames 400
    python tools/tact_test.py --tact-suite

Split out of tools/test_ui.py (tooling:TL-SUITE-SPLIT); `test_ui.py --tact-*` still forwards here.
"""

import argparse
import concurrent.futures as cf
import glob
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import desktop  # noqa: E402  the raw CreateProcessW launch that honours lpDesktop
import lane_alloc  # noqa: E402  fork F4H: the ONE place a lane NUMBER comes from
import ui_test  # noqa: E402  boot_lock + wait_past_pack_load, shared with the UI suite
import machine_config as machine  # noqa: E402
import make_lane  # noqa: E402  LANE_ROOT + the lane builder used by --local
from ui_suite_common import (  # noqa: E402
    LOCAL_PORT_BASE,
    REPO,
    RunnerConfig,
    TACT_PLAY_MOUSE_DIV,
    TACT_SCENARIOS,
    _tact_pid_handle,
    add_extra_ini_arg,
    add_runner_args,
    parse_mode_args,
    print_desktop_banner,
    tact_apply_extra_ini,
    tact_merge_ini,
    write_all_original_ini,
)

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


def run_tact_trim(args, cfg):
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
    return run_tact_verify(sub_args, cfg)


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


def run_tact_equiv(args, cfg):
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
        run_dir = tact_run_arm(lane_dir, args.tact_save, wall, cfg)
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


def run_tact_suite(args, cfg):
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
            if fn(sub_args, cfg):
                rc = 1
    print("tact-suite: %s" % ("PASS" if rc == 0 else "FAIL"))
    return rc


def run_tact_verify(args, cfg):
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
    run_dir = tact_run_arm(lane_dir, args.tact_save, wall, cfg)
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
    mrun = tact_run_arm(lane_dir, args.tact_save, wall, cfg)
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


def run_tact_replay(args, cfg):
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
        return name, tact_run_arm(lane, args.tact_save, wall, cfg)

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


def run_tact_arm(args, cfg):
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
    run_dir = tact_run_arm(lane_dir, args.tact_save, wall, cfg)
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


def tact_run_arm(lane_dir, save, wall_s, cfg=None):
    """Launch one arm and return its run-log directory, or None.

    ON THE ISOLATED DESKTOP when one is in force. This path used to call subprocess.Popen directly
    and so never honoured --desktop, which reaches ui_test.py children through run_ui_test's argv
    and nothing else -- while test_ui.py went on printing "game windows cannot reach your desktop"
    at startup, because that banner is emitted from argument PARSING rather than from anything that
    launches a process. A claim about isolation printed by a code path that does not perform it is
    worse than no claim; every tactical arm this rig has ever run put a window on the operator's
    screen. lpDesktop lives in STARTUPINFO, which subprocess does not expose, so the isolated path
    is a raw CreateProcessW in tools/desktop.py -- the same one ui_test.py uses."""
    cfg = cfg or RunnerConfig()
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
        if cfg.desktop:
            cfg.hold_desktop_once()
            proc = _tact_pid_handle(
                desktop.spawn(
                    exe, "--tactical %s --skip-intro" % save, cwd=lane_dir, desktop=cfg.desktop
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
        "  Verify it:  python tools/tact_test.py --tact-replay %s"
        % os.path.relpath(jpath, REPO).replace(chr(92), "/")
    )
    print("  Keep it:    copy it into tools/uiscripts/journals/ and register the scenario")
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


def run_tact_determinism(args, cfg):
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
        run_dir = tact_run_arm(lane_dir, args.tact_save, args.tact_wall, cfg)
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


def add_args(ap):
    """The tactical modes' flags."""
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
        default=2,
        metavar="N",
        help="TACT-REC: replay arms to run concurrently, one lane each (default 2 = the `tact` lane "
        "block's width since 2026-09-19; 3 ran all arms at once). 1 keeps the single historical lane "
        "and runs them back to back.",
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
    if args.tact_equiv:
        return run_tact_equiv(args, cfg)
    if args.tact_verify:
        return run_tact_verify(args, cfg)
    if args.tact_replay:
        return run_tact_replay(args, cfg)
    if args.tact_arm:
        return run_tact_arm(args, cfg)
    if args.tact_determinism:
        return run_tact_determinism(args, cfg)
    if args.tact_play:
        return run_tact_play(args)
    if args.tact_trim:
        return run_tact_trim(args, cfg)
    if args.tact_suite:
        return run_tact_suite(args, cfg)
    ap.print_usage()
    print(
        "tact_test.py: pick a mode (--tact-equiv / --tact-verify / --tact-replay / --tact-arm / "
        "--tact-determinism / --tact-play / --tact-trim / --tact-suite)"
    )
    return 2


def tail_args(args, journal):
    """The --tact-equiv namespace the registry suite's tactical tail runs a journal with: this
    parser's defaults, overlaid with every field the suite's own args also carry."""
    sub = build_parser().parse_args([])
    for k, v in vars(args).items():
        if hasattr(sub, k):
            setattr(sub, k, v)
    sub.tact_equiv = journal
    return sub


if __name__ == "__main__":
    import hostlock

    raise SystemExit(hostlock.run_rig_tool(main, "tact_test"))
